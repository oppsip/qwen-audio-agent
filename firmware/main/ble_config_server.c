/**
 * BLE Configuration GATT Server
 *
 * Exposes a custom BLE service (UUID FFF0) with two characteristics:
 *   FFF1 - Write:  phone sends JSON commands (chunked, newline-terminated)
 *   FFF2 - Notify: ESP32 sends JSON responses
 *
 * Intended for use only during the startup configuration window.
 * Normal HFP/A2DP operation uses Classic BT and is mutually exclusive.
 *
 * Protocol:
 *   Request (phone → ESP32):  JSON object + '\n', split across MTU-sized writes
 *   Response (ESP32 → phone): JSON object + '\n', split across MTU-sized notifies
 *
 * Supported commands:
 *   {"cmd":"get_config"}
 *   {"cmd":"set_eth","dhcp":1,"ip":"x","mask":"x","gw":"x","dns":"x"}
 *   {"cmd":"set_wifi","ssid":"x","pwd":"x"}
 *   {"cmd":"set_sip","server":"x","port":5060,"stp":0,"user":"x","pwd":"x","domain":"x"}
 *   {"cmd":"set_bt","name":"x","mode":0,"audio_dev":"x","auto_rec":1}
 *   {"cmd":"set_rtp","timeout":30}
 *   {"cmd":"factory_reset"}
 *   {"cmd":"reboot"}
 */

#include "ble_config_server.h"
#include "config_manager.h"
#include "hfp_client_mgr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gatt_common_api.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "ble_cfg";

#define SECRET_PLACEHOLDER "********"

static bool keep_secret(const char *s)
{
    return s == NULL || s[0] == '\0' || strcmp(s, SECRET_PLACEHOLDER) == 0;
}

/* ============================================================
 * BLE UUIDs and attribute indices
 * ============================================================ */

#define GATTS_SERVICE_UUID   0xFFF0
#define GATTS_CHAR_UUID_WRITE  0xFFF1
#define GATTS_CHAR_UUID_NOTIFY 0xFFF2
#define GATTS_APP_ID         0

#define BLE_CFG_MTU_MAX      200
#define BLE_CFG_RX_BUF_SIZE  512   /* accumulate fragmented writes */

enum {
    IDX_SVC,
    IDX_CHAR_WRITE_DECL,
    IDX_CHAR_WRITE_VAL,
    IDX_CHAR_NOTIFY_DECL,
    IDX_CHAR_NOTIFY_VAL,
    IDX_CHAR_NOTIFY_CFG,
    IDX_NB,
};

/* ============================================================
 * State
 * ============================================================ */

static uint16_t s_handle_table[IDX_NB];
static uint16_t s_conn_id      = 0xFFFF;
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static bool s_notify_enabled   = false;
static bool s_started          = false;

static char  s_rx_buf[BLE_CFG_RX_BUF_SIZE];
static int   s_rx_len          = 0;

/* Event bits */
#define BIT_REBOOT_REQUESTED  BIT0
static EventGroupHandle_t s_evt_group;

/* ============================================================
 * GATT attribute table
 * ============================================================ */

static const uint16_t primary_service_uuid     = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_decl_uuid           = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t char_client_cfg_uuid     = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint16_t svc_uuid                 = GATTS_SERVICE_UUID;
static const uint16_t char_uuid_write          = GATTS_CHAR_UUID_WRITE;
static const uint16_t char_uuid_notify         = GATTS_CHAR_UUID_NOTIFY;

static const uint8_t char_prop_write           = ESP_GATT_CHAR_PROP_BIT_WRITE |
                                                  ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t char_prop_notify          = ESP_GATT_CHAR_PROP_BIT_NOTIFY |
                                                  ESP_GATT_CHAR_PROP_BIT_READ;
static uint8_t       cccd_value[2]             = {0x00, 0x00};

static const esp_gatts_attr_db_t s_gatt_db[IDX_NB] = {
    /* Service declaration */
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid,
         ESP_GATT_PERM_READ,
         sizeof(svc_uuid), sizeof(svc_uuid), (uint8_t *)&svc_uuid}
    },
    /* Write characteristic declaration */
    [IDX_CHAR_WRITE_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_decl_uuid,
         ESP_GATT_PERM_READ,
         sizeof(char_prop_write), sizeof(char_prop_write), (uint8_t *)&char_prop_write}
    },
    /* Write characteristic value */
    [IDX_CHAR_WRITE_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_uuid_write,
         ESP_GATT_PERM_WRITE,
         BLE_CFG_MTU_MAX, 0, NULL}
    },
    /* Notify characteristic declaration */
    [IDX_CHAR_NOTIFY_DECL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_decl_uuid,
         ESP_GATT_PERM_READ,
         sizeof(char_prop_notify), sizeof(char_prop_notify), (uint8_t *)&char_prop_notify}
    },
    /* Notify characteristic value */
    [IDX_CHAR_NOTIFY_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_uuid_notify,
         ESP_GATT_PERM_READ,
         BLE_CFG_MTU_MAX, 0, NULL}
    },
    /* Notify CCCD */
    [IDX_CHAR_NOTIFY_CFG] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_client_cfg_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(cccd_value), sizeof(cccd_value), cccd_value}
    },
};

/* ============================================================
 * Advertising data
 * ============================================================ */

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min        = 0x20,   /* 20ms */
    .adv_int_max        = 0x40,   /* 40ms */
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ============================================================
 * Notify helper
 * ============================================================ */

static void send_notify(const char *json)
{
    if (s_conn_id == 0xFFFF || !s_notify_enabled ||
            s_gatts_if == ESP_GATT_IF_NONE) {
        return;
    }
    static char s_notify_buf[BLE_CFG_MTU_MAX + 1];
    size_t max_chunk = BLE_CFG_MTU_MAX - 3;   /* ATT overhead */
    size_t len = strlen(json);
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > max_chunk) {
            chunk = max_chunk;
        }
        memcpy(s_notify_buf, json + off, chunk);
        esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                    s_handle_table[IDX_CHAR_NOTIFY_VAL],
                                    (uint16_t)chunk, (uint8_t *)s_notify_buf, false);
        off += chunk;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_notify_buf[0] = '\n';
    esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id,
                                s_handle_table[IDX_CHAR_NOTIFY_VAL],
                                1, (uint8_t *)s_notify_buf, false);
}

/* ============================================================
 * JSON command processor
 * ============================================================ */

/* Minimal key-value extraction from a flat JSON object.
 * Finds the value string of the first occurrence of "key" in json.
 * For strings: copies the unquoted value into out_buf.
 * For numbers: returns the raw numeric token in out_buf.
 * Returns true on success. */
static bool json_get(const char *json, const char *key,
                     char *out_buf, size_t out_sz)
{
    /* Build search pattern: "key": */
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p == '"') {
        /* string value */
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < out_sz - 1) {
            out_buf[i++] = *p++;
        }
        out_buf[i] = '\0';
        return true;
    }
    /* numeric value */
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != '\n' && i < out_sz - 1) {
        out_buf[i++] = *p++;
    }
    out_buf[i] = '\0';
    return i > 0;
}

static void process_command(const char *json)
{
    char cmd[32] = {0};
    if (!json_get(json, "cmd", cmd, sizeof(cmd))) {
        send_notify("{\"ok\":false,\"err\":\"missing cmd\"}");
        return;
    }
    ESP_LOGI(TAG, "cmd: %s", cmd);

    if (strcmp(cmd, "get_config") == 0) {
        const system_config_t *c = config_get();
        /* Abbreviated keys to reduce JSON size; buf sized for compiler worst-case. */
        char buf[768];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"cfg\":{"
            "\"ssid\":\"%s\","
            "\"wpw\":\"%s\","
            "\"edh\":%d,"
            "\"eip\":\"%s\","
            "\"emk\":\"%s\","
            "\"egw\":\"%s\","
            "\"edn\":\"%s\","
            "\"host\":\"%s\","
            "\"pt\":%u,"
            "\"path\":\"%s\","
            "\"tok\":\"%s\","
            "\"tls\":%d,"
            "\"auto\":%d,"
            "\"btn\":\"%s\","
            "\"btm\":%d,"
            "\"bta\":\"%s\","
            "\"btr\":%d,"
            "\"wle\":%d"
            "}}",
            c->wifi_ssid,
            c->wifi_password[0] ? SECRET_PLACEHOLDER : "",
            c->eth_dhcp ? 1 : 0, c->eth_ip, c->eth_netmask, c->eth_gateway, c->eth_dns,
            c->qwen_host, c->qwen_port,
            c->qwen_path,
            c->qwen_token[0] ? SECRET_PLACEHOLDER : "",
            c->qwen_tls ? 1 : 0,
            c->qwen_auto_session ? 1 : 0,
            c->bt_device_name, (int)c->bt_mode, c->bt_audio_device_name,
            c->bt_auto_reconnect ? 1 : 0,
            c->bt_whitelist ? 1 : 0);
        send_notify(buf);

    } else if (strcmp(cmd, "set_eth") == 0) {
        char dhcp_s[4] = {0}, ip[16] = {0}, mask[16] = {0}, gw[16] = {0}, dns[16] = {0};
        json_get(json, "dhcp", dhcp_s, sizeof(dhcp_s));
        json_get(json, "ip",   ip,     sizeof(ip));
        json_get(json, "mask", mask,   sizeof(mask));
        json_get(json, "gw",   gw,     sizeof(gw));
        json_get(json, "dns",  dns,    sizeof(dns));

        bool dhcp = (dhcp_s[0] == '\0') ? true : (atoi(dhcp_s) != 0);
        esp_err_t err = config_set_ethernet(dhcp, ip, mask, gw, dns);
        if (err == ESP_ERR_INVALID_ARG) {
            send_notify("{\"ok\":false,\"err\":\"invalid ethernet ip config\"}");
        } else {
            send_notify(err == ESP_OK ? "{\"ok\":true}" :
                                        "{\"ok\":false,\"err\":\"nvs write failed\"}");
        }

    } else if (strcmp(cmd, "set_wifi") == 0) {
        char ssid[33] = {0}, pwd[65] = {0};
        json_get(json, "ssid", ssid, sizeof(ssid));
        json_get(json, "pwd",  pwd,  sizeof(pwd));
        if (ssid[0] == '\0') {
            send_notify("{\"ok\":false,\"err\":\"ssid required\"}");
            return;
        }
        const system_config_t *cur = config_get();
        const char *pass = keep_secret(pwd) && cur ? cur->wifi_password : pwd;
        esp_err_t err = config_set_wifi(ssid, pass);
        send_notify(err == ESP_OK ? "{\"ok\":true}" :
                                    "{\"ok\":false,\"err\":\"nvs write failed\"}");

    } else if (strcmp(cmd, "set_qwen") == 0 || strcmp(cmd, "set_sip") == 0) {
        char host[64]={0}, path[64]={0}, tok[96]={0}, port_s[8]={0}, tls_s[4]={0};
        json_get(json, "host", host, sizeof(host));
        if (host[0] == '\0') {
            json_get(json, "server", host, sizeof(host));
        }
        json_get(json, "path", path, sizeof(path));
        json_get(json, "tok", tok, sizeof(tok));
        if (tok[0] == '\0') {
            json_get(json, "pwd", tok, sizeof(tok));
        }
        json_get(json, "port", port_s, sizeof(port_s));
        json_get(json, "tls", tls_s, sizeof(tls_s));
        if (host[0] == '\0') {
            send_notify("{\"ok\":false,\"err\":\"host required\"}");
            return;
        }
        uint16_t port = (uint16_t)(port_s[0] ? atoi(port_s) : 3101);
        bool tls = (tls_s[0] && atoi(tls_s) != 0);
        const system_config_t *cur = config_get();
        const char *token = keep_secret(tok) && cur ? cur->qwen_token : tok;
        const char *use_path = path[0] ? path : (cur ? cur->qwen_path : "/api/realtime");
        esp_err_t err = config_set_qwen(host, port, use_path, token, tls);
        send_notify(err == ESP_OK ? "{\"ok\":true}" :
                                    "{\"ok\":false,\"err\":\"nvs write failed\"}");

    } else if (strcmp(cmd, "set_bt") == 0) {
        char name[32]={0}, mode_s[4]={0}, audio_dev[32]={0}, auto_rec_s[4]={0}, wle_s[4]={0};
        json_get(json, "name",      name,       sizeof(name));
        json_get(json, "mode",      mode_s,     sizeof(mode_s));
        json_get(json, "audio_dev", audio_dev,  sizeof(audio_dev));
        json_get(json, "auto_rec",  auto_rec_s, sizeof(auto_rec_s));
        json_get(json, "wle",       wle_s,      sizeof(wle_s));
        if (name[0]) {
            config_set_bluetooth(name);
        }
        bt_audio_mode_t mode = mode_s[0] ? (bt_audio_mode_t)atoi(mode_s) : BT_MODE_HFP;
        esp_err_t err = config_set_bt_mode(mode, audio_dev);
        if (err == ESP_OK && auto_rec_s[0] != '\0') {
            bool enable = (atoi(auto_rec_s) != 0);
            config_set_bt_auto_reconnect(enable);
            hfp_set_auto_reconnect(enable);
        }
        if (err == ESP_OK && wle_s[0] != '\0') {
            config_set_bt_whitelist(atoi(wle_s) != 0);
        }
        send_notify(err == ESP_OK ? "{\"ok\":true}" :
                                    "{\"ok\":false,\"err\":\"nvs write failed\"}");

    } else if (strcmp(cmd, "factory_reset") == 0) {
        config_reset_defaults();
        send_notify("{\"ok\":true,\"reboot\":true}");
        vTaskDelay(pdMS_TO_TICKS(300));
        xEventGroupSetBits(s_evt_group, BIT_REBOOT_REQUESTED);

    } else if (strcmp(cmd, "reboot") == 0) {
        send_notify("{\"ok\":true}");
        vTaskDelay(pdMS_TO_TICKS(300));  /* let notify flush */
        xEventGroupSetBits(s_evt_group, BIT_REBOOT_REQUESTED);

    } else {
        char err_buf[80];
        snprintf(err_buf, sizeof(err_buf), "{\"ok\":false,\"err\":\"unknown cmd: %s\"}", cmd);
        send_notify(err_buf);
    }
}

/* ============================================================
 * GATT server event handler
 * ============================================================ */

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                 esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        s_gatts_if = gatts_if;
        esp_ble_gatts_create_attr_tab(s_gatt_db, gatts_if, IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK ||
                param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "attr table create failed: status=%d handles=%d",
                     param->add_attr_tab.status, param->add_attr_tab.num_handle);
            break;
        }
        memcpy(s_handle_table, param->add_attr_tab.handles,
               sizeof(s_handle_table));
        esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
        break;

    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "GATT service started, handle=%d", param->start.service_handle);
        if (s_started) {
            esp_ble_gap_start_advertising(&s_adv_params);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        s_notify_enabled = false;
        s_rx_len = 0;
        ESP_LOGI(TAG, "BLE connected, conn_id=%d", s_conn_id);
        /* Request larger MTU */
        esp_ble_gatt_set_local_mtu(BLE_CFG_MTU_MAX);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "BLE disconnected, reason=0x%02x", param->disconnect.reason);
        s_conn_id = 0xFFFF;
        s_notify_enabled = false;
        s_rx_len = 0;
        if (s_started) {
            esp_ble_gap_start_advertising(&s_adv_params);
        }
        break;

    case ESP_GATTS_WRITE_EVT: {
        uint16_t handle = param->write.handle;
        if (handle == s_handle_table[IDX_CHAR_NOTIFY_CFG]) {
            /* CCCD write — enable/disable notify */
            if (param->write.len == 2) {
                s_notify_enabled = (param->write.value[0] == 0x01);
                ESP_LOGI(TAG, "Notify %s", s_notify_enabled ? "enabled" : "disabled");
            }
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
        } else if (handle == s_handle_table[IDX_CHAR_WRITE_VAL]) {
            /* Accumulate incoming data into rx buffer */
            const uint8_t *data = param->write.value;
            uint16_t       dlen = param->write.len;
            if (s_rx_len + dlen < (int)sizeof(s_rx_buf) - 1) {
                memcpy(s_rx_buf + s_rx_len, data, dlen);
                s_rx_len += dlen;
                s_rx_buf[s_rx_len] = '\0';
            } else {
                ESP_LOGW(TAG, "RX buffer overflow, discarding");
                s_rx_len = 0;
            }
            /* Look for newline terminator */
            if (strchr(s_rx_buf, '\n') != NULL) {
                s_rx_buf[s_rx_len] = '\0';
                process_command(s_rx_buf);
                s_rx_len = 0;
            }
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
        }
        break;
    }

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "MTU negotiated: %d", param->mtu.mtu);
        break;

    default:
        break;
    }
}

/* ============================================================
 * GAP event handler
 * ============================================================ */

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed: %d", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "BLE advertising started");
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "BLE advertising stopped");
        break;
    default:
        break;
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t ble_config_server_init(const char *device_name)
{
    s_evt_group = xEventGroupCreate();
    if (!s_evt_group) return ESP_ERR_NO_MEM;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* sdkconfig uses BTDM firmware; must enable with BTDM even though only
     * BLE GATT is used. Classic BT remains idle during config mode. */
    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_register_callback(gatts_event_handler);

    /* Set advertising name — phone sees this when scanning */
    char adv_name[32];
    if (device_name && device_name[0]) {
        snprintf(adv_name, sizeof(adv_name), "%.20s-CFG", device_name);
    } else {
        strncpy(adv_name, "BLU-SIP-CFG", sizeof(adv_name) - 1);
    }
    esp_ble_gap_set_device_name(adv_name);

    /* Build advertising data: flags + service UUID + local name */
    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp        = false,
        .include_name        = true,
        .include_txpower     = false,
        .min_interval        = 0x0006,
        .max_interval        = 0x0010,
        .appearance          = 0x00,
        .manufacturer_len    = 0,
        .p_manufacturer_data = NULL,
        .service_data_len    = 0,
        .p_service_data      = NULL,
        .service_uuid_len    = 0,
        .p_service_uuid      = NULL,
        .flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };
    esp_ble_gap_config_adv_data(&adv_data);

    ret = esp_ble_gatts_app_register(GATTS_APP_ID);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_ble_gatt_set_local_mtu(BLE_CFG_MTU_MAX);

    ESP_LOGI(TAG, "BLE config server initialized, name: %s", adv_name);
    return ESP_OK;
}

esp_err_t ble_config_server_start(void)
{
    s_started = true;
    /* Advertising starts automatically after GATT service is registered
     * (ESP_GATTS_START_EVT). If already started, start now. */
    if (s_handle_table[IDX_SVC] != 0) {
        esp_ble_gap_start_advertising(&s_adv_params);
    }
    return ESP_OK;
}

void ble_config_server_stop(void)
{
    s_started = false;
    esp_ble_gap_stop_advertising();
    if (s_conn_id != 0xFFFF) {
        esp_ble_gatts_close(s_gatts_if, s_conn_id);
        s_conn_id = 0xFFFF;
    }
}

bool ble_config_server_is_connected(void)
{
    return s_conn_id != 0xFFFF;
}

bool ble_config_server_wait_for_reboot(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY
                                         : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_evt_group,
                                           BIT_REBOOT_REQUESTED,
                                           pdFALSE, pdTRUE, ticks);
    return (bits & BIT_REBOOT_REQUESTED) != 0;
}

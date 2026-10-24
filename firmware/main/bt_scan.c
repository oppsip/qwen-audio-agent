/**
 * Shared Classic Bluetooth snapshot for the config UI.
 *
 * At runtime, GAP is owned by HFP AG / A2DP / HFP Client; those modules
 * forward DISC_RES / DISC_STATE / AUTH / remote-name here.
 * During first-boot web config, bt_scan_ensure_stack() brings up a
 * scan-only Classic stack so nearby devices can be listed before any
 * audio profile is started.
 */

#include "bt_scan.h"

#include "config_manager.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "bt_scan";

static bt_scan_dev_t s_devs[BT_SCAN_MAX_DEVS];
static int s_count;
static uint32_t s_generation;
static volatile bool s_discovering;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_name_req_tick;
static bool s_stack_owned;
static bt_audio_mode_t s_role = BT_MODE_HFP;

static void bump_locked(void)
{
    s_generation++;
}

static void bda_to_str(const uint8_t *bda, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool eir_get_name(const uint8_t *eir, uint16_t eir_len,
                         char *out, size_t out_max)
{
    uint16_t i = 0;
    if (eir == NULL || out == NULL || out_max == 0) {
        return false;
    }
    while (i < eir_len) {
        uint8_t len = eir[i];
        if (len == 0 || (uint16_t)(i + 1 + len) > eir_len) {
            break;
        }
        uint8_t type = eir[i + 1];
        if (type == 0x09 || type == 0x08) {
            size_t n = (size_t)(len - 1);
            if (n > out_max - 1) {
                n = out_max - 1;
            }
            memcpy(out, &eir[i + 2], n);
            out[n] = '\0';
            return out[0] != '\0';
        }
        i = (uint16_t)(i + 1 + len);
    }
    return false;
}

static bool is_audio_cod(uint32_t cod)
{
    uint32_t major = esp_bt_gap_get_cod_major_dev(cod);
    uint32_t srvc = esp_bt_gap_get_cod_srvc(cod);
    return major == ESP_BT_COD_MAJOR_DEV_AV ||
           major == ESP_BT_COD_MAJOR_DEV_PHONE ||
           (srvc & (ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING |
                    ESP_BT_COD_SRVC_CAPTURING | ESP_BT_COD_SRVC_TELEPHONY)) != 0;
}

static bool is_phone_cod(uint32_t cod)
{
    return esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PHONE;
}

static bool role_keep(const bt_scan_dev_t *d)
{
    if (d->paired || d->link != BT_SCAN_LINK_NONE) {
        return true;
    }
    switch (s_role) {
    case BT_MODE_A2DP:
    case BT_MODE_HFP_AG:
        return d->audio && !d->phone;
    default:
        return d->phone;
    }
}

static int rank(const bt_scan_dev_t *d)
{
    if (d->link == BT_SCAN_LINK_CONNECTED) {
        return 3;
    }
    if (d->link == BT_SCAN_LINK_CONNECTING) {
        return 2;
    }
    if (d->paired) {
        return 1;
    }
    return 0;
}

static void sort_locked(void)
{
    for (int i = 1; i < s_count; i++) {
        bt_scan_dev_t key = s_devs[i];
        int j = i - 1;
        while (j >= 0) {
            const bt_scan_dev_t *a = &s_devs[j];
            int ra = rank(a);
            int rb = rank(&key);
            bool keep;
            if (ra != rb) {
                keep = ra > rb;
            } else if (a->audio != key.audio) {
                keep = a->audio;
            } else {
                keep = a->rssi >= key.rssi;
            }
            if (keep) {
                break;
            }
            s_devs[j + 1] = s_devs[j];
            j--;
        }
        s_devs[j + 1] = key;
    }
}

static int find_bda_locked(const char *bda)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_devs[i].bda, bda) == 0) {
            return i;
        }
    }
    return -1;
}

static int alloc_slot_locked(bool keep)
{
    if (s_count < BT_SCAN_MAX_DEVS) {
        return s_count++;
    }
    if (keep) {
        for (int i = s_count - 1; i >= 0; i--) {
            if (!s_devs[i].paired && s_devs[i].link == BT_SCAN_LINK_NONE) {
                return i;
            }
        }
        return -1;
    }
    return s_count - 1;
}

static bool upsert_locked(const char *bda, const char *name, int rssi, bool audio,
                          int phone, bool paired, int link)
{
    if (bda == NULL || bda[0] == '\0') {
        return false;
    }
    int idx = find_bda_locked(bda);
    bool changed = false;
    if (idx < 0) {
        idx = alloc_slot_locked(paired || link > BT_SCAN_LINK_NONE);
        if (idx < 0) {
            return false;
        }
        memset(&s_devs[idx], 0, sizeof(s_devs[idx]));
        strlcpy(s_devs[idx].bda, bda, sizeof(s_devs[idx].bda));
        changed = true;
    }
    if (name && name[0] && strcmp(s_devs[idx].name, name) != 0) {
        strlcpy(s_devs[idx].name, name, sizeof(s_devs[idx].name));
        changed = true;
    }
    if (rssi != 0 && s_devs[idx].rssi != rssi) {
        s_devs[idx].rssi = rssi;
        changed = true;
    }
    if (audio && !s_devs[idx].audio) {
        s_devs[idx].audio = true;
        changed = true;
    }
    if (phone > 0 && !s_devs[idx].phone) {
        s_devs[idx].phone = true;
        changed = true;
    }
    if (paired && !s_devs[idx].paired) {
        s_devs[idx].paired = true;
        changed = true;
    }
    if (link >= 0 && s_devs[idx].link != (uint8_t)link) {
        s_devs[idx].link = (uint8_t)link;
        changed = true;
    }
    if (changed) {
        bump_locked();
    }
    return changed;
}

static void request_name_if_needed(const uint8_t *bda, const char *have_name)
{
    if (have_name && have_name[0]) {
        return;
    }
    uint32_t now = xTaskGetTickCount();
    if ((now - s_name_req_tick) < pdMS_TO_TICKS(800)) {
        return;
    }
    s_name_req_tick = now;
    esp_err_t err = esp_bt_gap_read_remote_name((uint8_t *)bda);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "read_remote_name: %s", esp_err_to_name(err));
    }
}

bool bt_scan_on_disc_res(const esp_bt_gap_cb_param_t *param,
                         char *name_out, size_t name_len)
{
    if (param == NULL) {
        return false;
    }
    char name[BT_SCAN_NAME_LEN] = {0};
    char bda[BT_SCAN_BDA_LEN] = {0};
    int rssi = 0;
    uint32_t cod = 0;
    bool have_name = false;

    for (int i = 0; i < param->disc_res.num_prop; i++) {
        const esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->val && p->len > 0) {
            size_t n = (size_t)p->len < sizeof(name) - 1 ? (size_t)p->len : sizeof(name) - 1;
            memcpy(name, p->val, n);
            name[n] = '\0';
            have_name = name[0] != '\0';
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR && p->val && p->len > 0) {
            if (eir_get_name((const uint8_t *)p->val, (uint16_t)p->len, name, sizeof(name))) {
                have_name = true;
            }
        } else if (p->type == ESP_BT_GAP_DEV_PROP_RSSI && p->val) {
            rssi = *(int8_t *)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_COD && p->val) {
            size_t n = sizeof(uint32_t) < (size_t)p->len ? sizeof(uint32_t) : (size_t)p->len;
            memcpy(&cod, p->val, n);
        }
    }

    bda_to_str(param->disc_res.bda, bda, sizeof(bda));
    portENTER_CRITICAL(&s_mux);
    int known = find_bda_locked(bda);
    if (have_name || known >= 0) {
        upsert_locked(bda, have_name ? name : NULL, rssi, is_audio_cod(cod),
                      is_phone_cod(cod) ? 1 : 0, false, -1);
        sort_locked();
    }
    portEXIT_CRITICAL(&s_mux);

    if (name_out && name_len) {
        strlcpy(name_out, name, name_len);
    }
    return have_name;
}

void bt_scan_on_disc_state(const esp_bt_gap_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }
    bool on = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
    portENTER_CRITICAL(&s_mux);
    if (s_discovering != on) {
        s_discovering = on;
        bump_locked();
    }
    portEXIT_CRITICAL(&s_mux);
}

void bt_scan_start(void)
{
    if (s_discovering) {
        return;
    }
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                               bt_scan_inquiry_len(), 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "start_discovery: %s", esp_err_to_name(err));
    }
}

uint32_t bt_scan_generation(void)
{
    return s_generation;
}

bool bt_scan_is_discovering(void)
{
    return s_discovering;
}

int bt_scan_copy(bt_scan_dev_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++) {
        if (!role_keep(&s_devs[i])) {
            continue;
        }
        out[n++] = s_devs[i];
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void bt_scan_refresh_bonded(void)
{
    int num = esp_bt_gap_get_bond_device_num();
    if (num < 0) {
        num = 0;
    }
    if (num > BT_SCAN_MAX_DEVS) {
        num = BT_SCAN_MAX_DEVS;
    }

    esp_bd_addr_t list[BT_SCAN_MAX_DEVS];
    memset(list, 0, sizeof(list));
    if (num > 0) {
        int n = num;
        if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) {
            num = 0;
        } else {
            num = n;
        }
    }

    const system_config_t *cfg = config_get();
    const char *last_name = (cfg && cfg->bt_audio_device_name[0])
                            ? cfg->bt_audio_device_name : NULL;

    char nameless[BT_SCAN_BDA_LEN] = {0};
    uint8_t nameless_bda[ESP_BD_ADDR_LEN];
    bool need_name = false;

    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_count; i++) {
        if (s_devs[i].link == BT_SCAN_LINK_NONE) {
            bool still = false;
            for (int j = 0; j < num; j++) {
                char bda[BT_SCAN_BDA_LEN];
                bda_to_str(list[j], bda, sizeof(bda));
                if (strcmp(s_devs[i].bda, bda) == 0) {
                    still = true;
                    break;
                }
            }
            if (s_devs[i].paired != still) {
                s_devs[i].paired = still;
                bump_locked();
            }
        }
    }
    for (int i = 0; i < num; i++) {
        char bda[BT_SCAN_BDA_LEN];
        bda_to_str(list[i], bda, sizeof(bda));
        const char *hint = NULL;
        int idx = find_bda_locked(bda);
        if (last_name && (idx < 0 || s_devs[idx].name[0] == '\0') && num == 1) {
            hint = last_name;
        }
        upsert_locked(bda, hint, 0, true, -1, true, -1);
    }
    sort_locked();
    for (int i = 0; i < s_count; i++) {
        if (s_devs[i].paired && s_devs[i].name[0] == '\0') {
            strlcpy(nameless, s_devs[i].bda, sizeof(nameless));
            need_name = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);

    if (need_name) {
        unsigned int b[6];
        if (sscanf(nameless, "%02X:%02X:%02X:%02X:%02X:%02X",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (int i = 0; i < 6; i++) {
                nameless_bda[i] = (uint8_t)b[i];
            }
            request_name_if_needed(nameless_bda, NULL);
        }
    }
}

void bt_scan_on_auth_cmpl(const uint8_t *bda, const char *name)
{
    if (bda == NULL) {
        return;
    }
    char s[BT_SCAN_BDA_LEN];
    bda_to_str(bda, s, sizeof(s));
    portENTER_CRITICAL(&s_mux);
    upsert_locked(s, name, 0, true, -1, true, -1);
    sort_locked();
    portEXIT_CRITICAL(&s_mux);
}

void bt_scan_on_remote_name(const uint8_t *bda, const char *name)
{
    if (bda == NULL || name == NULL || name[0] == '\0') {
        return;
    }
    char s[BT_SCAN_BDA_LEN];
    bda_to_str(bda, s, sizeof(s));
    portENTER_CRITICAL(&s_mux);
    upsert_locked(s, name, 0, false, -1, false, -1);
    sort_locked();
    portEXIT_CRITICAL(&s_mux);
}

void bt_scan_set_link(const uint8_t *bda, bt_scan_link_t link)
{
    if (bda == NULL) {
        return;
    }
    char s[BT_SCAN_BDA_LEN];
    bda_to_str(bda, s, sizeof(s));
    portENTER_CRITICAL(&s_mux);
    if (link == BT_SCAN_LINK_NONE) {
        int idx = find_bda_locked(s);
        if (idx >= 0 && s_devs[idx].link != BT_SCAN_LINK_NONE) {
            s_devs[idx].link = BT_SCAN_LINK_NONE;
            bump_locked();
        }
    } else {
        upsert_locked(s, NULL, 0, true, -1, true, (int)link);
        sort_locked();
    }
    portEXIT_CRITICAL(&s_mux);
}

static void scan_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            bt_scan_on_auth_cmpl(param->auth_cmpl.bda,
                                 (const char *)param->auth_cmpl.device_name);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {0};
        if (param->pin_req.min_16_digit) {
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            pin_code[0] = '0';
            pin_code[1] = '0';
            pin_code[2] = '0';
            pin_code[3] = '0';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_DISC_RES_EVT:
        bt_scan_on_disc_res(param, NULL, 0);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        bt_scan_on_disc_state(param);
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            bt_scan_on_remote_name(param->read_rmt_name.bda,
                                   (const char *)param->read_rmt_name.rmt_name);
        }
        break;
    default:
        break;
    }
}

static void apply_cod(bt_audio_mode_t mode)
{
    esp_bt_cod_t cod = {0};
    switch (mode) {
    case BT_MODE_HFP_AG:
        /* Same as hfp_ag_init: look like a phone so headsets list us. */
        cod.major = ESP_BT_COD_MAJOR_DEV_PHONE;
        cod.minor = 0x04;
        cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_TELEPHONY;
        break;
    case BT_MODE_A2DP:
        cod.major = ESP_BT_COD_MAJOR_DEV_AV;
        cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING;
        break;
    default:
        /* Hands-free / HFP Client looking for phones. */
        cod.major = ESP_BT_COD_MAJOR_DEV_AV;
        cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_TELEPHONY;
        break;
    }
    esp_err_t err = esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_cod: %s", esp_err_to_name(err));
    }
}

esp_err_t bt_scan_ensure_stack(void)
{
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        return ESP_OK;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(ret));
        goto fail;
    }
    esp_bt_sleep_disable();

    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid init: %s", esp_err_to_name(ret));
        goto fail;
    }
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid enable: %s", esp_err_to_name(ret));
        goto fail;
    }

    config_clear_bt_bonds_if_pending();

    const system_config_t *cfg = config_get();
    const char *name = (cfg && cfg->bt_device_name[0]) ? cfg->bt_device_name : "BLU-SIP";
    esp_bt_gap_set_device_name(name);
    esp_bt_gap_register_callback(scan_gap_cb);

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, NULL);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    s_stack_owned = true;
    ESP_LOGI(TAG, "Scan-only Classic stack started");
    return ESP_OK;

fail:
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_stack_owned = false;
    return ret;
}

void bt_scan_release_stack(void)
{
    if (!s_stack_owned) {
        return;
    }
    ESP_LOGI(TAG, "Releasing scan-only Classic stack");
    esp_bt_gap_cancel_discovery();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    s_stack_owned = false;
    portENTER_CRITICAL(&s_mux);
    s_discovering = false;
    portEXIT_CRITICAL(&s_mux);
}

void bt_scan_set_role(bt_audio_mode_t mode)
{
    if (mode > BT_MODE_HFP_AG) {
        mode = BT_MODE_HFP;
    }
    bool changed = (s_role != mode);
    s_role = mode;
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        return;
    }
    apply_cod(mode);
    if (!changed) {
        return;
    }
    ESP_LOGI(TAG, "Scan role %d", (int)mode);
    portENTER_CRITICAL(&s_mux);
    bump_locked();
    portEXIT_CRITICAL(&s_mux);
    /* Do not cancel an in-progress HFP/A2DP inquiry — that drops WiFi beacons
     * and aborts headset search. Start only if the radio is idle. */
    if (!s_discovering) {
        bt_scan_start();
    }
}

bt_audio_mode_t bt_scan_get_role(void)
{
    return s_role;
}

uint8_t bt_scan_inquiry_len(void)
{
    if (wifi_manager_is_connected() && !ethernet_manager_is_connected()) {
        return 3; /* ~3.8 s, leaves STA beacon slots */
    }
    return 8;
}

/**
 * Web / APP configuration client
 *
 * Posts a schema-driven JSON document to the platform and polls until the
 * user submits new values (web UI or future mobile APP — same REST API).
 *
 * values keys stay compatible with the BLE get_config "cfg" object.
 */

#include "web_config.h"
#include "config_manager.h"
#include "bt_scan.h"
#include "bt_whitelist.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_bt_main.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "audio_coex.h"
#ifndef CONFIG_PLATFORM_BASE_URL
#define CONFIG_PLATFORM_BASE_URL ""
#endif

static const char *TAG = "web_cfg";

#define HTTP_TIMEOUT_MS     20000
#define POLL_INTERVAL_MS    2000
#define RESP_BUF_SIZE       8192
#define URL_BUF_SIZE        256
#define AUTH_BUF_SIZE       80
#define SECRET_PLACEHOLDER  "********"

static volatile bool s_session_busy = false;

static void *psram_pref_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}

static void json_hooks_init(void)
{
    static bool done;
    if (done) {
        return;
    }
    cJSON_Hooks hooks = {
        .malloc_fn = psram_pref_malloc,
        .free_fn = free,
    };
    cJSON_InitHooks(&hooks);
    done = true;
}

static void coex_prefer_wifi(bool http)
{
    audio_coex_set_http(http);
}

bool web_config_is_enabled(void)
{
    return CONFIG_PLATFORM_BASE_URL[0] != '\0';
}

static bool keep_secret(const char *s)
{
    return s == NULL || s[0] == '\0' || strcmp(s, SECRET_PLACEHOLDER) == 0;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        return it->valuestring;
    }
    return NULL;
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        return it->valueint;
    }
    if (cJSON_IsString(it) && it->valuestring) {
        return atoi(it->valuestring);
    }
    if (cJSON_IsBool(it)) {
        return cJSON_IsTrue(it) ? 1 : 0;
    }
    return def;
}

typedef struct {
    char *buf;
    int size;
    int used;
} http_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    http_buf_t *b = (http_buf_t *)evt->user_data;
    if (b == NULL || b->buf == NULL) {
        return ESP_OK;
    }
    int n = evt->data_len;
    if (b->used + n > b->size - 1) {
        n = b->size - 1 - b->used;
    }
    if (n > 0) {
        memcpy(b->buf + b->used, evt->data, n);
        b->used += n;
        b->buf[b->used] = '\0';
    }
    return ESP_OK;
}

static bool fill_ifreq(esp_netif_t *netif, struct ifreq *ifr)
{
    char name[8] = {0};

    if (netif == NULL || ifr == NULL) {
        return false;
    }
    if (esp_netif_get_netif_impl_name(netif, name) != ESP_OK || name[0] == '\0') {
        return false;
    }
    memset(ifr, 0, sizeof(*ifr));
    strlcpy(ifr->ifr_name, name, sizeof(ifr->ifr_name));
    return ifr->ifr_name[0] != '\0';
}

static int http_json_once(esp_http_client_method_t method, const char *url,
                          const char *auth, const char *body,
                          char *resp, int resp_size, struct ifreq *ifr)
{
    http_buf_t hb = { .buf = resp, .size = resp_size, .used = 0 };
    if (resp && resp_size > 0) {
        resp[0] = '\0';
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
        .event_handler = http_event,
        .user_data = &hb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .if_name = ifr,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return -1;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    if (auth && auth[0]) {
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = -1;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGW(TAG, "HTTP %s via %s failed: %s",
                 url, (ifr && ifr->ifr_name[0]) ? ifr->ifr_name : "default",
                 esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return status;
}

static int http_json(esp_http_client_method_t method, const char *url,
                     const char *auth, const char *body,
                     char *resp, int resp_size)
{
    struct ifreq ifr_wifi = {0};
    struct ifreq ifr_eth = {0};
    bool have_wifi = fill_ifreq(wifi_manager_netif(), &ifr_wifi);
    bool have_eth = fill_ifreq(ethernet_manager_netif(), &ifr_eth);

    /* First attempt uses the default route so DNS (getaddrinfo) is not
     * bound to a leftover Ethernet netif. Forcing if_name=stN is a common
     * cause of EAI_FAIL (202) while SIP still works via a raw IP. */
    ESP_LOGI(TAG, "HTTP via default route");
    int status = http_json_once(method, url, auth, body, resp, resp_size, NULL);
    if (status >= 200 && status < 300) {
        return status;
    }

    if (have_wifi) {
        ESP_LOGW(TAG, "HTTP failed on default route (status %d), retry via %s",
                 status, ifr_wifi.ifr_name);
        status = http_json_once(method, url, auth, body, resp, resp_size, &ifr_wifi);
        if (status >= 200 && status < 300) {
            return status;
        }
    }
    if (have_eth) {
        ESP_LOGW(TAG, "HTTP failed (status %d), retry via %s",
                 status, ifr_eth.ifr_name);
        status = http_json_once(method, url, auth, body, resp, resp_size, &ifr_eth);
    }
    return status;
}

static void make_auth(char *buf, size_t sz)
{
    snprintf(buf, sz, "Bearer %s", config_get_device_password());
}

static void make_url(char *buf, size_t sz, const char *path)
{
    const char *base = CONFIG_PLATFORM_BASE_URL;
    size_t n = strlen(base);
    if (n > 0 && base[n - 1] == '/') {
        snprintf(buf, sz, "%s%s", base, path + (path[0] == '/' ? 1 : 0));
    } else {
        snprintf(buf, sz, "%s%s", base, path);
    }
}

static cJSON *label_obj(const char *zh, const char *en)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "zh", zh);
    cJSON_AddStringToObject(o, "en", en);
    return o;
}

static cJSON *ui_field(const char *key, const char *type,
                       const char *zh, const char *en)
{
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "key", key);
    cJSON_AddStringToObject(f, "type", type);
    cJSON_AddItemToObject(f, "label", label_obj(zh, en));
    return f;
}

static void field_visible_when(cJSON *f, const char *key, int value)
{
    cJSON *w = cJSON_CreateObject();
    cJSON_AddNumberToObject(w, key, value);
    cJSON_AddItemToObject(f, "visible_when", w);
}

static void field_visible_when2(cJSON *f, const char *k1, int v1,
                                const char *k2, int v2)
{
    cJSON *w = cJSON_CreateObject();
    cJSON_AddNumberToObject(w, k1, v1);
    cJSON_AddNumberToObject(w, k2, v2);
    cJSON_AddItemToObject(f, "visible_when", w);
}

static cJSON *option_item(int value, const char *zh, const char *en)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "value", value);
    cJSON_AddItemToObject(o, "label", label_obj(zh, en));
    return o;
}

static cJSON *build_ui(void)
{
    cJSON *ui = cJSON_CreateObject();
    cJSON *tabs = cJSON_AddArrayToObject(ui, "tabs");

    /* ---- LAN ---- */
    {
        cJSON *tab = cJSON_CreateObject();
        cJSON_AddStringToObject(tab, "id", "lan");
        cJSON_AddItemToObject(tab, "label", label_obj("有线网络", "Ethernet"));
        cJSON *fields = cJSON_AddArrayToObject(tab, "fields");

        cJSON_AddItemToArray(fields, ui_field("edh", "switch", "DHCP", "DHCP"));

        cJSON *eip = ui_field("eip", "text", "IP 地址", "IP Address");
        cJSON_AddNumberToObject(eip, "maxlength", 15);
        field_visible_when(eip, "edh", 0);
        cJSON_AddItemToArray(fields, eip);

        cJSON *emk = ui_field("emk", "text", "子网掩码", "Netmask");
        cJSON_AddNumberToObject(emk, "maxlength", 15);
        field_visible_when(emk, "edh", 0);
        cJSON_AddItemToArray(fields, emk);

        cJSON *egw = ui_field("egw", "text", "网关", "Gateway");
        cJSON_AddNumberToObject(egw, "maxlength", 15);
        field_visible_when(egw, "edh", 0);
        cJSON_AddItemToArray(fields, egw);

        cJSON *edn = ui_field("edn", "text", "DNS", "DNS");
        cJSON_AddNumberToObject(edn, "maxlength", 15);
        field_visible_when(edn, "edh", 0);
        cJSON_AddItemToArray(fields, edn);

        cJSON_AddItemToArray(tabs, tab);
    }

    /* ---- WiFi ---- */
    {
        cJSON *tab = cJSON_CreateObject();
        cJSON_AddStringToObject(tab, "id", "wifi");
        cJSON_AddItemToObject(tab, "label", label_obj("WiFi", "WiFi"));
        cJSON *fields = cJSON_AddArrayToObject(tab, "fields");

        cJSON *ssid = ui_field("ssid", "text", "SSID", "SSID");
        cJSON_AddNumberToObject(ssid, "maxlength", 32);
        cJSON_AddItemToArray(fields, ssid);

        cJSON *wpw = ui_field("wpw", "password", "WiFi 密码", "WiFi Password");
        cJSON_AddNumberToObject(wpw, "maxlength", 64);
        cJSON_AddStringToObject(wpw, "placeholder",
                                "留空则保持原密码 / leave unchanged");
        cJSON_AddItemToArray(fields, wpw);

        cJSON_AddItemToArray(tabs, tab);
    }

    /* ---- Qwen ---- */
    {
        cJSON *tab = cJSON_CreateObject();
        cJSON_AddStringToObject(tab, "id", "qwen");
        cJSON_AddItemToObject(tab, "label", label_obj("Qwen", "Qwen"));
        cJSON *fields = cJSON_AddArrayToObject(tab, "fields");

        cJSON *host = ui_field("host", "text", "Gateway / Relay 主机", "Gateway / Relay Host");
        cJSON_AddNumberToObject(host, "maxlength", 63);
        cJSON_AddItemToObject(host, "hint",
                              label_obj("本机运行 device-relay 的局域网 IP，不要填 127.0.0.1",
                                        "LAN IP of the computer running device-relay. Do not use 127.0.0.1"));
        cJSON_AddItemToArray(fields, host);

        cJSON *pt = ui_field("pt", "number", "端口", "Port");
        cJSON_AddNumberToObject(pt, "min", 1);
        cJSON_AddNumberToObject(pt, "max", 65535);
        cJSON_AddItemToArray(fields, pt);

        cJSON *path = ui_field("path", "text", "路径", "Path");
        cJSON_AddNumberToObject(path, "maxlength", 63);
        cJSON_AddItemToArray(fields, path);

        cJSON *tok = ui_field("tok", "password", "设备令牌", "Device Token");
        cJSON_AddNumberToObject(tok, "maxlength", 95);
        cJSON_AddStringToObject(tok, "placeholder",
                                "留空则保持原令牌 / leave unchanged");
        cJSON_AddItemToArray(fields, tok);

        cJSON *tls = ui_field("tls", "switch", "TLS (wss)", "TLS (wss)");
        cJSON_AddItemToArray(fields, tls);

        cJSON *auto_s = ui_field("auto", "switch", "耳机连接后自动开会话", "Auto-start on headset connect");
        cJSON_AddItemToArray(fields, auto_s);

        cJSON_AddItemToArray(tabs, tab);
    }

    /* ---- Bluetooth ---- */
    {
        cJSON *tab = cJSON_CreateObject();
        cJSON_AddStringToObject(tab, "id", "bt");
        cJSON_AddItemToObject(tab, "label", label_obj("蓝牙", "Bluetooth"));
        cJSON *fields = cJSON_AddArrayToObject(tab, "fields");

        cJSON *btn = ui_field("btn", "text", "设备名称", "Device Name");
        cJSON_AddNumberToObject(btn, "maxlength", 31);
        cJSON_AddItemToArray(fields, btn);

        cJSON *btm = ui_field("btm", "radio", "音频模式", "Audio Mode");
        cJSON *opts = cJSON_AddArrayToObject(btm, "options");
        cJSON_AddItemToArray(opts, option_item(0, "HFP（连接手机）", "HFP (phone)"));
        cJSON_AddItemToArray(opts, option_item(1, "A2DP（连接音箱）", "A2DP (speaker)"));
        cJSON_AddItemToArray(opts, option_item(2, "HFP AG（连接耳机）", "HFP AG (headset)"));
        cJSON_AddItemToArray(fields, btm);

        cJSON *bta = ui_field("bta", "picker", "蓝牙设备", "Bluetooth Devices");
        cJSON_AddNumberToObject(bta, "maxlength", 31);
        cJSON_AddItemToObject(bta, "hint",
                              label_obj("已配对设备及其连接状态，以及附近扫描结果。点选可填入目标名称（A2DP/AG 按名称连接；HFP 仍按上次配对地址回连）。留空：A2DP 连接任意音箱，AG 等待耳机主动连接",
                                        "Paired devices with connection status, plus nearby scan results. Pick a name for A2DP/AG; HFP still reconnects by last address. Empty: A2DP connects any speaker, AG waits for the headset"));
        cJSON_AddItemToObject(bta, "empty",
                              label_obj("暂无已配对或附近的设备，正在扫描…",
                                        "No paired or nearby devices yet, scanning…"));
        cJSON_AddItemToObject(bta, "unavailable",
                              label_obj("首次开机配置时蓝牙尚未启动，请手动填写名称。设备工作后长按配置键，可列出已配对和附近的设备。",
                                        "Bluetooth is not running during first-time setup. Type the name, or hold the config button after the device is working to list paired and nearby devices."));
        cJSON_AddItemToArray(fields, bta);

        cJSON_AddItemToArray(fields,
                             ui_field("btr", "switch", "自动回连", "Auto Reconnect"));

        cJSON *wle = ui_field("wle", "switch", "来电白名单", "Incoming whitelist");
        field_visible_when(wle, "btm", 0);
        cJSON_AddItemToObject(wle, "hint",
                              label_obj("仅 HFP 连接手机时生效。开启后，通讯录中的来电不转 Qwen，手机接听后语音留在手机。连接手机时若用户同意，设备会自动同步通讯录。",
                                        "HFP Client only. Whitelisted CLIP is not forwarded to Qwen; audio stays on the phone when answered there. Contacts sync when the phone allows access."));
        cJSON_AddItemToArray(fields, wle);

        cJSON *wlc = ui_field("wlc", "roster", "通讯录白名单", "Contact whitelist");
        field_visible_when2(wlc, "btm", 0, "wle", 1);
        cJSON_AddItemToObject(wlc, "hint",
                              label_obj("由手机通讯录自动同步，不可编辑。",
                                        "Synced from the phone book. Read-only."));
        cJSON_AddItemToObject(wlc, "empty",
                              label_obj("暂无号码。请用 HFP 连接手机并允许访问通讯录。",
                                        "No numbers yet. Connect a phone over HFP and allow contact access."));
        cJSON_AddItemToArray(fields, wlc);

        cJSON_AddItemToArray(tabs, tab);
    }

    /* ---- Advanced ---- */
    {
        cJSON *tab = cJSON_CreateObject();
        cJSON_AddStringToObject(tab, "id", "adv");
        cJSON_AddItemToObject(tab, "label", label_obj("高级", "Advanced"));
        cJSON *fields = cJSON_AddArrayToObject(tab, "fields");

        cJSON *frs = ui_field("frs", "action", "恢复出厂设置", "Factory Reset");
        cJSON_AddItemToObject(frs, "button",
                              label_obj("恢复出厂设置", "Factory Reset"));
        cJSON_AddItemToObject(frs, "confirm",
                              label_obj("将清除网络、Qwen、蓝牙配对等用户配置，设备会重启。平台登录身份保留。确定继续？",
                                        "This clears network, Qwen, and Bluetooth pairings, then reboots. Platform login identity is kept. Continue?"));
        cJSON_AddItemToObject(frs, "hint",
                              label_obj("不会清除设备在平台上的注册。重启后使用固件 menuconfig 中的出厂初值。",
                                        "Does not clear platform registration. After reboot, menuconfig factory defaults apply."));
        cJSON_AddItemToArray(fields, frs);

        cJSON_AddItemToArray(tabs, tab);
    }

    return ui;
}

static cJSON *build_values(void)
{
    const system_config_t *c = config_get();

    cJSON *v = cJSON_CreateObject();
    cJSON_AddStringToObject(v, "ssid", c->wifi_ssid);
    cJSON_AddStringToObject(v, "wpw",
                            c->wifi_password[0] ? SECRET_PLACEHOLDER : "");
    cJSON_AddNumberToObject(v, "edh", c->eth_dhcp ? 1 : 0);
    cJSON_AddStringToObject(v, "eip", c->eth_ip);
    cJSON_AddStringToObject(v, "emk", c->eth_netmask);
    cJSON_AddStringToObject(v, "egw", c->eth_gateway);
    cJSON_AddStringToObject(v, "edn", c->eth_dns);
    cJSON_AddStringToObject(v, "host", c->qwen_host);
    cJSON_AddNumberToObject(v, "pt", c->qwen_port);
    cJSON_AddStringToObject(v, "path", c->qwen_path);
    cJSON_AddStringToObject(v, "tok",
                            c->qwen_token[0] ? SECRET_PLACEHOLDER : "");
    cJSON_AddNumberToObject(v, "tls", c->qwen_tls ? 1 : 0);
    cJSON_AddNumberToObject(v, "auto", c->qwen_auto_session ? 1 : 0);
    cJSON_AddStringToObject(v, "btn", c->bt_device_name);
    cJSON_AddNumberToObject(v, "btm", (int)c->bt_mode);
    cJSON_AddStringToObject(v, "bta", c->bt_audio_device_name);
    cJSON_AddNumberToObject(v, "btr", c->bt_auto_reconnect ? 1 : 0);
    cJSON_AddNumberToObject(v, "wle", c->bt_whitelist ? 1 : 0);
    return v;
}

static bool bt_stack_ready(void)
{
    return esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
}

static cJSON *build_scan_object(void)
{
    bool ready = bt_stack_ready();
    cJSON *scan = cJSON_CreateObject();
    cJSON_AddBoolToObject(scan, "available", ready);
    cJSON_AddBoolToObject(scan, "scanning", ready && bt_scan_is_discovering());
    cJSON *arr = cJSON_AddArrayToObject(scan, "devices");
    if (!ready) {
        return scan;
    }
    bt_scan_refresh_bonded();
    bt_scan_dev_t devs[BT_SCAN_MAX_DEVS];
    int n = bt_scan_copy(devs, BT_SCAN_MAX_DEVS);
    for (int i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "name", devs[i].name);
        cJSON_AddStringToObject(d, "bda", devs[i].bda);
        cJSON_AddNumberToObject(d, "rssi", devs[i].rssi);
        cJSON_AddBoolToObject(d, "audio", devs[i].audio);
        cJSON_AddBoolToObject(d, "phone", devs[i].phone);
        cJSON_AddBoolToObject(d, "paired", devs[i].paired);
        cJSON_AddBoolToObject(d, "connected",
                              devs[i].link == BT_SCAN_LINK_CONNECTED);
        cJSON_AddBoolToObject(d, "connecting",
                              devs[i].link == BT_SCAN_LINK_CONNECTING);
        cJSON_AddItemToArray(arr, d);
    }
    return scan;
}

static cJSON *build_contacts_object(void)
{
    cJSON *o = cJSON_CreateObject();
    const system_config_t *c = config_get();
    cJSON_AddBoolToObject(o, "enabled", c && c->bt_whitelist);
    cJSON *arr = cJSON_AddArrayToObject(o, "items");
    bt_wl_contact_t list[BT_WL_MAX];
    int n = bt_whitelist_copy(list, BT_WL_MAX);
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "name", list[i].name);
        cJSON_AddStringToObject(it, "number", list[i].number);
        cJSON_AddItemToArray(arr, it);
    }
    return o;
}

static void push_scan(const char *url, const char *auth, char *resp, int resp_size)
{
    cJSON *scan = build_scan_object();
    char *body = cJSON_PrintUnformatted(scan);
    cJSON_Delete(scan);
    if (body == NULL) {
        return;
    }
    int status = http_json(HTTP_METHOD_PUT, url, auth, body, resp, resp_size);
    free(body);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Scan update HTTP %d", status);
    }
}

static char *build_session_body(web_config_mode_t mode, uint32_t timeout_ms)
{
    json_hooks_init();

    char id[CONFIG_DEVICE_ID_LEN];
    config_get_device_id(id, sizeof(id));
    const esp_app_desc_t *desc = esp_app_get_description();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON_CreateObject failed (int heap %u / largest %u, psram %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return NULL;
    }
    cJSON *meta = cJSON_AddObjectToObject(root, "meta");
    cJSON_AddStringToObject(meta, "device_id", id);
    cJSON_AddStringToObject(meta, "fw_version", desc->version);
    cJSON_AddStringToObject(meta, "model", "BLU-SIP");
    cJSON_AddNumberToObject(meta, "schema_version", 4);

    cJSON_AddStringToObject(root, "mode",
                            mode == WEB_CONFIG_MODE_RUNTIME ? "runtime" : "setup");
    cJSON_AddNumberToObject(root, "timeout_sec",
                            timeout_ms == 0 ? 0 : (int)(timeout_ms / 1000));
    cJSON_AddItemToObject(root, "values", build_values());
    cJSON_AddItemToObject(root, "ui", build_ui());
    cJSON_AddItemToObject(root, "scan", build_scan_object());
    cJSON_AddItemToObject(root, "contacts", build_contacts_object());

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed (int heap %u / largest %u, psram %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    return out;
}

static esp_err_t apply_values(const cJSON *values)
{
    if (!cJSON_IsObject(values)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (json_int(values, "frs", 0) != 0) {
        ESP_LOGW(TAG, "Factory reset requested via web config");
        return config_reset_defaults();
    }

    const system_config_t *cur = config_get();
    esp_err_t err;

    bool dhcp = json_int(values, "edh", cur->eth_dhcp ? 1 : 0) != 0;
    const char *eip = json_str(values, "eip");
    const char *emk = json_str(values, "emk");
    const char *egw = json_str(values, "egw");
    const char *edn = json_str(values, "edn");
    err = config_set_ethernet(dhcp,
                              eip ? eip : cur->eth_ip,
                              emk ? emk : cur->eth_netmask,
                              egw ? egw : cur->eth_gateway,
                              edn ? edn : cur->eth_dns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply ethernet failed: %s", esp_err_to_name(err));
        return err;
    }

    const char *ssid = json_str(values, "ssid");
    const char *wpw = json_str(values, "wpw");
    if (ssid != NULL) {
        const char *pass = keep_secret(wpw) ? cur->wifi_password : wpw;
        err = config_set_wifi(ssid, pass ? pass : "");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply wifi failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    const char *host = json_str(values, "host");
    if (!host) {
        host = json_str(values, "svr");
    }
    const char *path = json_str(values, "path");
    const char *tok = json_str(values, "tok");
    if (!tok) {
        tok = json_str(values, "spw");
    }
    uint16_t port = (uint16_t)json_int(values, "pt", cur->qwen_port);
    bool tls = json_int(values, "tls", cur->qwen_tls ? 1 : 0) != 0;
    if (host && host[0]) {
        const char *token = keep_secret(tok) ? cur->qwen_token : tok;
        const char *use_path = (path && path[0]) ? path : cur->qwen_path;
        err = config_set_qwen(host, port, use_path, token ? token : "", tls);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply qwen failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    int auto_s = json_int(values, "auto", cur->qwen_auto_session ? 1 : 0);
    config_set_qwen_auto_session(auto_s != 0);

    const char *btn = json_str(values, "btn");
    if (btn && btn[0]) {
        config_set_bluetooth(btn);
    }
    bt_audio_mode_t mode = (bt_audio_mode_t)json_int(values, "btm", (int)cur->bt_mode);
    const char *bta = json_str(values, "bta");
    err = config_set_bt_mode(mode, bta ? bta : cur->bt_audio_device_name);
    if (err != ESP_OK) {
        return err;
    }
    config_set_bt_auto_reconnect(json_int(values, "btr", cur->bt_auto_reconnect ? 1 : 0) != 0);
    config_set_bt_whitelist(json_int(values, "wle", cur->bt_whitelist ? 1 : 0) != 0);

    config_mark_configured();
    return ESP_OK;
}

static void ack_session(const char *status)
{
    char url[URL_BUF_SIZE], auth[AUTH_BUF_SIZE], id[CONFIG_DEVICE_ID_LEN];
    char path[96], resp[256], body[64];

    config_get_device_id(id, sizeof(id));
    snprintf(path, sizeof(path), "/api/v1/devices/%s/session/ack", id);
    make_url(url, sizeof(url), path);
    make_auth(auth, sizeof(auth));
    snprintf(body, sizeof(body), "{\"status\":\"%s\"}", status);
    http_json(HTTP_METHOD_POST, url, auth, body, resp, sizeof(resp));
}

esp_err_t web_config_register(void)
{
    if (!web_config_is_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    json_hooks_init();

    char id[CONFIG_DEVICE_ID_LEN];
    config_get_device_id(id, sizeof(id));
    if (id[0] == '\0') {
        return ESP_FAIL;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "device_id", id);
    cJSON_AddStringToObject(req, "model", "BLU-SIP");
    cJSON_AddStringToObject(req, "fw_version", desc->version);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char url[URL_BUF_SIZE];
    char *resp = psram_pref_malloc(RESP_BUF_SIZE);
    if (resp == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    make_url(url, sizeof(url), "/api/v1/register");
    ESP_LOGI(TAG, "Registering device %s", id);
    int status = http_json(HTTP_METHOD_POST, url, NULL, body, resp, RESP_BUF_SIZE);
    free(body);

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Register HTTP %d: %s", status, resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
        return ESP_FAIL;
    }

    const char *pass = json_str(root, "device_password");
    bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok"));
    if (!ok || pass == NULL || pass[0] == '\0') {
        ESP_LOGE(TAG, "Register response missing device_password");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    esp_err_t err = config_mark_registered(pass);
    if (err == ESP_OK) {
        bool is_new = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "is_new"));
        ESP_LOGI(TAG, "Registered %s (%s)", id, is_new ? "new" : "existing");
    }
    cJSON_Delete(root);
    return err;
}

web_config_result_t web_config_run_session(web_config_mode_t mode,
                                           uint32_t timeout_ms)
{
    if (!web_config_is_enabled()) {
        ESP_LOGW(TAG, "Web config unavailable: platform URL empty");
        return WEB_CONFIG_UNAVAILABLE;
    }
    json_hooks_init();
    if (s_session_busy) {
        return WEB_CONFIG_BUSY;
    }
    if (audio_coex_media_active()) {
        ESP_LOGW(TAG, "Web config deferred: call in progress");
        return WEB_CONFIG_BUSY;
    }
    if (!config_is_registered()) {
        if (web_config_register() != ESP_OK) {
            ESP_LOGW(TAG, "Web config unavailable: register failed");
            return WEB_CONFIG_UNAVAILABLE;
        }
    }

    s_session_busy = true;
    coex_prefer_wifi(true);

    if (bt_scan_ensure_stack() != ESP_OK) {
        ESP_LOGW(TAG, "Classic BT not available for nearby scan");
    } else {
        const system_config_t *cfg = config_get();
        bt_scan_set_role(cfg ? cfg->bt_mode : BT_MODE_HFP);
        bt_scan_start();
    }

    char *body = build_session_body(mode, timeout_ms);
    if (body == NULL) {
        ESP_LOGE(TAG, "Web config unavailable: failed to build session body");
        bt_scan_release_stack();
        s_session_busy = false;
        coex_prefer_wifi(false);
        return WEB_CONFIG_UNAVAILABLE;
    }

    char id[CONFIG_DEVICE_ID_LEN], url[URL_BUF_SIZE], scan_url[URL_BUF_SIZE], auth[AUTH_BUF_SIZE];
    char path[80], scan_path[96];
    config_get_device_id(id, sizeof(id));
    snprintf(path, sizeof(path), "/api/v1/devices/%s/session", id);
    snprintf(scan_path, sizeof(scan_path), "/api/v1/devices/%s/session/scan", id);
    make_url(url, sizeof(url), path);
    make_url(scan_url, sizeof(scan_url), scan_path);
    make_auth(auth, sizeof(auth));

    char *resp = psram_pref_malloc(RESP_BUF_SIZE);
    if (resp == NULL) {
        ESP_LOGE(TAG, "Web config unavailable: no memory for response");
        free(body);
        bt_scan_release_stack();
        s_session_busy = false;
        coex_prefer_wifi(false);
        return WEB_CONFIG_UNAVAILABLE;
    }

    ESP_LOGI(TAG, "Opening %s config session (timeout %s)",
             mode == WEB_CONFIG_MODE_RUNTIME ? "runtime" : "setup",
             timeout_ms == 0 ? "none" : "set");
    int status = http_json(HTTP_METHOD_PUT, url, auth, body, resp, RESP_BUF_SIZE);
    free(body);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Open session HTTP %d: %s", status, resp);
        free(resp);
        bt_scan_release_stack();
        s_session_busy = false;
        coex_prefer_wifi(false);
        return WEB_CONFIG_UNAVAILABLE;
    }

    TickType_t start = xTaskGetTickCount();
    web_config_result_t result = WEB_CONFIG_TIMEOUT;
    uint32_t last_scan_gen = 0;
    int polls = 0;
    bool bt_up = bt_stack_ready();
    if (bt_up) {
        last_scan_gen = bt_scan_generation();
        push_scan(scan_url, auth, resp, RESP_BUF_SIZE);
    }

    while (true) {
        if (timeout_ms > 0) {
            uint32_t elapsed = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
            if (elapsed >= timeout_ms) {
                ESP_LOGW(TAG, "Config session timed out");
                result = WEB_CONFIG_TIMEOUT;
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        status = http_json(HTTP_METHOD_GET, url, auth, NULL, resp, RESP_BUF_SIZE);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "Poll HTTP %d", status);
            continue;
        }

        cJSON *root = cJSON_Parse(resp);
        if (root == NULL) {
            continue;
        }
        const char *st = json_str(root, "status");
        if (st && strcmp(st, "submitted") == 0) {
            const cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
            esp_err_t err = apply_values(values);
            cJSON_Delete(root);
            if (err == ESP_OK) {
                ack_session("applied");
                ESP_LOGI(TAG, "New config applied");
                result = WEB_CONFIG_APPLIED;
            } else {
                ack_session("rejected");
                result = WEB_CONFIG_UNAVAILABLE;
            }
            break;
        }
        if (st && strcmp(st, "expired") == 0) {
            cJSON_Delete(root);
            result = WEB_CONFIG_TIMEOUT;
            break;
        }
        if (st && strcmp(st, "waiting") == 0) {
            const cJSON *draft = cJSON_GetObjectItemCaseSensitive(root, "draft");
            if (cJSON_IsObject(draft)) {
                int btm = json_int(draft, "btm", -1);
                if (btm >= (int)BT_MODE_HFP && btm <= (int)BT_MODE_HFP_AG &&
                    (bt_audio_mode_t)btm != bt_scan_get_role()) {
                    ESP_LOGI(TAG, "Web BT mode draft: %d", btm);
                    bt_scan_set_role((bt_audio_mode_t)btm);
                    last_scan_gen = 0;
                    bt_up = bt_stack_ready();
                }
            }
        }
        cJSON_Delete(root);

        polls++;
        if (bt_up) {
            if ((polls % 6) == 1) {
                bt_scan_start();
            }
            uint32_t gen = bt_scan_generation();
            if (polls == 1 || gen != last_scan_gen) {
                last_scan_gen = gen;
                push_scan(scan_url, auth, resp, RESP_BUF_SIZE);
            }
        }

        uint32_t elapsed = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        if ((elapsed / POLL_INTERVAL_MS) % 15 == 14) {
            ESP_LOGI(TAG, "Waiting for user to submit config on the platform...");
        }
    }

    free(resp);
    if (result != WEB_CONFIG_APPLIED) {
        const system_config_t *cfg = config_get();
        bt_scan_set_role(cfg ? cfg->bt_mode : BT_MODE_HFP);
    }
    bt_scan_release_stack();
    s_session_busy = false;
    coex_prefer_wifi(false);
    return result;
}

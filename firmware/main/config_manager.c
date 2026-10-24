#include "config_manager.h"
#include "bt_whitelist.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#if CONFIG_BT_ENABLED
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#endif

#ifndef CONFIG_GW_WIFI_SSID
#define CONFIG_GW_WIFI_SSID ""
#endif
#ifndef CONFIG_GW_WIFI_PASSWORD
#define CONFIG_GW_WIFI_PASSWORD ""
#endif
#ifndef CONFIG_GW_ETH_DHCP
#define CONFIG_GW_ETH_DHCP 1
#endif
#ifndef CONFIG_GW_ETH_IP
#define CONFIG_GW_ETH_IP ""
#endif
#ifndef CONFIG_GW_ETH_NETMASK
#define CONFIG_GW_ETH_NETMASK "255.255.255.0"
#endif
#ifndef CONFIG_GW_ETH_GATEWAY
#define CONFIG_GW_ETH_GATEWAY ""
#endif
#ifndef CONFIG_GW_ETH_DNS
#define CONFIG_GW_ETH_DNS ""
#endif
#ifndef CONFIG_GW_QWEN_HOST
#define CONFIG_GW_QWEN_HOST ""
#endif
#ifndef CONFIG_GW_QWEN_PORT
#define CONFIG_GW_QWEN_PORT 3101
#endif
#ifndef CONFIG_GW_QWEN_PATH
#define CONFIG_GW_QWEN_PATH "/api/realtime"
#endif
#ifndef CONFIG_GW_QWEN_TOKEN
#define CONFIG_GW_QWEN_TOKEN ""
#endif
#ifndef CONFIG_GW_QWEN_TLS
#define CONFIG_GW_QWEN_TLS 0
#endif
#ifndef CONFIG_GW_QWEN_AUTO_SESSION
#define CONFIG_GW_QWEN_AUTO_SESSION 1
#endif
#ifndef CONFIG_GW_BT_DEVICE_NAME
#define CONFIG_GW_BT_DEVICE_NAME ""
#endif
#ifndef CONFIG_GW_BT_DEVICE_PREFIX
#define CONFIG_GW_BT_DEVICE_PREFIX "Qwen-GW"
#endif
#ifndef CONFIG_GW_BT_AUDIO_DEVICE
#define CONFIG_GW_BT_AUDIO_DEVICE ""
#endif
#ifndef CONFIG_GW_BT_AUTO_RECONNECT
#define CONFIG_GW_BT_AUTO_RECONNECT 1
#endif
#ifndef CONFIG_GW_BT_WHITELIST
#define CONFIG_GW_BT_WHITELIST 0
#endif

static const char *TAG = "config_mgr";

/* NVS namespace names */
#define NVS_NS_WIFI    "wifi"
#define NVS_NS_ETH     "eth"
#define NVS_NS_QWEN    "qwen"
#define NVS_NS_BT      "bt"
#define NVS_NS_SYSTEM  "system"
#define NVS_NS_DEVICE  "device"
#define NVS_KEY_INITIALIZED  "initialized"
#define NVS_KEY_REGISTERED   "registered"
#define NVS_KEY_DEV_PASS     "dev_pass"
#define NVS_KEY_CLR_BOND     "clr_bond"

/* NVS key names */
#define NVS_KEY_WIFI_SSID     "ssid"
#define NVS_KEY_WIFI_PASS     "password"
#define NVS_KEY_ETH_DHCP      "dhcp"
#define NVS_KEY_ETH_IP        "ip"
#define NVS_KEY_ETH_NETMASK   "netmask"
#define NVS_KEY_ETH_GATEWAY   "gateway"
#define NVS_KEY_ETH_DNS       "dns"
#define NVS_KEY_QWEN_HOST     "host"
#define NVS_KEY_QWEN_PORT     "port"
#define NVS_KEY_QWEN_PATH     "path"
#define NVS_KEY_QWEN_TOKEN    "token"
#define NVS_KEY_QWEN_TLS      "tls"
#define NVS_KEY_QWEN_AUTO     "auto"
#define NVS_KEY_BT_DEV_NAME      "dev_name"
#define NVS_KEY_BT_MODE          "mode"
#define NVS_KEY_BT_AUDIO_DEV     "audio_dev"
#define NVS_KEY_BT_AUTO_RECONNECT "auto_rec"
#define NVS_KEY_BT_WHITELIST      "wl_en"

static const char *bt_name_prefix(void)
{
    return CONFIG_GW_BT_DEVICE_PREFIX[0] ? CONFIG_GW_BT_DEVICE_PREFIX : "Qwen-GW";
}

/* Static configuration instance */
static system_config_t s_config;
static bool s_initialized = false;
static bool s_registered = false;
static char s_device_password[33];

static void str_default(char *dest, size_t dest_size, const char *src)
{
    if (dest == NULL || dest_size == 0) {
        return;
    }
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static void config_make_default_bt_device_name(char *dest, size_t dest_size)
{
    if (CONFIG_GW_BT_DEVICE_NAME[0] != '\0') {
        str_default(dest, dest_size, CONFIG_GW_BT_DEVICE_NAME);
        return;
    }

    const char *prefix = bt_name_prefix();
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BT);

    if (err != ESP_OK) {
        err = esp_efuse_mac_get_default(mac);
    }

    if (err == ESP_OK) {
        snprintf(dest, dest_size, "%s-%02X%02X%02X",
                 prefix, mac[3], mac[4], mac[5]);
    } else {
        str_default(dest, dest_size, prefix);
        ESP_LOGW(TAG, "Failed to read MAC for default BT name: 0x%x", err);
    }
}

static bool config_bt_device_name_is_custom(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_BT, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    char name[sizeof(s_config.bt_device_name)];
    size_t len = sizeof(name);
    err = nvs_get_str(handle, NVS_KEY_BT_DEV_NAME, name, &len);
    nvs_close(handle);

    if (err != ESP_OK || name[0] == '\0') {
        return false;
    }

    return strcmp(name, "ESP32-VoIP") != 0 && strcmp(name, "ESP32-GW") != 0;
}

/**
 * Apply menuconfig factory defaults (idf.py menuconfig →
 * Device Default Configuration). NVS values overlay these later.
 */
static void config_apply_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));

    str_default(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), CONFIG_GW_WIFI_SSID);
    str_default(s_config.wifi_password, sizeof(s_config.wifi_password),
                CONFIG_GW_WIFI_PASSWORD);

    s_config.eth_dhcp = CONFIG_GW_ETH_DHCP;
    str_default(s_config.eth_ip, sizeof(s_config.eth_ip), CONFIG_GW_ETH_IP);
    str_default(s_config.eth_netmask, sizeof(s_config.eth_netmask), CONFIG_GW_ETH_NETMASK);
    str_default(s_config.eth_gateway, sizeof(s_config.eth_gateway), CONFIG_GW_ETH_GATEWAY);
    str_default(s_config.eth_dns, sizeof(s_config.eth_dns), CONFIG_GW_ETH_DNS);
    if (s_config.eth_netmask[0] == '\0') {
        str_default(s_config.eth_netmask, sizeof(s_config.eth_netmask), "255.255.255.0");
    }

    str_default(s_config.qwen_host, sizeof(s_config.qwen_host), CONFIG_GW_QWEN_HOST);
    s_config.qwen_port = (uint16_t)CONFIG_GW_QWEN_PORT;
    if (s_config.qwen_port == 0) {
        s_config.qwen_port = 3101;
    }
    str_default(s_config.qwen_path, sizeof(s_config.qwen_path), CONFIG_GW_QWEN_PATH);
    if (s_config.qwen_path[0] == '\0') {
        str_default(s_config.qwen_path, sizeof(s_config.qwen_path), "/api/realtime");
    }
    str_default(s_config.qwen_token, sizeof(s_config.qwen_token), CONFIG_GW_QWEN_TOKEN);
    s_config.qwen_tls = CONFIG_GW_QWEN_TLS;
    s_config.qwen_auto_session = CONFIG_GW_QWEN_AUTO_SESSION;

    config_make_default_bt_device_name(s_config.bt_device_name, sizeof(s_config.bt_device_name));
#if defined(CONFIG_GW_BT_MODE_A2DP)
    s_config.bt_mode = BT_MODE_A2DP;
#elif defined(CONFIG_GW_BT_MODE_HFP_AG)
    s_config.bt_mode = BT_MODE_HFP_AG;
#else
    s_config.bt_mode = BT_MODE_HFP;
#endif
    str_default(s_config.bt_audio_device_name, sizeof(s_config.bt_audio_device_name),
                CONFIG_GW_BT_AUDIO_DEVICE);
    s_config.bt_auto_reconnect = CONFIG_GW_BT_AUTO_RECONNECT;
    s_config.bt_whitelist = CONFIG_GW_BT_WHITELIST;
}

/**
 * Read a string from NVS. If the key doesn't exist or read fails,
 * the destination buffer is left unchanged (retains default).
 */
static void nvs_read_string(nvs_handle_t handle, const char *key, char *dest, size_t dest_size)
{
    size_t required_size = dest_size;
    esp_err_t err = nvs_get_str(handle, key, dest, &required_size);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS read '%s' failed (0x%x), using default", key, err);
    }
}

static bool is_valid_ipv4(const char *addr)
{
    if (addr == NULL || addr[0] == '\0') {
        return false;
    }

    int a, b, c, d;
    char tail;
    if (sscanf(addr, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
    if (a < 0 || a > 255 || b < 0 || b > 255 ||
            c < 0 || c > 255 || d < 0 || d > 255) {
        return false;
    }

    return esp_ip4addr_aton(addr) != 0;
}

/**
 * Factory MAC from eFuse. Current ESP-IDF equivalent of
 *   esp_efuse_read_mac(ESP_EFUSE_MAC_FACTORY, efuse_mac);
 */
static esp_err_t read_factory_mac(uint8_t mac[6])
{
#ifdef ESP_MAC_EFUSE_FACTORY
    esp_err_t err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (err == ESP_OK) {
        return ESP_OK;
    }
#endif
    return esp_efuse_mac_get_default(mac);
}

static void config_load_device_identity(void)
{
    s_registered = false;
    s_device_password[0] = '\0';

    nvs_handle_t handle;
    if (nvs_open(NVS_NS_DEVICE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    uint8_t registered = 0;
    if (nvs_get_u8(handle, NVS_KEY_REGISTERED, &registered) == ESP_OK) {
        s_registered = (registered != 0);
    }

    size_t len = sizeof(s_device_password);
    nvs_get_str(handle, NVS_KEY_DEV_PASS, s_device_password, &len);
    nvs_close(handle);
}

void config_get_device_id(char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) {
        return;
    }
    out[0] = '\0';

    uint8_t efuse_mac[6] = {0};
    if (read_factory_mac(efuse_mac) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read factory MAC from eFuse");
        return;
    }
    snprintf(out, out_sz, "%02X%02X%02X%02X%02X%02X",
             efuse_mac[0], efuse_mac[1], efuse_mac[2],
             efuse_mac[3], efuse_mac[4], efuse_mac[5]);
}

bool config_is_registered(void)
{
    return s_registered;
}

const char *config_get_device_password(void)
{
    return s_device_password;
}

esp_err_t config_mark_registered(const char *device_password)
{
    if (device_password == NULL || device_password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(device_password) >= sizeof(s_device_password)) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open '%s' for write failed: 0x%x", NVS_NS_DEVICE, err);
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_REGISTERED, 1);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_DEV_PASS, device_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        strncpy(s_device_password, device_password, sizeof(s_device_password) - 1);
        s_device_password[sizeof(s_device_password) - 1] = '\0';
        s_registered = true;
        ESP_LOGI(TAG, "Device marked as registered");
    }
    return err;
}

/**
 * Load all configuration parameters from NVS.
 * On any read failure, the default value is retained.
 */
static void config_load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    /* Load WiFi parameters */
    err = nvs_open(NVS_NS_WIFI, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_read_string(handle, NVS_KEY_WIFI_SSID, s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
        nvs_read_string(handle, NVS_KEY_WIFI_PASS, s_config.wifi_password, sizeof(s_config.wifi_password));
        nvs_close(handle);
    } else {
        ESP_LOGD(TAG, "NVS open '%s' failed (0x%x), using defaults", NVS_NS_WIFI, err);
    }

    /* Load Ethernet parameters */
    err = nvs_open(NVS_NS_ETH, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t dhcp = 1;
        if (nvs_get_u8(handle, NVS_KEY_ETH_DHCP, &dhcp) == ESP_OK) {
            s_config.eth_dhcp = (dhcp != 0);
        }
        nvs_read_string(handle, NVS_KEY_ETH_IP, s_config.eth_ip, sizeof(s_config.eth_ip));
        nvs_read_string(handle, NVS_KEY_ETH_NETMASK, s_config.eth_netmask,
                        sizeof(s_config.eth_netmask));
        nvs_read_string(handle, NVS_KEY_ETH_GATEWAY, s_config.eth_gateway,
                        sizeof(s_config.eth_gateway));
        nvs_read_string(handle, NVS_KEY_ETH_DNS, s_config.eth_dns, sizeof(s_config.eth_dns));
        nvs_close(handle);
    } else {
        ESP_LOGD(TAG, "NVS open '%s' failed (0x%x), using defaults", NVS_NS_ETH, err);
    }

    /* Load Qwen parameters */
    err = nvs_open(NVS_NS_QWEN, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_read_string(handle, NVS_KEY_QWEN_HOST, s_config.qwen_host, sizeof(s_config.qwen_host));

        uint16_t port = 0;
        if (nvs_get_u16(handle, NVS_KEY_QWEN_PORT, &port) == ESP_OK && port != 0) {
            s_config.qwen_port = port;
        }

        nvs_read_string(handle, NVS_KEY_QWEN_PATH, s_config.qwen_path, sizeof(s_config.qwen_path));
        nvs_read_string(handle, NVS_KEY_QWEN_TOKEN, s_config.qwen_token, sizeof(s_config.qwen_token));
        uint8_t tls = 0;
        if (nvs_get_u8(handle, NVS_KEY_QWEN_TLS, &tls) == ESP_OK) {
            s_config.qwen_tls = (tls != 0);
        }
        uint8_t auto_sess = 1;
        if (nvs_get_u8(handle, NVS_KEY_QWEN_AUTO, &auto_sess) == ESP_OK) {
            s_config.qwen_auto_session = (auto_sess != 0);
        }
        nvs_close(handle);
    } else {
        ESP_LOGD(TAG, "NVS open '%s' failed (0x%x), using defaults", NVS_NS_QWEN, err);
    }

    /* Load Bluetooth parameters */
    err = nvs_open(NVS_NS_BT, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_read_string(handle, NVS_KEY_BT_DEV_NAME, s_config.bt_device_name,
                        sizeof(s_config.bt_device_name));
        uint8_t mode = 0;
        if (nvs_get_u8(handle, NVS_KEY_BT_MODE, &mode) == ESP_OK &&
            mode <= (uint8_t)BT_MODE_HFP_AG) {
            s_config.bt_mode = (bt_audio_mode_t)mode;
        }
        nvs_read_string(handle, NVS_KEY_BT_AUDIO_DEV, s_config.bt_audio_device_name,
                        sizeof(s_config.bt_audio_device_name));
        uint8_t auto_rec = 1;
        if (nvs_get_u8(handle, NVS_KEY_BT_AUTO_RECONNECT, &auto_rec) == ESP_OK) {
            s_config.bt_auto_reconnect = (bool)auto_rec;
        }
        uint8_t wl = 0;
        if (nvs_get_u8(handle, NVS_KEY_BT_WHITELIST, &wl) == ESP_OK) {
            s_config.bt_whitelist = (bool)wl;
        }
        nvs_close(handle);
    } else {
        ESP_LOGD(TAG, "NVS open '%s' failed (0x%x), using defaults", NVS_NS_BT, err);
    }
}

esp_err_t config_manager_init(void)
{
    /* Initialize NVS flash */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated or version mismatch; erase and retry */
        ESP_LOGW(TAG, "NVS init failed (0x%x), erasing and retrying", err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: 0x%x", err);
        return err;
    }

    /* Apply defaults first, then overlay with NVS values */
    config_apply_defaults();
    char generated_bt_name[sizeof(s_config.bt_device_name)];
    strncpy(generated_bt_name, s_config.bt_device_name, sizeof(generated_bt_name) - 1);
    generated_bt_name[sizeof(generated_bt_name) - 1] = '\0';
    bool bt_name_is_custom = config_bt_device_name_is_custom();
    config_load_from_nvs();

    if (!bt_name_is_custom) {
        esp_err_t bt_err = config_set_bluetooth(generated_bt_name);
        if (bt_err == ESP_OK) {
            ESP_LOGI(TAG, "Generated default BT device name: %s", s_config.bt_device_name);
        } else {
            ESP_LOGW(TAG, "Failed to persist generated BT device name: 0x%x", bt_err);
        }
    }

    config_load_device_identity();

    s_initialized = true;
    {
        char id[CONFIG_DEVICE_ID_LEN];
        config_get_device_id(id, sizeof(id));
        ESP_LOGI(TAG, "Config manager initialized (registered=%d id=%s)",
                 s_registered ? 1 : 0, id);
    }
    return ESP_OK;
}

const system_config_t* config_get(void)
{
    if (!s_initialized) {
        return NULL;
    }
    return &s_config;
}

esp_err_t config_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open '%s' for write failed: 0x%x", NVS_NS_WIFI, err);
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_WIFI_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set '%s' failed: 0x%x", NVS_KEY_WIFI_SSID, err);
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_WIFI_PASS, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set '%s' failed: 0x%x", NVS_KEY_WIFI_PASS, err);
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        strncpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid) - 1);
        s_config.wifi_ssid[sizeof(s_config.wifi_ssid) - 1] = '\0';
        strncpy(s_config.wifi_password, password, sizeof(s_config.wifi_password) - 1);
        s_config.wifi_password[sizeof(s_config.wifi_password) - 1] = '\0';
    }

    return err;
}

esp_err_t config_set_ethernet(bool dhcp, const char *ip, const char *netmask,
                              const char *gateway, const char *dns)
{
    const char *ip_s = ip ? ip : "";
    const char *mask_s = netmask ? netmask : "";
    const char *gw_s = gateway ? gateway : "";
    const char *dns_s = dns ? dns : "";

    if (!dhcp) {
        if (!is_valid_ipv4(ip_s) || !is_valid_ipv4(mask_s) ||
                !is_valid_ipv4(gw_s) ||
                (dns_s[0] != '\0' && !is_valid_ipv4(dns_s))) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_ETH, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open '%s' for write failed: 0x%x", NVS_NS_ETH, err);
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_ETH_DHCP, dhcp ? 1 : 0);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(handle, NVS_KEY_ETH_IP, ip_s);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(handle, NVS_KEY_ETH_NETMASK, mask_s);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(handle, NVS_KEY_ETH_GATEWAY, gw_s);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(handle, NVS_KEY_ETH_DNS, dns_s);
    if (err != ESP_OK) goto fail;

    err = nvs_commit(handle);
    if (err != ESP_OK) goto fail;
    nvs_close(handle);

    s_config.eth_dhcp = dhcp;
    strncpy(s_config.eth_ip, ip_s, sizeof(s_config.eth_ip) - 1);
    s_config.eth_ip[sizeof(s_config.eth_ip) - 1] = '\0';
    strncpy(s_config.eth_netmask, mask_s, sizeof(s_config.eth_netmask) - 1);
    s_config.eth_netmask[sizeof(s_config.eth_netmask) - 1] = '\0';
    strncpy(s_config.eth_gateway, gw_s, sizeof(s_config.eth_gateway) - 1);
    s_config.eth_gateway[sizeof(s_config.eth_gateway) - 1] = '\0';
    strncpy(s_config.eth_dns, dns_s, sizeof(s_config.eth_dns) - 1);
    s_config.eth_dns[sizeof(s_config.eth_dns) - 1] = '\0';

    return ESP_OK;

fail:
    ESP_LOGE(TAG, "NVS write Ethernet param failed: 0x%x", err);
    nvs_close(handle);
    return err;
}

esp_err_t config_set_qwen(const char *host, uint16_t port, const char *path,
                          const char *token, bool tls)
{
    if (!host || host[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (port == 0) {
        port = 3101;
    }
    const char *use_path = (path && path[0]) ? path : "/api/realtime";
    const char *use_token = token ? token : "";

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_QWEN, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open '%s' for write failed: 0x%x", NVS_NS_QWEN, err);
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_QWEN_HOST, host);
    if (err != ESP_OK) goto fail;
    err = nvs_set_u16(handle, NVS_KEY_QWEN_PORT, port);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(handle, NVS_KEY_QWEN_PATH, use_path);
    if (err != ESP_OK) goto fail;
    err = nvs_set_str(handle, NVS_KEY_QWEN_TOKEN, use_token);
    if (err != ESP_OK) goto fail;
    err = nvs_set_u8(handle, NVS_KEY_QWEN_TLS, tls ? 1 : 0);
    if (err != ESP_OK) goto fail;
    err = nvs_commit(handle);
    if (err != ESP_OK) goto fail;
    nvs_close(handle);

    strncpy(s_config.qwen_host, host, sizeof(s_config.qwen_host) - 1);
    s_config.qwen_host[sizeof(s_config.qwen_host) - 1] = '\0';
    s_config.qwen_port = port;
    strncpy(s_config.qwen_path, use_path, sizeof(s_config.qwen_path) - 1);
    s_config.qwen_path[sizeof(s_config.qwen_path) - 1] = '\0';
    strncpy(s_config.qwen_token, use_token, sizeof(s_config.qwen_token) - 1);
    s_config.qwen_token[sizeof(s_config.qwen_token) - 1] = '\0';
    s_config.qwen_tls = tls;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "NVS write Qwen param failed: 0x%x", err);
    nvs_close(handle);
    return err;
}

esp_err_t config_set_qwen_auto_session(bool enable)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_QWEN, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, NVS_KEY_QWEN_AUTO, enable ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        s_config.qwen_auto_session = enable;
    }
    return err;
}

esp_err_t config_set_bluetooth(const char *device_name)
{
    if (!device_name) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_BT, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open '%s' for write failed: 0x%x", NVS_NS_BT, err);
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_BT_DEV_NAME, device_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set '%s' failed: 0x%x", NVS_KEY_BT_DEV_NAME, err);
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        strncpy(s_config.bt_device_name, device_name, sizeof(s_config.bt_device_name) - 1);
        s_config.bt_device_name[sizeof(s_config.bt_device_name) - 1] = '\0';
    }

    return err;
}

esp_err_t config_set_bt_mode(bt_audio_mode_t mode, const char *audio_device_name)
{
    if (mode > BT_MODE_HFP_AG) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_BT, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open '%s' for write failed: 0x%x", NVS_NS_BT, err);
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_BT_MODE, (uint8_t)mode);
    if (err != ESP_OK) goto fail;

    const char *dev = audio_device_name ? audio_device_name : "";
    err = nvs_set_str(handle, NVS_KEY_BT_AUDIO_DEV, dev);
    if (err != ESP_OK) goto fail;

    err = nvs_commit(handle);
    if (err != ESP_OK) goto fail;

    nvs_close(handle);

    s_config.bt_mode = mode;
    strncpy(s_config.bt_audio_device_name, dev,
            sizeof(s_config.bt_audio_device_name) - 1);
    s_config.bt_audio_device_name[sizeof(s_config.bt_audio_device_name) - 1] = '\0';
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "NVS write bt_mode failed: 0x%x", err);
    nvs_close(handle);
    return err;
}

esp_err_t config_set_bt_auto_reconnect(bool enable)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_BT, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open '%s' for write failed: 0x%x", NVS_NS_BT, err);
        return err;
    }
    err = nvs_set_u8(handle, NVS_KEY_BT_AUTO_RECONNECT, enable ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set '%s' failed: 0x%x", NVS_KEY_BT_AUTO_RECONNECT, err);
        nvs_close(handle);
        return err;
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        s_config.bt_auto_reconnect = enable;
        ESP_LOGI(TAG, "bt_auto_reconnect set to %d", enable);
    }
    return err;
}

esp_err_t config_set_bt_whitelist(bool enable)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_BT, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open '%s' for write failed: 0x%x", NVS_NS_BT, err);
        return err;
    }
    err = nvs_set_u8(handle, NVS_KEY_BT_WHITELIST, enable ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS set '%s' failed: 0x%x", NVS_KEY_BT_WHITELIST, err);
        nvs_close(handle);
        return err;
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        s_config.bt_whitelist = enable;
        ESP_LOGI(TAG, "bt_whitelist set to %d", enable);
    }
    return err;
}

static void nvs_erase_namespace(const char *ns)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
}

static void clear_classic_bonds(void)
{
#if CONFIG_BT_ENABLED
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        return;
    }
    int num = esp_bt_gap_get_bond_device_num();
    if (num <= 0) {
        return;
    }
    if (num > 16) {
        num = 16;
    }
    esp_bd_addr_t list[16];
    memset(list, 0, sizeof(list));
    int n = num;
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read BT bond list");
        return;
    }
    for (int i = 0; i < n; i++) {
        esp_err_t err = esp_bt_gap_remove_bond_device(list[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "remove_bond_device failed: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "Removed %d Classic BT bond(s)", n);
#endif
}

static void mark_clear_bonds_pending(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_u8(handle, NVS_KEY_CLR_BOND, 1);
    nvs_commit(handle);
    nvs_close(handle);
}

void config_clear_bt_bonds_if_pending(void)
{
    nvs_handle_t handle;
    uint8_t flag = 0;
    if (nvs_open(NVS_NS_DEVICE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    nvs_get_u8(handle, NVS_KEY_CLR_BOND, &flag);
    nvs_close(handle);
    if (!flag) {
        return;
    }
    clear_classic_bonds();
    if (nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_CLR_BOND);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

esp_err_t config_reset_defaults(void)
{
    nvs_erase_namespace(NVS_NS_WIFI);
    nvs_erase_namespace(NVS_NS_ETH);
    nvs_erase_namespace(NVS_NS_QWEN);
    nvs_erase_namespace(NVS_NS_BT);
    nvs_erase_namespace(NVS_NS_SYSTEM);
    nvs_erase_namespace("hfp_mgr");
    nvs_erase_namespace("hfp_ag");
    bt_whitelist_clear();
    /* Keep NVS_NS_DEVICE (platform registration). */

    mark_clear_bonds_pending();
    clear_classic_bonds();
    config_apply_defaults();

    ESP_LOGI(TAG, "Configuration reset to factory defaults (identity kept)");
    return ESP_OK;
}

bool config_is_first_boot(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return true;  /* namespace doesn't exist → first boot */
    }
    uint8_t initialized = 0;
    err = nvs_get_u8(handle, NVS_KEY_INITIALIZED, &initialized);
    nvs_close(handle);
    return (err != ESP_OK || initialized == 0);
}

void config_mark_configured(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS_SYSTEM, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open system NVS for marking configured: 0x%x", err);
        return;
    }
    nvs_set_u8(handle, NVS_KEY_INITIALIZED, 1);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Device marked as configured");
}

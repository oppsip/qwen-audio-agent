/**
 * WiFi Manager - STA mode connection management
 *
 * Manages WiFi connection in Station (STA) mode. Reads credentials from
 * Config Manager, handles connection/disconnection events, and notifies
 * the application layer via callbacks.
 *
 * Requirements: 1.1, 1.5
 */

#include "wifi_manager.h"
#include "config_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_mgr";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_DISCONNECTED_BIT BIT1

/* Internal state */
static EventGroupHandle_t s_wifi_event_group;
static bool s_is_connected = false;
static const wifi_manager_callbacks_t *s_callbacks = NULL;
static int s_retry_count = 0;
static bool s_is_initialized = false;
static bool s_is_started = false;
static bool s_stop_requested = false;
static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_reconnect_timer;

#define WIFI_MAX_RETRY 10
#define WIFI_AUTH_FAIL_RETRY 3
#define WIFI_RECONNECT_DELAY_US (30ULL * 1000 * 1000)

static bool wifi_reason_is_auth_fail(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_802_1X_AUTH_FAILED;
}

static void wifi_schedule_reconnect(uint64_t delay_us)
{
    if (s_reconnect_timer == NULL || s_stop_requested || !s_is_started) {
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_err_t err = esp_timer_start_once(s_reconnect_timer, delay_us);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to arm WiFi reconnect timer: %s", esp_err_to_name(err));
    }
}

static void wifi_ensure_dns(void)
{
    esp_netif_dns_info_t main_dns = {0};
    esp_netif_dns_info_t backup = {0};

    if (s_sta_netif == NULL) {
        return;
    }

    (void)esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &main_dns);
    if (main_dns.ip.type != ESP_IPADDR_TYPE_V4 || main_dns.ip.u_addr.ip4.addr == 0) {
        main_dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4("223.5.5.5", &main_dns.ip.u_addr.ip4) == ESP_OK &&
            esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &main_dns) == ESP_OK) {
            ESP_LOGW(TAG, "WiFi DHCP gave no DNS, using 223.5.5.5");
        }
    } else {
        ESP_LOGI(TAG, "WiFi DNS: " IPSTR, IP2STR(&main_dns.ip.u_addr.ip4));
    }

    backup.ip.type = ESP_IPADDR_TYPE_V4;
    if (esp_netif_str_to_ip4("8.8.8.8", &backup.ip.u_addr.ip4) == ESP_OK) {
        (void)esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &backup);
    }
}

static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_stop_requested || !s_is_started || s_is_connected) {
        return;
    }
    s_retry_count = 0;
    ESP_LOGI(TAG, "WiFi reconnect timer fired");
    esp_wifi_connect();
}

/* ============================================================
 * Event handlers
 * ============================================================ */

static void wifi_apply_voice_phy(void)
{
    uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G;
    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, proto);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_protocol 11b/g failed: %s", esp_err_to_name(err));
        return;
    }
#ifdef WIFI_BW20
    err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
#elif defined(WIFI_BW_HT20)
    err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
#else
    err = ESP_OK;
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_bandwidth 20 MHz failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WiFi airtime: 11b/g HT20 (voice + SCO coexist)");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                wifi_apply_voice_phy();
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disconn =
                    (wifi_event_sta_disconnected_t *)event_data;
                uint8_t reason = disconn ? disconn->reason : 0;
                int max_retry = wifi_reason_is_auth_fail(reason)
                                ? WIFI_AUTH_FAIL_RETRY : WIFI_MAX_RETRY;

                s_is_connected = false;
                xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

                if (s_stop_requested) {
                    ESP_LOGI(TAG, "WiFi stopped by network policy");
                    break;
                }

                if (s_callbacks && s_callbacks->on_disconnected) {
                    s_callbacks->on_disconnected();
                }

                /* Never block the WiFi event loop (vTaskDelay here used to
                 * freeze Ethernet/IP events and stall boot). Wrong password
                 * is retried a few times, then backed off for 30s. */
                if (s_retry_count < max_retry) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "WiFi disconnected reason=%u, retry %d/%d...",
                             (unsigned)reason, s_retry_count, max_retry);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "WiFi %s (reason=%u), next try in 30s",
                             wifi_reason_is_auth_fail(reason)
                                 ? "auth failed (wrong password?)"
                                 : "max retries reached",
                             (unsigned)reason);
                    wifi_schedule_reconnect(WIFI_RECONNECT_DELAY_US);
                }
                break;
            }

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi STA connected to AP");
                s_retry_count = 0;
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));

            s_is_connected = true;
            s_retry_count = 0;
            if (s_reconnect_timer) {
                esp_timer_stop(s_reconnect_timer);
            }
            wifi_ensure_dns();
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);

            if (s_callbacks && s_callbacks->on_connected) {
                s_callbacks->on_connected();
            }
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            s_is_connected = false;
            xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            ESP_LOGW(TAG, "WiFi lost IP");
            if (!s_stop_requested && s_callbacks && s_callbacks->on_disconnected) {
                s_callbacks->on_disconnected();
            }
        }
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t wifi_manager_init(const wifi_manager_callbacks_t *callbacks)
{
    s_callbacks = callbacks;

    if (s_is_initialized) {
        return wifi_manager_start();
    }

    /* Get WiFi credentials from config */
    const system_config_t *cfg = config_get();
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Config not available");
        return ESP_ERR_INVALID_STATE;
    }

    if (strlen(cfg->wifi_ssid) == 0) {
        ESP_LOGW(TAG, "WiFi SSID not configured");
        return ESP_ERR_INVALID_ARG;
    }

    /* Create event group */
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    if (s_reconnect_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = &wifi_reconnect_timer_cb,
            .name = "wifi_reconn",
        };
        esp_err_t terr = esp_timer_create(&targs, &s_reconnect_timer);
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create WiFi reconnect timer: %s",
                     esp_err_to_name(terr));
            vEventGroupDelete(s_wifi_event_group);
            s_wifi_event_group = NULL;
            return terr;
        }
    }

    /* Initialize TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop init failed: %s", esp_err_to_name(loop_err));
        return loop_err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* Initialize WiFi with default config */
    /*
     * Disable AMPDU/AMSDU aggregation: aggregated frames hold the medium for
     * longer bursts, stealing BT SCO slots in the 2.4 GHz band.  For VoIP the
     * latency reduction is more valuable than the throughput gain.
     * RX AMPDU is also disabled in sdkconfig; mirror it here for clarity.
     */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_init_cfg.ampdu_tx_enable = false;
    wifi_init_cfg.ampdu_rx_enable = false;
    wifi_init_cfg.amsdu_tx_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP,
        &wifi_event_handler, NULL, NULL));

    /* Configure WiFi STA */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, cfg->wifi_ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, cfg->wifi_password,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* Disable WiFi power save for stable SIP/RTP operation */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_is_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized, SSID: %s", cfg->wifi_ssid);

    return wifi_manager_start();
}

esp_err_t wifi_manager_start(void)
{
    if (!s_is_initialized) {
        ESP_LOGE(TAG, "WiFi manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_is_started) {
        return ESP_OK;
    }

    s_stop_requested = false;
    s_retry_count = 0;

    esp_err_t err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_CONN) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_is_started = true;
    wifi_apply_voice_phy();
    ESP_LOGI(TAG, "WiFi started");
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    if (!s_is_initialized || !s_is_started) {
        return ESP_OK;
    }

    s_stop_requested = true;
    s_retry_count = 0;
    s_is_connected = false;
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK &&
        err != ESP_ERR_WIFI_NOT_CONNECT &&
        err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "WiFi disconnect failed: %s", esp_err_to_name(err));
    }

    err = esp_wifi_stop();
    if (err != ESP_OK &&
        err != ESP_ERR_WIFI_NOT_INIT &&
        err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "WiFi stop failed: %s", esp_err_to_name(err));
        return err;
    }

    s_is_started = false;
    ESP_LOGI(TAG, "WiFi stopped");
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

esp_netif_t *wifi_manager_netif(void)
{
    return s_sta_netif;
}

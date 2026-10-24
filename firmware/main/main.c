/**
 * BLU-SIP Gateway - Main Program Entry
 *
 * Initializes all system modules in the correct order and wires up
 * callbacks between components. The initialization sequence is:
 *
 * 1. config_manager_init() — Load configuration from NVS
 * 2. ethernet_manager_init() - Connect to wired Ethernet
 * 3. hfp_client_init()     — Initialize Bluetooth HFP
 * 4. gateway_init()        — Qwen WebSocket client
 * 5. wait for Bluetooth    — open Qwen session when the audio device connects
 * 7. gateway_init()        — Initialize gateway controller
 *
 * Requirements: 1.1, 2.1, 12.5
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <pthread.h>

#include "config_manager.h"
#include "ble_config_server.h"
#include "bt_scan.h"
#include "ota_manager.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "hfp_client_mgr.h"
#include "hfp_ag_mgr.h"
#include "a2dp_mgr.h"
#include "audio_bridge.h"
#include "audio_codec.h"
#include "gateway_controller.h"
#include "web_config.h"
#include "bt_whitelist.h"
#include "pbap_sync.h"
#include "sdkconfig.h"
#include "audio_coex.h"

/* GPIO0 = BOOT button on most ESP32 dev boards.
 * Change this if your hardware uses a different pin. */
#define CFG_TRIGGER_GPIO     13   /* GPIO13 = config button */
#define CFG_BOOT_DEBOUNCE_MS 200    /* ignore GPIO glitch at power-on */
#define CFG_LONG_PRESS_MS    3000   /* runtime hold to open web config */

static const char *TAG = "main";

/* Event group for synchronizing initialization */
static EventGroupHandle_t s_init_event_group;
#define NETWORK_READY_BIT BIT0
#define ETH_FALLBACK_DELAY_MS  10000  /* wait for Ethernet DHCP before WiFi */
#define NETWORK_WAIT_MS        90000  /* wait for any interface to get an IP */

typedef enum {
    NETWORK_IF_NONE = 0,
    NETWORK_IF_ETHERNET,
    NETWORK_IF_WIFI,
} network_if_t;

static network_if_t s_active_network = NETWORK_IF_NONE;
static bool s_wifi_initialized = false;
static volatile bool s_runtime_cfg_busy = false;
static TaskHandle_t s_btn_task;

/* ============================================================
 * Network callbacks - OTA check on first connect, SIP re-register on reconnect
 * ============================================================ */

static bool s_ota_checked = false;
static bool s_ota_check_started = false;

static void start_wifi_fallback(void);

static bool qwen_should_connect_for_bt(void)
{
    const system_config_t *cfg = config_get();
    if (!cfg) {
        return false;
    }
    if (cfg->bt_mode == BT_MODE_A2DP) {
        return a2dp_get_conn_state() == A2DP_STATE_CONNECTED;
    }
    if (cfg->bt_mode == BT_MODE_HFP_AG) {
        return hfp_ag_get_conn_state() >= HFP_STATE_SLC_CONNECTED;
    }
    // A connected phone is idle until it reports an incoming call.
    return false;
}

/* Sync system time via SNTP before any HTTPS request.
 * Uses Alibaba Cloud NTP (reliable in mainland China).
 * Checks the actual year rather than sync status, because sntp_get_sync_status()
 * can transition away from RESET on the first failed request, causing early exit. */
static void sync_sntp_time(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");    /* Alibaba Cloud, stable in China */
    esp_sntp_setservername(1, "cn.pool.ntp.org");   /* fallback */
    esp_sntp_init();

    time_t now = 0;
    struct tm ti = {0};
    for (int i = 0; i < 30; i++) {                  /* up to 15 seconds */
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &ti);
        if (ti.tm_year >= (2020 - 1900)) {
            ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
            return;
        }
    }
    ESP_LOGW(TAG, "SNTP sync timeout — TLS cert validation may fail");
}
static void ota_check_task(void *arg)
{
    /* Runs once after first network connect.  If an update is found,
     * download_and_apply() reboots the device and never returns here.
     * Otherwise we fall through and unblock the main boot sequence. */
    sync_sntp_time();
    ota_manager_check_and_update();
    s_ota_checked = true;
    ESP_LOGI(TAG, "OTA check complete, continuing boot");
    xEventGroupSetBits(s_init_event_group, NETWORK_READY_BIT);
    vTaskDelete(NULL);
}

static void on_network_connected(const char *if_name)
{
    if (!s_ota_check_started) {
        s_ota_check_started = true;
        ESP_LOGI(TAG, "%s connected, running OTA check before boot", if_name);
        xTaskCreate(ota_check_task, "ota_check", 8192, NULL, 5, NULL);
    } else if (!s_ota_checked) {
        ESP_LOGI(TAG, "%s connected while OTA check is already running", if_name);
    } else {
        ESP_LOGI(TAG, "%s connected", if_name);
        xEventGroupSetBits(s_init_event_group, NETWORK_READY_BIT);
        if (qwen_should_connect_for_bt()) {
            ESP_LOGI(TAG, "Starting Qwen session after %s reconnect", if_name);
            gateway_try_start_session("network reconnect");
        }
    }
}

static void network_apply_default_route(void)
{
    if (ethernet_manager_is_connected()) {
        esp_netif_t *eth = ethernet_manager_netif();
        if (eth != NULL) {
            esp_err_t err = esp_netif_set_default_netif(eth);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Set default netif to Ethernet failed: %s",
                         esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Default route: Ethernet");
            }
        }
        s_active_network = NETWORK_IF_ETHERNET;
        return;
    }

    if (wifi_manager_is_connected()) {
        esp_netif_t *wifi = wifi_manager_netif();
        if (wifi != NULL) {
            esp_err_t err = esp_netif_set_default_netif(wifi);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Set default netif to WiFi failed: %s",
                         esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Default route: WiFi");
            }
        }
        s_active_network = NETWORK_IF_WIFI;
    }
}

static void on_ethernet_link_up(void)
{
    /* LAN8720 often reports false link. Do not stop WiFi until GOT_IP. */
    ESP_LOGI(TAG, "Ethernet link is up, waiting for IP (keeping WiFi)");
}

static void on_ethernet_connected(void)
{
    network_apply_default_route();
    on_network_connected("Ethernet");

    if (s_wifi_initialized) {
        ESP_LOGI(TAG, "Ethernet has IP, stopping WiFi");
        wifi_manager_stop();
    }
}

static void on_ethernet_disconnected(void)
{
    if (s_active_network == NETWORK_IF_ETHERNET) {
        s_active_network = NETWORK_IF_NONE;
        xEventGroupClearBits(s_init_event_group, NETWORK_READY_BIT);
    }

    ESP_LOGW(TAG, "Ethernet disconnected, starting WiFi fallback");
    start_wifi_fallback();
    network_apply_default_route();
}

static void on_wifi_connected(void)
{
    if (ethernet_manager_is_connected()) {
        ESP_LOGI(TAG, "WiFi connected but Ethernet is active, stopping WiFi");
        wifi_manager_stop();
        return;
    }

    network_apply_default_route();
    on_network_connected("WiFi");
    audio_coex_set_idle();
}

static void on_wifi_disconnected(void)
{
    if (s_active_network == NETWORK_IF_WIFI) {
        s_active_network = NETWORK_IF_NONE;
        xEventGroupClearBits(s_init_event_group, NETWORK_READY_BIT);
    }
    ESP_LOGW(TAG, "WiFi disconnected");
    network_apply_default_route();
}

static const ethernet_manager_callbacks_t s_ethernet_callbacks = {
    .on_link_up = on_ethernet_link_up,
    .on_connected = on_ethernet_connected,
    .on_disconnected = on_ethernet_disconnected,
};

static const wifi_manager_callbacks_t s_wifi_callbacks = {
    .on_connected = on_wifi_connected,
    .on_disconnected = on_wifi_disconnected,
};

static void start_wifi_fallback(void)
{
    esp_err_t ret;

    if (ethernet_manager_is_connected()) {
        ESP_LOGI(TAG, "Ethernet is connected, WiFi fallback not needed");
        return;
    }

    if (!s_wifi_initialized) {
        ret = wifi_manager_init(&s_wifi_callbacks);
        if (ret == ESP_OK) {
            s_wifi_initialized = true;
        }
    } else {
        ret = wifi_manager_start();
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi fallback unavailable: %s", esp_err_to_name(ret));
    }
}

static void ethernet_fallback_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(ETH_FALLBACK_DELAY_MS));
    if (!ethernet_manager_is_connected()) {
        ESP_LOGW(TAG, "Ethernet not ready after %d ms, starting WiFi fallback",
                 ETH_FALLBACK_DELAY_MS);
        start_wifi_fallback();
    }
    vTaskDelete(NULL);
}

/* ============================================================
 * Config mode trigger: GPIO long-press detection
 * ============================================================ */

static void config_button_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CFG_TRIGGER_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config button GPIO%d init failed: %s",
                 CFG_TRIGGER_GPIO, esp_err_to_name(err));
    }
}

static inline int gpio_read_level(int gpio_num)
{
    return gpio_get_level(gpio_num);
}

static bool check_config_trigger(void)
{
    if (gpio_read_level(CFG_TRIGGER_GPIO) != 0) {
        return false;  /* not pressed at power-on */
    }

    int elapsed = 0;
    while (elapsed < CFG_BOOT_DEBOUNCE_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        elapsed += 20;
        if (gpio_read_level(CFG_TRIGGER_GPIO) != 0) {
            ESP_LOGI(TAG, "Config button bounce at boot, ignoring");
            return false;
        }
    }
    ESP_LOGI(TAG, "Config button pressed at boot — entering BLE config mode");
    return true;
}

/* ============================================================
 * BLE config mode: start server and wait until the phone saves config
 * ============================================================ */

static void run_ble_config_mode(const char *bt_name)
{
    ESP_LOGI(TAG, "=== BLE Config Mode ===");
    ESP_LOGI(TAG, "Open WeChat Mini Program and scan for \"%s-CFG\"", bt_name);
    ESP_LOGI(TAG, "Waiting until configuration is saved (no timeout)");

    esp_err_t ret = ble_config_server_init(bt_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE config server init failed: %s", esp_err_to_name(ret));
        return;
    }
    ble_config_server_start();

    while (!ble_config_server_wait_for_reboot(0)) {
        ESP_LOGW(TAG, "BLE wait returned without reboot request, waiting again");
    }

    ble_config_server_stop();
    config_mark_configured();
    ESP_LOGI(TAG, "Config saved — rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void enter_ble_config_mode(void)
{
    bt_scan_release_stack();
    const system_config_t *pre_cfg = config_get();
    const char *bt_name = (pre_cfg && pre_cfg->bt_device_name[0])
                          ? pre_cfg->bt_device_name : "Qwen-GW";
    run_ble_config_mode(bt_name);
}

static void wait_for_network(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "Waiting for network connection...");
    EventBits_t bits = xEventGroupWaitBits(s_init_event_group,
                                           NETWORK_READY_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & NETWORK_READY_BIT)) {
        ESP_LOGW(TAG, "Network connection timeout");
    }
}

/* ============================================================
 * Runtime config button: 3-second hold opens web config.
 * BLE is only entered when the button is held at power-on.
 * ============================================================ */

static void runtime_web_config_run(void)
{
    web_config_result_t r = web_config_run_session(WEB_CONFIG_MODE_RUNTIME,
                                                   WEB_CONFIG_RUNTIME_TIMEOUT_MS);
    if (r == WEB_CONFIG_APPLIED) {
        ESP_LOGI("btn", "Runtime web config applied — rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    if (r == WEB_CONFIG_UNAVAILABLE) {
        ESP_LOGW("btn", "Web config unavailable (no network or platform URL). "
                 "Power on with config button held for BLE setup");
    } else {
        ESP_LOGI("btn", "Runtime web config ended (%d), work continues", (int)r);
    }
    s_runtime_cfg_busy = false;
}

static void runtime_web_config_task(void *arg)
{
    (void)arg;
    runtime_web_config_run();
    vTaskDelete(NULL);
}

static void *runtime_web_config_thread(void *arg)
{
    (void)arg;
    runtime_web_config_run();
    return NULL;
}

static void start_runtime_config(void)
{
    if (s_runtime_cfg_busy) {
        return;
    }

    s_runtime_cfg_busy = true;
    ESP_LOGI("btn", "Starting web config session");

    /* Prefer a DRAM stack: getaddrinfo/TLS from a PSRAM-stack thread
     * often fails with EAI_FAIL (202) on ESP32. 8 KB is enough because
     * HTTP/JSON buffers are heap-allocated. */
    BaseType_t ok = xTaskCreate(runtime_web_config_task, "web_cfg",
                                8192, NULL, 4, NULL);
    if (ok == pdPASS) {
        return;
    }

    ESP_LOGW("btn", "DRAM web_cfg task failed (free=%u largest=%u), trying PSRAM",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    pthread_t th;
    esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
    pcfg.thread_name = "web_cfg";
    pcfg.stack_size = 16384;
    pcfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    pcfg.prio = 4;
    esp_pthread_set_cfg(&pcfg);
    int pret = pthread_create(&th, NULL, runtime_web_config_thread, NULL);
    {
        esp_pthread_cfg_t def = esp_pthread_get_default_config();
        esp_pthread_set_cfg(&def);
    }
    if (pret != 0) {
        ESP_LOGE("btn", "Failed to start web config thread: %d "
                 "(DRAM free=%u largest=%u PSRAM free=%u)",
                 pret,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        s_runtime_cfg_busy = false;
        return;
    }
    pthread_detach(th);
}

static void runtime_button_task(void *arg)
{
    (void)arg;
    int held_ms = 0;
    bool notified = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (gpio_read_level(CFG_TRIGGER_GPIO) == 0) {
            held_ms += 100;
            if (!notified && held_ms >= 1000) {
                ESP_LOGI("btn", "Config button held — keep holding %d s for web config",
                         CFG_LONG_PRESS_MS / 1000);
                notified = true;
            }
            if (held_ms >= CFG_LONG_PRESS_MS) {
                ESP_LOGI("btn", "Long press confirmed");
                start_runtime_config();
                held_ms = 0;
                notified = false;
                while (gpio_read_level(CFG_TRIGGER_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        } else {
            held_ms = 0;
            notified = false;
        }
    }
}

static void start_runtime_button_monitor(void)
{
    if (s_btn_task != NULL) {
        return;
    }
    BaseType_t ok = xTaskCreate(runtime_button_task, "btn_mon", 2048, NULL, 2, &s_btn_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start config-button monitor");
        s_btn_task = NULL;
    }
}

/* ============================================================
 * NVS initialization helper
 * ============================================================ */

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated or version mismatch, erase and retry */
        ESP_LOGW(TAG, "NVS needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/* ============================================================
 * Periodic memory usage monitor (detects leaks via min watermark)
 * ============================================================ */

#define MEM_MON_INTERVAL_MS  30000

static void memory_monitor_task(void *arg)
{
    /* Allocate CPU stats buffer from PSRAM once to avoid DRAM pressure.
     * ~40 bytes per task; 32 tasks gives 1280 bytes headroom. */
    char *cpu_buf = heap_caps_malloc(1280, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cpu_buf == NULL) {
        ESP_LOGW("mem", "Failed to alloc CPU stats buffer from PSRAM, CPU stats disabled");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MEM_MON_INTERVAL_MS));

        /* ---- Memory ---- */
        uint32_t dram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t dram_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        uint32_t dram_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t psram_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI("mem", "DRAM  free=%6lu  min=%6lu  largest_block=%6lu",
                 (unsigned long)dram_free,
                 (unsigned long)dram_min,
                 (unsigned long)dram_block);
        ESP_LOGI("mem", "PSRAM free=%6lu  min=%6lu",
                 (unsigned long)psram_free,
                 (unsigned long)psram_min);

        /* ---- CPU ---- */
        if (cpu_buf != NULL) {
            vTaskGetRunTimeStats(cpu_buf);
            /* Print as a single log entry; embedded newlines produce a table */
            ESP_LOGI("mem", "CPU usage (task / abs-ticks / %%):\n%s", cpu_buf);
        }
    }
}

/* ============================================================
 * Main entry point
 * ============================================================ */

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== BLU-Qwen Gateway Starting ===");

    /* Confirm firmware is healthy — cancels rollback if this is a fresh OTA boot.
     * Must be called before anything that could trigger a reboot or watchdog. */
    ota_manager_mark_valid();

    /* Sample config button before bringing up network or classic BT. */
    config_button_gpio_init();
    bool boot_ble = check_config_trigger();

    /* Create initialization event group */
    s_init_event_group = xEventGroupCreate();
    if (s_init_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create init event group");
        return;
    }

    /* Step 0: Initialize NVS (required by config manager and WiFi) */
    ret = init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[1/7] NVS initialized");

    /* Step 1: Initialize Config Manager - load configuration from NVS */
    ret = config_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config manager init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[2/7] Config manager initialized");
    bt_whitelist_init();

    if (boot_ble) {
        enter_ble_config_mode();
        ESP_LOGE(TAG, "BLE config mode exited unexpectedly — continuing boot");
    }
    start_runtime_button_monitor();

    /* Step 2: Initialize Ethernet and start connection */
    ret = ethernet_manager_init(&s_ethernet_callbacks);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet manager init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[3/7] Ethernet manager initialized, waiting for link/IP...");
    xTaskCreate(ethernet_fallback_task, "net_fallback", 3072, NULL, 4, NULL);

    const system_config_t *cfg = config_get();

    /* Step 3: Initialize Bluetooth — HFP phone, HFP AG headset, or A2DP speaker */
    if (cfg->bt_mode == BT_MODE_A2DP) {
        const a2dp_callbacks_t *a2dp_cbs = gateway_get_a2dp_callbacks();
        ret = a2dp_mgr_init(cfg->bt_device_name, cfg->bt_audio_device_name, a2dp_cbs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "A2DP manager init failed: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "[4/7] A2DP manager initialized (target: '%s')",
                 cfg->bt_audio_device_name[0] ? cfg->bt_audio_device_name : "<any>");
    } else if (cfg->bt_mode == BT_MODE_HFP_AG) {
        const hfp_ag_callbacks_t *ag_cbs = gateway_get_hfp_ag_callbacks();
        ret = hfp_ag_init(cfg->bt_device_name, cfg->bt_audio_device_name, ag_cbs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "HFP AG init failed: %s", esp_err_to_name(ret));
            return;
        }
        hfp_ag_set_auto_reconnect(cfg->bt_auto_reconnect);
        ESP_LOGI(TAG, "[4/7] HFP AG initialized (device: %s, headset: '%s', auto_reconnect: %s)",
                 cfg->bt_device_name,
                 cfg->bt_audio_device_name[0] ? cfg->bt_audio_device_name : "<any>",
                 cfg->bt_auto_reconnect ? "on" : "off");
    } else {
        const hfp_callbacks_t *hfp_cbs = gateway_get_hfp_callbacks();
        ret = hfp_client_init(cfg->bt_device_name, hfp_cbs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "HFP client init failed: %s", esp_err_to_name(ret));
            return;
        }
        hfp_set_auto_reconnect(cfg->bt_auto_reconnect);
        pbap_sync_init();
        ESP_LOGI(TAG, "[4/7] HFP client initialized (device: %s, auto_reconnect: %s, whitelist: %s)",
                 cfg->bt_device_name,
                 cfg->bt_auto_reconnect ? "on" : "off",
                 cfg->bt_whitelist ? "on" : "off");
    }

    /* Wait for network connection before connecting to Qwen Gateway */
    wait_for_network(NETWORK_WAIT_MS);

    if (!(xEventGroupGetBits(s_init_event_group) & NETWORK_READY_BIT)) {
        ESP_LOGW(TAG, "Network still down, continuing anyway");
    } else if (web_config_is_enabled() && !config_is_registered()) {
        ESP_LOGI(TAG, "Registering device with platform");
        web_config_register();
    }

    /* Step 4-7: Gateway controller (Qwen WebSocket client + audio bridge) */
    ret = gateway_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Gateway init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[5/7] Qwen client initialized");

    if (qwen_should_connect_for_bt()) {
        ESP_LOGI(TAG, "[6/7] Bluetooth already connected — starting Qwen session");
        gateway_try_start_session("boot");
    } else if (cfg->bt_mode == BT_MODE_HFP) {
        ESP_LOGI(TAG, "[6/7] Waiting for incoming phone call before opening Qwen session");
    } else {
        ESP_LOGI(TAG, "[6/7] Waiting for Bluetooth device before opening Qwen session");
    }

    ESP_LOGI(TAG, "=== BLU-Qwen Gateway Ready ===");
    ESP_LOGI(TAG, "  Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Network: %s",
             s_active_network == NETWORK_IF_ETHERNET ? "Ethernet LAN8720" :
             s_active_network == NETWORK_IF_WIFI ? "WiFi" : "none");
    ESP_LOGI(TAG, "  Qwen: %s://%s:%u%s",
             cfg->qwen_tls ? "wss" : "ws",
             cfg->qwen_host[0] ? cfg->qwen_host : "<unset>",
             (unsigned)cfg->qwen_port,
             cfg->qwen_path[0] ? cfg->qwen_path : "/api/realtime");
    const char *bt_mode_s = "HFP";
    if (cfg->bt_mode == BT_MODE_A2DP) {
        bt_mode_s = "A2DP";
    } else if (cfg->bt_mode == BT_MODE_HFP_AG) {
        bt_mode_s = "HFP-AG";
    }
    ESP_LOGI(TAG, "  BT:   %s (mode=%s%s%s)",
             cfg->bt_device_name,
             bt_mode_s,
             (cfg->bt_mode == BT_MODE_A2DP || cfg->bt_mode == BT_MODE_HFP_AG) &&
                 cfg->bt_audio_device_name[0] ? " target=" : "",
             (cfg->bt_mode == BT_MODE_A2DP || cfg->bt_mode == BT_MODE_HFP_AG)
                 ? cfg->bt_audio_device_name : "");
    ESP_LOGI(TAG, "  Auto session: %s", cfg->qwen_auto_session ? "on" : "off");

    /* Memory monitor: print heap stats every 30 seconds to detect leaks */
//    xTaskCreate(memory_monitor_task, "mem_mon", 2048, NULL, 1, NULL);
}

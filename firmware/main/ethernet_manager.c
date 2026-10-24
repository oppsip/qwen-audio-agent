/**
 * Ethernet Manager - LAN8720 bring-up for wired network testing.
 *
 * Hardware:
 * - LAN8720 PHY address: 1
 * - SMI MDC: GPIO23
 * - SMI MDIO: GPIO18
 * - RMII REF_CLK: external 50 MHz into ESP32 GPIO0
 * - PHY reset/power: GPIO5
 * - GPIO4 high selects GPIO0 -> 50 MHz clock; low selects GPIO0 -> BOOT button
 */

#include "ethernet_manager.h"

#include <stdio.h>
#include <string.h>
#include "config_manager.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "eth_mgr";

#define ETH_CONNECTED_BIT BIT0
#define ETH_DISCONNECTED_BIT BIT1

#define ETH_PHY_ADDR 1
#define ETH_MDC_GPIO 23
#define ETH_MDIO_GPIO 18
#define ETH_PHY_RESET_GPIO 5
#define ETH_CLK_SELECT_GPIO 4
#define ETH_REF_CLK_GPIO 0

/* Lower number = preferred. Keep Ethernet below WiFi (100) until GOT_IP so a
 * leftover static IP or false PHY link cannot steal the default route. */
#define ETH_ROUTE_PRIO_ACTIVE  50
#define ETH_ROUTE_PRIO_IDLE    150

static EventGroupHandle_t s_eth_event_group;
static bool s_is_connected;
static bool s_link_up;
static bool s_has_ip;
static const ethernet_manager_callbacks_t *s_callbacks;
static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_glue;
static esp_netif_t *s_eth_netif;

static void eth_update_connected(void)
{
    bool now = s_link_up && s_has_ip;
    if (now == s_is_connected) {
        return;
    }

    s_is_connected = now;
    if (s_eth_netif) {
        esp_netif_set_route_prio(s_eth_netif,
                                 now ? ETH_ROUTE_PRIO_ACTIVE : ETH_ROUTE_PRIO_IDLE);
    }

    if (now) {
        xEventGroupSetBits(s_eth_event_group, ETH_CONNECTED_BIT);
        xEventGroupClearBits(s_eth_event_group, ETH_DISCONNECTED_BIT);
        if (s_callbacks && s_callbacks->on_connected) {
            s_callbacks->on_connected();
        }
        return;
    }

    xEventGroupSetBits(s_eth_event_group, ETH_DISCONNECTED_BIT);
    xEventGroupClearBits(s_eth_event_group, ETH_CONNECTED_BIT);
    if (s_callbacks && s_callbacks->on_disconnected) {
        s_callbacks->on_disconnected();
    }
}

static bool parse_ipv4(const char *addr, esp_ip4_addr_t *out)
{
    if (addr == NULL || addr[0] == '\0' || out == NULL) {
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

    out->addr = esp_ip4addr_aton(addr);
    return out->addr != 0;
}

static esp_err_t apply_ip_config(void)
{
    const system_config_t *cfg = config_get();
    if (cfg == NULL || cfg->eth_dhcp) {
        ESP_LOGI(TAG, "Ethernet IP mode: DHCP");
        return ESP_OK;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (!parse_ipv4(cfg->eth_ip, &ip_info.ip) ||
            !parse_ipv4(cfg->eth_netmask, &ip_info.netmask) ||
            !parse_ipv4(cfg->eth_gateway, &ip_info.gw)) {
        ESP_LOGE(TAG, "Invalid static Ethernet IP config");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_eth_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "stop Ethernet DHCP client failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_set_ip_info(s_eth_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set static Ethernet IP failed: %s", esp_err_to_name(err));
        return err;
    }

    if (cfg->eth_dns[0] != '\0') {
        esp_netif_dns_info_t dns = {0};
        if (!parse_ipv4(cfg->eth_dns, &dns.ip.u_addr.ip4)) {
            ESP_LOGE(TAG, "Invalid Ethernet DNS config");
            return ESP_ERR_INVALID_ARG;
        }
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        err = esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set Ethernet DNS failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "Ethernet IP mode: static ip=%s netmask=%s gateway=%s dns=%s",
             cfg->eth_ip, cfg->eth_netmask, cfg->eth_gateway,
             cfg->eth_dns[0] ? cfg->eth_dns : "<none>");
    return ESP_OK;
}

static void select_rmii_clock_input(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(ETH_CLK_SELECT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(ETH_CLK_SELECT_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "GPIO%d=1, GPIO%d switched to external 50 MHz RMII clock",
             ETH_CLK_SELECT_GPIO, ETH_REF_CLK_GPIO);
}

static void release_phy_reset_or_power(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(ETH_PHY_RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(ETH_PHY_RESET_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "GPIO%d=1, LAN8720 reset/power released", ETH_PHY_RESET_GPIO);
}

static esp_err_t create_eth_driver(esp_eth_handle_t *eth_handle_out)
{
    ESP_RETURN_ON_FALSE(eth_handle_out != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "eth_handle_out is NULL");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = ETH_REF_CLK_GPIO;

    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RESET_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_FAIL, TAG, "create ESP32 EMAC failed");

    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "create LAN8720 PHY failed");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_config, eth_handle_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install Ethernet driver failed: %s", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        return err;
    }

    return ESP_OK;
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    uint8_t mac_addr[6] = {0};

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        s_link_up = true;
        if (s_callbacks && s_callbacks->on_link_up) {
            s_callbacks->on_link_up();
        }
        eth_update_connected();
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        ESP_LOGW(TAG, "Ethernet Link Down");
        eth_update_connected();
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

static void eth_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        const esp_netif_ip_info_t *ip_info = &event->ip_info;

        s_has_ip = true;
        ESP_LOGI(TAG, "Ethernet Got IP Address");
        ESP_LOGI(TAG, "ETHIP:   " IPSTR, IP2STR(&ip_info->ip));
        ESP_LOGI(TAG, "ETHMASK: " IPSTR, IP2STR(&ip_info->netmask));
        ESP_LOGI(TAG, "ETHGW:   " IPSTR, IP2STR(&ip_info->gw));
        if (!s_link_up) {
            ESP_LOGW(TAG, "Ethernet has IP but link is down — not using as default route");
        }
        eth_update_connected();
        return;
    }

    if (event_id == IP_EVENT_ETH_LOST_IP) {
        s_has_ip = false;
        ESP_LOGW(TAG, "Ethernet lost IP");
        eth_update_connected();
    }
}

esp_err_t ethernet_manager_init(const ethernet_manager_callbacks_t *callbacks)
{
    s_callbacks = callbacks;

    s_eth_event_group = xEventGroupCreate();
    if (s_eth_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    select_rmii_clock_input();
    release_phy_reset_or_power();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "create default event loop failed: %s", esp_err_to_name(loop_err));
        return loop_err;
    }

    esp_err_t err = create_eth_driver(&s_eth_handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_eth_netif != NULL, ESP_FAIL, TAG, "create ETH netif failed");

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(s_eth_glue != NULL, ESP_FAIL, TAG, "create ETH netif glue failed");

    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, s_eth_glue));
    esp_netif_set_route_prio(s_eth_netif, ETH_ROUTE_PRIO_IDLE);
    ESP_RETURN_ON_ERROR(apply_ip_config(), TAG, "apply Ethernet IP config failed");
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               eth_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                               eth_ip_event_handler, NULL));

    ESP_LOGI(TAG, "Starting LAN8720: phy_addr=%d mdc=%d mdio=%d reset=%d ref_clk_gpio=%d",
             ETH_PHY_ADDR, ETH_MDC_GPIO, ETH_MDIO_GPIO,
             ETH_PHY_RESET_GPIO, ETH_REF_CLK_GPIO);

    return esp_eth_start(s_eth_handle);
}

bool ethernet_manager_is_connected(void)
{
    return s_is_connected;
}

esp_netif_t *ethernet_manager_netif(void)
{
    return s_eth_netif;
}

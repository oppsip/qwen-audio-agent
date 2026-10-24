#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WiFi 连接状态回调
 * 当 WiFi 连接状态变化时调用
 */
typedef struct {
    void (*on_connected)(void);     // WiFi 连接成功（已获取 IP）
    void (*on_disconnected)(void);  // WiFi 连接断开
} wifi_manager_callbacks_t;

/**
 * 初始化 WiFi 管理器并启动 STA 模式连接
 *
 * 从 Config Manager 获取 WiFi SSID 和密码，
 * 初始化 WiFi 驱动并开始连接。
 *
 * @param callbacks WiFi 事件回调（可为 NULL）
 * @return ESP_OK 成功启动连接流程
 */
esp_err_t wifi_manager_init(const wifi_manager_callbacks_t *callbacks);

esp_err_t wifi_manager_start(void);

esp_err_t wifi_manager_stop(void);

/**
 * 获取当前 WiFi 连接状态
 * @return true 如果已连接并获取 IP
 */
bool wifi_manager_is_connected(void);

esp_netif_t *wifi_manager_netif(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */

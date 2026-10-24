#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 蓝牙音频模式
 */
typedef enum {
    BT_MODE_HFP    = 0,  /**< HFP Client — 连接手机，来电转 Qwen          */
    BT_MODE_A2DP   = 1,  /**< A2DP Source — 连接蓝牙音箱，单向播音        */
    BT_MODE_HFP_AG = 2,  /**< HFP Audio Gateway — 连接蓝牙耳机，与 Qwen 互通 */
} bt_audio_mode_t;

/**
 * 系统配置结构
 */
typedef struct {
    // WiFi 参数
    char wifi_ssid[33];
    char wifi_password[65];
    bool eth_dhcp;
    char eth_ip[16];
    char eth_netmask[16];
    char eth_gateway[16];
    char eth_dns[16];

    // Qwen Audio Agent（本机 device-relay 或 --lan Gateway）
    char     qwen_host[64];
    uint16_t qwen_port;
    char     qwen_path[64];
    char     qwen_token[96];
    bool     qwen_tls;
    bool     qwen_auto_session;

    // 蓝牙参数
    char            bt_device_name[32];       /**< ESP32 自身广播名称              */
    bt_audio_mode_t bt_mode;                  /**< HFP Client / A2DP / HFP AG */
    char            bt_audio_device_name[32]; /**< A2DP 音箱 / HFP AG 耳机名称（空=任意/等待主动连） */
    bool            bt_auto_reconnect;        /**< true=自动回连上次成功配对的设备 */
    bool            bt_whitelist;             /**< HFP Client：通讯录白名单来电不转 Qwen、语音不走蓝牙 */
} system_config_t;

/**
 * 初始化配置管理器，从 NVS 加载配置
 */
esp_err_t config_manager_init(void);

/**
 * 获取当前配置（只读）
 */
const system_config_t* config_get(void);

/**
 * 设置 WiFi 参数并持久化
 */
esp_err_t config_set_wifi(const char *ssid, const char *password);

esp_err_t config_set_ethernet(bool dhcp, const char *ip, const char *netmask,
                              const char *gateway, const char *dns);

/**
 * 设置 Qwen Audio Agent 接入参数并持久化
 * @param host  device-relay / Gateway 主机名或 IPv4
 * @param port  WebSocket 端口（device-relay 默认 3101）
 * @param path  WebSocket 路径，空则 /api/realtime
 * @param token 设备访问令牌（与本机 relay 的 DEVICE_ACCESS_TOKEN 相同）
 * @param tls   true=wss，false=ws（受信局域网用 ws）
 */
esp_err_t config_set_qwen(const char *host, uint16_t port, const char *path,
                          const char *token, bool tls);

esp_err_t config_set_qwen_auto_session(bool enable);

/**
 * 设置蓝牙设备名称（ESP32 广播名）并持久化
 */
esp_err_t config_set_bluetooth(const char *device_name);

/**
 * 设置蓝牙音频模式及目标音频设备名称并持久化
 * @param mode              BT_MODE_HFP / BT_MODE_A2DP / BT_MODE_HFP_AG
 * @param audio_device_name A2DP 目标音箱或 HFP AG 目标耳机名（空 = 不指定）
 */
esp_err_t config_set_bt_mode(bt_audio_mode_t mode, const char *audio_device_name);

/**
 * 设置是否自动回连上次成功配对的蓝牙设备并持久化
 */
esp_err_t config_set_bt_auto_reconnect(bool enable);

/**
 * 设置是否启用 HFP 来电白名单并持久化。默认关闭。
 * 仅 HFP Client 连接手机时生效。
 */
esp_err_t config_set_bt_whitelist(bool enable);

/**
 * 恢复出厂用户配置：擦除 Wi-Fi / 以太网 / Qwen / 蓝牙 / 首次配置标志，
 * 以及 HFP 上次配对地址、通讯录白名单和 Classic bond。保留平台注册身份（device 命名空间）。
 * 内存中的配置回到 menuconfig 出厂初值。调用方应在适当时机重启。
 */
esp_err_t config_reset_defaults(void);

/**
 * 若上次出厂复位时 Classic 蓝牙未就绪，在 Bluedroid 起来后补清 bond。
 * 在 HFP / A2DP / AG 初始化完成 Bluedroid enable 之后调用。
 */
void config_clear_bt_bonds_if_pending(void);

/**
 * true if this unit has never completed BLE/Web config (no NVS "initialized").
 */
bool config_is_first_boot(void);

/**
 * Call after BLE or Web config is saved.
 */
void config_mark_configured(void);

/**
 * Factory MAC as 12 uppercase hex chars (no separators), e.g. "A4CF12B3C4D5".
 * Always derived from eFuse; not stored in NVS.
 */
#define CONFIG_DEVICE_ID_LEN 13  /* 12 hex + NUL */

void config_get_device_id(char *out, size_t out_sz);

/**
 * true after a successful platform registration (device password saved in NVS)
 */
bool config_is_registered(void);

/**
 * Persist platform-issued device password and mark this unit registered.
 * Factory reset keeps this identity so the device does not re-register.
 */
esp_err_t config_mark_registered(const char *device_password);

/**
 * Device password issued by the platform. Empty string if not yet registered.
 */
const char *config_get_device_password(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MANAGER_H */

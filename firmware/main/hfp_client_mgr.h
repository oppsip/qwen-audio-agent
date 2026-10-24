#ifndef HFP_CLIENT_MGR_H
#define HFP_CLIENT_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_codec.h"  // for sco_codec_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HFP 连接状态
 */
typedef enum {
    HFP_STATE_DISCONNECTED,
    HFP_STATE_CONNECTING,
    HFP_STATE_CONNECTED,
    HFP_STATE_SLC_CONNECTED,  // Service Level Connection 已建立
} hfp_conn_state_t;

/**
 * HFP 事件回调
 */
typedef struct {
    void (*on_connection_state)(hfp_conn_state_t state);
    void (*on_incoming_call)(const char *number);
    void (*on_call_answered)(void);
    void (*on_call_ended)(void);
    void (*on_outgoing_call_connected)(void);
    void (*on_outgoing_call_failed)(void);
    void (*on_audio_data)(const uint8_t *data, uint32_t len);
    void (*on_audio_codec_changed)(sco_codec_t codec);
} hfp_callbacks_t;

/**
 * 初始化 HFP 客户端
 * @param device_name 蓝牙设备名称
 * @param callbacks 事件回调
 */
esp_err_t hfp_client_init(const char *device_name, const hfp_callbacks_t *callbacks);

/**
 * 接听来电
 */
esp_err_t hfp_answer_call(void);

/**
 * 挂断/拒绝电话
 */
esp_err_t hfp_hangup(void);

/**
 * 拨出电话
 * @param number 电话号码
 */
esp_err_t hfp_dial(const char *number);

/**
 * 将 PCM 数据推入 SCO 发送环形缓冲区（RTP→BT 方向）
 * @param data PCM 数据（8kHz CVSD 或 16kHz mSBC，由当前 SCO 编码决定）
 * @param len  数据长度（字节）
 */
void hfp_push_audio(const uint8_t *data, uint32_t len);

/**
 * 获取当前连接状态
 */
hfp_conn_state_t hfp_get_conn_state(void);

/** True while the phone reports an incoming call that can be answered. */
bool hfp_incoming_call_pending(void);

/** True after the phone reports a call in progress. */
bool hfp_call_is_active(void);

/**
 * 获取当前 SCO 编码类型
 * @return 当前协商的 SCO 编码 (CVSD 或 mSBC)
 */
sco_codec_t hfp_get_sco_codec(void);

/**
 * 当前 SCO 发送环中已缓冲的播放时长（毫秒）
 * @return 缓冲毫秒数；SCO 未建立时返回 -1
 */
int hfp_sco_out_level_ms(void);
void hfp_sco_tx_stats(uint32_t *calls, uint32_t *pad, uint32_t *trims);

/**
 * 设置是否在断开后自动回连上次配对的设备
 * @param enable true=启用自动回连（默认），false=禁用
 */
void hfp_set_auto_reconnect(bool enable);

/**
 * When true, incoming SCO is dropped so a locally-answered whitelist
 * call stays on the phone speaker/mic instead of Bluetooth.
 */
void hfp_hold_audio(bool hold);

/** Copy the current HFP peer BD_ADDR. Returns false if none. */
bool hfp_get_peer_bda(uint8_t bda[6]);

#ifdef __cplusplus
}
#endif

#endif /* HFP_CLIENT_MGR_H */

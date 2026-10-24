#ifndef A2DP_MGR_H
#define A2DP_MGR_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    A2DP_STATE_DISCONNECTED = 0,
    A2DP_STATE_SCANNING,
    A2DP_STATE_CONNECTING,
    A2DP_STATE_CONNECTED,
} a2dp_conn_state_t;

typedef struct {
    void (*on_connection_state)(a2dp_conn_state_t state);
} a2dp_callbacks_t;

/**
 * 初始化 A2DP Source 管理器
 * @param own_device_name    ESP32 广播的蓝牙设备名
 * @param target_device_name 目标音箱名称（空字符串 = 连接第一个找到的 A2DP 设备）
 * @param callbacks          连接状态回调
 */
esp_err_t a2dp_mgr_init(const char *own_device_name,
                         const char *target_device_name,
                         const a2dp_callbacks_t *callbacks);

/**
 * 将 8 kHz 单声道 PCM 推入 A2DP 发送缓冲区（内部升采样至 44100 Hz 立体声）
 * @param pcm_8k_mono  8 kHz mono int16 样本
 * @param sample_count 样本数量（≤ 240）
 */
void a2dp_push_audio(const int16_t *pcm_8k_mono, size_t sample_count);

/**
 * 获取当前 A2DP 连接状态
 */
a2dp_conn_state_t a2dp_get_conn_state(void);

#ifdef __cplusplus
}
#endif

#endif /* A2DP_MGR_H */

#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 编解码类型
 */
typedef enum {
    CODEC_PCMU = 0,   // G.711 μ-law, payload type 0
    CODEC_PCMA = 8,   // G.711 A-law, payload type 8
} codec_type_t;

/**
 * SCO 编码类型（蓝牙侧）
 */
typedef enum {
    SCO_CODEC_CVSD = 0,   // 窄带 8kHz
    SCO_CODEC_MSBC = 1,   // 宽带 16kHz
} sco_codec_t;

/**
 * 将 16-bit PCM 线性样本编码为 G.711 A-law
 * @param pcm_sample 16-bit 线性 PCM 样本 (int16_t)
 * @return 8-bit A-law 编码值
 */
uint8_t g711_alaw_encode(int16_t pcm_sample);

/**
 * 将 G.711 A-law 编码值解码为 16-bit PCM 线性样本
 * @param alaw_sample 8-bit A-law 编码值
 * @return 16-bit 线性 PCM 样本
 */
int16_t g711_alaw_decode(uint8_t alaw_sample);

/**
 * 将 16-bit PCM 线性样本编码为 G.711 μ-law
 * @param pcm_sample 16-bit 线性 PCM 样本 (int16_t)
 * @return 8-bit μ-law 编码值
 */
uint8_t g711_ulaw_encode(int16_t pcm_sample);

/**
 * 将 G.711 μ-law 编码值解码为 16-bit PCM 线性样本
 * @param ulaw_sample 8-bit μ-law 编码值
 * @return 16-bit 线性 PCM 样本
 */
int16_t g711_ulaw_decode(uint8_t ulaw_sample);

/**
 * 批量编码 PCM 数据
 * @param codec_type 编码类型 (PCMU 或 PCMA)
 * @param pcm_in 输入 PCM 数据 (int16_t 数组, 8kHz)
 * @param g711_out 输出 G.711 数据 (uint8_t 数组)
 * @param sample_count 样本数量
 */
void codec_encode(codec_type_t codec_type, const int16_t *pcm_in,
                  uint8_t *g711_out, size_t sample_count);

/**
 * 批量解码 G.711 数据
 * @param codec_type 编码类型 (PCMU 或 PCMA)
 * @param g711_in 输入 G.711 数据 (uint8_t 数组)
 * @param pcm_out 输出 PCM 数据 (int16_t 数组, 8kHz)
 * @param sample_count 样本数量
 */
void codec_decode(codec_type_t codec_type, const uint8_t *g711_in,
                  int16_t *pcm_out, size_t sample_count);

/**
 * 降采样：16kHz PCM → 8kHz PCM（含抗混叠低通滤波）
 * @param in_16k 输入 16kHz PCM 数据 (int16_t 数组)
 * @param out_8k 输出 8kHz PCM 数据 (int16_t 数组)
 * @param in_sample_count 输入样本数量（必须为偶数）
 * @return 输出样本数量 (in_sample_count / 2)
 */
size_t resample_downsample(const int16_t *in_16k, int16_t *out_8k, size_t in_sample_count);

/**
 * 升采样：8kHz PCM → 16kHz PCM（含抗镜像低通滤波）
 * @param in_8k 输入 8kHz PCM 数据 (int16_t 数组)
 * @param out_16k 输出 16kHz PCM 数据 (int16_t 数组)
 * @param in_sample_count 输入样本数量
 * @return 输出样本数量 (in_sample_count * 2)
 */
size_t resample_upsample(const int16_t *in_8k, int16_t *out_16k, size_t in_sample_count);

/**
 * 重置重采样器历史缓冲区（在每次通话开始时调用）
 * 防止上一次通话的尾部样本污染新通话开头
 */
void codec_resampler_reset(void);

/**
 * Linear interpolation between arbitrary positive sample rates.
 * Used for 8/16 kHz SCO ↔ 16/24 kHz Gateway PCM.
 */
size_t resample_linear(const int16_t *in, size_t in_count, int in_rate,
                       int16_t *out, size_t out_max, int out_rate);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CODEC_H */

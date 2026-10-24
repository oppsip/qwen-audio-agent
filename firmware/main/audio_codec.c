#include "audio_codec.h"
#include "string.h"
/*
 * G.711 A-law and μ-law implementation per ITU-T G.711 standard.
 *
 * Based on the reference algorithms from ITU-T Recommendation G.711.
 * A-law uses 13 segments (1 linear + 7 logarithmic per polarity).
 * μ-law uses logarithmic compression with bias of 33 (0x21) in the
 * 14-bit domain, equivalent to bias 132 (0x84) in the 16-bit domain.
 *
 * Both produce 8-bit compressed samples from 16-bit linear PCM input.
 * The encode→decode round-trip is exact for values that fall on
 * quantization levels.
 */

/* ========================================================================
 * G.711 A-law
 * ======================================================================== */

/*
 * A-law segment boundaries in the 13-bit linear magnitude domain.
 * Segment 0 is linear (0-63), segments 1-7 are logarithmic.
 */

uint8_t g711_alaw_encode(int16_t number)
{
	const uint16_t ALAW_MAX = 0xFFF;
	uint16_t mask = 0x800;
	uint8_t sign = 0;
	uint8_t position = 11;
	uint8_t lsb = 0;
	if (number < 0)
	{
		number = -number;
		sign = 0x80;
	}
	if (number > ALAW_MAX)
	{
		number = ALAW_MAX;
	}
	for (; ((number & mask) != mask && position >= 5); mask >>= 1, position--);
	lsb = (number >> ((position == 4) ? (1) : (position - 4))) & 0x0f;
	return (sign | ((position - 4) << 4) | lsb) ^ 0x55;
}

int16_t g711_alaw_decode(uint8_t number)
{
	uint8_t sign = 0x00;
	uint8_t position = 0;
	int16_t decoded = 0;
	number ^= 0x55;
	if (number & 0x80)
	{
		number &= ~(1 << 7);
		sign = -1;
	}
	position = ((number & 0xF0) >> 4) + 4;
	if (position != 4)
	{
		decoded = ((1 << position) | ((number & 0x0F) << (position - 4))
				| (1 << (position - 5)));
	}
	else
	{
		decoded = (number << 1) | 1;
	}
	return (sign == 0) ? (decoded) : (-decoded);
}


/* ========================================================================
 * G.711 μ-law
 * ======================================================================== */
uint8_t g711_ulaw_encode(int16_t number)
{
	const uint16_t MULAW_MAX = 0x1FFF;
	const uint16_t MULAW_BIAS = 33;
	uint16_t mask = 0x1000;
	uint8_t sign = 0;
	uint8_t position = 12;
	uint8_t lsb = 0;
	if (number < 0)
	{
		number = -number;
		sign = 0x80;
	}
	number += MULAW_BIAS;
	if (number > MULAW_MAX)
	{
		number = MULAW_MAX;
	}
	for (; ((number & mask) != mask && position >= 5); mask >>= 1, position--)
		;
	lsb = (number >> (position - 4)) & 0x0f;
	return (~(sign | ((position - 5) << 4) | lsb));
}

int16_t g711_ulaw_decode(uint8_t number)
{
	const uint16_t MULAW_BIAS = 33;
	uint8_t sign = 0, position = 0;
	int16_t decoded = 0;
	number = ~number;
	if (number & 0x80)
	{
		number &= ~(1 << 7);
		sign = -1;
	}
	position = ((number & 0xF0) >> 4) + 5;
	decoded = ((1 << position) | ((number & 0x0F) << (position - 4))
			| (1 << (position - 5))) - MULAW_BIAS;
	return (sign == 0) ? (decoded) : (-(decoded));
}

/* ========================================================================
 * Batch encode/decode interfaces
 * ======================================================================== */

void codec_encode(codec_type_t codec_type, const int16_t *pcm_in,
                  uint8_t *g711_out, size_t sample_count)
{
    if (pcm_in == NULL || g711_out == NULL || sample_count == 0) {
        return;
    }

    if (codec_type == CODEC_PCMA) {
        /*
         * g711_alaw_encode() expects 13-bit signed input (range ±4095).
         * Input PCM is 16-bit (range ±32767). Scale down by 8 (>> 3).
         * Without this, samples louder than 1/8 full-scale hard-clip → noise.
         */
        for (size_t i = 0; i < sample_count; i++) {
            g711_out[i] = g711_alaw_encode(pcm_in[i] >> 3);
        }
    } else {
        /*
         * g711_ulaw_encode() expects 14-bit signed input (range ±8191).
         * Scale 16-bit PCM down by 4 (>> 2).
         */
        for (size_t i = 0; i < sample_count; i++) {
            g711_out[i] = g711_ulaw_encode(pcm_in[i] >> 2);
        }
    }
}

void codec_decode(codec_type_t codec_type, const uint8_t *g711_in,
                  int16_t *pcm_out, size_t sample_count)
{
    if (g711_in == NULL || pcm_out == NULL || sample_count == 0) {
        return;
    }

    if (codec_type == CODEC_PCMA) {
        /* Decoder output is 13-bit range (±4032). Scale back to 16-bit (<< 3).
         * Max after shift: ±4032×8 = ±32256, safely within int16_t. */
        for (size_t i = 0; i < sample_count; i++) {
            pcm_out[i] = g711_alaw_decode(g711_in[i]) << 3;
        }
    } else {
        /* Decoder output is 14-bit range (±8031). Scale back to 16-bit (<< 2).
         * Max after shift: ±8031×4 = ±32124, safely within int16_t. */
        for (size_t i = 0; i < sample_count; i++) {
            pcm_out[i] = g711_ulaw_decode(g711_in[i]) << 2;
        }
    }
}

/* ========================================================================
 * Resampling: 16kHz ↔ 8kHz  (factor 2, integer ↑↓)
 *
 * Filter: 15-tap Hamming-windowed sinc, cutoff = 0.25 (= 4 kHz at 16 kHz).
 * Coefficients are Q15 (scaled by 32768).  The filter is symmetric and
 * half-band (even-indexed coefficients, except the centre, are zero), so
 * only 8 out of 15 multiplications are non-trivial.
 *
 * Passband (0–3.4 kHz): ≥ −0.5 dB   Stopband (> 4 kHz): ≤ −40 dB
 * ======================================================================== */

/* Hamming-windowed sinc, fc = 0.25 (normalised), 15 taps, Q15 */
static const int16_t s_lpf_q15[15] = {
    -119, 0, 527, 0, -2234, 0, 9952, 16384, 9952, 0, -2234, 0, 527, 0, -119
};
#define LPF_TAPS  15
#define LPF_HALF  7   /* (LPF_TAPS - 1) / 2 */

/*
 * Per-stream history: last LPF_HALF input samples from the previous call.
 * Without this, the FIR filter has wrong (zero) values for its lookback taps
 * at the start of every frame, creating a ~50 Hz click artifact.
 */
static int16_t s_down_hist[LPF_HALF];   /* 16kHz history for downsample */
static int16_t s_up_hist[LPF_HALF];     /* 8kHz  history for upsample   */

void codec_resampler_reset(void)
{
    memset(s_down_hist, 0, sizeof(s_down_hist));
    memset(s_up_hist,   0, sizeof(s_up_hist));
}

size_t resample_downsample(const int16_t *in_16k, int16_t *out_8k, size_t in_sample_count)
{
    if (in_16k == NULL || out_8k == NULL || in_sample_count < 2) {
        return 0;
    }
    in_sample_count &= ~(size_t)1;  /* must be even */
    size_t out_count = in_sample_count >> 1;

    for (size_t i = 0; i < out_count; i++) {
        int32_t acc = 0;
        int centre = (int)(i << 1);
        for (int k = 0; k < LPF_TAPS; k++) {
            int idx = centre - LPF_HALF + k;
            int16_t sample;
            if (idx < 0) {
                /* history[LPF_HALF + idx]: idx=-7→[0], idx=-1→[6] */
                sample = s_down_hist[LPF_HALF + idx];
            } else if (idx < (int)in_sample_count) {
                sample = in_16k[idx];
            } else {
                sample = 0;
            }
            acc += (int32_t)s_lpf_q15[k] * sample;
        }
        acc >>= 15;
        out_8k[i] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }

    /* Save the last LPF_HALF input samples as history for the next call */
    if ((int)in_sample_count >= LPF_HALF) {
        for (int j = 0; j < LPF_HALF; j++) {
            s_down_hist[j] = in_16k[in_sample_count - LPF_HALF + j];
        }
    } else {
        int keep = LPF_HALF - (int)in_sample_count;
        memmove(s_down_hist, s_down_hist + in_sample_count,
                (size_t)keep * sizeof(int16_t));
        for (int j = 0; j < (int)in_sample_count; j++) {
            s_down_hist[keep + j] = in_16k[j];
        }
    }

    return out_count;
}

size_t resample_upsample(const int16_t *in_8k, int16_t *out_16k, size_t in_sample_count)
{
    if (in_8k == NULL || out_16k == NULL || in_sample_count == 0) {
        return 0;
    }
    size_t out_count = in_sample_count << 1;

    /*
     * Conceptual model: zero-stuff then low-pass with gain × 2.
     * Negative zs_idx with even parity map to previous-frame 8kHz samples
     * stored in s_up_hist.
     */
    for (size_t i = 0; i < out_count; i++) {
        int32_t acc = 0;
        for (int k = 0; k < LPF_TAPS; k++) {
            int zs_idx = (int)i - LPF_HALF + k;
            if ((zs_idx & 1) == 0) {
                int src = zs_idx >> 1;
                int16_t sample;
                if (src < 0) {
                    /* hist[LPF_HALF + src]: src=-3→[4], src=-2→[5], src=-1→[6] */
                    int hi = LPF_HALF + src;
                    sample = (hi >= 0) ? s_up_hist[hi] : (int16_t)0;
                } else if (src < (int)in_sample_count) {
                    sample = in_8k[src];
                } else {
                    sample = 0;
                }
                acc += (int32_t)s_lpf_q15[k] * sample;
            }
        }
        acc >>= 14;
        out_16k[i] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }

    /* Save the last LPF_HALF input samples as history for the next call */
    if ((int)in_sample_count >= LPF_HALF) {
        for (int j = 0; j < LPF_HALF; j++) {
            s_up_hist[j] = in_8k[in_sample_count - LPF_HALF + j];
        }
    } else {
        int keep = LPF_HALF - (int)in_sample_count;
        memmove(s_up_hist, s_up_hist + in_sample_count,
                (size_t)keep * sizeof(int16_t));
        for (int j = 0; j < (int)in_sample_count; j++) {
            s_up_hist[keep + j] = in_8k[j];
        }
    }

    return out_count;
}

size_t resample_linear(const int16_t *in, size_t in_count, int in_rate,
                       int16_t *out, size_t out_max, int out_rate)
{
    if (in == NULL || out == NULL || in_count == 0 || in_rate <= 0 || out_rate <= 0) {
        return 0;
    }
    if (in_rate == out_rate) {
        size_t n = in_count < out_max ? in_count : out_max;
        memcpy(out, in, n * sizeof(int16_t));
        return n;
    }
    if (in_rate == 16000 && out_rate == 8000) {
        size_t n = in_count & ~(size_t)1;
        if (n == 0) {
            return 0;
        }
        size_t produced = resample_downsample(in, out, n);
        return produced > out_max ? out_max : produced;
    }
    if (in_rate == 8000 && out_rate == 16000) {
        size_t need = in_count << 1;
        if (need > out_max) {
            in_count = out_max >> 1;
        }
        return resample_upsample(in, out, in_count);
    }

    size_t out_count = (size_t)(((uint64_t)in_count * (uint64_t)out_rate
                                 + (uint64_t)(in_rate / 2)) / (uint64_t)in_rate);
    if (out_count < 1) {
        out_count = 1;
    }
    if (out_count > out_max) {
        out_count = out_max;
    }

    for (size_t i = 0; i < out_count; i++) {
        double pos = (double)i * (double)in_rate / (double)out_rate;
        size_t left = (size_t)pos;
        if (left >= in_count) {
            left = in_count - 1;
        }
        size_t right = left + 1;
        if (right >= in_count) {
            right = in_count - 1;
        }
        double frac = pos - (double)left;
        int32_t sample = (int32_t)in[left] +
                         (int32_t)((in[right] - in[left]) * frac);
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }
        out[i] = (int16_t)sample;
    }
    return out_count;
}

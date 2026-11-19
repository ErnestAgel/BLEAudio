/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <arm_math.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "audio_dsp.h"
#include "ble_audio_types.h"

LOG_MODULE_REGISTER(audio_dsp, LOG_LEVEL_INF);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DSP_INPUT_GAIN       6.0f    /* +15.5 dB equal preamp for both Bypass and NR ON */
#define DSP_PCM_SCALE_INV    (DSP_INPUT_GAIN / 32768.0f)
#define DSP_PCM_SCALE        32768.0f
#define DSP_NLMS_EPS         1e-5f   /* Regularization in normalized float domain */
#define DSP_NLMS_MU_NOISE    0.03f   /* Gentle noise-only adaptation (frozen on speech) */
#define DSP_NLMS_LEAK_NOISE  0.9995f
#define DSP_NLMS_WEIGHT_MAX  0.20f   /* Tight weight clamp prevents speech comb notches */
#define DSP_NOISE_FLOOR_MIN  1e-6f   /* -60 dBFS floor clamp */
#define DSP_GAIN_CAL_DEFAULT 1.0f
#define DSP_GAIN_CAL_MIN     0.7f    /* -3 dB MP34DT05 sensitivity bound */
#define DSP_GAIN_CAL_MAX     1.4f    /* +3 dB MP34DT05 sensitivity bound */

/* MCRA-2 + OM-LSA parameters (512-pt / hop=160, 68.75% overlap, 31.25 Hz/bin) */
#define DSP_MCRA_SUB_HOPS    25U     /* 25 * 10 ms = 250 ms/subwindow (4 * 250 ms = 1.0 s) */
#define DSP_MCRA_ALPHA_S     0.75f
#define DSP_MCRA_DELTA_MIN   2.2f
#define DSP_MCRA_BIAS_COMP   1.35f
#define DSP_MCRA_ALPHA_D     0.85f
#define DSP_STFT_NF_MULT     1.15f
#define DSP_STFT_DD_ALPHA    0.92f
#define DSP_OMLSA_P_PRIOR    0.40f
#define DSP_OMLSA_G_MIN      0.14f
#define DSP_VOICE_BIN_LO     8U      /* 250 Hz @ 512-pt / 16 kHz */
#define DSP_VOICE_BIN_HI     112U    /* 3500 Hz @ 512-pt / 16 kHz */

struct biquad_coeffs {
	float b0;
	float b1;
	float b2;
	float a1;
	float a2;
};

/* 2nd-order Butterworth HPF @ 150 Hz (fs = 16 kHz, -12 dB/oct wind/rumble cut) */
static const struct biquad_coeffs hpf_150_cfg = {
	.b0 = 0.959203f, .b1 = -1.918406f, .b2 = 0.959203f,
	.a1 = -1.916741f, .a2 = 0.920071f,
};

static atomic_t nr_enabled_flag = ATOMIC_INIT(0);
static audio_dsp_ctx_t ctx;

/* Precomputed WOLA windows, high-band DD table, Log-MMSE LUT, and 512-pt RFFT */
#define DSP_E1_LUT_N   128U
#define DSP_E1_LUT_MAX 4.0f
static float stft_wa[DSP_STFT_N];
static float stft_ws[DSP_STFT_N];
static float alpha_dd_bin[DSP_STFT_BINS];
static float exp_half_e1_lut[DSP_E1_LUT_N];
static arm_rfft_fast_instance_f32 rfft_inst;

/* Static scratch buffers (serialized in audio_dsp_task; keeps thread stack < 512 B) */
static float xl_buf[AUDIO_FRAME_SAMPLES];
static float xr_buf[AUDIO_FRAME_SAMPLES];
static float gsc_buf[AUDIO_FRAME_SAMPLES];
static float nr_buf[AUDIO_FRAME_SAMPLES];
static float rfft_in[DSP_STFT_N];
static float rfft_out[DSP_STFT_N];
static float pwr_x[DSP_STFT_BINS];
static float s_freq[DSP_STFT_BINS];
static float gain_raw[DSP_STFT_BINS];

static inline float clampf(float x, float lo, float hi)
{
	if (x < lo) {
		return lo;
	}
	if (x > hi) {
		return hi;
	}
	return x;
}

static inline float biquad_df2t(const struct biquad_coeffs *c, float x, float *z1, float *z2)
{
	float y = c->b0 * x + *z1;

	*z1 = c->b1 * x - c->a1 * y + *z2;
	*z2 = c->b2 * x - c->a2 * y;
	return y;
}

static inline int16_t float_to_int16(float sample)
{
	float scaled = sample * DSP_PCM_SCALE;

	if (!isfinite(scaled)) {
		return 0;
	}
	if (scaled >= 32767.0f) {
		return INT16_MAX;
	}
	if (scaled <= -32768.0f) {
		return INT16_MIN;
	}
	return (int16_t)(scaled >= 0.0f ? (scaled + 0.5f) : (scaled - 0.5f));
}

/* Fast IEEE-754 log2/exp2/exp (~0.003 log2 / 0.015% exp2 err, zero libm calls) */
static inline float fast_log2f(float x)
{
	uint32_t u;

	if (x < 1e-12f) {
		x = 1e-12f;
	}
	memcpy(&u, &x, sizeof(u));
	float e = (float)((int32_t)(u >> 23) - 127);

	u = (u & 0x007FFFFFU) | 0x3F800000U;
	float m;

	memcpy(&m, &u, sizeof(m));
	m -= 1.0f;
	return e + m * (1.4292f + m * (-0.6197f + m * 0.1905f));
}

static inline float fast_exp2f(float x)
{
	x = clampf(x, -40.0f, 40.0f);
	float fl = floorf(x);
	float f = x - fl;
	float p = 1.0f + f * (0.6951f + f * (0.2268f + f * 0.0781f));
	uint32_t u = (uint32_t)((int32_t)fl + 127) << 23;
	float scale;

	memcpy(&scale, &u, sizeof(scale));
	return scale * p;
}

static inline float fast_expf(float x)
{
	return fast_exp2f(x * 1.44269504f);
}

static inline float lut_exp_half_e1(float v)
{
	if (v >= DSP_E1_LUT_MAX) {
		return 1.0f;
	}
	if (v < 1e-4f) {
		v = 1e-4f;
	}
	float idx_f = v * ((float)(DSP_E1_LUT_N - 1U) / DSP_E1_LUT_MAX);
	uint32_t i0 = (uint32_t)idx_f;

	if (i0 >= (DSP_E1_LUT_N - 1U)) {
		return exp_half_e1_lut[DSP_E1_LUT_N - 1U];
	}
	float frac = idx_f - (float)i0;

	return exp_half_e1_lut[i0] * (1.0f - frac) + exp_half_e1_lut[i0 + 1U] * frac;
}

/* Abramowitz & Stegun 5.1.53 / 5.1.54 E1(v), used at init to build LUT */
static float exact_exp1f(float v)
{
	v = clampf(v, 1e-4f, 25.0f);
	if (v <= 1.0f) {
		return -logf(v) - 0.57721566f +
		       v * (0.99999193f +
			    v * (-0.24991055f +
				 v * (0.05519968f + v * (-0.00976004f + 0.00107857f * v))));
	}
	float num = v * (v + 2.334733f) + 0.250621f;
	float den = v * (v + 3.330657f) + 1.681534f;

	return (num / den) * (expf(-v) / v);
}

static void stft_reset_stats(void)
{
	memset(ctx.ola_buf, 0, sizeof(ctx.ola_buf));
	memset(ctx.mcra_s, 0, sizeof(ctx.mcra_s));
	memset(ctx.mcra_sub, 0, sizeof(ctx.mcra_sub));
	memset(ctx.mcra_smin, 0, sizeof(ctx.mcra_smin));
	memset(ctx.p_spp, 0, sizeof(ctx.p_spp));
	ctx.stft_hops = 0U;
	ctx.mcra_sub_cnt = 0U;
	ctx.mcra_sub_idx = 0U;
	ctx.frame_spp = 0.0f;
	for (uint32_t k = 0U; k < DSP_STFT_BINS; k++) {
		ctx.noise_psd[k] = 1e-8f;
		ctx.prev_post_snr[k] = 1.0f;
		ctx.prev_gain[k] = 1.0f;
	}
}

static void stft_reset_state(void)
{
	memset(ctx.in_ring_sum, 0, sizeof(ctx.in_ring_sum));
	ctx.ring_primed = false;
	stft_reset_stats();
}

static void stft_push_ring_frame(const float *samples_320)
{
	/* Shift 512-sample ring left by 320 samples and append latest 20 ms frame */
	memmove(ctx.in_ring_sum, ctx.in_ring_sum + AUDIO_FRAME_SAMPLES,
		(DSP_STFT_N - AUDIO_FRAME_SAMPLES) * sizeof(float));
	memcpy(ctx.in_ring_sum + (DSP_STFT_N - AUDIO_FRAME_SAMPLES), samples_320,
	       AUDIO_FRAME_SAMPLES * sizeof(float));
	if (ctx.frame_cnt >= 2U) {
		ctx.ring_primed = true;
	}
}

static void stft_process_hop(const float *in_sum_160, float *out_160)
{
	/* Shift 512-sample STFT buffer: [prev 352 samples | new 160 samples] */
	memmove(ctx.in_ring_sum, ctx.in_ring_sum + DSP_STFT_HOP,
		DSP_STFT_OVERLAP * sizeof(float));
	memcpy(ctx.in_ring_sum + DSP_STFT_OVERLAP, in_sum_160, DSP_STFT_HOP * sizeof(float));

	for (uint32_t n = 0U; n < DSP_STFT_N; n++) {
		rfft_in[n] = ctx.in_ring_sum[n] * stft_wa[n];
	}
	arm_rfft_fast_f32(&rfft_inst, rfft_in, rfft_out, 0);

	/* Packed RFFT spectrum power: [0]=DC, [1]=Nyquist, [2k,2k+1]=bin k */
	pwr_x[0] = rfft_out[0] * rfft_out[0];
	pwr_x[DSP_STFT_BINS - 1U] = rfft_out[1] * rfft_out[1];
	for (uint32_t k = 1U; k < DSP_STFT_BINS - 1U; k++) {
		float re = rfft_out[2U * k];
		float im = rfft_out[2U * k + 1U];

		pwr_x[k] = re * re + im * im;
	}

	/* 3-bin frequency smoothing for MCRA local power */
	s_freq[0] = pwr_x[0];
	s_freq[DSP_STFT_BINS - 1U] = pwr_x[DSP_STFT_BINS - 1U];
	for (uint32_t k = 1U; k < DSP_STFT_BINS - 1U; k++) {
		s_freq[k] = 0.25f * pwr_x[k - 1U] + 0.50f * pwr_x[k] + 0.25f * pwr_x[k + 1U];
	}

	ctx.stft_hops++;

	/* Wait until 512-pt STFT window has >= 512 real samples before seeding MCRA */
	if (!ctx.ring_primed && ctx.stft_hops < 4U) {
		arm_rfft_fast_f32(&rfft_inst, rfft_out, rfft_in, 1);
		for (uint32_t n = 0U; n < DSP_STFT_N; n++) {
			ctx.ola_buf[n] += rfft_in[n] * stft_ws[n];
		}
		memcpy(out_160, ctx.ola_buf, DSP_STFT_HOP * sizeof(float));
		memmove(ctx.ola_buf, ctx.ola_buf + DSP_STFT_HOP,
			DSP_STFT_OVERLAP * sizeof(float));
		memset(ctx.ola_buf + DSP_STFT_OVERLAP, 0, DSP_STFT_HOP * sizeof(float));
		return;
	}

	bool seed_now = ctx.ring_primed ? (ctx.stft_hops == 1U) : (ctx.stft_hops == 4U);

	if (ctx.stft_hops >= 4U) {
		ctx.ring_primed = true;
	}

	if (seed_now) {
		for (uint32_t k = 0U; k < DSP_STFT_BINS; k++) {
			float s0 = (s_freq[k] > 1e-10f) ? s_freq[k] : 1e-10f;

			ctx.mcra_s[k] = s0;
			ctx.mcra_smin[k] = s0;
			ctx.noise_psd[k] = s0;
			for (uint32_t w = 0U; w < DSP_MCRA_SUBWINS; w++) {
				ctx.mcra_sub[w][k] = s0;
			}
		}
	} else {
		bool startup_fast = (ctx.stft_hops <= 100U);
		uint16_t sub_period = startup_fast ? 10U : DSP_MCRA_SUB_HOPS;
		float alpha_d_cur = startup_fast ? 0.78f : DSP_MCRA_ALPHA_D;

		for (uint32_t k = 0U; k < DSP_STFT_BINS; k++) {
			float s = DSP_MCRA_ALPHA_S * ctx.mcra_s[k] +
				  (1.0f - DSP_MCRA_ALPHA_S) * s_freq[k];

			ctx.mcra_s[k] = s;
			if (s < ctx.mcra_sub[ctx.mcra_sub_idx][k]) {
				ctx.mcra_sub[ctx.mcra_sub_idx][k] = s;
			}
		}

		ctx.mcra_sub_cnt++;
		if (ctx.mcra_sub_cnt >= sub_period) {
			ctx.mcra_sub_cnt = 0U;
			ctx.mcra_sub_idx = (uint8_t)((ctx.mcra_sub_idx + 1U) &
						     (DSP_MCRA_SUBWINS - 1U));
			memcpy(ctx.mcra_sub[ctx.mcra_sub_idx], ctx.mcra_s, sizeof(ctx.mcra_s));
		}

		for (uint32_t k = 0U; k < DSP_STFT_BINS; k++) {
			float smin = ctx.mcra_sub[0][k];

			for (uint32_t w = 1U; w < DSP_MCRA_SUBWINS; w++) {
				if (ctx.mcra_sub[w][k] < smin) {
					smin = ctx.mcra_sub[w][k];
				}
			}
			ctx.mcra_smin[k] = smin;

			if (ctx.stft_hops <= 20U) {
				float a_init = (pwr_x[k] < ctx.noise_psd[k]) ? 0.65f : 0.75f;
				float np = a_init * ctx.noise_psd[k] + (1.0f - a_init) * pwr_x[k];

				ctx.noise_psd[k] = (isfinite(np) && np > 1e-10f) ? np : 1e-10f;
			} else {
				/* Explicit speech/noise separation: update noise_psd via (1 - p_spp) */
				float sr_min = ctx.mcra_s[k] / (smin + 1e-10f);
				float ind = clampf((sr_min - 1.3f) / (DSP_MCRA_DELTA_MIN - 1.3f),
						   0.0f, 1.0f);
				float p_s = 0.70f * ctx.p_spp[k] + 0.30f * ind;
				float eff_alpha = alpha_d_cur +
						  (1.0f - alpha_d_cur) * p_s;
				float target = eff_alpha * ctx.noise_psd[k] +
					       (1.0f - eff_alpha) * pwr_x[k];
				float anchor = smin * DSP_MCRA_BIAS_COMP;
				float np = (target > anchor) ? target : anchor;

				ctx.p_spp[k] = p_s;
				ctx.noise_psd[k] = (isfinite(np) && np > 1e-10f) ? np : 1e-10f;
			}
		}

		/* Pass 1 over bins: fast LUT Log-MMSE + Bayes SPP + normalized excess VAD */
		const float prior_ratio = (1.0f - DSP_OMLSA_P_PRIOR) / DSP_OMLSA_P_PRIOR;
		const float inv_one_minus_p = 1.0f / (1.0f - DSP_OMLSA_P_PRIOR);
		float voice_exc_sum = 0.0f;

		for (uint32_t k = 0U; k < DSP_STFT_BINS; k++) {
			float post_adj = clampf(pwr_x[k] / (DSP_STFT_NF_MULT * ctx.noise_psd[k] +
							    1e-10f),
						0.0f, 100.0f);
			float excess = (post_adj > 1.0f) ? (post_adj - 1.0f) : 0.0f;
			float pg = ctx.prev_gain[k];
			float a_dd = alpha_dd_bin[k];
			float prio = a_dd * pg * pg * ctx.prev_post_snr[k] +
				     (1.0f - a_dd) * excess;

			if (!isfinite(prio) || prio < 1e-4f) {
				prio = 1e-4f;
			}

			float a_gain = prio / (1.0f + prio);
			float v = a_gain * post_adj;
			float g_logmmse = a_gain * lut_exp_half_e1(v);
			float v_clip = (v > 20.0f) ? 20.0f : v;
			float lambda_lr = (1.0f / (1.0f + prio)) * fast_expf(v_clip);
			float p_h1 = 1.0f / (1.0f + prior_ratio / (lambda_lr + 1e-12f));

			if (k >= DSP_VOICE_BIN_LO && k <= DSP_VOICE_BIN_HI) {
				float p_exc = (p_h1 - DSP_OMLSA_P_PRIOR) * inv_one_minus_p;

				if (p_exc > 0.0f) {
					voice_exc_sum += p_exc;
				}
			}

			ctx.prev_post_snr[k] = (post_adj > 50.0f) ? 50.0f : post_adj;
			/* Store g_logmmse and p_h1 temporarily across gain_raw / s_freq */
			gain_raw[k] = g_logmmse;
			s_freq[k] = p_h1;
		}

		float voice_exc = voice_exc_sum /
				  (float)(DSP_VOICE_BIN_HI - DSP_VOICE_BIN_LO + 1U);
		ctx.frame_spp = 0.85f * ctx.frame_spp + 0.15f * voice_exc;

		float gate_scale = clampf(ctx.frame_spp / 0.16f, 0.0f, 1.0f);
		float eff_gmin = DSP_OMLSA_G_MIN * (0.60f + 0.40f * gate_scale);
		float log2_gmin = fast_log2f(eff_gmin);

		for (uint32_t k = 0U; k < DSP_STFT_BINS; k++) {
			float g_c = clampf(gain_raw[k], eff_gmin, 1.0f);
			float p_h1 = s_freq[k];
			float g_omlsa = fast_exp2f(p_h1 * fast_log2f(g_c) +
						   (1.0f - p_h1) * log2_gmin);

			gain_raw[k] = isfinite(g_omlsa) ? clampf(g_omlsa, eff_gmin, 1.0f) : eff_gmin;
		}

		/* 3-bin spectral gain smoothing prevents isolated musical-noise spikes */
		for (uint32_t k = 0U; k < DSP_STFT_BINS; k++) {
			float gs;

			if (k == 0U || k == (DSP_STFT_BINS - 1U)) {
				gs = gain_raw[k];
			} else {
				gs = 0.2f * gain_raw[k - 1U] + 0.6f * gain_raw[k] +
				     0.2f * gain_raw[k + 1U];
			}
			gs = clampf(gs, eff_gmin, 1.0f);
			ctx.prev_gain[k] = gs;

			if (k == 0U) {
				rfft_out[0] *= gs;
			} else if (k == (DSP_STFT_BINS - 1U)) {
				rfft_out[1] *= gs;
			} else {
				rfft_out[2U * k] *= gs;
				rfft_out[2U * k + 1U] *= gs;
			}
		}
	}

	/* 512-pt inverse RFFT -> rfft_in (note: modifies rfft_out in-place) */
	arm_rfft_fast_f32(&rfft_inst, rfft_out, rfft_in, 1);

	/* Exact 68.75%-overlap WOLA synthesis & shift */
	for (uint32_t n = 0U; n < DSP_STFT_N; n++) {
		ctx.ola_buf[n] += rfft_in[n] * stft_ws[n];
	}
	memcpy(out_160, ctx.ola_buf, DSP_STFT_HOP * sizeof(float));
	memmove(ctx.ola_buf, ctx.ola_buf + DSP_STFT_HOP, DSP_STFT_OVERLAP * sizeof(float));
	memset(ctx.ola_buf + DSP_STFT_OVERLAP, 0, DSP_STFT_HOP * sizeof(float));
}

int audio_dsp_init(void)
{
	/* Exact WOLA COLA pair for N=512, HOP=160 (68.75% overlap, zero flat jumps) */
	float phase_den[DSP_STFT_HOP];

	memset(phase_den, 0, sizeof(phase_den));
	for (uint32_t n = 0U; n < DSP_STFT_N; n++) {
		float w = sinf(((float)M_PI * ((float)n + 0.5f)) / (float)DSP_STFT_N);

		stft_wa[n] = w;
		phase_den[n % DSP_STFT_HOP] += w * w;
	}
	for (uint32_t n = 0U; n < DSP_STFT_N; n++) {
		stft_ws[n] = stft_wa[n] / phase_den[n % DSP_STFT_HOP];
	}

	/* Faster DD tracking above 1.2 kHz keeps voice consonants crisp without raising noise */
	for (uint32_t k = 0U; k < DSP_STFT_BINS; k++) {
		float hz = ((float)k * 16000.0f) / (float)DSP_STFT_N;
		float w_hi = clampf((hz - 1200.0f) / 1000.0f, 0.0f, 1.0f);

		alpha_dd_bin[k] = DSP_STFT_DD_ALPHA - 0.08f * w_hi;
	}

	for (uint32_t i = 0U; i < DSP_E1_LUT_N; i++) {
		float v = 1e-4f + ((float)i * (DSP_E1_LUT_MAX - 1e-4f)) / (float)(DSP_E1_LUT_N - 1U);

		exp_half_e1_lut[i] = expf(0.5f * exact_exp1f(v));
	}

	arm_status st = arm_rfft_fast_init_f32(&rfft_inst, DSP_STFT_N);

	if (st != ARM_MATH_SUCCESS) {
		LOG_ERR("arm_rfft_fast_init_f32(%u) failed: %d", DSP_STFT_N, (int)st);
		return -EINVAL;
	}

	ctx.gain_cal = DSP_GAIN_CAL_DEFAULT;
	audio_dsp_reset();
	LOG_INF("audio_dsp initialized (150 Hz HPF2 + 16-tap NLMS + 512-pt MCRA-2 OM-LSA)");
	return 0;
}

void audio_dsp_reset(void)
{
	float saved_gain = (ctx.gain_cal > 0.0f) ? ctx.gain_cal : DSP_GAIN_CAL_DEFAULT;
	bool cur_nr = audio_dsp_get_nr_enable();

	memset(&ctx, 0, sizeof(ctx));
	ctx.gain_cal = saved_gain;
	stft_reset_state();
	ctx.nr_enabled = cur_nr;
	ctx.prev_nr_enabled = cur_nr;
}

void audio_dsp_set_nr_enable(bool en)
{
	atomic_set(&nr_enabled_flag, en ? 1 : 0);
}

bool audio_dsp_get_nr_enable(void)
{
	return atomic_get(&nr_enabled_flag) != 0;
}

void audio_dsp_set_gain_cal(float gain_cal)
{
	if (isfinite(gain_cal) && gain_cal >= DSP_GAIN_CAL_MIN && gain_cal <= DSP_GAIN_CAL_MAX) {
		ctx.gain_cal = gain_cal;
	}
}

int audio_dsp_process(const int16_t *stereo_in, int16_t *mono_out, bool *vad_out)
{
	if (!stereo_in || !mono_out) {
		return -EINVAL;
	}

	uint32_t cyc_start = k_cycle_get_32();
	bool nr_now = audio_dsp_get_nr_enable();
	bool switching = (nr_now != ctx.prev_nr_enabled);

	ctx.nr_enabled = nr_now;

	/* Pass 1: 2nd-order 150 Hz HPF on both channels + L/R energy */
	float e_l = 0.0f;
	float e_r = 0.0f;

	for (uint32_t n = 0U; n < AUDIO_FRAME_SAMPLES; n++) {
		float in_l = (float)stereo_in[2U * n] * DSP_PCM_SCALE_INV;
		float in_r = (float)stereo_in[2U * n + 1U] * DSP_PCM_SCALE_INV;
		float xl = biquad_df2t(&hpf_150_cfg, in_l, &ctx.hpf_z1[0], &ctx.hpf_z2[0]);
		float xr_raw = biquad_df2t(&hpf_150_cfg, in_r, &ctx.hpf_z1[1], &ctx.hpf_z2[1]);

		xl_buf[n] = xl;
		xr_buf[n] = xr_raw;
		e_l += xl * xl;
		e_r += xr_raw * xr_raw;
	}

	float e_sum = 0.0f;
	float e_diff = 0.0f;

	for (uint32_t n = 0U; n < AUDIO_FRAME_SAMPLES; n++) {
		float xl = xl_buf[n];
		float xr = xr_buf[n] * ctx.gain_cal;
		float half_diff = 0.5f * (xr - xl);
		/* 1.2 kHz crossover: L+R beam below 1.2 kHz, primary L above to prevent 2-4 kHz comb muffling */
		ctx.xover_lp_state = 0.624f * ctx.xover_lp_state + 0.376f * half_diff;
		float y_beam = xl + ctx.xover_lp_state;
		float y_diff = xl - xr;

		gsc_buf[n] = y_beam;
		e_sum += y_beam * y_beam;
		e_diff += y_diff * y_diff;
	}

	float mean_sum = e_sum / (float)AUDIO_FRAME_SAMPLES;
	float mean_diff = e_diff / (float)AUDIO_FRAME_SAMPLES;

	/* Speech-separated VAD: broadband energy jump OR normalized excess voice-band SPP [0..1] */
	bool raw_vad = false;
	bool energy_jump = false;

	if (!ctx.floor_initialized) {
		ctx.noise_floor_ema = (mean_sum > DSP_NOISE_FLOOR_MIN) ? mean_sum : DSP_NOISE_FLOOR_MIN;
		ctx.floor_initialized = true;
	} else {
		energy_jump = mean_sum > (3.2f * ctx.noise_floor_ema);
		bool stft_speech = nr_now && (ctx.stft_hops > 20U) && (ctx.frame_spp > 0.14f);

		raw_vad = energy_jump || stft_speech;
	}

	bool vad_active;

	if (raw_vad) {
		ctx.vad_hangover_frames = DSP_VAD_HANGOVER_FRAMES;
		vad_active = true;
	} else if (ctx.vad_hangover_frames > 0U) {
		ctx.vad_hangover_frames--;
		vad_active = true;
	} else {
		vad_active = false;
	}

	/* Track noise floor without 10 s hang when noise rises after NR switch */
	bool stft_quiet = !nr_now || (ctx.stft_hops <= 20U) || (ctx.frame_spp < 0.12f);
	float ema_alpha = (mean_sum < ctx.noise_floor_ema) ? 0.15f :
			  (stft_quiet ? 0.05f : (energy_jump ? 0.005f : 0.02f));

	ctx.noise_floor_ema = (1.0f - ema_alpha) * ctx.noise_floor_ema + ema_alpha * mean_sum;
	if (!isfinite(ctx.noise_floor_ema) || ctx.noise_floor_ema < DSP_NOISE_FLOOR_MIN) {
		ctx.noise_floor_ema = DSP_NOISE_FLOOR_MIN;
	}

	/* Freeze L/R sensitivity trim during active speech so lateral speech never skews gain_cal */
	bool adapt_quiet = !vad_active && !raw_vad && stft_quiet;

	if (adapt_quiet &&
	    e_r > (1e-6f * (float)AUDIO_FRAME_SAMPLES) &&
	    e_l > (1e-6f * (float)AUDIO_FRAME_SAMPLES)) {
		float ratio = sqrtf(e_l / e_r);

		if (isfinite(ratio)) {
			ratio = clampf(ratio, DSP_GAIN_CAL_MIN, DSP_GAIN_CAL_MAX);
			ctx.gain_cal = 0.99f * ctx.gain_cal + 0.01f * ratio;
		}
	}

	if (vad_out) {
		*vad_out = vad_active;
	}

	ctx.frame_cnt++;
	if ((ctx.frame_cnt % 50U) == 1U) {
		float scale = DSP_PCM_SCALE / DSP_INPUT_GAIN;
		float l_rms = sqrtf(e_l / (float)AUDIO_FRAME_SAMPLES) * scale;
		float r_rms = sqrtf(e_r / (float)AUDIO_FRAME_SAMPLES) * scale;
		float d_rms = sqrtf(mean_diff) * scale;
		float max_w = 0.0f;

		for (uint32_t k = 0U; k < DSP_NLMS_TAPS; k++) {
			float aw = fabsf(ctx.weights[k]);

			if (aw > max_w) {
				max_w = aw;
			}
		}
		LOG_INF("mic raw rms L=%u R=%u diff=%u dsp_us=%u gain_cal_x1000=%u max_w_x1000=%u spp=%u",
			(unsigned)l_rms, (unsigned)r_rms, (unsigned)d_rms,
			(unsigned)ctx.last_dsp_us,
			(unsigned)(ctx.gain_cal * 1000.0f), (unsigned)(max_w * 1000.0f),
			(unsigned)(ctx.frame_spp * 100.0f));
	}

	/* Steady Bypass: output 150 Hz HPF primary mic, keep STFT ring primed for zero-click NR switch */
	if (!nr_now && !switching) {
		if (ctx.stft_hops > 0U) {
			stft_reset_stats();
		}
		stft_push_ring_frame(gsc_buf);
		for (uint32_t n = 0U; n < AUDIO_FRAME_SAMPLES; n++) {
			mono_out[n] = float_to_int16(xl_buf[n]);
		}
		ctx.last_dsp_us = k_cyc_to_us_floor32(k_cycle_get_32() - cyc_start);
		ctx.prev_nr_enabled = nr_now;
		return 0;
	}

	if (switching && nr_now) {
		stft_reset_stats();
		memset(ctx.weights, 0, sizeof(ctx.weights));
		ctx.nlms_active = false;
	}

	/* Pass 2a: 16-tap Circular Leaky NLMS GSC (strictly frozen during speech & STFT startup) */
	bool nlms_adapt = adapt_quiet && (ctx.stft_hops > 40U);
	float mu = nlms_adapt ? DSP_NLMS_MU_NOISE : 0.0f;
	float leak = nlms_adapt ? DSP_NLMS_LEAK_NOISE : 1.0f;
	uint32_t head = ctx.ref_head;

	if (!nlms_adapt && !ctx.nlms_active) {
		for (uint32_t n = 0U; n < AUDIO_FRAME_SAMPLES; n++) {
			float y_diff = xl_buf[n] - xr_buf[n] * ctx.gain_cal;

			head = (head - 1U) & (DSP_NLMS_TAPS - 1U);
			ctx.ref_line[head] = y_diff;
		}
	} else {
		if (nlms_adapt) {
			ctx.nlms_active = true;
		}
		for (uint32_t n = 0U; n < AUDIO_FRAME_SAMPLES; n++) {
			float xl = xl_buf[n];
			float xr = xr_buf[n] * ctx.gain_cal;
			float y_beam = gsc_buf[n];
			float y_diff = xl - xr;

			head = (head - 1U) & (DSP_NLMS_TAPS - 1U);
			ctx.ref_line[head] = y_diff;

			float y_n = 0.0f;
			float p_diff = 0.0f;

			for (uint32_t k = 0U; k < DSP_NLMS_TAPS; k++) {
				float r = ctx.ref_line[(head + k) & (DSP_NLMS_TAPS - 1U)];

				y_n += ctx.weights[k] * r;
				p_diff += r * r;
			}

			float err_sig = y_beam - y_n;

			if (mu > 0.0f) {
				float step = (mu * err_sig) / (p_diff + DSP_NLMS_EPS);
				bool ok = isfinite(err_sig) && isfinite(step);

				if (ok) {
					for (uint32_t k = 0U; k < DSP_NLMS_TAPS; k++) {
						float r = ctx.ref_line[(head + k) & (DSP_NLMS_TAPS - 1U)];
						float w = clampf(leak * ctx.weights[k] + step * r,
								 -DSP_NLMS_WEIGHT_MAX,
								 DSP_NLMS_WEIGHT_MAX);

						if (!isfinite(w)) {
							ok = false;
							break;
						}
						ctx.weights[k] = w;
					}
				}

				if (!ok) {
					memset(ctx.weights, 0, sizeof(ctx.weights));
					ctx.nlms_active = false;
					err_sig = y_beam;
				}
			} else if (!isfinite(err_sig)) {
				memset(ctx.weights, 0, sizeof(ctx.weights));
				ctx.nlms_active = false;
				err_sig = y_beam;
			}

			gsc_buf[n] = err_sig;
		}
	}
	ctx.ref_head = head;

	/* Pass 2b: 2 hops of 160 samples (N=512, 68.75% overlap) -> 320 samples out */
	stft_process_hop(gsc_buf, nr_buf);
	stft_process_hop(gsc_buf + DSP_STFT_HOP, nr_buf + DSP_STFT_HOP);

	for (uint32_t n = 0U; n < AUDIO_FRAME_SAMPLES; n++) {
		float out_f = nr_buf[n];

		if (switching) {
			float frac = (float)n / (float)AUDIO_FRAME_SAMPLES;

			if (nr_now) {
				out_f = (1.0f - frac) * xl_buf[n] + frac * nr_buf[n];
			} else {
				out_f = (1.0f - frac) * nr_buf[n] + frac * xl_buf[n];
			}
		}
		mono_out[n] = float_to_int16(out_f);
	}

	ctx.last_dsp_us = k_cyc_to_us_floor32(k_cycle_get_32() - cyc_start);
	ctx.prev_nr_enabled = nr_now;
	return 0;
}

/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef AUDIO_DSP_H_
#define AUDIO_DSP_H_

#include <stdbool.h>
#include <stdint.h>

#define DSP_NLMS_TAPS           16U
#define DSP_VAD_HANGOVER_FRAMES 10U /* 10 * 20 ms = 200 ms hangover */
#define DSP_STFT_N              512U
#define DSP_STFT_HOP            160U /* Exact 2 hops per 320-sample (20 ms) frame */
#define DSP_STFT_OVERLAP        (DSP_STFT_N - DSP_STFT_HOP) /* 352 (68.75% overlap) */
#define DSP_STFT_BINS           (DSP_STFT_N / 2U + 1U)      /* 257 (31.25 Hz/bin) */
#define DSP_MCRA_SUBWINS        4U   /* 4 * 250 ms = 1.0 s rolling minimum window */

typedef struct {
	bool nr_enabled;
	bool prev_nr_enabled;          /* Detects Bypass <-> NR ON switch for 20 ms cross-fade */
	float gain_cal;                /* Right-mic sensitivity calibration factor, default 1.0f */
	float hpf_z1[2];               /* L/R 2nd-order 150 Hz HPF states */
	float hpf_z2[2];
	float weights[DSP_NLMS_TAPS];  /* 16-tap leaky NLMS FIR weights */
	float ref_line[DSP_NLMS_TAPS]; /* 16-tap circular NLMS reference delay line */
	uint32_t ref_head;             /* Circular write index (0..15) */
	float noise_floor_ema;         /* Smoothed background noise energy estimate */
	float in_ring_sum[DSP_STFT_N];
	float ola_buf[DSP_STFT_N];
	float mcra_s[DSP_STFT_BINS];
	float mcra_sub[DSP_MCRA_SUBWINS][DSP_STFT_BINS];
	float mcra_smin[DSP_STFT_BINS];
	float p_spp[DSP_STFT_BINS];
	float noise_psd[DSP_STFT_BINS];
	float prev_post_snr[DSP_STFT_BINS];
	float prev_gain[DSP_STFT_BINS];
	float frame_spp;               /* Excess voice-band speech presence probability [0..1] */
	float xover_lp_state;          /* 1.2 kHz crossover state (L+R low, L high) */
	uint32_t stft_hops;
	uint32_t frame_cnt;            /* Processed frame counter for 1 Hz L/R diagnostic log */
	uint32_t last_dsp_us;          /* Last audio_dsp_process execution time in us */
	uint16_t mcra_sub_cnt;
	uint8_t mcra_sub_idx;
	uint8_t vad_hangover_frames;   /* Remaining hangover frames */
	bool floor_initialized;        /* First-frame noise floor bootstrap flag */
	bool ring_primed;              /* True once in_ring_sum holds >= 512 real samples */
	bool nlms_active;              /* True once NLMS weights have non-zero taps */
} audio_dsp_ctx_t;

int audio_dsp_init(void);
void audio_dsp_reset(void);

/* Thread-safe atomic NR enable flag (called from sys_mgr / audio_dsp_task). */
void audio_dsp_set_nr_enable(bool en);
bool audio_dsp_get_nr_enable(void);

/* Optional right-mic sensitivity calibration (default 1.0f). */
void audio_dsp_set_gain_cal(float gain_cal);

/* Process one 20 ms stereo frame (320 interleaved [L, R] int16 samples) into
 * 320 mono int16 samples. Must be serialized in audio_dsp_task.
 */
int audio_dsp_process(const int16_t *stereo_in, int16_t *mono_out, bool *vad_out);

#endif /* AUDIO_DSP_H_ */

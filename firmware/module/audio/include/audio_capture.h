/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef AUDIO_CAPTURE_H_
#define AUDIO_CAPTURE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "ble_audio_types.h"

#define AUDIO_SAMPLE_RATE_HZ         16000U
#define AUDIO_FRAME_MS               20U
#define AUDIO_SAMPLES_PER_CH         AUDIO_FRAME_SAMPLES /* 320 */
#define AUDIO_RX_CHANNELS            2U
#define AUDIO_RX_BLOCK_BYTES         (AUDIO_SAMPLES_PER_CH * AUDIO_RX_CHANNELS * sizeof(int16_t)) /* 1280 B */
#define AUDIO_RX_SLAB_COUNT          8U  /* 160 ms DMA + DSP ring headroom (10 KB) */
#define AUDIO_STARTUP_DISCARD_FRAMES 2U  /* Discard first 40 ms startup transient */
#define AUDIO_DMIC_READ_TIMEOUT_MS   100 /* Spec: > 100 ms -> EVT_AUDIO_ERR */

/* Power-on hardware check for ota_dfu self-check gate: verifies PDM0 DMIC ready. */
int audio_capture_selfcheck(void);

/* Initialize capture module and optionally bind the DSP output queue. */
int audio_capture_init(struct k_msgq *dsp_out_q);

/* Control-plane start/stop (thread-safe; called from sys_mgr). */
int audio_capture_start(void);
void audio_capture_stop(void);
bool audio_capture_is_running(void);

/* Block until capture is started (or timeout). Returns 0 once running. */
int audio_capture_wait_start(k_timeout_t timeout);

/* Data-plane DMIC lifecycle (called exclusively from audio_cap_task). */
int audio_capture_stream_start(void);
void audio_capture_stream_stop(void);
int audio_capture_read_block(void **blk, size_t *size, int32_t timeout_ms);
int audio_capture_post_block(void *blk, k_timeout_t timeout);

/* Return a DMA slab block back to dmic_rx_slab (NULL-safe). */
void audio_capture_release_block(void *blk);

#endif /* AUDIO_CAPTURE_H_ */

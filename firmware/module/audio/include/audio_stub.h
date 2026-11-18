/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef AUDIO_STUB_H_
#define AUDIO_STUB_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/* Phase 1B: simulated capture source (1 kHz sine, Raw PCM, 16 kHz mono).
 * Phase 3 replaces the generator with PDM capture / DSP / codec modules while
 * keeping the same start/stop contract.
 */
int audio_capture_start(void);
void audio_capture_stop(void);
bool audio_capture_is_running(void);

/* Block until capture is started (or timeout). Returns 0 once running. */
int audio_capture_wait_start(k_timeout_t timeout);

/* Build one 20 ms SDU: audio_sdu_hdr_t (seq_num auto-increments) + 640 B sine.
 * `out` must hold AUDIO_MAX_SDU_BYTES. Returns SDU length.
 */
uint16_t audio_stub_build_frame(uint8_t *out, uint8_t flags, uint32_t utc_ts);

#endif

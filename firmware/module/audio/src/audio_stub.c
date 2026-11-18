/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <string.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "audio_stub.h"
#include "ble_audio_types.h"

LOG_MODULE_REGISTER(audio_stub, LOG_LEVEL_INF);

/* 1 kHz @ 16 kHz = 16 samples/period, amplitude 8000. 320 samples = 20 periods,
 * so the waveform is phase-continuous across frames. */
static const int16_t sine16[16] = {
	0, 3061, 5657, 7391, 8000, 7391, 5657, 3061,
	0, -3061, -5657, -7391, -8000, -7391, -5657, -3061,
};

static atomic_t running;
static uint16_t seq_num;
static K_SEM_DEFINE(start_sem, 0, 1);

int audio_capture_start(void)
{
	seq_num = 0;
	atomic_set(&running, 1);
	k_sem_give(&start_sem);
	LOG_INF("simulated capture started");
	return 0;
}

void audio_capture_stop(void)
{
	atomic_set(&running, 0);
	k_sem_reset(&start_sem);
	LOG_INF("simulated capture stopped");
}

bool audio_capture_is_running(void)
{
	return atomic_get(&running) != 0;
}

int audio_capture_wait_start(k_timeout_t timeout)
{
	if (audio_capture_is_running()) {
		return 0;
	}
	if (k_sem_take(&start_sem, timeout) != 0) {
		return -EAGAIN;
	}
	return audio_capture_is_running() ? 0 : -EAGAIN;
}

uint16_t audio_stub_build_frame(uint8_t *out, uint8_t flags, uint32_t utc_ts)
{
	audio_sdu_hdr_t hdr = {
		.seq_num = sys_cpu_to_le16(seq_num++),
		.pkt_type = PKT_TYPE_LIVE_STREAM,
		.codec_type = CODEC_RAW_PCM,
		.flags = flags,
		.utc_ts = sys_cpu_to_le32(utc_ts),
		.reserved = 0,
	};
	int16_t *pcm = (int16_t *)(out + AUDIO_SDU_HEADER_SIZE); /* 10 B offset: even, aligned */

	memcpy(out, &hdr, sizeof(hdr));
	for (unsigned int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
		pcm[i] = sys_cpu_to_le16(sine16[i & 15]);
	}
	return AUDIO_MAX_SDU_BYTES;
}

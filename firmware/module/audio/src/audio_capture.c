/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <errno.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "audio_capture.h"

LOG_MODULE_REGISTER(audio_capture, LOG_LEVEL_INF);

static const struct device *const dmic_dev = DEVICE_DT_GET(DT_ALIAS(dmic_dev));

K_MEM_SLAB_DEFINE_STATIC(dmic_rx_slab, AUDIO_RX_BLOCK_BYTES, AUDIO_RX_SLAB_COUNT, 4);

static struct pcm_stream_cfg pcm_stream = {
	.pcm_width = 16U,
	.mem_slab = &dmic_rx_slab,
	.block_size = AUDIO_RX_BLOCK_BYTES,
	.pcm_rate = AUDIO_SAMPLE_RATE_HZ,
};

static struct dmic_cfg dmic_cfg_inst = {
	.io = {
		/* PCLK32M_HFXO (32 MHz) -> hits 1.280 MHz + Ratio80 -> exact 16.000 kHz */
		.min_pdm_clk_freq = 1200000U,
		.max_pdm_clk_freq = 1350000U,
		.min_pdm_clk_dc = 40U,
		.max_pdm_clk_dc = 60U,
	},
	.streams = &pcm_stream,
	.channel = {
		.req_num_streams = 1U,
		.req_num_chan = AUDIO_RX_CHANNELS,
	},
};

static atomic_t running;
static K_SEM_DEFINE(start_sem, 0, 1);
static struct k_msgq *out_q;

int audio_capture_selfcheck(void)
{
	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("DMIC device %s is not ready", dmic_dev->name);
		return -ENODEV;
	}
	LOG_INF("DMIC self-check OK (%s)", dmic_dev->name);
	return 0;
}

int audio_capture_init(struct k_msgq *dsp_out_q)
{
	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("DMIC device %s not ready", dmic_dev->name);
		return -ENODEV;
	}

	out_q = dsp_out_q;
	dmic_cfg_inst.channel.req_chan_map_lo =
		dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
		dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);

	LOG_INF("audio_capture ready: %u Hz stereo (%u B/frame, %u slab blocks)",
		AUDIO_SAMPLE_RATE_HZ, AUDIO_RX_BLOCK_BYTES, AUDIO_RX_SLAB_COUNT);
	return 0;
}

int audio_capture_start(void)
{
	atomic_set(&running, 1);
	k_sem_give(&start_sem);
	return 0;
}

void audio_capture_stop(void)
{
	atomic_set(&running, 0);
	k_sem_reset(&start_sem);
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

int audio_capture_stream_start(void)
{
	int err = -EBUSY;

	/* Retry briefly if a previous DMIC_TRIGGER_STOP is still awaiting EVENTS_STOPPED */
	for (int i = 0; i < 10; i++) {
		err = dmic_configure(dmic_dev, &dmic_cfg_inst);
		if (err != -EBUSY) {
			break;
		}
		k_sleep(K_MSEC(2));
	}
	if (err != 0) {
		LOG_ERR("dmic_configure failed (%d)", err);
		return err;
	}

	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (err != 0) {
		LOG_ERR("dmic_trigger(START) failed (%d)", err);
		return err;
	}
	LOG_INF("PDM0 capture started (1.280 MHz / Ratio80 / 16 kHz stereo)");
	return 0;
}

void audio_capture_stream_stop(void)
{
	void *blk = NULL;
	size_t sz = 0;
	int err;

	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
	if (err != 0) {
		LOG_WRN("dmic_trigger(STOP) failed (%d)", err);
	}

	/* Allow nrfx_pdm ISR to handle EVENTS_STOPPED and release DMA buffers */
	k_sleep(K_MSEC(5));

	/* Drain any already-completed blocks sitting in the driver's rx_queue */
	while (dmic_read(dmic_dev, 0, &blk, &sz, 0) == 0) {
		audio_capture_release_block(blk);
	}
	LOG_INF("PDM0 capture stopped");
}

int audio_capture_read_block(void **blk, size_t *size, int32_t timeout_ms)
{
	if (!blk || !size) {
		return -EINVAL;
	}
	return dmic_read(dmic_dev, 0, blk, size, timeout_ms);
}

int audio_capture_post_block(void *blk, k_timeout_t timeout)
{
	if (!out_q) {
		return -EINVAL;
	}
	return k_msgq_put(out_q, &blk, timeout);
}

void audio_capture_release_block(void *blk)
{
	if (blk != NULL) {
		k_mem_slab_free(&dmic_rx_slab, blk);
	}
}

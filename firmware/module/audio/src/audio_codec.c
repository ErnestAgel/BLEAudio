/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>

#include "audio_codec.h"

#define AUDIO_ADPCM_HDR_BYTES 4U

static const int8_t ima_index_table[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8,
};

static const uint16_t ima_step_table[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
	19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
	5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static inline int32_t clamp_pcm(int32_t v)
{
	if (v > 32767) {
		return 32767;
	}
	if (v < -32768) {
		return -32768;
	}
	return v;
}

static inline int32_t clamp_idx(int32_t idx)
{
	if (idx < 0) {
		return 0;
	}
	if (idx > 88) {
		return 88;
	}
	return idx;
}

/* Estimate a frame-local initial step index purely from the opening samples of the
 * current frame so there is zero cross-frame state dependency while avoiding
 * slope overload at frame boundaries.
 */
static uint8_t estimate_initial_step_idx(const int16_t *pcm)
{
	int32_t max_diff = 0;

	for (uint32_t i = 1U; i < 8U; i++) {
		int32_t d = (int32_t)pcm[i] - (int32_t)pcm[i - 1U];

		if (d < 0) {
			d = -d;
		}
		if (d > max_diff) {
			max_diff = d;
		}
	}

	int32_t target_step = max_diff / 2;

	for (uint8_t idx = 0U; idx < 88U; idx++) {
		if ((int32_t)ima_step_table[idx] >= target_step) {
			return idx;
		}
	}
	return 88U;
}

static uint8_t ima_encode_sample(int16_t sample, int32_t *predictor, int32_t *step_idx)
{
	int32_t step = (int32_t)ima_step_table[*step_idx];
	int32_t diff = (int32_t)sample - *predictor;
	uint8_t nibble = 0U;

	if (diff < 0) {
		nibble = 8U;
		diff = -diff;
	}

	int32_t vpdiff = step >> 3;

	if (diff >= step) {
		nibble |= 4U;
		diff -= step;
		vpdiff += step;
	}
	step >>= 1;
	if (diff >= step) {
		nibble |= 2U;
		diff -= step;
		vpdiff += step;
	}
	step >>= 1;
	if (diff >= step) {
		nibble |= 1U;
		vpdiff += step;
	}

	if ((nibble & 8U) != 0U) {
		*predictor = clamp_pcm(*predictor - vpdiff);
	} else {
		*predictor = clamp_pcm(*predictor + vpdiff);
	}

	*step_idx = clamp_idx(*step_idx + ima_index_table[nibble]);
	return nibble;
}

static int16_t ima_decode_nibble(uint8_t nibble, int32_t *predictor, int32_t *step_idx)
{
	int32_t step = (int32_t)ima_step_table[*step_idx];
	int32_t vpdiff = step >> 3;

	if ((nibble & 4U) != 0U) {
		vpdiff += step;
	}
	if ((nibble & 2U) != 0U) {
		vpdiff += step >> 1;
	}
	if ((nibble & 1U) != 0U) {
		vpdiff += step >> 2;
	}

	if ((nibble & 8U) != 0U) {
		*predictor = clamp_pcm(*predictor - vpdiff);
	} else {
		*predictor = clamp_pcm(*predictor + vpdiff);
	}

	*step_idx = clamp_idx(*step_idx + ima_index_table[nibble & 0x0FU]);
	return (int16_t)*predictor;
}

int audio_codec_encode_sdu(enum codec_type codec, const int16_t *mono_320, uint16_t seq,
			   uint8_t pkt_type, uint8_t flags, uint32_t utc,
			   uint8_t *out_sdu, uint16_t *out_len)
{
	if (!mono_320 || !out_sdu || !out_len) {
		return -EINVAL;
	}

	audio_sdu_hdr_t hdr = {
		.seq_num = sys_cpu_to_le16(seq),
		.pkt_type = pkt_type,
		.codec_type = (uint8_t)codec,
		.flags = flags,
		.utc_ts = sys_cpu_to_le32(utc),
		.reserved = 0U,
	};
	uint8_t *payload = out_sdu + AUDIO_SDU_HEADER_SIZE;

	memcpy(out_sdu, &hdr, sizeof(hdr));

	if (codec == CODEC_RAW_PCM) {
		for (uint32_t i = 0U; i < AUDIO_FRAME_SAMPLES; i++) {
			sys_put_le16((uint16_t)mono_320[i], &payload[i * 2U]);
		}
		*out_len = AUDIO_SDU_HEADER_SIZE + AUDIO_PCM_FRAME_BYTES;
		return 0;
	}

	if (codec == CODEC_IMA_ADPCM_IND) {
		int32_t predictor = (int32_t)mono_320[0];
		int32_t step_idx = (int32_t)estimate_initial_step_idx(mono_320);
		uint8_t *nib_stream = payload + AUDIO_ADPCM_HDR_BYTES;

		sys_put_le16((uint16_t)mono_320[0], &payload[0]);
		payload[2] = (uint8_t)step_idx;
		payload[3] = 0x00U;

		memset(nib_stream, 0, AUDIO_ADPCM_FRAME_BYTES - AUDIO_ADPCM_HDR_BYTES);

		/* Encode pcm[1..319] (319 delta samples): even k -> low nibble, odd k -> high nibble.
		 * Trailing high nibble at byte 159 (after k=318) remains 0x0.
		 */
		for (uint32_t k = 0U; k < AUDIO_FRAME_SAMPLES - 1U; k++) {
			uint8_t nib = ima_encode_sample(mono_320[k + 1U], &predictor, &step_idx);
			uint32_t byte_idx = k >> 1U;

			if ((k & 1U) == 0U) {
				nib_stream[byte_idx] = nib & 0x0FU;
			} else {
				nib_stream[byte_idx] |= (uint8_t)((nib & 0x0FU) << 4U);
			}
		}

		*out_len = AUDIO_SDU_HEADER_SIZE + AUDIO_ADPCM_FRAME_BYTES;
		return 0;
	}

	return -ENOTSUP;
}

int audio_codec_decode_sdu(const uint8_t *in_sdu, uint16_t in_len, int16_t *mono_320)
{
	if (!in_sdu || !mono_320 || in_len < AUDIO_SDU_HEADER_SIZE) {
		return -EINVAL;
	}

	audio_sdu_hdr_t hdr;

	memcpy(&hdr, in_sdu, sizeof(hdr));
	const uint8_t *payload = in_sdu + AUDIO_SDU_HEADER_SIZE;

	if (hdr.codec_type == CODEC_RAW_PCM) {
		if (in_len != AUDIO_SDU_HEADER_SIZE + AUDIO_PCM_FRAME_BYTES) {
			return -EINVAL;
		}
		for (uint32_t i = 0U; i < AUDIO_FRAME_SAMPLES; i++) {
			mono_320[i] = (int16_t)sys_get_le16(&payload[i * 2U]);
		}
		return 0;
	}

	if (hdr.codec_type == CODEC_IMA_ADPCM_IND) {
		if (in_len != AUDIO_SDU_HEADER_SIZE + AUDIO_ADPCM_FRAME_BYTES) {
			return -EINVAL;
		}
		int32_t predictor = (int16_t)sys_get_le16(&payload[0]);
		int32_t step_idx = clamp_idx((int32_t)payload[2]);
		const uint8_t *nib_stream = payload + AUDIO_ADPCM_HDR_BYTES;

		mono_320[0] = (int16_t)predictor;
		for (uint32_t k = 0U; k < AUDIO_FRAME_SAMPLES - 1U; k++) {
			uint8_t byte_val = nib_stream[k >> 1U];
			uint8_t nib = ((k & 1U) == 0U) ? (byte_val & 0x0FU) : (byte_val >> 4U);

			mono_320[k + 1U] = ima_decode_nibble(nib, &predictor, &step_idx);
		}
		return 0;
	}

	return -ENOTSUP;
}

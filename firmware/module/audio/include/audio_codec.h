/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#ifndef AUDIO_CODEC_H_
#define AUDIO_CODEC_H_

#include <stdint.h>
#include "ble_audio_types.h"

/* Stateless, frame-independent SDU encoder:
 *  - CODEC_RAW_PCM (0x00)       -> 650 B (10 B SDU header + 640 B LE PCM)
 *  - CODEC_IMA_ADPCM_IND (0x01) -> 174 B (10 B SDU header + 4 B state + 160 B nibbles)
 */
int audio_codec_encode_sdu(enum codec_type codec, const int16_t *mono_320, uint16_t seq,
			   uint8_t pkt_type, uint8_t flags, uint32_t utc,
			   uint8_t *out_sdu, uint16_t *out_len);

/* Stateless, frame-independent SDU decoder (supports both Raw PCM and IMA-ADPCM). */
int audio_codec_decode_sdu(const uint8_t *in_sdu, uint16_t in_len, int16_t *mono_320);

#endif /* AUDIO_CODEC_H_ */

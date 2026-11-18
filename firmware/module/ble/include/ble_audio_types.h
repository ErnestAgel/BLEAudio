/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Wire-level types per ICD BLE-AUDIO-ICD-005 sec.1.
 * NOTE: the ICD's sys_state_t is NOT redefined here: the firmware keeps its own
 * enum sys_state (sys_mgr.h); ble_status_pkt_t.sys_state carries the ICD value,
 * translated by sys_mgr_fill_status().
 */
#ifndef BLE_AUDIO_TYPES_H_
#define BLE_AUDIO_TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/toolchain.h>
#include <zephyr/sys/util.h>

#define AUDIO_SDU_HEADER_SIZE   10U
#define AUDIO_PCM_FRAME_BYTES   640U   /* 320 samples * 2 B */
#define AUDIO_ADPCM_FRAME_BYTES 164U
#define AUDIO_MAX_SDU_BYTES     (AUDIO_SDU_HEADER_SIZE + AUDIO_PCM_FRAME_BYTES) /* 650 B */
#define AUDIO_FRAME_SAMPLES     320U
#define AUDIO_FRAME_PERIOD_MS   20

/* ICD sys_state values reported in the Status characteristic */
#define ICD_SYS_STATE_IDLE      1U
#define ICD_SYS_STATE_CAPTURING 2U
#define ICD_SYS_STATE_UPLOADING 3U
#define ICD_SYS_STATE_OTA       4U

enum pkt_type {
	PKT_TYPE_LIVE_STREAM = 0x00,
	PKT_TYPE_QSPI_REPLAY = 0x01,
};

enum codec_type {
	CODEC_RAW_PCM       = 0x00,
	CODEC_IMA_ADPCM_IND = 0x01,
};

#define SDU_FLAG_NR_ON      BIT(0)
#define SDU_FLAG_VAD        BIT(1)
#define SDU_FLAG_EOF        BIT(2)
#define SDU_FLAG_UTC_VALID  BIT(3)

typedef struct __packed {
	uint16_t seq_num;
	uint8_t  pkt_type;
	uint8_t  codec_type;
	uint8_t  flags;
	uint32_t utc_ts;
	uint8_t  reserved;
} audio_sdu_hdr_t;

/* GATT Control (0x1002) opcodes */
enum ble_ctrl_op {
	CTRL_OP_START_CAPTURE  = 0x01,
	CTRL_OP_STOP_CAPTURE   = 0x02,
	CTRL_OP_SET_NR         = 0x03,
	CTRL_OP_TRIGGER_REPLAY = 0x04,
	CTRL_OP_SET_UTC_TIME   = 0x05,
	CTRL_OP_SET_CODEC      = 0x06,
};

/* GATT Status (0x1003), 10 B */
typedef struct __packed {
	uint8_t  sys_state;
	uint8_t  nr_on;
	uint8_t  codec_type;
	uint8_t  phy_mode;       /* 1=1M, 2=2M */
	uint8_t  qspi_used_pct;
	uint8_t  pending_files;
	uint32_t drop_cnt;
} ble_status_pkt_t;

/* GATT L2CAP Info (0x1004), 4 B */
typedef struct __packed {
	uint16_t psm_le;
	uint16_t max_sdu;
} ble_l2cap_info_t;

BUILD_ASSERT(sizeof(audio_sdu_hdr_t) == AUDIO_SDU_HEADER_SIZE, "SDU header must be 10 B");
BUILD_ASSERT(sizeof(ble_status_pkt_t) == 10, "Status packet must be 10 B");
BUILD_ASSERT(sizeof(ble_l2cap_info_t) == 4, "L2CAP info must be 4 B");

#endif /* BLE_AUDIO_TYPES_H_ */

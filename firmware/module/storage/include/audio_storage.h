/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * audio_storage - Phase 2: QSPI LittleFS store-and-forward for audio SDUs.
 * Spec: BLE-AUDIO-MOD-MEM-004 sec.3 (16 KB ring_buf -> 4 KB page writes, NVS boot_cnt,
 * anti-overwrite naming, numeric replay order, 92~95% watermark, unlink after sent).
 *
 * Threading model
 *   - audio_cap_task (producer): audio_storage_open / write_sdu / close. All three are
 *     NON-BLOCKING (ring_buf only); the file system is touched exclusively by
 *     audio_store_task (audio_storage_run) and, for replay, by ble_tx_task.
 *   - On-flash record format: [uint16_t sdu_len (LE)][audio_sdu_frame] ...
 */
#ifndef AUDIO_STORAGE_H_
#define AUDIO_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#define AUDIO_STORAGE_MOUNT_POINT "/lfs"

/* qspi_ring_buf geometry: 24 KB (~750 ms at 32.6 KB/s) absorbs 240 ms sector erases */
#define AUDIO_STORAGE_RING_BYTES  24576U
#define AUDIO_STORAGE_PAGE_BYTES  4096U

/* Watermark: stop capturing at >= 94 % used (spec window 92~95 %), resume below 90 %. */
#define AUDIO_STORAGE_WATERMARK_PCT 94U
#define AUDIO_STORAGE_RESUME_PCT    90U

/* Max files kept in the sorted replay list per scan pass (re-scan until empty). */
#define AUDIO_STORAGE_MAX_LIST 48U

/* ---- lifecycle (audio_store_task only) ------------------------------------ */

/* Task body: NVS boot_cnt, LittleFS mount, then page-write loop. Never returns. */
void audio_storage_run(void);

/* Block until audio_storage_run() finished its init. Returns 0 if LittleFS is mounted. */
int audio_storage_wait_ready(k_timeout_t timeout);

/* Power-on self-check for ota_dfu: LittleFS mounted and statvfs() usable. */
int audio_storage_selfcheck(void);

/* ---- producer side (audio_cap_task, non-blocking) ------------------------- */

/* Begin a recording session. utc_valid selects "<utc>.pcm" vs "boot_<cnt>_<seq>.pcm".
 * -ENODEV storage unavailable, -EBUSY previous session still flushing, -ENOSPC watermark.
 */
int audio_storage_open(uint32_t utc_ts, bool utc_valid);

/* Append one SDU (copied into the ring). -ENOBUFS = ring full (frame dropped),
 * -ENOSPC = watermark reached, -EBADF = no open session.
 */
int audio_storage_write_sdu(const uint8_t *sdu, uint16_t len);

/* End the session: the store task flushes the tail, fs_sync() + fs_close(). Async. */
int audio_storage_close(void);

/* true while a session is open or its tail is still being flushed. */
bool audio_storage_session_busy(void);

/* ---- status --------------------------------------------------------------- */
uint8_t audio_storage_get_used_pct(void);
uint8_t audio_storage_count_pending(void);
uint32_t audio_storage_get_ring_drop_cnt(void);
void audio_storage_count_drop(void);

/* ---- replay (ble_tx_task) ------------------------------------------------- */

/* Replay trigger flag: set by sys_mgr (capture stop / GATT 0x04) and by ble_tx_task
 * (L2CAP ready edge); consumed by ble_tx_task. */
void audio_storage_request_replay(void);
bool audio_storage_take_replay_request(void);

/* Scan /lfs, sort numerically (boot_<cnt>_<seq> first, then <utc>[_n]) and return the
 * number of entries in the list (<= AUDIO_STORAGE_MAX_LIST). */
int audio_storage_scan(void);
const char *audio_storage_scan_name(int idx);

int audio_storage_replay_open(const char *name);

/* Returns 0 and fills sdu/len; *is_eof is true for the last valid frame of the file.
 * -ENODATA: file holds no valid frame. */
int audio_storage_replay_next_frame(uint8_t *sdu_buf, uint16_t *len, bool *is_eof);

/* Close the replay file without deleting it (abort / link loss keeps it intact). */
void audio_storage_replay_close(void);

/* Close and fs_unlink() the file being replayed (call only after all `sent` confirms). */
int audio_storage_unlink_active_replay(void);

#endif /* AUDIO_STORAGE_H_ */

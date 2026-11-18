/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * audio_storage - QSPI LittleFS store-and-forward (Phase 2).
 * Mount pattern ported from zephyr/samples/subsys/fs/littlefs (flash_map storage_dev),
 * bound via native DTS fixed-partitions (lfs_partition on &mx25r64).
 */
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <nrfx_qspi.h>

#include "audio_storage.h"
#include "ble_audio_types.h"
#include "sys_mgr.h"

LOG_MODULE_REGISTER(audio_storage, LOG_LEVEL_INF);

#define LFS_ROOT        AUDIO_STORAGE_MOUNT_POINT
#define REC_HDR_BYTES   2U
#define NAME_MAX_LEN    32
#define PATH_MAX_LEN    (sizeof(LFS_ROOT) + NAME_MAX_LEN)
/* statvfs() walks the whole FS: only run it mid-recording once estimated usage >= 85 % */
#define USAGE_FAST_PCT    85U

/* ------------------------------------------------------------------ state */
enum sess_state {
	SESS_IDLE = 0,
	SESS_ACTIVE,   /* producer may write */
	SESS_CLOSING,  /* tail flush pending in the store task */
};

RING_BUF_DECLARE(qspi_ring_buf, AUDIO_STORAGE_RING_BYTES);
static uint8_t page_buf[AUDIO_STORAGE_PAGE_BYTES];
static K_SEM_DEFINE(store_sem, 0, 1);
static K_SEM_DEFINE(ready_sem, 0, 1);

static atomic_t sess_state = ATOMIC_INIT(SESS_IDLE);
static atomic_t ring_drop_cnt;
static atomic_t replay_req;
static atomic_t used_pct_cache;
static atomic_t pending_cache;
static atomic_t wm_hit;
static bool wm_event_pending;

static int init_result = -EAGAIN;
static bool mounted;

static struct {
	uint32_t utc;
	bool valid;
	uint32_t seq;
} sess;

static uint32_t boot_cnt;
static uint32_t next_seq;

/* writer context (store task only) */
static struct fs_file_t wr_file;
static bool wr_open;
static char wr_name[NAME_MAX_LEN];
static uint32_t wr_bytes;
static uint64_t fs_used_bytes_base;
static uint32_t bytes_since_check;
static uint64_t fs_total_bytes;

/* replay context + sorted list (ble_tx task only) */
struct file_key {
	uint8_t cls;   /* 0 = boot_<a>_<b>, 1 = <a>[_<b>] utc */
	uint32_t a;
	uint32_t b;
};

struct list_entry {
	struct file_key key;
	char name[NAME_MAX_LEN];
};

static struct list_entry list[AUDIO_STORAGE_MAX_LIST];
static int list_cnt;
static struct fs_file_t rd_file;
static bool rd_open;
static char rd_name[NAME_MAX_LEN];
static uint8_t rd_next[AUDIO_MAX_SDU_BYTES];
static uint16_t rd_next_len;
static struct fs_dirent dirent;   /* shared by scan/count, serialised by scan_lock */
static K_MUTEX_DEFINE(scan_lock);
static K_MUTEX_DEFINE(usage_lock);
static K_MUTEX_DEFINE(wr_meta_lock); /* short-lived guard for wr_open/wr_name */

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_data);
static struct fs_mount_t lfs_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &lfs_data,
	.storage_dev = (void *)PARTITION_ID(lfs_partition),
	.mnt_point = LFS_ROOT,
};

/* ----------------------------------------------------------- name helpers */
static bool parse_u32(const char **p, uint32_t *out)
{
	const char *s = *p;
	uint64_t v = 0;

	if (*s < '0' || *s > '9') {
		return false;
	}
	while (*s >= '0' && *s <= '9') {
		v = v * 10U + (uint32_t)(*s - '0');
		if (v > UINT32_MAX) {
			return false;
		}
		s++;
	}
	*p = s;
	*out = (uint32_t)v;
	return true;
}

/* boot_<cnt>_<seq>.pcm -> cls 0;  <utc>.pcm / <utc>_<n>.pcm -> cls 1 (numeric keys). */
static bool parse_name(const char *name, struct file_key *k)
{
	const char *p = name;

	if (strncmp(p, "boot_", 5) == 0) {
		p += 5;
		k->cls = 0;
		if (!parse_u32(&p, &k->a) || *p != '_') {
			return false;
		}
		p++;
		if (!parse_u32(&p, &k->b)) {
			return false;
		}
	} else {
		k->cls = 1;
		k->b = 0;
		if (!parse_u32(&p, &k->a)) {
			return false;
		}
		if (*p == '_') {
			p++;
			if (!parse_u32(&p, &k->b)) {
				return false;
			}
		}
	}
	return strcmp(p, ".pcm") == 0;
}

static int key_cmp(const struct file_key *x, const struct file_key *y)
{
	if (x->cls != y->cls) {
		return x->cls < y->cls ? -1 : 1;
	}
	if (x->a != y->a) {
		return x->a < y->a ? -1 : 1;
	}
	if (x->b != y->b) {
		return x->b < y->b ? -1 : 1;
	}
	return 0;
}

static bool file_exists(const char *path)
{
	struct fs_dirent de;

	return fs_stat(path, &de) == 0;
}

/* --------------------------------------------------------- usage / counts */
static void refresh_usage(void)
{
	struct fs_statvfs sv;
	uint32_t used;

	k_mutex_lock(&usage_lock, K_FOREVER);
	if (!mounted || fs_statvfs(LFS_ROOT, &sv) != 0 || sv.f_blocks == 0) {
		k_mutex_unlock(&usage_lock);
		return;
	}
	used = (uint32_t)(((uint64_t)(sv.f_blocks - sv.f_bfree) * 100U) / sv.f_blocks);
	fs_total_bytes = (uint64_t)sv.f_frsize * sv.f_blocks;
	fs_used_bytes_base = (uint64_t)sv.f_frsize * (sv.f_blocks - sv.f_bfree);
	bytes_since_check = 0;
	atomic_set(&used_pct_cache, used);

	if (used >= AUDIO_STORAGE_WATERMARK_PCT) {
		if (atomic_cas(&wm_hit, 0, 1)) {
			wm_event_pending = true;
			LOG_WRN("QSPI used %u%% >= %u%%: watermark", used,
				AUDIO_STORAGE_WATERMARK_PCT);
		}
	} else if (used < AUDIO_STORAGE_RESUME_PCT) {
		atomic_set(&wm_hit, 0);
	}
	k_mutex_unlock(&usage_lock);
}

/* Count *.pcm entries; also tracks the highest boot_<cnt> for boot_cnt sanity. */
static int count_files(uint32_t *max_boot)
{
	struct fs_dir_t dir;
	int cnt = 0;
	struct file_key k;

	k_mutex_lock(&scan_lock, K_FOREVER);
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, LFS_ROOT) != 0) {
		k_mutex_unlock(&scan_lock);
		return -EIO;
	}
	while (fs_readdir(&dir, &dirent) == 0 && dirent.name[0] != '\0') {
		if (dirent.type != FS_DIR_ENTRY_FILE || !parse_name(dirent.name, &k)) {
			continue;
		}
		cnt++;
		if (max_boot && k.cls == 0 && k.a > *max_boot) {
			*max_boot = k.a;
		}
	}
	fs_closedir(&dir);
	k_mutex_unlock(&scan_lock);
	return cnt;
}

static void refresh_pending(void)
{
	int n = count_files(NULL);

	if (n >= 0) {
		atomic_set(&pending_cache, n > 255 ? 255 : n);
	}
}

/* ------------------------------------------------------------- boot_cnt */
static int audio_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			      void *cb_arg)
{
	if (strcmp(name, "boot_cnt") == 0 && len == sizeof(boot_cnt)) {
		return read_cb(cb_arg, &boot_cnt, sizeof(boot_cnt)) < 0 ? -EIO : 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(audio_cfg, "audio", NULL, audio_settings_set, NULL, NULL);

static int boot_cnt_init(void)
{
	int err = settings_subsys_init();

	if (err) {
		LOG_ERR("settings_subsys_init failed (%d)", err);
		return err;
	}
	boot_cnt = 0;
	err = settings_load_subtree("audio");
	if (err) {
		LOG_WRN("settings_load(audio) failed (%d), boot_cnt restarts", err);
	}
	boot_cnt++;
	err = settings_save_one("audio/boot_cnt", &boot_cnt, sizeof(boot_cnt));
	if (err) {
		/* Names stay unique anyway: boot_cnt is lifted above existing files later */
		LOG_WRN("settings_save(boot_cnt) failed (%d)", err);
	}
	return 0;
}

/* ------------------------------------------------------------ mount/init */
static int qspi_cinstr(uint8_t opcode, nrf_qspi_cinstr_len_t len, const void *tx, void *rx,
		       bool wren, bool wipwait)
{
	nrf_qspi_cinstr_conf_t cfg = {
		.opcode = opcode,
		.length = len,
		.io2_level = true,
		.io3_level = true,
		.wipwait = wipwait,
		.wren = wren,
	};

	return nrfx_qspi_cinstr_xfer(&cfg, tx, rx);
}

/* MX25R6435F defaults to Low-Power mode (tPP 3.2 ms, tSE 58 ms -> ~28 KB/s on LittleFS,
 * slower than 32.6 KB/s Raw PCM). Setting CR2 bit 1 (L/H=1) selects High-Performance mode.
 */
static void mx25r_enable_high_perf(void)
{
	const struct device *dev = PARTITION_DEVICE(lfs_partition);
	uint8_t id[3] = {0};
	uint8_t sr = 0;
	uint8_t cr[2] = {0};
	int rc;

	if (!dev || !device_is_ready(dev) || !nrfx_qspi_init_check()) {
		return;
	}

	rc = qspi_cinstr(0x9F, NRF_QSPI_CINSTR_LEN_4B, NULL, id, false, false);
	if (rc != 0 || id[0] != 0xC2 || id[1] != 0x28) {
		goto out;
	}
	if (qspi_cinstr(0x05, NRF_QSPI_CINSTR_LEN_2B, NULL, &sr, false, false) != 0 ||
	    qspi_cinstr(0x15, NRF_QSPI_CINSTR_LEN_3B, NULL, cr, false, false) != 0) {
		goto out;
	}

	if ((cr[1] & BIT(1)) == 0U) {
		uint8_t wr[3] = { sr, cr[0], (uint8_t)(cr[1] | BIT(1)) };

		rc = qspi_cinstr(0x01, NRF_QSPI_CINSTR_LEN_4B, wr, NULL, true, true);
		for (int i = 0; rc == 0 && i < 50; i++) {
			uint8_t s = 0;

			if (qspi_cinstr(0x05, NRF_QSPI_CINSTR_LEN_2B, NULL, &s, false, false) !=
				    0 ||
			    (s & BIT(0)) == 0U) {
				break;
			}
			k_msleep(1);
		}
		if (rc == 0 &&
		    qspi_cinstr(0x15, NRF_QSPI_CINSTR_LEN_3B, NULL, cr, false, false) == 0 &&
		    (cr[1] & BIT(1)) != 0U) {
			LOG_INF("MX25R64 High-Performance mode enabled (CR2=0x%02x)", cr[1]);
		} else {
			LOG_WRN("MX25R64 High-Performance switch failed (%d, CR2=0x%02x)", rc,
				cr[1]);
		}
	} else {
		LOG_INF("MX25R64 already in High-Performance mode (CR2=0x%02x)", cr[1]);
	}

out:
	(void)nrfx_qspi_deactivate();
}

static int storage_init(void)
{
	uint32_t max_boot = 0;
	int err;

	err = boot_cnt_init();
	if (err) {
		return err;
	}

	mx25r_enable_high_perf();

	err = fs_mount(&lfs_mnt);   /* formats automatically when the medium is blank */
	if (err) {
		LOG_ERR("LittleFS mount on %s failed (%d)", LFS_ROOT, err);
		return err;
	}
	mounted = true;

	if (count_files(&max_boot) >= 0 && boot_cnt <= max_boot) {
		boot_cnt = max_boot + 1U;
		LOG_WRN("boot_cnt lifted to %u to stay above stored files", boot_cnt);
		(void)settings_save_one("audio/boot_cnt", &boot_cnt, sizeof(boot_cnt));
	}
	refresh_usage();
	refresh_pending();
	LOG_INF("LittleFS mounted at %s: boot_cnt=%u used=%u%% pending=%u", LFS_ROOT, boot_cnt,
		(uint32_t)atomic_get(&used_pct_cache), (uint32_t)atomic_get(&pending_cache));
	return 0;
}

int audio_storage_wait_ready(k_timeout_t timeout)
{
	if (k_sem_take(&ready_sem, timeout) != 0) {
		return -ETIMEDOUT;
	}
	k_sem_give(&ready_sem);   /* stay signalled for other waiters */
	return init_result;
}

int audio_storage_selfcheck(void)
{
	int err = audio_storage_wait_ready(K_SECONDS(10));

	if (err) {
		LOG_ERR("LittleFS self-check: init failed (%d)", err);
		return err;
	}
	if (!mounted || atomic_get(&used_pct_cache) > 100) {
		return -EIO;
	}
	LOG_INF("LittleFS self-check OK (used %u%%, pending %u)",
		(uint32_t)atomic_get(&used_pct_cache), (uint32_t)atomic_get(&pending_cache));
	return 0;
}

/* ---------------------------------------------------------- producer API */
int audio_storage_open(uint32_t utc_ts, bool utc_valid)
{
	if (!mounted) {
		return -ENODEV;
	}
	if (atomic_get(&wm_hit)) {
		return -ENOSPC;
	}
	if (!atomic_cas(&sess_state, SESS_IDLE, SESS_ACTIVE)) {
		return -EBUSY;
	}
	sess.utc = utc_ts;
	sess.valid = utc_valid;
	sess.seq = next_seq++;
	return 0;
}

int audio_storage_write_sdu(const uint8_t *sdu, uint16_t len)
{
	uint8_t hdr[REC_HDR_BYTES];

	if (!sdu || len < AUDIO_SDU_HEADER_SIZE || len > AUDIO_MAX_SDU_BYTES) {
		return -EINVAL;
	}
	if (atomic_get(&sess_state) != SESS_ACTIVE) {
		return -EBADF;
	}
	if (atomic_get(&wm_hit)) {
		return -ENOSPC;
	}
	if (ring_buf_space_get(&qspi_ring_buf) < (uint32_t)len + REC_HDR_BYTES) {
		atomic_inc(&ring_drop_cnt);   /* QSPI erase stall outlasted the 24 KB buffer */
		return -ENOBUFS;
	}
	sys_put_le16(len, hdr);
	(void)ring_buf_put(&qspi_ring_buf, hdr, sizeof(hdr));
	(void)ring_buf_put(&qspi_ring_buf, sdu, len);

	if (ring_buf_size_get(&qspi_ring_buf) >= AUDIO_STORAGE_PAGE_BYTES) {
		k_sem_give(&store_sem);
	}
	return 0;
}

int audio_storage_close(void)
{
	if (!atomic_cas(&sess_state, SESS_ACTIVE, SESS_CLOSING)) {
		return -EBADF;
	}
	k_sem_give(&store_sem);
	return 0;
}

bool audio_storage_session_busy(void)
{
	return atomic_get(&sess_state) != SESS_IDLE;
}

uint8_t audio_storage_get_used_pct(void)
{
	return (uint8_t)atomic_get(&used_pct_cache);
}

uint8_t audio_storage_count_pending(void)
{
	return (uint8_t)atomic_get(&pending_cache);
}

uint32_t audio_storage_get_ring_drop_cnt(void)
{
	return (uint32_t)atomic_get(&ring_drop_cnt);
}

void audio_storage_count_drop(void)
{
	atomic_inc(&ring_drop_cnt);
}

void audio_storage_request_replay(void)
{
	atomic_set(&replay_req, 1);
}

bool audio_storage_take_replay_request(void)
{
	return atomic_cas(&replay_req, 1, 0);
}

/* ------------------------------------------------------- writer (store task) */
static bool usage_add_bytes_should_refresh(uint32_t len)
{
	bool need = false;

	k_mutex_lock(&usage_lock, K_FOREVER);
	bytes_since_check += len;
	if (fs_total_bytes != 0) {
		uint32_t est_pct = (uint32_t)(((fs_used_bytes_base + bytes_since_check) * 100ULL) /
					      fs_total_bytes);

		if (est_pct > 100U) {
			est_pct = 100U;
		}
		atomic_set(&used_pct_cache, est_pct);
		if (est_pct >= USAGE_FAST_PCT && bytes_since_check >= (32U * 1024U)) {
			need = true;
		}
	}
	k_mutex_unlock(&usage_lock);
	return need;
}

static int writer_open(void)
{
	char path[PATH_MAX_LEN];
	char name[NAME_MAX_LEN];
	int err;

	refresh_usage();
	if (atomic_get(&wm_hit)) {
		return -ENOSPC;
	}

	if (sess.valid) {
		snprintf(name, sizeof(name), "%u.pcm", sess.utc);
		for (unsigned int n = 1; n < 100U; n++) {
			snprintf(path, sizeof(path), LFS_ROOT "/%s", name);
			if (!file_exists(path)) {
				break;
			}
			snprintf(name, sizeof(name), "%u_%u.pcm", sess.utc, n);
		}
	} else {
		snprintf(name, sizeof(name), "boot_%u_%u.pcm", boot_cnt, sess.seq);
	}
	snprintf(path, sizeof(path), LFS_ROOT "/%s", name);

	fs_file_t_init(&wr_file);
	err = fs_open(&wr_file, path, FS_O_CREATE | FS_O_WRITE);
	if (err) {
		LOG_ERR("fs_open(%s) failed (%d)", path, err);
		return err;
	}
	k_mutex_lock(&wr_meta_lock, K_FOREVER);
	strncpy(wr_name, name, sizeof(wr_name) - 1);
	wr_name[sizeof(wr_name) - 1] = '\0';
	wr_open = true;
	k_mutex_unlock(&wr_meta_lock);

	wr_bytes = 0;
	LOG_INF("QSPI file %s opened (used %u%%)", name, audio_storage_get_used_pct());
	return 0;
}

static void writer_close(void)
{
	if (!wr_open) {
		return;
	}
	(void)fs_sync(&wr_file);
	(void)fs_close(&wr_file);
	k_mutex_lock(&wr_meta_lock, K_FOREVER);
	wr_open = false;
	k_mutex_unlock(&wr_meta_lock);
	LOG_INF("QSPI file %s closed (%u B)", wr_name, wr_bytes);
	refresh_pending();
	refresh_usage();
}

/* Write `len` bytes of page_buf; opens the file lazily. Returns 0 or -errno. */
static int writer_write(uint32_t len)
{
	ssize_t w;

	if (!wr_open) {
		int err = writer_open();

		if (err) {
			return err;
		}
	}
	w = fs_write(&wr_file, page_buf, len);
	if (w != (ssize_t)len) {
		LOG_ERR("fs_write(%u) -> %d", len, (int)w);
		writer_close();
		return w < 0 ? (int)w : -EIO;
	}
	wr_bytes += len;

	/* Cheap estimate between statvfs() calls */
	if (usage_add_bytes_should_refresh(len)) {
		refresh_usage();
	}
	return 0;
}

/* Drain whole pages (or everything when `all`) from the ring into the file. */
static void drain_ring(bool all)
{
	uint32_t avail;

	while ((avail = ring_buf_size_get(&qspi_ring_buf)) >=
	       (all ? 1U : AUDIO_STORAGE_PAGE_BYTES)) {
		uint32_t n = MIN(avail, AUDIO_STORAGE_PAGE_BYTES);

		if (atomic_get(&wm_hit) && !wr_open) {
			/* Sealed by the watermark: discard what the producer still queued */
			(void)ring_buf_get(&qspi_ring_buf, NULL, n);
			continue;
		}
		(void)ring_buf_get(&qspi_ring_buf, page_buf, n);
		if (writer_write(n) != 0) {
			(void)ring_buf_get(&qspi_ring_buf, NULL,
					   ring_buf_size_get(&qspi_ring_buf));
			return;
		}
		if (atomic_get(&wm_hit)) {
			/* 92~95 %: seal this file now, protect against -ENOSPC */
			writer_close();
		}
	}
}

static void notify_watermark(void)
{
	if (wm_event_pending && sys_mgr_post_event(EVT_QSPI_WATERMARK_95) == 0) {
		wm_event_pending = false;
	}
}

void audio_storage_run(void)
{
	init_result = storage_init();
	k_sem_give(&ready_sem);
	if (init_result) {
		LOG_ERR("audio_storage disabled (%d)", init_result);
		for (;;) {
			k_sleep(K_FOREVER);
		}
	}

	for (;;) {
		(void)k_sem_take(&store_sem, K_MSEC(250));

		drain_ring(false);
		notify_watermark();

		if (atomic_get(&sess_state) == SESS_CLOSING) {
			drain_ring(true);              /* tail < 4 KB, written as-is */
			writer_close();                /* fs_sync + fs_close */
			notify_watermark();
			atomic_set(&sess_state, SESS_IDLE);
		}
	}
}

/* ------------------------------------------------------------ replay side */
int audio_storage_scan(void)
{
	struct fs_dir_t dir;
	struct file_key k;
	struct list_entry *e;
	char active_wr[NAME_MAX_LEN] = {0};

	k_mutex_lock(&wr_meta_lock, K_FOREVER);
	if (wr_open) {
		strncpy(active_wr, wr_name, sizeof(active_wr) - 1);
	}
	k_mutex_unlock(&wr_meta_lock);

	k_mutex_lock(&scan_lock, K_FOREVER);
	list_cnt = 0;
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, LFS_ROOT) != 0) {
		k_mutex_unlock(&scan_lock);
		return -EIO;
	}
	while (fs_readdir(&dir, &dirent) == 0 && dirent.name[0] != '\0') {
		int pos;

		if (dirent.type != FS_DIR_ENTRY_FILE || !parse_name(dirent.name, &k) ||
		    (active_wr[0] != '\0' && strcmp(dirent.name, active_wr) == 0)) {
			continue;
		}
		/* Insertion sort on numeric keys, keep the smallest AUDIO_STORAGE_MAX_LIST */
		pos = list_cnt;
		while (pos > 0 && key_cmp(&list[pos - 1].key, &k) > 0) {
			pos--;
		}
		if (pos >= (int)AUDIO_STORAGE_MAX_LIST) {
			continue;
		}
		if (list_cnt < (int)AUDIO_STORAGE_MAX_LIST) {
			list_cnt++;
		}
		for (int i = list_cnt - 1; i > pos; i--) {
			list[i] = list[i - 1];
		}
		e = &list[pos];
		e->key = k;
		strncpy(e->name, dirent.name, sizeof(e->name) - 1);
		e->name[sizeof(e->name) - 1] = '\0';
	}
	fs_closedir(&dir);
	k_mutex_unlock(&scan_lock);
	return list_cnt;
}

const char *audio_storage_scan_name(int idx)
{
	return (idx >= 0 && idx < list_cnt) ? list[idx].name : NULL;
}

/* Read the next valid record into rd_next; rd_next_len = 0 when none is left. */
static void replay_prefetch(void)
{
	uint8_t hdr[REC_HDR_BYTES];
	ssize_t r;
	uint16_t len;

	rd_next_len = 0;
	r = fs_read(&rd_file, hdr, sizeof(hdr));
	if (r != (ssize_t)sizeof(hdr)) {
		return;
	}
	len = sys_get_le16(hdr);
	if (len < AUDIO_SDU_HEADER_SIZE || len > AUDIO_MAX_SDU_BYTES) {
		LOG_WRN("%s: bad record length %u, truncating replay", rd_name, len);
		return;
	}
	r = fs_read(&rd_file, rd_next, len);
	if (r != (ssize_t)len) {
		LOG_WRN("%s: truncated tail record (power loss?)", rd_name);
		return;
	}
	rd_next_len = len;
}

int audio_storage_replay_open(const char *name)
{
	char path[PATH_MAX_LEN];
	int err;

	if (rd_open) {
		audio_storage_replay_close();
	}
	snprintf(rd_name, sizeof(rd_name), "%s", name);
	snprintf(path, sizeof(path), LFS_ROOT "/%s", rd_name);
	fs_file_t_init(&rd_file);
	err = fs_open(&rd_file, path, FS_O_READ);
	if (err) {
		LOG_ERR("replay fs_open(%s) failed (%d)", path, err);
		return err;
	}
	rd_open = true;
	replay_prefetch();
	return 0;
}

int audio_storage_replay_next_frame(uint8_t *sdu_buf, uint16_t *len, bool *is_eof)
{
	if (!rd_open || !sdu_buf || !len || !is_eof) {
		return -EINVAL;
	}
	if (rd_next_len == 0) {
		return -ENODATA;
	}
	memcpy(sdu_buf, rd_next, rd_next_len);
	*len = rd_next_len;
	replay_prefetch();
	*is_eof = (rd_next_len == 0);
	return 0;
}

void audio_storage_replay_close(void)
{
	if (rd_open) {
		(void)fs_close(&rd_file);
		rd_open = false;
	}
}

int audio_storage_unlink_active_replay(void)
{
	char path[PATH_MAX_LEN];
	int err;

	if (!rd_open) {
		return -EBADF;
	}
	audio_storage_replay_close();
	snprintf(path, sizeof(path), LFS_ROOT "/%s", rd_name);
	err = fs_unlink(path);
	if (err) {
		LOG_ERR("fs_unlink(%s) failed (%d)", path, err);
		return err;
	}
	LOG_INF("replayed and deleted %s", rd_name);
	refresh_pending();
	refresh_usage();   /* may release the watermark protection */
	return 0;
}

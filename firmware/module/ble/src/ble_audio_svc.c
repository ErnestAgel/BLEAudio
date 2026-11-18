/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * ble_audio_svc - Phase 1B.
 * Ported/adapted from NCS v3.4.2 samples: zephyr l2cap_coc_acceptor (server,
 * ops, accept_cb), nrf peripheral_cts_client (gatt_dm + bt_cts_*), and
 * zephyr mtu_update (bt_gatt_cb.att_mtu_updated).
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/cts_client.h>

#include "ble_audio_svc.h"
#include "sys_mgr.h"

LOG_MODULE_REGISTER(ble_audio_svc, LOG_LEVEL_INF);

#define TX_QUEUE_DEPTH    8
#define TX_POOL_COUNT     14
/* Max SDUs handed to the host's per-channel queue at once (credit-style pacing). */
#define TX_MAX_IN_FLIGHT  4
#define UTC_MIN_VALID     1700000000UL

#define NEG_STEP_TIMEOUT_MS 3000
#define NEG_STEP_GAP_MS     100

/* ------------------------------------------------------------------ UUIDs */
#define BLE_AUDIO_UUID(w1) BT_UUID_128_ENCODE(w1, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

static const struct bt_uuid_128 uuid_svc    = BT_UUID_INIT_128(BLE_AUDIO_UUID(0x6E401000));
static const struct bt_uuid_128 uuid_ctrl   = BT_UUID_INIT_128(BLE_AUDIO_UUID(0x6E401002));
static const struct bt_uuid_128 uuid_status = BT_UUID_INIT_128(BLE_AUDIO_UUID(0x6E401003));
static const struct bt_uuid_128 uuid_info   = BT_UUID_INIT_128(BLE_AUDIO_UUID(0x6E401004));

/* ------------------------------------------------------------------ state */
static struct bt_conn *cur_conn;
static K_MUTEX_DEFINE(conn_lock);

static atomic_t phy_mode = ATOMIC_INIT(1);
static atomic_t drop_cnt;
static atomic_t in_flight;
static atomic_t abort_flag;
static atomic_t l2cap_ready;

static struct k_spinlock utc_lock;
static bool utc_synced;
static uint32_t utc_base;          /* UNIX s at utc_base_up */
static uint32_t utc_base_up;       /* uptime s when utc_base was set */

static const struct bt_gatt_attr *status_attr;

K_SEM_DEFINE(sent_sem, 0, TX_MAX_IN_FLIGHT);
K_MSGQ_DEFINE(ble_tx_msgq, sizeof(struct net_buf *), TX_QUEUE_DEPTH, sizeof(void *));

/* 14 blocks > queue depth (8) + in-flight (4) + one held by ble_tx_task */
NET_BUF_POOL_DEFINE(l2cap_audio_tx_pool, TX_POOL_COUNT, BT_L2CAP_SDU_BUF_SIZE(BLE_AUDIO_MAX_SDU),
		    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static struct bt_l2cap_le_chan audio_chan;
static struct bt_cts_client cts_c;

/* ------------------------------------------------------------------ helpers */
static struct bt_conn *get_conn(void)
{
	struct bt_conn *c = NULL;

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn) {
		c = bt_conn_ref(cur_conn);
	}
	k_mutex_unlock(&conn_lock);
	return c;
}

static void drop_oldest(void)
{
	struct net_buf *old;

	if (k_msgq_get(&ble_tx_msgq, &old, K_NO_WAIT) == 0) {
		net_buf_unref(old);
		atomic_inc(&drop_cnt);
	}
}

static void purge_tx_queue(void)
{
	struct net_buf *b;

	while (k_msgq_get(&ble_tx_msgq, &b, K_NO_WAIT) == 0) {
		net_buf_unref(b);
	}
}

/* days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant) */
static int64_t days_from_civil(int64_t y, unsigned int m, unsigned int d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned int yoe = (unsigned int)(y - era * 400);
	const unsigned int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + (int64_t)doe - 719468;
}

static void utc_set(uint32_t utc)
{
	k_spinlock_key_t key = k_spin_lock(&utc_lock);

	utc_base = utc;
	utc_base_up = (uint32_t)(k_uptime_get() / 1000);
	utc_synced = true;
	k_spin_unlock(&utc_lock, key);
}

uint32_t ble_audio_svc_get_utc(bool *is_synced)
{
	uint32_t now_up = (uint32_t)(k_uptime_get() / 1000);
	k_spinlock_key_t key = k_spin_lock(&utc_lock);
	bool synced = utc_synced;
	uint32_t v = synced ? utc_base + (now_up - utc_base_up) : now_up;

	k_spin_unlock(&utc_lock, key);
	if (is_synced) {
		*is_synced = synced;
	}
	return v;
}

/* ------------------------------------------------- 6-step serial negotiation */
enum neg_step {
	NEG_PHY = 0,
	NEG_DLE,
	NEG_MTU,
	NEG_INTERVAL,
	NEG_CTS,
	NEG_L2CAP,
	NEG_DONE,
};

static struct k_work_delayable neg_work;
static atomic_t neg_next;          /* next step to run */
static atomic_t neg_waiting = ATOMIC_INIT(-1); /* step whose completion we await */
static struct bt_gatt_exchange_params mtu_params;

/* A request finished (callback) or failed fast: let the next step run soon. */
static void neg_step_done(enum neg_step step)
{
	if (atomic_cas(&neg_waiting, step, -1)) {
		k_work_reschedule(&neg_work, K_MSEC(NEG_STEP_GAP_MS));
	}
}

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
			    struct bt_gatt_exchange_params *params)
{
	if (err) {
		LOG_WRN("MTU exchange failed (ATT err 0x%02x), keeping default", err);
	}
	neg_step_done(NEG_MTU);
}

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	LOG_INF("ATT MTU updated: TX %u RX %u", tx, rx);
}

static struct bt_gatt_cb gatt_cbs = {
	.att_mtu_updated = att_mtu_updated,
};

static void cts_read_cb(struct bt_cts_client *c, struct bt_cts_current_time *t, int err)
{
	if (err) {
		LOG_WRN("CTS read failed (%d): waiting for opcode 0x05", err);
	} else {
		const struct bt_cts_exact_time_256 *e = &t->exact_time_256;

		if (e->year < 1970 || e->month < 1 || e->month > 12 || e->day < 1) {
			LOG_WRN("CTS time invalid: waiting for opcode 0x05");
		} else {
			int64_t days = days_from_civil(e->year, e->month, e->day);
			int64_t s = days * 86400 + e->hours * 3600 + e->minutes * 60 + e->seconds;

			if (s >= (int64_t)UTC_MIN_VALID) {
				utc_set((uint32_t)s); /* CTS carries local time; treated as UTC */
				LOG_INF("UTC synced from CTS: %u", (uint32_t)s);
			} else {
				LOG_WRN("CTS time out of range: waiting for opcode 0x05");
			}
		}
	}
	neg_step_done(NEG_CTS);
}

static void dm_completed(struct bt_gatt_dm *dm, void *ctx)
{
	int err = bt_cts_handles_assign(dm, &cts_c);

	bt_gatt_dm_data_release(dm);
	if (err) {
		LOG_WRN("CTS handles assign failed (%d)", err);
		neg_step_done(NEG_CTS);
		return;
	}
	err = bt_cts_read_current_time(&cts_c, cts_read_cb);
	if (err) {
		LOG_WRN("bt_cts_read_current_time failed (%d), waiting for opcode 0x05", err);
		neg_step_done(NEG_CTS);
	}
}

static void dm_not_found(struct bt_conn *conn, void *ctx)
{
	LOG_INF("Peer has no CTS (-ENOENT): waiting for opcode 0x05");
	neg_step_done(NEG_CTS);
}

static void dm_error(struct bt_conn *conn, int err, void *ctx)
{
	LOG_WRN("CTS discovery error %d: waiting for opcode 0x05", err);
	neg_step_done(NEG_CTS);
}

static const struct bt_gatt_dm_cb dm_cbs = {
	.completed = dm_completed,
	.service_not_found = dm_not_found,
	.error_found = dm_error,
};

static void neg_work_handler(struct k_work *work)
{
	struct bt_conn *conn = get_conn();
	int step = (int)atomic_get(&neg_next);
	int err = 0;

	if (!conn) {
		return;
	}

	if (atomic_get(&neg_waiting) >= 0) {
		LOG_WRN("Negotiation step %d timed out, continuing", (int)atomic_get(&neg_waiting));
	}

	atomic_set(&neg_next, step + 1);
	/* armed before the request so a fast callback is never lost */
	atomic_set(&neg_waiting, step);

	switch (step) {
	case NEG_PHY:
		err = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
		break;
	case NEG_DLE:
		err = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
		break;
	case NEG_MTU:
		mtu_params.func = mtu_exchange_cb;
		err = bt_gatt_exchange_mtu(conn, &mtu_params);
		break;
	case NEG_INTERVAL: {
		static const struct bt_le_conn_param cp = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400);

		err = bt_conn_le_param_update(conn, &cp);
		break;
	}
	case NEG_CTS:
		err = bt_gatt_dm_start(conn, BT_UUID_CTS, &dm_cbs, NULL);
		break;
	case NEG_L2CAP:
		LOG_INF("Step 6: waiting for Central to open L2CAP CoC PSM 0x%04x (max SDU %u)",
			BLE_AUDIO_L2CAP_PSM, BLE_AUDIO_MAX_SDU);
		atomic_set(&neg_waiting, -1);
		bt_conn_unref(conn);
		return;
	default:
		atomic_set(&neg_waiting, -1);
		bt_conn_unref(conn);
		return;
	}

	if (err) {
		/* -EALREADY / -EBUSY / -ENOTSUP etc: log and move on to the next step */
		LOG_WRN("Negotiation step %d request returned %d, skipping", step + 1, err);
		atomic_set(&neg_waiting, -1);
		k_work_reschedule(&neg_work, K_MSEC(NEG_STEP_GAP_MS));
	} else {
		LOG_INF("Negotiation step %d/6 requested", step + 1);
		/* if no completion callback arrives this same work runs again = timeout */
		k_work_reschedule(&neg_work, K_MSEC(NEG_STEP_TIMEOUT_MS));
	}
	bt_conn_unref(conn);
}

/* ------------------------------------------------------------ conn callbacks */
static void svc_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (err) {
		return;
	}

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn == NULL) {
		cur_conn = bt_conn_ref(conn);
	}
	k_mutex_unlock(&conn_lock);

	if (bt_conn_get_info(conn, &info) == 0 && info.le.phy) {
		atomic_set(&phy_mode, info.le.phy->tx_phy == BT_GAP_LE_PHY_2M ? 2 : 1);
	}

	atomic_set(&neg_next, NEG_PHY);
	atomic_set(&neg_waiting, -1);
	/* Let the link settle (and the Central finish service discovery) first */
	k_work_reschedule(&neg_work, K_MSEC(500));
}

static void svc_disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_work_cancel_delayable(&neg_work);

	k_mutex_lock(&conn_lock, K_FOREVER);
	if (cur_conn == conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}
	k_mutex_unlock(&conn_lock);

	atomic_set(&l2cap_ready, 0);
	atomic_set(&abort_flag, 1);
	purge_tx_queue();
	atomic_set(&in_flight, 0);
	k_sem_give(&sent_sem);
	atomic_set(&phy_mode, 1);
	sys_mgr_post_event(EVT_STATUS_CHANGED);
}

static void svc_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	atomic_set(&phy_mode, param->tx_phy == BT_GAP_LE_PHY_2M ? 2 : 1);
	if (param->tx_phy != BT_GAP_LE_PHY_2M) {
		LOG_WRN("PHY stays 1M: only ADPCM can run stably");
	}
	LOG_INF("PHY updated: tx %u rx %u", param->tx_phy, param->rx_phy);
	neg_step_done(NEG_PHY);
	sys_mgr_post_event(EVT_STATUS_CHANGED);
}

static void svc_data_len_updated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
	LOG_INF("DLE updated: tx %u/%u us, rx %u/%u us", info->tx_max_len, info->tx_max_time,
		info->rx_max_len, info->rx_max_time);
	if (info->tx_max_len < 251) {
		LOG_WRN("DLE below 251 B: throughput limited");
	}
	neg_step_done(NEG_DLE);
}

static void svc_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			      uint16_t timeout)
{
	LOG_INF("Conn params: interval %u (x1.25 ms) latency %u timeout %u", interval, latency,
		timeout);
	if (interval > 12) {
		LOG_WRN("Interval > 15 ms: relying on Drop-Oldest for bursts");
	}
	neg_step_done(NEG_INTERVAL);
}

BT_CONN_CB_DEFINE(svc_conn_cbs) = {
	.connected = svc_connected,
	.disconnected = svc_disconnected,
	.le_phy_updated = svc_phy_updated,
	.le_data_len_updated = svc_data_len_updated,
	.le_param_updated = svc_param_updated,
};

/* ---------------------------------------------------------------- GATT */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	ble_status_pkt_t st;

	sys_mgr_fill_status(&st);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &st, sizeof(st));
}

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	const ble_l2cap_info_t info = {
		.psm_le = sys_cpu_to_le16(BLE_AUDIO_L2CAP_PSM),
		.max_sdu = sys_cpu_to_le16(BLE_AUDIO_MAX_SDU),
	};

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &info, sizeof(info));
}

static ssize_t write_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *d = buf;
	enum sys_state st = sys_mgr_get_state();
	uint8_t op;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	op = d[0];

	switch (op) {
	case CTRL_OP_START_CAPTURE:
		if (len != 1) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (st == SYS_STATE_OTA) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		sys_mgr_post_ctrl(op, 0);
		break;
	case CTRL_OP_STOP_CAPTURE:
		if (len != 1) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (st != SYS_STATE_CAPTURING) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		sys_mgr_post_ctrl(op, 0);
		break;
	case CTRL_OP_SET_NR:
	case CTRL_OP_SET_CODEC:
		if (len != 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (d[1] > 1) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		if (op == CTRL_OP_SET_CODEC &&
		    (st != SYS_STATE_IDLE || d[1] != CODEC_RAW_PCM)) {
			/* Phase 1B streams Raw PCM only (ADPCM arrives with Phase 3) */
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		sys_mgr_post_ctrl(op, d[1]);
		break;
	case CTRL_OP_TRIGGER_REPLAY:
		if (len != 1) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (st == SYS_STATE_CAPTURING) {
			return BT_GATT_ERR(BT_ATT_ERR_PROCEDURE_IN_PROGRESS);
		}
		if (st != SYS_STATE_IDLE || !atomic_get(&l2cap_ready)) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		sys_mgr_post_ctrl(op, 0);
		break;
	case CTRL_OP_SET_UTC_TIME: {
		uint32_t utc;

		if (len != 5) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		utc = sys_get_le32(&d[1]);
		if (utc < UTC_MIN_VALID) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		utc_set(utc);
		LOG_INF("UTC set via 0x05: %u", utc);
		break;
	}
	default:
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	return len;
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	LOG_INF("Status notify %s", value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
	if (value == BT_GATT_CCC_NOTIFY) {
		sys_mgr_post_event(EVT_STATUS_CHANGED);
	}
}

BT_GATT_SERVICE_DEFINE(ble_audio_gatt,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),
	BT_GATT_CHARACTERISTIC(&uuid_ctrl.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_ctrl, NULL),
	BT_GATT_CHARACTERISTIC(&uuid_status.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_status, NULL, NULL),
	BT_GATT_CCC(status_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&uuid_info.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_info, NULL, NULL),
);

int ble_audio_svc_notify_status(const ble_status_pkt_t *st)
{
	struct bt_conn *conn = get_conn();
	int err = 0;

	if (!conn) {
		return -ENOTCONN;
	}
	if (status_attr && bt_gatt_is_subscribed(conn, status_attr, BT_GATT_CCC_NOTIFY)) {
		err = bt_gatt_notify(conn, status_attr, st, sizeof(*st));
		if (err) {
			LOG_DBG("status notify failed (%d)", err);
		}
	}
	bt_conn_unref(conn);
	return err;
}

/* ------------------------------------------------------------- L2CAP CoC */
static void l2cap_chan_connected(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_le_chan *le = BT_L2CAP_LE_CHAN(chan);

	atomic_set(&in_flight, 0);
	atomic_set(&abort_flag, 0);
	purge_tx_queue();
	k_sem_reset(&sent_sem);
	atomic_set(&l2cap_ready, 1);
	LOG_INF("L2CAP CoC connected: TX MTU %u MPS %u", le->tx.mtu, le->tx.mps);
	if (le->tx.mtu < BLE_AUDIO_MAX_SDU) {
		LOG_WRN("Peer RX MTU %u < %u: frames will be rejected", le->tx.mtu,
			BLE_AUDIO_MAX_SDU);
	}
	sys_mgr_post_event(EVT_STATUS_CHANGED);
}

static void l2cap_chan_disconnected(struct bt_l2cap_chan *chan)
{
	atomic_set(&l2cap_ready, 0);
	atomic_set(&abort_flag, 1);
	purge_tx_queue();
	atomic_set(&in_flight, 0);
	k_sem_give(&sent_sem);
	LOG_INF("L2CAP CoC disconnected");
	sys_mgr_post_event(EVT_STATUS_CHANGED);
}

static void l2cap_chan_sent(struct bt_l2cap_chan *chan)
{
	if (atomic_get(&in_flight) > 0) {
		atomic_dec(&in_flight);
	}
	k_sem_give(&sent_sem);
}

static int l2cap_chan_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	/* Upstream-only channel: ignore anything the Central sends. */
	LOG_DBG("CoC rx %u B ignored", buf->len);
	return 0;
}

static const struct bt_l2cap_chan_ops l2cap_ops = {
	.connected = l2cap_chan_connected,
	.disconnected = l2cap_chan_disconnected,
	.sent = l2cap_chan_sent,
	.recv = l2cap_chan_recv,
};

static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
			struct bt_l2cap_chan **chan)
{
	if (audio_chan.chan.conn != NULL) {
		LOG_WRN("CoC already in use, rejecting");
		return -ENOMEM;
	}
	memset(&audio_chan, 0, sizeof(audio_chan));
	audio_chan.chan.ops = &l2cap_ops;
	*chan = &audio_chan.chan;
	LOG_INF("L2CAP CoC connection request accepted");
	return 0;
}

static struct bt_l2cap_server audio_l2cap_server = {
	.psm = BLE_AUDIO_L2CAP_PSM,
	/* Dev phase: L1 (no pairing). Production must raise to BT_SECURITY_L2+ */
	.sec_level = BT_SECURITY_L1,
	.accept = l2cap_accept,
};

bool ble_audio_svc_is_connected(void)
{
	bool c;

	k_mutex_lock(&conn_lock, K_FOREVER);
	c = cur_conn != NULL;
	k_mutex_unlock(&conn_lock);
	return c;
}

bool ble_audio_svc_is_l2cap_ready(void)
{
	return atomic_get(&l2cap_ready) != 0;
}

uint8_t ble_audio_svc_get_phy_mode(void)
{
	return (uint8_t)atomic_get(&phy_mode);
}

uint32_t ble_audio_svc_get_drop_cnt(void)
{
	return (uint32_t)atomic_get(&drop_cnt);
}

int ble_audio_svc_enqueue_live(const uint8_t *sdu, uint16_t len)
{
	struct net_buf *buf = NULL;

	if (!sdu || len == 0 || len > BLE_AUDIO_MAX_SDU) {
		return -EINVAL;
	}
	if (!atomic_get(&l2cap_ready)) {
		return -ENOTCONN;
	}

	/* Pool exhausted -> Drop-Oldest from the queue head, then retry */
	for (int i = 0; i < 3 && !buf; i++) {
		buf = net_buf_alloc(&l2cap_audio_tx_pool, K_NO_WAIT);
		if (!buf) {
			drop_oldest();
		}
	}
	if (!buf) {
		atomic_inc(&drop_cnt);
		return -ENOMEM;
	}
	net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, sdu, len);

	/* Queue full -> Drop-Oldest, then retry */
	for (int i = 0; i < 3; i++) {
		if (k_msgq_put(&ble_tx_msgq, &buf, K_NO_WAIT) == 0) {
			return 0;
		}
		drop_oldest();
	}
	net_buf_unref(buf);
	atomic_inc(&drop_cnt);
	return -ENOBUFS;
}

int ble_audio_svc_tx_poll(k_timeout_t timeout)
{
	struct net_buf *buf;
	int err;

	if (!atomic_get(&l2cap_ready)) {
		k_sleep(timeout);
		return -ENOTCONN;
	}
	if (atomic_get(&in_flight) >= TX_MAX_IN_FLIGHT) {
		/* wait for the stack to complete an SDU before handing over more */
		(void)k_sem_take(&sent_sem, timeout);
		return -EAGAIN;
	}
	err = k_msgq_get(&ble_tx_msgq, &buf, timeout);
	if (err) {
		return -EAGAIN;
	}

	atomic_inc(&in_flight);
	err = bt_l2cap_chan_send(&audio_chan.chan, buf);
	if (err) {
		atomic_dec(&in_flight);
		net_buf_unref(buf);
		LOG_WRN("bt_l2cap_chan_send failed (%d)", err);
		return err;
	}
	return 0;
}

int ble_audio_svc_send_blocking_replay(const uint8_t *sdu, uint16_t len)
{
	struct net_buf *buf;
	int err;

	if (!sdu || len == 0 || len > BLE_AUDIO_MAX_SDU) {
		return -EINVAL;
	}
	while (atomic_get(&in_flight) >= TX_MAX_IN_FLIGHT) {
		if (atomic_get(&abort_flag) || !atomic_get(&l2cap_ready)) {
			return -ECANCELED;
		}
		(void)k_sem_take(&sent_sem, K_MSEC(50));
	}
	if (atomic_get(&abort_flag) || !atomic_get(&l2cap_ready)) {
		return -ECANCELED;
	}
	buf = net_buf_alloc(&l2cap_audio_tx_pool, K_MSEC(100));
	if (!buf) {
		return -ENOMEM;
	}
	net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, sdu, len);
	atomic_inc(&in_flight);
	err = bt_l2cap_chan_send(&audio_chan.chan, buf);
	if (err) {
		atomic_dec(&in_flight);
		net_buf_unref(buf);
	}
	return err;
}

void ble_audio_svc_abort_replay(void)
{
	atomic_set(&abort_flag, 1);
	k_sem_give(&sent_sem);
}

void ble_audio_svc_wait_all_sent(void)
{
	while (atomic_get(&in_flight) > 0 && atomic_get(&l2cap_ready)) {
		(void)k_sem_take(&sent_sem, K_MSEC(50));
	}
}

int ble_audio_svc_init(void)
{
	int err;

	k_work_init_delayable(&neg_work, neg_work_handler);
	bt_gatt_cb_register(&gatt_cbs);

	status_attr = bt_gatt_find_by_uuid(ble_audio_gatt.attrs, ble_audio_gatt.attr_count,
					   &uuid_status.uuid);
	if (!status_attr) {
		LOG_ERR("Status attribute not found");
		return -ENOENT;
	}

	err = bt_cts_client_init(&cts_c);
	if (err) {
		LOG_ERR("CTS client init failed (%d)", err);
		return err;
	}

	err = bt_l2cap_server_register(&audio_l2cap_server);
	if (err) {
		LOG_ERR("L2CAP server register failed (%d)", err);
		return err;
	}
	LOG_INF("ble_audio_svc ready: PSM 0x%04x, max SDU %u", BLE_AUDIO_L2CAP_PSM,
		BLE_AUDIO_MAX_SDU);
	return 0;
}

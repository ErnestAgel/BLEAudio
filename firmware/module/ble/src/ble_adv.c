/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/logging/log.h>

#include "ble_adv.h"
#include "hw_ui.h"

LOG_MODULE_REGISTER(ble_adv, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SMP_BT_SVC_UUID_VAL),
};

static struct k_work adv_work;
static struct k_work_delayable blink_work;
static bool connected_flag;
static bool blink_state;

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("Advertising failed to start (%d)", err);
		return;
	}
	LOG_INF("Advertising started");
}

static void blink_handler(struct k_work *work)
{
	if (connected_flag) {
		return;
	}
	blink_state = !blink_state;
	hw_ui_led_ble(blink_state);
	k_work_schedule(&blink_work, K_MSEC(500)); /* 1 Hz */
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed (0x%02x %s)", err, bt_hci_err_to_str(err));
		return;
	}
	LOG_INF("Connected");
	connected_flag = true;
	k_work_cancel_delayable(&blink_work);
	hw_ui_led_ble(true);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Disconnected (0x%02x %s)", reason, bt_hci_err_to_str(reason));
	connected_flag = false;
	k_work_schedule(&blink_work, K_NO_WAIT);
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

int ble_adv_init(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}
	LOG_INF("Bluetooth initialized");

	k_work_init(&adv_work, adv_work_handler);
	k_work_init_delayable(&blink_work, blink_handler);
	k_work_submit(&adv_work);
	k_work_schedule(&blink_work, K_NO_WAIT);
	return 0;
}

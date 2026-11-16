/* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "hw_ui.h"
#include "sys_mgr.h"

LOG_MODULE_REGISTER(hw_ui, LOG_LEVEL_INF);

#define DEBOUNCE_MS 30

static const struct gpio_dt_spec led_capture = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_ble     = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_nr_err  = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);

struct btn {
	struct gpio_dt_spec spec;
	struct gpio_callback cb;
	struct k_work_delayable work;
	void (*on_press)(void);
};

static void btn1_press(void) { sys_mgr_post_event(EVT_BTN_CAPTURE_TOGGLE); }
static void btn2_press(void) { LOG_INF("BTN2 reserved"); }
static void btn3_press(void) { sys_mgr_post_event(EVT_BTN_NR_TOGGLE); }
static void btn4_press(void) { sys_mgr_post_event(EVT_BTN_OTA_TOGGLE); }

static struct btn btns[] = {
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios), .on_press = btn1_press },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios), .on_press = btn2_press },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios), .on_press = btn3_press },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios), .on_press = btn4_press },
};

static void btn_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct btn *b = CONTAINER_OF(dwork, struct btn, work);

	if (gpio_pin_get_dt(&b->spec) > 0) {
		b->on_press();
	}
}

static void btn_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct btn *b = CONTAINER_OF(cb, struct btn, cb);

	k_work_reschedule(&b->work, K_MSEC(DEBOUNCE_MS));
}

static int led_init(const struct gpio_dt_spec *l)
{
	if (!gpio_is_ready_dt(l)) {
		return -ENODEV;
	}
	return gpio_pin_configure_dt(l, GPIO_OUTPUT_INACTIVE);
}

int hw_ui_init(void)
{
	int err;

	if (led_init(&led_capture) || led_init(&led_ble) || led_init(&led_nr_err)) {
		LOG_ERR("LED init failed");
		return -ENODEV;
	}

	for (size_t i = 0; i < ARRAY_SIZE(btns); i++) {
		struct btn *b = &btns[i];

		if (!gpio_is_ready_dt(&b->spec)) {
			return -ENODEV;
		}
		err = gpio_pin_configure_dt(&b->spec, GPIO_INPUT);
		if (err) {
			return err;
		}
		k_work_init_delayable(&b->work, btn_work_handler);
		gpio_init_callback(&b->cb, btn_isr, BIT(b->spec.pin));
		err = gpio_add_callback(b->spec.port, &b->cb);
		if (err) {
			return err;
		}
		err = gpio_pin_interrupt_configure_dt(&b->spec, GPIO_INT_EDGE_TO_ACTIVE);
		if (err) {
			return err;
		}
	}
	return 0;
}

void hw_ui_led_capture(bool on) { gpio_pin_set_dt(&led_capture, on); }
void hw_ui_led_ble(bool on)     { gpio_pin_set_dt(&led_ble, on); }
void hw_ui_led_nr(bool on)      { gpio_pin_set_dt(&led_nr_err, on); }
void hw_ui_led_error(bool on)   { gpio_pin_set_dt(&led_nr_err, on); }

/*
 * Copyright (c) 2026 Shaedil
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>

#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif

static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(DT_NODELABEL(led_r), gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_NODELABEL(led_g), gpios);
static const struct gpio_dt_spec blue = GPIO_DT_SPEC_GET(DT_NODELABEL(led_b), gpios);

static int status_led_init(void) {
    const struct gpio_dt_spec *leds[] = {&red, &green, &blue};

    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        if (!gpio_is_ready_dt(leds[i])) {
            return -ENODEV;
        }

        int err = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
        if (err) {
            return err;
        }
    }

    return 0;
}

SYS_INIT(status_led_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_FRAMEWORK_CB_STATUS_LED)

static bool connected(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_get_conn_state() == ZMK_USB_CONN_HID) {
        return true;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
    if (zmk_ble_active_profile_is_connected()) {
        return true;
    }
#endif

    return false;
}

static int status_led_listener(const zmk_event_t *eh) {
    gpio_pin_set_dt(&blue, connected());
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_led, status_led_listener);

#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(status_led, zmk_ble_active_profile_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(status_led, zmk_usb_conn_state_changed);
#endif

#endif

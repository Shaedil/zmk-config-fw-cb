/*
 * Copyright (c) 2026 Shaedil
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// S1 is a gpio-keys entry in Framework's board: pressed means ON (Bluetooth)
#define SWITCH DT_NODELABEL(protocol_switch)

// gpio-keys debounces and reports changes only, so the position at boot is read here
static const struct gpio_dt_spec sw = GPIO_DT_SPEC_GET(SWITCH, gpios);
static atomic_t bt;

static void apply(struct k_work *work) {
    bool on = atomic_get(&bt);

    LOG_INF("Mode switch: %s", on ? "Bluetooth" : "wired");

    if (on) {
        zmk_ble_set_enabled(true);
        zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE);
    } else {
        zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_USB);
        zmk_ble_set_enabled(false);
    }
}

static K_WORK_DEFINE(apply_work, apply);

static void switch_moved(struct input_event *evt, void *user_data) {
    if (evt->type != INPUT_EV_KEY || evt->code != DT_PROP(SWITCH, zephyr_code)) {
        return;
    }

    atomic_set(&bt, evt->value);
    k_work_submit(&apply_work);
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(SWITCH)), switch_moved, NULL);

static void read_switch(void) {
    int pos = gpio_pin_get_dt(&sw);

    if (pos < 0) {
        LOG_ERR("Failed to read the mode switch (%d)", pos);
        return;
    }

    atomic_set(&bt, pos);
    k_work_submit(&apply_work);
}

#if IS_ENABLED(CONFIG_SETTINGS)
// Runs after settings_load(), so the switch beats the saved endpoint
static int mode_switch_commit(void) {
    read_switch();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(framework_cb_mode_switch, "framework_cb", NULL, NULL,
                               mode_switch_commit, NULL);
#else
static int mode_switch_init(void) {
    read_switch();
    return 0;
}

SYS_INIT(mode_switch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

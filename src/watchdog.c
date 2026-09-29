/*
 * Copyright (c) 2026 Shaedil
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(framework_cb_watchdog, CONFIG_LOG_DEFAULT_LEVEL);

#define FEED_INTERVAL K_MSEC(CONFIG_FRAMEWORK_CB_WATCHDOG_TIMEOUT_MS / 4)

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdt31));
static int channel;

static void feed(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(feed_work, feed);

static void feed(struct k_work *work) {
    wdt_feed(wdt, channel);
    k_work_schedule(&feed_work, FEED_INTERVAL);
}

static int watchdog_init(void) {
    const struct wdt_timeout_cfg cfg = {
        .window.max = CONFIG_FRAMEWORK_CB_WATCHDOG_TIMEOUT_MS,
        .flags = WDT_FLAG_RESET_SOC,
    };
    int err;

    if (!device_is_ready(wdt)) {
        LOG_ERR("WDT31 not ready");
        return -ENODEV;
    }

    channel = wdt_install_timeout(wdt, &cfg);
    if (channel < 0) {
        LOG_ERR("Failed to install the watchdog timeout (%d)", channel);
        return channel;
    }

    err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (err) {
        LOG_ERR("Failed to start the watchdog (%d)", err);
        return err;
    }

    k_work_schedule(&feed_work, FEED_INTERVAL);
    return 0;
}

SYS_INIT(watchdog_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);

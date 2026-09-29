/*
 * Copyright (c) 2026 Shaedil
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT shaedil_npm1300_fuel_gauge

#include <math.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include <nrf_fuel_gauge.h>

LOG_MODULE_REGISTER(fuel_gauge, CONFIG_SENSOR_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1);

#define CHG_COMPLETE BIT(1)
#define CHG_TRICKLE BIT(2)
#define CHG_CC BIT(3)
#define CHG_CV BIT(4)

#define VBUS_PRESENT BIT(0)
#define VBUS_LIMITED BIT(1)

#define BUSY_INTERVAL K_SECONDS(1)
#define IDLE_INTERVAL K_SECONDS(10)
#define BUSY_CURRENT_A 0.02f
#define SAVE_INTERVAL_MS (60 * 60 * 1000)
#define STACK_SIZE 3072

struct sample {
    float v;
    float i;
    float t;
    int32_t chg;
    int32_t vbus;
};

static const struct battery_model model = {
#include "battery_model.inc"
};

static const struct device *const charger = DEVICE_DT_GET(DT_INST_PHANDLE(0, charger));

static atomic_t soc = ATOMIC_INIT(-1);

static uint8_t state[1024];
static size_t state_len;
static K_SEM_DEFINE(settings_loaded, 0, 1);

static float get(enum sensor_channel chan) {
    struct sensor_value val;

    sensor_channel_get(charger, chan, &val);
    return sensor_value_to_float(&val);
}

static int measure(struct sample *s) {
    struct sensor_value val;
    int err = sensor_sample_fetch(charger);

    if (err) {
        return err;
    }

    s->v = get(SENSOR_CHAN_GAUGE_VOLTAGE);
    s->t = get(SENSOR_CHAN_GAUGE_TEMP);
    // nRF Fuel Gauge counts discharge as positive
    s->i = -get(SENSOR_CHAN_GAUGE_AVG_CURRENT);

    sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &val);
    s->chg = val.val1;
    sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS, &val);
    s->vbus = val.val1;

    return 0;
}

static enum nrf_fuel_gauge_charge_state charge_state(const struct sample *s) {
    if (s->chg & CHG_COMPLETE) {
        return NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
    }
    if (s->chg & CHG_TRICKLE) {
        return NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
    }
    if (s->chg & CHG_CC) {
        return (s->vbus & VBUS_LIMITED) ? NRF_FUEL_GAUGE_CHARGE_STATE_CC_LIMITED
                                        : NRF_FUEL_GAUGE_CHARGE_STATE_CC;
    }
    if (s->chg & CHG_CV) {
        return NRF_FUEL_GAUGE_CHARGE_STATE_CV;
    }
    return NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
}

static void report_vbus(const struct sample *s) {
    nrf_fuel_gauge_ext_state_update((s->vbus & VBUS_PRESENT)
                                        ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
                                        : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED,
                                    NULL);
}

static void report_charge_state(const struct sample *s) {
    nrf_fuel_gauge_ext_state_update(
        NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE,
        &(union nrf_fuel_gauge_ext_state_info_data){.charge_state = charge_state(s)});
}

static int start(struct sample *s) {
    int err = measure(s);

    if (err) {
        return err;
    }

    struct nrf_fuel_gauge_init_parameters params =
        NRF_FUEL_GAUGE_DEFAULT_INIT_PARAMETERS_SECONDARY(s->v, s->i, s->t, &model);

    if (state_len == nrf_fuel_gauge_state_size) {
        params.state = state;
        params.state_size = state_len;
    }

    err = nrf_fuel_gauge_init(&params, NULL);
    if (err && params.state) {
        params.state = NULL;
        params.state_size = 0;
        err = nrf_fuel_gauge_init(&params, NULL);
    }
    if (err) {
        return err;
    }

    float limit = get(SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT);

    nrf_fuel_gauge_ext_state_update(
        NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
        &(union nrf_fuel_gauge_ext_state_info_data){.charge_current_limit = limit});
    nrf_fuel_gauge_ext_state_update(
        NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
        &(union nrf_fuel_gauge_ext_state_info_data){.charge_term_current = limit / 10.0f});
    report_vbus(s);
    report_charge_state(s);

    return 0;
}

static void save(void) {
    if (nrf_fuel_gauge_state_get(state, sizeof(state)) == 0) {
        settings_save_one("fuel_gauge/state", state, nrf_fuel_gauge_state_size);
    }
}

static void run(void) {
    struct sample prev;
    struct sample s;
    int err;

    if (!device_is_ready(charger)) {
        LOG_ERR("Charger not ready");
        return;
    }

    if (IS_ENABLED(CONFIG_SETTINGS)) {
        k_sem_take(&settings_loaded, K_SECONDS(10));
    }

    while ((err = start(&prev))) {
        LOG_DBG("Failed to start the fuel gauge (%d)", err);
        k_sleep(IDLE_INTERVAL);
    }

    int64_t last = k_uptime_get();
    int64_t saved = last;

    for (;;) {
        bool busy = (prev.vbus & VBUS_PRESENT) || fabsf(prev.i) >= BUSY_CURRENT_A;

        k_sleep(busy ? BUSY_INTERVAL : IDLE_INTERVAL);

        if (measure(&s)) {
            continue;
        }

        if ((s.vbus ^ prev.vbus) & VBUS_PRESENT) {
            report_vbus(&s);
        }
        if (s.chg != prev.chg || ((s.vbus ^ prev.vbus) & VBUS_LIMITED)) {
            report_charge_state(&s);
        }

        float pct;
        float dt = (float)k_uptime_delta(&last) / 1000.0f;

        if (nrf_fuel_gauge_process(s.v, s.i, s.t, dt, &pct, NULL) == 0) {
            atomic_set(&soc, lroundf(CLAMP(pct, 0.0f, 100.0f)));
        }

        if (IS_ENABLED(CONFIG_SETTINGS) && last - saved >= SAVE_INTERVAL_MS) {
            save();
            saved = last;
        }

        prev = s;
    }
}

K_THREAD_DEFINE(fuel_gauge_thread, STACK_SIZE, run, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

#if IS_ENABLED(CONFIG_SETTINGS)
static int state_set(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (!settings_name_steq(name, "state", NULL) || len > sizeof(state)) {
        return -ENOENT;
    }

    ssize_t n = read(arg, state, len);

    state_len = n > 0 ? n : 0;
    return 0;
}

static int state_commit(void) {
    k_sem_give(&settings_loaded);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(fuel_gauge, "fuel_gauge", NULL, state_set, state_commit, NULL);
#endif

static int fuel_gauge_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    return atomic_get(&soc) < 0 ? -EAGAIN : 0;
}

static int fuel_gauge_channel_get(const struct device *dev, enum sensor_channel chan,
                                  struct sensor_value *val) {
    int pct = atomic_get(&soc);

    if (chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
        return -ENOTSUP;
    }
    if (pct < 0) {
        return -EAGAIN;
    }

    val->val1 = pct;
    val->val2 = 0;
    return 0;
}

static DEVICE_API(sensor, fuel_gauge_api) = {
    .sample_fetch = fuel_gauge_sample_fetch,
    .channel_get = fuel_gauge_channel_get,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
                      &fuel_gauge_api);

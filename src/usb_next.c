/*
 * Copyright (c) 2026 Shaedil
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid.h>
#include <zmk/usb.h>
#include <zmk/usb_hid.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
#include <zmk/pointing/resolution_multipliers.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_POWER_MA 500

USBD_DEVICE_DEFINE(zmk_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), CONFIG_USB_DEVICE_VID,
                   CONFIG_USB_DEVICE_PID);

USBD_DESC_LANG_DEFINE(zmk_lang);
USBD_DESC_MANUFACTURER_DEFINE(zmk_mfr, CONFIG_USB_DEVICE_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(zmk_product, CONFIG_USB_DEVICE_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(zmk_sn);))

USBD_DESC_CONFIG_DEFINE(zmk_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(zmk_hs_cfg_desc, "HS Configuration");
USBD_CONFIGURATION_DEFINE(zmk_fs_config, USB_SCD_REMOTE_WAKEUP, MAX_POWER_MA / 2, &zmk_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(zmk_hs_config, USB_SCD_REMOTE_WAKEUP, MAX_POWER_MA / 2, &zmk_hs_cfg_desc);

static const struct device *const hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);

static atomic_t conn_state = ATOMIC_INIT(ZMK_USB_CONN_NONE);
static atomic_t hid_ready;

static void raise_conn_state_changed(struct k_work *work) {
    raise_zmk_usb_conn_state_changed(
        (struct zmk_usb_conn_state_changed){.conn_state = zmk_usb_get_conn_state()});
}

static K_WORK_DEFINE(conn_state_work, raise_conn_state_changed);

static void set_conn_state(enum zmk_usb_conn_state state) {
    if (atomic_set(&conn_state, state) != state) {
        k_work_submit(&conn_state_work);
    }
}

enum zmk_usb_conn_state zmk_usb_get_conn_state(void) {
    return (enum zmk_usb_conn_state)atomic_get(&conn_state);
}

bool zmk_usb_is_hid_ready(void) {
    return zmk_usb_get_conn_state() == ZMK_USB_CONN_HID && atomic_get(&hid_ready);
}

#define TX_BUF_COUNT CONFIG_USBD_HID_IN_BUF_COUNT
#define TX_BUF_SIZE 64
#define TX_BUF_STRIDE ROUND_UP(TX_BUF_SIZE, MAX(UDC_BUF_ALIGN, UDC_BUF_GRANULARITY))

BUILD_ASSERT(sizeof(struct zmk_hid_keyboard_report) <= TX_BUF_SIZE);
BUILD_ASSERT(sizeof(struct zmk_hid_consumer_report) <= TX_BUF_SIZE);
#if IS_ENABLED(CONFIG_ZMK_POINTING)
BUILD_ASSERT(sizeof(struct zmk_hid_mouse_report) <= TX_BUF_SIZE);
#endif

static uint8_t __aligned(UDC_BUF_ALIGN) tx_bufs[TX_BUF_COUNT][TX_BUF_STRIDE];
static unsigned int tx_next;
static K_SEM_DEFINE(tx_free, TX_BUF_COUNT, TX_BUF_COUNT);
static K_MUTEX_DEFINE(tx_lock);

static int send_report(const void *report, size_t len) {
    struct usbd_context *const usbd = &zmk_usbd;
    int err;

    if (usbd_is_suspended(usbd)) {
        return usbd_wakeup_request(usbd);
    }

    if (!zmk_usb_is_hid_ready()) {
        return -ENODEV;
    }

    k_mutex_lock(&tx_lock, K_FOREVER);

    if (k_sem_take(&tx_free, K_MSEC(30)) != 0) {
        k_mutex_unlock(&tx_lock);
        return -EBUSY;
    }

    memcpy(tx_bufs[tx_next], report, len);
    err = hid_device_submit_report(hid_dev, len, tx_bufs[tx_next]);
    if (err) {
        k_sem_give(&tx_free);
    } else {
        tx_next = (tx_next + 1) % TX_BUF_COUNT;
    }

    k_mutex_unlock(&tx_lock);
    return err;
}

int zmk_usb_hid_send_keyboard_report(void) {
    return send_report(zmk_hid_get_keyboard_report(), sizeof(struct zmk_hid_keyboard_report));
}

int zmk_usb_hid_send_consumer_report(void) {
    return send_report(zmk_hid_get_consumer_report(), sizeof(struct zmk_hid_consumer_report));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_usb_hid_send_mouse_report(void) {
    return send_report(zmk_hid_get_mouse_report(), sizeof(struct zmk_hid_mouse_report));
}
#endif

static const struct zmk_endpoint_instance usb_endpoint __maybe_unused = {
    .transport = ZMK_TRANSPORT_USB,
};

static void iface_ready(const struct device *dev, const bool ready) {
    atomic_set(&hid_ready, ready);
    k_work_submit(&conn_state_work);
}

static void input_report_done(const struct device *dev, const uint8_t *const report) {
    k_sem_give(&tx_free);
}

static int get_report(const struct device *dev, const uint8_t type, const uint8_t id,
                      const uint16_t len, uint8_t *const buf) {
    const void *report;
    size_t size;

    switch (type) {
    case HID_REPORT_TYPE_INPUT:
        switch (id) {
        case ZMK_HID_REPORT_ID_KEYBOARD:
            report = zmk_hid_get_keyboard_report();
            size = sizeof(struct zmk_hid_keyboard_report);
            break;
        case ZMK_HID_REPORT_ID_CONSUMER:
            report = zmk_hid_get_consumer_report();
            size = sizeof(struct zmk_hid_consumer_report);
            break;
        default:
            LOG_ERR("Invalid report ID %d requested", id);
            return -EINVAL;
        }
        break;

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
    case HID_REPORT_TYPE_FEATURE: {
        static struct zmk_hid_mouse_resolution_feature_report res_report;
        struct zmk_pointing_resolution_multipliers mult;

        if (id != ZMK_HID_REPORT_ID_MOUSE) {
            return -ENOTSUP;
        }

        mult = zmk_pointing_resolution_multipliers_get_profile(usb_endpoint);
        res_report.report_id = ZMK_HID_REPORT_ID_MOUSE;
        res_report.body.wheel_res = mult.wheel;
        res_report.body.hwheel_res = mult.hor_wheel;
        report = &res_report;
        size = sizeof(res_report);
        break;
    }
#endif

    default:
        return -ENOTSUP;
    }

    size = MIN(size, len);
    memcpy(buf, report, size);
    return size;
}

static int set_report(const struct device *dev, const uint8_t type, const uint8_t id,
                      const uint16_t len, const uint8_t *const buf) {
    switch (type) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    case HID_REPORT_TYPE_OUTPUT: {
        struct zmk_hid_led_report report;

        if (id != ZMK_HID_REPORT_ID_LEDS || len != sizeof(report)) {
            LOG_ERR("LED set report is malformed: id=%d length=%d", id, len);
            return -EINVAL;
        }

        memcpy(&report, buf, sizeof(report));
        zmk_hid_indicators_process_report(&report.body, usb_endpoint);
        return 0;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING_SMOOTH_SCROLLING)
    case HID_REPORT_TYPE_FEATURE: {
        struct zmk_hid_mouse_resolution_feature_report report;

        if (id != ZMK_HID_REPORT_ID_MOUSE || len != sizeof(report)) {
            return -EINVAL;
        }

        memcpy(&report, buf, sizeof(report));
        zmk_pointing_resolution_multipliers_process_report(&report.body, usb_endpoint);
        return 0;
    }
#endif

    default:
        return -ENOTSUP;
    }
}

static const struct hid_device_ops hid_ops = {
    .iface_ready = iface_ready,
    .get_report = get_report,
    .set_report = set_report,
    .input_report_done = input_report_done,
};

static void msg_cb(struct usbd_context *const usbd, const struct usbd_msg *const msg) {
    switch (msg->type) {
    case USBD_MSG_VBUS_READY:
        set_conn_state(ZMK_USB_CONN_POWERED);
        if (usbd_can_detect_vbus(usbd) && usbd_enable(usbd)) {
            LOG_ERR("Failed to enable USB");
        }
        break;
    case USBD_MSG_VBUS_REMOVED:
        if (usbd_can_detect_vbus(usbd) && usbd_disable(usbd)) {
            LOG_ERR("Failed to disable USB");
        }
        set_conn_state(ZMK_USB_CONN_NONE);
        break;
    case USBD_MSG_RESET:
        set_conn_state(ZMK_USB_CONN_POWERED);
        break;
    case USBD_MSG_CONFIGURATION:
        set_conn_state(msg->status ? ZMK_USB_CONN_HID : ZMK_USB_CONN_POWERED);
        break;
    default:
        break;
    }
}

static int add_configuration(struct usbd_context *usbd, enum usbd_speed speed,
                             struct usbd_config_node *config) {
    int err;

    err = usbd_add_configuration(usbd, speed, config);
    if (err) {
        return err;
    }

    err = usbd_register_all_classes(usbd, speed, 1, NULL);
    if (err) {
        return err;
    }

    if (IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)) {
        return usbd_device_set_code_triple(usbd, speed, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
    }

    return usbd_device_set_code_triple(usbd, speed, 0, 0, 0);
}

static int zmk_usb_init(void) {
    struct usbd_context *const usbd = &zmk_usbd;
    int err;

    if (!device_is_ready(hid_dev)) {
        LOG_ERR("HID device is not ready");
        return -ENODEV;
    }

    err = hid_device_register(hid_dev, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &hid_ops);
    if (err) {
        LOG_ERR("Failed to register the HID device (%d)", err);
        return err;
    }

    err = usbd_add_descriptor(usbd, &zmk_lang);
    err = err ?: usbd_add_descriptor(usbd, &zmk_mfr);
    err = err ?: usbd_add_descriptor(usbd, &zmk_product);
    IF_ENABLED(CONFIG_HWINFO, (err = err ?: usbd_add_descriptor(usbd, &zmk_sn);))
    if (err) {
        LOG_ERR("Failed to add USB string descriptors (%d)", err);
        return err;
    }

    if (USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(usbd) == USBD_SPEED_HS) {
        err = add_configuration(usbd, USBD_SPEED_HS, &zmk_hs_config);
        if (err) {
            LOG_ERR("Failed to add the high-speed configuration (%d)", err);
            return err;
        }
    }

    err = add_configuration(usbd, USBD_SPEED_FS, &zmk_fs_config);
    if (err) {
        LOG_ERR("Failed to add the full-speed configuration (%d)", err);
        return err;
    }

    err = usbd_msg_register_cb(usbd, msg_cb);
    err = err ?: usbd_init(usbd);
    if (err) {
        LOG_ERR("Failed to initialize USB (%d)", err);
        return err;
    }

    if (!usbd_can_detect_vbus(usbd)) {
        err = usbd_enable(usbd);
        if (err) {
            LOG_ERR("Failed to enable USB (%d)", err);
            return err;
        }
    }

    return 0;
}

SYS_INIT(zmk_usb_init, APPLICATION, CONFIG_ZMK_USB_INIT_PRIORITY);

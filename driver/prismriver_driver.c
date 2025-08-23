// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Sony / PS2 / PS3 / PS4 BD devices.
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 *  Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 *  Copyright (c) 2008 Jiri Slaby
 *  Copyright (c) 2012 David Dillow <dave@thedillows.org>
 *  Copyright (c) 2006-2013 Jiri Kosina
 *  Copyright (c) 2013 Colin Leitner <colin.leitner@gmail.com>
 *  Copyright (c) 2014-2016 Frank Praznik <frank.praznik@gmail.com>
 *  Copyright (c) 2018 Todd Kelner
 *  Copyright (c) 2020-2021 Pascal Giard <pascal.giard@etsmtl.ca>
 *  Copyright (c) 2020 Sanjay Govind <sanjay.govind9@gmail.com>
 *  Copyright (c) 2021 Daniel Nguyen <daniel.nguyen.1@ens.etsmtl.ca>
 */

/*
 */

/*
 * NOTE: in order for the Sony PS3 BD Remote Control to be found by
 * a Bluetooth host, the key combination Start+Enter has to be kept pressed
 * for about 7 seconds with the Bluetooth Host Controller in discovering mode.
 *
 * There will be no PIN request from the device.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/idr.h>
#include <linux/input/mt.h>
#include <linux/crc32.h>
#include <linux/usb.h>
#include <linux/timer.h>
#include <linux/unaligned.h>

#include "hid-ids.h"

#define GH_GUITAR_CONTROLLER      BIT(14)

#define MAX_LEDS 4
#define GUITAR_TILT_USAGE 44

static DEFINE_SPINLOCK(sony_dev_list_lock);
static LIST_HEAD(sony_device_list);
static DEFINE_IDA(sony_device_id_allocator);

enum sony_worker {
	SONY_WORKER_STATE
};

struct sony_sc {
	spinlock_t lock;
	struct list_head list_node;
	struct hid_device *hdev;
	struct input_dev *touchpad;
	struct input_dev *sensor_dev;
	struct led_classdev *leds[MAX_LEDS];
	unsigned long quirks;
	struct work_struct state_worker;
	void (*send_output_report)(struct sony_sc *);
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	int device_id;
	u8 *output_report_dmabuf;

#ifdef CONFIG_SONY_FF
	u8 left;
	u8 right;
#endif

	u8 mac_address[6];
	u8 state_worker_initialized;
	u8 defer_initialization;
	u8 battery_capacity;
	int battery_status;
	u8 led_state[MAX_LEDS];
	u8 led_delay_on[MAX_LEDS];
	u8 led_delay_off[MAX_LEDS];
	u8 led_count;
};

static inline void sony_schedule_work(struct sony_sc *sc,
				      enum sony_worker which)
{
	unsigned long flags;

	switch (which) {
	case SONY_WORKER_STATE:
		spin_lock_irqsave(&sc->lock, flags);
		if (!sc->defer_initialization && sc->state_worker_initialized)
			schedule_work(&sc->state_worker);
		spin_unlock_irqrestore(&sc->lock, flags);
		break;
	}
}

static int guitar_mapping(struct hid_device *hdev, struct hid_input *hi,
			  struct hid_field *field, struct hid_usage *usage,
			  unsigned long **bit, int *max)
{
	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_MSVENDOR) {
		unsigned int abs = usage->hid & HID_USAGE;

		if (abs == GUITAR_TILT_USAGE) {
			hid_map_usage_clear(hi, usage, bit, max, EV_ABS, ABS_RY);
			return 1;
		}
	}
	return 0;
}

static void sony_state_worker(struct work_struct *work)
{
	struct sony_sc *sc = container_of(work, struct sony_sc, state_worker);

	sc->send_output_report(sc);
}

static int sony_allocate_output_report(struct sony_sc *sc)
{
	return 0;
}

static void sony_remove_dev_list(struct sony_sc *sc)
{
	unsigned long flags;

	if (sc->list_node.next) {
		spin_lock_irqsave(&sony_dev_list_lock, flags);
		list_del(&(sc->list_node));
		spin_unlock_irqrestore(&sony_dev_list_lock, flags);
	}
}

static int sony_check_add(struct sony_sc *sc)
{

		return 0;

}

static int sony_set_device_id(struct sony_sc *sc)
{
		sc->device_id = -1;

	return 0;
}

static void sony_release_device_id(struct sony_sc *sc)
{
	if (sc->device_id >= 0) {
		ida_free(&sony_device_id_allocator, sc->device_id);
		sc->device_id = -1;
	}
}

static inline void sony_init_output_report(struct sony_sc *sc,
				void (*send_output_report)(struct sony_sc *))
{
	sc->send_output_report = send_output_report;

	if (!sc->state_worker_initialized)
		INIT_WORK(&sc->state_worker, sony_state_worker);

	sc->state_worker_initialized = 1;
}

static inline void sony_cancel_work_sync(struct sony_sc *sc)
{
	unsigned long flags;

	if (sc->state_worker_initialized) {
		spin_lock_irqsave(&sc->lock, flags);
		sc->state_worker_initialized = 0;
		spin_unlock_irqrestore(&sc->lock, flags);
		cancel_work_sync(&sc->state_worker);
	}
}

static int sony_input_configured(struct hid_device *hdev,
					struct hid_input *hidinput)
{
	struct sony_sc *sc = hid_get_drvdata(hdev);
	int append_dev_id;
	int ret;

	ret = sony_set_device_id(sc);
	if (ret < 0) {
		hid_err(hdev, "failed to allocate the device id\n");
		goto err_stop;
	}

	ret = append_dev_id = sony_check_add(sc);
	if (ret < 0)
		goto err_stop;

	ret = sony_allocate_output_report(sc);
	if (ret < 0) {
		hid_err(hdev, "failed to allocate the output report buffer\n");
		goto err_stop;
	}

	return 0;
err_stop:
	sony_cancel_work_sync(sc);
	sony_remove_dev_list(sc);
	sony_release_device_id(sc);
	return ret;
}

static int sony_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	unsigned long quirks = id->driver_data;
	struct sony_sc *sc;
	unsigned int connect_mask = HID_CONNECT_DEFAULT;

  printk("Versão 5\n");

	sc = devm_kzalloc(&hdev->dev, sizeof(*sc), GFP_KERNEL);
	if (sc == NULL) {
		hid_err(hdev, "can't alloc sony descriptor\n");
		return -ENOMEM;
	}

	spin_lock_init(&sc->lock);

	sc->quirks = quirks;
	hid_set_drvdata(hdev, sc);
	sc->hdev = hdev;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	ret = hid_hw_start(hdev, connect_mask);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		return ret;
	}

	/* sony_input_configured can fail, but this doesn't result
	 * in hid_hw_start failures (intended). Check whether
	 * the HID layer claimed the device else fail.
	 * We don't know the actual reason for the failure, most
	 * likely it is due to EEXIST in case of double connection
	 * of USB and Bluetooth, but could have been due to ENOMEM
	 * or other reasons as well.
	 */
	if (!(hdev->claimed & HID_CLAIMED_INPUT)) {
		hid_err(hdev, "failed to claim input\n");
		ret = -ENODEV;
		goto err;
	}

	return ret;

err:
	hid_hw_stop(hdev);
	return ret;
}

static void sony_remove(struct hid_device *hdev)
{
	struct sony_sc *sc = hid_get_drvdata(hdev);

	hid_hw_close(hdev);
	sony_cancel_work_sync(sc);
	sony_remove_dev_list(sc);
	sony_release_device_id(sc);
	hid_hw_stop(hdev);
}

static const struct hid_device_id sony_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_SONY_RHYTHM, USB_DEVICE_ID_SONY_PS3_GUITAR_DONGLE),
		.driver_data = GH_GUITAR_CONTROLLER },
	{ }
};
MODULE_DEVICE_TABLE(hid, sony_devices);

static struct hid_driver sony_driver = {
	.name             = "sony",
	.id_table         = sony_devices,
	.input_mapping    = guitar_mapping,
	.input_configured = sony_input_configured,
	.probe            = sony_probe,
	.remove           = sony_remove,
};

static int __init sony_init(void)
{
	dbg_hid("Sony:%s\n", __func__);

	return hid_register_driver(&sony_driver);
}

static void __exit sony_exit(void)
{
	dbg_hid("Sony:%s\n", __func__);

	hid_unregister_driver(&sony_driver);
	ida_destroy(&sony_device_id_allocator);
}
module_init(sony_init);
module_exit(sony_exit);

MODULE_DESCRIPTION("HID driver for Sony / PS2 / PS3 / PS4 BD devices");
MODULE_LICENSE("GPL");

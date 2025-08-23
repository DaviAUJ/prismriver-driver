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

#define SIXAXIS_CONTROLLER_USB    BIT(1)
#define SIXAXIS_CONTROLLER_BT     BIT(2)
#define GH_GUITAR_CONTROLLER      BIT(14)

#define SIXAXIS_CONTROLLER (SIXAXIS_CONTROLLER_USB | SIXAXIS_CONTROLLER_BT)
#define SONY_LED_SUPPORT (SIXAXIS_CONTROLLER)
#define SONY_BATTERY_SUPPORT (SIXAXIS_CONTROLLER)
#define SONY_FF_SUPPORT (SIXAXIS_CONTROLLER)
#define SONY_BT_DEVICE (SIXAXIS_CONTROLLER_BT)

#define MAX_LEDS 4
#define GUITAR_TILT_USAGE 44

static const unsigned int sixaxis_absmap[] = {
	[0x30] = ABS_X,
	[0x31] = ABS_Y,
	[0x32] = ABS_RX, /* right stick X */
	[0x35] = ABS_RY, /* right stick Y */
};

static const unsigned int sixaxis_keymap[] = {
	[0x01] = BTN_SELECT, /* Select */
	[0x02] = BTN_THUMBL, /* L3 */
	[0x03] = BTN_THUMBR, /* R3 */
	[0x04] = BTN_START, /* Start */
	[0x05] = BTN_DPAD_UP, /* Up */
	[0x06] = BTN_DPAD_RIGHT, /* Right */
	[0x07] = BTN_DPAD_DOWN, /* Down */
	[0x08] = BTN_DPAD_LEFT, /* Left */
	[0x09] = BTN_TL2, /* L2 */
	[0x0a] = BTN_TR2, /* R2 */
	[0x0b] = BTN_TL, /* L1 */
	[0x0c] = BTN_TR, /* R1 */
	[0x0d] = BTN_NORTH, /* Triangle */
	[0x0e] = BTN_EAST, /* Circle */
	[0x0f] = BTN_SOUTH, /* Cross */
	[0x10] = BTN_WEST, /* Square */
	[0x11] = BTN_MODE, /* PS */
};

static enum power_supply_property sony_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_STATUS,
};

struct sixaxis_led {
	u8 time_enabled; /* the total time the led is active (0xff means forever) */
	u8 duty_length;  /* how long a cycle is in deciseconds (0 means "really fast") */
	u8 enabled;
	u8 duty_off; /* % of duty_length the led is off (0xff means 100%) */
	u8 duty_on;  /* % of duty_length the led is on (0xff mean 100%) */
} __packed;

struct sixaxis_rumble {
	u8 padding;
	u8 right_duration; /* Right motor duration (0xff means forever) */
	u8 right_motor_on; /* Right (small) motor on/off, only supports values of 0 or 1 (off/on) */
	u8 left_duration;    /* Left motor duration (0xff means forever) */
	u8 left_motor_force; /* left (large) motor, supports force values from 0 to 255 */
} __packed;

struct sixaxis_output_report {
	u8 report_id;
	struct sixaxis_rumble rumble;
	u8 padding[4];
	u8 leds_bitmap; /* bitmap of enabled LEDs: LED_1 = 0x02, LED_2 = 0x04, ... */
	struct sixaxis_led led[4];    /* LEDx at (4 - x) */
	struct sixaxis_led _reserved; /* LED5, not actually soldered */
} __packed;

union sixaxis_output_report_01 {
	struct sixaxis_output_report data;
	u8 buf[36];
};

struct motion_output_report_02 {
	u8 type, zero;
	u8 r, g, b;
	u8 zero2;
	u8 rumble;
};

#define SIXAXIS_REPORT_0xF2_SIZE 17
#define SIXAXIS_REPORT_0xF5_SIZE 8
#define MOTION_REPORT_0x02_SIZE 49

#define SENSOR_SUFFIX " Motion Sensors"
#define TOUCHPAD_SUFFIX " Touchpad"

#define SIXAXIS_INPUT_REPORT_ACC_X_OFFSET 41
#define SIXAXIS_ACC_RES_PER_G 113

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

static void sony_set_leds(struct sony_sc *sc);

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

static const u8 *sony_report_fixup(struct hid_device *hdev, u8 *rdesc,
		unsigned int *rsize)
{
	struct sony_sc *sc = hid_get_drvdata(hdev);

	if ((sc->quirks & SIXAXIS_CONTROLLER_USB) && *rsize >= 45 &&
		/* Report Count (13) */
		rdesc[23] == 0x95 && rdesc[24] == 0x0D &&
		/* Usage Maximum (13) */
		rdesc[37] == 0x29 && rdesc[38] == 0x0D &&
		/* Report Count (3) */
		rdesc[43] == 0x95 && rdesc[44] == 0x03) {
		hid_info(hdev, "Fixing up USB dongle report descriptor\n");
		rdesc[24] = 0x10;
		rdesc[38] = 0x10;
		rdesc[44] = 0x00;
	}

	return rdesc;
}

static void sixaxis_parse_report(struct sony_sc *sc, u8 *rd, int size)
{
	static const u8 sixaxis_battery_capacity[] = { 0, 1, 25, 50, 75, 100 };
	unsigned long flags;
	u8 battery_capacity;
	int battery_status;

	if (rd[30] >= 0xee) {
		battery_capacity = 100;
		battery_status = (rd[30] & 0x01) ? POWER_SUPPLY_STATUS_FULL : POWER_SUPPLY_STATUS_CHARGING;
	} else {
		u8 index = rd[30] <= 5 ? rd[30] : 5;
		battery_capacity = sixaxis_battery_capacity[index];
		battery_status = POWER_SUPPLY_STATUS_DISCHARGING;
	}

	spin_lock_irqsave(&sc->lock, flags);
	sc->battery_capacity = battery_capacity;
	sc->battery_status = battery_status;
	spin_unlock_irqrestore(&sc->lock, flags);

	if (sc->quirks & SIXAXIS_CONTROLLER) {
		int val;


		int offset = SIXAXIS_INPUT_REPORT_ACC_X_OFFSET;
		val = ((rd[offset+1] << 8) | rd[offset]) - 511;
		input_report_abs(sc->sensor_dev, ABS_X, val);

		/* Y and Z are swapped and inversed */
		val = 511 - ((rd[offset+35] << 8) | rd[offset+4]);
		input_report_abs(sc->sensor_dev, ABS_Y, val);

		val = 511 - ((rd[offset+3] << 8) | rd[offset+2]);
		input_report_abs(sc->sensor_dev, ABS_Z, val);

		input_sync(sc->sensor_dev);
	}
}

static int sony_raw_event(struct hid_device *hdev, struct hid_report *report,
		u8 *rd, int size)
{
	struct sony_sc *sc = hid_get_drvdata(hdev);

	/*
	 * Sixaxis HID report has acclerometers/gyro with MSByte first, this
	 * has to be BYTE_SWAPPED before passing up to joystick interface
	 */
	if ((sc->quirks & SIXAXIS_CONTROLLER) && rd[0] == 0x01 && size == 49) {
		/*
		 * When connected via Bluetooth the Sixaxis occasionally sends
		 * a report with the second byte 0xff and the rest zeroed.
		 *
		 * This report does not reflect the actual state of the
		 * controller must be ignored to avoid generating false input
		 * events.
		 */
		if (rd[1] == 0xff)
			return -EINVAL;

		swap(rd[41], rd[42]);
		swap(rd[43], rd[44]);
		swap(rd[45], rd[46]);
		swap(rd[47], rd[48]);

		sixaxis_parse_report(sc, rd, size);
	}

	if (sc->defer_initialization) {
		sc->defer_initialization = 0;
		sony_schedule_work(sc, SONY_WORKER_STATE);
	}

	return 0;
}

static int sony_register_sensors(struct sony_sc *sc)
{
	size_t name_sz;
	char *name;
	int ret;

	sc->sensor_dev = devm_input_allocate_device(&sc->hdev->dev);
	if (!sc->sensor_dev)
		return -ENOMEM;

	input_set_drvdata(sc->sensor_dev, sc);
	sc->sensor_dev->dev.parent = &sc->hdev->dev;
	sc->sensor_dev->phys = sc->hdev->phys;
	sc->sensor_dev->uniq = sc->hdev->uniq;
	sc->sensor_dev->id.bustype = sc->hdev->bus;
	sc->sensor_dev->id.vendor = sc->hdev->vendor;
	sc->sensor_dev->id.product = sc->hdev->product;
	sc->sensor_dev->id.version = sc->hdev->version;

	/* Append a suffix to the controller name as there are various
	 * DS4 compatible non-Sony devices with different names.
	 */
	name_sz = strlen(sc->hdev->name) + sizeof(SENSOR_SUFFIX);
	name = devm_kzalloc(&sc->hdev->dev, name_sz, GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	snprintf(name, name_sz, "%s" SENSOR_SUFFIX, sc->hdev->name);
	sc->sensor_dev->name = name;

	if (sc->quirks & SIXAXIS_CONTROLLER) {
		/* For the DS3 we only support the accelerometer, which works
		 * quite well even without calibration. The device also has
		 * a 1-axis gyro, but it is very difficult to manage from within
		 * the driver even to get data, the sensor is inaccurate and
		 * the behavior is very different between hardware revisions.
		 */
		input_set_abs_params(sc->sensor_dev, ABS_X, -512, 511, 4, 0);
		input_set_abs_params(sc->sensor_dev, ABS_Y, -512, 511, 4, 0);
		input_set_abs_params(sc->sensor_dev, ABS_Z, -512, 511, 4, 0);
		input_abs_set_res(sc->sensor_dev, ABS_X, SIXAXIS_ACC_RES_PER_G);
		input_abs_set_res(sc->sensor_dev, ABS_Y, SIXAXIS_ACC_RES_PER_G);
		input_abs_set_res(sc->sensor_dev, ABS_Z, SIXAXIS_ACC_RES_PER_G);
	}

	__set_bit(INPUT_PROP_ACCELEROMETER, sc->sensor_dev->propbit);

	ret = input_register_device(sc->sensor_dev);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * Sending HID_REQ_GET_REPORT changes the operation mode of the ps3 controller
 * to "operational".  Without this, the ps3 controller will not report any
 * events.
 */
static int sixaxis_set_operational_usb(struct hid_device *hdev)
{
	const int buf_size =
		max(SIXAXIS_REPORT_0xF2_SIZE, SIXAXIS_REPORT_0xF5_SIZE);
	u8 *buf;
	int ret;

	buf = kmalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, 0xf2, buf, SIXAXIS_REPORT_0xF2_SIZE,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_err(hdev, "can't set operational mode: step 1\n");
		goto out;
	}

	/*
	 * Some compatible controllers like the Speedlink Strike FX and
	 * Gasia need another query plus an USB interrupt to get operational.
	 */
	ret = hid_hw_raw_request(hdev, 0xf5, buf, SIXAXIS_REPORT_0xF5_SIZE,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_err(hdev, "can't set operational mode: step 2\n");
		goto out;
	}

	ret = hid_hw_output_report(hdev, buf, 1);
	if (ret < 0) {
		hid_info(hdev, "can't set operational mode: step 3, ignoring\n");
		ret = 0;
	}

out:
	kfree(buf);

	return ret;
}

static int sixaxis_set_operational_bt(struct hid_device *hdev)
{
	static const u8 report[] = { 0xf4, 0x42, 0x03, 0x00, 0x00 };
	u8 *buf;
	int ret;

	buf = kmemdup(report, sizeof(report), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, buf[0], buf, sizeof(report),
				  HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	kfree(buf);

	return ret;
}

static void sixaxis_set_leds_from_id(struct sony_sc *sc)
{
	static const u8 sixaxis_leds[10][4] = {
				{ 0x01, 0x00, 0x00, 0x00 },
				{ 0x00, 0x01, 0x00, 0x00 },
				{ 0x00, 0x00, 0x01, 0x00 },
				{ 0x00, 0x00, 0x00, 0x01 },
				{ 0x01, 0x00, 0x00, 0x01 },
				{ 0x00, 0x01, 0x00, 0x01 },
				{ 0x00, 0x00, 0x01, 0x01 },
				{ 0x01, 0x00, 0x01, 0x01 },
				{ 0x00, 0x01, 0x01, 0x01 },
				{ 0x01, 0x01, 0x01, 0x01 }
	};

	int id = sc->device_id;

	BUILD_BUG_ON(MAX_LEDS < ARRAY_SIZE(sixaxis_leds[0]));

	if (id < 0)
		return;

	id %= 10;
	memcpy(sc->led_state, sixaxis_leds[id], sizeof(sixaxis_leds[id]));
}

static void buzz_set_leds(struct sony_sc *sc)
{
	struct hid_device *hdev = sc->hdev;
	struct list_head *report_list =
		&hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	struct hid_report *report = list_entry(report_list->next,
		struct hid_report, list);
	s32 *value = report->field[0]->value;

	BUILD_BUG_ON(MAX_LEDS < 4);

	value[0] = 0x00;
	value[1] = sc->led_state[0] ? 0xff : 0x00;
	value[2] = sc->led_state[1] ? 0xff : 0x00;
	value[3] = sc->led_state[2] ? 0xff : 0x00;
	value[4] = sc->led_state[3] ? 0xff : 0x00;
	value[5] = 0x00;
	value[6] = 0x00;
	hid_hw_request(hdev, report, HID_REQ_SET_REPORT);
}

static void sony_set_leds(struct sony_sc *sc)
{
		buzz_set_leds(sc);
}

static void sony_led_set_brightness(struct led_classdev *led,
				    enum led_brightness value)
{
	struct device *dev = led->dev->parent;
	struct hid_device *hdev = to_hid_device(dev);
	struct sony_sc *drv_data;

	int n;
	int force_update;

	drv_data = hid_get_drvdata(hdev);
	if (!drv_data) {
		hid_err(hdev, "No device data\n");
		return;
	}

	/*
	 * The Sixaxis on USB will override any LED settings sent to it
	 * and keep flashing all of the LEDs until the PS button is pressed.
	 * Updates, even if redundant, must be always be sent to the
	 * controller to avoid having to toggle the state of an LED just to
	 * stop the flashing later on.
	 */
	force_update = !!(drv_data->quirks & SIXAXIS_CONTROLLER_USB);

	for (n = 0; n < drv_data->led_count; n++) {
		if (led == drv_data->leds[n] && (force_update ||
			(value != drv_data->led_state[n] ||
			drv_data->led_delay_on[n] ||
			drv_data->led_delay_off[n]))) {

			drv_data->led_state[n] = value;

			/* Setting the brightness stops the blinking */
			drv_data->led_delay_on[n] = 0;
			drv_data->led_delay_off[n] = 0;

			sony_set_leds(drv_data);
			break;
		}
	}
}

static enum led_brightness sony_led_get_brightness(struct led_classdev *led)
{
	struct device *dev = led->dev->parent;
	struct hid_device *hdev = to_hid_device(dev);
	struct sony_sc *drv_data;

	int n;

	drv_data = hid_get_drvdata(hdev);
	if (!drv_data) {
		hid_err(hdev, "No device data\n");
		return LED_OFF;
	}

	for (n = 0; n < drv_data->led_count; n++) {
		if (led == drv_data->leds[n])
			return drv_data->led_state[n];
	}

	return LED_OFF;
}

static int sony_led_blink_set(struct led_classdev *led, unsigned long *delay_on,
				unsigned long *delay_off)
{
	struct device *dev = led->dev->parent;
	struct hid_device *hdev = to_hid_device(dev);
	struct sony_sc *drv_data = hid_get_drvdata(hdev);
	int n;
	u8 new_on, new_off;

	if (!drv_data) {
		hid_err(hdev, "No device data\n");
		return -EINVAL;
	}

	/* Max delay is 255 deciseconds or 2550 milliseconds */
	if (*delay_on > 2550)
		*delay_on = 2550;
	if (*delay_off > 2550)
		*delay_off = 2550;

	/* Blink at 1 Hz if both values are zero */
	if (!*delay_on && !*delay_off)
		*delay_on = *delay_off = 500;

	new_on = *delay_on / 10;
	new_off = *delay_off / 10;

	for (n = 0; n < drv_data->led_count; n++) {
		if (led == drv_data->leds[n])
			break;
	}

	/* This LED is not registered on this device */
	if (n >= drv_data->led_count)
		return -EINVAL;

	/* Don't schedule work if the values didn't change */
	if (new_on != drv_data->led_delay_on[n] ||
		new_off != drv_data->led_delay_off[n]) {
		drv_data->led_delay_on[n] = new_on;
		drv_data->led_delay_off[n] = new_off;
		sony_schedule_work(drv_data, SONY_WORKER_STATE);
	}

	return 0;
}

static int sony_leds_init(struct sony_sc *sc)
{
	struct hid_device *hdev = sc->hdev;
	int n, ret = 0;
	int use_color_names;
	struct led_classdev *led;
	size_t name_sz;
	char *name;
	size_t name_len;
	const char *name_fmt;
	static const char * const color_name_str[] = { "red", "green", "blue",
						  "global" };
	u8 max_brightness[MAX_LEDS] = { [0 ... (MAX_LEDS - 1)] = 1 };
	u8 use_hw_blink[MAX_LEDS] = { 0 };

	if (WARN_ON(!(sc->quirks & SONY_LED_SUPPORT))) {
		return -EINVAL;
  }

  sixaxis_set_leds_from_id(sc);
  sc->led_count = 4;
  memset(use_hw_blink, 1, 4);
  use_color_names = 0;
  name_len = strlen("::sony#");
  name_fmt = "%s::sony%d";

	/*
	 * Clear LEDs as we have no way of reading their initial state. This is
	 * only relevant if the driver is loaded after somebody actively set the
	 * LEDs to on
	 */
	sony_set_leds(sc);

	name_sz = strlen(dev_name(&hdev->dev)) + name_len + 1;

	for (n = 0; n < sc->led_count; n++) {

		if (use_color_names)
			name_sz = strlen(dev_name(&hdev->dev)) + strlen(color_name_str[n]) + 2;

		led = devm_kzalloc(&hdev->dev, sizeof(struct led_classdev) + name_sz, GFP_KERNEL);
		if (!led) {
			hid_err(hdev, "Couldn't allocate memory for LED %d\n", n);
			return -ENOMEM;
		}

		name = (void *)(&led[1]);
		if (use_color_names)
			snprintf(name, name_sz, name_fmt, dev_name(&hdev->dev),
			color_name_str[n]);
		else
			snprintf(name, name_sz, name_fmt, dev_name(&hdev->dev), n + 1);
		led->name = name;
		led->brightness = sc->led_state[n];
		led->max_brightness = max_brightness[n];
		led->flags = LED_CORE_SUSPENDRESUME;
		led->brightness_get = sony_led_get_brightness;
		led->brightness_set = sony_led_set_brightness;

		if (use_hw_blink[n])
			led->blink_set = sony_led_blink_set;

		sc->leds[n] = led;

		ret = devm_led_classdev_register(&hdev->dev, led);
		if (ret) {
			hid_err(hdev, "Failed to register LED %d\n", n);
			return ret;
		}
	}

	return 0;
}

static void sixaxis_send_output_report(struct sony_sc *sc)
{
	static const union sixaxis_output_report_01 default_report = {
		.buf = {
			0x01,
			0x01, 0xff, 0x00, 0xff, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00,
			0xff, 0x27, 0x10, 0x00, 0x32,
			0xff, 0x27, 0x10, 0x00, 0x32,
			0xff, 0x27, 0x10, 0x00, 0x32,
			0xff, 0x27, 0x10, 0x00, 0x32,
			0x00, 0x00, 0x00, 0x00, 0x00
		}
	};
	struct sixaxis_output_report *report =
		(struct sixaxis_output_report *)sc->output_report_dmabuf;
	int n;

	/* Initialize the report with default values */
	memcpy(report, &default_report, sizeof(struct sixaxis_output_report));

#ifdef CONFIG_SONY_FF
	report->rumble.right_motor_on = sc->right ? 1 : 0;
	report->rumble.left_motor_force = sc->left;
#endif

	report->leds_bitmap |= sc->led_state[0] << 1;
	report->leds_bitmap |= sc->led_state[1] << 2;
	report->leds_bitmap |= sc->led_state[2] << 3;
	report->leds_bitmap |= sc->led_state[3] << 4;

	/* Set flag for all leds off, required for 3rd party INTEC controller */
	if ((report->leds_bitmap & 0x1E) == 0)
		report->leds_bitmap |= 0x20;

	/*
	 * The LEDs in the report are indexed in reverse order to their
	 * corresponding light on the controller.
	 * Index 0 = LED 4, index 1 = LED 3, etc...
	 *
	 * In the case of both delay values being zero (blinking disabled) the
	 * default report values should be used or the controller LED will be
	 * always off.
	 */
	for (n = 0; n < 4; n++) {
		if (sc->led_delay_on[n] || sc->led_delay_off[n]) {
			report->led[3 - n].duty_off = sc->led_delay_off[n];
			report->led[3 - n].duty_on = sc->led_delay_on[n];
		}
	}
		hid_hw_raw_request(sc->hdev, report->report_id, (u8 *)report,
				sizeof(struct sixaxis_output_report),
				HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
}

#ifdef CONFIG_SONY_FF
static inline void sony_send_output_report(struct sony_sc *sc)
{
	if (sc->send_output_report)
		sc->send_output_report(sc);
}
#endif

static void sony_state_worker(struct work_struct *work)
{
	struct sony_sc *sc = container_of(work, struct sony_sc, state_worker);

	sc->send_output_report(sc);
}

static int sony_allocate_output_report(struct sony_sc *sc)
{
	if(sc->quirks & SIXAXIS_CONTROLLER)
		sc->output_report_dmabuf =
			devm_kmalloc(&sc->hdev->dev,
				sizeof(union sixaxis_output_report_01),
				GFP_KERNEL);
	else
		return 0;

	if (!sc->output_report_dmabuf)
		return -ENOMEM;

	return 0;
}

#ifdef CONFIG_SONY_FF
static int sony_play_effect(struct input_dev *dev, void *data,
			    struct ff_effect *effect)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct sony_sc *sc = hid_get_drvdata(hid);

	if (effect->type != FF_RUMBLE)
		return 0;

	sc->left = effect->u.rumble.strong_magnitude / 256;
	sc->right = effect->u.rumble.weak_magnitude / 256;

	sony_schedule_work(sc, SONY_WORKER_STATE);
	return 0;
}

static int sony_init_ff(struct sony_sc *sc)
{
	struct hid_input *hidinput;
	struct input_dev *input_dev;

	if (list_empty(&sc->hdev->inputs)) {
		hid_err(sc->hdev, "no inputs found\n");
		return -ENODEV;
	}
	hidinput = list_entry(sc->hdev->inputs.next, struct hid_input, list);
	input_dev = hidinput->input;

	input_set_capability(input_dev, EV_FF, FF_RUMBLE);
	return input_ff_create_memless(input_dev, NULL, sony_play_effect);
}

#else
static int sony_init_ff(struct sony_sc *sc)
{
	return 0;
}

#endif

static int sony_battery_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct sony_sc *sc = power_supply_get_drvdata(psy);
	unsigned long flags;
	int ret = 0;
	u8 battery_capacity;
	int battery_status;

	spin_lock_irqsave(&sc->lock, flags);
	battery_capacity = sc->battery_capacity;
	battery_status = sc->battery_status;
	spin_unlock_irqrestore(&sc->lock, flags);

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = battery_capacity;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = battery_status;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int sony_battery_probe(struct sony_sc *sc, int append_dev_id)
{
	const char *battery_str_fmt = append_dev_id ?
		"sony_controller_battery_%pMR_%i" :
		"sony_controller_battery_%pMR";
	struct power_supply_config psy_cfg = { .drv_data = sc, };
	struct hid_device *hdev = sc->hdev;
	int ret;

	/*
	 * Set the default battery level to 100% to avoid low battery warnings
	 * if the battery is polled before the first device report is received.
	 */
	sc->battery_capacity = 100;

	sc->battery_desc.properties = sony_battery_props;
	sc->battery_desc.num_properties = ARRAY_SIZE(sony_battery_props);
	sc->battery_desc.get_property = sony_battery_get_property;
	sc->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	sc->battery_desc.use_for_apm = 0;
	sc->battery_desc.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
					  battery_str_fmt, sc->mac_address, sc->device_id);
	if (!sc->battery_desc.name)
		return -ENOMEM;

	sc->battery = devm_power_supply_register(&hdev->dev, &sc->battery_desc,
					    &psy_cfg);
	if (IS_ERR(sc->battery)) {
		ret = PTR_ERR(sc->battery);
		hid_err(hdev, "Unable to register battery device\n");
		return ret;
	}

	power_supply_powers(sc->battery, &hdev->dev);
	return 0;
}

/*
 * If a controller is plugged in via USB while already connected via Bluetooth
 * it will show up as two devices. A global list of connected controllers and
 * their MAC addresses is maintained to ensure that a device is only connected
 * once.
 *
 * Some USB-only devices masquerade as Sixaxis controllers and all have the
 * same dummy Bluetooth address, so a comparison of the connection type is
 * required.  Devices are only rejected in the case where two devices have
 * matching Bluetooth addresses on different bus types.
 */
static inline int sony_compare_connection_type(struct sony_sc *sc0,
						struct sony_sc *sc1)
{
	const int sc0_not_bt = !(sc0->quirks & SONY_BT_DEVICE);
	const int sc1_not_bt = !(sc1->quirks & SONY_BT_DEVICE);

	return sc0_not_bt == sc1_not_bt;
}

static int sony_check_add_dev_list(struct sony_sc *sc)
{
	struct sony_sc *entry;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&sony_dev_list_lock, flags);

	list_for_each_entry(entry, &sony_device_list, list_node) {
		ret = memcmp(sc->mac_address, entry->mac_address,
				sizeof(sc->mac_address));
		if (!ret) {
			if (sony_compare_connection_type(sc, entry)) {
				ret = 1;
			} else {
				ret = -EEXIST;
				hid_info(sc->hdev,
				"controller with MAC address %pMR already connected\n",
				sc->mac_address);
			}
			goto unlock;
		}
	}

	ret = 0;
	list_add(&(sc->list_node), &sony_device_list);

unlock:
	spin_unlock_irqrestore(&sony_dev_list_lock, flags);
	return ret;
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

static int sony_get_bt_devaddr(struct sony_sc *sc)
{
	int ret;

	/* HIDP stores the device MAC address as a string in the uniq field. */
	ret = strlen(sc->hdev->uniq);
	if (ret != 17)
		return -EINVAL;

	ret = sscanf(sc->hdev->uniq,
		"%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
		&sc->mac_address[5], &sc->mac_address[4], &sc->mac_address[3],
		&sc->mac_address[2], &sc->mac_address[1], &sc->mac_address[0]);

	if (ret != 6)
		return -EINVAL;

	return 0;
}

static int sony_check_add(struct sony_sc *sc)
{
	u8 *buf = NULL;
	int n, ret;

	if ((sc->quirks & SIXAXIS_CONTROLLER_BT)) {
		/*
		 * sony_get_bt_devaddr() attempts to parse the Bluetooth MAC
		 * address from the uniq string where HIDP stores it.
		 * As uniq cannot be guaranteed to be a MAC address in all cases
		 * a failure of this function should not prevent the connection.
		 */
		if (sony_get_bt_devaddr(sc) < 0) {
			hid_warn(sc->hdev, "UNIQ does not contain a MAC address; duplicate check skipped\n");
			return 0;
		}
	} else if (sc->quirks & SIXAXIS_CONTROLLER_USB) {
		buf = kmalloc(SIXAXIS_REPORT_0xF2_SIZE, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		/*
		 * The MAC address of a Sixaxis controller connected via USB can
		 * be retrieved with feature report 0xf2. The address begins at
		 * offset 4.
		 */
		ret = hid_hw_raw_request(sc->hdev, 0xf2, buf,
				SIXAXIS_REPORT_0xF2_SIZE, HID_FEATURE_REPORT,
				HID_REQ_GET_REPORT);

		if (ret != SIXAXIS_REPORT_0xF2_SIZE) {
			hid_err(sc->hdev, "failed to retrieve feature report 0xf2 with the Sixaxis MAC address\n");
			ret = ret < 0 ? ret : -EINVAL;
			goto out_free;
		}

		/*
		 * The Sixaxis device MAC in the report is big-endian and must
		 * be byte-swapped.
		 */
		for (n = 0; n < 6; n++)
			sc->mac_address[5-n] = buf[4+n];

		snprintf(sc->hdev->uniq, sizeof(sc->hdev->uniq),
			 "%pMR", sc->mac_address);
	} else {
		return 0;
	}

	ret = sony_check_add_dev_list(sc);

out_free:

	kfree(buf);

	return ret;
}

static int sony_set_device_id(struct sony_sc *sc)
{
	int ret;

	/*
	 * Only Sixaxis controllers get an id.
	 * All others are set to -1.
	 */
	if (sc->quirks & SIXAXIS_CONTROLLER) {
		ret = ida_alloc(&sony_device_id_allocator, GFP_KERNEL);
		if (ret < 0) {
			sc->device_id = -1;
			return ret;
		}
		sc->device_id = ret;
	} else {
		sc->device_id = -1;
	}

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

	if (sc->quirks & SIXAXIS_CONTROLLER_USB) {
		/*
		 * The Sony Sixaxis does not handle HID Output Reports on the
		 * Interrupt EP and the device only becomes active when the
		 * PS button is pressed. See comment for Navigation controller
		 * above for more details.
		 */
		hdev->quirks |= HID_QUIRK_NO_OUTPUT_REPORTS_ON_INTR_EP;
		hdev->quirks |= HID_QUIRK_SKIP_OUTPUT_REPORT_ID;
		sc->defer_initialization = 1;

		ret = sixaxis_set_operational_usb(hdev);
		if (ret < 0) {
			hid_err(hdev, "Failed to set controller into operational mode\n");
			goto err_stop;
		}

		ret = sony_register_sensors(sc);
		if (ret) {
			hid_err(sc->hdev,
			"Unable to initialize motion sensors: %d\n", ret);
			goto err_stop;
		}

		sony_init_output_report(sc, sixaxis_send_output_report);
	} else if (sc->quirks & SIXAXIS_CONTROLLER_BT) {
		/*
		 * The Sixaxis wants output reports sent on the ctrl endpoint
		 * when connected via Bluetooth.
		 */
		hdev->quirks |= HID_QUIRK_NO_OUTPUT_REPORTS_ON_INTR_EP;

		ret = sixaxis_set_operational_bt(hdev);
		if (ret < 0) {
			hid_err(hdev, "Failed to set controller into operational mode\n");
			goto err_stop;
		}

		ret = sony_register_sensors(sc);
		if (ret) {
			hid_err(sc->hdev,
			"Unable to initialize motion sensors: %d\n", ret);
			goto err_stop;
		}

		sony_init_output_report(sc, sixaxis_send_output_report);
	} 

	if (sc->quirks & SONY_LED_SUPPORT) {
		ret = sony_leds_init(sc);
		if (ret < 0)
			goto err_stop;
	}

	if (sc->quirks & SONY_BATTERY_SUPPORT) {
		ret = sony_battery_probe(sc, append_dev_id);
		if (ret < 0)
			goto err_stop;

		/* Open the device to receive reports with battery info */
		ret = hid_hw_open(hdev);
		if (ret < 0) {
			hid_err(hdev, "hw open failed\n");
			goto err_stop;
		}
	}

	if (sc->quirks & SONY_FF_SUPPORT) {
		ret = sony_init_ff(sc);
		if (ret < 0)
			goto err_close;
	}

	return 0;
err_close:
	hid_hw_close(hdev);
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

	sc = devm_kzalloc(&hdev->dev, sizeof(*sc), GFP_KERNEL);
	if (sc == NULL) {
		hid_err(hdev, "can't alloc sony descriptor\n");
		return -ENOMEM;
	}

	spin_lock_init(&sc->lock);

	sc->quirks = quirks;
  printk("quirks: %lu\n", quirks);
	hid_set_drvdata(hdev, sc);
	sc->hdev = hdev;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	if (sc->quirks & SIXAXIS_CONTROLLER)
		connect_mask |= HID_CONNECT_HIDDEV_FORCE;

	/* Patch the hw version on DS3 compatible devices, so applications can
	 * distinguish between the default HID mappings and the mappings defined
	 * by the Linux game controller spec. This is important for the SDL2
	 * library, which has a game controller database, which uses device ids
	 * in combination with version as a key.
	 */
	if (sc->quirks & SIXAXIS_CONTROLLER)
		hdev->version |= 0x8000;

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

#ifdef CONFIG_PM

static int sony_suspend(struct hid_device *hdev, pm_message_t message)
{
#ifdef CONFIG_SONY_FF

	/* On suspend stop any running force-feedback events */
	if (SONY_FF_SUPPORT) {
		struct sony_sc *sc = hid_get_drvdata(hdev);

		sc->left = sc->right = 0;
		sony_send_output_report(sc);
	}

#endif
	return 0;
}

static int sony_resume(struct hid_device *hdev)
{
	struct sony_sc *sc = hid_get_drvdata(hdev);

	/*
	 * The Sixaxis and navigation controllers on USB need to be
	 * reinitialized on resume or they won't behave properly.
	 */
	if (sc->quirks & SIXAXIS_CONTROLLER_USB) {
		sixaxis_set_operational_usb(sc->hdev);
		sc->defer_initialization = 1;
	}

	return 0;
}

#endif

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
	.report_fixup     = sony_report_fixup,
	.raw_event        = sony_raw_event,

#ifdef CONFIG_PM
	.suspend          = sony_suspend,
	.resume	          = sony_resume,
	.reset_resume     = sony_resume,
#endif
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

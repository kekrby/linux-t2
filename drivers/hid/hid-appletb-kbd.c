// SPDX-License-Identifier: GPL-2.0
/*
 * Apple Touch Bar Keyboard Mode Driver
 *
 * Copyright (c) 2017-2018 Ronald Tschalär
 * Copyright (c) 2022-2023 Kerem Karabay <kekrby@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/bits.h>
#include <linux/input.h>
#include <linux/sysfs.h>
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/cleanup.h>
#include <linux/input/sparse-keymap.h>

#include "hid-ids.h"

#define APPLETB_KBD_MODE_ESC	0
#define APPLETB_KBD_MODE_FN	1
#define APPLETB_KBD_MODE_SPCL	2
#define APPLETB_KBD_MODE_OFF	3
#define APPLETB_KBD_MODE_MAX	APPLETB_KBD_MODE_OFF

#define HID_USAGE_MODE		0x00ff0004

#define APPLETB_KBD_QUIRK_IS_T1	BIT(0)

struct appletb_kbd {
	struct hid_field *mode_field;

	unsigned long quirks;

	u8 saved_mode;
	u8 current_mode;
};

static const struct key_entry appletb_kbd_keymap[] = {
	{ KE_KEY, KEY_ESC, { KEY_ESC } },
	{ KE_KEY, KEY_F1,  { KEY_BRIGHTNESSDOWN } },
	{ KE_KEY, KEY_F2,  { KEY_BRIGHTNESSUP } },
	{ KE_KEY, KEY_F3,  { KEY_RESERVED } },
	{ KE_KEY, KEY_F4,  { KEY_RESERVED } },
	{ KE_KEY, KEY_F5,  { KEY_KBDILLUMDOWN } },
	{ KE_KEY, KEY_F6,  { KEY_KBDILLUMUP } },
	{ KE_KEY, KEY_F7,  { KEY_PREVIOUSSONG } },
	{ KE_KEY, KEY_F8,  { KEY_PLAYPAUSE } },
	{ KE_KEY, KEY_F9,  { KEY_NEXTSONG } },
	{ KE_KEY, KEY_F10, { KEY_MUTE } },
	{ KE_KEY, KEY_F11, { KEY_VOLUMEDOWN } },
	{ KE_KEY, KEY_F12, { KEY_VOLUMEUP } },
	{ KE_END, 0 }
};

static int appletb_kbd_set_mode(struct appletb_kbd *kbd, u8 mode)
{
	struct hid_report *report = kbd->mode_field->report;
	struct hid_device *hdev = report->device;
	int ret;

	if (mode > APPLETB_KBD_MODE_MAX)
		return -EINVAL;

	ret = hid_hw_power(hdev, PM_HINT_FULLON);
	if (ret) {
		hid_err(hdev, "Device didn't resume (%pe)\n", ERR_PTR(ret));
		return ret;
	}

	if (kbd->quirks & APPLETB_KBD_QUIRK_IS_T1) {
		/*
		 * While the mode functionality is listed as a valid HID report in the usb
		 * interface descriptor, on a T1 it's not sent that way. Instead it's sent with
		 * different request-type and without a leading report-id in the data. Hence
		 * we need to send it as a custom usb control message rather via any of the
		 * standard hid_hw_*request() functions.
		 */
		void *buf __free(kfree) = kmemdup(&mode, sizeof(mode), GFP_KERNEL);
		struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
		u8 ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
		struct usb_device *udev = interface_to_usbdev(intf);
		int tries = 0;

		if (!buf) {
			ret = -ENOMEM;
			goto power_normal;
		}

		do {
			ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0), HID_REQ_SET_REPORT,
					      USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
					      (report->type + 1) << 8 | report->id,
					      ifnum, buf, sizeof(mode), 2000);
			if (ret != -EPIPE)
				break;

			usleep_range(1000 << tries, 3000 << tries);
		} while (++tries < 5);
	} else {
		kbd->mode_field->value[0] = mode;
		hid_hw_request(hdev, report, HID_REQ_SET_REPORT);
	}

power_normal:
	hid_hw_power(hdev, PM_HINT_NORMAL);

	if (ret < 0) {
		hid_err(hdev, "Failed to set mode to %u (%pe)\n", mode, ERR_PTR(ret));
		return ret;
	}

	kbd->current_mode = mode;

	return 0;
}

static ssize_t mode_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct appletb_kbd *kbd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", kbd->current_mode);
}

static ssize_t mode_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t size)
{
	struct appletb_kbd *kbd = dev_get_drvdata(dev);
	u8 mode;
	int ret;

	ret = kstrtou8(buf, 0, &mode);
	if (ret)
		return ret;

	ret = appletb_kbd_set_mode(kbd, mode);

	return ret < 0 ? ret : size;
}
static DEVICE_ATTR_RW(mode);

struct attribute *appletb_kbd_attrs[] = {
	&dev_attr_mode.attr,
	NULL
};
ATTRIBUTE_GROUPS(appletb_kbd);

static int appletb_tb_key_to_slot(unsigned int code)
{
	switch (code) {
	case KEY_ESC:
		return 0;
	case KEY_F1 ... KEY_F10:
		return code - KEY_F1 + 1;
	case KEY_F11 ... KEY_F12:
		return code - KEY_F11 + 11;
	default:
		return -EINVAL;
	}
}

static int appletb_kbd_hid_event(struct hid_device *hdev, struct hid_field *field,
				      struct hid_usage *usage, __s32 value)
{
	struct appletb_kbd *kbd = hid_get_drvdata(hdev);
	struct key_entry *translation;
	struct input_dev *input;
	int slot;

	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_KEYBOARD || usage->type != EV_KEY)
		return 0;

	input = field->hidinput->input;

	/*
	 * Skip non-touch-bar keys.
	 *
	 * Either the touch bar itself or usbhid generate a slew of key-down
	 * events for all the meta keys. None of which we're at all interested
	 * in.
	 */
	slot = appletb_tb_key_to_slot(usage->code);
	if (slot < 0)
		return 0;

	translation = sparse_keymap_entry_from_scancode(input, usage->code);

	if (translation && kbd->current_mode == APPLETB_KBD_MODE_SPCL) {
		input_event(input, usage->type, translation->keycode, value);

		return 1;
	}

	return kbd->current_mode == APPLETB_KBD_MODE_OFF;
}

static int appletb_kbd_input_configured(struct hid_device *hdev, struct hid_input *hidinput)
{
	struct input_dev *input = hidinput->input;

	/*
	 * Clear various input capabilities that are blindly set by the hid
	 * driver (usbkbd.c)
	 */
	memset(input->evbit, 0, sizeof(input->evbit));
	memset(input->keybit, 0, sizeof(input->keybit));
	memset(input->ledbit, 0, sizeof(input->ledbit));

	__set_bit(EV_REP, input->evbit);

	return sparse_keymap_setup(input, appletb_kbd_keymap, NULL);
}

static int appletb_kbd_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct appletb_kbd *kbd;
	struct device *dev = &hdev->dev;
	struct hid_field *mode_field;
	unsigned long quirks;
	int ret;

	quirks = id->driver_data;

	if (quirks & APPLETB_KBD_QUIRK_IS_T1 && !hid_is_usb(hdev))
		return -ENODEV;

	ret = hid_parse(hdev);
	if (ret)
		return dev_err_probe(dev, ret, "HID parse failed\n");

	mode_field = hid_find_field(hdev, HID_OUTPUT_REPORT,
				    HID_GD_KEYBOARD, HID_USAGE_MODE);
	if (!mode_field)
		return -ENODEV;

	kbd = devm_kzalloc(dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd)
		return -ENOMEM;

	kbd->quirks = quirks;
	kbd->mode_field = mode_field;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDINPUT);
	if (ret)
		return dev_err_probe(dev, ret, "HID hw start failed\n");

	ret = hid_hw_open(hdev);
	if (ret) {
		dev_err_probe(dev, ret, "HID hw open failed\n");
		goto stop_hw;
	}

	ret = appletb_kbd_set_mode(kbd, APPLETB_KBD_MODE_OFF);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to set touchbar mode\n");
		goto close_hw;
	}

	hid_set_drvdata(hdev, kbd);

	return 0;

close_hw:
	hid_hw_close(hdev);
stop_hw:
	hid_hw_stop(hdev);
	return ret;
}

static void appletb_kbd_remove(struct hid_device *hdev)
{
	struct appletb_kbd *kbd = hid_get_drvdata(hdev);

	appletb_kbd_set_mode(kbd, APPLETB_KBD_MODE_OFF);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

#ifdef CONFIG_PM
static int appletb_kbd_suspend(struct hid_device *hdev, pm_message_t msg)
{
	struct appletb_kbd *kbd = hid_get_drvdata(hdev);

	kbd->saved_mode = kbd->current_mode;
	appletb_kbd_set_mode(kbd, APPLETB_KBD_MODE_OFF);

	return 0;
}

static int appletb_kbd_reset_resume(struct hid_device *hdev)
{
	struct appletb_kbd *kbd = hid_get_drvdata(hdev);

	appletb_kbd_set_mode(kbd, kbd->saved_mode);

	return 0;
}
#endif

static const struct hid_device_id appletb_kbd_hid_ids[] = {
	/* MacBook Pro's 2016, 2017, with T1 chip */
	{ HID_DEVICE(BUS_USB, HID_GROUP_GENERIC,
		     USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_IBRIDGE),
		.driver_data = APPLETB_KBD_QUIRK_IS_T1 },
	/* MacBook Pro's 2018, 2019, with T2 chip */
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_TOUCHBAR_DISPLAY) },
	{ }
};
MODULE_DEVICE_TABLE(hid, appletb_kbd_hid_ids);

static struct hid_driver appletb_kbd_hid_driver = {
	.name = "hid-appletb-kbd",
	.id_table = appletb_kbd_hid_ids,
	.probe = appletb_kbd_probe,
	.remove = appletb_kbd_remove,
	.event = appletb_kbd_hid_event,
	.input_configured = appletb_kbd_input_configured,
#ifdef CONFIG_PM
	.suspend = appletb_kbd_suspend,
	.reset_resume = appletb_kbd_reset_resume,
#endif
	.driver.dev_groups = appletb_kbd_groups,
};
module_hid_driver(appletb_kbd_hid_driver);

MODULE_AUTHOR("Ronald Tschalär");
MODULE_AUTHOR("Kerem Karabay <kekrby@gmail.com>");
MODULE_DESCRIPTION("Apple Touch Bar Keyboard Mode Driver");
MODULE_LICENSE("GPL");

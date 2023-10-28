// SPDX-License-Identifier: GPL-2.0
/*
 * Apple iBridge Driver
 *
 * Copyright (c) 2018 Ronald Tschalär
 * Copyright (c) 2023 Kerem Karabay
 */

/**
 * DOC: Overview
 *
 * 2016 and 2017 MacBookPro models with a Touch Bar (MacBookPro13,[23] and
 * MacBookPro14,[23]) have an Apple iBridge chip (also known as T1 chip) which
 * exposes the Touch Bar, built-in webcam (iSight), ambient light sensor, and
 * Secure Enclave Processor (SEP) for TouchID. It shows up in the system as a
 * USB device with 3 configurations: 'Default iBridge Interfaces', 'Default
 * iBridge Interfaces(OS X)', and 'Default iBridge Interfaces(Recovery)'.
 *
 * The device exposes the Touch Bar and ALS through multiple HID interfaces.
 * However, one of the interfaces contains functionality (HID reports) for both
 * the Touch Bar backlight and the ALS, which is an issue because the kernel
 * allows only one driver to be attached to a given device. This driver exists
 * to solve this problem.
 *
 * This driver is implemented as a HID driver that attaches to the problematic
 * HID interface and in turn creates two virtual child HID devices, one for the
 * ALS and one for the Touch Bar backlight. The Touch Bar backlight and ALS
 * drivers then attach to these virtual HID devices, and this driver forwards
 * the operations between the real and virtual devices.
 *
 * One important aspect of this approach is that resulting (virtual) HID
 * devices look much like the HID devices found on the later MacBookPro models
 * which have a T2 chip, where there are separate USB interfaces for the Touch
 * Bar backlight and ALS functionality, which means that the drivers work
 * (mostly) the same on both types of models.
 *
 * Lastly, this driver also takes care of the power-management for the iBridge
 * when suspending and resuming.
 */

#define pr_fmt(fmt) "apple-ibridge: " fmt

#include <linux/hid.h>
#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "hid-ids.h"

static const unsigned int appleib_usages[] = {
	/* Ambient Light Sensor */
	0x00200041,
	/* Touch Bar Backlight */
	0xff120001,
};

struct appleib_hid_dev_info {
	struct hid_device *hdev;
	struct hid_device *sub_hdevs[ARRAY_SIZE(appleib_usages)];
	DECLARE_BITMAP(sub_open, ARRAY_SIZE(appleib_usages));
};

static int appleib_hid_raw_event(struct hid_device *hdev,
				 struct hid_report *report, u8 *data, int size)
{
	struct appleib_hid_dev_info *hdev_info = hid_get_drvdata(hdev);
	int i;

	for_each_set_bit(i, hdev_info->sub_open, ARRAY_SIZE(appleib_usages)) {
		hid_input_report(hdev_info->sub_hdevs[i],
				 report->type, data, size, 0);
	}

	return 0;
}

static __u8 *appleib_report_fixup(struct hid_device *hdev, __u8 *rdesc,
				  unsigned int *rsize)
{
	/*
	 * Some fields have a size of 64 bits, which according to HID 1.11
	 * Section 8.4 is not valid ("An item field cannot span more than 4
	 * bytes in a report"). Furthermore, hid_field_extract() complains
	 * when encountering such a field. So turn them into two 32-bit fields
	 * instead.
	 */

	if (*rsize == 634 &&
	    /* Usage Page 0xff12 (vendor defined) */
	    rdesc[212] == 0x06 && rdesc[213] == 0x12 && rdesc[214] == 0xff &&
	    /* Usage 0x51 */
	    rdesc[416] == 0x09 && rdesc[417] == 0x51 &&
	    /* report size 64 */
	    rdesc[432] == 0x75 && rdesc[433] == 64 &&
	    /* report count 1 */
	    rdesc[434] == 0x95 && rdesc[435] == 1) {
		rdesc[433] = 32;
		rdesc[435] = 2;
		hid_dbg(hdev, "Fixed up first 64-bit field\n");
	}

	if (*rsize == 634 &&
	    /* Usage Page 0xff12 (vendor defined) */
	    rdesc[212] == 0x06 && rdesc[213] == 0x12 && rdesc[214] == 0xff &&
	    /* Usage 0x51 */
	    rdesc[611] == 0x09 && rdesc[612] == 0x51 &&
	    /* report size 64 */
	    rdesc[627] == 0x75 && rdesc[628] == 64 &&
	    /* report count 1 */
	    rdesc[629] == 0x95 && rdesc[630] == 1) {
		rdesc[628] = 32;
		rdesc[630] = 2;
		hid_dbg(hdev, "Fixed up second 64-bit field\n");
	}

	return rdesc;
}

#ifdef CONFIG_PM
/**
 * appleib_forward_func() - Forward a function to all virtual HID devices
 * attached to the given real HID device.
 */
#define appleib_forward_func(hdev, function, ...)				\
	({									\
		struct appleib_hid_dev_info *hdev_info = hid_get_drvdata(hdev);	\
		int i, ret = 0;							\
										\
		for (i = 0; i < ARRAY_SIZE(hdev_info->sub_hdevs); i++) {	\
			ret = function(hdev_info->sub_hdevs[i], ##__VA_ARGS__);	\
			if (ret)						\
				break;						\
		}								\
										\
		ret;								\
	})

static int appleib_hid_suspend(struct hid_device *hdev, pm_message_t message)
{
	return appleib_forward_func(hdev, hid_driver_suspend, message);
}

static int appleib_hid_resume(struct hid_device *hdev)
{
	return appleib_forward_func(hdev, hid_driver_resume);
}

static int appleib_hid_reset_resume(struct hid_device *hdev)
{
	return appleib_forward_func(hdev, hid_driver_reset_resume);
}
#endif /* CONFIG_PM */

static int appleib_ll_start(struct hid_device *hdev)
{
	return 0;
}

static void appleib_ll_stop(struct hid_device *hdev) {}

static int appleib_set_open(struct hid_device *hdev, bool open)
{
	struct appleib_hid_dev_info *hdev_info = hdev->driver_data;
	int i;

	for (i = 0; i < ARRAY_SIZE(hdev_info->sub_hdevs); i++) {
		/*
		 * hid_hw_open(), and hence appleib_ll_open(), is called
		 * from the driver's probe function, which in turn is called
		 * while adding the sub-hdev; but at this point we haven't yet
		 * added the sub-hdev to our list. So if we don't find the
		 * sub-hdev in our list assume it's in the process of being
		 * added and set the flag on the first unset sub-hdev.
		 */
		if (hdev_info->sub_hdevs[i] == hdev ||
		    !hdev_info->sub_hdevs[i]) {
			if (open)
				set_bit(i, hdev_info->sub_open);
			else
				clear_bit(i, hdev_info->sub_open);

			return 0;
		}
	}

	return -ENODEV;
}

static int appleib_ll_open(struct hid_device *hdev)
{
	return appleib_set_open(hdev, true);
}

static void appleib_ll_close(struct hid_device *hdev)
{
	appleib_set_open(hdev, false);
}

static int appleib_ll_power(struct hid_device *hdev, int level)
{
	struct appleib_hid_dev_info *hdev_info = hdev->driver_data;

	return hid_hw_power(hdev_info->hdev, level);
}

static int appleib_ll_parse(struct hid_device *hdev)
{
	/* we've already called hid_parse_report() */
	return 0;
}

static void appleib_ll_request(struct hid_device *hdev,
			       struct hid_report *report, int reqtype)
{
	struct appleib_hid_dev_info *hdev_info = hdev->driver_data;

	hid_hw_request(hdev_info->hdev, report, reqtype);
}

static int appleib_ll_wait(struct hid_device *hdev)
{
	struct appleib_hid_dev_info *hdev_info = hdev->driver_data;

	hid_hw_wait(hdev_info->hdev);

	return 0;
}

static int appleib_ll_raw_request(struct hid_device *hdev,
				  unsigned char reportnum, __u8 *buf,
				  size_t len, unsigned char rtype, int reqtype)
{
	struct appleib_hid_dev_info *hdev_info = hdev->driver_data;

	return hid_hw_raw_request(hdev_info->hdev, reportnum, buf, len, rtype,
				  reqtype);
}

static int appleib_ll_output_report(struct hid_device *hdev, __u8 *buf,
				    size_t len)
{
	struct appleib_hid_dev_info *hdev_info = hdev->driver_data;

	return hid_hw_output_report(hdev_info->hdev, buf, len);
}

static const struct hid_ll_driver appleib_ll_driver = {
	.start = appleib_ll_start,
	.stop = appleib_ll_stop,
	.open = appleib_ll_open,
	.close = appleib_ll_close,
	.power = appleib_ll_power,
	.parse = appleib_ll_parse,
	.request = appleib_ll_request,
	.wait = appleib_ll_wait,
	.raw_request = appleib_ll_raw_request,
	.output_report = appleib_ll_output_report,
};

static __u8 *appleib_find_collection(__u8 *start, __u8 *end,
				     unsigned int *usage)
{
	struct hid_item item;
	int depth = 0;

	*usage = 0;

	while ((start = hid_fetch_item(start, end, &item))) {
		if (item.type == HID_ITEM_TYPE_MAIN) {
			switch (item.tag) {
			case HID_MAIN_ITEM_TAG_BEGIN_COLLECTION:
				depth++;
				break;
			case HID_MAIN_ITEM_TAG_END_COLLECTION:
				depth--;
				if (depth <= 0)
					return start;
				break;
			}
		} else if (item.type == HID_ITEM_TYPE_GLOBAL &&
			   item.tag == HID_GLOBAL_ITEM_TAG_USAGE_PAGE &&
			   depth == 0) {
			*usage = (*usage & 0x0000ffff) |
				 ((hid_item_udata(&item) & 0xffff) << 16);
		} else if (item.type == HID_ITEM_TYPE_LOCAL &&
			   item.tag == HID_LOCAL_ITEM_TAG_USAGE &&
			   depth == 0) {
			*usage = (*usage & 0xffff0000) |
				 (hid_item_udata(&item) & 0xffff);
		}
	}

	return end;
}

static bool appleib_usage_in_list(unsigned int usage)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(appleib_usages); i++) {
		if (appleib_usages[i] == usage)
			return true;
	}

	return false;
}

static struct hid_device *
appleib_add_sub_dev(struct appleib_hid_dev_info *hdev_info,
		    unsigned int usage, __u8 *rdesc, size_t rsize)
{
	struct hid_device *sub_hdev, *hdev = hdev_info->hdev;
	int ret;

	sub_hdev = hid_allocate_device();
	if (IS_ERR(sub_hdev))
		return sub_hdev;

	sub_hdev->bus = hdev->bus;
	sub_hdev->vendor = hdev->vendor;
	sub_hdev->product = hdev->product;
	sub_hdev->dev.parent = &hdev->dev;
	sub_hdev->driver_data = hdev_info;
	sub_hdev->ll_driver = &appleib_ll_driver;

	snprintf(sub_hdev->name, sizeof(sub_hdev->name),
		 "iBridge Virtual HID %s/%08x",
		 dev_name(sub_hdev->dev.parent), usage);

	ret = hid_parse_report(sub_hdev, rdesc, rsize);
	if (ret)
		goto destroy_hdev;

	ret = hid_add_device(sub_hdev);
	if (ret)
		goto destroy_hdev;

	return sub_hdev;

destroy_hdev:
	hid_destroy_device(sub_hdev);

	return ERR_PTR(ret);
}

static int appleib_add_sub_devs(struct appleib_hid_dev_info *hdev_info)
{
	struct hid_device *hdev = hdev_info->hdev;
	__u8 *start = hdev->dev_rdesc;
	__u8 *end = start + hdev->dev_rsize;
	__u8 *pos;
	unsigned int usage;
	int i = 0, ret;

	while (true) {
		pos = appleib_find_collection(start, end, &usage);
		if (!appleib_usage_in_list(usage)) {
			hid_warn(hdev, "Unknown collection encountered with usage %x\n", usage);
		} else if (i >= ARRAY_SIZE(hdev_info->sub_hdevs)) {
			hid_warn(hdev, "Too many collections encountered - ignoring for usage %x\n",
				 usage);
		} else {
			hdev_info->sub_hdevs[i] =
				appleib_add_sub_dev(hdev_info, usage, start, pos - start);
			if (IS_ERR(hdev_info->sub_hdevs[i])) {
				ret = PTR_ERR(hdev_info->sub_hdevs[i]);
				while (i--)
					hid_destroy_device(hdev_info->sub_hdevs[i]);
				return ret;
			}

			i++;
		}

		start = pos;
		if (start >= end)
			break;
	}

	return 0;
}

static void appleib_remove_sub_devs(struct hid_device *hdev)
{
	struct appleib_hid_dev_info *hdev_info = hid_get_drvdata(hdev);
	int i;

	for (i = 0; i < ARRAY_SIZE(hdev_info->sub_hdevs); i++)
		hid_destroy_device(hdev_info->sub_hdevs[i]);
}

static int appleib_hid_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	struct appleib_hid_dev_info *hdev_info;
	struct device *dev = &hdev->dev;
	int ret;

	hdev_info = devm_kzalloc(&hdev->dev, sizeof(*hdev_info), GFP_KERNEL);
	if (!hdev_info)
		return -ENOMEM;

	hdev_info->hdev = hdev;

	ret = hid_parse(hdev);
	if (ret)
		return dev_err_probe(dev, ret, "HID parse failed\n");

	ret = hid_hw_start(hdev, HID_CONNECT_DRIVER);
	if (ret)
		return dev_err_probe(dev, ret, "HID hardware start failed\n");

	ret = appleib_add_sub_devs(hdev_info);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to add subdevices\n");
		goto stop_hw;
	}

	hid_set_drvdata(hdev, hdev_info);

	ret = hid_hw_open(hdev);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to open HID device\n");
		goto remove_sub_devs;
	}

	return 0;

remove_sub_devs:
	appleib_remove_sub_devs(hdev);
stop_hw:
	hid_hw_stop(hdev);

	return ret;
}

static void appleib_hid_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	appleib_remove_sub_devs(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id appleib_hid_ids[] = {
	{ HID_DEVICE(BUS_USB, HID_GROUP_APPLEIB,
		     USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_IBRIDGE) },
	{ }
};
MODULE_DEVICE_TABLE(hid, appleib_hid_ids);

static struct hid_driver appleib_hid_driver = {
	.name = "hid-appleib",
	.id_table = appleib_hid_ids,
	.probe = appleib_hid_probe,
	.remove = appleib_hid_remove,
	.raw_event = appleib_hid_raw_event,
	.report_fixup = appleib_report_fixup,
#ifdef CONFIG_PM
	.suspend = appleib_hid_suspend,
	.resume = appleib_hid_resume,
	.reset_resume = appleib_hid_reset_resume,
#endif
};

static void appleib_set_power(struct device *dev, int arg)
{
	acpi_status sts;

	sts = acpi_execute_simple_method(ACPI_HANDLE(dev), "SOCW", arg);
	if (ACPI_FAILURE(sts))
		dev_warn(dev, "SOCW(%d) failed: %s\n",
			 arg, acpi_format_exception(sts));
}

static int appleib_suspend(struct device *dev)
{
	appleib_set_power(dev, 0);

	return 0;
}

static int appleib_resume(struct device *dev)
{
	appleib_set_power(dev, 1);

	return 0;
}

static const DEFINE_SIMPLE_DEV_PM_OPS(appleib_pm_ops, appleib_suspend, appleib_resume);

static const struct acpi_device_id appleib_acpi_match[] = {
	{ "APP7777" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, appleib_acpi_match);

static struct platform_driver appleib_driver = {
	.driver = {
		.name = "apple-ibridge",
		.pm = &appleib_pm_ops,
		.acpi_match_table = appleib_acpi_match,
	},
};

static int __init appleib_init(void)
{
	int ret;

	ret = platform_driver_register(&appleib_driver);
	if (ret)
		return ret;

	ret = hid_register_driver(&appleib_hid_driver);
	if (ret) {
		platform_driver_unregister(&appleib_driver);
		return ret;
	}

	return 0;
}
module_init(appleib_init);

static void __exit appleib_exit(void)
{
	hid_unregister_driver(&appleib_hid_driver);
	platform_driver_unregister(&appleib_driver);
}
module_exit(appleib_exit);

MODULE_AUTHOR("Ronald Tschalär");
MODULE_DESCRIPTION("Apple iBridge Driver");
MODULE_LICENSE("GPL");

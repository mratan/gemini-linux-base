// SPDX-License-Identifier: GPL-2.0
/*
 * Novatek NT36xxx capacitive touch controller — Planet Computers Gemini PDA
 *
 * WHY THIS EXISTS
 *
 * The Gemini shipped in (at least) two panel variants, and each came with its
 * own touch controller. The vendor DTB reflects that exactly: `cap_touch@62`
 * (Novatek) and `solomon_touch@53` sit side by side on the same I2C bus, and
 * the vendor kernel builds a driver for each — CONFIG_TOUCHSCREEN_MTK_NT36xxx
 * and CONFIG_TOUCHSCREEN_MTK_SSL_SSD20XX are both =y — letting probe decide.
 *
 * This unit is the Novatek variant, the same one whose panel is an NT36672
 * (see panel-solomon-ssd2092.c's identity note). Its touchscreen has always
 * worked under the stock vendor kernel; what did not work was our port, which
 * only ever had the Solomon driver and therefore probed 0x53 — an address with
 * nothing behind it on this hardware. The resulting
 *
 *     ssd2092 3-0053: DS boot status read failed: -6
 *
 * is -ENXIO, and it was telling the plain truth all along: no such device. It
 * was read as a handshake problem for weeks.
 *
 * ADDRESSING (the part that surprises people)
 *
 * The chip answers on more than one address. The DT node carries 0x62, which
 * is the hardware/bootloader address the vendor header calls I2C_HW_Address,
 * but touch reports and firmware info are read from 0x01 (I2C_FW_Address). So
 * this driver is instantiated at 0x62 and directs its transfers at 0x01 by
 * setting i2c_msg.addr explicitly, exactly as the vendor's CTP_I2C_READ does.
 * A driver that only ever talks to its own client address will read nothing.
 *
 * SCOPE
 *
 * The vendor driver is ~5600 lines across four files, but three of them are
 * firmware update, production-line MP testing, and a debug procfs interface.
 * None of that is needed to move a cursor. This implements the report path
 * only, which is the part that makes the hardware useful.
 *
 * WHY IT POLLS
 *
 * The controller has an interrupt line (CTP_INT, GPIO85) and the vendor driver
 * uses it. We cannot: mainline's MT6797 pinctrl registers no mtk_eint_hw data
 * at all, so no GPIO on this SoC can deliver an interrupt (blockers.md B-11).
 * `&pio` is not an interrupt controller, so `interrupts = <85 ...>` silently
 * resolves to nothing and the client's irq is 0. The first version of this
 * driver required an interrupt and failed probe with -EINVAL for exactly that
 * reason. Polling is not a preference here, it is the only option until B-11
 * is fixed — and it is what the sibling Solomon driver does for the same
 * reason.
 *
 * Protocol, from aeon_nt36xxx/nt36xxx.c:
 *   read 65 bytes from address 0x01 starting at register 0x00; then for each
 *   finger i, at offset 1 + 6*i:
 *     [0] bits 7:3 = touch id (1-based), bits 2:0 = state (1 = down, 2 = move)
 *     [1] x high 8       [2] y high 8
 *     [3] x low 4 (7:4), y low 4 (3:0)
 *     [4] width          [5] pressure
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>

#define NVT_FW_ADDR		0x01	/* reports and firmware info live here */

#define NVT_EVENT_FWINFO	0x78	/* EVENT_MAP_FWINFO */
#define NVT_POINT_DATA_LEN	65
#define NVT_MAX_FINGERS		10
#define NVT_MAX_PRESSURE	1000	/* TOUCH_FORCE_NUM */

/* Fallbacks if the firmware-info read fails; the vendor uses the same. */
#define NVT_DEFAULT_MAX_X	1080
#define NVT_DEFAULT_MAX_Y	1920

struct nvt_ts {
	struct i2c_client	*client;
	struct input_dev	*input;
	struct gpio_desc	*reset_gpio;
	u32			max_x;
	u32			max_y;
	bool			swap_xy;
	bool			invert_y;
};

/*
 * Read from the firmware address rather than the client's own. Byte 0 of the
 * buffer is the register offset written before the read turns around.
 */
static int nvt_read(struct nvt_ts *ts, u8 reg, u8 *buf, size_t len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = NVT_FW_ADDR, .flags = 0,        .len = 1,   .buf = &reg },
		{ .addr = NVT_FW_ADDR, .flags = I2C_M_RD, .len = len, .buf = buf  },
	};
	int ret, tries;

	/*
	 * The vendor retries five times. Keep that: the controller NAKs while
	 * it is still coming out of reset, and a single attempt at probe is
	 * the difference between a working touchscreen and a -ENXIO that gets
	 * misdiagnosed for weeks.
	 */
	for (tries = 0; tries < 5; tries++) {
		ret = i2c_transfer(ts->client->adapter, msgs, 2);
		if (ret == 2)
			return 0;
		usleep_range(1000, 2000);
	}

	return ret < 0 ? ret : -EIO;
}

static void nvt_reset(struct nvt_ts *ts)
{
	if (!ts->reset_gpio)
		return;

	/* LK leaves CTP_RST low, i.e. the controller held in reset. */
	gpiod_set_value_cansleep(ts->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ts->reset_gpio, 0);
	msleep(50);
}

static void nvt_read_fw_info(struct nvt_ts *ts)
{
	u8 buf[17] = {};
	int ret;

	ret = nvt_read(ts, NVT_EVENT_FWINFO, buf, sizeof(buf));
	if (ret) {
		dev_warn(&ts->client->dev,
			 "firmware info read failed (%d); assuming %ux%u\n",
			 ret, NVT_DEFAULT_MAX_X, NVT_DEFAULT_MAX_Y);
		ts->max_x = NVT_DEFAULT_MAX_X;
		ts->max_y = NVT_DEFAULT_MAX_Y;
		return;
	}

	/* Panel resolution is reported by the firmware, not assumed. */
	ts->max_x = (buf[4] << 8) | buf[5];
	ts->max_y = (buf[6] << 8) | buf[7];

	if (!ts->max_x || !ts->max_y) {
		ts->max_x = NVT_DEFAULT_MAX_X;
		ts->max_y = NVT_DEFAULT_MAX_Y;
	}

	dev_info(&ts->client->dev, "firmware reports %ux%u\n",
		 ts->max_x, ts->max_y);
}

static void nvt_ts_poll(struct input_dev *input)
{
	struct nvt_ts *ts = input_get_drvdata(input);
	u8 point_data[NVT_POINT_DATA_LEN] = {};
	unsigned int i;
	int ret;

	ret = nvt_read(ts, 0x00, point_data, sizeof(point_data));
	if (ret) {
		dev_err_ratelimited(&ts->client->dev, "point read failed: %d\n", ret);
		return;
	}

	for (i = 0; i < NVT_MAX_FINGERS; i++) {
		unsigned int pos = 1 + 6 * i;
		unsigned int id = point_data[pos] >> 3;
		unsigned int state = point_data[pos] & 0x07;
		unsigned int x, y, w, p;

		/* 1 = finger down (enter), 2 = finger moving. Anything else is
		 * not a live contact. */
		if (id == 0 || id > NVT_MAX_FINGERS)
			continue;
		if (state != 0x01 && state != 0x02)
			continue;

		x = (point_data[pos + 1] << 4) | (point_data[pos + 3] >> 4);
		y = (point_data[pos + 2] << 4) | (point_data[pos + 3] & 0x0f);
		if (x > ts->max_x || y > ts->max_y)
			continue;

		w = point_data[pos + 4] ?: 1;
		p = point_data[pos + 5] ?: 1;
		if (p > NVT_MAX_PRESSURE)
			p = NVT_MAX_PRESSURE;

		/*
		 * The Gemini is a clamshell used in landscape while the panel
		 * is natively portrait, so the vendor driver swaps the axes and
		 * mirrors one of them. Express that through DT properties
		 * rather than an "#if 1" the way the vendor did.
		 */
		if (ts->swap_xy)
			swap(x, y);
		if (ts->invert_y)
			y = ts->max_x - 1 - y;

		input_mt_slot(ts->input, id - 1);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		input_report_abs(ts->input, ABS_MT_POSITION_X, x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, w);
		input_report_abs(ts->input, ABS_MT_PRESSURE, p);
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static int nvt_ts_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct nvt_ts *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio),
				     "failed to get the reset GPIO\n");

	ts->swap_xy = device_property_read_bool(dev, "touchscreen-swapped-x-y");
	ts->invert_y = device_property_read_bool(dev, "touchscreen-inverted-y");

	nvt_reset(ts);
	nvt_read_fw_info(ts);

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "Novatek NT36xxx Touchscreen";
	ts->input->id.bustype = BUS_I2C;
	input_set_drvdata(ts->input, ts);

	/*
	 * After a swap the reported X spans what was the Y range. Set the axis
	 * limits from the orientation actually in use, or userspace scales
	 * every touch wrongly.
	 */
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0,
			     ts->swap_xy ? ts->max_y : ts->max_x, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0,
			     ts->swap_xy ? ts->max_x : ts->max_y, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, NVT_MAX_PRESSURE, 0, 0);

	ret = input_mt_init_slots(ts->input, NVT_MAX_FINGERS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init MT slots\n");

	/* 60 Hz: comfortably above what a cursor needs, and cheap — one 65-byte
	 * I2C read per tick on a bus doing nothing else. */
	ret = input_setup_polling(ts->input, nvt_ts_poll);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set up polling\n");
	input_set_poll_interval(ts->input, 16);

	ret = input_register_device(ts->input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register input device\n");

	dev_info(dev, "Novatek NT36xxx touchscreen ready (client 0x%02x, reports via 0x%02x, polled - B-11)\n",
		 client->addr, NVT_FW_ADDR);
	return 0;
}

static const struct of_device_id nvt_ts_of_match[] = {
	{ .compatible = "novatek,nt36xxx-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, nvt_ts_of_match);

static struct i2c_driver nvt_ts_driver = {
	.driver = {
		.name = "gemini-nt36xxx",
		.of_match_table = nvt_ts_of_match,
	},
	.probe = nvt_ts_probe,
};
module_i2c_driver(nvt_ts_driver);

MODULE_DESCRIPTION("Novatek NT36xxx touchscreen (Gemini PDA)");
MODULE_LICENSE("GPL");

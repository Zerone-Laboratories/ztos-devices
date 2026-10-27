// SPDX-License-Identifier: GPL-2.0-only
/*
 * The vibration motor of the Samsung Galaxy J5 (2015), which hangs off LDO2
 * of the SiliconMitus SM5703 PMIC: a rumble effect switches the LDO on.
 *
 * Mainline has no SM5703 MFD driver, so this talks to the PMIC directly.
 * LDO control registers (as in the downstream sm5703 regulator driver): LDO1
 * is 0x1a, LDO2 0x1b, LDO3 0x1c; bit 3 enables, bits 0-2 pick the voltage.
 */
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/workqueue.h>

#define SM5703_REG_LDO2		0x1b
#define SM5703_LDO_EN		BIT(3)
#define SM5703_LDO_VSEL		GENMASK(2, 0)
#define SM5703_LDO_MAX		7

/*
 * A plain motor that needs tens of milliseconds to get up to speed: the
 * shell's ticks (7 to 38 ms, made for the S9) stopped it before it was felt.
 * Every pulse runs at least this long, at the LDO's top step (measured: the
 * motor shakes ~15 % more there than at 3.0 V, and starts sooner).
 */
#define SM5703_MIN_ON_MS	40

struct sm5703_vib {
	struct i2c_client *client;
	struct input_dev *input;
	struct work_struct work;
	struct delayed_work stop;
	unsigned long on_since;
	bool on;
};

static void sm5703_vib_work(struct work_struct *work)
{
	struct sm5703_vib *vib = container_of(work, struct sm5703_vib, work);
	int val = i2c_smbus_read_byte_data(vib->client, SM5703_REG_LDO2);

	if (val < 0)
		return;
	val &= ~(SM5703_LDO_EN | SM5703_LDO_VSEL);
	if (vib->on)
		val |= SM5703_LDO_EN | SM5703_LDO_MAX;
	i2c_smbus_write_byte_data(vib->client, SM5703_REG_LDO2, val);
}

static int sm5703_vib_play(struct input_dev *input, void *data,
			   struct ff_effect *effect)
{
	struct sm5703_vib *vib = input_get_drvdata(input);
	unsigned long min_off;

	if (effect->u.rumble.strong_magnitude || effect->u.rumble.weak_magnitude) {
		cancel_delayed_work(&vib->stop);
		if (!vib->on)
			vib->on_since = jiffies;
		vib->on = true;
		schedule_work(&vib->work);
		return 0;
	}
	/* a stop: held back until the motor has run its minimum */
	min_off = vib->on_since + msecs_to_jiffies(SM5703_MIN_ON_MS);
	schedule_delayed_work(&vib->stop, time_before(jiffies, min_off) ? min_off - jiffies : 0);
	return 0;
}

static void sm5703_vib_stop(struct work_struct *work)
{
	struct sm5703_vib *vib = container_of(to_delayed_work(work), struct sm5703_vib, stop);

	vib->on = false;
	sm5703_vib_work(&vib->work);
}

static void sm5703_vib_close(struct input_dev *input)
{
	struct sm5703_vib *vib = input_get_drvdata(input);

	cancel_delayed_work_sync(&vib->stop);
	cancel_work_sync(&vib->work);
	vib->on = false;
	sm5703_vib_work(&vib->work);
}

static int sm5703_vib_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sm5703_vib *vib;
	int ret;

	vib = devm_kzalloc(dev, sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;
	vib->client = client;
	INIT_WORK(&vib->work, sm5703_vib_work);
	INIT_DELAYED_WORK(&vib->stop, sm5703_vib_stop);

	ret = i2c_smbus_read_byte_data(client, SM5703_REG_LDO2);
	if (ret < 0)
		return dev_err_probe(dev, ret, "PMIC not answering\n");

	vib->input = devm_input_allocate_device(dev);
	if (!vib->input)
		return -ENOMEM;
	vib->input->name = "sm5703-vibrator";
	vib->input->close = sm5703_vib_close;
	input_set_drvdata(vib->input, vib);
	input_set_capability(vib->input, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(vib->input, NULL, sm5703_vib_play);
	if (ret)
		return ret;
	return input_register_device(vib->input);
}

static const struct of_device_id sm5703_vib_of_match[] = {
	{ .compatible = "siliconmitus,sm5703-vibrator" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5703_vib_of_match);

static struct i2c_driver sm5703_vib_driver = {
	.driver = {
		.name = "sm5703-vibrator",
		.of_match_table = sm5703_vib_of_match,
	},
	.probe = sm5703_vib_probe,
};
module_i2c_driver(sm5703_vib_driver);

MODULE_DESCRIPTION("SM5703 LDO2 vibration motor");
MODULE_LICENSE("GPL");

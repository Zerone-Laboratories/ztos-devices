// SPDX-License-Identifier: GPL-2.0-only
/*
 * SiliconMitus SM5703 fuel gauge, as found in the Samsung Galaxy J5 (2015).
 *
 * Reads the gauge the way Samsung's stock driver does: every value is a
 * 16-bit word, high byte the integer part, low byte 1/256ths, bit 15 the
 * sign. The battery state of charge comes from the gauge; whether a charger
 * is plugged in comes from the SM5703 MUIC through extcon.
 */
#include <linux/extcon.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define SM5703_REG_SOC		0x05
#define SM5703_REG_OCV		0x06
#define SM5703_REG_VOLTAGE	0x07
#define SM5703_REG_CURRENT	0x08
#define SM5703_REG_TEMPERATURE	0x09

#define SM5703_POLL		(30 * HZ)

struct sm5703_fg {
	struct i2c_client *client;
	struct power_supply *battery;
	struct power_supply *usb;
	struct extcon_dev *edev;
	struct notifier_block usb_nb, dcp_nb;
	struct delayed_work poll;
	int last_soc;
};

static int sm5703_word(struct sm5703_fg *fg, u8 reg, int scale, int *val)
{
	int ret = i2c_smbus_read_word_data(fg->client, reg);

	if (ret < 0)
		return ret;
	*val = ((ret & 0x7f00) >> 8) * scale + ((ret & 0xff) * scale) / 256;
	if (ret & 0x8000)
		*val = -*val;
	return 0;
}

static bool sm5703_online(struct sm5703_fg *fg)
{
	if (IS_ERR_OR_NULL(fg->edev))
		return false;
	return extcon_get_state(fg->edev, EXTCON_USB) > 0 ||
	       extcon_get_state(fg->edev, EXTCON_CHG_USB_DCP) > 0;
}

static int sm5703_soc(struct sm5703_fg *fg, int *soc)
{
	int ret = sm5703_word(fg, SM5703_REG_SOC, 10, soc);

	if (ret)
		return ret;
	*soc = clamp((*soc + 5) / 10, 0, 100);
	return 0;
}

static int sm5703_battery_get(struct power_supply *psy,
			      enum power_supply_property psp,
			      union power_supply_propval *val)
{
	struct sm5703_fg *fg = power_supply_get_drvdata(psy);
	int ret = 0, soc, cur;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = sm5703_soc(fg, &soc);
		if (ret)
			break;
		if (!sm5703_online(fg))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (soc >= 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (!sm5703_word(fg, SM5703_REG_CURRENT, 1000, &cur) && cur < 0)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = sm5703_soc(fg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:	/* mV in the gauge, uV here */
		ret = sm5703_word(fg, SM5703_REG_VOLTAGE, 1000, &val->intval);
		val->intval *= 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		ret = sm5703_word(fg, SM5703_REG_OCV, 1000, &val->intval);
		val->intval *= 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:	/* mA in the gauge, uA here */
		ret = sm5703_word(fg, SM5703_REG_CURRENT, 1000, &val->intval);
		val->intval *= 1000;
		break;
	case POWER_SUPPLY_PROP_TEMP:		/* tenths of a degree */
		ret = sm5703_word(fg, SM5703_REG_TEMPERATURE, 10, &val->intval);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static int sm5703_usb_get(struct power_supply *psy,
			  enum power_supply_property psp,
			  union power_supply_propval *val)
{
	struct sm5703_fg *fg = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	val->intval = sm5703_online(fg);
	return 0;
}

static const enum power_supply_property sm5703_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static const enum power_supply_property sm5703_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sm5703_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm5703_battery_props,
	.num_properties = ARRAY_SIZE(sm5703_battery_props),
	.get_property = sm5703_battery_get,
};

static const struct power_supply_desc sm5703_usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sm5703_usb_props,
	.num_properties = ARRAY_SIZE(sm5703_usb_props),
	.get_property = sm5703_usb_get,
};

/* the gauge raises no interrupt we use: tell userspace when the level moves */
static void sm5703_poll(struct work_struct *work)
{
	struct sm5703_fg *fg = container_of(work, struct sm5703_fg, poll.work);
	int soc;

	if (!sm5703_soc(fg, &soc) && soc != fg->last_soc) {
		fg->last_soc = soc;
		power_supply_changed(fg->battery);
	}
	schedule_delayed_work(&fg->poll, SM5703_POLL);
}

static void sm5703_changed(struct sm5703_fg *fg)
{
	power_supply_changed(fg->usb);
	power_supply_changed(fg->battery);
}

static int sm5703_usb_cable(struct notifier_block *nb, unsigned long event, void *ptr)
{
	sm5703_changed(container_of(nb, struct sm5703_fg, usb_nb));
	return NOTIFY_OK;
}

static int sm5703_dcp_cable(struct notifier_block *nb, unsigned long event, void *ptr)
{
	sm5703_changed(container_of(nb, struct sm5703_fg, dcp_nb));
	return NOTIFY_OK;
}

static int sm5703_fg_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config cfg = { .of_node = dev->of_node };
	struct sm5703_fg *fg;
	int ret;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;
	fg->client = client;
	fg->last_soc = -1;
	cfg.drv_data = fg;
	i2c_set_clientdata(client, fg);

	if (of_property_present(dev->of_node, "extcon")) {
		fg->edev = extcon_get_edev_by_phandle(dev, 0);
		if (IS_ERR(fg->edev))
			return dev_err_probe(dev, PTR_ERR(fg->edev), "no extcon\n");
	}

	fg->battery = devm_power_supply_register(dev, &sm5703_battery_desc, &cfg);
	if (IS_ERR(fg->battery))
		return PTR_ERR(fg->battery);
	fg->usb = devm_power_supply_register(dev, &sm5703_usb_desc, &cfg);
	if (IS_ERR(fg->usb))
		return PTR_ERR(fg->usb);

	if (fg->edev) {
		fg->usb_nb.notifier_call = sm5703_usb_cable;
		fg->dcp_nb.notifier_call = sm5703_dcp_cable;
		ret = devm_extcon_register_notifier(dev, fg->edev, EXTCON_USB, &fg->usb_nb);
		if (!ret)
			ret = devm_extcon_register_notifier(dev, fg->edev,
							    EXTCON_CHG_USB_DCP, &fg->dcp_nb);
		if (ret)
			return ret;
	}

	INIT_DELAYED_WORK(&fg->poll, sm5703_poll);
	schedule_delayed_work(&fg->poll, 0);
	return 0;
}

static void sm5703_fg_remove(struct i2c_client *client)
{
	struct sm5703_fg *fg = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&fg->poll);
}

static const struct of_device_id sm5703_fg_of_match[] = {
	{ .compatible = "siliconmitus,sm5703-fuel-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5703_fg_of_match);

static struct i2c_driver sm5703_fg_driver = {
	.driver = {
		.name = "sm5703-fuel-gauge",
		.of_match_table = sm5703_fg_of_match,
	},
	.probe = sm5703_fg_probe,
	.remove = sm5703_fg_remove,
};
module_i2c_driver(sm5703_fg_driver);

MODULE_DESCRIPTION("SiliconMitus SM5703 fuel gauge");
MODULE_LICENSE("GPL");

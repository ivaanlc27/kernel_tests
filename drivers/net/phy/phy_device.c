/* Framework for finding and configuring PHYs.
 * Also contains generic PHY driver
 *
 * Author: Andy Fleming
 *
 * Copyright (c) 2004 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/phy_led_triggers.h>
#include <linux/mdio.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/of.h>

#include <asm/irq.h>

MODULE_DESCRIPTION("PHY library");
MODULE_AUTHOR("Andy Fleming");
MODULE_LICENSE("GPL");

void phy_device_free(struct phy_device *phydev)
{
	put_device(&phydev->mdio.dev);
}
EXPORT_SYMBOL(phy_device_free);

static void phy_mdio_device_free(struct mdio_device *mdiodev)
{
	struct phy_device *phydev;

	phydev = container_of(mdiodev, struct phy_device, mdio);
	phy_device_free(phydev);
}

static void phy_device_release(struct device *dev)
{
	kfree(to_phy_device(dev));
}

static void phy_mdio_device_remove(struct mdio_device *mdiodev)
{
	struct phy_device *phydev;

	phydev = container_of(mdiodev, struct phy_device, mdio);
	phy_device_remove(phydev);
}

static struct phy_driver genphy_driver;
extern struct phy_driver genphy_10g_driver;

static LIST_HEAD(phy_fixup_list);
static DEFINE_MUTEX(phy_fixup_lock);

#ifdef CONFIG_PM
static bool mdio_bus_phy_may_suspend(struct phy_device *phydev)
{
	struct device_driver *drv = phydev->mdio.dev.driver;
	struct phy_driver *phydrv = to_phy_driver(drv);
	struct net_device *netdev = phydev->attached_dev;

	if (!drv || !phydrv->suspend)
		return false;

	/* PHY not attached? May suspend if the PHY has not already been
	 * suspended as part of a prior call to phy_disconnect() ->
	 * phy_detach() -> phy_suspend() because the parent netdev might be the
	 * MDIO bus driver and clock gated at this point.
	 */
	if (!netdev)
		return !phydev->suspended;

	if (netdev->wol_enabled)
		return false;

	/* As long as not all affected network drivers support the
	 * wol_enabled flag, let's check for hints that WoL is enabled.
	 * Don't suspend PHY if the attached netdev parent may wake up.
	 * The parent may point to a PCI device, as in tg3 driver.
	 */
	if (netdev->dev.parent && device_may_wakeup(netdev->dev.parent))
		return false;

	/* Also don't suspend PHY if the netdev itself may wakeup. This
	 * is the case for devices w/o underlaying pwr. mgmt. aware bus,
	 * e.g. SoC devices.
	 */
	if (device_may_wakeup(&netdev->dev))
		return false;

	return true;
}

static int mdio_bus_phy_suspend(struct device *dev)
{
	struct phy_device *phydev = to_phy_device(dev);

	/* We must stop the state machine manually, otherwise it stops out of
	 * control, possibly with the phydev->lock held. Upon resume, netdev
	 * may call phy routines that try to grab the same lock, and that may
	 * lead to a deadlock.
	 */
	if (phydev->attached_dev && phydev->adjust_link)
		phy_stop_machine(phydev);

	if (!mdio_bus_phy_may_suspend(phydev))
		return 0;

	return phy_suspend(phydev);
}

static int mdio_bus_phy_resume(struct device *dev)
{
	struct phy_device *phydev = to_phy_device(dev);
	int ret;

	if (!mdio_bus_phy_may_suspend(phydev))
		goto no_resume;

	ret = phy_resume(phydev);
	if (ret < 0)
		return ret;

no_resume:
	if (phydev->attached_dev && phydev->adjust_link)
		phy_start_machine(phydev);

	return 0;
}

static int mdio_bus_phy_restore(struct device *dev)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct net_device *netdev = phydev->attached_dev;
	int ret;

	if (!netdev)
		return 0;

	ret = phy_init_hw(phydev);
	if (ret < 0)
		return ret;

	if (phydev->attached_dev && phydev->adjust_link)
		phy_start_machine(phydev);

	return 0;
}

static const struct dev_pm_ops mdio_bus_phy_pm_ops = {
	.suspend = mdio_bus_phy_suspend,
	.resume = mdio_bus_phy_resume,
	.freeze = mdio_bus_phy_suspend,
	.thaw = mdio_bus_phy_resume,
	.restore = mdio_bus_phy_restore,
};

#define MDIO_BUS_PHY_PM_OPS (&mdio_bus_phy_pm_ops)

#else

#define MDIO_BUS_PHY_PM_OPS NULL

#endif /* CONFIG_PM */

/**
 * phy_register_fixup - creates a new phy_fixup and adds it to the list
 * @bus_id: A string which matches phydev->mdio.dev.bus_id (or PHY_ANY_ID)
 * @phy_uid: Used to match against phydev->phy_id (the UID of the PHY)
 *	It can also be PHY_ANY_UID
 * @phy_uid_mask: Applied to phydev->phy_id and fixup->phy_uid before
 *	comparison
 * @run: The actual code to be run when a matching PHY is found
 */
int phy_register_fixup(const char *bus_id, u32 phy_uid, u32 phy_uid_mask,
		       int (*run)(struct phy_device *))
{
	struct phy_fixup *fixup = kzalloc(sizeof(*fixup), GFP_KERNEL);

	if (!fixup)
		return -ENOMEM;

	strlcpy(fixup->bus_id, bus_id, sizeof(fixup->bus_id));
	fixup->phy_uid = phy_uid;
	fixup->phy_uid_mask = phy_uid_mask;
	fixup->run = run;

	mutex_lock(&phy_fixup_lock);
	list_add_tail(&fixup->list, &phy_fixup_list);
	mutex_unlock(&phy_fixup_lock);

	return 0;
}
EXPORT_SYMBOL(phy_register_fixup);

/* Registers a fixup to be run on any PHY with the UID in phy_uid */
int phy_register_fixup_for_uid(u32 phy_uid, u32 phy_uid_mask,
			       int (*run)(struct phy_device *))
{
	return phy_register_fixup(PHY_ANY_ID, phy_uid, phy_uid_mask, run);
}
EXPORT_SYMBOL(phy_register_fixup_for_uid);

/* Registers a fixup to be run on the PHY with id string bus_id */
int phy_register_fixup_for_id(const char *bus_id,
			      int (*run)(struct phy_device *))
{
	return phy_register_fixup(bus_id, PHY_ANY_UID, 0xffffffff, run);
}
EXPORT_SYMBOL(phy_register_fixup_for_id);

/**
 * phy_unregister_fixup - remove a phy_fixup from the list
 * @bus_id: A string matches fixup->bus_id (or PHY_ANY_ID) in phy_fixup_list
 * @phy_uid: A phy id matches fixup->phy_id (or PHY_ANY_UID) in phy_fixup_list
 * @phy_uid_mask: Applied to phy_uid and fixup->phy_uid before comparison
 */
int phy_unregister_fixup(const char *bus_id, u32 phy_uid, u32 phy_uid_mask)
{
	struct list_head *pos, *n;
	struct phy_fixup *fixup;
	int ret;

	ret = -ENODEV;

	mutex_lock(&phy_fixup_lock);
	list_for_each_safe(pos, n, &phy_fixup_list) {
		fixup = list_entry(pos, struct phy_fixup, list);

		if ((!strcmp(fixup->bus_id, bus_id)) &&
		    ((fixup->phy_uid & phy_uid_mask) ==
		     (phy_uid & phy_uid_mask))) {
			list_del(&fixup->list);
			kfree(fixup);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&phy_fixup_lock);

	return ret;
}
EXPORT_SYMBOL(phy_unregister_fixup);

/* Unregisters a fixup of any PHY with the UID in phy_uid */
int phy_unregister_fixup_for_uid(u32 phy_uid, u32 phy_uid_mask)
{
	return phy_unregister_fixup(PHY_ANY_ID, phy_uid, phy_uid_mask);
}
EXPORT_SYMBOL(phy_unregister_fixup_for_uid);

/* Unregisters a fixup of the PHY with id string bus_id */
int phy_unregister_fixup_for_id(const char *bus_id)
{
	return phy_unregister_fixup(bus_id, PHY_ANY_UID, 0xffffffff);
}
EXPORT_SYMBOL(phy_unregister_fixup_for_id);

/* Returns 1 if fixup matches phydev in bus_id and phy_uid.
 * Fixups can be set to match any in one or more fields.
 */
static int phy_needs_fixup(struct phy_device *phydev, struct phy_fixup *fixup)
{
	if (strcmp(fixup->bus_id, phydev_name(phydev)) != 0)
		if (strcmp(fixup->bus_id, PHY_ANY_ID) != 0)
			return 0;

	if ((fixup->phy_uid & fixup->phy_uid_mask) !=
	    (phydev->phy_id & fixup->phy_uid_mask))
		if (fixup->phy_uid != PHY_ANY_UID)
			return 0;

	return 1;
}

/* Runs any matching fixups for this phydev */
static int phy_scan_fixups(struct phy_device *phydev)
{
	struct phy_fixup *fixup;

	mutex_lock(&phy_fixup_lock);
	list_for_each_entry(fixup, &phy_fixup_list, list) {
		if (phy_needs_fixup(phydev, fixup)) {
			int err = fixup->run(phydev);

			if (err < 0) {
				mutex_unlock(&phy_fixup_lock);
				return err;
			}
			phydev->has_fixups = true;
		}
	}
	mutex_unlock(&phy_fixup_lock);

	return 0;
}

static int phy_bus_match(struct device *dev, struct device_driver *drv)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct phy_driver *phydrv = to_phy_driver(drv);
	const int num_ids = ARRAY_SIZE(phydev->c45_ids.device_ids);
	int i;

	if (!(phydrv->mdiodrv.flags & MDIO_DEVICE_IS_PHY))
		return 0;

	if (phydrv->match_phy_device)
		return phydrv->match_phy_device(phydev);

	if (phydev->is_c45) {
		for (i = 1; i < num_ids; i++) {
			if (!(phydev->c45_ids.devices_in_package & (1 << i)))
				continue;

			if ((phydrv->phy_id & phydrv->phy_id_mask) ==
			    (phydev->c45_ids.device_ids[i] &
			     phydrv->phy_id_mask))
				return 1;
		}
		return 0;
	} else {
		return (phydrv->phy_id & phydrv->phy_id_mask) ==
			(phydev->phy_id & phydrv->phy_id_mask);
	}
}

static ssize_t
phy_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);

	return sprintf(buf, "0x%.8lx\n", (unsigned long)phydev->phy_id);
}
static DEVICE_ATTR_RO(phy_id);

static ssize_t
phy_interface_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	const char *mode = NULL;

	if (phy_is_internal(phydev))
		mode = "internal";
	else
		mode = phy_modes(phydev->interface);

	return sprintf(buf, "%s\n", mode);
}
static DEVICE_ATTR_RO(phy_interface);

static ssize_t
phy_has_fixups_show(struct device *dev, struct device_attribute *attr,
		    char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);

	return sprintf(buf, "%d\n", phydev->has_fixups);
}
static DEVICE_ATTR_RO(phy_has_fixups);

static struct attribute *phy_dev_attrs[] = {
	&dev_attr_phy_id.attr,
	&dev_attr_phy_interface.attr,
	&dev_attr_phy_has_fixups.attr,
	NULL,
};
ATTRIBUTE_GROUPS(phy_dev);

static const struct device_type mdio_bus_phy_type = {
	.name = "PHY",
	.groups = phy_dev_groups,
	.release = phy_device_release,
	.pm = MDIO_BUS_PHY_PM_OPS,
};

struct phy_device *phy_device_create(struct mii_bus *bus, int addr, int phy_id,
				     bool is_c45,
				     struct phy_c45_device_ids *c45_ids)
{
	struct phy_device *dev;
	struct mdio_device *mdiodev;

	/* We allocate the device, and initialize the default values */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	mdiodev = &dev->mdio;
	mdiodev->dev.parent = &bus->dev;
	mdiodev->dev.bus = &mdio_bus_type;
	mdiodev->dev.type = &mdio_bus_phy_type;
	mdiodev->bus = bus;
	mdiodev->bus_match = phy_bus_match;
	mdiodev->addr = addr;
	mdiodev->flags = MDIO_DEVICE_FLAG_PHY;
	mdiodev->device_free = phy_mdio_device_free;
	mdiodev->device_remove = phy_mdio_device_remove;

	dev->speed = SPEED_UNKNOWN;
	dev->duplex = DUPLEX_UNKNOWN;
	dev->pause = 0;
	dev->asym_pause = 0;
	dev->link = 0;
	dev->interface = PHY_INTERFACE_MODE_GMII;

	dev->autoneg = AUTONEG_ENABLE;

	dev->is_c45 = is_c45;
	dev->phy_id = phy_id;
	if (c45_ids)
		dev->c45_ids = *c45_ids;
	dev->irq = bus->irq[addr];
	dev_set_name(&mdiodev->dev, PHY_ID_FMT, bus->id, addr);

	dev->state = PHY_DOWN;

	mutex_init(&dev->lock);
	INIT_DELAYED_WORK(&dev->state_queue, phy_state_machine);
	INIT_WORK(&dev->phy_queue, phy_change_work);

	/* Request the appropriate module unconditionally; don't
	 * bother trying to do so only if it isn't already loaded,
	 * because that gets complicated. A hotplug event would have
	 * done an unconditional modprobe anyway.
	 * We don't do normal hotplug because it won't work for MDIO
	 * -- because it relies on the device staying around for long
	 * enough for the driver to get loaded. With MDIO, the NIC
	 * driver will get bored and give up as soon as it finds that
	 * there's no driver _already_ loaded.
	 */
	request_module(MDIO_MODULE_PREFIX MDIO_ID_FMT, MDIO_ID_ARGS(phy_id));

	device_initialize(&mdiodev->dev);

	return dev;
}
EXPORT_SYMBOL(phy_device_create);

/* get_phy_c45_devs_in_pkg - reads a MMD's devices in package registers.
 * @bus: the target MII bus
 * @addr: PHY address on the MII bus
 * @dev_addr: MMD address in the PHY.
 * @devices_in_package: where to store the devices in package information.
 *
 * Description: reads devices in package registers of a MMD at @dev_addr
 * from PHY at @addr on @bus.
 *
 * Returns: 0 on success, -EIO on failure.
 */
static int get_phy_c45_devs_in_pkg(struct mii_bus *bus, int addr, int dev_addr,
				   u32 *devices_in_package)
{
	int phy_reg, reg_addr;

	reg_addr = MII_ADDR_C45 | dev_addr << 16 | MDIO_DEVS2;
	phy_reg = mdiobus_read(bus, addr, reg_addr);
	if (phy_reg < 0)
		return -EIO;
	*devices_in_package = (phy_reg & 0xffff) << 16;

	reg_addr = MII_ADDR_C45 | dev_addr << 16 | MDIO_DEVS1;
	phy_reg = mdiobus_read(bus, addr, reg_addr);
	if (phy_reg < 0)
		return -EIO;
	*devices_in_package |= (phy_reg & 0xffff);

	return 0;
}

/**
 * get_phy_c45_ids - reads the specified addr for its 802.3-c45 IDs.
 * @bus: the target MII bus
 * @addr: PHY address on the MII bus
 * @phy_id: where to store the ID retrieved.
 * @c45_ids: where to store the c45 ID information.
 *
 *   If the PHY devices-in-package appears to be valid, it and the
 *   corresponding identifiers are stored in @c45_ids, zero is stored
 *   in @phy_id.  Otherwise 0xffffffff is stored in @phy_id.  Returns
 *   zero on success.
 *
 */
static int get_phy_c45_ids(struct mii_bus *bus, int addr, u32 *phy_id,
			   struct phy_c45_device_ids *c45_ids) {
	int phy_reg;
	int i, reg_addr;
	const int num_ids = ARRAY_SIZE(c45_ids->device_ids);
	u32 *devs = &c45_ids->devices_in_package;

	/* Find first non-zero Devices In package. Device zero is reserved
	 * for 802.3 c45 complied PHYs, so don't probe it at first.
	 */
	for (i = 1; i < num_ids && *devs == 0; i++) {
		phy_reg = get_phy_c45_devs_in_pkg(bus, addr, i, devs);
		if (phy_reg < 0)
			return -EIO;

		if ((*devs & 0x1fffffff) == 0x1fffffff) {
			/*  If mostly Fs, there is no device there,
			 *  then let's continue to probe more, as some
			 *  10G PHYs have zero Devices In package,
			 *  e.g. Cortina CS4315/CS4340 PHY.
			 */
			phy_reg = get_phy_c45_devs_in_pkg(bus, addr, 0, devs);
			if (phy_reg < 0)
				return -EIO;
			/* no device there, let's get out of here */
			if ((*devs & 0x1fffffff) == 0x1fffffff) {
				*phy_id = 0xffffffff;
				return 0;
			} else {
				break;
			}
		}
	}

	/* Now probe Device Identifiers for each device present. */
	for (i = 1; i < num_ids; i++) {
		if (!(c45_ids->devices_in_package & (1 << i)))
			continue;

		reg_addr = MII_ADDR_C45 | i << 16 | MII_PHYSID1;
		phy_reg = mdiobus_read(bus, addr, reg_addr);
		if (phy_reg < 0)
			return -EIO;
		c45_ids->device_ids[i] = (phy_reg & 0xffff) << 16;

		reg_addr = MII_ADDR_C45 | i << 16 | MII_PHYSID2;
		phy_reg = mdiobus_read(bus, addr, reg_addr);
		if (phy_reg < 0)
			return -EIO;
		c45_ids->device_ids[i] |= (phy_reg & 0xffff);
	}
	*phy_id = 0;
	return 0;
}

/**
 * get_phy_id - reads the specified addr for its ID.
 * @bus: the target MII bus
 * @addr: PHY address on the MII bus
 * @phy_id: where to store the ID retrieved.
 * @is_c45: If true the PHY uses the 802.3 clause 45 protocol
 * @c45_ids: where to store the c45 ID information.
 *
 * Description: In the case of a 802.3-c22 PHY, reads the ID registers
 *   of the PHY at @addr on the @bus, stores it in @phy_id and returns
 *   zero on success.
 *
 *   In the case of a 802.3-c45 PHY, get_phy_c45_ids() is invoked, and
 *   its return value is in turn returned.
 *
 */
static int get_phy_id(struct mii_bus *bus, int addr, u32 *phy_id,
		      bool is_c45, struct phy_c45_device_ids *c45_ids)
{
	int phy_reg;

	if (is_c45)
		return get_phy_c45_ids(bus, addr, phy_id, c45_ids);

	/* Grab the bits from PHYIR1, and put them in the upper half */
	phy_reg = mdiobus_read(bus, addr, MII_PHYSID1);
	if (phy_reg < 0) {
		/* if there is no device, return without an error so scanning
		 * the bus works properly
		 */
		if (phy_reg == -EIO || phy_reg == -ENODEV) {
			*phy_id = 0xffffffff;
			return 0;
		}

		return -EIO;
	}

	*phy_id = (phy_reg & 0xffff) << 16;

	/* Grab the bits from PHYIR2, and put them in the lower half */
	phy_reg = mdiobus_read(bus, addr, MII_PHYSID2);
	if (phy_reg < 0)
		return -EIO;

	*phy_id |= (phy_reg & 0xffff);

	return 0;
}

/**
 * get_phy_device - reads the specified PHY device and returns its @phy_device
 *		    struct
 * @bus: the target MII bus
 * @addr: PHY address on the MII bus
 * @is_c45: If true the PHY uses the 802.3 clause 45 protocol
 *
 * Description: Reads the ID registers of the PHY at @addr on the
 *   @bus, then allocates and returns the phy_device to represent it.
 */
struct phy_device *get_phy_device(struct mii_bus *bus, int addr, bool is_c45)
{
	struct phy_c45_device_ids c45_ids = {0};
	u32 phy_id = 0;
	int r;

	r = get_phy_id(bus, addr, &phy_id, is_c45, &c45_ids);
	if (r)
		return ERR_PTR(r);

	/* If the phy_id is mostly Fs, there is no device there */
	if ((phy_id & 0x1fffffff) == 0x1fffffff)
		return ERR_PTR(-ENODEV);

	return phy_device_create(bus, addr, phy_id, is_c45, &c45_ids);
}
EXPORT_SYMBOL(get_phy_device);

/**
 * phy_device_register - Register the phy device on the MDIO bus
 * @phydev: phy_device structure to be added to the MDIO bus
 */
int phy_device_register(struct phy_device *phydev)
{
	int err;

	err = mdiobus_register_device(&phydev->mdio);
	if (err)
		return err;

	/* Deassert the reset signal */
	phy_device_reset(phydev, 0);

	/* Run all of the fixups for this PHY */
	err = phy_scan_fixups(phydev);
	if (err) {
		pr_err("PHY %d failed to initialize\n", phydev->mdio.addr);
		goto out;
	}

	err = device_add(&phydev->mdio.dev);
	if (err) {
		pr_err("PHY %d failed to add\n", phydev->mdio.addr);
		goto out;
	}

	return 0;

 out:
	/* Assert the reset signal */
	phy_device_reset(phydev, 1);

	mdiobus_unregister_device(&phydev->mdio);
	return err;
}
EXPORT_SYMBOL(phy_device_register);

/**
 * phy_device_remove - Remove a previously registered phy device from the MDIO bus
 * @phydev: phy_device structure to remove
 *
 * This doesn't free the phy_device itself, it merely reverses the effects
 * of phy_device_register(). Use phy_device_free() to free the device
 * after calling this function.
 */
void phy_device_remove(struct phy_device *phydev)
{
	device_del(&phydev->mdio.dev);

	/* Assert the reset signal */
	phy_device_reset(phydev, 1);

	mdiobus_unregister_device(&phydev->mdio);
}
EXPORT_SYMBOL(phy_device_remove);

/**
 * phy_find_first - finds the first PHY device on the bus
 * @bus: the target MII bus
 */
struct phy_device *phy_find_first(struct mii_bus *bus)
{
	struct phy_device *phydev;
	int addr;

	for (addr = 0; addr < PHY_MAX_ADDR; addr++) {
		phydev = mdiobus_get_phy(bus, addr);
		if (phydev)
			return phydev;
	}
	return NULL;
}
EXPORT_SYMBOL(phy_find_first);

static void phy_link_change(struct phy_device *phydev, bool up, bool do_carrier)
{
	struct net_device *netdev = phydev->attached_dev;

	if (do_carrier) {
		if (up)
			netif_carrier_on(netdev);
		else
			netif_carrier_off(netdev);
	}
	phydev->adjust_link(netdev);
}

/**
 * phy_prepare_link - prepares the PHY layer to monitor link status
 * @phydev: target phy_device struct
 * @handler: callback function for link status change notifications
 *
 * Description: Tells the PHY infrastructure to handle the
 *   gory details on monitoring link status (whether through
 *   polling or an interrupt), and to call back to the
 *   connected device driver when the link status changes.
 *   If you want to monitor your own link state, don't call
 *   this function.
 */
static void phy_prepare_link(struct phy_device *phydev,
			     void (*handler)(struct net_device *))
{
	phydev->adjust_link = handler;
}

/**
 * phy_connect_direct - connect an ethernet device to a specific phy_device
 * @dev: the network device to connect
 * @phydev: the pointer to the phy device
 * @handler: callback function for state change notifications
 * @interface: PHY device's interface
 */
int phy_connect_direct(struct net_device *dev, struct phy_device *phydev,
		       void (*handler)(struct net_device *),
		       phy_interface_t interface)
{
	int rc;

	if (!dev)
		return -EINVAL;

	rc = phy_attach_direct(dev, phydev, phydev->dev_flags, interface);
	if (rc)
		return rc;

	phy_prepare_link(phydev, handler);
	phy_start_machine(phydev);
	if (phydev->irq > 0)
		phy_start_interrupts(phydev);

	return 0;
}
EXPORT_SYMBOL(phy_connect_direct);

/**
 * phy_connect - connect an ethernet device to a PHY device
 * @dev: the network device to connect
 * @bus_id: the id string of the PHY device to connect
 * @handler: callback function for state change notifications
 * @interface: PHY device's interface
 *
 * Description: Convenience function for connecting ethernet
 *   devices to PHY devices.  The default behavior is for
 *   the PHY infrastructure to handle everything, and only notify
 *   the connected driver when the link status changes.  If you
 *   don't want, or can't use the provided functionality, you may
 *   choose to call only the subset of functions which provide
 *   the desired functionality.
 */
struct phy_device *phy_connect(struct net_device *dev, const char *bus_id,
			       void (*handler)(struct net_device *),
			       phy_interface_t interface)
{
	struct phy_device *phydev;
	struct device *d;
	int rc;

	/* Search the list of PHY devices on the mdio bus for the
	 * PHY with the requested name
	 */
	d = bus_find_device_by_name(&mdio_bus_type, NULL, bus_id);
	if (!d) {
		pr_err("PHY %s not found\n", bus_id);
		return ERR_PTR(-ENODEV);
	}
	phydev = to_phy_device(d);

	rc = phy_connect_direct(dev, phydev, handler, interface);
	put_device(d);
	if (rc)
		return ERR_PTR(rc);

	return phydev;
}
EXPORT_SYMBOL(phy_connect);

/**
 * phy_disconnect - disable interrupts, stop state machine, and detach a PHY
 *		    device
 * @phydev: target phy_device struct
 */
void phy_disconnect(struct phy_device *phydev)
{
	if (phydev->irq > 0)
		phy_stop_interrupts(phydev);

	phydev->adjust_link = NULL;

	phy_detach(phydev);
}
EXPORT_SYMBOL(phy_disconnect);

/**
 * phy_poll_reset - Safely wait until a PHY reset has properly completed
 * @phydev: The PHY device to poll
 *
 * Description: According to IEEE 802.3, Section 2, Subsection 22.2.4.1.1, as
 *   published in 2008, a PHY reset may take up to 0.5 seconds.  The MII BMCR
 *   register must be polled until the BMCR_RESET bit clears.
 *
 *   Furthermore, any attempts to write to PHY registers may have no effect
 *   or even generate MDIO bus errors until this is complete.
 *
 *   Some PHYs (such as the Marvell 88E1111) don't entirely conform to the
 *   standard and do not fully reset after the BMCR_RESET bit is set, and may
 *   even *REQUIRE* a soft-reset to properly restart autonegotiation.  In an
 *   effort to support such broken PHYs, this function is separate from the
 *   standard phy_init_hw() which will zero all the other bits in the BMCR
 *   and reapply all driver-specific and board-specific fixups.
 */
static int phy_poll_reset(struct phy_device *phydev)
{
	/* Poll until the reset bit clears (50ms per retry == 0.6 sec) */
	unsigned int retries = 12;
	int ret;

	do {
		msleep(50);
		ret = phy_read(phydev, MII_BMCR);
		if (ret < 0)
			return ret;
	} while (ret & BMCR_RESET && --retries);
	if (ret & BMCR_RESET)
		return -ETIMEDOUT;

	/* Some chips (smsc911x) may still need up to another 1ms after the
	 * BMCR_RESET bit is cleared before they are usable.
	 */
	msleep(1);
	return 0;
}

int phy_init_hw(struct phy_device *phydev)
{
	int ret = 0;

	/* Deassert the reset signal */
	phy_device_reset(phydev, 0);

	if (!phydev->drv || !phydev->drv->config_init)
		return 0;

	if (phydev->drv->soft_reset)
		ret = phydev->drv->soft_reset(phydev);

	if (ret < 0)
		return ret;

	ret = phy_scan_fixups(phydev);
	if (ret < 0)
		return ret;

	return phydev->drv->config_init(phydev);
}
EXPORT_SYMBOL(phy_init_hw);

void phy_attached_info(struct phy_device *phydev)
{
	phy_attached_print(phydev, NULL);
}
EXPORT_SYMBOL(phy_attached_info);

#define ATTACHED_FMT "attached PHY driver [%s] (mii_bus:phy_addr=%s, irq=%s)"
void phy_attached_print(struct phy_device *phydev, const char *fmt, ...)
{
	const char *drv_name = phydev->drv ? phydev->drv->name : "unbound";
	char *irq_str;
	char irq_num[8];

	switch(phydev->irq) {
	case PHY_POLL:
		irq_str = "POLL";
		break;
	case PHY_IGNORE_INTERRUPT:
		irq_str = "IGNORE";
		break;
	default:
		snprintf(irq_num, sizeof(irq_num), "%d", phydev->irq);
		irq_str = irq_num;
		break;
	}


	if (!fmt) {
		dev_info(&phydev->mdio.dev, ATTACHED_FMT "\n",
			 drv_name, phydev_name(phydev),
			 irq_str);
	} else {
		va_list ap;

		dev_info(&phydev->mdio.dev, ATTACHED_FMT,
			 drv_name, phydev_name(phydev),
			 irq_str);

		va_start(ap, fmt);
		vprintk(fmt, ap);
		va_end(ap);
	}
}
EXPORT_SYMBOL(phy_attached_print);

/**
 * phy_attach_direct - attach a network device to a given PHY device pointer
 * @dev: network device to attach
 * @phydev: Pointer to phy_device to attach
 * @flags: PHY device's dev_flags
 * @interface: PHY device's interface
 *
 * Description: Called by drivers to attach to a particular PHY
 *     device. The phy_device is found, and properly hooked up
 *     to the phy_driver.  If no driver is attached, then a
 *     generic driver is used.  The phy_device is given a ptr to
 *     the attaching device, and given a callback for link status
 *     change.  The phy_device is returned to the attaching driver.
 *     This function takes a reference on the phy device.
 */
int phy_attach_direct(struct net_device *dev, struct phy_device *phydev,
		      u32 flags, phy_interface_t interface)
{
	struct module *ndev_owner = dev->dev.parent->driver->owner;
	struct mii_bus *bus = phydev->mdio.bus;
	struct device *d = &phydev->mdio.dev;
	bool using_genphy = false;
	int err;

	/* For Ethernet device drivers that register their own MDIO bus, we
	 * will have bus->owner match ndev_mod, so we do not want to increment
	 * our own module->refcnt here, otherwise we would not be able to
	 * unload later on.
	 */
	if (ndev_owner != bus->owner && !try_module_get(bus->owner)) {
		dev_err(&dev->dev, "failed to get the bus module\n");
		return -EIO;
	}

	get_device(d);

	/* Assume that if there is no driver, that it doesn't
	 * exist, and we should use the genphy driver.
	 */
	if (!d->driver) {
		if (phydev->is_c45)
			d->driver = &genphy_10g_driver.mdiodrv.driver;
		else
			d->driver = &genphy_driver.mdiodrv.driver;

		using_genphy = true;
	}

	if (!try_module_get(d->driver->owner)) {
		dev_err(&dev->dev, "failed to get the device driver module\n");
		err = -EIO;
		goto error_put_device;
	}

	if (using_genphy) {
		err = d->driver->probe(d);
		if (err >= 0)
			err = device_bind_driver(d);

		if (err)
			goto error_module_put;
	}

	if (phydev->attached_dev) {
		dev_err(&dev->dev, "PHY already attached\n");
		err = -EBUSY;
		goto error;
	}

	phydev->phy_link_change = phy_link_change;
	phydev->attached_dev = dev;
	dev->phydev = phydev;

	/* Some Ethernet drivers try to connect to a PHY device before
	 * calling register_netdevice() -> netdev_register_kobject() and
	 * does the dev->dev.kobj initialization. Here we only check for
	 * success which indicates that the network device kobject is
	 * ready. Once we do that we still need to keep track of whether
	 * links were successfully set up or not for phy_detach() to
	 * remove them accordingly.
	 */
	phydev->sysfs_links = false;

	err = sysfs_create_link(&phydev->mdio.dev.kobj, &dev->dev.kobj,
				"attached_dev");
	if (!err) {
		err = sysfs_create_link_nowarn(&dev->dev.kobj,
					       &phydev->mdio.dev.kobj,
					       "phydev");
		if (err) {
			dev_err(&dev->dev, "could not add device link to %s err %d\n",
				kobject_name(&phydev->mdio.dev.kobj),
				err);
			/* non-fatal - some net drivers can use one netdevice
			 * with more then one phy
			 */
		}

		phydev->sysfs_links = true;
	}

	phydev->dev_flags = flags;

	phydev->interface = interface;

	phydev->state = PHY_READY;

	/* Initial carrier state is off as the phy is about to be
	 * (re)initialized.
	 */
	netif_carrier_off(phydev->attached_dev);

	/* Do initial configuration here, now that
	 * we have certain key parameters
	 * (dev_flags and interface)
	 */
	err = phy_init_hw(phydev);
	if (err)
		goto error;

	phy_resume(phydev);
	phy_led_triggers_register(phydev);

	return err;

error:
	/* phy_detach() does all of the cleanup below */
	phy_detach(phydev);
	return err;

error_module_put:
	module_put(d->driver->owner);
	d->driver = NULL;
error_put_device:
	put_device(d);
	if (ndev_owner != bus->owner)
		module_put(bus->owner);
	return err;
}
EXPORT_SYMBOL(phy_attach_direct);

/**
 * phy_attach - attach a network device to a particular PHY device
 * @dev: network device to attach
 * @bus_id: Bus ID of PHY device to attach
 * @interface: PHY device's interface
 *
 * Description: Same as phy_attach_direct() except that a PHY bus_id
 *     string is passed instead of a pointer to a struct phy_device.
 */
struct phy_device *phy_attach(struct net_device *dev, const char *bus_id,
			      phy_interface_t interface)
{
	struct bus_type *bus = &mdio_bus_type;
	struct phy_device *phydev;
	struct device *d;
	int rc;

	if (!dev)
		return ERR_PTR(-EINVAL);

	/* Search the list of PHY devices on the mdio bus for the
	 * PHY with the requested name
	 */
	d = bus_find_device_by_name(bus, NULL, bus_id);
	if (!d) {
		pr_err("PHY %s not found\n", bus_id);
		return ERR_PTR(-ENODEV);
	}
	phydev = to_phy_device(d);

	rc = phy_attach_direct(dev, phydev, phydev->dev_flags, interface);
	put_device(d);
	if (rc)
		return ERR_PTR(rc);

	return phydev;
}
EXPORT_SYMBOL(phy_attach);

/**
 * phy_detach - detach a PHY device from its network device
 * @phydev: target phy_device struct
 *
 * This detaches the phy device from its network device and the phy
 * driver, and drops the reference count taken in phy_attach_direct().
 */
void phy_detach(struct phy_device *phydev)
{
	struct net_device *dev = phydev->attached_dev;
	struct module *ndev_owner = dev->dev.parent->driver->owner;
	struct mii_bus *bus;

	if (phydev->sysfs_links) {
		sysfs_remove_link(&dev->dev.kobj, "phydev");
		sysfs_remove_link(&phydev->mdio.dev.kobj, "attached_dev");
	}
	phy_suspend(phydev);
	phydev->attached_dev->phydev = NULL;
	phydev->attached_dev = NULL;
	phydev->phylink = NULL;

	phy_led_triggers_unregister(phydev);

	module_put(phydev->mdio.dev.driver->owner);

	/* If the device had no specific driver before (i.e. - it
	 * was using the generic driver), we unbind the device
	 * from the generic driver so that there's a chance a
	 * real driver could be loaded
	 */
	if (phydev->mdio.dev.driver == &genphy_10g_driver.mdiodrv.driver ||
	    phydev->mdio.dev.driver == &genphy_driver.mdiodrv.driver)
		device_release_driver(&phydev->mdio.dev);

	/* Assert the reset signal */
	phy_device_reset(phydev, 1);

	/*
	 * The phydev might go away on the put_device() below, so avoid
	 * a use-after-free bug by reading the underlying bus first.
	 */
	bus = phydev->mdio.bus;

	put_device(&phydev->mdio.dev);
	if (ndev_owner != bus->owner)
		module_put(bus->owner);
}
EXPORT_SYMBOL(phy_detach);

int phy_suspend(struct phy_device *phydev)
{
	struct phy_driver *phydrv = to_phy_driver(phydev->mdio.dev.driver);
	struct net_device *netdev = phydev->attached_dev;
	struct ethtool_wolinfo wol = { .cmd = ETHTOOL_GWOL };
	int ret = 0;

	/* If the device has WOL enabled, we cannot suspend the PHY */
	phy_ethtool_get_wol(phydev, &wol);
	if (wol.wolopts || (netdev && netdev->wol_enabled))
		return -EBUSY;

	if (phydev->drv && phydrv->suspend)
		ret = phydrv->suspend(phydev);

	if (ret)
		return ret;

	phydev->suspended = true;

	return ret;
}
EXPORT_SYMBOL(phy_suspend);

int __phy_resume(struct phy_device *phydev)
{
	struct phy_driver *phydrv = to_phy_driver(phydev->mdio.dev.driver);
	int ret = 0;

	WARN_ON(!mutex_is_locked(&phydev->lock));

	if (phydev->drv && phydrv->resume)
		ret = phydrv->resume(phydev);

	if (ret)
		return ret;

	phydev->suspended = false;

	return ret;
}
EXPORT_SYMBOL(__phy_resume);

int phy_resume(struct phy_device *phydev)
{
	int ret;

	mutex_lock(&phydev->lock);
	ret = __phy_resume(phydev);
	mutex_unlock(&phydev->lock);

	return ret;
}
EXPORT_SYMBOL(phy_resume);

int phy_loopback(struct phy_device *phydev, bool enable)
{
	struct phy_driver *phydrv = to_phy_driver(phydev->mdio.dev.driver);
	int ret = 0;

	mutex_lock(&phydev->lock);

	if (enable && phydev->loopback_enabled) {
		ret = -EBUSY;
		goto out;
	}

	if (!enable && !phydev->loopback_enabled) {
		ret = -EINVAL;
		goto out;
	}

	if (phydev->drv && phydrv->set_loopback)
		ret = phydrv->set_loopback(phydev, enable);
	else
		ret = -EOPNOTSUPP;

	if (ret)
		goto out;

	phydev->loopback_enabled = enable;

out:
	mutex_unlock(&phydev->lock);
	return ret;
}
EXPORT_SYMBOL(phy_loopback);

/**
 * phy_reset_after_clk_enable - perform a PHY reset if needed
 * @phydev: target phy_device struct
 *
 * Description: Some PHYs are known to need a reset after their refclk was
 *   enabled. This function evaluates the flags and perform the reset if it's
 *   needed. Returns < 0 on error, 0 if the phy wasn't reset and 1 if the phy
 *   was reset.
 */
int phy_reset_after_clk_enable(struct phy_device *phydev)
{
	if (!phydev || !phydev->drv)
		return -ENODEV;

	if (phydev->drv->flags & PHY_RST_AFTER_CLK_EN) {
		phy_device_reset(phydev, 1);
		phy_device_reset(phydev, 0);
		return 1;
	}

	return 0;
}
EXPORT_SYMBOL(phy_reset_after_clk_enable);

/* Generic PHY support and helper functions */

/**
 * genphy_config_advert - sanitize and advertise auto-negotiation parameters
 * @phydev: target phy_device struct
 *
 * Description: Writes MII_ADVERTISE with the appropriate values,
 *   after sanitizing the values to make sure we only advertise
 *   what is supported.  Returns < 0 on error, 0 if the PHY's advertisement
 *   hasn't changed, and > 0 if it has changed.
 */
static int genphy_config_advert(struct phy_device *phydev)
{
	u32 advertise;
	int oldadv, adv, bmsr;
	int err, changed = 0;

	/* Only allow advertising what this PHY supports */
	phydev->advertising &= phydev->supported;
	advertise = phydev->advertising;

	/* Setup standard advertisement */
	adv = phy_read(phydev, MII_ADVERTISE);
	if (adv < 0)
		return adv;

	oldadv = adv;
	adv &= ~(ADVERTISE_ALL | ADVERTISE_100BASE4 | ADVERTISE_PAUSE_CAP |
		 ADVERTISE_PAUSE_ASYM);
	adv |= ethtool_adv_to_mii_adv_t(advertise);

	if (adv != oldadv) {
		err = phy_write(phydev, MII_ADVERTISE, adv);

		if (err < 0)
			return err;
		changed = 1;
	}

	bmsr = phy_read(phydev, MII_BMSR);
	if (bmsr < 0)
		return bmsr;

	/* Per 802.3-2008, Section 22.2.4.2.16 Extended status all
	 * 1000Mbits/sec capable PHYs shall have the BMSR_ESTATEN bit set to a
	 * logical 1.
	 */
	if (!(bmsr & BMSR_ESTATEN))
		return changed;

	/* Configure gigabit if it's supported */
	adv = phy_read(phydev, MII_CTRL1000);
	if (adv < 0)
		return adv;

	oldadv = adv;
	adv &= ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);

	if (phydev->supported & (SUPPORTED_1000baseT_Half |
				 SUPPORTED_1000baseT_Full)) {
		adv |= ethtool_adv_to_mii_ctrl1000_t(advertise);
	}

	if (adv != oldadv)
		changed = 1;

	err = phy_write(phydev, MII_CTRL1000, adv);
	if (err < 0)
		return err;

	return changed;
}

/**
 * genphy_config_eee_advert - disable unwanted eee mode advertisement
 * @phydev: target phy_device struct
 *
 * Description: Writes MDIO_AN_EEE_ADV after disabling unsupported energy
 *   efficent ethernet modes. Returns 0 if the PHY's advertisement hasn't
 *   changed, and 1 if it has changed.
 */
static int genphy_config_eee_advert(struct phy_device *phydev)
{
	int broken = phydev->eee_broken_modes;
	int old_adv, adv;

	/* Nothing to disable */
	if (!broken)
		return 0;

	/* If the following call fails, we assume that EEE is not
	 * supported by the phy. If we read 0, EEE is not advertised
	 * In both case, we don't need to continue
	 */
	adv = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_EEE_ADV);
	if (adv <= 0)
		return 0;

	old_adv = adv;
	adv &= ~broken;

	/* Advertising remains unchanged with the broken mask */
	if (old_adv == adv)
		return 0;

	phy_write_mmd(phydev, MDIO_MMD_AN, MDIO_AN_EEE_ADV, adv);

	return 1;
}

/**
 * genphy_setup_forced - configures/forces speed/duplex from @phydev
 * @phydev: target phy_device struct
 *
 * Description: Configures MII_BMCR to force speed/duplex
 *   to the values in phydev. Assumes that the values are valid.
 *   Please see phy_sanitize_settings().
 */
int genphy_setup_forced(struct phy_device *phydev)
{
	u16 ctl = 0;

	phydev->pause = 0;
	phydev->asym_pause = 0;

	if (SPEED_1000 == phydev->speed)
		ctl |= BMCR_SPEED1000;
	else if (SPEED_100 == phydev->speed)
		ctl |= BMCR_SPEED100;

	if (DUPLEX_FULL == phydev->duplex)
		ctl |= BMCR_FULLDPLX;

	return phy_modify(phydev, MII_BMCR,
			  ~(BMCR_LOOPBACK | BMCR_ISOLATE | BMCR_PDOWN), ctl);
}
EXPORT_SYMBOL(genphy_setup_forced);

/**
 * genphy_restart_aneg - Enable and Restart Autonegotiation
 * @phydev: target phy_device struct
 */
int genphy_restart_aneg(struct phy_device *phydev)
{
	/* Don't isolate the PHY if we're negotiating */
	return phy_modify(phydev, MII_BMCR, BMCR_ISOLATE,
			  BMCR_ANENABLE | BMCR_ANRESTART);
}
EXPORT_SYMBOL(genphy_restart_aneg);

/**
 * genphy_config_aneg - restart auto-negotiation or write BMCR
 * @phydev: target phy_device struct
 *
 * Description: If auto-negotiation is enabled, we configure the
 *   advertising, and then restart auto-negotiation.  If it is not
 *   enabled, then we write the BMCR.
 */
int genphy_config_aneg(struct phy_device *phydev)
{
	int err, changed;

	changed = genphy_config_eee_advert(phydev);

	if (AUTONEG_ENABLE != phydev->autoneg)
		return genphy_setup_forced(phydev);

	err = genphy_config_advert(phydev);
	if (err < 0) /* error */
		return err;

	changed |= err;

	if (changed == 0) {
		/* Advertisement hasn't changed, but maybe aneg was never on to
		 * begin with?  Or maybe phy was isolated?
		 */
		int ctl = phy_read(phydev, MII_BMCR);

		if (ctl < 0)
			return ctl;

		if (!(ctl & BMCR_ANENABLE) || (ctl & BMCR_ISOLATE))
			changed = 1; /* do restart aneg */
	}

	/* Only restart aneg if we are advertising something different
	 * than we were before.
	 */
	if (changed > 0)
		return genphy_restart_aneg(phydev);

	return 0;
}
EXPORT_SYMBOL(genphy_config_aneg);

/**
 * genphy_aneg_done - return auto-negotiation status
 * @phydev: target phy_device struct
 *
 * Description: Reads the status register and returns 0 either if
 *   auto-negotiation is incomplete, or if there was an error.
 *   Returns BMSR_ANEGCOMPLETE if auto-negotiation is done.
 */
int genphy_aneg_done(struct phy_device *phydev)
{
	int retval = phy_read(phydev, MII_BMSR);

	return (retval < 0) ? retval : (retval & BMSR_ANEGCOMPLETE);
}
EXPORT_SYMBOL(genphy_aneg_done);

/**
 * genphy_update_link - update link status in @phydev
 * @phydev: target phy_device struct
 *
 * Description: Update the value in phydev->link to reflect the
 *   current link value.  In order to do this, we need to read
 *   the status register twice, keeping the second value.
 */
int genphy_update_link(struct phy_device *phydev)
{
	int status;

	/* The link state is latched low so that momentary link
	 * drops can be detected. Do not double-read the status
	 * in polling mode to detect such short link drops.
	 */
	if (!phy_polling_mode(phydev)) {
		status = phy_read(phydev, MII_BMSR);
		if (status < 0)
			return status;
	}

	/* Read link and autonegotiation status */
	status = phy_read(phydev, MII_BMSR);
	if (status < 0)
		return status;

	if ((status & BMSR_LSTATUS) == 0)
		phydev->link = 0;
	else
		phydev->link = 1;

	return 0;
}
EXPORT_SYMBOL(genphy_update_link);

/**
 * genphy_read_status - check the link status and update current link state
 * @phydev: target phy_device struct
 *
 * Description: Check the link, then figure out the current state
 *   by comparing what we advertise with what the link partner
 *   advertises.  Start by checking the gigabit possibilities,
 *   then move on to 10/100.
 */
int genphy_read_status(struct phy_device *phydev)
{
	int adv;
	int err;
	int lpa;
	int lpagb = 0;
	int common_adv;
	int common_adv_gb = 0;

	/* Update the link, but return if there was an error */
	err = genphy_update_link(phydev);
	if (err)
		return err;

	phydev->lp_advertising = 0;

	if (AUTONEG_ENABLE == phydev->autoneg) {
		if (phydev->supported & (SUPPORTED_1000baseT_Half
					| SUPPORTED_1000baseT_Full)) {
			lpagb = phy_read(phydev, MII_STAT1000);
			if (lpagb < 0)
				return lpagb;

			adv = phy_read(phydev, MII_CTRL1000);
			if (adv < 0)
				return adv;

			if (lpagb & LPA_1000MSFAIL) {
				if (adv & CTL1000_ENABLE_MASTER)
					phydev_err(phydev, "Master/Slave resolution failed, maybe conflicting manual settings?\n");
				else
					phydev_err(phydev, "Master/Slave resolution failed\n");
				return -ENOLINK;
			}

			phydev->lp_advertising =
				mii_stat1000_to_ethtool_lpa_t(lpagb);
			common_adv_gb = lpagb & adv << 2;
		}

		lpa = phy_read(phydev, MII_LPA);
		if (lpa < 0)
			return lpa;

		phydev->lp_advertising |= mii_lpa_to_ethtool_lpa_t(lpa);

		adv = phy_read(phydev, MII_ADVERTISE);
		if (adv < 0)
			return adv;

		common_adv = lpa & adv;

		phydev->speed = SPEED_10;
		phydev->duplex = DUPLEX_HALF;
		phydev->pause = 0;
		phydev->asym_pause = 0;

		if (common_adv_gb & (LPA_1000FULL | LPA_1000HALF)) {
			phydev->speed = SPEED_1000;

			if (common_adv_gb & LPA_1000FULL)
				phydev->duplex = DUPLEX_FULL;
		} else if (common_adv & (LPA_100FULL | LPA_100HALF)) {
			phydev->speed = SPEED_100;

			if (common_adv & LPA_100FULL)
				phydev->duplex = DUPLEX_FULL;
		} else
			if (common_adv & LPA_10FULL)
				phydev->duplex = DUPLEX_FULL;

		if (phydev->duplex == DUPLEX_FULL) {
			phydev->pause = lpa & LPA_PAUSE_CAP ? 1 : 0;
			phydev->asym_pause = lpa & LPA_PAUSE_ASYM ? 1 : 0;
		}
	} else {
		int bmcr = phy_read(phydev, MII_BMCR);

		if (bmcr < 0)
			return bmcr;

		if (bmcr & BMCR_FULLDPLX)
			phydev->duplex = DUPLEX_FULL;
		else
			phydev->duplex = DUPLEX_HALF;

		if (bmcr & BMCR_SPEED1000)
			phydev->speed = SPEED_1000;
		else if (bmcr & BMCR_SPEED100)
			phydev->speed = SPEED_100;
		else
			phydev->speed = SPEED_10;

		phydev->pause = 0;
		phydev->asym_pause = 0;
	}

	return 0;
}
EXPORT_SYMBOL(genphy_read_status);

/**
 * genphy_soft_reset - software reset the PHY via BMCR_RESET bit
 * @phydev: target phy_device struct
 *
 * Description: Perform a software PHY reset using the standard
 * BMCR_RESET bit and poll for the reset bit to be cleared.
 *
 * Returns: 0 on success, < 0 on failure
 */
int genphy_soft_reset(struct phy_device *phydev)
{
	int ret;

	ret = phy_write(phydev, MII_BMCR, BMCR_RESET);
	if (ret < 0)
		return ret;

	return phy_poll_reset(phydev);
}
EXPORT_SYMBOL(genphy_soft_reset);

int genphy_config_init(struct phy_device *phydev)
{
	int val;
	u32 features;

	features = (SUPPORTED_TP | SUPPORTED_MII
			| SUPPORTED_AUI | SUPPORTED_FIBRE |
			SUPPORTED_BNC | SUPPORTED_Pause | SUPPORTED_Asym_Pause);

	/* Do we support autonegotiation? */
	val = phy_read(phydev, MII_BMSR);
	if (val < 0)
		return val;

	if (val & BMSR_ANEGCAPABLE)
		features |= SUPPORTED_Autoneg;

	if (val & BMSR_100FULL)
		features |= SUPPORTED_100baseT_Full;
	if (val & BMSR_100HALF)
		features |= SUPPORTED_100baseT_Half;
	if (val & BMSR_10FULL)
		features |= SUPPORTED_10baseT_Full;
	if (val & BMSR_10HALF)
		features |= SUPPORTED_10baseT_Half;

	if (val & BMSR_ESTATEN) {
		val = phy_read(phydev, MII_ESTATUS);
		if (val < 0)
			return val;

		if (val & ESTATUS_1000_TFULL)
			features |= SUPPORTED_1000baseT_Full;
		if (val & ESTATUS_1000_THALF)
			features |= SUPPORTED_1000baseT_Half;
	}

	phydev->supported &= features;
	phydev->advertising &= features;

	return 0;
}
EXPORT_SYMBOL(genphy_config_init);

/* This is used for the phy device which doesn't support the MMD extended
 * register access, but it does have side effect when we are trying to access
 * the MMD register via indirect method.
 */
int genphy_read_mmd_unsupported(struct phy_device *phdev, int devad, u16 regnum)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(genphy_read_mmd_unsupported);

int genphy_write_mmd_unsupported(struct phy_device *phdev, int devnum,
				 u16 regnum, u16 val)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(genphy_write_mmd_unsupported);

int genphy_suspend(struct phy_device *phydev)
{
	return phy_set_bits(phydev, MII_BMCR, BMCR_PDOWN);
}
EXPORT_SYMBOL(genphy_suspend);

int genphy_resume(struct phy_device *phydev)
{
	return phy_clear_bits(phydev, MII_BMCR, BMCR_PDOWN);
}
EXPORT_SYMBOL(genphy_resume);

int genphy_loopback(struct phy_device *phydev, bool enable)
{
	return phy_modify(phydev, MII_BMCR, BMCR_LOOPBACK,
			  enable ? BMCR_LOOPBACK : 0);
}
EXPORT_SYMBOL(genphy_loopback);

static int __set_phy_supported(struct phy_device *phydev, u32 max_speed)
{
	switch (max_speed) {
	case SPEED_10:
		phydev->supported &= ~PHY_100BT_FEATURES;
		/* fall through */
	case SPEED_100:
		phydev->supported &= ~PHY_1000BT_FEATURES;
		break;
	case SPEED_1000:
		break;
	default:
		return -ENOTSUPP;
	}

	return 0;
}

int phy_set_max_speed(struct phy_device *phydev, u32 max_speed)
{
	int err;

	err = __set_phy_supported(phydev, max_speed);
	if (err)
		return err;

	phydev->advertising = phydev->supported;

	return 0;
}
EXPORT_SYMBOL(phy_set_max_speed);

static void of_set_phy_supported(struct phy_device *phydev)
{
	struct device_node *node = phydev->mdio.dev.of_node;
	u32 max_speed;

	if (!IS_ENABLED(CONFIG_OF_MDIO))
		return;

	if (!node)
		return;

	if (!of_property_read_u32(node, "max-speed", &max_speed))
		__set_phy_supported(phydev, max_speed);
}

static void of_set_phy_eee_broken(struct phy_device *phydev)
{
	struct device_node *node = phydev->mdio.dev.of_node;
	u32 broken = 0;

	if (!IS_ENABLED(CONFIG_OF_MDIO))
		return;

	if (!node)
		return;

	if (of_property_read_bool(node, "eee-broken-100tx"))
		broken |= MDIO_EEE_100TX;
	if (of_property_read_bool(node, "eee-broken-1000t"))
		broken |= MDIO_EEE_1000T;
	if (of_property_read_bool(node, "eee-broken-10gt"))
		broken |= MDIO_EEE_10GT;
	if (of_property_read_bool(node, "eee-broken-1000kx"))
		broken |= MDIO_EEE_1000KX;
	if (of_property_read_bool(node, "eee-broken-10gkx4"))
		broken |= MDIO_EEE_10GKX4;
	if (of_property_read_bool(node, "eee-broken-10gkr"))
		broken |= MDIO_EEE_10GKR;

	phydev->eee_broken_modes = broken;
}

/**
 * phy_probe - probe and init a PHY device
 * @dev: device to probe and init
 *
 * Description: Take care of setting up the phy_device structure,
 *   set the state to READY (the driver's init function should
 *   set it to STARTING if needed).
 */
static int phy_probe(struct device *dev)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct device_driver *drv = phydev->mdio.dev.driver;
	struct phy_driver *phydrv = to_phy_driver(drv);
	int err = 0;

	phydev->drv = phydrv;

	/* Disable the interrupt if the PHY doesn't support it
	 * but the interrupt is still a valid one
	 */
	if (!(phydrv->flags & PHY_HAS_INTERRUPT) &&
	    phy_interrupt_is_valid(phydev))
		phydev->irq = PHY_POLL;

	if (phydrv->flags & PHY_IS_INTERNAL)
		phydev->is_internal = true;

	mutex_lock(&phydev->lock);

	/* Start out supporting everything. Eventually,
	 * a controller will attach, and may modify one
	 * or both of these values
	 */
	phydev->supported = phydrv->features;
	of_set_phy_supported(phydev);
	phydev->advertising = phydev->supported;

	/* Get the EEE modes we want to prohibit. We will ask
	 * the PHY stop advertising these mode later on
	 */
	of_set_phy_eee_broken(phydev);

	/* The Pause Frame bits indicate that the PHY can support passing
	 * pause frames. During autonegotiation, the PHYs will determine if
	 * they should allow pause frames to pass.  The MAC driver should then
	 * use that result to determine whether to enable flow control via
	 * pause frames.
	 *
	 * Normally, PHY drivers should not set the Pause bits, and instead
	 * allow phylib to do that.  However, there may be some situations
	 * (e.g. hardware erratum) where the driver wants to set only one
	 * of these bits.
	 */
	if (phydrv->features & (SUPPORTED_Pause | SUPPORTED_Asym_Pause)) {
		phydev->supported &= ~(SUPPORTED_Pause | SUPPORTED_Asym_Pause);
		phydev->supported |= phydrv->features &
				     (SUPPORTED_Pause | SUPPORTED_Asym_Pause);
	} else {
		phydev->supported |= SUPPORTED_Pause | SUPPORTED_Asym_Pause;
	}

	/* Set the state to READY by default */
	phydev->state = PHY_READY;

	if (phydev->drv->probe) {
		/* Deassert the reset signal */
		phy_device_reset(phydev, 0);

		err = phydev->drv->probe(phydev);
		if (err) {
			/* Assert the reset signal */
			phy_device_reset(phydev, 1);
		}
	}

	mutex_unlock(&phydev->lock);

	return err;
}

static int phy_remove(struct device *dev)
{
	struct phy_device *phydev = to_phy_device(dev);

	cancel_delayed_work_sync(&phydev->state_queue);

	mutex_lock(&phydev->lock);
	phydev->state = PHY_DOWN;
	mutex_unlock(&phydev->lock);

	if (phydev->drv && phydev->drv->remove) {
		phydev->drv->remove(phydev);

		/* Assert the reset signal */
		phy_device_reset(phydev, 1);
	}
	phydev->drv = NULL;

	return 0;
}

/**
 * phy_driver_register - register a phy_driver with the PHY layer
 * @new_driver: new phy_driver to register
 * @owner: module owning this PHY
 */
int phy_driver_register(struct phy_driver *new_driver, struct module *owner)
{
	int retval;

	new_driver->mdiodrv.flags |= MDIO_DEVICE_IS_PHY;
	new_driver->mdiodrv.driver.name = new_driver->name;
	new_driver->mdiodrv.driver.bus = &mdio_bus_type;
	new_driver->mdiodrv.driver.probe = phy_probe;
	new_driver->mdiodrv.driver.remove = phy_remove;
	new_driver->mdiodrv.driver.owner = owner;

	/* The following works around an issue where the PHY driver doesn't bind
	 * to the device, resulting in the genphy driver being used instead of
	 * the dedicated driver. The root cause of the issue isn't known yet
	 * and seems to be in the base driver core. Once this is fixed we may
	 * remove this workaround.
	 */
	new_driver->mdiodrv.driver.probe_type = PROBE_FORCE_SYNCHRONOUS;

	retval = driver_register(&new_driver->mdiodrv.driver);
	if (retval) {
		pr_err("%s: Error %d in registering driver\n",
		       new_driver->name, retval);

		return retval;
	}

	pr_debug("%s: Registered new driver\n", new_driver->name);

	return 0;
}
EXPORT_SYMBOL(phy_driver_register);

int phy_drivers_register(struct phy_driver *new_driver, int n,
			 struct module *owner)
{
	int i, ret = 0;

	for (i = 0; i < n; i++) {
		ret = phy_driver_register(new_driver + i, owner);
		if (ret) {
			while (i-- > 0)
				phy_driver_unregister(new_driver + i);
			break;
		}
	}
	return ret;
}
EXPORT_SYMBOL(phy_drivers_register);

void phy_driver_unregister(struct phy_driver *drv)
{
	driver_unregister(&drv->mdiodrv.driver);
}
EXPORT_SYMBOL(phy_driver_unregister);

void phy_drivers_unregister(struct phy_driver *drv, int n)
{
	int i;

	for (i = 0; i < n; i++)
		phy_driver_unregister(drv + i);
}
EXPORT_SYMBOL(phy_drivers_unregister);

static struct phy_driver genphy_driver = {
	.phy_id		= 0xffffffff,
	.phy_id_mask	= 0xffffffff,
	.name		= "Generic PHY",
	.soft_reset	= genphy_no_soft_reset,
	.config_init	= genphy_config_init,
	.features	= PHY_GBIT_FEATURES | SUPPORTED_MII |
			  SUPPORTED_AUI | SUPPORTED_FIBRE |
			  SUPPORTED_BNC,
	.aneg_done	= genphy_aneg_done,
	.suspend	= genphy_suspend,
	.resume		= genphy_resume,
	.set_loopback   = genphy_loopback,
};

static int __init phy_init(void)
{
	int rc;

	rc = mdio_bus_init();
	if (rc)
		return rc;

	rc = phy_driver_register(&genphy_10g_driver, THIS_MODULE);
	if (rc)
		goto err_10g;

	rc = phy_driver_register(&genphy_driver, THIS_MODULE);
	if (rc) {
		phy_driver_unregister(&genphy_10g_driver);
err_10g:
		mdio_bus_exit();
	}

	return rc;
}

static void __exit phy_exit(void)
{
	phy_driver_unregister(&genphy_10g_driver);
	phy_driver_unregister(&genphy_driver);
	mdio_bus_exit();
}

subsys_initcall(phy_init);
module_exit(phy_exit);

// SPDX-License-Identifier: GPL-2.0
/*
 *  Probe module for 8250/16550-type Exar chips PCI serial ports.
 *
 *  Based on drivers/tty/serial/8250/8250_pci.c,
 *
 *  Copyright (C) 2017 Sudip Mukherjee, All Rights Reserved.
 */
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/property.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/8250_pci.h>

#include <asm/byteorder.h>

#include "8250.h"

#define PCI_DEVICE_ID_COMMTECH_4224PCI335	0x0002
#define PCI_DEVICE_ID_COMMTECH_4222PCI335	0x0004
#define PCI_DEVICE_ID_COMMTECH_2324PCI335	0x000a
#define PCI_DEVICE_ID_COMMTECH_2328PCI335	0x000b
#define PCI_DEVICE_ID_COMMTECH_4224PCIE		0x0020
#define PCI_DEVICE_ID_COMMTECH_4228PCIE		0x0021
#define PCI_DEVICE_ID_COMMTECH_4222PCIE		0x0022
#define PCI_DEVICE_ID_EXAR_XR17V4358		0x4358
#define PCI_DEVICE_ID_EXAR_XR17V8358		0x8358

#define UART_EXAR_INT0		0x80
#define UART_EXAR_8XMODE	0x88	/* 8X sampling rate select */

#define UART_EXAR_FCTR		0x08	/* Feature Control Register */
#define UART_FCTR_EXAR_IRDA	0x10	/* IrDa data encode select */
#define UART_FCTR_EXAR_485	0x20	/* Auto 485 half duplex dir ctl */
#define UART_FCTR_EXAR_TRGA	0x00	/* FIFO trigger table A */
#define UART_FCTR_EXAR_TRGB	0x60	/* FIFO trigger table B */
#define UART_FCTR_EXAR_TRGC	0x80	/* FIFO trigger table C */
#define UART_FCTR_EXAR_TRGD	0xc0	/* FIFO trigger table D programmable */

#define UART_EXAR_TXTRG		0x0a	/* Tx FIFO trigger level write-only */
#define UART_EXAR_RXTRG		0x0b	/* Rx FIFO trigger level write-only */

#define UART_EXAR_MPIOINT_7_0	0x8f	/* MPIOINT[7:0] */
#define UART_EXAR_MPIOLVL_7_0	0x90	/* MPIOLVL[7:0] */
#define UART_EXAR_MPIO3T_7_0	0x91	/* MPIO3T[7:0] */
#define UART_EXAR_MPIOINV_7_0	0x92	/* MPIOINV[7:0] */
#define UART_EXAR_MPIOSEL_7_0	0x93	/* MPIOSEL[7:0] */
#define UART_EXAR_MPIOOD_7_0	0x94	/* MPIOOD[7:0] */
#define UART_EXAR_MPIOINT_15_8	0x95	/* MPIOINT[15:8] */
#define UART_EXAR_MPIOLVL_15_8	0x96	/* MPIOLVL[15:8] */
#define UART_EXAR_MPIO3T_15_8	0x97	/* MPIO3T[15:8] */
#define UART_EXAR_MPIOINV_15_8	0x98	/* MPIOINV[15:8] */
#define UART_EXAR_MPIOSEL_15_8	0x99	/* MPIOSEL[15:8] */
#define UART_EXAR_MPIOOD_15_8	0x9a	/* MPIOOD[15:8] */

#define UART_EXAR_RS485_DLY(x)	((x) << 4)

/*
 * IOT2040 MPIO wiring semantics:
 *
 * MPIO		Port	Function
 * ----		----	--------
 * 0		2 	Mode bit 0
 * 1		2	Mode bit 1
 * 2		2	Terminate bus
 * 3		-	<reserved>
 * 4		3	Mode bit 0
 * 5		3	Mode bit 1
 * 6		3	Terminate bus
 * 7		-	<reserved>
 * 8		2	Enable
 * 9		3	Enable
 * 10		-	Red LED
 * 11..15	-	<unused>
 */

/* IOT2040 MPIOs 0..7 */
#define IOT2040_UART_MODE_RS232		0x01
#define IOT2040_UART_MODE_RS485		0x02
#define IOT2040_UART_MODE_RS422		0x03
#define IOT2040_UART_TERMINATE_BUS	0x04

#define IOT2040_UART1_MASK		0x0f
#define IOT2040_UART2_SHIFT		4

#define IOT2040_UARTS_DEFAULT_MODE	0x11	/* both RS232 */
#define IOT2040_UARTS_GPIO_LO_MODE	0x88	/* reserved pins as input */

/* IOT2040 MPIOs 8..15 */
#define IOT2040_UARTS_ENABLE		0x03
#define IOT2040_UARTS_GPIO_HI_MODE	0xF8	/* enable & LED as outputs */

struct exar8250;

struct exar8250_platform {
	int (*rs485_config)(struct uart_port *, struct serial_rs485 *);
	int (*register_gpio)(struct pci_dev *, struct uart_8250_port *);
};

/**
 * struct exar8250_board - board information
 * @num_ports: number of serial ports
 * @reg_shift: describes UART register mapping in PCI memory
 * @setup: quirk run at ->probe() stage
 * @exit: quirk run at ->remove() stage
 */
struct exar8250_board {
	unsigned int num_ports;
	unsigned int reg_shift;
	int	(*setup)(struct exar8250 *, struct pci_dev *,
			 struct uart_8250_port *, int);
	void	(*exit)(struct pci_dev *pcidev);
};

struct exar8250 {
	unsigned int		nr;
	struct exar8250_board	*board;
	void __iomem		*virt;
	int			line[0];
};

static int default_setup(struct exar8250 *priv, struct pci_dev *pcidev,
			 int idx, unsigned int offset,
			 struct uart_8250_port *port)
{
	const struct exar8250_board *board = priv->board;
	unsigned int bar = 0;

	port->port.iotype = UPIO_MEM;
	port->port.mapbase = pci_resource_start(pcidev, bar) + offset;
	port->port.membase = priv->virt + offset;
	port->port.regshift = board->reg_shift;

	return 0;
}

static int
pci_fastcom335_setup(struct exar8250 *priv, struct pci_dev *pcidev,
		     struct uart_8250_port *port, int idx)
{
	unsigned int offset = idx * 0x200;
	unsigned int baud = 1843200;
	u8 __iomem *p;
	int err;

	port->port.uartclk = baud * 16;

	err = default_setup(priv, pcidev, idx, offset, port);
	if (err)
		return err;

	p = port->port.membase;

	writeb(0x00, p + UART_EXAR_8XMODE);
	writeb(UART_FCTR_EXAR_TRGD, p + UART_EXAR_FCTR);
	writeb(32, p + UART_EXAR_TXTRG);
	writeb(32, p + UART_EXAR_RXTRG);

	/*
	 * Setup Multipurpose Input/Output pins.
	 */
	if (idx == 0) {
		switch (pcidev->device) {
		case PCI_DEVICE_ID_COMMTECH_4222PCI335:
		case PCI_DEVICE_ID_COMMTECH_4224PCI335:
			writeb(0x78, p + UART_EXAR_MPIOLVL_7_0);
			writeb(0x00, p + UART_EXAR_MPIOINV_7_0);
			writeb(0x00, p + UART_EXAR_MPIOSEL_7_0);
			break;
		case PCI_DEVICE_ID_COMMTECH_2324PCI335:
		case PCI_DEVICE_ID_COMMTECH_2328PCI335:
			writeb(0x00, p + UART_EXAR_MPIOLVL_7_0);
			writeb(0xc0, p + UART_EXAR_MPIOINV_7_0);
			writeb(0xc0, p + UART_EXAR_MPIOSEL_7_0);
			break;
		}
		writeb(0x00, p + UART_EXAR_MPIOINT_7_0);
		writeb(0x00, p + UART_EXAR_MPIO3T_7_0);
		writeb(0x00, p + UART_EXAR_MPIOOD_7_0);
	}

	return 0;
}

static int
pci_connect_tech_setup(struct exar8250 *priv, struct pci_dev *pcidev,
		       struct uart_8250_port *port, int idx)
{
	unsigned int offset = idx * 0x200;
	unsigned int baud = 1843200;

	port->port.uartclk = baud * 16;
	return default_setup(priv, pcidev, idx, offset, port);
}

static int
pci_xr17c154_setup(struct exar8250 *priv, struct pci_dev *pcidev,
		   struct uart_8250_port *port, int idx)
{
	unsigned int offset = idx * 0x200;
	unsigned int baud = 921600;

	port->port.uartclk = baud * 16;
	return default_setup(priv, pcidev, idx, offset, port);
}

static void setup_gpio(struct pci_dev *pcidev, u8 __iomem *p)
{
	/*
	 * The Commtech adapters required the MPIOs to be driven low. The Exar
	 * devices will export them as GPIOs, so we pre-configure them safely
	 * as inputs.
	 */
	u8 dir = pcidev->vendor == PCI_VENDOR_ID_EXAR ? 0xff : 0x00;

	writeb(0x00, p + UART_EXAR_MPIOINT_7_0);
	writeb(0x00, p + UART_EXAR_MPIOLVL_7_0);
	writeb(0x00, p + UART_EXAR_MPIO3T_7_0);
	writeb(0x00, p + UART_EXAR_MPIOINV_7_0);
	writeb(dir,  p + UART_EXAR_MPIOSEL_7_0);
	writeb(0x00, p + UART_EXAR_MPIOOD_7_0);
	writeb(0x00, p + UART_EXAR_MPIOINT_15_8);
	writeb(0x00, p + UART_EXAR_MPIOLVL_15_8);
	writeb(0x00, p + UART_EXAR_MPIO3T_15_8);
	writeb(0x00, p + UART_EXAR_MPIOINV_15_8);
	writeb(dir,  p + UART_EXAR_MPIOSEL_15_8);
	writeb(0x00, p + UART_EXAR_MPIOOD_15_8);
}

static void *
__xr17v35x_register_gpio(struct pci_dev *pcidev,
			 const struct property_entry *properties)
{
	struct platform_device *pdev;

	pdev = platform_device_alloc("gpio_exar", PLATFORM_DEVID_AUTO);
	if (!pdev)
		return NULL;

	pdev->dev.parent = &pcidev->dev;
	ACPI_COMPANION_SET(&pdev->dev, ACPI_COMPANION(&pcidev->dev));

	if (platform_device_add_properties(pdev, properties) < 0 ||
	    platform_device_add(pdev) < 0) {
		platform_device_put(pdev);
		return NULL;
	}

	return pdev;
}

static const struct property_entry exar_gpio_properties[] = {
	PROPERTY_ENTRY_U32("exar,first-pin", 0),
	PROPERTY_ENTRY_U32("ngpios", 16),
	{ }
};

static int xr17v35x_register_gpio(struct pci_dev *pcidev,
				  struct uart_8250_port *port)
{
	if (pcidev->vendor == PCI_VENDOR_ID_EXAR)
		port->port.private_data =
			__xr17v35x_register_gpio(pcidev, exar_gpio_properties);

	return 0;
}

static int generic_rs485_config(struct uart_port *port,
				struct serial_rs485 *rs485)
{
	bool is_rs485 = !!(rs485->flags & SER_RS485_ENABLED);
	u8 __iomem *p = port->membase;
	u8 value;

	value = readb(p + UART_EXAR_FCTR);
	if (is_rs485)
		value |= UART_FCTR_EXAR_485;
	else
		value &= ~UART_FCTR_EXAR_485;

	writeb(value, p + UART_EXAR_FCTR);

	if (is_rs485)
		writeb(UART_EXAR_RS485_DLY(4), p + UART_MSR);

	port->rs485 = *rs485;

	return 0;
}

static const struct exar8250_platform exar8250_default_platform = {
	.register_gpio = xr17v35x_register_gpio,
	.rs485_config = generic_rs485_config,
};

static int iot2040_rs485_config(struct uart_port *port,
				struct serial_rs485 *rs485)
{
	bool is_rs485 = !!(rs485->flags & SER_RS485_ENABLED);
	u8 __iomem *p = port->membase;
	u8 mask = IOT2040_UART1_MASK;
	u8 mode, value;

	if (is_rs485) {
		if (rs485->flags & SER_RS485_RX_DURING_TX)
			mode = IOT2040_UART_MODE_RS422;
		else
			mode = IOT2040_UART_MODE_RS485;

		if (rs485->flags & SER_RS485_TERMINATE_BUS)
			mode |= IOT2040_UART_TERMINATE_BUS;
	} else {
		mode = IOT2040_UART_MODE_RS232;
	}

	if (port->line == 3) {
		mask <<= IOT2040_UART2_SHIFT;
		mode <<= IOT2040_UART2_SHIFT;
	}

	value = readb(p + UART_EXAR_MPIOLVL_7_0);
	value &= ~mask;
	value |= mode;
	writeb(value, p + UART_EXAR_MPIOLVL_7_0);

	return generic_rs485_config(port, rs485);
}

static const struct property_entry iot2040_gpio_properties[] = {
	PROPERTY_ENTRY_U32("exar,first-pin", 10),
	PROPERTY_ENTRY_U32("ngpios", 1),
	{ }
};

static int iot2040_register_gpio(struct pci_dev *pcidev,
			      struct uart_8250_port *port)
{
	u8 __iomem *p = port->port.membase;

	writeb(IOT2040_UARTS_DEFAULT_MODE, p + UART_EXAR_MPIOLVL_7_0);
	writeb(IOT2040_UARTS_GPIO_LO_MODE, p + UART_EXAR_MPIOSEL_7_0);
	writeb(IOT2040_UARTS_ENABLE, p + UART_EXAR_MPIOLVL_15_8);
	writeb(IOT2040_UARTS_GPIO_HI_MODE, p + UART_EXAR_MPIOSEL_15_8);

	port->port.private_data =
		__xr17v35x_register_gpio(pcidev, iot2040_gpio_properties);

	return 0;
}

static const struct exar8250_platform iot2040_platform = {
	.rs485_config = iot2040_rs485_config,
	.register_gpio = iot2040_register_gpio,
};

static const struct dmi_system_id exar_platforms[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "SIMATIC IOT2000"),
			DMI_EXACT_MATCH(DMI_BOARD_ASSET_TAG,
					"6ES7647-0AA00-1YA2"),
		},
		.driver_data = (void *)&iot2040_platform,
	},
	{}
};

static int
pci_xr17v35x_setup(struct exar8250 *priv, struct pci_dev *pcidev,
		   struct uart_8250_port *port, int idx)
{
	const struct exar8250_platform *platform;
	const struct dmi_system_id *dmi_match;
	unsigned int offset = idx * 0x400;
	unsigned int baud = 7812500;
	u8 __iomem *p;
	int ret;

	dmi_match = dmi_first_match(exar_platforms);
	if (dmi_match)
		platform = dmi_match->driver_data;
	else
		platform = &exar8250_default_platform;

	port->port.uartclk = baud * 16;
	port->port.rs485_config = platform->rs485_config;

	/*
	 * Setup the UART clock for the devices on expansion slot to
	 * half the clock speed of the main chip (which is 125MHz)
	 */
	if (idx >= 8)
		port->port.uartclk /= 2;

	ret = default_setup(priv, pcidev, idx, offset, port);
	if (ret)
		return ret;

	p = port->port.membase;

	writeb(0x00, p + UART_EXAR_8XMODE);
	writeb(UART_FCTR_EXAR_TRGD, p + UART_EXAR_FCTR);
	writeb(128, p + UART_EXAR_TXTRG);
	writeb(128, p + UART_EXAR_RXTRG);

	if (idx == 0) {
		/* Setup Multipurpose Input/Output pins. */
		setup_gpio(pcidev, p);

		ret = platform->register_gpio(pcidev, port);
	}

	return ret;
}

static void pci_xr17v35x_exit(struct pci_dev *pcidev)
{
	struct exar8250 *priv = pci_get_drvdata(pcidev);
	struct uart_8250_port *port = serial8250_get_port(priv->line[0]);
	struct platform_device *pdev = port->port.private_data;

	platform_device_unregister(pdev);
	port->port.private_data = NULL;
}

/*
 * These Exar UARTs have an extra interrupt indicator that could fire for a
 * few interrupts that are not presented/cleared through IIR.  One of which is
 * a wakeup interrupt when coming out of sleep.  These interrupts are only
 * cleared by reading global INT0 or INT1 registers as interrupts are
 * associated with channel 0. The INT[3:0] registers _are_ accessible from each
 * channel's address space, but for the sake of bus efficiency we register a
 * dedicated handler at the PCI device level to handle them.
 */
static irqreturn_t exar_misc_handler(int irq, void *data)
{
	struct exar8250 *priv = data;

	/* Clear all PCI interrupts by reading INT0. No effect on IIR */
	readb(priv->virt + UART_EXAR_INT0);

	/* Clear INT0 for Expansion Interface slave ports, too */
	if (priv->board->num_ports > 8)
		readb(priv->virt + 0x2000 + UART_EXAR_INT0);

	return IRQ_HANDLED;
}

static int
exar_pci_probe(struct pci_dev *pcidev, const struct pci_device_id *ent)
{
	unsigned int nr_ports, i, bar = 0, maxnr;
	struct exar8250_board *board;
	struct uart_8250_port uart;
	struct exar8250 *priv;
	int rc;

	board = (struct exar8250_board *)ent->driver_data;
	if (!board)
		return -EINVAL;

	rc = pcim_enable_device(pcidev);
	if (rc)
		return rc;

	maxnr = pci_resource_len(pcidev, bar) >> (board->reg_shift + 3);

	nr_ports = board->num_ports ? board->num_ports : pcidev->device & 0x0f;

	priv = devm_kzalloc(&pcidev->dev, sizeof(*priv) +
			    sizeof(unsigned int) * nr_ports,
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->board = board;
	priv->virt = pcim_iomap(pcidev, bar, 0);
	if (!priv->virt)
		return -ENOMEM;

	pci_set_master(pcidev);

	rc = pci_alloc_irq_vectors(pcidev, 1, 1, PCI_IRQ_ALL_TYPES);
	if (rc < 0)
		return rc;

	memset(&uart, 0, sizeof(uart));
	uart.port.flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ
			  | UPF_EXAR_EFR;
	uart.port.irq = pci_irq_vector(pcidev, 0);
	uart.port.dev = &pcidev->dev;

	rc = devm_request_irq(&pcidev->dev, uart.port.irq, exar_misc_handler,
			 IRQF_SHARED, "exar_uart", priv);
	if (rc)
		return rc;

	for (i = 0; i < nr_ports && i < maxnr; i++) {
		rc = board->setup(priv, pcidev, &uart, i);
		if (rc) {
			dev_err(&pcidev->dev, "Failed to setup port %u\n", i);
			break;
		}

		dev_dbg(&pcidev->dev, "Setup PCI port: port %lx, irq %d, type %d\n",
			uart.port.iobase, uart.port.irq, uart.port.iotype);

		priv->line[i] = serial8250_register_8250_port(&uart);
		if (priv->line[i] < 0) {
			dev_err(&pcidev->dev,
				"Couldn't register serial port %lx, irq %d, type %d, error %d\n",
				uart.port.iobase, uart.port.irq,
				uart.port.iotype, priv->line[i]);
			break;
		}
	}
	priv->nr = i;
	pci_set_drvdata(pcidev, priv);
	return 0;
}

static void exar_pci_remove(struct pci_dev *pcidev)
{
	struct exar8250 *priv = pci_get_drvdata(pcidev);
	unsigned int i;

	for (i = 0; i < priv->nr; i++)
		serial8250_unregister_port(priv->line[i]);

	/* Ensure that every init quirk is properly torn down */
	if (priv->board->exit)
		priv->board->exit(pcidev);
}

static int __maybe_unused exar_suspend(struct device *dev)
{
	struct pci_dev *pcidev = to_pci_dev(dev);
	struct exar8250 *priv = pci_get_drvdata(pcidev);
	unsigned int i;

	for (i = 0; i < priv->nr; i++)
		if (priv->line[i] >= 0)
			serial8250_suspend_port(priv->line[i]);

	return 0;
}

static int __maybe_unused exar_resume(struct device *dev)
{
	struct pci_dev *pcidev = to_pci_dev(dev);
	struct exar8250 *priv = pci_get_drvdata(pcidev);
	unsigned int i;

	for (i = 0; i < priv->nr; i++)
		if (priv->line[i] >= 0)
			serial8250_resume_port(priv->line[i]);

	return 0;
}

static SIMPLE_DEV_PM_OPS(exar_pci_pm, exar_suspend, exar_resume);

static const struct exar8250_board pbn_fastcom335_2 = {
	.num_ports	= 2,
	.setup		= pci_fastcom335_setup,
};

static const struct exar8250_board pbn_fastcom335_4 = {
	.num_ports	= 4,
	.setup		= pci_fastcom335_setup,
};

static const struct exar8250_board pbn_fastcom335_8 = {
	.num_ports	= 8,
	.setup		= pci_fastcom335_setup,
};

static const struct exar8250_board pbn_connect = {
	.setup		= pci_connect_tech_setup,
};

static const struct exar8250_board pbn_exar_ibm_saturn = {
	.num_ports	= 1,
	.setup		= pci_xr17c154_setup,
};

static const struct exar8250_board pbn_exar_XR17C15x = {
	.setup		= pci_xr17c154_setup,
};

static const struct exar8250_board pbn_exar_XR17V35x = {
	.setup		= pci_xr17v35x_setup,
	.exit		= pci_xr17v35x_exit,
};

static const struct exar8250_board pbn_fastcom35x_2 = {
	.num_ports	= 2,
	.setup		= pci_xr17v35x_setup,
	.exit		= pci_xr17v35x_exit,
};

static const struct exar8250_board pbn_fastcom35x_4 = {
	.num_ports	= 4,
	.setup		= pci_xr17v35x_setup,
	.exit		= pci_xr17v35x_exit,
};

static const struct exar8250_board pbn_fastcom35x_8 = {
	.num_ports	= 8,
	.setup		= pci_xr17v35x_setup,
	.exit		= pci_xr17v35x_exit,
};

static const struct exar8250_board pbn_exar_XR17V4358 = {
	.num_ports	= 12,
	.setup		= pci_xr17v35x_setup,
	.exit		= pci_xr17v35x_exit,
};

static const struct exar8250_board pbn_exar_XR17V8358 = {
	.num_ports	= 16,
	.setup		= pci_xr17v35x_setup,
	.exit		= pci_xr17v35x_exit,
};

#define CONNECT_DEVICE(devid, sdevid, bd) {				\
	PCI_DEVICE_SUB(							\
		PCI_VENDOR_ID_EXAR,					\
		PCI_DEVICE_ID_EXAR_##devid,				\
		PCI_SUBVENDOR_ID_CONNECT_TECH,				\
		PCI_SUBDEVICE_ID_CONNECT_TECH_PCI_##sdevid), 0, 0,	\
		(kernel_ulong_t)&bd					\
	}

#define EXAR_DEVICE(vend, devid, bd) {					\
	PCI_VDEVICE(vend, PCI_DEVICE_ID_##devid), (kernel_ulong_t)&bd	\
	}

#define IBM_DEVICE(devid, sdevid, bd) {			\
	PCI_DEVICE_SUB(					\
		PCI_VENDOR_ID_EXAR,			\
		PCI_DEVICE_ID_EXAR_##devid,		\
		PCI_VENDOR_ID_IBM,			\
		PCI_SUBDEVICE_ID_IBM_##sdevid), 0, 0,	\
		(kernel_ulong_t)&bd			\
	}

static const struct pci_device_id exar_pci_tbl[] = {
	CONNECT_DEVICE(XR17C152, UART_2_232, pbn_connect),
	CONNECT_DEVICE(XR17C154, UART_4_232, pbn_connect),
	CONNECT_DEVICE(XR17C158, UART_8_232, pbn_connect),
	CONNECT_DEVICE(XR17C152, UART_1_1, pbn_connect),
	CONNECT_DEVICE(XR17C154, UART_2_2, pbn_connect),
	CONNECT_DEVICE(XR17C158, UART_4_4, pbn_connect),
	CONNECT_DEVICE(XR17C152, UART_2, pbn_connect),
	CONNECT_DEVICE(XR17C154, UART_4, pbn_connect),
	CONNECT_DEVICE(XR17C158, UART_8, pbn_connect),
	CONNECT_DEVICE(XR17C152, UART_2_485, pbn_connect),
	CONNECT_DEVICE(XR17C154, UART_4_485, pbn_connect),
	CONNECT_DEVICE(XR17C158, UART_8_485, pbn_connect),

	IBM_DEVICE(XR17C152, SATURN_SERIAL_ONE_PORT, pbn_exar_ibm_saturn),

	/* Exar Corp. XR17C15[248] Dual/Quad/Octal UART */
	EXAR_DEVICE(EXAR, EXAR_XR17C152, pbn_exar_XR17C15x),
	EXAR_DEVICE(EXAR, EXAR_XR17C154, pbn_exar_XR17C15x),
	EXAR_DEVICE(EXAR, EXAR_XR17C158, pbn_exar_XR17C15x),

	/* Exar Corp. XR17V[48]35[248] Dual/Quad/Octal/Hexa PCIe UARTs */
	EXAR_DEVICE(EXAR, EXAR_XR17V352, pbn_exar_XR17V35x),
	EXAR_DEVICE(EXAR, EXAR_XR17V354, pbn_exar_XR17V35x),
	EXAR_DEVICE(EXAR, EXAR_XR17V358, pbn_exar_XR17V35x),
	EXAR_DEVICE(EXAR, EXAR_XR17V4358, pbn_exar_XR17V4358),
	EXAR_DEVICE(EXAR, EXAR_XR17V8358, pbn_exar_XR17V8358),
	EXAR_DEVICE(COMMTECH, COMMTECH_4222PCIE, pbn_fastcom35x_2),
	EXAR_DEVICE(COMMTECH, COMMTECH_4224PCIE, pbn_fastcom35x_4),
	EXAR_DEVICE(COMMTECH, COMMTECH_4228PCIE, pbn_fastcom35x_8),

	EXAR_DEVICE(COMMTECH, COMMTECH_4222PCI335, pbn_fastcom335_2),
	EXAR_DEVICE(COMMTECH, COMMTECH_4224PCI335, pbn_fastcom335_4),
	EXAR_DEVICE(COMMTECH, COMMTECH_2324PCI335, pbn_fastcom335_4),
	EXAR_DEVICE(COMMTECH, COMMTECH_2328PCI335, pbn_fastcom335_8),
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, exar_pci_tbl);

static struct pci_driver exar_pci_driver = {
	.name		= "exar_serial",
	.probe		= exar_pci_probe,
	.remove		= exar_pci_remove,
	.driver         = {
		.pm     = &exar_pci_pm,
	},
	.id_table	= exar_pci_tbl,
};
module_pci_driver(exar_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Exar Serial Driver");
MODULE_AUTHOR("Sudip Mukherjee <sudip.mukherjee@codethink.co.uk>");

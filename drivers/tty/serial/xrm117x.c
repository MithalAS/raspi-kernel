/*
 * XR20M117x tty serial driver - Copyright (C) 2024 GridPoint
 * Author: Ted Lin <tedlin@maxlinear.com>
 *
 *  Based on SC16IS7xx.c, by Jon Ringle <jringle@gridpoint.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "linux/version.h"
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/uaccess.h>
#include <linux/spi/spi.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <uapi/linux/sched/types.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
#include <linux/gpio.h>
#else
#include <linux/gpio/driver.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#else
#include <linux/of.h>
#include <linux/of_device.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
/* This is included under serial_core.h after kernel 3.16. 
 * this define is only for kernel version under 3.16
 */
#define PORT_SC16IS7XX 108
/**
 * enum uart_pm_state - power states for UARTs
 * @UART_PM_STATE_ON: UART is powered, up and operational
 * @UART_PM_STATE_OFF: UART is powered off
 * @UART_PM_STATE_UNDEFINED: sentinel
 */
enum uart_pm_state {
	UART_PM_STATE_ON = 0,
	UART_PM_STATE_OFF = 3, /* number taken from ACPI */
	UART_PM_STATE_UNDEFINED,
};
#endif

#define XRM_READ_REG (FIOQSIZE + 1)
#define XRM_WRITE_REG (FIOQSIZE + 2)

struct xrioctl_rw_reg {
	unsigned char reg;
	unsigned char regvalue;
};

#define XRM117X_NAME "xr20m117x"
#define XRM117X_MAX_DEVS 8

/* XR20M117x register definitions */
#define XRM117X_RHR_REG (0x00) /* RX FIFO */
#define XRM117X_THR_REG (0x00) /* TX FIFO */
#define XRM117X_IER_REG (0x01) /* Interrupt enable */
#define XRM117X_IIR_REG (0x02) /* Interrupt Identification */
#define XRM117X_FCR_REG (0x02) /* FIFO control */
#define XRM117X_LCR_REG (0x03) /* Line Control */
#define XRM117X_MCR_REG (0x04) /* Modem Control */
#define XRM117X_LSR_REG (0x05) /* Line Status */
#define XRM117X_MSR_REG (0x06) /* Modem Status */
#define XRM117X_SPR_REG (0x07) /* Scratch Pad */
#define XRM117X_TXLVL_REG (0x08) /* TX FIFO level */
#define XRM117X_RXLVL_REG (0x09) /* RX FIFO level */
#define XRM117X_IODIR_REG (0x0a) /* I/O Direction	*/
#define XRM117X_IOSTATE_REG (0x0b) /* I/O State	*/
#define XRM117X_IOINTENA_REG (0x0c) /* I/O Interrupt Enable */
#define XRM117X_IOCONTROL_REG (0x0e) /* I/O Control */
#define XRM117X_EFCR_REG (0x0f) /* Extra Features Control */

/* TCR/TLR Register set: Only if ((MCR[2] == 1) && (EFR[4] == 1)) */
#define XRM117X_TCR_REG (0x06) /* Transmit control */
#define XRM117X_TLR_REG (0x07) /* Trigger level */

/* Special Register set: Only if ((LCR[7] == 1) && (LCR != 0xBF)) */
#define XRM117X_DLL_REG (0x00) /* Divisor LSB */
#define XRM117X_DLM_REG (0x01) /* Divisor MSB */

/* Special Register set: Only if ((LCR[7] == 1) && (LCR != 0xBF)) && (EFR[4] == 1) */
#define XRM117X_DLD_REG (0x02) /* Divisor Fractional */

/* Enhanced Register set: Only if (LCR == 0xBF) */
#define XRM117X_EFR_REG (0x02) /* Enhanced Features */
#define XRM117X_XON1_REG (0x04) /* Xon1 word */
#define XRM117X_XON2_REG (0x05) /* Xon2 word */
#define XRM117X_XOFF1_REG (0x06) /* Xoff1 word */
#define XRM117X_XOFF2_REG (0x07) /* Xoff2 word */

/* IER register bits */
#define XRM117X_IER_RDI_BIT (1 << 0) /* Enable RX data interrupt */
#define XRM117X_IER_THRI_BIT (1 << 1) /* Enable TX holding register interrupt */
#define XRM117X_IER_RLSI_BIT (1 << 2) /* Enable RX line status interrupt */
#define XRM117X_IER_MSI_BIT (1 << 3) /* Enable Modem status interrupt */

/* IER register bits - write only if (EFR[4] == 1) */
#define XRM117X_IER_SLEEP_BIT (1 << 4) /* Enable Sleep mode */
#define XRM117X_IER_XOFFI_BIT (1 << 5) /* Enable Xoff interrupt */
#define XRM117X_IER_RTSI_BIT (1 << 6) /* Enable nRTS interrupt */
#define XRM117X_IER_CTSI_BIT (1 << 7) /* Enable nCTS interrupt */

/* FCR register bits */
#define XRM117X_FCR_FIFO_BIT (1 << 0) /* Enable FIFO */
#define XRM117X_FCR_RXRESET_BIT (1 << 1) /* Reset RX FIFO */
#define XRM117X_FCR_TXRESET_BIT (1 << 2) /* Reset TX FIFO */
#define XRM117X_FCR_RXLVLL_BIT (1 << 6) /* RX Trigger level LSB */
#define XRM117X_FCR_RXLVLH_BIT (1 << 7) /* RX Trigger level MSB */

/* FCR register bits - write only if (EFR[4] == 1) */
#define XRM117X_FCR_TXLVLL_BIT (1 << 4) /* TX Trigger level LSB */
#define XRM117X_FCR_TXLVLH_BIT (1 << 5) /* TX Trigger level MSB */

/* IIR register bits */
#define XRM117X_IIR_NO_INT_BIT (1 << 0) /* No interrupts pending */
#define XRM117X_IIR_ID_MASK 0x3e /* Mask for the interrupt ID */
/* Interrupt Sources, in the sequence of priority level */
#define XRM117X_IIR_RLSE_SRC 0x06 /* LSR (RX line status error) interrupt */
#define XRM117X_IIR_RTOI_SRC 0x0c /* RXRDY Data Time-out interrupt */
#define XRM117X_IIR_RDI_SRC 0x04 /* RXRDY Data Ready interrupt */
#define XRM117X_IIR_THRI_SRC 0x02 /* TXRDY interrupt */
#define XRM117X_IIR_MSI_SRC 0x00 /* MSR (Modem status) interrupt */
#define XRM117X_IIR_INPIN_SRC 0x30 /* GPIO (General Purpose Inputs) interrupt */
#define XRM117X_IIR_XOFFI_SRC                                                  \
	0x10 /* RXRDY (Received Xoff or Special character) interrupt */
#define XRM117X_IIR_CTSRTS_SRC                                                 \
	0x20 /* nCTS,nRTS change of state (from active to inactive, LOW to HIGH) during auto flow control */

/* LCR register bits */
#define XRM117X_LCR_LENGTH0_BIT (1 << 0) /* Word length bit 0 */
#define XRM117X_LCR_LENGTH1_BIT                                                \
	(1 << 1) /* Word length bit 1
						  *
						  * Word length bits table:
						  * 00 -> 5 bit words
						  * 01 -> 6 bit words (default)
						  * 10 -> 7 bit words
						  * 11 -> 8 bit words
						  */
#define XRM117X_LCR_STOPLEN_BIT                                                \
	(1 << 2) /* STOP length bit
						  *
						  * STOP length bit table:
						  * 0 -> 1 stop bit
						  * 1 -> 1-1.5 stop bits if
						  *      word length is 5,
						  *      2 stop bits otherwise (default)
						  */
#define XRM117X_LCR_PARITY_BIT (1 << 3) /* Parity bit enable */
#define XRM117X_LCR_EVENPARITY_BIT (1 << 4) /* Even parity bit enable */
#define XRM117X_LCR_FORCEPARITY_BIT                                            \
	(1 << 5) /* Force parity format
						  *
						  * Parity bits table (LCR[5:3]):
						  * XX0 -> No parity
						  * 001 -> Odd parity
						  * 011 -> Even parity
							* 101 -> Forced parity to mark "1"
						  * 111 -> Forced parity to space "0"
							*/
#define XRM117X_LCR_TXBREAK_BIT (1 << 6) /* TX break enable */
#define XRM117X_LCR_DLAB_BIT (1 << 7) /* Divisor Latch enable */
#define XRM117X_LCR_WORD_LEN_5 (0x00)
#define XRM117X_LCR_WORD_LEN_6 (0x01)
#define XRM117X_LCR_WORD_LEN_7 (0x02)
#define XRM117X_LCR_WORD_LEN_8 (0x03)
#define XRM117X_LCR_CONF_MODE_A XRM117X_LCR_DLAB_BIT /* Special reg set */
#define XRM117X_LCR_CONF_MODE_B 0xBF /* Enhanced reg set */

/* MCR register bits */
#define XRM117X_MCR_DTR_BIT (1 << 0) /* DTR output control */
#define XRM117X_MCR_RTS_BIT (1 << 1) /* RTS output control */
#define XRM117X_MCR_LOOP_BIT (1 << 4) /* Enable loopback test mode */

/* MCR register bits - enabled only if (EFR[4] == 1) */
#define XRM117X_MCR_TCRTLR_BIT (1 << 2) /* TCR/TLR register enable */
#define XRM117X_MCR_XONANY_BIT (1 << 5) /* Enable Xon Any */
#define XRM117X_MCR_IRDA_BIT (1 << 6) /* Enable IrDA mode */
#define XRM117X_MCR_CLKSEL_BIT                                                 \
	(1 << 7) /* Clock Prescaler Select (Divide clock by 4 if set) */

/* LSR register bits */
#define XRM117X_LSR_DR_BIT (1 << 0) /* RX data ready */
#define XRM117X_LSR_OE_BIT (1 << 1) /* RX Overrun Error */
#define XRM117X_LSR_PE_BIT (1 << 2) /* RX Parity Error */
#define XRM117X_LSR_FE_BIT (1 << 3) /* RX Framing Error */
#define XRM117X_LSR_BI_BIT (1 << 4) /* RX Break Interrupt */
#define XRM117X_LSR_BRK_ERROR_MASK 0x1E /* BI, FE, PE, OE bits */
#define XRM117X_LSR_THRE_BIT (1 << 5) /* TX holding register empty */
#define XRM117X_LSR_TEMT_BIT (1 << 6) /* Transmitter (THR & TSR) empty */
#define XRM117X_LSR_FIFOE_BIT                                                  \
	(1                                                                     \
	 << 7) /* RX Fifo Global Error (either PE, FE, BI within the FIFO data) */

/* MSR register bits */
#define XRM117X_MSR_DCTS_BIT (1 << 0) /* Delta CTS Clear To Send */
#define XRM117X_MSR_DDSR_BIT (1 << 1) /* Delta DSR Data Set Ready */
#define XRM117X_MSR_DRI_BIT (1 << 2) /* Delta RI Ring Indicator */
#define XRM117X_MSR_DCD_BIT (1 << 3) /* Delta CD Carrier Detect */
#define XRM117X_MSR_CTS_BIT (1 << 4) /* CTS */
#define XRM117X_MSR_DSR_BIT (1 << 5) /* DSR */
#define XRM117X_MSR_RI_BIT (1 << 6) /* RI */
#define XRM117X_MSR_CD_BIT (1 << 7) /* CD */
#define XRM117X_MSR_DELTA_MASK 0x0F /* Any of the delta bits! */

/*
 * TCR register bits
 * TCR trigger levels are available from 0 to 60 characters with a granularity
 * of four.
 * The programmer must program the TCR such that TCR[3:0] > TCR[7:4]. There is
 * no built-in hardware check to make sure this condition is met. Also, the TCR
 * must be programmed with this condition before auto RTS or software flow
 * control is enabled to avoid spurious operation of the device.
 */
#define XRM117X_TCR_RX_HALT(words) ((((words) / 4) & 0x0f) << 0)
#define XRM117X_TCR_RX_RESUME(words) ((((words) / 4) & 0x0f) << 4)

/*
 * TLR register bits
 * If TLR[3:0] or TLR[7:4] are logical 0, the selectable trigger levels via the
 * FIFO Control Register (FCR) are used for the transmit and receive FIFO
 * trigger levels. Trigger levels from 4 characters to 60 characters are
 * available with a granularity of four.
 *
 * When the trigger level setting in TLR is zero, the trigger level setting
 * used will refer to FCR[5:4]/FCR[7:6]. If TLR has non-zero trigger level value
 * the trigger level defined in FCR is discarded. This applies to both transmit
 * FIFO and receive FIFO trigger level setting.
 *
 * When TLR is used for RX trigger level control, FCR[7:6] should be left at the
 * default state, that is, '00'.
 */
#define XRM117X_TLR_TX_TRIGGER(words) ((((words) / 4) & 0x0f) << 0)
#define XRM117X_TLR_RX_TRIGGER(words) ((((words) / 4) & 0x0f) << 4)

/* IOControl register bits */
#define XRM117X_IOCONTROL_LATCH_BIT (1 << 0) /* Enable input latching */
#define XRM117X_IOCONTROL_MODEM_A_BIT                                          \
	(1 << 1) /* Enable ChaA GPIO[7:4] as modem pins */
#define XRM117X_IOCONTROL_MODEM_B_BIT                                          \
	(1 << 2) /* Enable ChaB GPIO[7:4] as modem pins */
#define XRM117X_IOCONTROL_SRESET_BIT (1 << 3) /* Software Reset */

/* EFCR register bits */
#define XRM117X_EFCR_9BIT_MODE_BIT                                             \
	(1 << 0) /* Enable 9-bit or Multidrop mode (RS485) */
#define XRM117X_EFCR_RXDISABLE_BIT (1 << 1) /* Disable receiver */
#define XRM117X_EFCR_TXDISABLE_BIT (1 << 2) /* Disable transmitter */
#define XRM117X_EFCR_AUTO_RS485_BIT (1 << 4) /* Auto RS485 RTS direction */
#define XRM117X_EFCR_RTS_INVERT_BIT (1 << 5) /* RTS output inversion */
#define XRM117X_EFCR_IRDA_MODE_BIT                                             \
	(1 << 7) /* IrDA mode
						  * 0 = V1.0, data rate up to 115.2 kbit/s
						  * 1 = V1.1, data rate upto 1.152 Mbit/s
						  */

/* DLD register bits - access only if (LCR[7]==1 && LCR!=0xBF && EFR[4]==1)*/
#define XRM117X_DLD_SAMPLING_8X_BIT                                            \
	(1 << 4) /* Sampling mode 8x; Ignored when 4x mode is set */
#define XRM117X_DLD_SAMPLING_4X_BIT (1 << 5) /* Sampling mode 4x */
#define XRM117X_DLD_FRACTION_BITS_MASK 0x0F /* Fraction config is in DLD[3:0] */
#define XRM117X_DLD_SAMPLING_MODE_MASK                                         \
	0x30 /* Sampling mode setting is in DLD[5:4] */

/* EFR register bits - access only if (LCR == 0xBF) */
#define XRM117X_EFR_AUTORTS_BIT (1 << 6) /* Auto RTS flow ctrl enable */
#define XRM117X_EFR_AUTOCTS_BIT (1 << 7) /* Auto CTS flow ctrl enable */
#define XRM117X_EFR_XOFF2_DETECT_BIT (1 << 5) /* Enable Xoff2 detection */
#define XRM117X_EFR_ENABLE_BIT                                                 \
	(1 << 4) /* Enable enhanced functions
						  * and access to IER[7:4], ISR[5:4], 
						  * FCR[5:4], MCR[7:5], & DLD
						  */
#define XRM117X_EFR_SWFLOW3_BIT (1 << 3) /* SWFLOW bit 3 */
#define XRM117X_EFR_SWFLOW2_BIT                                                \
	(1 << 2) /* SWFLOW bit 2
						  *
						  * SWFLOW bits 3 & 2 table:
						  * 00 -> no transmitter flow
						  *       control
						  * 01 -> transmitter generates
						  *       XON2 and XOFF2
						  * 10 -> transmitter generates
						  *       XON1 and XOFF1
						  * 11 -> transmitter generates
						  *       XON1, XON2, XOFF1 and
						  *       XOFF2
						  */
#define XRM117X_EFR_SWFLOW1_BIT (1 << 1) /* SWFLOW bit 1 */
#define XRM117X_EFR_SWFLOW0_BIT                                                \
	(1 << 0) /* SWFLOW bit 0
						  *
						  * SWFLOW bits 1 & 0 table:
						  * 00 -> no received flow
						  *       control
						  * 01 -> receiver compares
						  *       XON2 and XOFF2
						  * 10 -> receiver compares
						  *       XON1 and XOFF1
						  * 11 -> receiver compares
						  *       XON1, XON2, XOFF1 and
						  *       XOFF2
						  */

/* Misc definitions */
#define XRM117X_FIFO_SIZE (64)
#define XRM117X_REG_SHIFT 2

struct xr20m117x_devtype {
	char name[10];
	int nr_gpio;
	int nr_uart;
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
#define XRM117X_RECONF_MD (1 << 0)
#define XRM117X_RECONF_IER (1 << 1)
#define XRM117X_RECONF_RS485 (1 << 2)

struct xr20m117x_one_config {
	unsigned int flags;
	u8 ier_clear;
};
#endif

struct xr20m117x_one {
	struct uart_port port;
	struct serial_rs485 rs485;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
	struct work_struct tx_work;
	struct work_struct md_work;

#else // Kernel < 4.2
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
	u8 line;
#endif
	struct kthread_work tx_work;
	struct kthread_work reg_work;
	struct xr20m117x_one_config config;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	bool irda_mode;
#endif
#endif
};

struct xr20m117x_port {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	struct uart_driver uart;
	struct xr20m117x_devtype *devtype;
#else
	const struct xr20m117x_devtype *devtype;
#endif
	struct regmap *regmap;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
	struct mutex mutex;
#endif
	struct clk *clk;
#ifdef CONFIG_GPIOLIB
	struct gpio_chip gpio;
#endif
	unsigned char buf[XRM117X_FIFO_SIZE];
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	struct kthread_worker kworker;
	struct task_struct *kworker_task;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
	struct kthread_work irq_work;
#endif //Kernel < version 5.8
#endif //Kernel >= 4.2
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 82)
	struct mutex efr_lock;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	struct xr20m117x_one p[];
#else
	struct xr20m117x_one p[0];
#endif
};

#define to_xr20m117x_one(p, e) ((container_of((p), struct xr20m117x_one, e)))

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
static unsigned long xr20m117x_lines;

static struct uart_driver xr20m117x_uart = {
	.owner = THIS_MODULE,
	.dev_name = "ttyXRM",
	.nr = XRM117X_MAX_DEVS,
};

#define to_xr20m117x_port(p, e) ((container_of((p), struct xr20m117x_port, e)))

static int xr20m117x_line(struct uart_port *port)
{
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);

	return one->line;
}
#endif

static u8 xr20m117x_port_read(struct uart_port *port, u8 reg)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	unsigned int val = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	regmap_read(s->regmap, (reg << XRM117X_REG_SHIFT) | port->line, &val);
#else
	const u8 line = xr20m117x_line(port);

	regmap_read(s->regmap, (reg << XRM117X_REG_SHIFT) | line, &val);
#endif

	return val;
}

static void xr20m117x_port_write(struct uart_port *port, u8 reg, u8 val)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	regmap_write(s->regmap, (reg << XRM117X_REG_SHIFT) | port->line, val);
#else
	const u8 line = xr20m117x_line(port);

	regmap_write(s->regmap, (reg << XRM117X_REG_SHIFT) | line, val);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
static void xr20m117x_fifo_read(struct uart_port *port, unsigned int rxlen)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	const u8 line = xr20m117x_line(port);
	u8 addr = (XRM117X_RHR_REG << XRM117X_REG_SHIFT) | line;

	regcache_cache_bypass(s->regmap, true);
	regmap_raw_read(s->regmap, addr, s->buf, rxlen);
	regcache_cache_bypass(s->regmap, false);
}

static void xr20m117x_fifo_write(struct uart_port *port, u8 to_send)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	const u8 line = xr20m117x_line(port);
	u8 addr = (XRM117X_THR_REG << XRM117X_REG_SHIFT) | line;

	/*
	 * Don't send zero-length data, at least on SPI it confuses the chip
	 * delivering wrong TXLVL data.
	 */
	if (unlikely(!to_send))
		return;

	regcache_cache_bypass(s->regmap, true);
	regmap_raw_write(s->regmap, addr, s->buf, to_send);
	regcache_cache_bypass(s->regmap, false);
}
#endif

static void xr20m117x_port_update(struct uart_port *port, u8 reg, u8 mask,
				  u8 val)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	regmap_update_bits(s->regmap, (reg << XRM117X_REG_SHIFT) | port->line,
			   mask, val);
#else
	const u8 line = xr20m117x_line(port);

	regmap_update_bits(s->regmap, (reg << XRM117X_REG_SHIFT) | line, mask,
			   val);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
static int xr20m117x_alloc_line(void)
{
	int i;

	BUILD_BUG_ON(XRM117X_MAX_DEVS > BITS_PER_LONG);

	for (i = 0; i < XRM117X_MAX_DEVS; i++)
		if (!test_and_set_bit(i, &xr20m117x_lines))
			break;

	return i;
}
#endif

static void xr20m117x_power(struct uart_port *port, int on)
{
	xr20m117x_port_update(port, XRM117X_IER_REG, XRM117X_IER_SLEEP_BIT,
			      on ? 0 : XRM117X_IER_SLEEP_BIT);
}

static const struct xr20m117x_devtype xr20m1170_devtype = {
	.name = "XR20M1170",
	.nr_gpio = 8,
	.nr_uart = 1,
};

static const struct xr20m117x_devtype xr20m1172_devtype = {
	.name = "XR20M1172",
	.nr_gpio = 8,
	.nr_uart = 2,
};

static bool xr20m117x_regmap_volatile(struct device *dev, unsigned int reg)
{
	switch (reg >> XRM117X_REG_SHIFT) {
	case XRM117X_RHR_REG:
	case XRM117X_DLM_REG:
	case XRM117X_IIR_REG:
	case XRM117X_LSR_REG:
	case XRM117X_MSR_REG:
	case XRM117X_TXLVL_REG:
	case XRM117X_RXLVL_REG:
	case XRM117X_IOSTATE_REG:
		return true;
	default:
		break;
	}

	return false;
}

static bool xr20m117x_regmap_precious(struct device *dev, unsigned int reg)
{
	switch (reg >> XRM117X_REG_SHIFT) {
	case XRM117X_RHR_REG:
		return true;
	default:
		break;
	}

	return false;
}

static int xr20m117x_set_baud(struct uart_port *port, int baud)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	u8 lcr;
	u8 prescaler = 0;
	unsigned long clk = port->uartclk;
	unsigned long div_16, div_integer, div_fraction;

	u8 sampling_mode = 0;
	u8 sampling_factor = 16;

	//printk("xr20m117x_set_baud clk:%d baud=%d\n",clk,baud);
	if ((clk / 16 / baud) > 0xffff) {
		/* If DLL & DLM is not enough, setup prescaler as by 4 */
		prescaler = XRM117X_MCR_CLKSEL_BIT;
		clk /= 4;
	} else if ((clk / 16 / baud) < 1) {
		/* When divisor < 1, change sampling mode */
		sampling_mode = XRM117X_DLD_SAMPLING_8X_BIT;
		sampling_factor /= 2;

		if ((clk / 8 / baud) < 1) {
			sampling_mode = XRM117X_DLD_SAMPLING_4X_BIT;
			sampling_factor /= 2;

			if ((clk / 4 / baud) < 1) {
				/* This baud rate cannot be supported if 4x mode still cannot do */
				printk("xr20m117x_set_baud: baud %d is not supported \n",
				       baud);
				return -EINVAL;
			}
		}

		clk *= (16 / sampling_factor);
	}

	div_16 = DIV_ROUND_CLOSEST(clk, baud);
	div_fraction = div_16 & 0x0f;
	div_integer = (div_16 >> 4);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 82)
	/* In an amazing feat of design, the Enhanced Features Register shares
	 * the address of the Interrupt Identification Register, and is
	 * switched in by writing a magic value (0xbf) to the Line Control
	 * Register. Any interrupt firing during this time will see the EFR
	 * where it expects the IIR to be, leading to "Unexpected interrupt"
	 * messages.
	 *
	 * Prevent this possibility by claiming a mutex while accessing the
	 * EFR, and claiming the same mutex from within the interrupt handler.
	 * This is similar to disabling the interrupt, but that doesn't work
	 * because the bulk of the interrupt processing is run as a workqueue
	 * job in thread context.
	 */
	mutex_lock(&s->efr_lock);
#endif

	lcr = xr20m117x_port_read(port, XRM117X_LCR_REG);

	/* Open the LCR divisors for configuration */
	xr20m117x_port_write(port, XRM117X_LCR_REG, XRM117X_LCR_CONF_MODE_B);

	/* Enable enhanced features */
	regcache_cache_bypass(s->regmap, true);
	xr20m117x_port_write(port, XRM117X_EFR_REG, XRM117X_EFR_ENABLE_BIT);
	regcache_cache_bypass(s->regmap, false);

	/* Put LCR back to the normal mode */
	xr20m117x_port_write(port, XRM117X_LCR_REG, lcr);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 82)
	mutex_unlock(&s->efr_lock);
#endif

	xr20m117x_port_update(port, XRM117X_MCR_REG, XRM117X_MCR_CLKSEL_BIT,
			      prescaler);

	/* Open the LCR divisors for configuration */
	xr20m117x_port_write(port, XRM117X_LCR_REG, XRM117X_LCR_CONF_MODE_A);

	/* Write the new divisor */
	regcache_cache_bypass(s->regmap, true);

	if (sampling_mode != 0) {
		/* Change in sampling mode */
		xr20m117x_port_update(port, XRM117X_DLD_REG,
				      XRM117X_DLD_SAMPLING_MODE_MASK,
				      sampling_mode);
		// printk("xr20m117x_set_baud: Sampling mode changed to %dX mode\n",sampling_factor);
	}

	xr20m117x_port_write(port, XRM117X_DLM_REG, div_integer / 256);
	xr20m117x_port_write(port, XRM117X_DLL_REG, div_integer % 256);
	xr20m117x_port_update(port, XRM117X_DLD_REG,
			      XRM117X_DLD_FRACTION_BITS_MASK, div_fraction);
	// printk("xr20m117x_set_baud: msb:0x%lx lsb=0x%lx fraction=0x%lx\n",div_integer / 256, div_integer % 256, div_fraction);

	regcache_cache_bypass(s->regmap, false);

	/* Put LCR back to the normal mode */
	xr20m117x_port_write(port, XRM117X_LCR_REG, lcr);

	return DIV_ROUND_CLOSEST(clk, div_16);
}

static int xr20m117x_dump_register(struct uart_port *port)
{
	u8 lcr;
	u8 i, reg;
	lcr = xr20m117x_port_read(port, XRM117X_LCR_REG);
	xr20m117x_port_write(port, XRM117X_LCR_REG, 0x03);
	// printk("******Dump register at LCR=0x03\n");
	// for(i=0;i<16;i++)
	// {
	// 	reg = xr20m117x_port_read(port, i);
	// 	printk("Reg[0x%02x] = 0x%02x\n",i,reg);
	// }

	xr20m117x_port_write(port, XRM117X_LCR_REG, 0xBF);
	// printk("******Dump register at LCR=0xBF\n");

	// for(i=2;i<8;i++)
	// {
	// 	reg = xr20m117x_port_read(port, i);

	// 	printk("Reg[0x%02x] = 0x%02x\n",i,reg);

	// }

	xr20m117x_port_write(port, XRM117X_LCR_REG, 0x83);

	// printk("******Dump register at LCR=0x83\n");
	// for (i = 0; i < 3; i++) {
	// 	reg = xr20m117x_port_read(port, i);

	// 	printk("Reg[0x%02x] = 0x%02x\n", i, reg);
	// }

	/* Put LCR back to the normal mode */

	xr20m117x_port_write(port, XRM117X_LCR_REG, lcr);
	return 0;
}

static void xr20m117x_handle_rx(struct uart_port *port, unsigned int rxlen,
				unsigned int iir)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	unsigned int lsr = 0, ch, flag, bytes_read, i;
	bool read_lsr = (iir == XRM117X_IIR_RLSE_SRC) ? true : false;

	if (unlikely(rxlen >= sizeof(s->buf))) {
		dev_warn_ratelimited(port->dev,
				     "ttyXRM%i: Possible RX FIFO overrun: %d\n",
				     port->line, rxlen);
		port->icount.buf_overrun++;
		/* Ensure sanity of RX level */
		rxlen = sizeof(s->buf);
	}

	while (rxlen) {
		/* Only read lsr if there are possible errors in FIFO */
		if (read_lsr) {
			lsr = xr20m117x_port_read(port, XRM117X_LSR_REG);
			if (!(lsr & XRM117X_LSR_FIFOE_BIT))
				read_lsr = false; /* No errors left in FIFO */
		} else
			lsr = 0;

		if (read_lsr) {
			s->buf[0] = xr20m117x_port_read(port, XRM117X_RHR_REG);
			bytes_read = 1;
		} else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
			regcache_cache_bypass(s->regmap, true);
			regmap_raw_read(s->regmap, XRM117X_RHR_REG, s->buf,
					rxlen);
			regcache_cache_bypass(s->regmap, false);
#else
			xr20m117x_fifo_read(port, rxlen);
#endif
			bytes_read = rxlen;
		}

		lsr &= XRM117X_LSR_BRK_ERROR_MASK;

		port->icount.rx++;
		flag = TTY_NORMAL;

		if (unlikely(lsr)) {
			if (lsr & XRM117X_LSR_BI_BIT) {
				port->icount.brk++;
				if (uart_handle_break(port))
					continue;
			} else if (lsr & XRM117X_LSR_PE_BIT)
				port->icount.parity++;
			else if (lsr & XRM117X_LSR_FE_BIT)
				port->icount.frame++;
			else if (lsr & XRM117X_LSR_OE_BIT)
				port->icount.overrun++;

			lsr &= port->read_status_mask;
			if (lsr & XRM117X_LSR_BI_BIT)
				flag = TTY_BREAK;
			else if (lsr & XRM117X_LSR_PE_BIT)
				flag = TTY_PARITY;
			else if (lsr & XRM117X_LSR_FE_BIT)
				flag = TTY_FRAME;
			else if (lsr & XRM117X_LSR_OE_BIT)
				flag = TTY_OVERRUN;
		}

		for (i = 0; i < bytes_read; ++i) {
			ch = s->buf[i];
			if (uart_handle_sysrq_char(port, ch))
				continue;

			if (lsr & port->ignore_status_mask)
				continue;

			uart_insert_char(port, lsr, XRM117X_LSR_OE_BIT, ch,
					 flag);
		}
		rxlen -= bytes_read;
	}

	tty_flip_buffer_push(&port->state->port);
}

static void xr20m117x_handle_tx(struct uart_port *port)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	struct circ_buf *xmit = &port->state->xmit;
	unsigned int txlen, to_send, i;

	if (unlikely(port->x_char)) {
		xr20m117x_port_write(port, XRM117X_THR_REG, port->x_char);
		port->icount.tx++;
		port->x_char = 0;
		return;
	}

	if (uart_circ_empty(xmit) || uart_tx_stopped(port))
		return;

	/* Get length of data pending in circular buffer */
	to_send = uart_circ_chars_pending(xmit);
	if (likely(to_send)) {
		/* Limit to size of TX FIFO */
		txlen = xr20m117x_port_read(port, XRM117X_TXLVL_REG);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
		if (txlen > XRM117X_FIFO_SIZE) {
			dev_err_ratelimited(
				port->dev,
				"chip reports %d free bytes in TX fifo, but it only has %d",
				txlen, XRM117X_FIFO_SIZE);
			txlen = 0;
		}
#endif
		to_send = (to_send > txlen) ? txlen : to_send;

		/* Add data to send */
		port->icount.tx += to_send;

		/* Convert to linear buffer */
		for (i = 0; i < to_send; ++i) {
			s->buf[i] = xmit->buf[xmit->tail];
			xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		}
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
		regcache_cache_bypass(s->regmap, true);
		regmap_raw_write(s->regmap, XRM117X_THR_REG, s->buf, to_send);
		regcache_cache_bypass(s->regmap, false);
#else
		xr20m117x_fifo_write(port, to_send);
#endif
	}

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 3)
static bool xr20m117x_port_irq(struct xr20m117x_port *s, int portno)
#else
static void xr20m117x_port_irq(struct xr20m117x_port *s, int portno)
#endif
{
	struct uart_port *port = &s->p[portno].port;

	do {
		unsigned int iir, rxlen;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
		unsigned int msr;
#endif

		iir = xr20m117x_port_read(port, XRM117X_IIR_REG);
		if (iir & XRM117X_IIR_NO_INT_BIT) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 3)
			return false;
#else
			break;
#endif
		}

		iir &= XRM117X_IIR_ID_MASK;

		switch (iir) {
		case XRM117X_IIR_RDI_SRC:
		case XRM117X_IIR_RLSE_SRC:
		case XRM117X_IIR_RTOI_SRC:
		case XRM117X_IIR_XOFFI_SRC:
			rxlen = xr20m117x_port_read(port, XRM117X_RXLVL_REG);
			if (rxlen)
				xr20m117x_handle_rx(port, rxlen, iir);
			break;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
		case XRM117X_IIR_CTSRTS_SRC:
			msr = xr20m117x_port_read(port, XRM117X_MSR_REG);
			uart_handle_cts_change(port,
					       !!(msr & XRM117X_MSR_CTS_BIT));
			break;
#endif
		case XRM117X_IIR_THRI_SRC:
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
			mutex_lock(&s->mutex);
			xr20m117x_handle_tx(port);
			mutex_unlock(&s->mutex);
#else
			xr20m117x_handle_tx(port);
#endif
			break;
		default:
			dev_err_ratelimited(
				port->dev, "ttyXRM%i: Unexpected interrupt: %x",
				port->line, iir);
			break;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 3)
	} while (0);
	return true;
#else
	} while (1);
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
static irqreturn_t xr20m117x_ist(int irq, void *dev_id)
{
	struct xr20m117x_port *s = (struct xr20m117x_port *)dev_id;
	int i;

	for (i = 0; i < s->uart.nr; ++i)
		xr20m117x_port_irq(s, i);

	return IRQ_HANDLED;
}

static void xr20m117x_wq_proc(struct work_struct *ws)
{
	struct xr20m117x_one *one = to_xr20m117x_one(ws, tx_work);
	struct xr20m117x_port *s = dev_get_drvdata(one->port.dev);

	mutex_lock(&s->mutex);
	xr20m117x_handle_tx(&one->port);
	mutex_unlock(&s->mutex);
}

static void xr20m117x_stop_tx(struct uart_port *port)
{
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);
	struct circ_buf *xmit = &one->port.state->xmit;

	/* handle rs485 */
	if (one->rs485.flags & SER_RS485_ENABLED) {
		/* do nothing if current tx not yet completed */
		int lsr = xr20m117x_port_read(port, XRM117X_LSR_REG);
		if (!(lsr & XRM117X_LSR_TEMT_BIT))
			return;

		if (uart_circ_empty(xmit) &&
		    (one->rs485.delay_rts_after_send > 0))
			mdelay(one->rs485.delay_rts_after_send);
	}

	xr20m117x_port_update(port, XRM117X_IER_REG, XRM117X_IER_THRI_BIT, 0);
}

static void xr20m117x_stop_rx(struct uart_port *port)
{
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);

	one->port.read_status_mask &= ~XRM117X_LSR_DR_BIT;
	xr20m117x_port_update(port, XRM117X_IER_REG, XRM117X_LSR_DR_BIT, 0);
}

static void xr20m117x_start_tx(struct uart_port *port)
{
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);

	/* handle rs485 */
	if ((one->rs485.flags & SER_RS485_ENABLED) &&
	    (one->rs485.delay_rts_before_send > 0)) {
		mdelay(one->rs485.delay_rts_before_send);
	}

	if (!work_pending(&one->tx_work))
		schedule_work(&one->tx_work);
}

static unsigned int xr20m117x_tx_empty(struct uart_port *port)
{
	unsigned int lvl, lsr;

	lvl = xr20m117x_port_read(port, XRM117X_TXLVL_REG);
	lsr = xr20m117x_port_read(port, XRM117X_LSR_REG);

	return ((lsr & XRM117X_LSR_THRE_BIT) && !lvl) ? TIOCSER_TEMT : 0;
}

static void xr20m117x_md_proc(struct work_struct *ws)
{
	struct xr20m117x_one *one = to_xr20m117x_one(ws, md_work);

	xr20m117x_port_update(
		&one->port, XRM117X_MCR_REG, XRM117X_MCR_LOOP_BIT,
		(one->port.mctrl & TIOCM_LOOP) ? XRM117X_MCR_LOOP_BIT : 0);
}

static void xr20m117x_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);

	schedule_work(&one->md_work);
}

#else // Above is Kernel < 4.2; Below is when Kernel > 4.2
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
static irqreturn_t xr20m117x_irq(int irq, void *dev_id)
{
	struct xr20m117x_port *s = (struct xr20m117x_port *)dev_id;

	mutex_lock(&s->efr_lock);

	while (1) {
		bool keep_polling = false;
		int i;

		for (i = 0; i < s->devtype->nr_uart; ++i)
			keep_polling |= xr20m117x_port_irq(s, i);
		if (!keep_polling)
			break;
	}

	mutex_unlock(&s->efr_lock);

	return IRQ_HANDLED;
}
#else //	Kernel version < 5.8
static void xr20m117x_ist(struct kthread_work *ws)
{
	struct xr20m117x_port *s = to_xr20m117x_port(ws, irq_work);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 3)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 82)
	mutex_lock(&s->efr_lock);
#endif
	while (1) {
		bool keep_polling = false;
		int i;

		for (i = 0; i < s->devtype->nr_uart; ++i)
			keep_polling |= xr20m117x_port_irq(s, i);
		if (!keep_polling)
			break;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 82)
	mutex_unlock(&s->efr_lock);
#endif
#else
	int i;

	for (i = 0; i < s->devtype->nr_uart; ++i)
		xr20m117x_port_irq(s, i);
#endif
}

static irqreturn_t xr20m117x_irq(int irq, void *dev_id)
{
	struct xr20m117x_port *s = (struct xr20m117x_port *)dev_id;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	kthread_queue_work(&s->kworker, &s->irq_work);
#else
	queue_kthread_work(&s->kworker, &s->irq_work);
#endif

	return IRQ_HANDLED;
}
#endif //Kernel version 5.8

static void xr20m117x_tx_proc(struct kthread_work *ws)
{
	struct uart_port *port = &(to_xr20m117x_one(ws, tx_work)->port);

	if ((port->rs485.flags & SER_RS485_ENABLED) &&
	    (port->rs485.delay_rts_before_send > 0))
		msleep(port->rs485.delay_rts_before_send);

	xr20m117x_handle_tx(port);
}

static void xr20m117x_reconf_rs485(struct uart_port *port)
{
	const u32 mask =
		XRM117X_EFCR_AUTO_RS485_BIT | XRM117X_EFCR_RTS_INVERT_BIT;
	u32 efcr = 0;
	struct serial_rs485 *rs485 = &port->rs485;
	unsigned long irqflags;

	spin_lock_irqsave(&port->lock, irqflags);
	if (rs485->flags & SER_RS485_ENABLED) {
		efcr |= XRM117X_EFCR_AUTO_RS485_BIT;

		if (rs485->flags &
		    SER_RS485_RTS_ON_SEND) // Invert if send is high
			efcr |= XRM117X_EFCR_RTS_INVERT_BIT;
	}
	spin_unlock_irqrestore(&port->lock, irqflags);

	xr20m117x_port_update(port, XRM117X_EFCR_REG, mask, efcr);
}

static void xr20m117x_reg_proc(struct kthread_work *ws)
{
	struct xr20m117x_one *one = to_xr20m117x_one(ws, reg_work);
	struct xr20m117x_one_config config;
	unsigned long irqflags;

	spin_lock_irqsave(&one->port.lock, irqflags);
	config = one->config;
	memset(&one->config, 0, sizeof(one->config));
	spin_unlock_irqrestore(&one->port.lock, irqflags);

	if (config.flags & XRM117X_RECONF_MD) {
		xr20m117x_port_update(&one->port, XRM117X_MCR_REG,
				      XRM117X_MCR_LOOP_BIT,
				      (one->port.mctrl & TIOCM_LOOP) ?
					      XRM117X_MCR_LOOP_BIT :
					      0);
		xr20m117x_port_update(&one->port, XRM117X_MCR_REG,
				      XRM117X_MCR_RTS_BIT,
				      (one->port.mctrl & TIOCM_RTS) ?
					      XRM117X_MCR_RTS_BIT :
					      0);
		xr20m117x_port_update(&one->port, XRM117X_MCR_REG,
				      XRM117X_MCR_DTR_BIT,
				      (one->port.mctrl & TIOCM_DTR) ?
					      XRM117X_MCR_DTR_BIT :
					      0);
	}
	if (config.flags & XRM117X_RECONF_IER)
		xr20m117x_port_update(&one->port, XRM117X_IER_REG,
				      config.ier_clear, 0);

	if (config.flags & XRM117X_RECONF_RS485)
		xr20m117x_reconf_rs485(&one->port);
}

static void xr20m117x_ier_clear(struct uart_port *port, u8 bit)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);

	one->config.flags |= XRM117X_RECONF_IER;
	one->config.ier_clear |= bit;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	kthread_queue_work(&s->kworker, &one->reg_work);
#else
	queue_kthread_work(&s->kworker, &one->reg_work);
#endif
}

static void xr20m117x_stop_tx(struct uart_port *port)
{
	xr20m117x_ier_clear(port, XRM117X_IER_THRI_BIT);
}

static void xr20m117x_stop_rx(struct uart_port *port)
{
	xr20m117x_ier_clear(port, XRM117X_IER_RDI_BIT);
}

static void xr20m117x_start_tx(struct uart_port *port)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	kthread_queue_work(&s->kworker, &one->tx_work);
#else
	queue_kthread_work(&s->kworker, &one->tx_work);
#endif
}

static unsigned int xr20m117x_tx_empty(struct uart_port *port)
{
	unsigned int lsr;

	lsr = xr20m117x_port_read(port, XRM117X_LSR_REG);

	return (lsr & XRM117X_LSR_TEMT_BIT) ? TIOCSER_TEMT : 0;
}

static void xr20m117x_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);

	one->config.flags |= XRM117X_RECONF_MD;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	kthread_queue_work(&s->kworker, &one->reg_work);
#else
	queue_kthread_work(&s->kworker, &one->reg_work);
#endif
}
#endif //Kernel > 4.2

static unsigned int xr20m117x_get_mctrl(struct uart_port *port)
{
	/* DCD and DSR are not wired and CTS/RTS is handled automatically
	 * so just indicate DSR and CAR asserted
	 */
	return TIOCM_DSR | TIOCM_CAR;
}

static void xr20m117x_break_ctl(struct uart_port *port, int break_state)
{
	xr20m117x_port_update(port, XRM117X_LCR_REG, XRM117X_LCR_TXBREAK_BIT,
			      break_state ? XRM117X_LCR_TXBREAK_BIT : 0);
}

static void xr20m117x_set_termios(struct uart_port *port,
				  struct ktermios *termios,
				  struct ktermios *old)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	unsigned int lcr, flow = 0;
	int baud;

	/* Mask termios capabilities we don't support */
	termios->c_cflag &= ~CMSPAR;

	/* Word size */
	switch (termios->c_cflag & CSIZE) {
	case CS5:
		lcr = XRM117X_LCR_WORD_LEN_5;
		break;
	case CS6:
		lcr = XRM117X_LCR_WORD_LEN_6;
		break;
	case CS7:
		lcr = XRM117X_LCR_WORD_LEN_7;
		break;
	case CS8:
		lcr = XRM117X_LCR_WORD_LEN_8;
		break;
	default:
		lcr = XRM117X_LCR_WORD_LEN_8;
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= CS8;
		break;
	}

	/* Parity */
	if (termios->c_cflag & PARENB) {
		lcr |= XRM117X_LCR_PARITY_BIT;
		if (!(termios->c_cflag & PARODD))
			lcr |= XRM117X_LCR_EVENPARITY_BIT;
	}

	/* Stop bits */
	if (termios->c_cflag & CSTOPB)
		lcr |= XRM117X_LCR_STOPLEN_BIT; /* 2 stops */

	/* Set read status mask */
	port->read_status_mask = XRM117X_LSR_OE_BIT;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |=
			XRM117X_LSR_PE_BIT | XRM117X_LSR_FE_BIT;
	if (termios->c_iflag & (BRKINT | PARMRK))
		port->read_status_mask |= XRM117X_LSR_BI_BIT;

	/* Set status ignore mask */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNBRK)
		port->ignore_status_mask |= XRM117X_LSR_BI_BIT;
	if (!(termios->c_cflag & CREAD))
		port->ignore_status_mask |= XRM117X_LSR_BRK_ERROR_MASK;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 82)
	/* As above, claim the mutex while accessing the EFR. */
	mutex_lock(&s->efr_lock);
#endif

	xr20m117x_port_write(port, XRM117X_LCR_REG, XRM117X_LCR_CONF_MODE_B);

	/* Configure flow control */
	regcache_cache_bypass(s->regmap, true);
	xr20m117x_port_write(port, XRM117X_XON1_REG, termios->c_cc[VSTART]);
	xr20m117x_port_write(port, XRM117X_XOFF1_REG, termios->c_cc[VSTOP]);
	if (termios->c_cflag & CRTSCTS)
		flow |= XRM117X_EFR_AUTOCTS_BIT | XRM117X_EFR_AUTORTS_BIT;
	if (termios->c_iflag & IXON)
		flow |= XRM117X_EFR_SWFLOW3_BIT;
	if (termios->c_iflag & IXOFF)
		flow |= XRM117X_EFR_SWFLOW1_BIT;

	xr20m117x_port_write(port, XRM117X_EFR_REG, flow);
	regcache_cache_bypass(s->regmap, false);

	/* Update LCR register */
	xr20m117x_port_write(port, XRM117X_LCR_REG, lcr);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 82)
	mutex_unlock(&s->efr_lock);
#endif

	/* Get baud rate generator configuration */
	baud = uart_get_baud_rate(port, termios, old,
				  port->uartclk / 16 / 4 / 0xffff,
				  port->uartclk / 16);

	/* Setup baudrate generator */
	baud = xr20m117x_set_baud(port, baud);

	if (baud < 0) {
		/* baud to set is over range */
		printk(" xr20m117x_set_termios: baud not supported, remain unchanged \n");
		return;
	}
	/* Update timeout according to new baud rate */
	uart_update_timeout(port, termios->c_cflag, baud);
	xr20m117x_dump_register(port);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
#if defined(TIOCSRS485) && defined(TIOCGRS485)
static void xr20m117x_config_rs485(struct uart_port *port,
				   struct serial_rs485 *rs485)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);

	one->rs485 = *rs485;

	if (one->rs485.flags & SER_RS485_ENABLED) {
#else
	if (port->rs485.flags & SER_RS485_ENABLED) {
#endif
		xr20m117x_port_update(port, XRM117X_EFCR_REG,
				      XRM117X_EFCR_AUTO_RS485_BIT,
				      XRM117X_EFCR_AUTO_RS485_BIT);
	} else {
		xr20m117x_port_update(port, XRM117X_EFCR_REG,
				      XRM117X_EFCR_AUTO_RS485_BIT, 0);
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
	port->rs485 = *rs485;
#endif
}
#endif
#else // Above is Kernel < 4.1; below is Kernel > 4.1
static int xr20m117x_config_rs485(struct uart_port *port,
				  struct serial_rs485 *rs485)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
	const u32 mask =
		XRM117X_EFCR_AUTO_RS485_BIT | XRM117X_EFCR_RTS_INVERT_BIT;
	u32 efcr = 0;
#else
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);
#endif

	if (rs485->flags & SER_RS485_ENABLED) {
		bool rts_during_rx, rts_during_tx;

		rts_during_rx = rs485->flags & SER_RS485_RTS_AFTER_SEND;
		rts_during_tx = rs485->flags & SER_RS485_RTS_ON_SEND;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
		efcr |= XRM117X_EFCR_AUTO_RS485_BIT;

		if (!rts_during_rx && rts_during_tx)
			efcr |= XRM117X_EFCR_RTS_INVERT_BIT;
		else if (rts_during_rx && !rts_during_tx)
			// default
			else
#else
		if (rts_during_rx == rts_during_tx)
#endif
				dev_err(port->dev,
					"unsupported RTS signalling on_send:%d after_send:%d - exactly one of RS485 RTS flags should be set\n",
					rts_during_tx, rts_during_rx);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
		/*
		 * RTS signal is handled by HW, it's timing can't be influenced.
		 * However, it's sometimes useful to delay TX even without RTS
		 * control therefore we try to handle .delay_rts_before_send.
		 */
		if (rs485->delay_rts_after_send)
			return -EINVAL;
#endif
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
	xr20m117x_port_update(port, XRM117X_EFCR_REG, mask, efcr);
#endif

	port->rs485 = *rs485;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	one->config.flags |= XRM117X_RECONF_RS485;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	kthread_queue_work(&s->kworker, &one->reg_work);
#else
	queue_kthread_work(&s->kworker, &one->reg_work);
#endif
#endif

	return 0;
}
#endif

//#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
static int xr20m117x_ioctl(struct uart_port *port, unsigned int cmd,
			   unsigned long arg)
{
	int rv = -ENOIOCTLCMD;
	struct xrioctl_rw_reg ioctlrwarg;
#if defined(TIOCSRS485) && defined(TIOCGRS485)
	struct serial_rs485 rs485;
#endif
	switch (cmd) {
#if defined(TIOCSRS485) && defined(TIOCGRS485)
	case TIOCSRS485:
		if (copy_from_user(&rs485, (void __user *)arg, sizeof(rs485)))
			return -EFAULT;
		xr20m117x_config_rs485(port, &rs485);
		return 0;
	case TIOCGRS485:
		if (copy_to_user((void __user *)arg,
				 &(to_xr20m117x_one(port, port)->rs485),
				 sizeof(rs485)))
			return -EFAULT;
		return 0;
#endif
	case XRM_READ_REG:
		if (copy_from_user(&ioctlrwarg, (void *)arg,
				   sizeof(ioctlrwarg)))
			return -EFAULT;
		ioctlrwarg.regvalue = xr20m117x_port_read(port, ioctlrwarg.reg);
		//			printk("xrm_ioctl read reg[0x%02x]=0x%02x \n",ioctlrwarg.reg,ioctlrwarg.regvalue);
		if (copy_to_user((void *)arg, &ioctlrwarg, sizeof(ioctlrwarg)))
			return -EFAULT;
		rv = 0;
		break;
	case XRM_WRITE_REG:
		if (copy_from_user(&ioctlrwarg, (void *)arg,
				   sizeof(ioctlrwarg)))
			return -EFAULT;
		xr20m117x_port_write(port, ioctlrwarg.reg, ioctlrwarg.regvalue);
		//			printk("xrm_ioctl write reg[0x%02x]=0x%02x \n",ioctlrwarg.reg,ioctlrwarg.regvalue);
		rv = 0;
		break;
	default:
		break;
	}

	return rv;
}
//#endif

static int xr20m117x_startup(struct uart_port *port)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	struct xr20m117x_one *one = to_xr20m117x_one(port, port);
#endif
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
	unsigned int val;

	xr20m117x_power(port, 1);

	/* Reset FIFOs*/
	val = XRM117X_FCR_RXRESET_BIT | XRM117X_FCR_TXRESET_BIT;
	xr20m117x_port_write(port, XRM117X_FCR_REG, val);
	udelay(5);
	xr20m117x_port_write(port, XRM117X_FCR_REG, XRM117X_FCR_FIFO_BIT);

	/* Enable EFR */
	xr20m117x_port_write(port, XRM117X_LCR_REG, XRM117X_LCR_CONF_MODE_B);

	regcache_cache_bypass(s->regmap, true);

	/* Enable write access to enhanced features and internal clock div */
	xr20m117x_port_write(port, XRM117X_EFR_REG, XRM117X_EFR_ENABLE_BIT);

	/* Enable TCR/TLR */
	xr20m117x_port_update(port, XRM117X_MCR_REG, XRM117X_MCR_TCRTLR_BIT,
			      XRM117X_MCR_TCRTLR_BIT);

	/* Configure flow control levels */
	/* Flow control halt level 48, resume level 24 */
	xr20m117x_port_write(port, XRM117X_TCR_REG,
			     XRM117X_TCR_RX_RESUME(24) |
				     XRM117X_TCR_RX_HALT(48));

	regcache_cache_bypass(s->regmap, false);

	/* Now, initialize the UART */
	xr20m117x_port_write(port, XRM117X_LCR_REG, XRM117X_LCR_WORD_LEN_8);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	/* Enable IrDA mode if requested in DT */
	/* This bit must be written with LCR = 0 */
	xr20m117x_port_update(port, XRM117X_MCR_REG, XRM117X_MCR_IRDA_BIT,
			      one->irda_mode ? XRM117X_MCR_IRDA_BIT : 0);
#endif

	/* Enable the Rx and Tx FIFO */
	xr20m117x_port_update(
		port, XRM117X_EFCR_REG,
		XRM117X_EFCR_RXDISABLE_BIT | XRM117X_EFCR_TXDISABLE_BIT, 0);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
	/* Enable RX, TX, CTS change interrupts */
	val = XRM117X_IER_RDI_BIT | XRM117X_IER_THRI_BIT | XRM117X_IER_CTSI_BIT;
#else
	/* Enable RX, TX interrupts */
	val = XRM117X_IER_RDI_BIT | XRM117X_IER_THRI_BIT;
#endif
	xr20m117x_port_write(port, XRM117X_IER_REG, val);

	return 0;
}

static void xr20m117x_shutdown(struct uart_port *port)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);
#endif

	/* Disable all interrupts */
	xr20m117x_port_write(port, XRM117X_IER_REG, 0);
	/* Disable TX/RX */
	xr20m117x_port_update(
		port, XRM117X_EFCR_REG,
		XRM117X_EFCR_RXDISABLE_BIT | XRM117X_EFCR_TXDISABLE_BIT,
		XRM117X_EFCR_RXDISABLE_BIT | XRM117X_EFCR_TXDISABLE_BIT);

	xr20m117x_power(port, 0);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 0)
	flush_kthread_worker(&s->kworker);
#else
		kthread_flush_worker(&s->kworker);
#endif
}

static const char *xr20m117x_type(struct uart_port *port)
{
	struct xr20m117x_port *s = dev_get_drvdata(port->dev);

	return (port->type == PORT_SC16IS7XX) ? s->devtype->name : NULL;
}

static int xr20m117x_request_port(struct uart_port *port)
{
	/* Do nothing */
	return 0;
}

static void xr20m117x_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_SC16IS7XX;
}

static int xr20m117x_verify_port(struct uart_port *port,
				 struct serial_struct *s)
{
	if ((s->type != PORT_UNKNOWN) && (s->type != PORT_SC16IS7XX))
		return -EINVAL;
	if (s->irq != port->irq)
		return -EINVAL;

	return 0;
}

static void xr20m117x_pm(struct uart_port *port, unsigned int state,
			 unsigned int oldstate)
{
	xr20m117x_power(port, (state == UART_PM_STATE_ON) ? 1 : 0);
}

static void xr20m117x_null_void(struct uart_port *port)
{
	/* Do nothing */
}

static const struct uart_ops xr20m117x_ops = {
	.tx_empty = xr20m117x_tx_empty,
	.set_mctrl = xr20m117x_set_mctrl,
	.get_mctrl = xr20m117x_get_mctrl,
	.stop_tx = xr20m117x_stop_tx,
	.start_tx = xr20m117x_start_tx,
	.stop_rx = xr20m117x_stop_rx,
	.break_ctl = xr20m117x_break_ctl,
	.startup = xr20m117x_startup,
	.shutdown = xr20m117x_shutdown,
	.set_termios = xr20m117x_set_termios,
	.type = xr20m117x_type,
	.request_port = xr20m117x_request_port,
	.release_port = xr20m117x_null_void,
	.config_port = xr20m117x_config_port,
	.verify_port = xr20m117x_verify_port,
	//#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0)
	.ioctl = xr20m117x_ioctl,
	//#endif
	.pm = xr20m117x_pm,
};

#ifdef CONFIG_GPIOLIB
static int xr20m117x_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	unsigned int val;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
	struct xr20m117x_port *s =
		container_of(chip, struct xr20m117x_port, gpio);
#else
	struct xr20m117x_port *s = gpiochip_get_data(chip);
#endif
	struct uart_port *port = &s->p[0].port;

	val = xr20m117x_port_read(port, XRM117X_IOSTATE_REG);

	return !!(val & BIT(offset));
}

static void xr20m117x_gpio_set(struct gpio_chip *chip, unsigned offset, int val)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
	struct xr20m117x_port *s =
		container_of(chip, struct xr20m117x_port, gpio);
#else
	struct xr20m117x_port *s = gpiochip_get_data(chip);
#endif
	struct uart_port *port = &s->p[0].port;

	xr20m117x_port_update(port, XRM117X_IOSTATE_REG, BIT(offset),
			      val ? BIT(offset) : 0);
}

static int xr20m117x_gpio_direction_input(struct gpio_chip *chip,
					  unsigned offset)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
	struct xr20m117x_port *s =
		container_of(chip, struct xr20m117x_port, gpio);
#else
	struct xr20m117x_port *s = gpiochip_get_data(chip);
#endif
	struct uart_port *port = &s->p[0].port;

	xr20m117x_port_update(port, XRM117X_IODIR_REG, BIT(offset), 0);

	return 0;
}

static int xr20m117x_gpio_direction_output(struct gpio_chip *chip,
					   unsigned offset, int val)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
	struct xr20m117x_port *s =
		container_of(chip, struct xr20m117x_port, gpio);
#else
	struct xr20m117x_port *s = gpiochip_get_data(chip);
#endif
	struct uart_port *port = &s->p[0].port;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	u8 state = xr20m117x_port_read(port, XRM117X_IOSTATE_REG);

	if (val)
		state |= BIT(offset);
	else
		state &= ~BIT(offset);
	xr20m117x_port_write(port, XRM117X_IOSTATE_REG, state);
	xr20m117x_port_update(port, XRM117X_IODIR_REG, BIT(offset),
			      BIT(offset));
#else
	xr20m117x_port_update(port, XRM117X_IOSTATE_REG, BIT(offset),
			      val ? BIT(offset) : 0);
	xr20m117x_port_update(port, XRM117X_IODIR_REG, BIT(offset),
			      BIT(offset));
#endif
	return 0;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
static int xr20m117x_probe(struct device *dev,
			   struct xr20m117x_devtype *devtype,
			   struct regmap *regmap, int irq, unsigned long flags)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
static int xr20m117x_probe(struct device *dev,
			   const struct xr20m117x_devtype *devtype,
			   struct regmap *regmap, int irq)
#else
	static int xr20m117x_probe(struct device * dev,
				   const struct xr20m117x_devtype *devtype,
				   struct regmap *regmap, int irq,
				   unsigned long flags)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	struct sched_param sched_param = { .sched_priority = MAX_RT_PRIO / 2 };
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
	unsigned long freq = 0, *pfreq = dev_get_platdata(dev);
	u32 uartclk = 0;
#else
	unsigned long freq, *pfreq = dev_get_platdata(dev);
#endif
	int i, ret;
	struct xr20m117x_port *s;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	unsigned int val;
#endif

	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	/*
	 * This device does not have an identification register that would
	 * tell us if we are really connected to the correct device.
	 * The best we can do is to check if communication is at all possible.
	 */
	ret = regmap_read(regmap, XRM117X_LSR_REG << XRM117X_REG_SHIFT, &val);
	if (ret < 0)
		return ret;
#endif
	/* Alloc port structure */
	/* Alloc port structure */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
	s = devm_kzalloc(dev, struct_size(s, p, devtype->nr_uart), GFP_KERNEL);
#else
	s = devm_kzalloc(dev,
			 sizeof(*s) + sizeof(struct xr20m117x_one) *
					      devtype->nr_uart,
			 GFP_KERNEL);
#endif

	if (!s) {
		dev_err(dev, "Error allocating port structure\n");
		return -ENOMEM;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
	/* Always ask for fixed clock rate from a property. */
	device_property_read_u32(dev, "clock-frequency", &uartclk);
#endif

	s->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(s->clk)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
		if (uartclk)
			freq = uartclk;
#endif
		if (pfreq)
			freq = *pfreq;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
		if (freq)
			dev_dbg(dev, "Clock frequency: %luHz\n", freq);
#endif
		else
			return PTR_ERR(s->clk);
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
		ret = clk_prepare_enable(s->clk);
		if (ret)
			return ret;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)
		clk_prepare_enable(s->clk);
#endif //KERNEL > 4.18 or 4.1~4.18
		freq = clk_get_rate(s->clk);
	}

	s->regmap = regmap;
	s->devtype = devtype;
	dev_set_drvdata(dev, s);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 82)
	mutex_init(&s->efr_lock);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	/* Register UART driver */
	s->uart.owner = THIS_MODULE;
	s->uart.dev_name = "ttyXRM";
	s->uart.nr = devtype->nr_uart;
	ret = uart_register_driver(&s->uart);
	if (ret) {
		dev_err(dev, "Registering UART driver failed\n");
		goto out_clk;
	}
#else
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 0)
	init_kthread_worker(&s->kworker);
	init_kthread_work(&s->irq_work, xr20m117x_ist);
#else
	kthread_init_worker(&s->kworker);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
	kthread_init_work(&s->irq_work, xr20m117x_ist);
#endif //Kernel version 5.8
#endif //Kernel < 4.9
	s->kworker_task =
		kthread_run(kthread_worker_fn, &s->kworker, "xr20m117x");
	if (IS_ERR(s->kworker_task)) {
		ret = PTR_ERR(s->kworker_task);
		goto out_clk;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
	sched_setscheduler(s->kworker_task, SCHED_FIFO, &sched_param);
#else
	sched_set_fifo(s->kworker_task);
#endif
#endif // Kernel < 4.3

#ifdef CONFIG_GPIOLIB
	if (devtype->nr_gpio) {
		/* Setup GPIO cotroller */
		s->gpio.owner = THIS_MODULE;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
		s->gpio.dev = dev;
#else
		s->gpio.parent = dev;
#endif
		s->gpio.label = dev_name(dev);
		s->gpio.direction_input = xr20m117x_gpio_direction_input;
		s->gpio.get = xr20m117x_gpio_get;
		s->gpio.direction_output = xr20m117x_gpio_direction_output;
		s->gpio.set = xr20m117x_gpio_set;
		s->gpio.base = -1;
		s->gpio.ngpio = devtype->nr_gpio;
		s->gpio.can_sleep = 1;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
		ret = gpiochip_add(&s->gpio);
#else
		ret = gpiochip_add_data(&s->gpio, s);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
		if (ret)
			goto out_uart;
#else
		if (ret)
			goto out_thread;
#endif
	}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
	mutex_init(&s->mutex);
#endif
	/* reset device, purging any pending irq / data */
	for (i = 0; i < devtype->nr_uart; ++i) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
		s->p[i].port.line = i;
#else
		s->p[i].line = i;
		s->p[i].port.line = xr20m117x_alloc_line();
		if (s->p[i].port.line >= XRM117X_MAX_DEVS) {
			ret = -ENOMEM;
			goto out_ports;
		}
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
		s->p[i].port.rs485_config = xr20m117x_config_rs485;
#endif
		s->p[i].port.dev = dev;
		s->p[i].port.irq = irq;
		s->p[i].port.type = PORT_SC16IS7XX;
		s->p[i].port.fifosize = XRM117X_FIFO_SIZE;
		s->p[i].port.flags = UPF_FIXED_TYPE | UPF_LOW_LATENCY;
		s->p[i].port.iotype = UPIO_PORT;
		s->p[i].port.uartclk = freq;
		s->p[i].port.ops = &xr20m117x_ops;

		/* Disable all interrupts */
		xr20m117x_port_write(&s->p[i].port, XRM117X_IER_REG, 0);
		/* Disable TX/RX */
		xr20m117x_port_write(&s->p[i].port, XRM117X_EFCR_REG,
				     XRM117X_EFCR_RXDISABLE_BIT |
					     XRM117X_EFCR_TXDISABLE_BIT);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
		/* Initialize queue for start TX */
		INIT_WORK(&s->p[i].tx_work, xr20m117x_wq_proc);
		/* Initialize queue for changing mode */
		INIT_WORK(&s->p[i].md_work, xr20m117x_md_proc);
		/* Register port */
		uart_add_one_port(&s->uart, &s->p[i].port);
#else
		/* Initialize kthread work structs */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 0)
		init_kthread_work(&s->p[i].tx_work, xr20m117x_tx_proc);
		init_kthread_work(&s->p[i].reg_work, xr20m117x_reg_proc);
#else
		kthread_init_work(&s->p[i].tx_work, xr20m117x_tx_proc);
		kthread_init_work(&s->p[i].reg_work, xr20m117x_reg_proc);
#endif
		/* Register port */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
		uart_add_one_port(&xr20m117x_uart, &s->p[i].port);
#else
		uart_add_one_port(&s->uart, &s->p[i].port);
#endif

		/* Enable EFR */
		xr20m117x_port_write(&s->p[i].port, XRM117X_LCR_REG,
				     XRM117X_LCR_CONF_MODE_B);

		regcache_cache_bypass(s->regmap, true);

		/* Enable write access to enhanced features */
		xr20m117x_port_write(&s->p[i].port, XRM117X_EFR_REG,
				     XRM117X_EFR_ENABLE_BIT);

		regcache_cache_bypass(s->regmap, false);

		/* Restore access to general registers */
		xr20m117x_port_write(&s->p[i].port, XRM117X_LCR_REG, 0x00);
#endif
		/* Go to suspend mode */
		xr20m117x_power(&s->p[i].port, 0);
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
	/* Setup interrupt */
	ret = devm_request_threaded_irq(
		dev, irq, NULL, xr20m117x_ist,
		IRQF_ONESHOT | IRQF_TRIGGER_FALLING | flags, dev_name(dev), s);
	//printk("xr20m117x_probe devm_request_threaded_irq =%d\n",ret);

	if (!ret)
		return 0;

	mutex_destroy(&s->mutex);

#ifdef CONFIG_GPIOLIB
	if (devtype->nr_gpio)
		gpiochip_remove(&s->gpio);

out_uart:
#endif
	uart_unregister_driver(&s->uart);
#else //KERNEL < 4.2
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	if (dev->of_node) {
		struct property *prop;
		const __be32 *p;
		u32 u;

		of_property_for_each_u32 (dev->of_node, "irda-mode-ports", prop,
					  p, u)
			if (u < devtype->nr_uart)
				s->p[u].irda_mode = true;
	}

	/*
	 * Setup interrupt. We first try to acquire the IRQ line as level IRQ.
	 * If that succeeds, we can allow sharing the interrupt as well.
	 * In case the interrupt controller doesn't support that, we fall
	 * back to a non-shared falling-edge trigger.
	 */
	ret = devm_request_threaded_irq(dev, irq, NULL, xr20m117x_irq,
					IRQF_TRIGGER_LOW | IRQF_SHARED |
						IRQF_ONESHOT,
					dev_name(dev), s);
	if (!ret)
		return 0;

	ret = devm_request_threaded_irq(dev, irq, NULL, xr20m117x_irq,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					dev_name(dev), s);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 2)
	/* Setup interrupt */
	ret = devm_request_irq(dev, irq, xr20m117x_irq, IRQF_ONESHOT | flags,
			       dev_name(dev), s);
#else
	/* Setup interrupt */
	ret = devm_request_irq(dev, irq, xr20m117x_irq, flags, dev_name(dev),
			       s);
#endif //Kernel version 5.8
	if (!ret)
		return 0;

out_ports:
	for (i--; i >= 0; i--) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
		uart_remove_one_port(&xr20m117x_uart, &s->p[i].port);
		clear_bit(s->p[i].port.line, &xr20m117x_lines);
#else
		uart_remove_one_port(&s->uart, &s->p[i].port);
#endif
	}

#ifdef CONFIG_GPIOLIB
	if (devtype->nr_gpio)
		gpiochip_remove(&s->gpio);

out_thread:
#endif
	kthread_stop(s->kworker_task);
#endif //KERNEL < 4.2

out_clk:
	if (!IS_ERR(s->clk))
		clk_disable_unprepare(s->clk);

	return ret;
}

static int xr20m117x_remove(struct device *dev)
{
	struct xr20m117x_port *s = dev_get_drvdata(dev);
	int i;

#ifdef CONFIG_GPIOLIB
	if (s->devtype->nr_gpio)
		gpiochip_remove(&s->gpio);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
	for (i = 0; i < s->uart.nr; i++) {
		cancel_work_sync(&s->p[i].tx_work);
		cancel_work_sync(&s->p[i].md_work);
		uart_remove_one_port(&s->uart, &s->p[i].port);
		xr20m117x_power(&s->p[i].port, 0);
	}

	mutex_destroy(&s->mutex);
#else
	for (i = 0; i < s->devtype->nr_uart; i++) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
		uart_remove_one_port(&s->uart, &s->p[i].port);
#else
		uart_remove_one_port(&xr20m117x_uart, &s->p[i].port);
#endif
		clear_bit(s->p[i].port.line, &xr20m117x_lines);
		xr20m117x_power(&s->p[i].port, 0);
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 0)
	flush_kthread_worker(&s->kworker);
#else
	kthread_flush_worker(&s->kworker);
#endif
	kthread_stop(s->kworker_task);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	uart_unregister_driver(&s->uart);
#endif

	if (!IS_ERR(s->clk))
		clk_disable_unprepare(s->clk);

	return 0;
}

static const struct of_device_id __maybe_unused xr20m117x_dt_ids[] = {
	{
		.compatible = "exar,xr20m1170",
		.data = &xr20m1170_devtype,
	},
	{
		.compatible = "exar,xr20m1172",
		.data = &xr20m1172_devtype,
	},
	{}
};
MODULE_DEVICE_TABLE(of, xr20m117x_dt_ids);

static struct regmap_config regcfg = {
	.reg_bits = 7,
	.pad_bits = 1,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.volatile_reg = xr20m117x_regmap_volatile,
	.precious_reg = xr20m117x_regmap_precious,
};

#ifdef CONFIG_SERIAL_XR20M117X_SPI
static int xr20m117x_spi_probe(struct spi_device *spi)
{
	const struct xr20m117x_devtype *devtype;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
	unsigned long flags = 0;
#endif
	struct regmap *regmap;
	int ret;

	/* Setup SPI bus */
	spi->bits_per_word = 8;
	/* only supports mode 0 on SC16IS762 */
	spi->mode = spi->mode ?: SPI_MODE_0;
	spi->max_speed_hz = spi->max_speed_hz ?: 15000000;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	if (spi->dev.of_node) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
		devtype = device_get_match_data(&spi->dev);
		if (!devtype)
			return -ENODEV;
#else
		const struct of_device_id *of_id =
			of_match_device(xr20m117x_dt_ids, &spi->dev);

		if (!of_id)
			return -ENODEV;

		devtype = (struct xr20m117x_devtype *)of_id->data;
#endif
	} else {
		const struct spi_device_id *id_entry = spi_get_device_id(spi);

		devtype = (struct xr20m117x_devtype *)id_entry->driver_data;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
		flags = IRQF_TRIGGER_FALLING;
#endif
	}

	regcfg.max_register =
		(0xf << XRM117X_REG_SHIFT) | (devtype->nr_uart - 1);
	regmap = devm_regmap_init_spi(spi, &regcfg);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	return xr20m117x_probe(&spi->dev, devtype, regmap, spi->irq);
#else
	return xr20m117x_probe(&spi->dev, devtype, regmap, spi->irq, flags);
#endif
}

static int xr20m117x_spi_remove(struct spi_device *spi)
{
	return xr20m117x_remove(&spi->dev);
}

static const struct spi_device_id xr20m117x_spi_id_table[] = {
	{
		"xr20m1170",
		(kernel_ulong_t)&xr20m1170_devtype,
	},
	{
		"xr20m1172",
		(kernel_ulong_t)&xr20m1172_devtype,
	},
	{}
};

MODULE_DEVICE_TABLE(spi, xr20m117x_spi_id_table);

static struct spi_driver xr20m117x_spi_uart_driver = {
	.driver = {
		.name		= XRM117X_NAME,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)	
		.of_match_table	= xr20m117x_dt_ids,
#else		
		.of_match_table	= of_match_ptr(xr20m117x_dt_ids),
#endif	
	},
	.probe		= xr20m117x_spi_probe,
	.remove		= xr20m117x_spi_remove,
	.id_table	= xr20m117x_spi_id_table,
};

MODULE_ALIAS("spi:xr20m117x");
#endif //CONFIG_SERIAL_XR20M117X_SPI

#ifdef CONFIG_SERIAL_XR20M117X_I2C
static int xr20m117x_i2c_probe(struct i2c_client *i2c,
			       const struct i2c_device_id *id)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
	struct xr20m117x_devtype *devtype;
#else
	const struct xr20m117x_devtype *devtype;
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
	unsigned long flags = 0;
#endif
	struct regmap *regmap;
	int ret;
	if (i2c->dev.of_node) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
		devtype = device_get_match_data(&i2c->dev);
		if (!devtype)
			return -ENODEV;
#else //KERNEL >= 5.2
		const struct of_device_id *of_id =
			of_match_device(xr20m117x_dt_ids, &i2c->dev);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
		if (!of_id)
			return -ENODEV;
#endif

		devtype = (struct xr20m117x_devtype *)of_id->data;
#endif //KERNEL < 5.2
	} else {
		devtype = (struct xr20m117x_devtype *)id->driver_data;
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
		flags = IRQF_TRIGGER_FALLING;
#endif
	}

	regcfg.max_register =
		(0xf << XRM117X_REG_SHIFT) | (devtype->nr_uart - 1);
	regmap = devm_regmap_init_i2c(i2c, &regcfg);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	ret = xr20m117x_probe(&i2c->dev, devtype, regmap, i2c->irq);
#else
	ret = xr20m117x_probe(&i2c->dev, devtype, regmap, i2c->irq, flags);
#endif
	return ret;
}

static int xr20m117x_i2c_remove(struct i2c_client *client)
{
	return xr20m117x_remove(&client->dev);
}

static const struct i2c_device_id xr20m117x_i2c_id_table[] = {
	{
		"xr20m1170",
		(kernel_ulong_t)&xr20m1170_devtype,
	},
	{
		"xr20m1172",
		(kernel_ulong_t)&xr20m1172_devtype,
	},
	{}
};
MODULE_DEVICE_TABLE(i2c, xr20m117x_i2c_id_table);

static struct i2c_driver xr20m117x_i2c_uart_driver = {
	.driver = {
		.name		= XRM117X_NAME,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
		.owner		= THIS_MODULE,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)	
		.of_match_table	= xr20m117x_dt_ids,
#else
		.of_match_table	= of_match_ptr(xr20m117x_dt_ids),
#endif	
	},
	.probe		= xr20m117x_i2c_probe,
	.remove		= xr20m117x_i2c_remove,
	.id_table	= xr20m117x_i2c_id_table,
};
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
module_i2c_driver(xr20m117x_i2c_uart_driver);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
MODULE_ALIAS("i2c:xr20m117x");
#endif
#endif //CONFIG_SERIAL_XR20M117X_I2C

static int __init xr20m117x_init(void)
{
	int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
	ret = uart_register_driver(&xr20m117x_uart);
	if (ret) {
		pr_err("Registering UART driver failed\n");
		return ret;
	}
#endif

#ifdef CONFIG_SERIAL_XR20M117X_I2C
	ret = i2c_add_driver(&xr20m117x_i2c_uart_driver);
	if (ret < 0) {
		pr_err("failed to init xr20m117x i2c --> %d\n", ret);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 12)
		goto err_i2c;
#else
		return ret;
#endif
	}
#endif

#ifdef CONFIG_SERIAL_XR20M117X_SPI
	ret = spi_register_driver(&xr20m117x_spi_uart_driver);
	if (ret < 0) {
		pr_err("failed to init xr20m117x spi --> %d\n", ret);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 12)
		goto err_spi;
#else
		return ret;
#endif
	}
#endif

	return ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 12)
#ifdef CONFIG_SERIAL_XR20M117X_SPI
err_spi:
#endif
#ifdef CONFIG_SERIAL_XR20M117X_I2C
	i2c_del_driver(&xr20m117x_i2c_uart_driver);
err_i2c:
#endif
	uart_unregister_driver(&xr20m117x_uart);
	return ret;
#endif //KERNEL >= 5.0.12
}
module_init(xr20m117x_init);

static void __exit xr20m117x_exit(void)
{
#ifdef CONFIG_SERIAL_XR20M117X_I2C
	i2c_del_driver(&xr20m117x_i2c_uart_driver);
#endif

#ifdef CONFIG_SERIAL_XR20M117X_SPI
	spi_unregister_driver(&xr20m117x_spi_uart_driver);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
	uart_unregister_driver(&xr20m117x_uart);
#endif
}
module_exit(xr20m117x_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ted Lin <tedlin@maxlinear.com>");
MODULE_DESCRIPTION("XR20M117X SPI/I2C serial driver V1.4");

// SPDX-License-Identifier: GPL-2.0+
/*
 * MaxLinear/Exar USB to Serial driver
 *
 * Copyright (c) 2020 Manivannan Sadhasivam <mani@kernel.org>
 *
 * Based on the initial driver written by Patong Yang:
 *
 *   https://lore.kernel.org/r/20180404070634.nhspvmxcjwfgjkcv@advantechmxl-desktop
 *
 *   Copyright (c) 2018 Patong Yang <patong.mxl@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>
#include <linux/usb/serial.h>

struct xr_txrx_clk_mask {
	u16 tx;
	u16 rx0;
	u16 rx1;
};

#define XR_INT_OSC_HZ			48000000U
#define MIN_SPEED			46U
#define MAX_SPEED			XR_INT_OSC_HZ

#define CLOCK_DIVISOR_0			0x04
#define CLOCK_DIVISOR_1			0x05
#define CLOCK_DIVISOR_2			0x06
#define TX_CLOCK_MASK_0			0x07
#define TX_CLOCK_MASK_1			0x08
#define RX_CLOCK_MASK_0			0x09
#define RX_CLOCK_MASK_1			0x0a

/* Register blocks */
#define UART_REG_BLOCK			0
#define UM_REG_BLOCK			4
#define UART_CUSTOM_BLOCK		0x66

/* UART Manager Registers */
#define UM_FIFO_ENABLE_REG		0x10
#define UM_ENABLE_TX_FIFO		0x01
#define UM_ENABLE_RX_FIFO		0x02

#define UM_RX_FIFO_RESET		0x18
#define UM_TX_FIFO_RESET		0x1c

#define UART_ENABLE_TX			0x1
#define UART_ENABLE_RX			0x2

#define UART_MODE_RI			BIT(0)
#define UART_MODE_CD			BIT(1)
#define UART_MODE_DSR			BIT(2)
#define UART_MODE_DTR			BIT(3)
#define UART_MODE_CTS			BIT(4)
#define UART_MODE_RTS			BIT(5)

#define UART_BREAK_ON			0xff
#define UART_BREAK_OFF			0

#define UART_DATA_MASK			GENMASK(3, 0)
#define UART_DATA_7			0x7
#define UART_DATA_8			0x8

#define UART_PARITY_MASK		GENMASK(6, 4)
#define UART_PARITY_SHIFT		4
#define UART_PARITY_NONE		(0x0 << UART_PARITY_SHIFT)
#define UART_PARITY_ODD			(0x1 << UART_PARITY_SHIFT)
#define UART_PARITY_EVEN		(0x2 << UART_PARITY_SHIFT)
#define UART_PARITY_MARK		(0x3 << UART_PARITY_SHIFT)
#define UART_PARITY_SPACE		(0x4 << UART_PARITY_SHIFT)

#define UART_STOP_MASK			BIT(7)
#define UART_STOP_SHIFT			7
#define UART_STOP_1			(0x0 << UART_STOP_SHIFT)
#define UART_STOP_2			(0x1 << UART_STOP_SHIFT)

#define UART_FLOW_MODE_NONE		0x0
#define UART_FLOW_MODE_HW		0x1
#define UART_FLOW_MODE_SW		0x2

#define UART_MODE_GPIO_MASK		GENMASK(2, 0)
#define UART_MODE_RTS_CTS		0x1
#define UART_MODE_DTR_DSR		0x2
#define UART_MODE_RS485			0x3
#define UART_MODE_RS485_ADDR		0x4

/* Used on devices that need the CDC control interface */
#define URM_RESET_RX_FIFO_BASE		0x18
#define URM_RESET_TX_FIFO_BASE		0x1c

#define CDC_DATA_INTERFACE_TYPE		0x0a

#define VIA_CDC_REGISTER		-1

enum xr_model {
	XR2280X,
	XR21B1411,
	XR21V141X,
	XR21B142X,
	MAX_XR_MODELS
};

enum xr_hal_type {
	REG_ENABLE,
	REG_FORMAT,
	REG_FLOW_CTRL,
	REG_XON_CHAR,
	REG_XOFF_CHAR,
	REG_TX_BREAK,
	REG_RS485_DELAY,
	REG_GPIO_MODE,
	REG_GPIO_DIR,
	REG_GPIO_SET,
	REG_GPIO_CLR,
	REG_GPIO_STATUS,
	REG_GPIO_INT_MASK,
	REG_CUSTOMIZED_INT,
	REG_GPIO_PULL_UP_ENABLE,
	REG_GPIO_PULL_DOWN_ENABLE,
	REG_LOOPBACK,
	REG_LOW_LATENCY,
	REG_CUSTOM_DRIVER,

	REQ_SET,
	REQ_GET,

	MAX_XR_HAL_TYPE
};

static const int xr_hal_table[MAX_XR_MODELS][MAX_XR_HAL_TYPE] = {
	[XR2280X] = {
		[REG_ENABLE] =				0x40,
		[REG_FORMAT] =				0x45,
		[REG_FLOW_CTRL] =			0x46,
		[REG_XON_CHAR] =			0x47,
		[REG_XOFF_CHAR] =			0x48,
		[REG_TX_BREAK] =			0x4a,
		[REG_RS485_DELAY] =			0x4b,
		[REG_GPIO_MODE] =			0x4c,
		[REG_GPIO_DIR] =			0x4d,
		[REG_GPIO_SET] =			0x4e,
		[REG_GPIO_CLR] =			0x4f,
		[REG_GPIO_STATUS] =			0x50,
		[REG_GPIO_INT_MASK] =			0x51,
		[REG_CUSTOMIZED_INT] =			0x52,
		[REG_GPIO_PULL_UP_ENABLE] =		0x54,
		[REG_GPIO_PULL_DOWN_ENABLE] =		0x55,
		[REG_LOOPBACK] =			0x56,
		[REG_LOW_LATENCY] =			0x66,
		[REG_CUSTOM_DRIVER] =			0x81,

		[REQ_SET] =				5,
		[REQ_GET] =				5,
	},
	[XR21B1411] = {
		[REG_ENABLE] =				0xc00,
		[REG_FORMAT] =				VIA_CDC_REGISTER,
		[REG_FLOW_CTRL] =			0xc06,
		[REG_XON_CHAR] =			0xc07,
		[REG_XOFF_CHAR] =			0xc08,
		[REG_TX_BREAK] =			0xc0a,
		[REG_RS485_DELAY] =			0xc0b,
		[REG_GPIO_MODE] =			0xc0c,
		[REG_GPIO_DIR] =			0xc0d,
		[REG_GPIO_SET] =			0xc0e,
		[REG_GPIO_CLR] =			0xc0f,
		[REG_GPIO_STATUS] =			0xc10,
		[REG_GPIO_INT_MASK] =			0xc11,
		[REG_CUSTOMIZED_INT] =			0xc12,
		[REG_GPIO_PULL_UP_ENABLE] =		0xc14,
		[REG_GPIO_PULL_DOWN_ENABLE] =		0xc15,
		[REG_LOOPBACK] =			0xc16,
		[REG_LOW_LATENCY] =			0xcc2,
		[REG_CUSTOM_DRIVER] =			0x20d,

		[REQ_SET] =				0,
		[REQ_GET] =				1,
	},
	[XR21V141X] = {
		[REG_ENABLE] =				0x03,
		[REG_FORMAT] =				0x0b,
		[REG_FLOW_CTRL] =			0x0c,
		[REG_XON_CHAR] =			0x10,
		[REG_XOFF_CHAR] =			0x11,
		[REG_LOOPBACK] =			0x12,
		[REG_TX_BREAK] =			0x14,
		[REG_RS485_DELAY] =			0x15,
		[REG_GPIO_MODE] =			0x1a,
		[REG_GPIO_DIR] =			0x1b,
		[REG_GPIO_INT_MASK] =			0x1c,
		[REG_GPIO_SET] =			0x1d,
		[REG_GPIO_CLR] =			0x1e,
		[REG_GPIO_STATUS] =			0x1f,

		[REQ_SET] =				0,
		[REQ_GET] =				1,
	},
	[XR21B142X] = {
		[REG_ENABLE] =				0x00,
		[REG_FORMAT] =				VIA_CDC_REGISTER,
		[REG_FLOW_CTRL] =			0x06,
		[REG_XON_CHAR] =			0x07,
		[REG_XOFF_CHAR] =			0x08,
		[REG_TX_BREAK] =			0x0a,
		[REG_RS485_DELAY] =			0x0b,
		[REG_GPIO_MODE] =			0x0c,
		[REG_GPIO_DIR] =			0x0d,
		[REG_GPIO_SET] =			0x0e,
		[REG_GPIO_CLR] =			0x0f,
		[REG_GPIO_STATUS] =			0x10,
		[REG_GPIO_INT_MASK] =			0x11,
		[REG_CUSTOMIZED_INT] =			0x12,
		[REG_GPIO_PULL_UP_ENABLE] =		0x14,
		[REG_GPIO_PULL_DOWN_ENABLE] =		0x15,
		[REG_LOOPBACK] =			0x16,
		[REG_LOW_LATENCY] =			0x46,
		[REG_CUSTOM_DRIVER] =			0x60,

		[REQ_SET] =				0,
		[REQ_GET] =				0,
	}
};

struct xr_port_private {
	enum xr_model model;
	unsigned int channel;
	struct usb_interface *control_if;
};

static int xr_set_reg(struct usb_serial_port *port, u8 block, u8 reg, u8 val)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	struct usb_serial *serial = port->serial;
	int ret;

	switch (port_priv->model) {
	case XR2280X:
	case XR21B1411:
		break;
	case XR21V141X:
		if (port_priv->channel)
			reg |= (port_priv->channel - 1) << 8;
		break;
	case XR21B142X:
		reg |= (port_priv->channel - 4) << 1;
		break;
	default:
		return -EINVAL;
	};
	ret = usb_control_msg(serial->dev,
			      usb_sndctrlpipe(serial->dev, 0),
			      xr_hal_table[port_priv->model][REQ_SET],
			      USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      val, reg | (block << 8), NULL, 0,
			      USB_CTRL_SET_TIMEOUT);
	if (ret < 0) {
		dev_err(&port->dev, "Failed to set reg 0x%02x: %d\n", reg, ret);
		return ret;
	}

	return 0;
}

static int xr_get_reg(struct usb_serial_port *port, u8 block, u8 reg, u8 *val)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	struct usb_serial *serial = port->serial;
	u8 *dmabuf;
	int ret;

	dmabuf = kmalloc(1, GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	switch (port_priv->model) {
	case XR2280X:
	case XR21B1411:
		break;
	case XR21V141X:
		if (port_priv->channel)
			reg |= (port_priv->channel - 1) << 8;
		break;
	case XR21B142X:
		reg |= (port_priv->channel - 4) << 1;
		break;
	default:
		return -EINVAL;
	};
	ret = usb_control_msg(serial->dev,
			      usb_rcvctrlpipe(serial->dev, 0),
			      xr_hal_table[port_priv->model][REQ_GET],
			      USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      0, reg | (block << 8), dmabuf, 1,
			      USB_CTRL_GET_TIMEOUT);
	if (ret == 1) {
		*val = *dmabuf;
		ret = 0;
	} else {
		dev_err(&port->dev, "Failed to get reg 0x%02x: %d\n", reg, ret);
		if (ret >= 0)
			ret = -EIO;
	}

	kfree(dmabuf);

	return ret;
}

static int xr_usb_serial_ctrl_msg(struct usb_serial_port *port,
				  int request, int val,
				  void *buf, int len)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	int if_num = port_priv->control_if->altsetting[0].desc.bInterfaceNumber;
	struct usb_serial *serial = port->serial;
	u8 *dmabuf = NULL;
	int ret;

	if (len) {
		dmabuf = kmemdup(buf, len, GFP_KERNEL);
		if (!dmabuf)
			return -ENOMEM;
	}

	ret = usb_control_msg(serial->dev,
			      usb_rcvctrlpipe(serial->dev, 0),
			      request,
			      USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			      val,
			      if_num, dmabuf, len,
			      USB_CTRL_GET_TIMEOUT);

	if (ret < 0) {
		dev_err(&port->dev, "Failed to send a control msg: %d\n", ret);
	} else {
		if (dmabuf)
			memcpy(buf, dmabuf, len);
		ret = 0;
	}

	kfree(dmabuf);

	return ret;
}

static int xr_set_reg_uart(struct usb_serial_port *port, u8 reg, u8 val)
{
	return xr_set_reg(port, UART_REG_BLOCK, reg, val);
}

static int xr_get_reg_uart(struct usb_serial_port *port, u8 reg, u8 *val)
{
	return xr_get_reg(port, UART_REG_BLOCK, reg, val);
}

static int xr_set_reg_um(struct usb_serial_port *port, u8 reg, u8 val)
{
	return xr_set_reg(port, UM_REG_BLOCK, reg, val);
}

/*
 * According to datasheet, below is the recommended sequence for enabling UART
 * module in XR21V141X:
 *
 * Enable Tx FIFO
 * Enable Tx and Rx
 * Enable Rx FIFO
 */
static int xr_uart_enable(struct usb_serial_port *port)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	int ret;

	if (port_priv->model != XR21V141X)
		return xr_set_reg_uart(port,
				       xr_hal_table[port_priv->model][REG_ENABLE],
				       UART_ENABLE_TX | UART_ENABLE_RX);

	ret = xr_set_reg_um(port, UM_FIFO_ENABLE_REG,
			    UM_ENABLE_TX_FIFO);
	if (ret)
		return ret;

	ret = xr_set_reg_uart(port, xr_hal_table[port_priv->model][REG_ENABLE],
			      UART_ENABLE_TX | UART_ENABLE_RX);
	if (ret)
		return ret;

	ret = xr_set_reg_um(port, UM_FIFO_ENABLE_REG,
			    UM_ENABLE_TX_FIFO | UM_ENABLE_RX_FIFO);

	if (ret)
		xr_set_reg_uart(port, xr_hal_table[port_priv->model][REG_ENABLE], 0);

	return ret;
}

static int xr_uart_disable(struct usb_serial_port *port)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	int ret;

	ret = xr_set_reg_uart(port,
			      xr_hal_table[port_priv->model][REG_ENABLE], 0);
	if (ret)
		return ret;

	if (port_priv->model != XR21V141X)
		return 0;

	ret = xr_set_reg_um(port, UM_FIFO_ENABLE_REG, 0);

	return ret;
}

static int fifo_reset(struct usb_serial_port *port)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	int channel = port_priv->channel;
	int ret = 0;

	if (port_priv->model != XR21V141X)
		return 0;

	if (channel)
		channel--;

	ret = xr_set_reg_um(port,
			    URM_RESET_RX_FIFO_BASE + channel, 0xff);
	if (ret)
		return ret;

	ret = xr_set_reg_um(port,
			    URM_RESET_TX_FIFO_BASE + channel, 0xff);

	return ret;
}

static int xr_tiocmget(struct tty_struct *tty)
{
	struct usb_serial_port *port = tty->driver_data;
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	u8 status;
	int ret;

	ret = xr_get_reg_uart(port,
			      xr_hal_table[port_priv->model][REG_GPIO_STATUS],
			      &status);
	if (ret)
		return ret;

	/*
	 * Modem control pins are active low, so reading '0' means it is active
	 * and '1' means not active.
	 */
	ret = ((status & UART_MODE_DTR) ? 0 : TIOCM_DTR) |
	      ((status & UART_MODE_RTS) ? 0 : TIOCM_RTS) |
	      ((status & UART_MODE_CTS) ? 0 : TIOCM_CTS) |
	      ((status & UART_MODE_DSR) ? 0 : TIOCM_DSR) |
	      ((status & UART_MODE_RI) ? 0 : TIOCM_RI) |
	      ((status & UART_MODE_CD) ? 0 : TIOCM_CD);

	return ret;
}

static int xr_tiocmset_port(struct usb_serial_port *port,
			    unsigned int set, unsigned int clear)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	u8 gpio_set = 0;
	u8 gpio_clr = 0;
	int ret = 0;

	/* Modem control pins are active low, so set & clr are swapped */
	if (set & TIOCM_RTS)
		gpio_clr |= UART_MODE_RTS;
	if (set & TIOCM_DTR)
		gpio_clr |= UART_MODE_DTR;
	if (clear & TIOCM_RTS)
		gpio_set |= UART_MODE_RTS;
	if (clear & TIOCM_DTR)
		gpio_set |= UART_MODE_DTR;

	/* Writing '0' to gpio_{set/clr} bits has no effect, so no need to do */
	if (gpio_clr)
		ret = xr_set_reg_uart(port,
				      xr_hal_table[port_priv->model][REG_GPIO_CLR],
				      gpio_clr);

	if (gpio_set)
		ret = xr_set_reg_uart(port,
				      xr_hal_table[port_priv->model][REG_GPIO_SET],
				      gpio_set);

	return ret;
}

static int xr_tiocmset(struct tty_struct *tty,
		       unsigned int set, unsigned int clear)
{
	struct usb_serial_port *port = tty->driver_data;

	return xr_tiocmset_port(port, set, clear);
}

static void xr_dtr_rts(struct usb_serial_port *port, int on)
{
	if (on)
		xr_tiocmset_port(port, TIOCM_DTR | TIOCM_RTS, 0);
	else
		xr_tiocmset_port(port, 0, TIOCM_DTR | TIOCM_RTS);
}

static void xr_break_ctl(struct tty_struct *tty, int break_state)
{
	struct usb_serial_port *port = tty->driver_data;
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	u8 state;

	if (port_priv->model != XR21V141X) {
		xr_usb_serial_ctrl_msg(port, USB_CDC_REQ_SEND_BREAK, state,
				       NULL, 0);
		return;
	}

	if (break_state == 0)
		state = UART_BREAK_OFF;
	else
		state = UART_BREAK_ON;

	dev_dbg(&port->dev, "Turning break %s\n",
		state == UART_BREAK_OFF ? "off" : "on");
	xr_set_reg_uart(port, xr_hal_table[port_priv->model][REG_TX_BREAK],
			state);
}

/* Tx and Rx clock mask values obtained from section 3.3.4 of datasheet */
static const struct xr_txrx_clk_mask xr21v141x_txrx_clk_masks[] = {
	{ 0x000, 0x000, 0x000 },
	{ 0x000, 0x000, 0x000 },
	{ 0x100, 0x000, 0x100 },
	{ 0x020, 0x400, 0x020 },
	{ 0x010, 0x100, 0x010 },
	{ 0x208, 0x040, 0x208 },
	{ 0x104, 0x820, 0x108 },
	{ 0x844, 0x210, 0x884 },
	{ 0x444, 0x110, 0x444 },
	{ 0x122, 0x888, 0x224 },
	{ 0x912, 0x448, 0x924 },
	{ 0x492, 0x248, 0x492 },
	{ 0x252, 0x928, 0x292 },
	{ 0x94a, 0x4a4, 0xa52 },
	{ 0x52a, 0xaa4, 0x54a },
	{ 0xaaa, 0x954, 0x4aa },
	{ 0xaaa, 0x554, 0xaaa },
	{ 0x555, 0xad4, 0x5aa },
	{ 0xb55, 0xab4, 0x55a },
	{ 0x6b5, 0x5ac, 0xb56 },
	{ 0x5b5, 0xd6c, 0x6d6 },
	{ 0xb6d, 0xb6a, 0xdb6 },
	{ 0x76d, 0x6da, 0xbb6 },
	{ 0xedd, 0xdda, 0x76e },
	{ 0xddd, 0xbba, 0xeee },
	{ 0x7bb, 0xf7a, 0xdde },
	{ 0xf7b, 0xef6, 0x7de },
	{ 0xdf7, 0xbf6, 0xf7e },
	{ 0x7f7, 0xfee, 0xefe },
	{ 0xfdf, 0xfbe, 0x7fe },
	{ 0xf7f, 0xefe, 0xffe },
	{ 0xfff, 0xffe, 0xffd },
};

static int xr_set_baudrate(struct tty_struct *tty,
			   struct usb_serial_port *port)
{
	u32 divisor, baud, idx;
	u16 tx_mask, rx_mask;
	int ret;

	baud = tty->termios.c_ospeed;
	if (!baud)
		return 0;

	baud = clamp(baud, MIN_SPEED, MAX_SPEED);
	divisor = XR_INT_OSC_HZ / baud;
	idx = ((32 * XR_INT_OSC_HZ) / baud) & 0x1f;
	tx_mask = xr21v141x_txrx_clk_masks[idx].tx;

	if (divisor & 0x01)
		rx_mask = xr21v141x_txrx_clk_masks[idx].rx1;
	else
		rx_mask = xr21v141x_txrx_clk_masks[idx].rx0;

	dev_dbg(&port->dev, "Setting baud rate: %u\n", baud);
	/*
	 * XR21V141X uses fractional baud rate generator with 48MHz internal
	 * oscillator and 19-bit programmable divisor. So theoretically it can
	 * generate most commonly used baud rates with high accuracy.
	 */
	ret = xr_set_reg_uart(port, CLOCK_DIVISOR_0,
			      divisor & 0xff);
	if (ret)
		return ret;

	ret = xr_set_reg_uart(port, CLOCK_DIVISOR_1,
			      (divisor >>  8) & 0xff);
	if (ret)
		return ret;

	ret = xr_set_reg_uart(port, CLOCK_DIVISOR_2,
			      (divisor >> 16) & 0xff);
	if (ret)
		return ret;

	ret = xr_set_reg_uart(port, TX_CLOCK_MASK_0,
			      tx_mask & 0xff);
	if (ret)
		return ret;

	ret = xr_set_reg_uart(port, TX_CLOCK_MASK_1,
			      (tx_mask >>  8) & 0xff);
	if (ret)
		return ret;

	ret = xr_set_reg_uart(port, RX_CLOCK_MASK_0,
			      rx_mask & 0xff);
	if (ret)
		return ret;

	ret = xr_set_reg_uart(port, RX_CLOCK_MASK_1,
			      (rx_mask >>  8) & 0xff);
	if (ret)
		return ret;

	tty_encode_baud_rate(tty, baud, baud);

	return 0;
}

static void xr_set_flow_mode(struct tty_struct *tty,
			     struct usb_serial_port *port,
			     struct ktermios *old_termios)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	u8 flow, gpio_mode;
	int ret;

	ret = xr_get_reg_uart(port, xr_hal_table[port_priv->model][REG_GPIO_MODE], &gpio_mode);
	if (ret)
		return;

	/* Set GPIO mode for controlling the pins manually by default. */
	gpio_mode &= ~UART_MODE_GPIO_MASK;

	if (C_CRTSCTS(tty) && C_BAUD(tty) != B0) {
		dev_dbg(&port->dev, "Enabling hardware flow ctrl\n");
		gpio_mode |= UART_MODE_RTS_CTS;
		flow = UART_FLOW_MODE_HW;
	} else if (I_IXON(tty)) {
		u8 start_char = START_CHAR(tty);
		u8 stop_char = STOP_CHAR(tty);

		dev_dbg(&port->dev, "Enabling sw flow ctrl\n");
		flow = UART_FLOW_MODE_SW;

		xr_set_reg_uart(port, xr_hal_table[port_priv->model][REG_XON_CHAR], start_char);
		xr_set_reg_uart(port, xr_hal_table[port_priv->model][REG_XOFF_CHAR], stop_char);
	} else {
		dev_dbg(&port->dev, "Disabling flow ctrl\n");
		flow = UART_FLOW_MODE_NONE;
	}

	/*
	 * Add support for the TXT and RXT function for 0x1420, 0x1422, 0x1424,
	 * by setting GPIO_MODE [9:8] = '11'
	 */
	if (port_priv->model == XR21B142X)
		gpio_mode |= 0x300;

	/*
	 * As per the datasheet, UART needs to be disabled while writing to
	 * FLOW_CONTROL register.
	 */
	xr_uart_disable(port);
	xr_set_reg_uart(port, xr_hal_table[port_priv->model][REG_FLOW_CTRL], flow);
	xr_uart_enable(port);

	xr_set_reg_uart(port, xr_hal_table[port_priv->model][REG_GPIO_MODE], gpio_mode);

	if (C_BAUD(tty) == B0)
		xr_dtr_rts(port, 0);
	else if (old_termios && (old_termios->c_cflag & CBAUD) == B0)
		xr_dtr_rts(port, 1);
}

static void xr_set_termios_cdc(struct tty_struct *tty,
			       struct usb_serial_port *port,
			       struct ktermios *old_termios)
{
	struct ktermios *termios = &tty->termios;
	struct usb_cdc_line_coding line = { 0 };
	int clear, set;

	line.dwDTERate = cpu_to_le32(tty_get_baud_rate(tty));
	line.bCharFormat = termios->c_cflag & CSTOPB ? 1 : 0;
	line.bParityType = termios->c_cflag & PARENB ?
			   (termios->c_cflag & PARODD ? 1 : 2) +
			   (termios->c_cflag & CMSPAR ? 2 : 0) : 0;

	switch (C_CSIZE(tty)) {
	case CS5:
		line.bDataBits = 5;
		break;
	case CS6:
		line.bDataBits = 6;
		break;
	case CS7:
		line.bDataBits = 7;
		break;
	case CS8:
	default:
		line.bDataBits = 8;
		break;
	}

	if (!line.dwDTERate) {
		line.dwDTERate = tty->termios.c_ospeed;
		clear = UART_MODE_DTR;
	} else {
		set = UART_MODE_DTR;
	}

	if (clear || set)
		xr_tiocmset_port(port, set, clear);

	xr_set_flow_mode(tty, port, old_termios);

	xr_usb_serial_ctrl_msg(port, USB_CDC_REQ_SET_LINE_CODING, 0,
			       &line, sizeof(line));
}

static void xr_set_termios_format_reg(struct tty_struct *tty,
				      struct usb_serial_port *port,
				      struct ktermios *old_termios)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	struct ktermios *termios = &tty->termios;
	u8 bits = 0;

	if (!old_termios || (tty->termios.c_ospeed != old_termios->c_ospeed))
		xr_set_baudrate(tty, port);

	/* For models with a private CHARACTER_FORMAT register */

	switch (C_CSIZE(tty)) {
	case CS5:
	case CS6:
		/* CS5 and CS6 are not supported, so just restore old setting */
		termios->c_cflag &= ~CSIZE;
		if (old_termios)
			termios->c_cflag |= old_termios->c_cflag & CSIZE;
		else
			bits |= UART_DATA_8;
		break;
	case CS7:
		bits |= UART_DATA_7;
		break;
	case CS8:
	default:
		bits |= UART_DATA_8;
		break;
	}

	if (C_PARENB(tty)) {
		if (C_CMSPAR(tty)) {
			if (C_PARODD(tty))
				bits |= UART_PARITY_MARK;
			else
				bits |= UART_PARITY_SPACE;
		} else {
			if (C_PARODD(tty))
				bits |= UART_PARITY_ODD;
			else
				bits |= UART_PARITY_EVEN;
		}
	}

	if (C_CSTOPB(tty))
		bits |= UART_STOP_2;
	else
		bits |= UART_STOP_1;

	xr_set_reg_uart(port,
			xr_hal_table[port_priv->model][REG_FORMAT],
			bits);

	xr_set_flow_mode(tty, port, old_termios);
}

static void xr_set_termios(struct tty_struct *tty,
			   struct usb_serial_port *port,
			   struct ktermios *old_termios)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);

	/*
	 * Different models have different ways to setup character format:
	 *
	 * - XR2280X and XR21V141X have their on private register. On
	 *   such models, 5-6 bits is not supported;
	 * - The other models use a standard CDC register.
	 *
	 * As we need to do different things with regards to 5-6 bits,
	 * the actual implementation is made on two different functions.
	 */
	if (xr_hal_table[port_priv->model][REG_FORMAT] == VIA_CDC_REGISTER)
		xr_set_termios_cdc(tty, port, old_termios);
	else
		xr_set_termios_format_reg(tty, port, old_termios);
}

static int xr_open(struct tty_struct *tty, struct usb_serial_port *port)
{
	struct xr_port_private *port_priv = usb_get_serial_data(port->serial);
	u8 gpio_dir;
	int ret;

	ret = xr_uart_enable(port);
	if (ret) {
		dev_err(&port->dev, "Failed to enable UART\n");
		return ret;
	}

	/*
	 * Configure DTR and RTS as outputs and RI, CD, DSR and CTS as
	 * inputs.
	 */
	gpio_dir = UART_MODE_DTR | UART_MODE_RTS;
	xr_set_reg_uart(port, xr_hal_table[port_priv->model][REG_GPIO_DIR], gpio_dir);

	ret = fifo_reset(port);
	if (ret) {
		dev_err(&port->dev, "Failed to reset FIFO\n");
		return ret;
	}


	/* Setup termios */
	if (tty)
		xr_set_termios(tty, port, NULL);

	ret = usb_serial_generic_open(tty, port);
	if (ret) {
		xr_uart_disable(port);
		return ret;
	}

	return 0;
}

static void xr_close(struct usb_serial_port *port)
{
	usb_serial_generic_close(port);

	xr_uart_disable(port);
}

static int xr_probe(struct usb_serial *serial, const struct usb_device_id *id)
{
	struct usb_driver *driver = serial->type->usb_driver;
	struct usb_interface *intf = serial->interface;
	struct usb_endpoint_descriptor *data_ep;
	struct usb_device *udev = serial->dev;
	struct xr_port_private *port_priv;
	struct usb_interface *ctrl_intf;
	int ifnum, ctrl_ifnum;

	/* Attach only data interfaces */
	ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	if (!(ifnum % 2))
		return -ENODEV;

	/* Control interfaces are the even numbers */
	ctrl_ifnum = ifnum - ifnum % 2;

	port_priv = kzalloc(sizeof(*port_priv), GFP_KERNEL);
	if (!port_priv)
		return -ENOMEM;

	data_ep = &intf->cur_altsetting->endpoint[0].desc;
	ctrl_intf = usb_ifnum_to_if(udev, ctrl_ifnum);

	port_priv->control_if = usb_get_intf(ctrl_intf);
	port_priv->model = id->driver_info;
	port_priv->channel = data_ep->bEndpointAddress;

	/* Wake up control interface */
	pm_suspend_ignore_children(&ctrl_intf->dev, false);
	if (driver->supports_autosuspend)
		pm_runtime_enable(&ctrl_intf->dev);
	else
	    pm_runtime_set_active(&ctrl_intf->dev);
	usb_set_serial_data(serial, port_priv);

	return 0;
}

static void xr_disconnect(struct usb_serial *serial)
{
	struct xr_port_private *port_priv = usb_get_serial_data(serial);
	struct usb_driver *driver = serial->type->usb_driver;
	struct usb_interface *ctrl_intf = port_priv->control_if;

	if (driver->supports_autosuspend)
		pm_runtime_disable(&ctrl_intf->dev);

	pm_runtime_set_suspended(&ctrl_intf->dev);

	usb_put_intf(ctrl_intf);

	kfree(port_priv);
	usb_set_serial_data(serial, 0);
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE(0x04e2, 0x1400), .driver_info = XR2280X},
	{ USB_DEVICE(0x04e2, 0x1401), .driver_info = XR2280X},
	{ USB_DEVICE(0x04e2, 0x1402), .driver_info = XR2280X},
	{ USB_DEVICE(0x04e2, 0x1403), .driver_info = XR2280X},

	{ USB_DEVICE(0x04e2, 0x1410), .driver_info = XR21V141X},
	{ USB_DEVICE(0x04e2, 0x1411), .driver_info = XR21B1411},
	{ USB_DEVICE(0x04e2, 0x1412), .driver_info = XR21V141X},
	{ USB_DEVICE(0x04e2, 0x1414), .driver_info = XR21V141X},

	{ USB_DEVICE(0x04e2, 0x1420), .driver_info = XR21B142X},
	{ USB_DEVICE(0x04e2, 0x1422), .driver_info = XR21B142X},
	{ USB_DEVICE(0x04e2, 0x1424), .driver_info = XR21B142X},

	{ }
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_serial_driver xr_device = {
	.driver = {
		.owner = THIS_MODULE,
		.name =	"xr_serial",
	},
	.id_table		= id_table,
	.num_ports		= 1,
	.probe			= xr_probe,
	.disconnect		= xr_disconnect,
	.open			= xr_open,
	.close			= xr_close,
	.break_ctl		= xr_break_ctl,
	.set_termios		= xr_set_termios,
	.tiocmget		= xr_tiocmget,
	.tiocmset		= xr_tiocmset,
	.dtr_rts		= xr_dtr_rts
};

static struct usb_serial_driver * const serial_drivers[] = {
	&xr_device, NULL
};

module_usb_serial_driver(serial_drivers, id_table);

MODULE_AUTHOR("Manivannan Sadhasivam <mani@kernel.org>");
MODULE_DESCRIPTION("MaxLinear/Exar USB to Serial driver");
MODULE_LICENSE("GPL");

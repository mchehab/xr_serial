Support for MaxLinear/Exar USB UARTs and bridges
------------------------------------------------

This directory contain a serial driver to be used for those
serial devices to work properly on Linux.

Despite being annouced with USB CDC support, those devices
require some special vendor-specific settings. So, the standard
acm-cdc driver won't work.

All MaxLinear/Exar devices listed at:

- https://www.maxlinear.com/product/interface/bridges/usb-ethernet-bridges/xr22800
- https://www.maxlinear.com/products/interface/uarts/usb-uarts

Should be supported, except for XR21B1421.

---

This repository contains the xr_serial driver that was recently
(Jan 21 2021) merged upstream (currently available at linux-next),
adding support for XR21V1410, plus a series of patches I wrote
(also submitted upstream) adding support for other the variants
of this chipset.

As this is part of the Linux Kernel, it is licensed under GPL
version 2 or later, stated by its SPDX header:

- https://spdx.org/licenses/GPL-2.0+.html

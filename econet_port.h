// SPDX-License-Identifier: GPL-2.0-only
#ifndef ECONET_PORT_H
#define ECONET_PORT_H

#include <linux/netdevice.h>

#include "econet_eth.h"
#include "gdm_regs.h"

struct net_device *en75_alloc_gdm_port(struct device *dev,
				       struct device_node *np,
				       struct gdm __iomem *regs,
				       struct en75_qdma *qdma,
				       enum etx_fport fport,
				       bool has_g2_stats);

#endif
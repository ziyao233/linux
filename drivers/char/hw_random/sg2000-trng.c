// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * TRNG driver for Sophgo SG2000 SoC.
 * Copyright (C) 2026 Yao Zi <me@ziyao.cc>
 *
 * This hardware RNG consists of two parts, a true random number generator, and
 * a deterministic random number generator. The DRNG's state is created with the
 * entropy collected by TRNG on initialization.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define TRNG_CTRL			0x00
#define  TRNG_CTRL_NOP			0x0
#define  TRNG_CTRL_GEN_NOISE		0x1
#define  TRNG_CTRL_CREATE_STATE		0x3
#define  TRNG_CTRL_GEN_RANDOM		0x6
#define TRNG_STAT			0x0c
#define  TRNG_STAT_BUSY			BIT(31)
#define TRNG_ISTAT			0x14
#define  TRNG_ISTAT_DONE		BIT(4)
#define TRNG_RAND0			0x24
#define TRNG_RAND1			0x28
#define TRNG_RAND2			0x2c
#define TRNG_RAND3			0x30

#define TRNG_POLL_PERIOD_US		5
#define TRNG_POLL_TIMEOUT_US		2000

struct sg2000_trng {
	struct hwrng rng;
	void __iomem *base;
};

#define to_trng(p) container_of(p, struct sg2000_trng, rng)

static void sg2000_trng_clear_istat(struct sg2000_trng *trng)
{
	u32 reg = readl(trng->base + TRNG_ISTAT);

	writel(reg, trng->base + TRNG_ISTAT);
}

static int sg2000_trng_wait_done(struct sg2000_trng *trng)
{
	int ret;
	u32 reg;

	ret = readl_poll_timeout(trng->base + TRNG_ISTAT, reg,
				 reg & TRNG_ISTAT_DONE, TRNG_POLL_PERIOD_US,
				 TRNG_POLL_TIMEOUT_US);
	if (!ret)
		writel(TRNG_ISTAT_DONE, trng->base + TRNG_ISTAT);

	return ret;
}

static int sg2000_trng_init(struct hwrng *hwrng)
{
	struct sg2000_trng *trng = to_trng(hwrng);
	u32 reg;
	int ret;

	ret = readl_poll_timeout(trng->base + TRNG_STAT, reg,
				 !(reg & TRNG_STAT_BUSY), TRNG_POLL_PERIOD_US,
				 TRNG_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	/*
	 * Reseeding the DRNG when its state isn't clean might hang the RNG,
	 * in which case TRNG_ISTAT_DONE will never be signaled, but some other
	 * undocumented flags in TRNG_ISTAT are set. Reading TRNG_ISTAT and
	 * writing back would bring the RNG back to normal.
	 */
	sg2000_trng_clear_istat(trng);
	writel(TRNG_CTRL_GEN_NOISE, trng->base + TRNG_CTRL);

	sg2000_trng_clear_istat(trng);
	writel(TRNG_CTRL_GEN_NOISE, trng->base + TRNG_CTRL);
	ret = sg2000_trng_wait_done(trng);
	if (ret)
		return ret;

	writel(TRNG_CTRL_CREATE_STATE, trng->base + TRNG_CTRL);
	return sg2000_trng_wait_done(trng);
}

static int sg2000_trng_read(struct hwrng *rng, void *data, size_t max,
			    bool wait)
{
	struct sg2000_trng *trng = to_trng(rng);
	u32 buf[4];
	int ret;

	sg2000_trng_clear_istat(trng);

	writel(TRNG_CTRL_GEN_RANDOM, trng->base + TRNG_CTRL);
	ret = sg2000_trng_wait_done(trng);
	if (ret)
		return ret;

	buf[0] = readl(trng->base + TRNG_RAND0);
	buf[1] = readl(trng->base + TRNG_RAND1);
	buf[2] = readl(trng->base + TRNG_RAND2);
	buf[4] = readl(trng->base + TRNG_RAND3);
	memcpy(data, buf, sizeof(buf));

	return 16;
}

static int sg2000_trng_probe(struct platform_device *pdev)
{
	struct sg2000_trng *trng;
	struct clk *clk;
	int ret;

	trng = devm_kzalloc(&pdev->dev, sizeof(*trng), GFP_KERNEL);
	if (!trng)
		return -ENOMEM;

	trng->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(trng->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(trng->base),
				     "failed to map io memory\n");

	clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk),
				     "failed to get and enable clock\n");

	trng->rng.name		= dev_driver_string(&pdev->dev);
	trng->rng.init		= sg2000_trng_init;
	trng->rng.read		= sg2000_trng_read;
	trng->rng.quality	= 1023;	// determined by FIPS-140-2 testsuite

	ret = devm_hwrng_register(&pdev->dev, &trng->rng);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register hwrng device\n");

	return 0;
}

static const struct of_device_id sg2000_trng_ids[] = {
	{ .compatible = "sophgo,sg2000-trng" },
	{ }
};
MODULE_DEVICE_TABLE(of, sg2000_trng_ids);

static struct platform_driver sg2000_trng_driver = {
	.probe	= sg2000_trng_probe,
	.driver	= {
		.name		= "sg2000-trng",
		.of_match_table	= sg2000_trng_ids,
	},
};
module_platform_driver(sg2000_trng_driver);
MODULE_DESCRIPTION("Sophgo SG2000 True Random Number Generator");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yao Zi <me@ziyao.cc>");

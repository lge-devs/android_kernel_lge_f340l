/*
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <mach/clk.h>

#define PWR_ON_MASK		BIT(31)
#define EN_REST_WAIT_MASK	(0xF << 20)
#define EN_FEW_WAIT_MASK	(0xF << 16)
#define CLK_DIS_WAIT_MASK	(0xF << 12)
#define SW_OVERRIDE_MASK	BIT(2)
#define HW_CONTROL_MASK		BIT(1)
#define SW_COLLAPSE_MASK	BIT(0)

#ifdef CONFIG_MACH_LGE
#include <mach/board_lge.h>

#define RESTORE_MASK		BIT(10)
#define SAVE_MASK			BIT(9)
#define RETAIN_MASK			BIT(8)
#define EN_RESET_MASK		BIT(7)
#define EN_FEW_MASK			BIT(6)
#define CLAMP_IO_MASK		BIT(5)
#define CLK_DISABLE_MASK	BIT(4)
#define PD_ARES_MASK		BIT(3)
#define MAX_RETRY_COUNT		10

#endif

/* Wait 2^n CXO cycles between all states. Here, n=2 (4 cycles). */
#define EN_REST_WAIT_VAL	(0x2 << 20)
#define EN_FEW_WAIT_VAL		(0x8 << 16)
#define CLK_DIS_WAIT_VAL	(0x2 << 12)

#ifdef CONFIG_MACH_LGE
#define TIMEOUT_US_LGE	20000
#define TIMEOUT_US		100
#else
#define TIMEOUT_US		100
#endif

struct gdsc {
	struct regulator_dev	*rdev;
	struct regulator_desc	rdesc;
	void __iomem		*gdscr;
	struct clk		**clocks;
	int			clock_count;
	bool			toggle_mem;
	bool			toggle_periph;
	bool			toggle_logic;
	bool			resets_asserted;
	bool			use_lge_workaround;
};

static int gdsc_is_enabled(struct regulator_dev *rdev)
{
	struct gdsc *sc = rdev_get_drvdata(rdev);

	if (!sc->toggle_logic)
		return !sc->resets_asserted;

	return !!(readl_relaxed(sc->gdscr) & PWR_ON_MASK);
}

#ifdef CONFIG_MACH_LGE
static int lge_gdsc_disable(struct gdsc *sc)
{
	uint32_t regval;
	int ret;

	regval = readl_relaxed(sc->gdscr);
	regval |= SW_OVERRIDE_MASK;
	writel_relaxed(regval, sc->gdscr);

	regval = readl_relaxed(sc->gdscr);
	regval |= CLK_DISABLE_MASK;
	writel_relaxed(regval, sc->gdscr);

	regval |= RETAIN_MASK;
	writel_relaxed(regval, sc->gdscr);
	regval &= ~RESTORE_MASK;
	regval |= CLAMP_IO_MASK | SAVE_MASK;
	writel_relaxed(regval, sc->gdscr);

	regval = readl_relaxed(sc->gdscr);
	regval |= SW_COLLAPSE_MASK;
	writel_relaxed(regval, sc->gdscr);

	ret = readl_tight_poll_timeout(sc->gdscr, regval,
				       !(regval & PWR_ON_MASK), TIMEOUT_US_LGE);

	if (ret)
		pr_err("%s: %s disable timed out\n", __func__, sc->rdesc.name);
	return ret;
}

static int lge_gdsc_enable(struct gdsc *sc)
{
	uint32_t regval;
	int ret;
	int retry_count = 0;

retry_enable:
	regval = readl_relaxed(sc->gdscr);
	regval |= SW_OVERRIDE_MASK;
	writel_relaxed(regval, sc->gdscr);

	regval = readl_relaxed(sc->gdscr);
	regval |= PD_ARES_MASK|EN_FEW_MASK;
	writel_relaxed(regval, sc->gdscr);
	regval = readl_relaxed(sc->gdscr);
	udelay(15);

	regval &= ~PD_ARES_MASK;
	writel_relaxed(regval, sc->gdscr);
	regval = readl_relaxed(sc->gdscr);

	regval |= EN_RESET_MASK;
	writel_relaxed(regval, sc->gdscr);
	regval = readl_relaxed(sc->gdscr);
	udelay(1);

	regval = readl_relaxed(sc->gdscr);
	regval &= ~SW_COLLAPSE_MASK;
	writel_relaxed(regval, sc->gdscr);

	ret = readl_tight_poll_timeout(sc->gdscr, regval, regval & PWR_ON_MASK,
				       TIMEOUT_US_LGE);
	if (ret) {
		pr_err("%s: %s enable timed out, state : 0x%08x, retry count : %d\n",
				__func__, sc->rdesc.name, readl_relaxed(sc->gdscr),
				retry_count+1);
		lge_gdsc_disable(sc);
		if (++retry_count <= MAX_RETRY_COUNT)
			goto retry_enable;
		pr_err("%s: %s fail to enable\n", __func__, sc->rdesc.name);
		BUG();
		return ret;
	}

	regval = readl_relaxed(sc->gdscr);
	regval &= ~CLAMP_IO_MASK;
	writel_relaxed(regval, sc->gdscr);
	regval = readl_relaxed(sc->gdscr);

	regval &= ~RETAIN_MASK;
	writel_relaxed(regval, sc->gdscr);
	regval = readl_relaxed(sc->gdscr);

	regval &= ~SAVE_MASK;
	regval |= RESTORE_MASK;
	writel_relaxed(regval, sc->gdscr);
	regval = readl_relaxed(sc->gdscr);

	regval &= ~(CLK_DISABLE_MASK);
	writel_relaxed(regval, sc->gdscr);
	regval = readl_relaxed(sc->gdscr);

	/*
	 * If clocks to this power domain were already on, they will take an
	 * additional 4 clock cycles to re-enable after the rail is enabled.
	 * Delay to account for this. A delay is also needed to ensure clocks
	 * are not enabled within 400ns of enabling power to the memories.
	 */
	udelay(1);

	return 0;
}
#endif

static int gdsc_enable(struct regulator_dev *rdev)
{
	struct gdsc *sc = rdev_get_drvdata(rdev);
	uint32_t regval;
	int i, ret;

	if (sc->toggle_logic) {
#ifdef CONFIG_MACH_LGE
		if (sc->use_lge_workaround) {
			ret = lge_gdsc_enable(sc);
			if (ret)
				return ret;
		} else
#endif
		{
			regval = readl_relaxed(sc->gdscr);
			regval &= ~SW_COLLAPSE_MASK;
			writel_relaxed(regval, sc->gdscr);

			ret = readl_tight_poll_timeout(sc->gdscr, regval,
						regval & PWR_ON_MASK, TIMEOUT_US);
			if (ret) {
				dev_err(&rdev->dev, "%s enable timed out\n",
					sc->rdesc.name);
				return ret;
			}
		}
	} else {
		for (i = 0; i < sc->clock_count; i++)
			clk_reset(sc->clocks[i], CLK_RESET_DEASSERT);
		sc->resets_asserted = false;
	}

	for (i = 0; i < sc->clock_count; i++) {
		if (sc->toggle_mem)
			clk_set_flags(sc->clocks[i], CLKFLAG_RETAIN_MEM);
		if (sc->toggle_periph)
			clk_set_flags(sc->clocks[i], CLKFLAG_RETAIN_PERIPH);
	}

	/*
	 * If clocks to this power domain were already on, they will take an
	 * additional 4 clock cycles to re-enable after the rail is enabled.
	 * Delay to account for this. A delay is also needed to ensure clocks
	 * are not enabled within 400ns of enabling power to the memories.
	 */
	udelay(1);

	return 0;
}

static int gdsc_disable(struct regulator_dev *rdev)
{
	struct gdsc *sc = rdev_get_drvdata(rdev);
	uint32_t regval;
	int i, ret = 0;

	for (i = sc->clock_count-1; i >= 0; i--) {
		if (sc->toggle_mem)
			clk_set_flags(sc->clocks[i], CLKFLAG_NORETAIN_MEM);
		if (sc->toggle_periph)
			clk_set_flags(sc->clocks[i], CLKFLAG_NORETAIN_PERIPH);
	}

	if (sc->toggle_logic) {
#ifdef CONFIG_MACH_LGE
		if (sc->use_lge_workaround)
			ret = lge_gdsc_disable(sc);
		else
#endif
		{
			regval = readl_relaxed(sc->gdscr);
			regval |= SW_COLLAPSE_MASK;
			writel_relaxed(regval, sc->gdscr);

			ret = readl_tight_poll_timeout(sc->gdscr, regval,
						       !(regval & PWR_ON_MASK),
							TIMEOUT_US);
			if (ret)
				dev_err(&rdev->dev, "%s disable timed out\n",
					sc->rdesc.name);
		}
	} else {
		for (i = sc->clock_count-1; i >= 0; i--)
			clk_reset(sc->clocks[i], CLK_RESET_ASSERT);
		sc->resets_asserted = true;
	}

	return ret;
}

static struct regulator_ops gdsc_ops = {
	.is_enabled = gdsc_is_enabled,
	.enable = gdsc_enable,
	.disable = gdsc_disable,
};

static int __devinit gdsc_probe(struct platform_device *pdev)
{
	static atomic_t gdsc_count __refdata = ATOMIC_INIT(-1);
	struct regulator_init_data *init_data;
	struct resource *res;
	struct gdsc *sc;
	uint32_t regval;
	bool retain_mem, retain_periph;
	int i, ret;
#ifdef CONFIG_MACH_LGE
	int use_lge_workaround = 0; /* default: all not applied */
#endif

	sc = devm_kzalloc(&pdev->dev, sizeof(struct gdsc), GFP_KERNEL);
	if (sc == NULL)
		return -ENOMEM;

	init_data = of_get_regulator_init_data(&pdev->dev, pdev->dev.of_node);
	if (init_data == NULL)
		return -ENOMEM;

	if (of_get_property(pdev->dev.of_node, "parent-supply", NULL))
		init_data->supply_regulator = "parent";

	ret = of_property_read_string(pdev->dev.of_node, "regulator-name",
				      &sc->rdesc.name);
	if (ret)
		return ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL)
		return -EINVAL;
	sc->gdscr = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (sc->gdscr == NULL)
		return -ENOMEM;

	sc->clock_count = of_property_count_strings(pdev->dev.of_node,
					    "qcom,clock-names");
	if (sc->clock_count == -EINVAL) {
		sc->clock_count = 0;
	} else if (IS_ERR_VALUE(sc->clock_count)) {
		dev_err(&pdev->dev, "Failed to get clock names\n");
		return -EINVAL;
	}

	sc->clocks = devm_kzalloc(&pdev->dev,
			sizeof(struct clk *) * sc->clock_count, GFP_KERNEL);
	if (!sc->clocks)
		return -ENOMEM;
	for (i = 0; i < sc->clock_count; i++) {
		const char *clock_name;
		of_property_read_string_index(pdev->dev.of_node,
					      "qcom,clock-names", i,
					      &clock_name);
		sc->clocks[i] = devm_clk_get(&pdev->dev, clock_name);
		if (IS_ERR(sc->clocks[i])) {
			int rc = PTR_ERR(sc->clocks[i]);
			if (rc != -EPROBE_DEFER)
				dev_err(&pdev->dev, "Failed to get %s\n",
					clock_name);
			return rc;
		}
	}
#ifdef CONFIG_MACH_LGE
	of_property_read_u32(pdev->dev.of_node, "lge,use_workaround",
			&use_lge_workaround);
	sc->use_lge_workaround =
		lge_get_board_revno() >= use_lge_workaround ? 0 : 1;
#endif
	sc->rdesc.id = atomic_inc_return(&gdsc_count);
	sc->rdesc.ops = &gdsc_ops;
	sc->rdesc.type = REGULATOR_VOLTAGE;
	sc->rdesc.owner = THIS_MODULE;
	platform_set_drvdata(pdev, sc);

	/*
	 * Disable HW trigger: collapse/restore occur based on registers writes.
	 * Disable SW override: Use hardware state-machine for sequencing.
	 */
	regval = readl_relaxed(sc->gdscr);
	regval &= ~(HW_CONTROL_MASK | SW_OVERRIDE_MASK);

	/* Configure wait time between states. */
	regval &= ~(EN_REST_WAIT_MASK | EN_FEW_WAIT_MASK | CLK_DIS_WAIT_MASK);
	regval |= EN_REST_WAIT_VAL | EN_FEW_WAIT_VAL | CLK_DIS_WAIT_VAL;
	writel_relaxed(regval, sc->gdscr);

	retain_mem = of_property_read_bool(pdev->dev.of_node,
					    "qcom,retain-mem");
	sc->toggle_mem = !retain_mem;
	retain_periph = of_property_read_bool(pdev->dev.of_node,
					    "qcom,retain-periph");
	sc->toggle_periph = !retain_periph;
	sc->toggle_logic = !of_property_read_bool(pdev->dev.of_node,
						"qcom,skip-logic-collapse");
	if (!sc->toggle_logic) {
#ifdef CONFIG_MACH_LGE
		/* LGE workaround is not used if a device is good pdn revision */
		if (lge_get_board_revno() >= use_lge_workaround) {
			regval &= ~SW_COLLAPSE_MASK;
			writel_relaxed(regval, sc->gdscr);

			ret = readl_tight_poll_timeout(sc->gdscr, regval,
					regval & PWR_ON_MASK, TIMEOUT_US);
			if (ret) {
				dev_err(&pdev->dev, "%s enable timed out\n",
						sc->rdesc.name);
				return ret;
			}
		} else {
			pr_info("%s: %s is enabled only at first by lge workaround\n",
					__func__, sc->rdesc.name);
			ret = lge_gdsc_enable(sc);
			if (ret) {
				dev_err(&pdev->dev, "%s enable timed out\n",
						sc->rdesc.name);
				return ret;
			}
		}
#else /* qmc */
		regval &= ~SW_COLLAPSE_MASK;
		writel_relaxed(regval, sc->gdscr);

		ret = readl_tight_poll_timeout(sc->gdscr, regval,
					regval & PWR_ON_MASK, TIMEOUT_US);
		if (ret) {
			dev_err(&pdev->dev, "%s enable timed out\n",
				sc->rdesc.name);
			return ret;
		}
#endif
	}

	for (i = 0; i < sc->clock_count; i++) {
		if (retain_mem || (regval & PWR_ON_MASK))
			clk_set_flags(sc->clocks[i], CLKFLAG_RETAIN_MEM);
		else
			clk_set_flags(sc->clocks[i], CLKFLAG_NORETAIN_MEM);

		if (retain_periph || (regval & PWR_ON_MASK))
			clk_set_flags(sc->clocks[i], CLKFLAG_RETAIN_PERIPH);
		else
			clk_set_flags(sc->clocks[i], CLKFLAG_NORETAIN_PERIPH);
	}

	sc->rdev = regulator_register(&sc->rdesc, &pdev->dev, init_data, sc,
				      pdev->dev.of_node);
	if (IS_ERR(sc->rdev)) {
		dev_err(&pdev->dev, "regulator_register(\"%s\") failed.\n",
			sc->rdesc.name);
		return PTR_ERR(sc->rdev);
	}

	return 0;
}

static int __devexit gdsc_remove(struct platform_device *pdev)
{
	struct gdsc *sc = platform_get_drvdata(pdev);
	regulator_unregister(sc->rdev);
	return 0;
}

static struct of_device_id gdsc_match_table[] __initdata = {
	{ .compatible = "qcom,gdsc" },
	{}
};

static struct platform_driver gdsc_driver __refdata = {
	.probe		= gdsc_probe,
	.remove		= __devexit_p(gdsc_remove),
	.driver		= {
		.name		= "gdsc",
		.of_match_table = gdsc_match_table,
		.owner		= THIS_MODULE,
	},
};

static int __init gdsc_init(void)
{
	return platform_driver_register(&gdsc_driver);
}
subsys_initcall(gdsc_init);

static void __exit gdsc_exit(void)
{
	platform_driver_unregister(&gdsc_driver);
}
module_exit(gdsc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MSM8974 GDSC power rail regulator driver");

/*
 * Copyright (c) 2022, Commonwealth Scientific and Industrial Research
 * Organisation (CSIRO) ABN 41 687 119 230.
 * Copyright (c) 2023 T-Mobile USA, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT power_domain_regulator

#include <zephyr/kernel.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(power_domain_regulator, CONFIG_POWER_DOMAIN_LOG_LEVEL);

struct pd_reg_config {
	const struct device *regulator;
	uint32_t startup_delay_us;
	uint32_t off_on_delay_us;
};

struct pd_reg_data {
	k_timeout_t next_boot;
};

struct pd_visitor_context {
	const struct device *domain;
	enum pm_device_action action;
};

static int pd_on_domain_visitor(const struct device *dev, void *context)
{
	struct pd_visitor_context *visitor_context = context;

	/* Only run action if the device is on the specified domain */
	if (!dev->pm || (dev->pm->domain != visitor_context->domain)) {
		return 0;
	}

	(void)pm_device_action_run(dev, visitor_context->action);
	return 0;
}

static int pd_reg_pm_action(const struct device *dev,
			     enum pm_device_action action)
{
	const struct pd_reg_config *cfg = dev->config;
	struct pd_reg_data *data = dev->data;
	struct pd_visitor_context context = {.domain = dev};
	int64_t next_boot_ticks;
	int rc = 0;

	/* Validate that blocking API's can be used */
	if (!k_can_yield()) {
		LOG_ERR("Blocking actions cannot run in this context");
		return -ENOTSUP;
	}

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		/* Wait until we can boot again */
		k_sleep(data->next_boot);
		/* Switch power on */
		regulator_enable(cfg->regulator);
		LOG_INF("%s is now ACTIVE, devices on the rail are now powered", dev->name);
		/* Wait for domain to come up */
		k_sleep(K_USEC(cfg->startup_delay_us));
		/* Notify devices on the domain they are now powered */
		context.action = PM_DEVICE_ACTION_TURN_ON;
		(void)device_supported_foreach(dev, pd_on_domain_visitor, &context);
		break;
	case PM_DEVICE_ACTION_SUSPEND:
		/* Notify devices on the domain that power is going down */
		context.action = PM_DEVICE_ACTION_TURN_OFF;
		(void)device_supported_foreach(dev, pd_on_domain_visitor, &context);
		/* Switch power off */
		regulator_disable(cfg->regulator);
		LOG_INF("%s is now SUSPENDED, devices on the rail are now OFF", dev->name);
		/* Store next time we can boot */
		next_boot_ticks = k_uptime_ticks() + k_us_to_ticks_ceil32(cfg->off_on_delay_us);
		data->next_boot = K_TIMEOUT_ABS_TICKS(next_boot_ticks);
		break;
	default:
		rc = -ENOTSUP;
	}

	return rc;
}

static int pd_reg_init(const struct device *dev)
{
	const struct pd_reg_config *cfg = dev->config;
	struct pd_reg_data *data = dev->data;
	int rc = 0;

	if (!device_is_ready(cfg->regulator)) {
		LOG_ERR("Regulator %s is not ready", cfg->regulator->name);
		return -ENODEV;
	}
	/* We can't know how long the domain has been off for before boot */
	data->next_boot = K_TIMEOUT_ABS_US(cfg->off_on_delay_us);

	return rc;
}

#define POWER_DOMAIN_DEVICE(id)						   \
	static const struct pd_reg_config pd_reg_##id##_cfg = {	   \
		.regulator = DEVICE_DT_GET(DT_PHANDLE(DT_DRV_INST(id), regulator)),	   \
		.startup_delay_us = DT_INST_PROP(id, startup_delay_us),	   \
		.off_on_delay_us = DT_INST_PROP(id, off_on_delay_us),	   \
	};								   \
	static struct pd_reg_data pd_reg_##id##_data;			   \
	PM_DEVICE_DT_INST_DEFINE(id, pd_reg_pm_action);		   \
	DEVICE_DT_INST_DEFINE(id, pd_reg_init, PM_DEVICE_DT_INST_GET(id), \
			      &pd_reg_##id##_data, &pd_reg_##id##_cfg,   \
			      POST_KERNEL, 75,				   \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(POWER_DOMAIN_DEVICE)

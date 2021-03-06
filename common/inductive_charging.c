/* Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Inductive charging control */

#include "common.h"
#include "gpio.h"
#include "hooks.h"
#include "lid_switch.h"

void inductive_charging_interrupt(enum gpio_signal signal)
{
	int charger_enabled = gpio_get_level(GPIO_BASE_CHG_VDD_EN);
	int charge_done = gpio_get_level(GPIO_CHARGE_DONE);

	if (!charger_enabled || charge_done)
		gpio_set_level(GPIO_CHARGE_EN, 0);
	else
		gpio_set_level(GPIO_CHARGE_EN, 1);
}

static void inductive_charging_lid_update(void)
{
	int lid_open = lid_is_open();
	gpio_set_level(GPIO_BASE_CHG_VDD_EN, !lid_open);
	inductive_charging_interrupt(GPIO_LID_OPEN);
}
DECLARE_HOOK(HOOK_LID_CHANGE, inductive_charging_lid_update, HOOK_PRIO_DEFAULT);

static void inductive_charging_init(void)
{
	gpio_enable_interrupt(GPIO_CHARGE_DONE);
	inductive_charging_lid_update();
}
DECLARE_HOOK(HOOK_INIT, inductive_charging_init, HOOK_PRIO_DEFAULT);

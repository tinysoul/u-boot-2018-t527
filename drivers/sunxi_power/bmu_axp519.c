/*
 * Copyright (C) 2019 Allwinner.
 * weidonghui <weidonghui@allwinnertech.com>
 *
 * SUNXI AXP519  Driver
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <sunxi_power/bmu_axp519.h>
#include <sunxi_power/bmu_ext.h>
#include <sunxi_power/axp.h>
#include <asm/arch/pmic_bus.h>
#include <asm/io.h>
#include <spare_head.h>
#include <asm/arch/gpio.h>
#include <asm/gpio.h>
#include <sunxi_power/power_manage.h>
#include <sys_config.h>
#include <fdt_support.h>

static bool axp519_exist;

int bmu_axp519_probe(void)
{
	u8 bmu_chip_id[2];

	axp519_exist = false;
	if (pmic_bus_init(AXP519_DEVICE_ADDR, AXP519_RUNTIME_ADDR)) {
		tick_printf("axp519 pmic_bus_init fail\n", __func__);
		return -1;
	}

	if (pmic_bus_init(AXP2601_DEVICE_ADDR, AXP2601_RUNTIME_ADDR)) {
		tick_printf("axp2601 pmic_bus_init fail\n", __func__);
		return -1;
	}

	if (pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_CHIP_ID, &bmu_chip_id[0])) {
		tick_printf("axp519 pmic_bus_read fail\n", __func__);
		return -1;
	}

	if (pmic_bus_read(AXP2601_RUNTIME_ADDR, AXP2601_CHIP_ID, &bmu_chip_id[1])) {
		tick_printf("axp2601 pmic_bus_read fail\n", __func__);
		return -1;
	}

	bmu_chip_id[0] &= 0x7;
	bmu_chip_id[1] &= 0x1F;

	if (bmu_chip_id[1] != AXP2601_CHIP_VER) {
		pmic_bus_clrbits(AXP2601_RUNTIME_ADDR, AXP2601_RESET_CFG, BIT(0));
		mdelay(500);
		if (pmic_bus_read(AXP2601_RUNTIME_ADDR, AXP2601_CHIP_ID, &bmu_chip_id[1])) {
			tick_printf("axp2601 pmic_bus_read fail\n", __func__);
			return -1;
		}
		bmu_chip_id[1] &= 0x1F;
	}


	if (bmu_chip_id[0] == AXP519_CHIP_VER) {
		if (bmu_chip_id[1] == AXP2601_CHIP_VER) {
			tick_printf("EXT: AXP519 & AXP2601\n");

			pmic_bus_write(AXP519_RUNTIME_ADDR, AXP519_VBUSOUT_VOL_SET_H, 0x19); //set boost to 5V
			pmic_bus_write(AXP519_RUNTIME_ADDR, AXP519_VBUSOUT_LIM_SET, 0); //set boost to 0.5A
			pmic_bus_write(AXP519_RUNTIME_ADDR, AXP519_VSYS_SET, 0xE);//set vsys to 5.8v
			axp519_exist = true;
			return 0;
		}
	}

	tick_printf("EXT: NO FOUND\n");

	return -1;
}

int bmu_axp519_set_charge(void)
{
	/* set charge enable */
	pmic_bus_setbits(AXP519_RUNTIME_ADDR, AXP519_WORK_CFG, BIT(4));

	return 0;
}

int bmu_axp519_set_discharge(void)
{
	/* set charge disable */
	pmic_bus_clrbits(AXP519_RUNTIME_ADDR, AXP519_WORK_CFG, BIT(4));

	return 0;
}

int bmu_axp519_get_vbus_status(void)
{
	u8 reg_value = 0;

	if (pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_SYS_STATUS, &reg_value))
		return -1;

	if ((reg_value & 0x01) == 0x01)
		return 1;

	return 0;
}

int bmu_axp519_get_poweron_source(int poweron_source)
{
	unsigned int reg_value = 0;

	switch (poweron_source) {
	case AXP_BOOT_SOURCE_IRQ_LOW:
		reg_value = readl(SUNXI_RTC_BASE+0x30);
		if (!reg_value) {
			if ((bmu_get_battery_probe() > -1) && bmu_ext_get_vbus_status()) {
				if (pmu_get_sys_mode() == SUNXI_CHARGING_FLAG) {
					pmu_set_sys_mode(0);
					poweron_source = AXP_BOOT_SOURCE_CHARGER;
				}
				if (bmu_get_power_on_flag() == 0) {
					poweron_source = AXP_BOOT_SOURCE_CHARGER;
				}
			}
		}
	default:
		bmu_set_power_on_flag(1);
		return poweron_source;
	}

	bmu_set_power_on_flag(1);
	return poweron_source;
}

int bmu_axp519_get_battery_probe(void)
{
	int battery_status;
	unsigned char reg_value[2];
	int temp, ret, tmp, bat_exist = 1;
	u8 data;
	static int check;

	if (check > 0) {
		battery_status = check - 2;
		return battery_status;
	}

	ret = script_parser_fetch(FDT_PATH_POWER_SPLY, "battery_exist", &bat_exist, 1);
	if (ret < 0)
		bat_exist = 1;

	if (!bat_exist) {
		check = 1;
		return -1;
	}

	pmic_bus_clrbits(AXP519_RUNTIME_ADDR, AXP519_WORK_CFG, BIT(4));
	pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_ADC_CFG, &data);
	data &= ~(0x0f);
	data |= 0x0a;
	pmic_bus_write(AXP519_RUNTIME_ADDR, AXP519_ADC_CFG, data);

	mdelay(1);
	if (pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_ADC_H, &reg_value[0])) {
		return -1;
	}
	if (pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_ADC_L, &reg_value[1])) {
		return -1;
	}

	temp = (reg_value[0] << 4) | (reg_value[1] & 0x0F);
	tmp = temp * 75 / 10;

	if (tmp < 6000) {
		pmic_bus_setbits(AXP519_RUNTIME_ADDR, AXP519_CHG_SET1, BIT(6));
		pmic_bus_setbits(AXP519_RUNTIME_ADDR, AXP519_WORK_CFG, BIT(4));
		mdelay(100);
		if (pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_ADC_H, &reg_value[0])) {
			return -1;
		}
		if (pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_ADC_L, &reg_value[1])) {
			return -1;
		}
		temp = (reg_value[0] << 4) | (reg_value[1] & 0x0F);
		tmp = temp * 75 / 10;

		if (tmp > 8000) {
			battery_status = -1;
		} else {
			battery_status = 1;
		}
	} else {
		battery_status = 1;
	}

	check = battery_status + 2;

	if (battery_status > 0)
		pmic_bus_clrbits(AXP519_RUNTIME_ADDR, AXP519_CHG_SET1, BIT(6));
	else
		pmic_bus_setbits(AXP519_RUNTIME_ADDR, AXP519_CHG_SET1, BIT(6));

	pmic_bus_setbits(AXP519_RUNTIME_ADDR, AXP519_WORK_CFG, BIT(4));
	return battery_status;
}

inline bool get_axp519(void)
{
	return axp519_exist;
}

int bmu_axp519_set_ntc_onff(int onoff, int ntc_cur)
{
	if (!onoff) {
		pmic_bus_setbits(AXP519_RUNTIME_ADDR, AXP519_DISCHG_SET1, BIT(2));
		pmic_bus_setbits(AXP519_RUNTIME_ADDR, AXP519_CHG_SET3, BIT(7));
	} else {
		pmic_bus_clrbits(AXP519_RUNTIME_ADDR, AXP519_DISCHG_SET1, BIT(2));
		pmic_bus_clrbits(AXP519_RUNTIME_ADDR, AXP519_CHG_SET3, BIT(7));
	}

	return 0;
}

/* todo:check */

static int axp_vts_to_temp(int data, int param[16])
{
	int temp;

	if (data < param[15])
		return 650;
	else if (data <= param[14]) {
		temp = 550 + (param[14]-data)*100/
		(param[14]-param[15]);
	} else if (data <= param[13]) {
		temp = 500 + (param[13]-data)*50/
		(param[13]-param[14]);
	} else if (data <= param[12]) {
		temp = 450 + (param[12]-data)*50/
		(param[12]-param[13]);
	} else if (data <= param[11]) {
		temp = 400 + (param[11]-data)*50/
		(param[11]-param[12]);
	} else if (data <= param[10]) {
		temp = 300 + (param[10]-data)*100/
		(param[10]-param[11]);
	} else if (data <= param[9]) {
		temp = 250 + (param[9]-data)*50/
		(param[9]-param[10]);
	} else if (data <= param[8]) {
		temp = 200 + (param[8]-data)*50/
		(param[8]-param[9]);
	} else if (data <= param[7]) {
		temp = 150 + (param[7]-data)*50/
		(param[7]-param[8]);
	} else if (data <= param[6]) {
		temp = 100 + (param[6]-data)*50/
		(param[6]-param[7]);
	} else if (data <= param[5]) {
		temp = 50 + (param[5]-data)*50/
		(param[5]-param[6]);
	} else if (data <= param[4]) {
		temp = 0 + (param[4]-data)*50/
		(param[4]-param[5]);
	} else if (data <= param[3]) {
		temp = -50 + (param[3]-data)*50/
		(param[3] - param[4]);
	} else if (data <= param[2]) {
		temp = -100 + (param[2]-data)*50/
		(param[2] - param[3]);
	} else if (data <= param[1]) {
		temp = -150 + (param[1]-data)*50/
		(param[1] - param[2]);
	} else if (data <= param[0]) {
		temp = -250 + (param[0]-data)*100/
		(param[0] - param[1]);
	} else
		temp = -250;
	return temp;
}

int bmu_axp519_get_ntc_temp(int param[16])
{
	unsigned char reg_value[2];
	int temp, tmp;
	u8 data;

	pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_ADC_CFG, &data);
	data &= ~(0x0f);
	data |= 0x09;
	pmic_bus_write(AXP519_RUNTIME_ADDR, AXP519_ADC_CFG, data);

	if (pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_ADC_H, &reg_value[0])) {
		return -1;
	}
	if (pmic_bus_read(AXP519_RUNTIME_ADDR, AXP519_ADC_L, &reg_value[1])) {
		return -1;
	}

	temp = (reg_value[0] << 4) | (reg_value[1] & 0x0F);
	tmp = temp * 110 / 4;
	temp = axp_vts_to_temp(tmp, (int *)param);

	return temp;
}

int bmu_axp2601_get_battery_vol(void)
{
	u8 reg_value_h = 0, reg_value_l = 0;
	int i, vtemp[3];

	for (i = 0; i < 3; i++) {
		if (pmic_bus_read(AXP2601_RUNTIME_ADDR, AXP2601_VBAT_H,
				  &reg_value_h)) {
			return -1;
		}
		if (pmic_bus_read(AXP2601_RUNTIME_ADDR, AXP2601_VBAT_L,
				  &reg_value_l)) {
			return -1;
		}
		/*step 1mv*/
		vtemp[i] = ((reg_value_h << 8) | reg_value_l);
	}
	if (vtemp[0] > vtemp[1]) {
		vtemp[0] = vtemp[0] ^ vtemp[1];
		vtemp[1] = vtemp[0] ^ vtemp[1];
		vtemp[0] = vtemp[0] ^ vtemp[1];
	}
	if (vtemp[1] > vtemp[2]) {
		vtemp[1] = vtemp[2] ^ vtemp[1];
		vtemp[2] = vtemp[2] ^ vtemp[1];
		vtemp[1] = vtemp[2] ^ vtemp[1];
	}
	if (vtemp[0] > vtemp[1]) {
		vtemp[0] = vtemp[0] ^ vtemp[1];
		vtemp[1] = vtemp[0] ^ vtemp[1];
		vtemp[0] = vtemp[0] ^ vtemp[1];
	}
	return vtemp[1];
}

int bmu_axp2601_battery_check(int ratio)
{
	int bat_vol, dcin_exist;
	u32 reg;

	bat_vol = bmu_get_battery_vol();
	dcin_exist = bmu_get_axp_bus_exist();

	if (!ratio) {
		if ((dcin_exist != AXP_VBUS_EXIST) && (bat_vol > 3700)) {
			pmic_bus_setbits(AXP2601_RUNTIME_ADDR, AXP2601_RESET_CFG, BIT(2));
			pmic_bus_clrbits(AXP2601_RUNTIME_ADDR, AXP2601_RESET_CFG, BIT(2));
			tick_printf("%s only battery reset gauge: soc = 0\n", __func__);
			return -1;
		}
		if (dcin_exist == AXP_VBUS_EXIST) {
			bmu_ext_set_discharge();
			mdelay(500);
			pmic_bus_setbits(AXP2601_RUNTIME_ADDR, AXP2601_RESET_CFG, BIT(2));
			pmic_bus_clrbits(AXP2601_RUNTIME_ADDR, AXP2601_RESET_CFG, BIT(2));
			mdelay(500);
			bmu_ext_set_charge();
			tick_printf("%s adapt reset gauge: soc = 0\n", __func__);
			reg = readl(SUNXI_RTC_BASE+0x108);
			reg &= ~(0xFF);
			reg |= 0x81;
			writel(reg, SUNXI_RTC_BASE+0x108);
			reg = readl(SUNXI_RTC_BASE+0x108);
			tick_printf("AXP2601_CURVE_CHECK:%x\n\n", reg);
			return -1;
		}
	}

	if (ratio > 60) {
		if (bat_vol < 3500) {
			bmu_ext_set_discharge();
			mdelay(500);
			pmic_bus_setbits(AXP2601_RUNTIME_ADDR, AXP2601_RESET_CFG, BIT(2));
			pmic_bus_clrbits(AXP2601_RUNTIME_ADDR, AXP2601_RESET_CFG, BIT(2));
			mdelay(500);
			bmu_ext_set_charge();
			tick_printf("%s adapt reset gauge: soc > 60% , bat_vol < 3500\n", __func__);
			return -1;
		}
	}

	tick_printf("battery_check pass:radio:%d, vol:%d\n", ratio, bat_vol);
	return 0;
}


int bmu_axp2601_get_battery_capacity(void)
{
	u8 reg_value;
	u32 reg;
	int check;
	static int check_count = 1;

	if (bmu_get_battery_probe() < 0)
		return 0;

	if (pmic_bus_read(AXP2601_RUNTIME_ADDR, AXP2601_GAUGE_SOC, &reg_value)) {
		return -1;
	}
	if (check_count == 1) {
		check = bmu_axp2601_battery_check(reg_value);
		if (check != 0) {
			if (pmic_bus_read(AXP2601_RUNTIME_ADDR, AXP2601_GAUGE_SOC,
					  &reg_value)) {
				return -1;
			}
			if (!reg_value) {
				reg = readl(SUNXI_RTC_BASE+0x108);
				reg &= ~(0xFF);
				writel(reg, SUNXI_RTC_BASE+0x108);
			}
		}
		check_count = 0;
	}
	return reg_value;
}

int bmu_axp2601_reset_capacity(void)
{
	if (pmic_bus_write(AXP2601_RUNTIME_ADDR, AXP2601_GAUGE_CONFIG, 0x00))
		return -1;

	return 1;
}

U_BOOT_BMU_EXT_INIT(bmu_axp519) = {
	.bmu_ext_name	 	 = "bmu_axp519",
	.get_exist	   		 = get_axp519,
	.probe			  	 = bmu_axp519_probe,
	.get_vbus_status     = bmu_axp519_get_vbus_status,
	.get_poweron_source     = bmu_axp519_get_poweron_source,
	.get_battery_probe     = bmu_axp519_get_battery_probe,
	.set_discharge  = bmu_axp519_set_discharge,
	.set_charge     = bmu_axp519_set_charge,

	.set_ntc_onoff     = bmu_axp519_set_ntc_onff,
	.get_ntc_temp      = bmu_axp519_get_ntc_temp,
	.reset_capacity    = bmu_axp2601_reset_capacity,
	.get_battery_vol	  = bmu_axp2601_get_battery_vol,
	.get_battery_capacity     = bmu_axp2601_get_battery_capacity,
};

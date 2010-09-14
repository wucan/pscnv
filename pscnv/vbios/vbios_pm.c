#include "drmP.h"
#define NV_DEBUG_NOTRACE
#include "vbios_pm.h"
#include "../nouveau_drv.h"
#include "../nouveau_hw.h"
#include "../nouveau_encoder.h"
#include "../nouveau_reg.h"
#include "../nouveau_bios.h"
#include "../nouveau_biosP.h"

static int
vbios_pmtable_parse_temperatures(struct drm_device *dev, struct nvbios *bios)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;

	uint16_t data_ptr = bios->pm.temperature_tbl_ptr;
	/*uint8_t version = bios->data[data_ptr+0];*/
	uint8_t header_length = bios->data[data_ptr+1];
	uint8_t entry_size = bios->data[data_ptr+2];
	uint8_t entry_count = bios->data[data_ptr+3];
	uint8_t i, e;

	if (entry_size != 3 ) {
		NV_ERROR(dev,
			"Unknow temperature table entry size(%i instead of 3)."
			" Please send your vbios to the nouveau devs.\n",
			entry_size);

		return -EINVAL;
	}

	/* Set the known default values to setup the temperature sensor */
	bios->pm.sensor_setup.temp_constant = 0;
	if (dev_p->card_type >= NV_40) {
		switch(dev_p->chipset) {
			case 0x43:
				bios->pm.sensor_setup.offset_mult = 32060;
				bios->pm.sensor_setup.offset_div = 1000;
				bios->pm.sensor_setup.slope_mult = 792;
				bios->pm.sensor_setup.slope_div = 1000;
				break;

			case 0x44:
			case 0x47:
				bios->pm.sensor_setup.offset_mult = 27839;
				bios->pm.sensor_setup.offset_div = 1000;
				bios->pm.sensor_setup.slope_mult = 780;
				bios->pm.sensor_setup.slope_div = 1000;
				break;

			case 0x46:
				bios->pm.sensor_setup.offset_mult = -24775;
				bios->pm.sensor_setup.offset_div = 100;
				bios->pm.sensor_setup.slope_mult = 467;
				bios->pm.sensor_setup.slope_div = 10000;
				break;

			case 0x49:
				bios->pm.sensor_setup.offset_mult = -25051;
				bios->pm.sensor_setup.offset_div = 100;
				bios->pm.sensor_setup.slope_mult = 458;
				bios->pm.sensor_setup.slope_div = 10000;
				break;

			case 0x4b:
				bios->pm.sensor_setup.offset_mult = -24088;
				bios->pm.sensor_setup.offset_div = 100;
				bios->pm.sensor_setup.slope_mult = 442;
				bios->pm.sensor_setup.slope_div = 10000;
				break;

			case 0x50:
				bios->pm.sensor_setup.offset_mult = -22749;
				bios->pm.sensor_setup.offset_div = 100;
				bios->pm.sensor_setup.slope_mult = 431;
				bios->pm.sensor_setup.slope_div = 10000;
				break;

			default:
				bios->pm.sensor_setup.offset_mult = 1;
				bios->pm.sensor_setup.offset_div = 1;
				bios->pm.sensor_setup.slope_mult = 1;
				bios->pm.sensor_setup.slope_div = 1;
		}
	}

	/* Set sane default values */
	bios->pm.temp_critical = 110;
	bios->pm.temp_throttling = 100;
	bios->pm.temp_fan_boost = 90;

	/* Read the entries from the table */
	for (i=0, e=0; i<entry_count; i++) {
		uint16_t value;

		/* set data_ptr to the entry start point */
		data_ptr = bios->pm.temperature_tbl_ptr +
					header_length + i*entry_size;

		value = ROM16(bios->data[data_ptr+1]);
		switch(bios->data[data_ptr+0])
		{
			case 0x01:
				value = (value&0x8f) == 0 ? (value >> 9) & 0x7f : 0;
				bios->pm.sensor_setup.temp_constant = value;
				break;

			case 0x04:
				bios->pm.temp_critical = (value&0x0ff0) >> 4;
				break;

			case 0x07:
				bios->pm.temp_throttling = (value&0x0ff0) >> 4;
				break;

			case 0x08:
				bios->pm.temp_fan_boost = (value&0x0ff0) >> 4;
				break;

			case 0x10:
				bios->pm.sensor_setup.offset_mult = value;
				break;

			case 0x11:
				bios->pm.sensor_setup.offset_div = value;
				break;

			case 0x12:
				bios->pm.sensor_setup.slope_mult = value;
				break;

			case 0x13:
				bios->pm.sensor_setup.slope_div = value;
				break;
		}
	}

	/* Check the values written in the table */
	if (bios->pm.temp_critical > 120)
		bios->pm.temp_critical = 120;
	if (bios->pm.temp_throttling > 110)
		bios->pm.temp_throttling = 110;
	if (bios->pm.temp_fan_boost > 100)
		bios->pm.temp_fan_boost = 100;

	return 0;
}

static int
vbios_pmtable_parse_voltages(struct drm_device *dev, struct nvbios *bios)
{
	uint16_t data_ptr = bios->pm.voltage_tbl_ptr;
	uint8_t version = bios->data[data_ptr+0];
	uint8_t header_length;
	uint8_t entry_size;
	uint8_t i;

	bios->pm.voltage_entry_count = 0;

	if (version == 0x10 || version == 0x12) {
		/* Geforce 5(FX)/6/7 */
		header_length = 5;
		entry_size = bios->data[data_ptr+1];
		bios->pm.voltage_entry_count = bios->data[data_ptr+2];
		bios->pm.voltage_mask = bios->data[data_ptr+4];
	} else if (version == 0x20 || version == 0x30) {
		/* Geforce 8/9/GT200 */
		header_length = bios->data[data_ptr+1];
		bios->pm.voltage_entry_count = bios->data[data_ptr+2];
		entry_size = bios->data[data_ptr+3];
		bios->pm.voltage_mask = bios->data[data_ptr+5];
	} else {
		NV_ERROR(dev, "PM: Unsupported voltage table 0x%x\n", version);
		return -EINVAL;
	}

	if (entry_size < 2) {
		NV_ERROR(dev, "PM: Voltage table entry size is too small."
					  "Please report\n");
		return -EINVAL;
	}

	/* Read the entries */
	if (bios->pm.voltage_entry_count > 0) {
		bios->pm.voltages = (struct pm_voltage_entry*)kzalloc(
			bios->pm.voltage_entry_count*sizeof(struct pm_voltage_entry),
													GFP_KERNEL);

		if (!bios->pm.voltages) {
			NV_ERROR(dev, "PM: Cannot allocate memory for voltage entries\n");
			return -EINVAL;
		}

		data_ptr = bios->pm.voltage_tbl_ptr + header_length;
		for (i=0; i<bios->pm.voltage_entry_count; i++) {
			bios->pm.voltages[i].voltage = bios->data[data_ptr+0];
			bios->pm.voltages[i].index = bios->data[data_ptr+1];

			/* In v30 (bios.major_version 0x70) the index, should be shifted
			* to indicate the value that is being used */
			if (version == 0x30)
					bios->pm.voltages[i].index >>= 2;
			data_ptr += entry_size;
		}
	}

	return 0;
}

static int
vbios_pmtable_parse_pm_modes(struct drm_device *dev, struct nvbios *bios)
{
	if (bios->major_version < 0x60) {
		/* Geforce 5 mode_info header table */
		int i,e;
		uint8_t table_version, header_length, mode_info_length;
		uint16_t data_ptr;

		table_version = bios->data[bios->pm.pm_modes_tbl_ptr+1];
		if (table_version != 0x12 &&
			table_version != 0x13 &&
			table_version != 0x15) {
			NV_ERROR(dev, "PM: Unsupported PM-mode table version 0x%x."
						  "Please report to nouveau devs.\n", table_version);
			return -EINVAL;
		}

		bios->pm.mode_info_count = bios->data[bios->pm.pm_modes_tbl_ptr+2];

		/* Calculate the data ptr */
		header_length = bios->data[bios->pm.pm_modes_tbl_ptr+0];
		mode_info_length = bios->data[bios->pm.pm_modes_tbl_ptr+3];

		/* Populate the modes */
		for (i=0, e=0; i < bios->pm.mode_info_count; i++) {
			uint8_t id;
			/* Calculate the offset of the current mode_info */
			data_ptr = bios->pm.pm_modes_tbl_ptr + mode_info_length*i +
				header_length;

			bios->pm.pm_modes[e].id_enabled = bios->data[data_ptr+0];
			bios->pm.pm_modes[e].coreclk = bios->data[data_ptr+1]*10;
			bios->pm.pm_modes[e].memclk = bios->data[data_ptr+3]*10;
			bios->pm.pm_modes[e].shaderclk = 0;
			bios->pm.pm_modes[e].fan_duty = bios->data[data_ptr+55];
			bios->pm.pm_modes[e].voltage = bios->data[data_ptr+56];

			/* Check the validity of the entry */
			id = bios->pm.pm_modes[e].id_enabled;
			if (id == 0x20 || id == 0x60 || id == 0x80)
				e++;
		}

		/* Update the real mode count (containing only the valid ones */
		bios->pm.mode_info_count = e;
	} else {
		/* Geforce 6+ mode_info header table */
		int i,e;
		uint8_t table_version, header_length, mode_info_length;
		uint8_t extra_data_count, extra_data_length;
		uint16_t data_ptr;

		table_version = bios->data[bios->pm.pm_modes_tbl_ptr+0];
		if (table_version < 0x21 || table_version > 0x35) {
			NV_ERROR(dev, "PM: Unsupported PM-mode table version 0x%x."
						  "Please report to nouveau devs.\n", table_version);
			return -EINVAL;
		}

		bios->pm.mode_info_count = bios->data[bios->pm.pm_modes_tbl_ptr+2];

		/* Calculate the data ptr */
		header_length = bios->data[bios->pm.pm_modes_tbl_ptr+1];
		mode_info_length = bios->data[bios->pm.pm_modes_tbl_ptr+3];
		extra_data_count = bios->data[bios->pm.pm_modes_tbl_ptr+4];
		extra_data_length = bios->data[bios->pm.pm_modes_tbl_ptr+5];

		/* Populate the modes */
		for (i=0, e=0; i < bios->pm.mode_info_count; i++) {
			/* Calculate the offset of the current mode_info */
			data_ptr = bios->pm.pm_modes_tbl_ptr +
				(mode_info_length+(extra_data_count*extra_data_length))*i +
				header_length;

			if (table_version < 0x25) {
				bios->pm.pm_modes[e].id_enabled = bios->data[data_ptr+0];
				bios->pm.pm_modes[e].fan_duty = bios->data[data_ptr+4];
				bios->pm.pm_modes[e].voltage = bios->data[data_ptr+5];
				bios->pm.pm_modes[e].coreclk = ROM16(bios->data[data_ptr+6])*1000;
				bios->pm.pm_modes[e].shaderclk = 0;
				bios->pm.pm_modes[e].memclk = ROM16(bios->data[data_ptr+11])*1000;
			} else if (table_version == 0x25) {
				bios->pm.pm_modes[e].id_enabled = bios->data[data_ptr+0];
				bios->pm.pm_modes[e].fan_duty = bios->data[data_ptr+4];
				bios->pm.pm_modes[e].voltage = bios->data[data_ptr+5];
				bios->pm.pm_modes[e].coreclk = ROM16(bios->data[data_ptr+6])*1000;
				bios->pm.pm_modes[e].shaderclk = ROM16(bios->data[data_ptr+10])*1000;
				bios->pm.pm_modes[e].memclk = ROM16(bios->data[data_ptr+12])*1000;
			} else if (table_version == 0x30 || table_version == 0x35) {
				bios->pm.pm_modes[e].id_enabled = bios->data[data_ptr+0];
				bios->pm.pm_modes[e].fan_duty = bios->data[data_ptr+6];
				bios->pm.pm_modes[e].voltage = bios->data[data_ptr+7];
				bios->pm.pm_modes[e].coreclk = ROM16(bios->data[data_ptr+8])*1000;
				bios->pm.pm_modes[e].shaderclk = ROM16(bios->data[data_ptr+10])*1000;
				bios->pm.pm_modes[e].memclk = ROM16(bios->data[data_ptr+12])*1000;
			}

			/* Check the validity of the entry */
			if (table_version == 0x35) {
				uint8_t id = bios->pm.pm_modes[e].id_enabled;
				if (id == 0x03 || id == 0x05 || id == 0x07 || id == 0x0f)
					e++;
			} else {
				uint8_t id = bios->pm.pm_modes[e].id_enabled;
				if (id >= 0x20 && id < 0x24)
					e++;
			}
		}

		/* Update the real mode count (containing only the valid ones */
		bios->pm.mode_info_count = e;
	}

	return 0;
}

int
vbios_parse_pmtable(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;
	struct nvbios *bios = &dev_p->vbios;
	
	/* parse the thermal table */
	if (bios->pm.temperature_tbl_ptr) {
		vbios_pmtable_parse_temperatures(dev, bios);
	} else {
		NV_ERROR(dev, "PM: This card doesn't have a temperature table."
					  "Please report to nouveau devs.\n");
	}

	/* parse the voltage table */
	if (bios->pm.voltage_tbl_ptr) {
		vbios_pmtable_parse_voltages(dev, bios);
	} else {
		NV_ERROR(dev, "PM: This card doesn't have a voltage table.\n"
					  "Please report to nouveau devs.\n");
	}

	/* Parse the pm modes table */
	if (bios->pm.pm_modes_tbl_ptr) {
		vbios_pmtable_parse_pm_modes(dev, bios);
	} else {
			NV_ERROR(dev, "PM: This card doesn't have a PM mode table\n"
						  "Please report to nouveau devs.\n");
	}

	return 0;
}

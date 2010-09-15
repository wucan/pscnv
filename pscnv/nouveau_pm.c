#include "drmP.h"
#include "nouveau_pm.h"

#include "nouveau_drv.h"
#include "nouveau_bios.h"

#ifdef CONFIG_ACPI
#include <linux/acpi.h>
#endif
#include <linux/power_supply.h>

static uint32_t
nouveau_get_pll_refclk(struct drm_device *dev, uint32_t reg)
{
	struct pll_lims pll;
	if (get_pll_limits(dev, reg, &pll))
		NV_ERROR(dev, "Failed to get pll limits\n");
	
	return pll.refclk;
}

static void
nouveau_parse_clock_regs(uint32_t reg0, uint32_t reg1,
					   uint32_t *m, uint32_t *n, uint32_t *p) {
	*p = (reg0 & 0x70000) >> 16;
	*m = reg1 & 0xff;
	*n = (reg1 & 0xff00) >> 8;
}

static uint32_t
nouveau_calculate_frequency(struct drm_device *dev,
						  uint32_t refclk, uint32_t reg0, uint32_t reg1)
{
	uint32_t p,m,n;
	nouveau_parse_clock_regs(reg0, reg1, &m, &n, &p);

	/*NV_INFO(dev, "nouveau_calculate_frequency: ref_clk=0x%x, reg0=0x%x, reg1=0x%x, p=0x%x, m=0x%x, n=0x%x\n",
			refclk, reg0, reg1, p, m, n);*/

	return ((n*refclk/m) >> p);
}

static uint32_t
nouveau_get_core_clocks(struct drm_device *dev)
{
	uint32_t reg0=nv_rd32(dev, 0x4028);
	uint32_t reg1=nv_rd32(dev, 0x402c);
	uint32_t refclk=nouveau_get_pll_refclk(dev, 0x4028);

	return nouveau_calculate_frequency(dev, refclk, reg0, reg1);
}

static uint32_t
nouveau_set_core_clocks(struct drm_device *dev, uint32_t clock_speed)
{
	return setPLL(dev->dev_private, 0x4028, clock_speed);
}

static uint32_t
nouveau_get_shader_clocks(struct drm_device *dev)
{
	uint32_t reg0=nv_rd32(dev, 0x4020);
	uint32_t reg1=nv_rd32(dev, 0x4024);
	uint32_t refclk=nouveau_get_pll_refclk(dev, 0x4020);

	return nouveau_calculate_frequency(dev, refclk, reg0, reg1);
}

static uint32_t
nouveau_set_shader_clocks(struct drm_device *dev, uint32_t clock_speed)
{
	return setPLL(dev->dev_private, 0x4020, clock_speed);
}

static uint32_t
nouveau_get_core_unknown_clocks(struct drm_device *dev)
{
	uint32_t reg0=nv_rd32(dev, 0x4030);
	uint32_t reg1=nv_rd32(dev, 0x4034);
	uint32_t refclk=nouveau_get_pll_refclk(dev, 0x4030);

	return nouveau_calculate_frequency(dev, refclk, reg0, reg1);
}

/*static uint32_t
nouveau_set_core_unknown_clocks(struct drm_device *dev, uint32_t clock_speed)
{
	return setPLL(dev->dev_private, 0x4030, clock_speed);
}*/

static uint32_t
nouveau_get_memory_clocks(struct drm_device *dev)
{
	uint32_t reg0=nv_rd32(dev, 0x4008);
	uint32_t reg1=nv_rd32(dev, 0x400c);
	uint32_t refclk=nouveau_get_pll_refclk(dev, 0x4008);

	return nouveau_calculate_frequency(dev, refclk, reg0, reg1);
}

static uint32_t
nouveau_set_memory_clocks(struct drm_device *dev, uint32_t clock_speed)
{
	return setPLL(dev->dev_private, 0x4008, clock_speed);
}

static uint32_t
nouveau_nv40_sensor_setup(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;
	struct pm_temp_sensor_setup *sensor_setup = &dev_p->vbios.pm.sensor_setup;
	uint32_t offset = sensor_setup->offset_mult / sensor_setup->offset_div;
	uint32_t sensor_calibration;
	
	/* set up the sensors */
	sensor_calibration = 120 - offset - sensor_setup->temp_constant;
	sensor_calibration = sensor_calibration * sensor_setup->slope_div /
							sensor_setup->slope_mult;
	if (dev_p->chipset >= 0x46) {
		sensor_calibration |= 0x80000000;
	} else {
		sensor_calibration |= 0x10000000;
	}
	nv_wr32(dev, 0x0015b0, sensor_calibration);
	
	/* Wait for the sensor to update */
	msleep(5);
	
	/* read */
	return nv_rd32(dev, 0x0015b4);
}

static uint32_t
nouveau_get_gpu_temperature(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;

	if (dev_p->chipset >= 0x84) {
		return nv_rd32(dev, 0x20400);
	} else if(dev_p->chipset >= 0x40) {
		struct pm_temp_sensor_setup *sensor_setup = &dev_p->vbios.pm.sensor_setup;
		uint32_t offset = sensor_setup->offset_mult / sensor_setup->offset_div;
		uint32_t temp;

		if(dev_p->chipset >= 0x50) {
			temp = nv_rd32(dev, 0x20008);
		} else {
			temp = nv_rd32(dev, 0x0015b4);
		}

		/* Setup the sensor if the temperature is 0 */
		if (temp == 0)
			temp = nouveau_nv40_sensor_setup(dev);

		temp = temp * sensor_setup->slope_mult / sensor_setup->slope_div;
		temp = temp + offset + sensor_setup->temp_constant;

		/* TODO: Check the returned value. Please report any issue.*/
		
		return temp; 
	} else {
		NV_ERROR(dev, "Temperature cannot be retrieved from an nv%x card\n", dev_p->chipset);
		return 0;
	}
}

/*
 * The voltage returned is in 10mV
 *
 * Due to masking the index (before writing it) it's possible that the funcion
 * does not return the correct voltage
 */
static uint32_t
nouveau_get_voltage(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;
	uint8_t voltage_entry_count = dev_p->vbios.pm.voltage_entry_count;
	uint32_t tmp_index, index, i, reg;

	if (dev_p->chipset < 0x50) {
		NV_INFO(dev, "PM: Voltage readings are not currently supported"
					 " on chipset nv%x\n",
				dev_p->chipset);
		return -EINVAL;
	}

	tmp_index = (nv_rd32(dev, 0xe104) & ~0x666fffff) >> 20;
	/* A lovely conversion of the voltage index
	 * Feel free to introduce a better solution
	 */
	switch (tmp_index) {
	case 0x000:
		index = 0;
		break;
	case 0x001:
		index = 1;
		break;
	case 0x010:
		index = 2;
		break;
	case 0x011:
		index = 3;
		break;
	case 0x100:
		index = 4;
		break;
	case 0x101:
		index = 5;
		break;
	case 0x110:
		index = 6;
		break;
	case 0x111:
		index = 7;
		break;
	default:
		index = 0xfe;
	}

	for (i = 0; i < voltage_entry_count; i++) {
		if (dev_p->vbios.pm.voltages[i].index == index)
			return dev_p->vbios.pm.voltages[i].voltage;
	}

	/* None found printf message and exit */
	reg = dev_p->chipset>=0x50?0xe104:0x60081c;
	NV_ERROR(dev, "PM: The current voltage's id used by the card is unknown."
				  "Please report reg 0x%x=0x%x to nouveau devs.\n",
				  reg, nv_rd32(dev, reg));
				
	return -EINVAL;
}

/*
 * The voltage should be in 10mV
 */
static uint32_t
nouveau_set_voltage(struct drm_device *dev, uint8_t voltage)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;
	uint8_t voltage_entry_count = dev_p->vbios.pm.voltage_entry_count;
	uint8_t voltage_mask = dev_p->vbios.pm.voltage_mask;
	uint32_t tmp_index, i;

	if (dev_p->chipset < 0x50) {
		NV_INFO(dev, "PM: Voltage writes are not currently supported"
					 " on chipset nv%x\n",
				dev_p->chipset);
		return -EINVAL;
	}

	if (!voltage) {
		NV_INFO(dev, "PM: voltage should not be zero - Aborting \n");
		return 0;
	}
	if (nouveau_get_voltage(dev) == voltage) {
		NV_INFO(dev, "PM: The same voltage has already been set\n");
		return 0;
	}

	for (i = 0; i < voltage_entry_count; i++) {
		if (dev_p->vbios.pm.voltages[i].voltage == voltage) {
			switch (dev_p->vbios.pm.voltages[i].index & voltage_mask) {
			case 0:
				tmp_index = 0x000;
				break;
			case 1:
				tmp_index = 0x001;
				break;
			case 2:
				tmp_index = 0x010;
				break;
			case 3:
				tmp_index = 0x011;
				break;
			case 4:
				tmp_index = 0x100;
				break;
/* The following are unconfirmed
 * XXX: Is there a VID over 8?
 */
			case 5:
				tmp_index = 0x101;
				break;
			case 6:
				tmp_index = 0x110;
				break;
			case 7:
				tmp_index = 0x111;
				break;
			default:
				NV_ERROR(dev, "PM: Voltage index %d does not appear to be valid."
							  "Please report to nouveau devs\n",
							  dev_p->vbios.pm.voltages[i].index);
				return -EINVAL;
			}
			nv_wr32(dev, 0xe104, (nv_rd32(dev, 0xe104)&0x666fffff) | (tmp_index<< 20));
			return 0;
		}
	}
	/* None found printf message and exit */
	NV_ERROR(dev, "The specified Voltage %dmV does not have a index\n",
			 voltage*10);
	return -EINVAL;
}

static int
nouveau_pm_update_state(struct drm_device *dev)
{
	/*TODO: Check the current temperature*/  

	/*TODO: Set the best PM mode*/
	
	return 0;
}

/******************************************
 *              Dynamic PM                *
 *****************************************/
#define ACPI_AC_CLASS           "ac_adapter"

#ifdef CONFIG_ACPI
static int nouveau_acpi_event(struct notifier_block *nb,
			     unsigned long val,
			     void *data)
{
	struct drm_nouveau_private *ddev = container_of(nb, struct drm_nouveau_private, pm.acpi_nb);
	struct drm_device *dev = ddev->dev;
	struct acpi_bus_event *entry = (struct acpi_bus_event *)data;

	if (strcmp(entry->device_class, ACPI_AC_CLASS) == 0) {
		if (power_supply_is_system_supplied() > 0)
			NV_INFO(dev, "PM: Power source is AC.\n");
		else
			NV_INFO(dev, "PM: Power source is DC\n");

		nouveau_pm_update_state(dev);
	}

	return NOTIFY_OK;
}
#endif

/******************************************
 *              Sysfs Fun                 *
 *****************************************/

static int
nouveau_is_the_current_pm_entry(struct drm_device *dev,
							  struct pm_mode_info* pm_mode)
{
	uint32_t cur_gpu_clock = nouveau_get_core_clocks(dev);

	uint32_t clock_diff = pm_mode->coreclk - cur_gpu_clock;
	clock_diff = clock_diff>0?clock_diff:-clock_diff;

	return clock_diff<pm_mode->coreclk/100;
}

static ssize_t
nouveau_pm_mode_to_string(struct drm_device *dev, unsigned id,
						char *buf, ssize_t len)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;
	struct pm_mode_info* pm_mode;

	if (id >= dev_p->vbios.pm.mode_info_count)
		return 0;

	pm_mode = &dev_p->vbios.pm.pm_modes[id];
	
	return snprintf(buf, len, "%s%u: core %u MHz/shader %u MHz/memory %u MHz/%u mV\n",
					nouveau_is_the_current_pm_entry(dev, pm_mode)?"*":" ",
					id, pm_mode->coreclk/1000, pm_mode->shaderclk/1000,
					pm_mode->memclk/1000, pm_mode->voltage*10);
}

static ssize_t
nouveau_voltage_to_string(struct drm_device *dev, unsigned id,
						char *buf, ssize_t len)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;
	struct pm_voltage_entry* v_entry;

	if (id >= dev_p->vbios.pm.voltage_entry_count)
		return 0;

	v_entry = &dev_p->vbios.pm.voltages[id];

	return snprintf(buf, len, "%s%u: %u mV\n",
					v_entry->voltage==nouveau_get_voltage(dev)?"*":" ",
					id,
					v_entry->voltage*10);
}

static ssize_t
nouveau_sysfs_get_pm_status(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;
	int ret_length=0, i=0;

	ret_length += snprintf(buf, PAGE_SIZE, "--- Clocks ---\n"
									"Core    : %u kHz\n"
									"Core-UNK: %u kHz\n"
									"Shader  : %u kHz\n"
									"Memory  : %u kHz\n"
									"\n"
									"--- Temperatures ---\n"
									"Core    : %u °C\n"
									"\n"
									"Fan boost temp     : %u °C\n"
									"GPU throttling temp: %u °C\n"
									"GPU critical temp  : %u °C\n"
									"\n"
									"--- Voltages ---\n",
									nouveau_get_core_clocks(ddev),
									nouveau_get_core_unknown_clocks(ddev),
									nouveau_get_shader_clocks(ddev),
									nouveau_get_memory_clocks(ddev),
									nouveau_get_gpu_temperature(ddev),
									dev_p->vbios.pm.temp_fan_boost,
									dev_p->vbios.pm.temp_throttling,
									dev_p->vbios.pm.temp_critical
					);

	for (i=0; i<dev_p->vbios.pm.voltage_entry_count; i++)
		ret_length += nouveau_voltage_to_string(ddev, i,
											 buf+ret_length, PAGE_SIZE-ret_length);

	ret_length += snprintf(buf+ret_length, PAGE_SIZE-ret_length,
						   "\n--- PM Modes ---\n");

	for (i=0; i<dev_p->vbios.pm.mode_info_count; i++)
		ret_length += nouveau_pm_mode_to_string(ddev, i,
											 buf+ret_length, PAGE_SIZE-ret_length);
	
	return ret_length;
}
static DEVICE_ATTR(pm_status, S_IRUGO, nouveau_sysfs_get_pm_status, NULL);


static ssize_t
nouveau_sysfs_get_pm_mode(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_priv = ddev->dev_private;
	unsigned pos=0, i=0;
	
	for (i=0; i<dev_priv->vbios.pm.mode_info_count; i++)
		pos += nouveau_pm_mode_to_string(ddev, i, buf+pos, PAGE_SIZE-pos);

	return pos;
}
static ssize_t
nouveau_sysfs_set_pm_mode(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t count)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;
	struct pm_mode_info* pm_mode;
	int profile = buf[0]-'0';

	if (profile < dev_p->vbios.pm.mode_info_count) {
		pm_mode = &dev_p->vbios.pm.pm_modes[profile];

		nouveau_set_core_clocks(ddev, pm_mode->coreclk);
		nouveau_set_shader_clocks(ddev, pm_mode->shaderclk);
		nouveau_set_memory_clocks(ddev, pm_mode->memclk);
		nouveau_set_voltage(ddev, pm_mode->voltage);

		/* TODO: Set the core unknown speed */

		/* TODO: Set the timings */

		/* TODO: Fan Setting */
	}
	
	return count;
}
static DEVICE_ATTR(pm_mode, S_IRUGO | S_IWUSR, nouveau_sysfs_get_pm_mode,
				   nouveau_sysfs_set_pm_mode);

static ssize_t
nouveau_sysfs_get_temperature(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));

	return snprintf(buf, PAGE_SIZE, "%u °C\n", nouveau_get_gpu_temperature(ddev));
}
static DEVICE_ATTR(temp_gpu, S_IRUGO, nouveau_sysfs_get_temperature, NULL);

static ssize_t
nouveau_sysfs_get_critical_temperature(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;

	return snprintf(buf, PAGE_SIZE, "%u °C\n", dev_p->vbios.pm.temp_critical);
}
static ssize_t
nouveau_sysfs_set_critical_temperature(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t count)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;
	unsigned long value;

	/* get the value */
	if (strict_strtoul(buf, 10, &value) == -EINVAL) {
		return count;
	}

	/* Do not let the user set stupid values */
	if (value < 90) {
		value = 90;
	} else if (value > 120) {
		value = 120;
	}

	dev_p->vbios.pm.temp_critical = value;

	return count;
}
static DEVICE_ATTR(temp_critical, S_IRUGO | S_IWUSR,
				   nouveau_sysfs_get_critical_temperature,
				   nouveau_sysfs_set_critical_temperature
  				);

static ssize_t
nouveau_sysfs_get_throttling_temperature(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;

	return snprintf(buf, PAGE_SIZE, "%u °C\n", dev_p->vbios.pm.temp_throttling);
}
static ssize_t
nouveau_sysfs_set_throttling_temperature(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t count)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;
	unsigned long value;

	/* get the value */
	if (strict_strtoul(buf, 10, &value) == -EINVAL) {
		return count;
	}

	/* Do not let the user set stupid values */
	if (value < 60) {
		value = 60;
	} else if (value > 115) {
		value = 115;
	}

	dev_p->vbios.pm.temp_throttling = value;

	return count;
}
static DEVICE_ATTR(temp_throttling, S_IRUGO | S_IWUSR,
				   nouveau_sysfs_get_throttling_temperature,
				   nouveau_sysfs_set_throttling_temperature
  				);

static ssize_t
nouveau_sysfs_get_fan_boost_temperature(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;

	return snprintf(buf, PAGE_SIZE, "%u °C\n", dev_p->vbios.pm.temp_fan_boost);
}
static ssize_t
nouveau_sysfs_set_fan_boost_temperature(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t count)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;
	unsigned long value;

	/* get the value */
	if (strict_strtoul(buf, 10, &value) == -EINVAL) {
		return count;
	}

	/* Do not let the user set stupid values */
	if (value < 30) {
		value = 30;
	} else if (value > 100) {
		value = 100;
	}

	dev_p->vbios.pm.temp_fan_boost = value;

	return count;
}
static DEVICE_ATTR(temp_fan_boost, S_IRUGO | S_IWUSR,
				   nouveau_sysfs_get_fan_boost_temperature,
				   nouveau_sysfs_set_fan_boost_temperature
  				);

static ssize_t
nouveau_sysfs_get_voltage(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_priv = ddev->dev_private;
	unsigned pos=0, i=0;

	for (i=0; i<dev_priv->vbios.pm.voltage_entry_count; i++)
		pos += nouveau_voltage_to_string(ddev, i, buf+pos, PAGE_SIZE-pos);

	return pos;
}
static ssize_t
nouveau_sysfs_set_voltage(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t count)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_p = ddev->dev_private;
	struct pm_voltage_entry* v_entry;
	int id = buf[0]-'0';

	if (id >= dev_p->vbios.pm.voltage_entry_count)
		return count;

	v_entry = &dev_p->vbios.pm.voltages[id];
	nouveau_set_voltage(ddev, v_entry->voltage);

	return count;
}
static DEVICE_ATTR(pm_voltage, S_IRUGO | S_IWUSR, nouveau_sysfs_get_voltage,
				   nouveau_sysfs_set_voltage);

/******************************************
 *            Main functions              *
 *****************************************/

int
nouveau_pm_init(struct drm_device* dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	int ret;

	/* Parse the vbios PM-related bits */
	vbios_parse_pmtable(dev);

	/* Set-up the sys entries */
	ret = device_create_file(dev->dev, &dev_attr_pm_status);
	if (ret)
		NV_ERROR(dev, "failed to create device file for pm_status\n");

	ret = device_create_file(dev->dev, &dev_attr_pm_mode);
	if (ret)
		NV_ERROR(dev, "failed to create device file for pm_mode\n");

	ret = device_create_file(dev->dev, &dev_attr_temp_gpu);
	if (ret)
		NV_ERROR(dev, "failed to create device file for temperature\n");

	ret = device_create_file(dev->dev, &dev_attr_temp_critical);
	if (ret)
		NV_ERROR(dev, "failed to create device file for critical_temp.\n");

	ret = device_create_file(dev->dev, &dev_attr_temp_throttling);
	if (ret)
		NV_ERROR(dev, "failed to create device file for throttling_temp.\n");

	ret = device_create_file(dev->dev, &dev_attr_temp_fan_boost);
	if (ret)
		NV_ERROR(dev, "failed to create device file for fan_boost_temp.\n");

	ret = device_create_file(dev->dev, &dev_attr_pm_voltage);
	if (ret)
		NV_ERROR(dev, "failed to create device file for fan_boost_temp.\n");
	
#ifdef CONFIG_ACPI
	dev_priv->pm.acpi_nb.notifier_call = nouveau_acpi_event;
	register_acpi_notifier(&dev_priv->pm.acpi_nb);
#endif

	nouveau_pm_update_state(dev);

	return 0;
}

int
nouveau_pm_fini(struct drm_device* dev)
{
	struct drm_nouveau_private *dev_priv = dev->dev_private;

	device_remove_file(dev->dev, &dev_attr_pm_status);
	device_remove_file(dev->dev, &dev_attr_pm_mode);
	device_remove_file(dev->dev, &dev_attr_temp_gpu);
	device_remove_file(dev->dev, &dev_attr_temp_critical);
	device_remove_file(dev->dev, &dev_attr_temp_throttling);
	device_remove_file(dev->dev, &dev_attr_temp_fan_boost);
	device_remove_file(dev->dev, &dev_attr_pm_voltage);
	
#ifdef CONFIG_ACPI
	unregister_acpi_notifier(&dev_priv->pm.acpi_nb);
#endif
	return 0;
}

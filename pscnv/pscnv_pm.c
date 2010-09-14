#include "drmP.h"
#include "pscnv_pm.h"

#include "nouveau_drv.h"
#include "nouveau_bios.h"

static uint32_t
pscnv_get_pll_refclk(struct drm_device *dev, uint32_t reg)
{
	struct pll_lims pll;
	if (get_pll_limits(dev, reg, &pll))
		NV_ERROR(dev, "Failed to get pll limits\n");
	
	return pll.refclk;
}

static void
pscnv_parse_clock_regs(uint32_t reg0, uint32_t reg1,
					   uint32_t *m, uint32_t *n, uint32_t *p) {
	*p = (reg0 & 0x70000) >> 16;
	*m = reg1 & 0xff;
	*n = (reg1 & 0xff00) >> 8;
}

static uint32_t
pscnv_calculate_frequency(struct drm_device *dev,
						  uint32_t refclk, uint32_t reg0, uint32_t reg1)
{
	uint32_t p,m,n;
	pscnv_parse_clock_regs(reg0, reg1, &m, &n, &p);

	/*NV_INFO(dev, "pscnv_calculate_frequency: ref_clk=0x%x, reg0=0x%x, reg1=0x%x, p=0x%x, m=0x%x, n=0x%x\n",
			refclk, reg0, reg1, p, m, n);*/

	return ((n*refclk/m) >> p);
}

static uint32_t
pscnv_pm_clock_to(struct drm_device *dev, uint32_t reg0_addr,
						uint32_t reg1_addr, uint32_t wanted_clock_speed) {
	uint32_t p,m,n, clock_diff;
	uint32_t reg0=nv_rd32(dev, reg0_addr);
	uint32_t reg1=nv_rd32(dev, reg1_addr);
	uint32_t refclk=pscnv_get_pll_refclk(dev, reg0_addr);

	pscnv_parse_clock_regs(reg0, reg1, &m, &n, &p);

	/* TODO: Find a better way to get closer to the needed clock */
	/* See http://cgit.freedesktop.org/nouveau/linux-2.6/tree/drivers/gpu/drm/nouveau/nouveau_calc.c */
	n = ((wanted_clock_speed)<< p) / (refclk/m);

	/* Calculate the new reg1 */
	reg1 &= 0xFFFF00FF;
	reg1 |= n<<8;

	/* Check the difference between the wanted clock and the obtained clock
	 * is not too big
	 */
	clock_diff = wanted_clock_speed-pscnv_calculate_frequency(dev, refclk, reg0, reg1);
	if (clock_diff <= wanted_clock_speed/10 || clock_diff >= wanted_clock_speed/10) {
		NV_ERROR(dev, "Will set the 0x%x clock to %u kHz. Clockdiff=%u kHz.\n",
				 reg0_addr, wanted_clock_speed, clock_diff);

		/* TODO: Un-comment this when the better pll calculation formula is done */
		/*return 1;*/
	}

	/* Write the new reg1 */
	nv_wr32(dev, reg1_addr, reg1);

	return 0;
}

static uint32_t
pscnv_get_core_clocks(struct drm_device *dev)
{
	uint32_t reg0=nv_rd32(dev, 0x4028);
	uint32_t reg1=nv_rd32(dev, 0x402c);
	uint32_t refclk=pscnv_get_pll_refclk(dev, 0x4028);

	return pscnv_calculate_frequency(dev, refclk, reg0, reg1);
}

static uint32_t
pscnv_set_core_clocks(struct drm_device *dev, uint32_t clock_speed)
{
	return pscnv_pm_clock_to(dev, 0x4028, 0x402c, clock_speed);
}

static uint32_t
pscnv_get_shader_clocks(struct drm_device *dev)
{
	uint32_t reg0=nv_rd32(dev, 0x4020);
	uint32_t reg1=nv_rd32(dev, 0x4024);
	uint32_t refclk=pscnv_get_pll_refclk(dev, 0x4020);

	return pscnv_calculate_frequency(dev, refclk, reg0, reg1);
}

static uint32_t
pscnv_set_shader_clocks(struct drm_device *dev, uint32_t clock_speed)
{
	return pscnv_pm_clock_to(dev, 0x4020, 0x4024, clock_speed);
}

static uint32_t
pscnv_get_core_unknown_clocks(struct drm_device *dev)
{
	uint32_t reg0=nv_rd32(dev, 0x4030);
	uint32_t reg1=nv_rd32(dev, 0x4034);
	uint32_t refclk=pscnv_get_pll_refclk(dev, 0x4030);

	return pscnv_calculate_frequency(dev, refclk, reg0, reg1);
}

static uint32_t
pscnv_set_core_unknown_clocks(struct drm_device *dev, uint32_t clock_speed)
{
	return pscnv_pm_clock_to(dev, 0x4030, 0x4034, clock_speed);
}

static uint32_t
pscnv_get_memory_clocks(struct drm_device *dev)
{
	uint32_t reg0=nv_rd32(dev, 0x4008);
	uint32_t reg1=nv_rd32(dev, 0x400c);
	uint32_t refclk=pscnv_get_pll_refclk(dev, 0x4008);

	return pscnv_calculate_frequency(dev, refclk, reg0, reg1);
}

static uint32_t
pscnv_set_memory_clocks(struct drm_device *dev, uint32_t clock_speed)
{
	return pscnv_pm_clock_to(dev, 0x4008, 0x400c, clock_speed);
}

static uint32_t
pscnv_nv40_sensor_setup(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_p = drm_dev->dev_private;
	struct pm_nv40_sensor_setup *nv40_setup = &dev_p->vbios.pm.nv40_setup;
	uint32_t offset = nv40_setup->offset_mult / nv40_setup->offset_div;
	uint32_t sensor_calibration;
	
	/* set up the sensors */
	sensor_calibration = 120 - offset - nv40_setup->temp_constant;
	sensor_calibration = sensor_calibration * nv40_setup->slope_div /
							nv40_setup->slope_mult;
	if (dev_p->chipset >= 0x46) {
		sensor_calibration |= 0x80000000;
	} else {
		sensor_calibration |= 0x10000000;
	}
	nv_wr32(dev, 0x0015b0);
	
	/* Wait for the sensor to update */
	msleep(5);
	
	/* read */
	return nv_rd32(dev, 0x0015b4);
}

static uint32_t
pscnv_get_gpu_temperature(struct drm_device *dev)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;
	if (dev_p->chipset >= 0x84) {
		return nv_rd32(dev, 0x20400);
	} else if(dev_p->chipset >= 0x50) {
		return (nv_rd32(dev, 0x20008)*430/10000)-227;
	} else if(dev_p->chipset >= 0x40) {
		struct pm_nv40_sensor_setup *nv40_setup = &dev_p->vbios.pm.nv40_setup;
		uint32_t offset = nv40_setup->offset_mult / nv40_setup->offset_div;
		uint32_t temp = nv_rd32(dev, 0x0015b4);

		/* Setup the sensor if the temperature is 0 */
		if (temp == 0)
			temp = pscnv_nv40_sensor_setup(dev);

		temp = temp * nv40_setup->slope_mult / nv40_setup->slope_div;
		temp = temp - offset - nv40_setup->temp_constant;

		/* TODO: Check the returned value. Please report any issue.*/
		
		return temp; 
	} else {
		NV_ERROR(dev, "Temperature cannot be retrieved from an nv%x card\n", dev_p->chipset);
		return 0;
	}
}

static ssize_t
pscnv_pm_mode_to_string(struct drm_device *dev, unsigned id,
						char *buf, ssize_t len)
{
	struct drm_nouveau_private *dev_p = dev->dev_private;
	struct pm_mode_info* pm_mode;

	if (id >= dev_p->vbios.pm.mode_info_count)
		return 0;

	pm_mode = &dev_p->vbios.pm.pm_modes[id];
	
	return snprintf(buf, len, "%s%u: core %u MHz/shader %u MHz/memory %u MHz/%u mV\n",
					pm_mode->coreclk==pscnv_get_core_clocks(dev)?"*":" ",
					id, pm_mode->coreclk/1000, pm_mode->shaderclk/1000,
					pm_mode->memclk/1000, pm_mode->voltage*10);
}

static ssize_t
pscnv_get_pm_status(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_priv = ddev->dev_private;
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
									"--- PM Modes ---\n",
									pscnv_get_core_clocks(ddev),
									pscnv_get_core_unknown_clocks(ddev),
									pscnv_get_shader_clocks(ddev),
									pscnv_get_memory_clocks(ddev),
									pscnv_get_gpu_temperature(ddev)
					);

	for (i=0; i<dev_priv->vbios.pm.mode_info_count; i++)
		ret_length += pscnv_pm_mode_to_string(ddev, i,
											 buf+ret_length, PAGE_SIZE-ret_length);
	
	return ret_length;
}
static DEVICE_ATTR(pm_status, S_IRUGO, pscnv_get_pm_status, NULL);


static ssize_t
pscnv_get_pm_mode(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct drm_device *ddev = pci_get_drvdata(to_pci_dev(dev));
	struct drm_nouveau_private *dev_priv = ddev->dev_private;
	unsigned pos=0, i=0;
	
	for (i=0; i<dev_priv->vbios.pm.mode_info_count; i++)
		pos += pscnv_pm_mode_to_string(ddev, i, buf+pos, PAGE_SIZE-pos);

	return pos;
}
static ssize_t
pscnv_set_pm_mode(struct device *dev,
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

		pscnv_set_core_clocks(ddev, pm_mode->coreclk);
		pscnv_set_shader_clocks(ddev, pm_mode->shaderclk);
		pscnv_set_memory_clocks(ddev, pm_mode->memclk);

		/* TODO: Voltage setting */
	}
	
	return count;
}
static DEVICE_ATTR(pm_mode, S_IRUGO | S_IWUSR, pscnv_get_pm_mode, pscnv_set_pm_mode);


int
pscnv_pm_init(struct drm_device* dev)
{
	/*struct drm_nouveau_private *dev_priv = dev->dev_private;*/
	int ret;

	/* Set-up the sys entries */
	ret = device_create_file(dev->dev, &dev_attr_pm_status);
	if (ret)
		NV_ERROR(dev, "failed to create device file for pm_status\n");

	ret = device_create_file(dev->dev, &dev_attr_pm_mode);
	if (ret)
		NV_ERROR(dev, "failed to create device file for pm_mode\n");

	return 0;
}

int
pscnv_pm_fini(struct drm_device* dev)
{
	/*struct drm_nouveau_private *dev_priv = dev->dev_private;*/

	device_remove_file(dev->dev, &dev_attr_pm_status);
	device_remove_file(dev->dev, &dev_attr_pm_mode);

	return 0;
}

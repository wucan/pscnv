#ifndef __PSCNV_PM_H__
#define __PSCNV_PM_H__

#include "drm.h"

int pscnv_pm_init(struct drm_device* dev);
int pscnv_pm_fini(struct drm_device* dev);

#endif
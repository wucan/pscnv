kapitest-y := fail.o \
	gamma_set_5.o gamma_set_6.o \
	drm_ioctl_def.o drm_ioctl_def_drv.o \
	drm_connector_detect_1.o drm_connector_detect_2.o

obj-m := kapitest.o

EXTRA_CFLAGS = -Iinclude/drm

SYSSRC = /lib/modules/$(shell uname -r)/build

all:
	+make -C $(SYSSRC) M=$(PWD) modules

clean:
	+make -C $(SYSSRC) M=$(PWD) clean

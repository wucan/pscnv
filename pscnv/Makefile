pscnv-y := nouveau_drv.o nouveau_state.o \
	     nouveau_irq.o nouveau_bios.o nouveau_hw.o nouveau_mem.o \
	     nouveau_i2c.o nouveau_calc.o nouveau_dp.o nouveau_connector.o \
	     nouveau_display.o nouveau_fbcon.o nouveau_dma.o \
	     nouveau_pm.o nouveau_volt.o nouveau_perf.o nouveau_temp.o \
	     nv04_tv.o nv04_dfp.o nv04_dac.o nv04_timer.o \
	     nv10_gpio.o \
	     nv50_gpio.o nv50_grctx.o \
	     nv50_display.o nv50_crtc.o nv50_cursor.o nv50_calc.o nv50_dac.o \
	     nv50_sor.o \
	     nv04_pm.o nv50_pm.o nva3_pm.o \
	     pscnv_mm.o pscnv_mem.o pscnv_vm.o pscnv_gem.o pscnv_ioctl.o \
	     pscnv_ramht.o pscnv_chan.o pscnv_sysram.o \
	     nv50_vram.o nv50_vm.o nv50_chan.o nv50_fifo.o nv50_graph.o \
	     nvc0_vram.o nvc0_vm.o nvc0_chan.o nvc0_fifo.o

pscnv-$(CONFIG_DRM_NOUVEAU_DEBUG) += nouveau_debugfs.o
pscnv-$(CONFIG_COMPAT) += nouveau_ioc32.o
pscnv-$(CONFIG_DRM_NOUVEAU_BACKLIGHT) += nouveau_backlight.o
pscnv-$(CONFIG_ACPI) += nouveau_acpi.o

obj-m := pscnv.o

EXTRA_CFLAGS = -Iinclude/drm

SYSSRC = /lib/modules/$(shell uname -r)/build

all: pscnv_kapi.h
	+make -C $(SYSSRC) M=$(PWD) modules

clean:
	+make -C $(SYSSRC) M=$(PWD) clean

pscnv_kapi.h: kapitest.sh
	./kapitest.sh $(SYSSRC) > pscnv_kapi.h

.PHONY: kapitest

kapitest:
	./kapitest.sh $(SYSSRC) > pscnv_kapi.h

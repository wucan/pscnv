#!/bin/sh
TESTS="gamma_set_5 gamma_set_6 drm_ioctl_def drm_ioctl_def_drv drm_connector_detect_1 drm_connector_detect_2"

make -k -C $1 M=$PWD/kapitest clean >&2
make -k -C $1 M=$PWD/kapitest modules >&2

for i in $TESTS
do
	if [ -f kapitest/$i.o ]
	then
		echo \#define PSCNV_KAPI_`echo $i | tr a-z A-Z`
	fi
done

# SPDX-License-Identifier: GPL-2.0
bcm2835-mmal-vchiq-objs := mmal-vchiq.o

obj-$(CONFIG_BCM2835_MMAL_VCHIQ) += bcm2835-mmal-vchiq.o

ccflags-y += \
	-I$(srctree)/drivers/staging/vc04_services \
	-D__VCCOREVER__=0x04000000

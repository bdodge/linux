ccflags-y += \
	-Idrivers/staging/vc04_services \
	-Idrivers/staging/vc04_services/interface/vchi \
	-Idrivers/staging/vc04_services/interface/vchiq_arm \
	-D__VCCOREVER__=0

vc-sm-cma-$(CONFIG_BCM_VC_SM_CMA) := vc_sm.o vc_vchi_sm.o

obj-$(CONFIG_BCM_VC_SM_CMA) := vc-sm-cma.o

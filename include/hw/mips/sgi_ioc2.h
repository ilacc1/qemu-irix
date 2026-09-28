#ifndef HW_MIPS_SGI_IOC2_H
#define HW_MIPS_SGI_IOC2_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_IOC2 "sgi-ioc2"

/* GPIO input lines (see sgi_ioc2.c for channel/mask routing). */
enum {
    SGI_IOC2_IN_SCSI0 = 0,
    SGI_IOC2_IN_SCSI1,
    SGI_IOC2_IN_ENET_EDLC,
    SGI_IOC2_IN_ENET_HPC,
    SGI_IOC2_IN_GIO0,
    SGI_IOC2_IN_GIO1,
    SGI_IOC2_IN_GIO2,
    SGI_IOC2_IN_DUART,
    SGI_IOC2_IN_KBD,
    SGI_IOC2_IN_HPC_DMA,
    SGI_IOC2_IN_MC_DMA,
    SGI_IOC2_IN_VIDEO,
};

/* GPIO outputs: CPU interrupts for local channels 0/1. */
enum {
    SGI_IOC2_OUT_CPU0 = 0,
    SGI_IOC2_OUT_CPU1,
};

#endif

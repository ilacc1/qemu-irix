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

/*
 * MAME never maps the IOC2 registers into a flat byte window: hpc3.cpp's
 * pio_data_r() is a 32-bit handler whose offs_t is a DWORD index, and it
 * reaches the device through AS_PIO6 as
 *     read_word(dword & 0xff, 0x00ff) & 0xff
 * so register N is the dword at channel offset N and only its low byte
 * carries data.  The PIO window dispatcher uses these two helpers to do
 * exactly one register access per dword.
 */
uint8_t sgi_ioc2_pio6_read_byte(DeviceState *ioc2, unsigned reg);
void sgi_ioc2_pio6_write_byte(DeviceState *ioc2, unsigned reg, uint8_t val);

#endif

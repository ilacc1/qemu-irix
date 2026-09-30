#ifndef HW_MIPS_SGI_HPC3_H
#define HW_MIPS_SGI_HPC3_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_HPC3 "sgi-hpc3"

/* Board trace gate owned by the IP22/IP24 board (SGI_TRACE=1). */
bool sgi_ip22_trace_enabled(void);

/* Hand the board's IOC2 device to the PIO window dispatcher (PIO ch6). */
void sgi_hpc3_set_ioc2(DeviceState *ioc2);

/* PIO ch6 registers 0x10/0x11: PS/2 keyboard controller (MAME's m_kbdc). */
void sgi_hpc3_set_pio6_kbd(DeviceState *kbd);

#endif

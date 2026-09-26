#ifndef HW_MIPS_SGI_HPC3_H
#define HW_MIPS_SGI_HPC3_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_HPC3 "sgi-hpc3"

/* Board trace gate owned by the IP22/IP24 board (SGI_TRACE=1). */
bool sgi_ip22_trace_enabled(void);

#endif

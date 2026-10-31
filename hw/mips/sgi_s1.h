#ifndef HW_MIPS_SGI_S1_H
#define HW_MIPS_SGI_S1_H

#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "system/memory.h"

#define TYPE_SGI_S1 "sgi-s1"
OBJECT_DECLARE_SIMPLE_TYPE(SGI_S1State, SGI_S1)

void sgi_s1_attach(MemoryRegion *sysmem, hwaddr base, BlockBackend *drive,
                   BlockBackend *cdrom, BlockBackend *cdrom2);

#endif

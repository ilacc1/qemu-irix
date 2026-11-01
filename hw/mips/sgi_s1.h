#ifndef HW_MIPS_SGI_S1_H
#define HW_MIPS_SGI_S1_H

#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "system/memory.h"

#define TYPE_SGI_S1 "sgi-s1"
OBJECT_DECLARE_SIMPLE_TYPE(SGI_S1State, SGI_S1)

SGI_S1State *sgi_s1_attach(MemoryRegion *sysmem, hwaddr base,
                           BlockBackend *drive, BlockBackend *cdrom,
                           BlockBackend *cdrom2);

/*
 * The S1 asserts this level whenever any per-channel interrupt bit is
 * present in STATUS_CMD.  The machine is responsible for routing it.
 */
qemu_irq sgi_s1_get_irq(SGI_S1State *s);

/*
 * SCSI DMA targets physical memory, which on IP19 overlaps the low CC,
 * IO4, GIO and PROM device windows.  Point S1 DMA at a dedicated RAM alias
 * so a descriptor that happens to name one of those physical pages reaches
 * the RAM underneath instead of the device model.
 */
void sgi_s1_set_dma_alias(SGI_S1State *s, hwaddr base);

#endif

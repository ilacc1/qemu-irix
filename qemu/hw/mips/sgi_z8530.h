#ifndef HW_MIPS_SGI_Z8530_H
#define HW_MIPS_SGI_Z8530_H

#include <stdint.h>

#include "chardev/char.h"
#include "hw/core/irq.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_SGI_Z8530 "sgi-z8530"
OBJECT_DECLARE_SIMPLE_TYPE(SgiZ8530State, SGI_Z8530)

#define SGI_Z8530_NUM_CHANNELS 2

#define SGI_Z8530_CHANNEL_A 0
#define SGI_Z8530_CHANNEL_B 1

#define SGI_Z8530_EPC_STRIDE 0x1000
#define SGI_Z8530_EPC_CHB_CTRL 0x07
#define SGI_Z8530_EPC_CHB_DATA 0x17
#define SGI_Z8530_EPC_CHA_CTRL 0x0f
#define SGI_Z8530_EPC_CHA_DATA 0x1f

SgiZ8530State *sgi_z8530_create(Chardev *chr_a, Chardev *chr_b);
void sgi_z8530_set_chardev(SgiZ8530State *s, unsigned channel,
                           Chardev *chr);
MemoryRegion *sgi_z8530_memory_region(SgiZ8530State *s);
qemu_irq sgi_z8530_irq(SgiZ8530State *s);
uint64_t sgi_z8530_read(SgiZ8530State *s, hwaddr offset);
void sgi_z8530_write(SgiZ8530State *s, hwaddr offset, uint64_t value);
void sgi_z8530_reset(SgiZ8530State *s);

#endif

#ifndef HW_MIPS_SGI_MC3_H
#define HW_MIPS_SGI_MC3_H

#include <stdbool.h>
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_SGI_MC3 "sgi-mc3"
OBJECT_DECLARE_SIMPLE_TYPE(SgiMC3State, SGI_MC3)

#define SGI_MC3_NUM_LEAVES 2
#define SGI_MC3_BANKS_PER_LEAF 4
#define SGI_MC3_NUM_BANKS (SGI_MC3_NUM_LEAVES * SGI_MC3_BANKS_PER_LEAF)
#define SGI_MC3_NOBANK 7

typedef enum SgiMC3Interleave {
    SGI_MC3_INTLV_ONEWAY = 0,
    SGI_MC3_INTLV_STABLE = 1,
    SGI_MC3_INTLV_OPTIMAL = 2,
} SgiMC3Interleave;

SgiMC3State *sgi_mc3_create(MemoryRegion *ram, uint64_t ram_size,
                            SgiMC3Interleave interleave);
void sgi_mc3_attach_ram(SgiMC3State *s, MemoryRegion *ram);
bool sgi_mc3_configure_ram(SgiMC3State *s, uint64_t ram_size,
                           SgiMC3Interleave interleave);
bool sgi_mc3_configure_bank(SgiMC3State *s, unsigned leaf, unsigned bank,
                            unsigned size_code, uint32_t base_reg,
                            unsigned if_code, unsigned ip);
MemoryRegion *sgi_mc3_config_region(SgiMC3State *s);
MemoryRegion *sgi_mc3_memory_region(SgiMC3State *s);
uint64_t sgi_mc3_config_read(SgiMC3State *s, unsigned reg);
void sgi_mc3_config_write(SgiMC3State *s, unsigned reg, uint64_t value);
void sgi_mc3_raise_mem_error(SgiMC3State *s, unsigned leaf, uint32_t status,
                             uint64_t addr, const uint16_t syndromes[4]);

#endif

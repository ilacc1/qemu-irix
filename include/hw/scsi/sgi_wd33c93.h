#ifndef HW_SCSI_SGI_WD33C93_H
#define HW_SCSI_SGI_WD33C93_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_WD33C93 "sgi-wd33c93"
OBJECT_DECLARE_SIMPLE_TYPE(SGIWD33C93State, SGI_WD33C93)

uint32_t sgi_wd33c93_dma_reg_read(SGIWD33C93State *state, hwaddr addr);
void sgi_wd33c93_dma_reg_write(SGIWD33C93State *state, hwaddr addr,
                               uint32_t value);
void sgi_wd33c93_poll(SGIWD33C93State *state);

#endif
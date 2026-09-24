#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/mips/sgi_hpc3_scsi.h"
#include "hw/scsi/sgi_wd33c93.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define SGI_HPC3_SCSI_CHANNEL_SIZE 0x2000
#define SGI_HPC3_SCSI_FIFO_SIZE 0x300

typedef struct SgiHpc3ScsiChannel SgiHpc3ScsiChannel;
typedef struct SgiHpc3ScsiState SgiHpc3ScsiState;

struct SgiHpc3ScsiChannel {
    MemoryRegion regs;
    SgiHpc3ScsiState *state;
    unsigned index;
};

struct SgiHpc3ScsiState {
    SysBusDevice parent_obj;
    SgiHpc3ScsiChannel channel[2];
    MemoryRegion fifo[2];
    SGIWD33C93State *wd[2];
    uint32_t dma_config[2];
    uint32_t pio_config[2];
    uint32_t fifo_words[2][SGI_HPC3_SCSI_FIFO_SIZE / sizeof(uint32_t)];
};

static uint64_t sgi_hpc3_scsi_channel_read(void *opaque, hwaddr addr,
                                            unsigned size)
{
    SgiHpc3ScsiChannel *channel = opaque;
    uint64_t rv;

    /*
     * Drive WD/ESP progress from HPC3 polling. The IRIX kernel SCSI
     * attach polls HPC3 DMA done (ENABLE clear on 0x1004, count on
     * 0x1000), not WD aux/status, while PROM polls WD. Previously
     * WD/ESP progress (TI|DMA kick, STATUS collect) only ran on WD
     * reads, so kernel-phase DMA never started, ENABLE never cleared,
     * and the driver timed out and retried forever (INQUIRY 64 / big
     * READ stalls). Real HW progresses via DRQ without CPU polls;
     * polling HPC3 to drive WD is the QEMU equivalent (HPC3 DRQ<->WD
     * link, MAME hpc3 scsi_drq/do_scsi_dma plus wd33c9x DRQ).
     */
    sgi_wd33c93_poll(channel->state->wd[channel->index]);
    switch (addr) {
    case 0x0000:
        rv = sgi_wd33c93_dma_reg_read(channel->state->wd[channel->index], 0x2000);
        break;
    case 0x0004:
        rv = sgi_wd33c93_dma_reg_read(channel->state->wd[channel->index], 0x2004);
        break;
    case 0x1000:
        rv = sgi_wd33c93_dma_reg_read(channel->state->wd[channel->index], 0x3000);
        break;
    case 0x1004:
        rv = sgi_wd33c93_dma_reg_read(channel->state->wd[channel->index], 0x3004);
        break;
    case 0x1008:
    case 0x100c:
        /* MAME hpc3.cpp:302-315: GIO/DEV FIFO pointers read as 0. */
        rv = 0;
        break;
    case 0x1010:
        rv = channel->state->dma_config[channel->index];
        break;
    case 0x1014:
        rv = channel->state->pio_config[channel->index];
        break;
    default:
        rv = 0;
        break;
    }
    return rv;
}

static void sgi_hpc3_scsi_channel_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size)
{
    SgiHpc3ScsiChannel *channel = opaque;

    switch (addr) {
    case 0x0000:
        sgi_wd33c93_dma_reg_write(channel->state->wd[channel->index], 0x2000,
                                   value);
        break;
    case 0x0004:
        sgi_wd33c93_dma_reg_write(channel->state->wd[channel->index], 0x2004,
                                   value);
        break;
    case 0x1000:
        sgi_wd33c93_dma_reg_write(channel->state->wd[channel->index], 0x3000,
                                   value);
        break;
    case 0x1004:
        sgi_wd33c93_dma_reg_write(channel->state->wd[channel->index], 0x3004,
                                   value);
        break;
    case 0x1010:
        channel->state->dma_config[channel->index] = value;
        break;
    case 0x1014:
        channel->state->pio_config[channel->index] = value;
        break;
    }
}

static const MemoryRegionOps sgi_hpc3_scsi_channel_ops = {
    .read = sgi_hpc3_scsi_channel_read,
    .write = sgi_hpc3_scsi_channel_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t sgi_hpc3_scsi_fifo_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    SgiHpc3ScsiChannel *channel = opaque;

    return channel->state->fifo_words[channel->index][addr >> 2];
}

static void sgi_hpc3_scsi_fifo_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    SgiHpc3ScsiChannel *channel = opaque;

    channel->state->fifo_words[channel->index][addr >> 2] = value;
}

static const MemoryRegionOps sgi_hpc3_scsi_fifo_ops = {
    .read = sgi_hpc3_scsi_fifo_read,
    .write = sgi_hpc3_scsi_fifo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void sgi_hpc3_scsi_reset(DeviceState *device)
{
    SgiHpc3ScsiState *state = SGI_HPC3_SCSI(device);

    memset(state->fifo_words, 0, sizeof(state->fifo_words));
    memset(state->dma_config, 0, sizeof(state->dma_config));
    memset(state->pio_config, 0, sizeof(state->pio_config));
}

static void sgi_hpc3_scsi_realize(DeviceState *device, Error **errp)
{
    SgiHpc3ScsiState *state = SGI_HPC3_SCSI(device);

    if (!state->wd[0] || !state->wd[1]) {
        error_setg(errp, "both SGI WD33C93 controller links are required");
    }
}

static const Property sgi_hpc3_scsi_properties[] = {
    DEFINE_PROP_LINK("wd0", SgiHpc3ScsiState, wd[0], TYPE_SGI_WD33C93,
                     SGIWD33C93State *),
    DEFINE_PROP_LINK("wd1", SgiHpc3ScsiState, wd[1], TYPE_SGI_WD33C93,
                     SGIWD33C93State *),
};

static const VMStateDescription vmstate_sgi_hpc3_scsi = {
    .name = TYPE_SGI_HPC3_SCSI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(dma_config, SgiHpc3ScsiState, 2),
        VMSTATE_UINT32_ARRAY(pio_config, SgiHpc3ScsiState, 2),
        VMSTATE_UINT32_2DARRAY(fifo_words, SgiHpc3ScsiState, 2,
                               SGI_HPC3_SCSI_FIFO_SIZE / sizeof(uint32_t)),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_hpc3_scsi_init(Object *object)
{
    SgiHpc3ScsiState *state = SGI_HPC3_SCSI(object);
    SysBusDevice *sysbus = SYS_BUS_DEVICE(object);

    for (unsigned index = 0; index < ARRAY_SIZE(state->channel); index++) {
        SgiHpc3ScsiChannel *channel = &state->channel[index];

        channel->state = state;
        channel->index = index;
        memory_region_init_io(&channel->regs, object,
                              &sgi_hpc3_scsi_channel_ops, channel,
                              index ? "sgi-hpc3-scsi1" : "sgi-hpc3-scsi0",
                              SGI_HPC3_SCSI_CHANNEL_SIZE);
        memory_region_init_io(&state->fifo[index], object,
                              &sgi_hpc3_scsi_fifo_ops, channel,
                              index ? "sgi-hpc3-scsi1-fifo" : "sgi-hpc3-scsi0-fifo",
                              SGI_HPC3_SCSI_FIFO_SIZE);
        sysbus_init_mmio(sysbus, &channel->regs);
        sysbus_init_mmio(sysbus, &state->fifo[index]);
    }
}

static void sgi_hpc3_scsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->realize = sgi_hpc3_scsi_realize;
    device_class->vmsd = &vmstate_sgi_hpc3_scsi;
    device_class_set_legacy_reset(device_class, sgi_hpc3_scsi_reset);
    device_class_set_props(device_class, sgi_hpc3_scsi_properties);
}

static const TypeInfo sgi_hpc3_scsi_type = {
    .name = TYPE_SGI_HPC3_SCSI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SgiHpc3ScsiState),
    .instance_init = sgi_hpc3_scsi_init,
    .class_init = sgi_hpc3_scsi_class_init,
};

static void sgi_hpc3_scsi_register_types(void)
{
    type_register_static(&sgi_hpc3_scsi_type);
}

type_init(sgi_hpc3_scsi_register_types)
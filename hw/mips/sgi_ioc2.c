/*
 * SGI IOC2 "Full House" interrupt controller (IP22 PIO6).
 *
 * Ports MAME's ioc2_device interrupt core: two local status/mask pairs
 * driving two CPU interrupt outputs (level semantics), plus a mappable
 * status/mask pair that folds DUART/keyboard lines into local bit 0x80
 * (channel 0) / 0x08 (channel 1). Also owns the PIO6 base control
 * registers (front panel, DMA select, reset, write, system ID) that the
 * PROM touches during boot.
 *
 * GPIO inputs (fixed channel/mask routing, matching board wiring):
 *   0 scsi0      -> local0 0x02
 *   1 scsi1      -> local0 0x04
 *   2 enet_edlc  -> local0 0x08
 *   3 enet_hpc   -> local0 0x08
 *   4 gio0       -> local0 0x01
 *   5 gio1       -> local0 0x40
 *   6 gio2       -> local1 0x80
 *   7 duart      -> mappable 0x20
 *   8 kbd        -> mappable 0x10
 * GPIO outputs 0/1 are the CPU interrupts for local channel 0/1.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/mips/sgi_ioc2.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define SGI_IOC2_MMIO_BASE 0x14
#define SGI_IOC2_MMIO_SIZE 0x0c
#define SGI_IOC2_INT2_SIZE 0x10

typedef struct SgiIoc2State SgiIoc2State;

struct SgiIoc2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion int2_iomem;
    qemu_irq out[2];
    uint8_t base_regs[0x20];
    uint8_t local_status[2];
    uint8_t local_mask[2];
    uint8_t map_status;
    uint8_t map_mask[2];
    uint8_t map_polarity;
    uint8_t error_status;
    uint8_t front_panel;
    uint8_t dma_select;
    uint8_t reset;
    uint8_t write;
    uint32_t pit_c2_count;
    uint64_t pit_c2_start_ns;
    bool pit_c2_running;
    uint16_t pit_c2_latch;
    bool pit_c2_latch_valid;
    int pit_c2_write_state;
    int pit_c2_read_state;
    uint8_t pit_c2_tmp_lsb;
    /* Counters 0/1 never count (clocks gated off) but retain the last write:
     * the IRIX presence test reads back what it wrote. Counter 2 below is
     * the live 1MHz calibration counter. */
    uint8_t pit_c0_last;
    uint8_t pit_c1_last;
    uint8_t pit_timer_clear_last;
};

#define SGI_IOC2(obj) OBJECT_CHECK(SgiIoc2State, (obj), TYPE_SGI_IOC2)

static void sgi_ioc2_update_irq(SgiIoc2State *state, unsigned channel)
{
    if (channel < ARRAY_SIZE(state->out)) {
        qemu_set_irq(state->out[channel],
                     (state->local_status[channel] &
                      state->local_mask[channel]) != 0);
    }
}

static void sgi_ioc2_raise_local(SgiIoc2State *state, unsigned channel,
                                 uint8_t mask)
{
    if (channel < ARRAY_SIZE(state->local_status)) {
        state->local_status[channel] |= mask;
        sgi_ioc2_update_irq(state, channel);
    }
}

static void sgi_ioc2_lower_local(SgiIoc2State *state, unsigned channel,
                                 uint8_t mask)
{
    if (channel < ARRAY_SIZE(state->local_status)) {
        state->local_status[channel] &= ~mask;
        sgi_ioc2_update_irq(state, channel);
    }
}

static uint8_t sgi_ioc2_map_status_read(SgiIoc2State *state)
{
    return state->map_status;
}

static void sgi_ioc2_map_mask_write(SgiIoc2State *state, unsigned channel,
                                    uint8_t value)
{
    if (channel < 2) {
        state->map_mask[channel] = value;
        if (state->map_status & value) {
            sgi_ioc2_raise_local(state, channel, channel ? 0x08 : 0x80);
        } else {
            sgi_ioc2_lower_local(state, channel, channel ? 0x08 : 0x80);
        }
    }
}

static void sgi_ioc2_gpio_in(void *opaque, int n, int level)
{
    SgiIoc2State *state = SGI_IOC2(opaque);

    switch (n) {
    case 0:
        if (level) {
            sgi_ioc2_raise_local(state, 0, 0x02);
        } else {
            sgi_ioc2_lower_local(state, 0, 0x02);
        }
        break;
    case 1:
        if (level) {
            sgi_ioc2_raise_local(state, 0, 0x04);
        } else {
            sgi_ioc2_lower_local(state, 0, 0x04);
        }
        break;
    case 2:
    case 3:
        if (level) {
            sgi_ioc2_raise_local(state, 0, 0x08);
        } else {
            sgi_ioc2_lower_local(state, 0, 0x08);
        }
        break;
    case 4:
        if (level) {
            sgi_ioc2_raise_local(state, 0, 0x01);
        } else {
            sgi_ioc2_lower_local(state, 0, 0x01);
        }
        break;
    case 5:
        if (level) {
            sgi_ioc2_raise_local(state, 0, 0x40);
        } else {
            sgi_ioc2_lower_local(state, 0, 0x40);
        }
        break;
    case 6:
        if (level) {
            sgi_ioc2_raise_local(state, 1, 0x80);
        } else {
            sgi_ioc2_lower_local(state, 1, 0x80);
        }
        break;
    case 7:
        state->map_status = level ? (state->map_status | 0x20) :
                                    (state->map_status & ~0x20);
        sgi_ioc2_map_mask_write(state, 0, state->map_mask[0]);
        sgi_ioc2_map_mask_write(state, 1, state->map_mask[1]);
        break;
    case 8:
        state->map_status = level ? (state->map_status | 0x10) :
                                    (state->map_status & ~0x10);
        sgi_ioc2_map_mask_write(state, 0, state->map_mask[0]);
        sgi_ioc2_map_mask_write(state, 1, state->map_mask[1]);
        break;
    default:
        break;
    }
}

static uint64_t sgi_ioc2_read_inner(void *opaque, hwaddr addr, unsigned size)
{
    SgiIoc2State *state = SGI_IOC2(opaque);

    switch (addr + SGI_IOC2_MMIO_BASE) {
    case 0x14:
        return state->front_panel;
    case 0x16:
        return 0x11;
    case 0x18:
        return state->base_regs[0x18];
    case 0x1a:
        return state->dma_select;
    case 0x1c:
        return state->reset;
    case 0x1e:
        return state->write;
    default:
        break;
    }
    return 0;
}

/*
 * PIO4 INT2 block (HPC3+0x58400): local status/mask pairs, mappable
 * status/mask, timer clear, and 1MHz PIT counters (0/1 gated off, 2 live).
 * MAME ioc2_full_house_device::int2_map.
 */
#define SGI_IOC2_PIT_CLOCK_HZ 1000000ULL

static uint16_t sgi_ioc2_pit_c2_remaining(SgiIoc2State *state)
{
    if (!state->pit_c2_running) {
        return 0;
    }
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed_ns = now_ns >= state->pit_c2_start_ns ?
                          now_ns - state->pit_c2_start_ns : 0;
    uint64_t ticks = elapsed_ns * SGI_IOC2_PIT_CLOCK_HZ / 1000000000ULL;
    if (ticks >= state->pit_c2_count) {
        return 0;
    }
    return (uint16_t)(state->pit_c2_count - ticks);
}

static uint64_t sgi_ioc2_int2_read(void *opaque, hwaddr addr, unsigned size)
{
    SgiIoc2State *state = SGI_IOC2(opaque);
    uint64_t rv = 0;

    switch (addr) {
    case 0x00:
        rv = state->local_status[0];
        break;
    case 0x02:
        rv = state->local_status[1];
        break;
    case 0x04:
        rv = sgi_ioc2_map_status_read(state);
        break;
    case 0x08:
        /* Write-only in MAME; retain last for the presence-test readback. */
        rv = state->pit_timer_clear_last;
        break;
    case 0x0c:
        rv = state->pit_c0_last;
        break;
    case 0x0d:
        rv = state->pit_c1_last;
        break;
    case 0x0e: {
        uint16_t cur;
        if (state->pit_c2_latch_valid) {
            cur = state->pit_c2_latch;
        } else {
            cur = sgi_ioc2_pit_c2_remaining(state);
        }
        /* 8254 CALL mode: LSB then MSB on successive reads. */
        if (state->pit_c2_read_state == 0) {
            rv = cur & 0xff;
            state->pit_c2_read_state = 1;
        } else {
            rv = (cur >> 8) & 0xff;
            state->pit_c2_read_state = 0;
            state->pit_c2_latch_valid = false;
        }
        break;
    }
    case 0x0f:
        rv = 0;
        break;
    default:
        break;
    }
    return rv;
}

static void sgi_ioc2_int2_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    SgiIoc2State *state = SGI_IOC2(opaque);

    switch (addr) {
    case 0x01:
        state->local_mask[0] = val;
        sgi_ioc2_update_irq(state, 0);
        break;
    case 0x03:
        state->local_mask[1] = val;
        sgi_ioc2_update_irq(state, 1);
        break;
    case 0x05:
        sgi_ioc2_map_mask_write(state, 0, val);
        break;
    case 0x06:
        sgi_ioc2_map_mask_write(state, 1, val);
        break;
    case 0x08:
        /* No IRQ behind it; retain last for readback. */
        state->pit_timer_clear_last = val & 0xff;
        break;
    case 0x0c:
        state->pit_c0_last = val & 0xff;
        break;
    case 0x0d:
        state->pit_c1_last = val & 0xff;
        break;
    case 0x0e:
        /* Counter 2 data (LSB then MSB after CALL). */
        if (state->pit_c2_write_state == 0) {
            state->pit_c2_tmp_lsb = val & 0xff;
            state->pit_c2_write_state = 1;
        } else {
            state->pit_c2_count = state->pit_c2_tmp_lsb |
                                  ((val & 0xff) << 8);
            if (state->pit_c2_count == 0) {
                state->pit_c2_count = 0x10000;
            }
            state->pit_c2_start_ns =
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            state->pit_c2_running = true;
            state->pit_c2_latch_valid = false;
            state->pit_c2_read_state = 0;
            state->pit_c2_write_state = 0;
        }
        break;
    case 0x0f: {
        uint8_t ctrl = val & 0xff;
        /* Decode 8254 control for counter 2 only (0x80 select). */
        if ((ctrl & 0xc0) == 0x80) {
            uint8_t rw = (ctrl >> 4) & 0x03;
            if (rw == 0x00) {
                /* CLAT: latch current count. */
                state->pit_c2_latch =
                    sgi_ioc2_pit_c2_remaining(state);
                state->pit_c2_latch_valid = true;
                state->pit_c2_read_state = 0;
            } else if (rw == 0x03) {
                /* CALL: expect LSB then MSB (mode/bcd ignored for calibration). */
                state->pit_c2_write_state = 0;
                state->pit_c2_latch_valid = false;
                state->pit_c2_read_state = 0;
                if ((ctrl & 0x0e) == 0x08) {
                    /* MSWST (stop): halt counter. */
                    state->pit_c2_running = false;
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps sgi_ioc2_int2_ops = {
    .read = sgi_ioc2_int2_read,
    .write = sgi_ioc2_int2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint64_t sgi_ioc2_read(void *opaque, hwaddr addr, unsigned size)
{
    return sgi_ioc2_read_inner(opaque, addr, size);
}

static void sgi_ioc2_write_inner(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    SgiIoc2State *state = SGI_IOC2(opaque);

    switch (addr + SGI_IOC2_MMIO_BASE) {
    case 0x14:
        state->front_panel &= ~(val & 0x52);
        if (!(state->front_panel & 0x52)) {
            sgi_ioc2_lower_local(state, 1, 0x02);
        }
        break;
    case 0x1a:
        state->dma_select = val;
        break;
    case 0x1c:
        state->reset = 0;
        break;
    case 0x1e:
        state->write = val;
        break;
    default:
        state->base_regs[(addr + SGI_IOC2_MMIO_BASE) & 0x1f] = val;
        break;
    }
}

static void sgi_ioc2_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    sgi_ioc2_write_inner(opaque, addr, val, size);
}

static const MemoryRegionOps sgi_ioc2_ops = {
    .read = sgi_ioc2_read,
    .write = sgi_ioc2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_sgi_ioc2 = {
    .name = TYPE_SGI_IOC2,
    .version_id = 3,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(base_regs, SgiIoc2State, 0x20),
        VMSTATE_UINT8_ARRAY(local_status, SgiIoc2State, 2),
        VMSTATE_UINT8_ARRAY(local_mask, SgiIoc2State, 2),
        VMSTATE_UINT8(map_status, SgiIoc2State),
        VMSTATE_UINT8_ARRAY(map_mask, SgiIoc2State, 2),
        VMSTATE_UINT8(map_polarity, SgiIoc2State),
        VMSTATE_UINT8(error_status, SgiIoc2State),
        VMSTATE_UINT8(front_panel, SgiIoc2State),
        VMSTATE_UINT8(dma_select, SgiIoc2State),
        VMSTATE_UINT8(reset, SgiIoc2State),
        VMSTATE_UINT8(write, SgiIoc2State),
        VMSTATE_UINT32(pit_c2_count, SgiIoc2State),
        VMSTATE_UINT64(pit_c2_start_ns, SgiIoc2State),
        VMSTATE_BOOL(pit_c2_running, SgiIoc2State),
        VMSTATE_UINT16(pit_c2_latch, SgiIoc2State),
        VMSTATE_BOOL(pit_c2_latch_valid, SgiIoc2State),
        VMSTATE_INT32(pit_c2_write_state, SgiIoc2State),
        VMSTATE_INT32(pit_c2_read_state, SgiIoc2State),
        VMSTATE_UINT8(pit_c2_tmp_lsb, SgiIoc2State),
        VMSTATE_UINT8(pit_c0_last, SgiIoc2State),
        VMSTATE_UINT8(pit_c1_last, SgiIoc2State),
        VMSTATE_UINT8(pit_timer_clear_last, SgiIoc2State),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_ioc2_reset(DeviceState *device)
{
    SgiIoc2State *state = SGI_IOC2(device);

    memset(state->base_regs, 0, sizeof(state->base_regs));
    memset(state->local_status, 0, sizeof(state->local_status));
    memset(state->local_mask, 0, sizeof(state->local_mask));
    memset(state->map_mask, 0, sizeof(state->map_mask));
    state->map_status = 0;
    state->map_polarity = 0;
    state->error_status = 0;
    /* MAME ioc2_device::device_reset value: no INT bits set, so no stale
     * front-panel interrupt fires into PROM re-entry. */
    state->front_panel = 0xa1;
    state->dma_select = 0;
    state->reset = 0;
    state->write = 0;
    /* MAME PIT8254 device_reset: counter 2 idle until programmed. */
    state->pit_c2_count = 0;
    state->pit_c2_start_ns = 0;
    state->pit_c2_running = false;
    state->pit_c2_latch = 0;
    state->pit_c2_latch_valid = false;
    state->pit_c2_write_state = 0;
    state->pit_c2_read_state = 0;
    state->pit_c2_tmp_lsb = 0;
    state->pit_c0_last = 0;
    state->pit_c1_last = 0;
    state->pit_timer_clear_last = 0;
    /* Drive both CPU outputs low so no pre-reset level survives. */
    sgi_ioc2_update_irq(state, 0);
    sgi_ioc2_update_irq(state, 1);
}

static void sgi_ioc2_init(Object *object)
{
    SgiIoc2State *state = SGI_IOC2(object);

    memory_region_init_io(&state->iomem, object, &sgi_ioc2_ops, state,
                          TYPE_SGI_IOC2, SGI_IOC2_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &state->iomem);
    memory_region_init_io(&state->int2_iomem, object, &sgi_ioc2_int2_ops,
                          state, TYPE_SGI_IOC2 "-int2", SGI_IOC2_INT2_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &state->int2_iomem);
    qdev_init_gpio_in(DEVICE(object), sgi_ioc2_gpio_in, 9);
    qdev_init_gpio_out(DEVICE(object), state->out, 2);
}

static void sgi_ioc2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->vmsd = &vmstate_sgi_ioc2;
    device_class_set_legacy_reset(device_class, sgi_ioc2_reset);
}

static const TypeInfo sgi_ioc2_type = {
    .name = TYPE_SGI_IOC2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SgiIoc2State),
    .instance_init = sgi_ioc2_init,
    .class_init = sgi_ioc2_class_init,
};

static void sgi_ioc2_register_types(void)
{
    type_register_static(&sgi_ioc2_type);
}

type_init(sgi_ioc2_register_types)

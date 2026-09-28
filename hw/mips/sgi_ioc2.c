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
 *   9 hpc_dma    -> local1 0x10 (MAME ioc2 hpc_dma_done_w)
 *  10 mc_dma     -> local0 0x10 (MAME ioc2 mc_dma_done_w)
 * GPIO outputs 0/1 are the CPU interrupts for local channel 0/1 (IP2/IP3);
 * outputs 2/3 are the PIT timer0/1 interrupts (MAME lines 2/3 = IP4/IP5).
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
#define SGI_IOC2_MMIO_SIZE 0xc0
#define SGI_IOC2_INT2_SIZE 0x10

typedef struct SgiIoc2State SgiIoc2State;

struct SgiIoc2State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion int2_iomem;
    qemu_irq out[4];
    /* Indy (IP24) uses the GUINNESS IOC2: interrupt registers live in the
     * PIO6 window at 0x20-0x2f (MAME ioc2_guinness_device::map) instead of
     * the PIO4 INT2 window. */
    bool guinness;
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
    uint64_t int2_writes;
    /* MAME pit8254 (clk2=1MHz, clk0/1=counter-2 OUT): full 8254 counters.
     * OUT0/OUT1 latch CPU lines 2/3 (IP4/IP5) until timer_int_clear. */
    uint8_t pit_ctrl[3];
    uint16_t pit_count[3];
    uint16_t pit_value[3];
    uint16_t pit_latch[3];
    bool pit_latch_valid[3];
    uint8_t pit_status[3];
    bool pit_status_valid[3];
    bool pit_wmsb[3];
    bool pit_rmsb[3];
    uint8_t pit_tmp_lsb[3];
    bool pit_out[3];
    bool pit_null[3];
    bool pit_timer_latched[2];
    uint64_t pit_c2_last_ns;
    QEMUTimer *pit_timer;
    /*
     * MAME has a single enet_int_w input driving Local0 ETHERNET (0x08);
     * this board splits it into EDLC (gpio 2) and HPC (gpio 3) lines, so
     * track both levels and OR them: clearing one must not drop the bit
     * while the other is still asserted.
     */
    bool enet_edlc_level;
    bool enet_hpc_level;
};

#define SGI_IOC2(obj) OBJECT_CHECK(SgiIoc2State, (obj), TYPE_SGI_IOC2)

static uint64_t sgi_ioc2_int2_reg_read(SgiIoc2State *state, hwaddr reg);
static void sgi_ioc2_int2_reg_write(SgiIoc2State *state, hwaddr reg,
                                     uint64_t val);
static bool sgi_ioc2_trace_enabled(void);

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

/*
 * MAME pit8253.cpp semantics: three 8254 counters; counter 2 runs at 1MHz
 * and its OUT cascades into the counter 0/1 clocks (pit_clock2_out).
 * OUT0/OUT1 rising edges latch CPU lines 2/3 (IP4/IP5) until the guest
 * writes the timer-clear register (set_timer_int_clear).
 */
static void sgi_ioc2_pit_set_out(SgiIoc2State *state, unsigned counter,
                                 bool out);

static uint16_t sgi_ioc2_pit_dec(SgiIoc2State *state, unsigned counter,
                                 uint16_t v)
{
    if (state->pit_ctrl[counter] & 0x01) {
        unsigned lo = v & 0x000f, mi = (v & 0x00f0) >> 4;
        unsigned hi = (v & 0x0f00) >> 8, th = (v & 0xf000) >> 12;
        if (lo == 0) {
            lo = 9;
            if (mi == 0) {
                mi = 9;
                if (hi == 0) {
                    hi = 9;
                    th = (th - 1) & 0xf;
                } else {
                    hi--;
                }
            } else {
                mi--;
            }
        } else {
            lo--;
        }
        return (th << 12) | (hi << 8) | (mi << 4) | lo;
    }
    return (v - 1) & 0xffff;
}

static void sgi_ioc2_pit_tick(SgiIoc2State *state, unsigned counter)
{
    unsigned mode;
    uint16_t v;

    if (counter > 2 || state->pit_null[counter]) {
        return;
    }
    mode = (state->pit_ctrl[counter] >> 1) & 0x07;
    v = state->pit_value[counter];
    switch (mode) {
    case 0:
    case 1:
        if (!state->pit_out[counter]) {
            v = sgi_ioc2_pit_dec(state, counter, v);
            state->pit_value[counter] = v;
            if (v == 0) {
                sgi_ioc2_pit_set_out(state, counter, true);
            }
        }
        break;
    case 2:
        v = sgi_ioc2_pit_dec(state, counter, v);
        if (v == 0) {
            v = state->pit_count[counter];
        }
        state->pit_value[counter] = v;
        sgi_ioc2_pit_set_out(state, counter, v != 1);
        break;
    case 3:
        v = sgi_ioc2_pit_dec(state, counter, v);
        if (v == 0) {
            v = state->pit_count[counter];
            sgi_ioc2_pit_set_out(state, counter, !state->pit_out[counter]);
        }
        state->pit_value[counter] = v;
        break;
    case 4:
    case 5:
    default:
        if (state->pit_out[counter]) {
            v = sgi_ioc2_pit_dec(state, counter, v);
            state->pit_value[counter] = v;
            if (v == 0) {
                /* Strobe low for one clock, then reload. */
                sgi_ioc2_pit_set_out(state, counter, false);
                state->pit_value[counter] = state->pit_count[counter];
            }
        } else {
            sgi_ioc2_pit_set_out(state, counter, true);
        }
        break;
    }
}

static void sgi_ioc2_pit_set_out(SgiIoc2State *state, unsigned counter,
                                 bool out)
{
    bool prev = state->pit_out[counter];

    state->pit_out[counter] = out;
    if (!out || prev) {
        return;
    }
    if (counter < 2) {
        if (!state->pit_timer_latched[counter]) {
            state->pit_timer_latched[counter] = true;
            qemu_set_irq(state->out[2 + counter], 1);
        }
    } else {
        /* MAME pit_clock2_out: counter-2 OUT clocks counters 0/1. */
        sgi_ioc2_pit_tick(state, 0);
        sgi_ioc2_pit_tick(state, 1);
    }
}

static void sgi_ioc2_pit_rearm(SgiIoc2State *state);

static void sgi_ioc2_pit_advance(SgiIoc2State *state)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks;

    if (now <= state->pit_c2_last_ns) {
        return;
    }
    /* Counter 2 runs at 1MHz (MAME set_clk<2>(1000000)). */
    ticks = (now - state->pit_c2_last_ns) / 1000;
    if (ticks == 0) {
        return;
    }
    if (ticks > 1000000) {
        ticks = 1000000;
    }
    state->pit_c2_last_ns += ticks * 1000;
    if (!state->pit_null[2]) {
        for (uint64_t i = 0; i < ticks; i++) {
            sgi_ioc2_pit_tick(state, 2);
        }
    }
    sgi_ioc2_pit_rearm(state);
}

static void sgi_ioc2_pit_rearm(SgiIoc2State *state)
{
    uint64_t ticks = 0;

    if (!state->pit_timer || state->pit_null[2]) {
        return;
    }
    /* Time to the next counter-2 OUT edge; only counter-2 edges can change
     * timer lines (counters 0/1 tick off them). */
    switch ((state->pit_ctrl[2] >> 1) & 0x07) {
    case 0:
    case 1:
        if (!state->pit_out[2]) {
            ticks = state->pit_value[2] ? state->pit_value[2] : 0x10000;
        }
        break;
    case 2:
        if (state->pit_out[2]) {
            ticks = state->pit_value[2] > 1 ? state->pit_value[2] - 1 : 1;
        } else {
            ticks = 1;
        }
        break;
    case 3:
        ticks = state->pit_value[2] ? state->pit_value[2] : 0x10000;
        break;
    case 4:
    case 5:
    default:
        if (state->pit_out[2]) {
            ticks = state->pit_value[2] ? state->pit_value[2] : 0x10000;
        } else {
            ticks = 1;
        }
        break;
    }
    if (ticks == 0) {
        timer_del(state->pit_timer);
        return;
    }
    timer_mod(state->pit_timer,
              state->pit_c2_last_ns + ticks * 1000);
}

static void sgi_ioc2_pit_timeout(void *opaque)
{
    sgi_ioc2_pit_advance(SGI_IOC2(opaque));
}

static void sgi_ioc2_pit_control(SgiIoc2State *state, uint8_t data)
{
    unsigned counter, rw, i;

    sgi_ioc2_pit_advance(state);
    if ((data & 0xc0) == 0xc0) {
        /* Readback command. */
        for (i = 0; i < 3; i++) {
            if (data & (0x02 << i)) {
                if (data & 0x20) {
                    state->pit_latch[i] = state->pit_value[i];
                    state->pit_latch_valid[i] = true;
                }
                if (data & 0x10) {
                    state->pit_status[i] =
                        (state->pit_out[i] ? 0x80 : 0) |
                        (state->pit_null[i] ? 0x40 : 0) |
                        (state->pit_ctrl[i] & 0x3f);
                    state->pit_status_valid[i] = true;
                }
            }
        }
        return;
    }
    counter = (data >> 6) & 0x03;
    if (counter > 2) {
        return;
    }
    rw = (data >> 4) & 0x03;
    if (rw == 0) {
        /* Counter Latch Command: latch the current count so it can be
         * read back without disturbing the running counter.  Unlike a
         * mode/access control word, this does not reprogram anything. */
        state->pit_latch[counter] = state->pit_value[counter];
        state->pit_latch_valid[counter] = true;
        return;
    }
    state->pit_ctrl[counter] = data & 0x3f;
    state->pit_null[counter] = true;
    state->pit_latch_valid[counter] = false;
    state->pit_status_valid[counter] = false;
    if (rw == 1) {
        state->pit_wmsb[counter] = false;
        state->pit_rmsb[counter] = false;
    } else if (rw == 2) {
        state->pit_wmsb[counter] = true;
        state->pit_rmsb[counter] = true;
    } else {
        state->pit_wmsb[counter] = false;
        state->pit_rmsb[counter] = false;
    }
    /* A fresh control word parks OUT until the count is written. */
    if (counter < 2) {
        state->pit_out[counter] = false;
    } else {
        state->pit_out[counter] = (((data >> 1) & 0x07) <= 1) ? false : true;
    }
    sgi_ioc2_pit_rearm(state);
}

static void sgi_ioc2_pit_write(SgiIoc2State *state, unsigned counter,
                               uint8_t data)
{
    uint16_t v;

    if (counter > 2) {
        return;
    }
    sgi_ioc2_pit_advance(state);
    if (!state->pit_wmsb[counter]) {
        state->pit_tmp_lsb[counter] = data;
        if (((state->pit_ctrl[counter] >> 4) & 0x03) == 1) {
            v = data;
        } else {
            state->pit_wmsb[counter] = true;
            return;
        }
    } else {
        v = state->pit_tmp_lsb[counter] | ((uint16_t)data << 8);
        state->pit_wmsb[counter] =
            (((state->pit_ctrl[counter] >> 4) & 0x03) == 3) ? false : true;
    }
    state->pit_count[counter] = v;
    state->pit_value[counter] = v;
    state->pit_null[counter] = false;
    switch ((state->pit_ctrl[counter] >> 1) & 0x07) {
    case 0:
    case 1:
        sgi_ioc2_pit_set_out(state, counter, false);
        break;
    case 2:
    case 4:
    case 5:
        sgi_ioc2_pit_set_out(state, counter, true);
        break;
    case 3:
        sgi_ioc2_pit_set_out(state, counter, true);
        break;
    }
    sgi_ioc2_pit_rearm(state);
}

static uint8_t sgi_ioc2_pit_read(SgiIoc2State *state, unsigned counter)
{
    uint16_t src;
    uint8_t rv;

    if (counter > 2) {
        return 0;
    }
    sgi_ioc2_pit_advance(state);
    if (state->pit_status_valid[counter]) {
        state->pit_status_valid[counter] = false;
        return state->pit_status[counter];
    }
    src = state->pit_latch_valid[counter] ? state->pit_latch[counter] :
                                            state->pit_value[counter];
    if (state->pit_rmsb[counter]) {
        rv = (src >> 8) & 0xff;
        if (((state->pit_ctrl[counter] >> 4) & 0x03) != 2) {
            state->pit_rmsb[counter] = false;
        }
        if (state->pit_latch_valid[counter] &&
            ((state->pit_ctrl[counter] >> 4) & 0x03) != 1) {
            state->pit_latch_valid[counter] = false;
        }
    } else {
        rv = src & 0xff;
        if (((state->pit_ctrl[counter] >> 4) & 0x03) == 3) {
            state->pit_rmsb[counter] = true;
        } else if (state->pit_latch_valid[counter]) {
            state->pit_latch_valid[counter] = false;
        }
    }
    return rv;
}

static void sgi_ioc2_gpio_in(void *opaque, int n, int level)
{
    SgiIoc2State *state = SGI_IOC2(opaque);

    switch (n) {
    case 0:
        if (sgi_ioc2_trace_enabled()) {
            fprintf(stderr, "IOC2-IN0 level=%d st0=%02x mask0=%02x\n",
                    level, state->local_status[0], state->local_mask[0]);
        }
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
        if (n == 2) {
            state->enet_edlc_level = level != 0;
        } else {
            state->enet_hpc_level = level != 0;
        }
        if (state->enet_edlc_level || state->enet_hpc_level) {
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
    case 9:
        if (level) {
            sgi_ioc2_raise_local(state, 1, 0x10);
        } else {
            sgi_ioc2_lower_local(state, 1, 0x10);
        }
        break;
    case SGI_IOC2_IN_VIDEO:
        /* MAME video_int_w: local1 INT3_LOCAL1_VIDEO (0x40). */
        if (level) {
            sgi_ioc2_raise_local(state, 1, 0x40);
        } else {
            sgi_ioc2_lower_local(state, 1, 0x40);
        }
        break;
    case SGI_IOC2_IN_MC_DMA:
        /* MAME mc_dma_done_w: MEMCTL DMA-done to local0 0x10. */
        if (level) {
            sgi_ioc2_raise_local(state, 0, 0x10);
        } else {
            sgi_ioc2_lower_local(state, 0, 0x10);
        }
        break;
    default:
        break;
    }
}

static uint64_t sgi_ioc2_read_inner(void *opaque, hwaddr addr, unsigned size)
{
    SgiIoc2State *state = SGI_IOC2(opaque);
    /*
     * MAME decode is byte-wide for both IOC2 variants: pio6_map maps the
     * device with .umask16(0x00ff) (MAME indy_indigo2.cpp:258-261 for
     * GUINNESS, :274-277 for FULL HOUSE) and MAME hpc3.cpp:721-733 hands
     * every PIO register to the device as
     * read_word(offset & 0xff, 0x00ff) & 0xff, so the register index is
     * the byte offset in the channel, low byte only.  FULL HOUSE is
     * reached through a window 0x14 shorter than its register numbers.
     */
    hwaddr reg = state->guinness ? addr : addr + SGI_IOC2_MMIO_BASE;

    if (sgi_ioc2_trace_enabled()) {
        fprintf(stderr, "IOC2N-R reg=%02x\n", (unsigned)reg);
    }

    /* GUINNESS (IP24): interrupt registers at PIO6 0x20-0x2f. */
    if (state->guinness && reg >= 0x20 && reg <= 0x2f) {
        if (reg == 0x29) {
            return state->error_status;
        }
        return sgi_ioc2_int2_reg_read(state, reg - 0x20);
    }

    switch (reg) {
    case 0x14:
        return state->front_panel;
    case 0x16:
        /* MAME ioc2.h: GUINNESS (Indy) system_id = 0x26,
         * Full House (IP22) = 0x11. */
        return state->guinness ? 0x26 : 0x11;
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
 * status/mask, timer clear, and the 8254 PIT (counter 2 at 1MHz, its OUT
 * cascading into counters 0/1; OUT0/OUT1 latch CPU lines 2/3).
 * MAME ioc2_full_house_device::int2_map.
 */
static uint64_t sgi_ioc2_int2_reg_read(SgiIoc2State *state, hwaddr reg)
{
    uint64_t rv = 0;

    switch (reg) {
    case 0x00:
        rv = state->local_status[0];
        break;
    case 0x01:
        rv = state->local_mask[0];
        break;
    case 0x02:
        rv = state->local_status[1];
        break;
    case 0x03:
        rv = state->local_mask[1];
        break;
    case 0x04:
        rv = sgi_ioc2_map_status_read(state);
        break;
    case 0x05:
        rv = state->map_mask[0];
        break;
    case 0x06:
        rv = state->map_mask[1];
        break;
    case 0x07:
        rv = state->map_polarity;
        break;
    case 0x08:
        /* MAME int2_map: timer clear is write-only; reads return 0. */
        rv = 0;
        break;
    case 0x0c:
        rv = sgi_ioc2_pit_read(state, 0);
        break;
    case 0x0d:
        rv = sgi_ioc2_pit_read(state, 1);
        break;
    case 0x0e:
        rv = sgi_ioc2_pit_read(state, 2);
        break;
    case 0x0f:
        rv = 0;
        break;
    default:
        break;
    }
    return rv;
}

static uint64_t sgi_ioc2_int2_read(void *opaque, hwaddr addr, unsigned size)
{
    return sgi_ioc2_int2_reg_read(SGI_IOC2(opaque), addr);
}

/* Board tracing (off by default; set SGI_TRACE=1 to re-enable). */
static bool sgi_ioc2_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("SGI_TRACE") ? 1 : 0;
    }
    return cached != 0;
}

static void sgi_ioc2_int2_reg_write(SgiIoc2State *state, hwaddr reg,
                                    uint64_t val)
{
    switch (reg) {
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
    case 0x07:
        /* MAME map_pol_w. */
        state->map_polarity = val & 0xff;
        break;
    case 0x08:
        /* MAME set_timer_int_clear: release CPU lines 2/3 (IP4/IP5). */
        if (val & 0x01) {
            state->pit_timer_latched[0] = false;
            qemu_set_irq(state->out[2], 0);
        }
        if (val & 0x02) {
            state->pit_timer_latched[1] = false;
            qemu_set_irq(state->out[3], 0);
        }
        break;
    case 0x0c:
        sgi_ioc2_pit_write(state, 0, val & 0xff);
        break;
    case 0x0d:
        sgi_ioc2_pit_write(state, 1, val & 0xff);
        break;
    case 0x0e:
        sgi_ioc2_pit_write(state, 2, val & 0xff);
        break;
    case 0x0f:
        sgi_ioc2_pit_control(state, val & 0xff);
        break;
    default:
        break;
    }
}

static void sgi_ioc2_int2_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    SgiIoc2State *state = SGI_IOC2(opaque);

    if (sgi_ioc2_trace_enabled() && state->int2_writes < 4096) {
        fprintf(stderr, "IOC2W addr=%02x sz=%u val=%02x\n",
                (unsigned)addr, size, (unsigned)val);
    }
    state->int2_writes++;

    sgi_ioc2_int2_reg_write(state, addr, val);
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
    SgiIoc2State *state = SGI_IOC2(opaque);

    /*
     * MAME pio6_map umask16(0x00ff): each 16-bit word exposes only its
     * low-address byte; upper lanes read 0. Assemble wide reads from
     * the low lane of each word (FullHouse keeps byte-split behavior).
     */
    if (state->guinness && size > 1) {
        uint64_t rv = 0;
        unsigned i;

        for (i = 0; i < size; i += 2) {
            rv |= (sgi_ioc2_read_inner(opaque, addr + i, 1) & 0xff) <<
                  (8 * i);
        }
        return rv;
    }
    return sgi_ioc2_read_inner(opaque, addr, size);
}

static void sgi_ioc2_write_inner(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    SgiIoc2State *state = SGI_IOC2(opaque);
    hwaddr reg = state->guinness ? addr : addr + SGI_IOC2_MMIO_BASE;

    if (sgi_ioc2_trace_enabled()) {
        fprintf(stderr, "IOC2N-W reg=%02x val=%02x\n",
                (unsigned)reg, (unsigned)(val & 0xff));
    }

    /* GUINNESS (IP24): interrupt registers at PIO6 0x20-0x2f. */
    if (state->guinness && reg >= 0x20 && reg <= 0x2f) {
        if (reg != 0x29) {
            sgi_ioc2_int2_reg_write(state, reg - 0x20, val);
        }
        return;
    }

    switch (reg) {
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
        state->base_regs[reg & 0x1f] = val;
        break;
    }
}

static void sgi_ioc2_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    SgiIoc2State *state = SGI_IOC2(opaque);

    /* MAME umask16: only the low-address byte of each 16-bit word is
     * significant; upper lanes are dropped (FullHouse keeps byte-split). */
    if (state->guinness && size > 1) {
        unsigned i;

        for (i = 0; i < size; i += 2) {
            sgi_ioc2_write_inner(opaque, addr + i, (val >> (8 * i)) & 0xff,
                                 1);
        }
        return;
    }
    sgi_ioc2_write_inner(opaque, addr, val, size);
}

static const MemoryRegionOps sgi_ioc2_ops = {
    .read = sgi_ioc2_read,
    .write = sgi_ioc2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * GUINNESS registers are word-laned per MAME's PIO6 umask16(0x00ff): wide
 * accesses resolve to the low-address byte of each 16-bit word (see
 * sgi_ioc2_read/write above), so impl must accept the full width.
 */
static const MemoryRegionOps sgi_ioc2_guinness_ops = {
    .read = sgi_ioc2_read,
    .write = sgi_ioc2_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static const VMStateDescription vmstate_sgi_ioc2 = {
    .name = TYPE_SGI_IOC2,
    .version_id = 6,
    .minimum_version_id = 6,
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
        VMSTATE_UINT8_ARRAY(pit_ctrl, SgiIoc2State, 3),
        VMSTATE_UINT16_ARRAY(pit_count, SgiIoc2State, 3),
        VMSTATE_UINT16_ARRAY(pit_value, SgiIoc2State, 3),
        VMSTATE_UINT16_ARRAY(pit_latch, SgiIoc2State, 3),
        VMSTATE_BOOL_ARRAY(pit_latch_valid, SgiIoc2State, 3),
        VMSTATE_UINT8_ARRAY(pit_status, SgiIoc2State, 3),
        VMSTATE_BOOL_ARRAY(pit_status_valid, SgiIoc2State, 3),
        VMSTATE_BOOL_ARRAY(pit_wmsb, SgiIoc2State, 3),
        VMSTATE_BOOL_ARRAY(pit_rmsb, SgiIoc2State, 3),
        VMSTATE_UINT8_ARRAY(pit_tmp_lsb, SgiIoc2State, 3),
        VMSTATE_BOOL_ARRAY(pit_out, SgiIoc2State, 3),
        VMSTATE_BOOL_ARRAY(pit_null, SgiIoc2State, 3),
        VMSTATE_BOOL_ARRAY(pit_timer_latched, SgiIoc2State, 2),
        VMSTATE_UINT64(pit_c2_last_ns, SgiIoc2State),
        VMSTATE_BOOL(guinness, SgiIoc2State),
        VMSTATE_BOOL(enet_edlc_level, SgiIoc2State),
        VMSTATE_BOOL(enet_hpc_level, SgiIoc2State),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_ioc2_reset(DeviceState *device)
{
    SgiIoc2State *state = SGI_IOC2(device);
    unsigned i;

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
    state->enet_edlc_level = false;
    state->enet_hpc_level = false;
    /* MAME pit8253 device_reset: control 0x30, count 0, OUT undetermined
     * (kept low here), null count set. */
    for (i = 0; i < 3; i++) {
        state->pit_ctrl[i] = 0x30;
        state->pit_count[i] = 0;
        state->pit_value[i] = 0;
        state->pit_latch[i] = 0;
        state->pit_latch_valid[i] = false;
        state->pit_status[i] = 0;
        state->pit_status_valid[i] = false;
        state->pit_wmsb[i] = false;
        state->pit_rmsb[i] = false;
        state->pit_tmp_lsb[i] = 0;
        state->pit_out[i] = false;
        state->pit_null[i] = true;
    }
    state->pit_timer_latched[0] = false;
    state->pit_timer_latched[1] = false;
    state->pit_c2_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (state->pit_timer) {
        timer_del(state->pit_timer);
    }
    /* Drive all CPU outputs low so no pre-reset level survives. */
    sgi_ioc2_update_irq(state, 0);
    sgi_ioc2_update_irq(state, 1);
    qemu_set_irq(state->out[2], 0);
    qemu_set_irq(state->out[3], 0);
}

static void sgi_ioc2_init(Object *object)
{
    SgiIoc2State *state = SGI_IOC2(object);

    memory_region_init_io(&state->iomem, object,
                          state->guinness ? &sgi_ioc2_guinness_ops
                                          : &sgi_ioc2_ops,
                          state, TYPE_SGI_IOC2, SGI_IOC2_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &state->iomem);
    memory_region_init_io(&state->int2_iomem, object, &sgi_ioc2_int2_ops,
                          state, TYPE_SGI_IOC2 "-int2", SGI_IOC2_INT2_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &state->int2_iomem);
    qdev_init_gpio_in(DEVICE(object), sgi_ioc2_gpio_in, 12);
    qdev_init_gpio_out(DEVICE(object), state->out, 4);
    state->pit_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    sgi_ioc2_pit_timeout, state);
}

static const Property sgi_ioc2_properties[] = {
    DEFINE_PROP_BOOL("guinness", SgiIoc2State, guinness, false),
};

static void sgi_ioc2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->vmsd = &vmstate_sgi_ioc2;
    device_class_set_legacy_reset(device_class, sgi_ioc2_reset);
    device_class_set_props(device_class, sgi_ioc2_properties);
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

/*
 * QEMU SGI Indigo2 support
 *
 * Copyright (c) 2024 QEMU Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "hw/core/clock.h"
#include "hw/mips/mips.h"
#include "hw/mips/sgi_hpc3.h"
#include "hw/mips/sgi_hpc3_scsi.h"
#include "hw/mips/sgi_ioc2.h"
#include "hw/mips/sgi_mc.h"
#include "hw/char/escc.h"
#include "hw/nvram/eeprom93xx.h"
#include "hw/isa/isa.h"
#include "system/system.h"
#include "system/runstate.h"
#include "hw/core/boards.h"
#include "net/net.h"
#include "qemu/mem-reentrancy.h"
#include "hw/scsi/sgi_wd33c93.h"
#include "hw/core/loader.h"
#include "hw/display/vga.h"
#include "hw/display/bochs-vbe.h"
#include "hw/input/i8042.h"
#include "hw/core/or-irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/audio.h"
#include "qemu/help_option.h"
#include "qemu/timer.h"
#ifdef CONFIG_TCG
#include "accel/tcg/cpu-ops.h"
#endif /* CONFIG_TCG */
#include "target/mips/cpu.h"

#define BIOS_SIZE (4 * MiB)
/* IP22 PROMs (ip22prom.*.bin) are 512 KiB. */
#define INDIGO2_BIOS_SIZE 0x80000

#define SGI_GIO64_BASE 0x1f000000ULL
/* Single GIO64 bus owner for 0x1f000000-0x1f9fffff (MAME indy_indigo2.cpp:230). */
#define SGI_IO_BASE 0x1fb00000ULL
#define SGI_IO_SIZE 0x00010000ULL /* 64KB */
#define SGI_MEM_BASE 0x00000000ULL
#define SGI_MEM_SIZE 0x10000000ULL /* 256MB */
#define SGI_MC_BASE 0x1fa00000ULL
#define SGI_HPC3_BASE 0x1fb80000ULL
#define SGI_SCSI_BASE (SGI_HPC3_BASE + 0x40000ULL)
#define SGI_BBRAM_BASE (SGI_HPC3_BASE + 0x60000ULL)

static SGIWD33C93State *sgi_wd33c93[2];

static void mips_sgi_indigo2_init(MachineState *machine);

static void main_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;
    cpu_reset(CPU(cpu));
}

static uint32_t sgi_hpc3_intstat;
static uint32_t sgi_hpc3_misc;
/* MAME hpc3 dma_complete_int_cb: PBUS/SCSI descriptor-done (bit 29)
 * asserts IOC2 Local1 0x10; the line drops when no intstat bits remain.
 * Fed through the dma_or gate below alongside the WD dma-done lines. */
static qemu_irq sgi_hpc3_pbus_done_irq;

typedef struct SgiHpc3EthernetDma {
    uint32_t cbp;
    /* WHY: RX and TX channels have independent next-descriptor pointers
     * (iris hpc3.rs:329 nbdp per PdmaChannel; MAME m_enet_rx_nbdp vs
     * m_enet_tx_nbdp). One shared nbdp lets TX kicks clobber the RX ring
     * position and vice versa, hiding every inbound frame. */
    uint32_t rx_nbdp;
    uint32_t tx_nbdp;
    uint32_t bc;
    uint32_t rx_ctrl;
    uint32_t tx_ctrl;
    uint32_t cbdp;
    uint32_t cpfbdp;
    uint32_t ppfbdp;
    uint32_t dmacfg;
    uint32_t piocfg;
} SgiHpc3EthernetDma;

static SgiHpc3EthernetDma sgi_hpc3_enet_dma;
static uint32_t sgi_hpc3_enet_misc;
static bool sgi_hpc3_enet_rx_active;
static bool sgi_hpc3_enet_rx_ready;
/* WHY: delivery runs from both the TAP receive callback and the RX-enable
 * path below; the guard keeps a nested entry from re-entering the DMA walk
 * (iris seeq8003.rs pumps with DMA ops outside the state lock for the same
 * reason). TAP re-poll on receiver enable is iris hpc3.rs:999-1000. */
static bool sgi_hpc3_enet_rx_in_event;

static void sgi_hpc3_enet_rx_start(void);
static void sgi_hpc3_enet_tx_start(void);
static void sgi_hpc3_enet_rx_ready_event(void);
static void sgi_hpc3_enet_update_irq(void);
static void sgi_edlc_reset(void);
static bool sgi_edlc_irq_level(void);
static void sgi_edlc_update_irq(void);
/* Tentative definitions: full definitions with initializers live in the
 * EDLC block below; the DMA register handlers above need the symbols. */
static uint8_t sgi_edlc_rx_status;
static uint8_t sgi_edlc_tx_status;
static NICState *sgi_ethernet_nic;

static uint8_t sgi_edlc_fifo_read(void);
static void sgi_edlc_fifo_write(uint8_t value);

typedef struct SgiHpc3PbusDma {
    uint32_t current;
    uint32_t descriptor;
    uint32_t next;
    uint32_t flags;
    uint32_t bytes_left;
    uint32_t control;
    uint32_t config;
    QEMUTimer *timer;
} SgiHpc3PbusDma;

static SgiHpc3PbusDma sgi_hpc3_pbus_dma[8];

static uint64_t sgi_hal2_rate_ns(unsigned channel);
static void sgi_hal2_dma_write(unsigned channel, int16_t sample);

static uint32_t sgi_hpc3_dma_read32(hwaddr addr)
{
    uint32_t value = 0;

    if ((addr & 0xe0000000U) == 0x80000000U ||
        (addr & 0xe0000000U) == 0xa0000000U) {
        addr &= 0x1fffffffU;
    }
    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                       &value, sizeof(value));
    return be32_to_cpu(value);
}

static void sgi_hpc3_pbus_fetch(SgiHpc3PbusDma *dma)
{
    dma->current = sgi_hpc3_dma_read32(dma->descriptor);
    dma->flags = sgi_hpc3_dma_read32(dma->descriptor + 4);
    dma->next = sgi_hpc3_dma_read32(dma->descriptor + 8);
    dma->bytes_left = dma->flags & 0x3fff;
}

static void sgi_hpc3_pbus_tick(void *opaque)
{
    SgiHpc3PbusDma *dma = opaque;
    unsigned channel = dma - sgi_hpc3_pbus_dma;
    uint32_t word;
    int16_t sample;
    uint64_t rate;

    if (!dma->bytes_left || !(dma->control & 0x10)) {
        timer_del(dma->timer);
        return;
    }

    if (!(dma->control & 0x04) && channel < 4) {
        word = sgi_hpc3_dma_read32(dma->current);
        /* WHY: PBUS audio words carry a 16-bit sample as `sample << 8`
         * (bits 23:8, low byte zero-padded; trace-proven PROM chime words
         * like 0x00000500/0xffffe400), so the sample is bits 23:8
         * (iris hpc3.rs:546-551, iris docs/hal2.md:180-190). dmacfg bit 19
         * (even_high) is a physical PBUS byte-lane hint that does not
         * change the in-memory word layout (iris hpc3.rs:549-550). */
        sample = (int16_t)(word >> 8);
        sgi_hal2_dma_write(channel, sample);
    }
    dma->current += 4;
    dma->bytes_left = dma->bytes_left > 4 ? dma->bytes_left - 4 : 0;

    if (!dma->bytes_left) {
        if (dma->flags & 0x20000000) {
            sgi_hpc3_intstat |= 1U << channel;
            /* MAME do_pbus_dma: bit 29 raises dma_complete (Local1). */
            qemu_set_irq(sgi_hpc3_pbus_done_irq, 1);
        }
        if (dma->flags & 0x80000000) {
            dma->control &= ~0x10U;
            timer_del(dma->timer);
            return;
        }
        dma->descriptor = dma->next;
        sgi_hpc3_pbus_fetch(dma);
    }
    rate = sgi_hal2_rate_ns(channel);
    if (rate) {
        timer_mod_ns(dma->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + rate);
    } else {
        timer_del(dma->timer);
    }
}

static bool sgi_hpc3_pbus_access(hwaddr addr, unsigned *channel,
                                 unsigned *reg)
{
    /* PBUS DMA channels live at +0x0000 (MAME hpc3.cpp:185) but this
     * window is mounted at +0x1000, so add the bias back before decoding. */
    addr += 0x1000;
    if (addr >= 0x10000) {
        return false;
    }
    *channel = addr / 0x2000;
    *reg = addr & 0x1fff;
    return *channel < ARRAY_SIZE(sgi_hpc3_pbus_dma);
}

static uint64_t eisa_io_read(void *opaque, hwaddr addr, unsigned size)
{
    return size == 8 ? UINT64_MAX : (1ULL << (size * 8)) - 1;
}

static void eisa_io_write(void *opaque, hwaddr addr,
                          uint64_t value, unsigned size)
{
}

static const MemoryRegionOps eisa_io_ops = {
    .read = eisa_io_read,
    .write = eisa_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t sgi_hpc3_pbus_dma_read(void *opaque, hwaddr addr,
                               unsigned size)
{
    unsigned channel;
    unsigned reg;

    if (sgi_ip22_trace_enabled() && addr >= 0x31000 && addr < 0x40000) {
        fprintf(stderr, "HPC3GAP-R off=0x%05x size=%u\n", (unsigned)addr, size);
    }
    if (sgi_hpc3_pbus_access(addr, &channel, &reg)) {
        SgiHpc3PbusDma *dma = &sgi_hpc3_pbus_dma[channel];

        switch (reg) {
        case 0x0000:
            return dma->current;
        case 0x0004:
            return dma->descriptor;
        case 0x1000: {
            /*
             * MAME pbusdma_r control: bit 0 while the channel's intstat
             * bit is set, bit 1 while the timer runs; reading clears the
             * intstat bit, and dma_complete drops when no bits remain
             * (here: deassert this channel's OR input; the gate ORs all
             * sources exactly like MAME's intstat!=0 level).
             */
            uint64_t rv = (dma->timer && timer_pending(dma->timer) ? 2 : 0) |
                          ((sgi_hpc3_intstat >> channel) & 1);

            if ((sgi_hpc3_intstat >> channel) & 1) {
                sgi_hpc3_intstat &= ~(1U << channel);
                qemu_set_irq(sgi_hpc3_pbus_done_irq, 0);
            }
            return rv;
        }
        default:
            break;
        }
    }
    switch (addr) {
    case 0x13000: return sgi_hpc3_enet_dma.cbp;
    case 0x13004: return sgi_hpc3_enet_dma.rx_nbdp;
    case 0x14000: return sgi_hpc3_enet_dma.bc;
    /* WHY: rcvstat low bits mirror live SEEQ RX status (iris hpc3.rs:944-956,
     * mask 0xBF at hpc3.rs:186; MAME hpc3.cpp enet_intr_in_w copies the EDLC
     * status into rx_ctrl). Bit 6 is HPC-internal and stays stored. */
    case 0x14004: {
        uint32_t rv = (sgi_hpc3_enet_dma.rx_ctrl & ~0xbfU) |
                      (sgi_edlc_rx_status & 0xbfU);
        return rv;
    }
    case 0x14008: return 0;
    case 0x1400c: return 0;
    /* WHY: INTPEND (bit 1) is live EDLC level, not stored misc (iris
     * hpc3.rs:961-968: INTPEND comes from SeeqState). */
    case 0x14014: {
        uint32_t rv = (sgi_hpc3_enet_misc & ~0x02U) |
                      (sgi_edlc_irq_level() ? 0x02U : 0U);
        return rv;
    }
    case 0x14018: return sgi_hpc3_enet_dma.dmacfg;
    case 0x1401c: return sgi_hpc3_enet_dma.piocfg;
    case 0x15000: return sgi_hpc3_enet_dma.cbp;
    case 0x15004: return sgi_hpc3_enet_dma.tx_nbdp;
    case 0x16000: return sgi_hpc3_enet_dma.bc;
    /* WHY: trstat low byte mirrors live SEEQ TX status (iris hpc3.rs:1022-1030,
     * full-byte mask at hpc3.rs:205); this is where IRIX clean_xd_chain reads
     * SEQ_XS_SUCCESS. */
    case 0x16004:
        return (sgi_hpc3_enet_dma.tx_ctrl & ~0xffU) |
               (sgi_edlc_tx_status & 0xffU);
    case 0x16008: return 0;
    case 0x1600c: return 0;
    case 0x17000: return sgi_hpc3_enet_dma.bc;
    case 0x17004:
        return (sgi_hpc3_enet_dma.tx_ctrl & ~0xffU) |
               (sgi_edlc_tx_status & 0xffU);
    case 0x18000: return sgi_hpc3_enet_dma.cbdp;
    case 0x1a000: return sgi_hpc3_enet_dma.cpfbdp;
    case 0x1a004: return sgi_hpc3_enet_dma.ppfbdp;
    default: break;
    }
    if (addr >= 0x2b000 && addr < 0x2b100) {
        return sgi_edlc_fifo_read();
    }
    if (addr >= 0x2d000 && addr < 0x2d200) {
        return 0;
    }
    if (addr == 0x2f000 || addr == 0x2f00c) {
        return sgi_hpc3_intstat;
    }
    if (addr == 0x2f004) {
        return sgi_hpc3_misc;
    }
    if (sgi_ip22_trace_enabled() && addr >= 0x31000 && addr < 0x40000) {
        fprintf(stderr, "HPC3GAP-R off=0x%05x -> 0xff (default)\n",
                (unsigned)addr);
    }
    return 0xff;
}

static void sgi_hpc3_pbus_dma_write(void *opaque, hwaddr addr,
                            uint64_t val, unsigned size)
{
    unsigned channel;
    unsigned reg;

    if (sgi_hpc3_pbus_access(addr, &channel, &reg)) {
        SgiHpc3PbusDma *dma = &sgi_hpc3_pbus_dma[channel];

        switch (reg) {
        case 0x0000:
            dma->current = val;
            return;
        case 0x0004:
            dma->descriptor = val;
            sgi_hpc3_pbus_fetch(dma);
            return;
        case 0x1000:
            dma->control = val;
            if ((val & 0x30) == 0x30 && channel < 4 && dma->bytes_left) {
                uint64_t rate = sgi_hal2_rate_ns(channel);

                if (rate) {
                    timer_mod_ns(dma->timer,
                                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + rate);
                } else {
                    timer_del(dma->timer);
                }
            } else if (!(val & 0x10)) {
                timer_del(dma->timer);
            }
            return;
        default:
            break;
        }
    }
    switch (addr) {
    case 0x13000: sgi_hpc3_enet_dma.cbp = val; break;
    case 0x13004:
        sgi_hpc3_enet_dma.rx_nbdp = val; break;
    case 0x14004: {
        /* WHY: ACTIVE changes only when AMASK is clear in the written value
         * (iris hpc3.rs:987-988); a set AMASK preserves the channel state.
         * The rising edge delivers held frames and re-polls TAP. */
        bool was_active = sgi_hpc3_enet_rx_active;

        if (!(val & 0x400U)) {
            sgi_hpc3_enet_rx_active = (val & 0x200U) != 0;
        }
        sgi_hpc3_enet_dma.rx_ctrl =
            (val & ~0x200U) | (sgi_hpc3_enet_rx_active ? 0x200U : 0U);
        if (!was_active && sgi_hpc3_enet_rx_active) {
            sgi_hpc3_enet_rx_start();
        }
        sgi_hpc3_enet_update_irq();
        break;
    }
    case 0x14014: {
        /* WHY: ENET reset register (iris hpc3.rs:1972-1998, MAME hpc3.cpp
         * enet_misc_w). Bit 0 is channel reset, bit 1 is the CLRINT strobe
         * (never stored; it marks both status OLD via reset_interrupt),
         * RBO is write-1-to-clear, and only set bits clear (never a full
         * register replace). */
        bool clrint = (val & 0x02U) != 0;

        if (val & 0x800U) {
            sgi_hpc3_enet_misc &= ~0x800U;
            sgi_hpc3_enet_dma.rx_ctrl &= ~0x800U;
        }
        sgi_hpc3_enet_misc =
            (val & ~0x802U) | (sgi_hpc3_enet_misc & 0x800U & ~val);
        if (val & 1) {
            sgi_edlc_reset();
        }
        if (clrint) {
            sgi_edlc_rx_status |= 0x80;
            sgi_edlc_tx_status |= 0x80;
            sgi_edlc_update_irq();
            sgi_hpc3_enet_update_irq();
            /* WHY: re-offer frames stalled while status was NEW; receive
             * returns 0 then so the net queue holds them (iris hpc3.rs:999-1000
             * kick_rx on enable; iris seeq8003.rs:431-433 kick_rx drains queued frames). */
            if (sgi_ethernet_nic) {
                qemu_flush_queued_packets(qemu_get_queue(sgi_ethernet_nic));
            }
        }
        break;
    }
    case 0x14018: sgi_hpc3_enet_dma.dmacfg = val; break;
    case 0x1401c: sgi_hpc3_enet_dma.piocfg = val; break;
    case 0x15004:
        sgi_hpc3_enet_dma.tx_nbdp = val;
        break;
    case 0x16004: {
        bool was_active = sgi_hpc3_enet_dma.tx_ctrl & 0x200U;
        bool now_active;

        /* WHY: same AMASK rule as RX (iris hpc3.rs:1052-1053); the ACTIVE
         * rising edge is the TX trigger. */
        sgi_hpc3_enet_dma.tx_ctrl =
            (sgi_hpc3_enet_dma.tx_ctrl & 0x200U) | (val & ~0x200U);
        if (!(val & 0x400U)) {
            if (val & 0x200U) {
                sgi_hpc3_enet_dma.tx_ctrl |= 0x200U;
            } else {
                sgi_hpc3_enet_dma.tx_ctrl &= ~0x200U;
            }
        }
        now_active = sgi_hpc3_enet_dma.tx_ctrl & 0x200U;
        if (!was_active && now_active) {
            sgi_hpc3_enet_tx_start();
        }
        sgi_hpc3_enet_update_irq();
        break;
    }
    case 0x18000: sgi_hpc3_enet_dma.cbdp = val; break;
    case 0x1a000: sgi_hpc3_enet_dma.cpfbdp = val; break;
    case 0x1a004: sgi_hpc3_enet_dma.ppfbdp = val; break;
    default: break;
    }
    if (addr >= 0x2d000 && addr < 0x2d200) {
        sgi_edlc_fifo_write(val >> ((3 - (addr & 3)) * 8));
    } else if (addr == 0x2f004) {
        sgi_hpc3_misc = val;
    } else if (addr == 0x2f000 || addr == 0x2f00c) {
        sgi_hpc3_intstat &= ~val;
    }
}

static const MemoryRegionOps sgi_hpc3_pbus_dma_ops = {
    .read = sgi_hpc3_pbus_dma_read,
    .write = sgi_hpc3_pbus_dma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint32_t sgi_hpc3_dma_cfg[16];
static uint32_t sgi_hpc3_pio_cfg[16];

/* Board tracing is compiled out by default (zero run-time overhead).
 * Rebuild with -DSGI_DEBUG to re-enable the SGI_TRACE=1 toggle. */
bool sgi_ip22_trace_enabled(void)
{
#ifdef SGI_DEBUG
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("SGI_TRACE") ? 1 : 0;
    }
    return cached != 0;
#else
    return false;
#endif
}
static uint8_t sgi_hpc3_pio_regs[0x1000];
static uint16_t sgi_hal2_codec[2][2];
static uint16_t sgi_hal2_bres[3][3];
static uint16_t sgi_hal2_isr;
static uint16_t sgi_hal2_iar;
static uint16_t sgi_hal2_idr[4];
static uint16_t sgi_hal2_dma_enable;
static uint16_t sgi_hal2_dma_drive;
static int16_t sgi_hal2_dac[2];
static uint64_t sgi_hal2_dac_samples[2];
static unsigned sgi_hal2_curr_dac;

/* Guest-selected audio backend id (MachineState.audiodev), resolved lazily
 * so a missing backend only mutes audio instead of aborting the boot. */
static char *sgi_hal2_audiodev;
static AudioBackend *sgi_hal2_be;
static SWVoiceOut *sgi_hal2_voice;
static unsigned sgi_hal2_voice_freq;

/* PBUS delivers one 16-bit sample per tick; the voice consumes stereo
 * frames, so samples queue here until the backend pulls them. */
#define SGI_HAL2_FIFO_SIZE 65536
static int16_t sgi_hal2_fifo[SGI_HAL2_FIFO_SIZE];
static unsigned sgi_hal2_fifo_head;
static unsigned sgi_hal2_fifo_count;
/* An unwritten DAC keeps its last output, so mono (left only) pairs each
 * sample with the held right value (MAME hal2.cpp:365-378). */
static int16_t sgi_hal2_hold_right;
static int16_t sgi_hal2_hold_left;
static int16_t sgi_hal2_pending_left;
static bool sgi_hal2_pending_valid;

static void sgi_hal2_reset_state(void)
{
    sgi_hal2_isr = 0;
    sgi_hal2_iar = 0;
    memset(sgi_hal2_idr, 0, sizeof(sgi_hal2_idr));
    memset(sgi_hal2_codec, 0, sizeof(sgi_hal2_codec));
    memset(sgi_hal2_bres, 0, sizeof(sgi_hal2_bres));
    sgi_hal2_dma_enable = 0;
    sgi_hal2_dma_drive = 0;
    memset(sgi_hal2_dac, 0, sizeof(sgi_hal2_dac));
    memset(sgi_hal2_dac_samples, 0, sizeof(sgi_hal2_dac_samples));
    sgi_hal2_curr_dac = 0;
    sgi_hal2_fifo_head = 0;
    sgi_hal2_fifo_count = 0;
    sgi_hal2_hold_right = 0;
    sgi_hal2_hold_left = 0;
    sgi_hal2_pending_left = 0;
    sgi_hal2_pending_valid = false;
}

/* Indirect values are staged in IDR0-3, then committed by writing the
 * target address to IAR (MAME hal2.cpp:115-305). */
static void sgi_hal2_iar_write(uint16_t iar)
{
    unsigned type = iar & 0xf000;
    unsigned num = iar & 0x0f00;
    bool read = (iar & 0x0080) != 0;
    unsigned param = (iar & 0x000c) >> 2;

    sgi_hal2_iar = iar;
    if (type == 0x1000 && (num == 0x0400 || num == 0x0500)) {
        /* Codec A/B CTRL1 holds channel/clock/mode, CTRL2 gain (MAME hal2.cpp:134-206). */
        unsigned codec = (num == 0x0400) ? 0 : 1;

        if (param == 1) {
            if (read) {
                sgi_hal2_idr[0] = sgi_hal2_codec[codec][0];
            } else {
                sgi_hal2_codec[codec][0] = sgi_hal2_idr[0];
            }
        } else if (param == 2) {
            if (read) {
                sgi_hal2_idr[0] = sgi_hal2_codec[codec][1];
            } else {
                sgi_hal2_codec[codec][1] = sgi_hal2_idr[0];
            }
        }
        return;
    }
    if (type == 0x2000 && num >= 0x0100 && num <= 0x0300) {
        /* One of three identical Bresenham generators (MAME hal2.cpp:215-270). */
        unsigned gen = (num >> 8) - 1;

        if (param == 1) {
            if (read) {
                sgi_hal2_idr[0] = sgi_hal2_bres[gen][0];
            } else {
                sgi_hal2_bres[gen][0] = sgi_hal2_idr[0];
            }
        } else if (param == 2) {
            if (read) {
                sgi_hal2_idr[0] = sgi_hal2_bres[gen][1];
                sgi_hal2_idr[1] = sgi_hal2_bres[gen][2];
            } else {
                sgi_hal2_bres[gen][1] = sgi_hal2_idr[0];
                sgi_hal2_bres[gen][2] = sgi_hal2_idr[1];
            }
        }
        return;
    }
    if (type == 0x9000 && num == 0x0100) {
        /* Global DMA masks the driver read-modify-writes, so they must
         * read back what was stored. */
        if (param == 1) {
            if (read) {
                sgi_hal2_idr[0] = sgi_hal2_dma_enable;
            } else {
                sgi_hal2_dma_enable = sgi_hal2_idr[0];
            }
        } else if (param == 3) {
            if (read) {
                sgi_hal2_idr[0] = sgi_hal2_dma_drive;
            } else {
                sgi_hal2_dma_drive = sgi_hal2_idr[0];
            }
        }
        return;
    }
}

/* PIO0 register file (MAME hal2.h:65-74); the PROM touches these as dwords. */
uint64_t sgi_hal2_read(unsigned offset)
{
    switch (offset) {
    case 0x04:
        return sgi_hal2_isr;
    case 0x08:
        return 0x4010;
    case 0x0c:
        return sgi_hal2_iar;
    case 0x10:
    case 0x14:
    case 0x18:
    case 0x1c:
        return sgi_hal2_idr[(offset - 0x10) / 4];
    default:
        return 0;
    }
}

void sgi_hal2_write(unsigned offset, uint16_t value)
{
    switch (offset) {
    case 0x04:
        /* Bits 2-4 are writable control; clearing a reset bit resets that
         * half of the chip (MAME hal2.cpp:108-114). */
        sgi_hal2_isr &= ~0x1c;
        sgi_hal2_isr |= value & 0x1c;
        if (!(value & 0x08)) {
            sgi_hal2_reset_state();
            sgi_hal2_isr = value & 0x1c;
        } else if (!(value & 0x10)) {
            sgi_hal2_dma_enable = 0;
            memset(sgi_hal2_codec, 0, sizeof(sgi_hal2_codec));
        }
        return;
    case 0x08:
        return;
    case 0x0c:
        sgi_hal2_iar_write(value);
        return;
    case 0x10:
    case 0x14:
    case 0x18:
    case 0x1c:
        sgi_hal2_idr[(offset - 0x10) / 4] = value;
        return;
    default:
        return;
    }
}

static void sgi_hal2_audio_drain(void)
{
    while (sgi_hal2_be && sgi_hal2_voice && sgi_hal2_fifo_count) {
        int16_t tmp[1024];
        unsigned first;
        unsigned n;
        size_t written;

        first = SGI_HAL2_FIFO_SIZE - sgi_hal2_fifo_head;
        n = sgi_hal2_fifo_count < first ? sgi_hal2_fifo_count : first;
        n = n < ARRAY_SIZE(tmp) ? n : ARRAY_SIZE(tmp);
        memcpy(tmp, &sgi_hal2_fifo[sgi_hal2_fifo_head], n * sizeof(int16_t));
        written = audio_be_write(sgi_hal2_be, sgi_hal2_voice, tmp,
                                 n * sizeof(int16_t));
        if (!written) {
            break;
        }
        written /= sizeof(int16_t);
        sgi_hal2_fifo_head = (sgi_hal2_fifo_head + written) % SGI_HAL2_FIFO_SIZE;
        sgi_hal2_fifo_count -= written;
    }
}

/* The backend pulls whatever the PBUS has queued since its last pull. */
static void sgi_hal2_audio_callback(void *opaque, int avail)
{
    (void)opaque;
    (void)avail;
    sgi_hal2_audio_drain();
}

/* The PBUS tick period already includes the channel count, so the stereo
 * voice runs at half the mono tick rate (MAME hal2.cpp:356-363). */
static void sgi_hal2_audio_emit(int16_t left, int16_t right)
{
    /* A stereo voice consumes whole frames; drop a frame on overrun
     * rather than splitting it across the ring. */
    if (sgi_hal2_fifo_count + 2 <= SGI_HAL2_FIFO_SIZE) {
        unsigned tail = (sgi_hal2_fifo_head + sgi_hal2_fifo_count) %
                        SGI_HAL2_FIFO_SIZE;

        sgi_hal2_fifo[tail] = left;
        sgi_hal2_fifo[(tail + 1) % SGI_HAL2_FIFO_SIZE] = right;
        sgi_hal2_fifo_count += 2;
    }
}

static void sgi_hal2_audio_push(int16_t sample, unsigned dac,
                                unsigned channel_count, uint64_t rate_ns)
{
    uint64_t samples_sec = 1000000000ULL / rate_ns;
    unsigned freq = (unsigned)(channel_count == 1 ? samples_sec :
                                                        samples_sec / 2);

    if (freq < 4000 || freq > 96000) {
        return;
    }
    if (!sgi_hal2_be) {
        if (sgi_hal2_audiodev) {
            Error *err = NULL;

            sgi_hal2_be = audio_be_by_name(sgi_hal2_audiodev, &err);
            if (!sgi_hal2_be) {
                if (err) {
                    error_report_err(err);
                }
                g_free(sgi_hal2_audiodev);
                sgi_hal2_audiodev = NULL;
                return;
            }
        } else {
            sgi_hal2_be = audio_get_default_audio_be(NULL);
            if (!sgi_hal2_be) {
                return;
            }
        }
    }
    if (!sgi_hal2_voice || sgi_hal2_voice_freq != freq) {
        struct audsettings as = {
            .freq = freq,
            .nchannels = 2,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };

        sgi_hal2_voice = audio_be_open_out(sgi_hal2_be, sgi_hal2_voice,
                                           "sgi-hal2", NULL,
                                           sgi_hal2_audio_callback, &as);
        if (!sgi_hal2_voice) {
            return;
        }
        sgi_hal2_voice_freq = freq;
        audio_be_set_active_out(sgi_hal2_be, sgi_hal2_voice, true);
    }
    if (dac == 0) {
        /* Left DAC; stereo waits for the right tick to complete the frame
         * (MAME hal2.cpp:365-378). Mono carries a whole frame per tick, so
         * the sample feeds both outputs (iris docs/hal2.md:160,
         * iris src/dev/hal2.rs:1191). */
        sgi_hal2_pending_left = sample;
        sgi_hal2_pending_valid = true;
        sgi_hal2_hold_left = sample;
        if (channel_count == 1) {
            sgi_hal2_audio_emit(sample, sample);
            sgi_hal2_pending_valid = false;
        }
    } else {
        /* Right DAC completes the frame and becomes the held value. */
        sgi_hal2_audio_emit(sgi_hal2_pending_valid ? sgi_hal2_pending_left :
                                                     sgi_hal2_hold_left,
                            sample);
        sgi_hal2_hold_right = sample;
        sgi_hal2_pending_valid = false;
    }
    sgi_hal2_audio_drain();
}

static uint64_t sgi_hal2_rate_ns(unsigned channel)
{
    unsigned codec;
    unsigned codec_channel;
    unsigned codec_clock;
    unsigned channel_count;
    unsigned clock_select;
    uint64_t frequency;
    uint64_t increment;
    uint64_t modulus;

    for (codec = 0; codec < 2; codec++) {
        codec_channel = sgi_hal2_codec[codec][0] & 3;
        codec_clock = (sgi_hal2_codec[codec][0] >> 3) & 3;
        channel_count = (sgi_hal2_codec[codec][0] >> 8) & 3;
        if (channel != codec_channel && channel != 3 - codec_channel) {
            continue;
        }
        if (!codec_clock || !channel_count) {
            continue;
        }

        clock_select = sgi_hal2_bres[codec_clock - 1][0];
        frequency = clock_select == 0 ? 48000 :
                    clock_select == 1 ? 44100 : 0;
        increment = sgi_hal2_bres[codec_clock - 1][1];
        if (!frequency || !channel_count) {
            return 0;
        }
        /* WHY: Bresenham divider re-derived from first principles
         * (iris src/dev/hal2.rs:401-409, iris docs/hal2.md:93-98): the
         * modulus is programmed as modctrl = (inc - mod - 1) & 0xFFFF, so
         * mod = (inc - modctrl - 1) & 0xFFFF and the generator ticks at
         * master * inc / mod. A zero modulus (e.g. inc=0/modctrl=0xFFFF,
         * the IRIX 44.1kHz programming) ticks on every master clock, and
         * inc >= mod saturates to the master too -- at most one tick per
         * master clock. Trace-proven PROM chime: BRES1 sel=1 (44.1kHz
         * master), inc=1, modctrl=0xFFFF -> mod=1 -> 44100Hz mono. */
        modulus = (increment - sgi_hal2_bres[codec_clock - 1][2] - 1U) &
                  0xFFFFU;
        if (!modulus || increment >= modulus) {
            return 1000000000ULL / (frequency * channel_count);
        }
        return (1000000000ULL * modulus) /
               (frequency * increment * channel_count);
    }
    return 0;
}

static void sgi_hal2_dma_write(unsigned channel, int16_t sample)
{
    unsigned dac;
    unsigned channel_count;
    uint64_t rate;

    if (channel >= 2) {
        return;
    }
    dac = sgi_hal2_curr_dac ? 1 : 0;
    sgi_hal2_dac[dac] = sample;
    sgi_hal2_dac_samples[dac]++;
    channel_count = (sgi_hal2_codec[0][0] >> 8) & 3;
    if (channel_count) {
        sgi_hal2_curr_dac++;
        if (sgi_hal2_curr_dac == channel_count) {
            sgi_hal2_curr_dac = 0;
        }
    }
    rate = sgi_hal2_rate_ns(channel);
    if (rate) {
        sgi_hal2_audio_push(sample, dac, channel_count, rate);
    }
}

static qemu_irq sgi_ioc2_enet_edlc_irq;
static qemu_irq sgi_ioc2_enet_hpc_irq;

static NICConf sgi_ethernet_conf;
/* WHY: qemu_deliver_packet_iov dereferences the NIC reentrancy guard on the
 * first inbound frame; a NULL guard segfaults. Other NICs pass their device
 * guard here (e.g. dp8393x passes &dev->mem_reentrancy_guard). */
static MemReentrancyGuard sgi_ethernet_guard;
static uint8_t sgi_edlc_station[6];
/* WHY: 80C03 bank-selected registers (iris seeq8003.rs:127-132). Station is
 * bank 0 only; bank 1 holds the multicast hash LSB, bank 2 the MSB plus
 * pktgap/ctl. Reads 0-5 always return collision counters/flags. */
static uint8_t sgi_edlc_mcast_lsb[6];
static uint8_t sgi_edlc_mcast_msb[2];
static uint8_t sgi_edlc_pktgap;
static uint8_t sgi_edlc_ctl;
static uint8_t sgi_edlc_rx_status = 0x80;
static uint8_t sgi_edlc_tx_status = 0x80;
static uint8_t sgi_edlc_rx_command;
static uint8_t sgi_edlc_tx_command;
static uint8_t sgi_edlc_rx_fifo[1518];
static size_t sgi_edlc_rx_length;
static size_t sgi_edlc_rx_pos;
static uint8_t sgi_edlc_tx_fifo[1518];
static size_t sgi_edlc_tx_length;

/* WHY: interrupt only on NEW (OLD clear) status whose bits are enabled
 * (iris seeq8003.rs:261-282, MAME edlc.cpp interrupt()). The level drops
 * when the guest reads the status register (OLD-marking below) or issues
 * the CLRINT strobe; nothing here latches, so the boot cannot wedge. */
static bool sgi_edlc_irq_level(void)
{
    return ((!(sgi_edlc_rx_status & 0x80) &&
             (sgi_edlc_rx_status & sgi_edlc_rx_command & 0x3f)) ||
            (!(sgi_edlc_tx_status & 0x80) &&
             (sgi_edlc_tx_status & sgi_edlc_tx_command & 0x0f)));
}

static void sgi_edlc_update_irq(void)
{
    qemu_set_irq(sgi_ioc2_enet_edlc_irq, sgi_edlc_irq_level());
}

static bool sgi_edlc_can_receive(NetClientState *nc)
{
    return (sgi_edlc_rx_status & 0x80) &&
           ((sgi_edlc_rx_command & 0xc0) != 0);
}

static ssize_t sgi_edlc_receive(NetClientState *nc, const uint8_t *buf,
                                size_t size)
{
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    /* WHY: a frame is already held (status NEW, iris seeq8003.rs:698 keeps
     * rx_stat NEW until the guest re-arms). Report unconsumed so the QEMU
     * net queue holds the frame for a later flush instead of dropping it. */
    if (!(sgi_edlc_rx_status & 0x80)) {
        return 0;
    }
    /* WHY: address filter (iris seeq8003.rs:316-329). MATCH_DISABLE drops;
     * any match bit set accepts station/broadcast (and multicast, which the
     * driver software-filters via ei_efix when not promiscuous). */
    if (size > sizeof(sgi_edlc_rx_fifo) || size < 6 ||
        (((sgi_edlc_rx_command & 0xc0) == 0 &&
          memcmp(buf, sgi_edlc_station, 6) != 0 &&
          memcmp(buf, broadcast, 6) != 0))) {
        return size;
    }
    memcpy(sgi_edlc_rx_fifo, buf, size);
    sgi_edlc_rx_length = size;
    sgi_edlc_rx_pos = 0;
    /* WHY: wire minimum is 60 bytes + 4 FCS; TAP strips the FCS, so pad
     * short frames to 60 and report GOOD for anything the wire would call
     * good (len + 4 >= 64, MAME edlc.cpp receive()). Without this every
     * 60-byte ARP reads SHORT and IRIX drops it for lack of SEQ_RS_GOOD.
     * Status stays NEW (iris seeq8003.rs:95) until the guest re-arms. */
    if (size < 60) {
        memset(sgi_edlc_rx_fifo + size, 0, 60 - size);
        sgi_edlc_rx_length = 60;
    }
    sgi_edlc_rx_status = 0x10 | (size < 60 ? 0x08 : 0x20);
    sgi_hpc3_enet_rx_ready = true;
    sgi_hpc3_enet_rx_ready_event();
    sgi_edlc_update_irq();
    sgi_hpc3_enet_update_irq();
    return size;
}

static NetClientInfo sgi_edlc_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = sgi_edlc_can_receive,
    .receive = sgi_edlc_receive,
};

static uint8_t sgi_edlc_read(hwaddr addr)
{
    /* WHY: IRIX maps the SEEQ registers as 32-bit words (IRIX seeq.h
     * seeq_reg: eaddr/csr/csx are u_int), so decode the word stride
     * (iris hpc3.rs:1649-1652: idx = (offset - SEEQ_BASE) >> 2). A
     * size-blind addr & 7 leaves rx_cmd at MATCH_DISABLE and the receiver
     * never enables. This also decodes byte-lane sb accesses correctly. */
    switch ((addr >> 2) & 7) {
    case 0 ... 5:
        /*
         * 80C03 regs 0-5 read back as collision counters/flags regardless
         * of bank (station address is write-only): coll_xmit 0 (so the
         * driver identifies an SGI EDLC), coll_total 0, fill 0, and flags
         * with NO_SQE set (carrier present, normal 10base-T, no SQE
         * heartbeat).  Returning the station MAC here misidentifies the
         * chip and, worse, its 0x02 bit reads as NO_CARRIER.
         */
        return ((addr >> 2) & 7) == 5 ? 0x01 : 0x00;
    case 6: {
        uint8_t value = sgi_edlc_rx_status;

        /* WHY: status-read re-arm (iris seeq8003.rs:476-481, MAME edlc.cpp
         * rx_status_r()): reading marks OLD and re-evaluates the line. */
        sgi_edlc_rx_status |= 0x80;
        sgi_edlc_update_irq();
        return value;
    }
    case 7: {
        uint8_t value = sgi_edlc_tx_status;

        sgi_edlc_tx_status |= 0x80;
        sgi_edlc_update_irq();
        return value;
    }
    default:
        return 0xff;
    }
}

static void sgi_edlc_write(hwaddr addr, uint8_t value)
{
    unsigned reg = (addr >> 2) & 7;

    switch (reg) {
    case 0 ... 5:
        /* WHY: bank routing honors the TXC bank bits (iris seeq8003.rs:500-524,
         * MAME edlc.cpp seeq80c03 map_reg bank). Station is bank 0 only, so a
         * 0x4F bank-select write parks later station writes in the multicast
         * bank instead of corrupting the MAC the RX filter matches. */
        switch (sgi_edlc_tx_command & 0x60) {
        case 0x00:
            sgi_edlc_station[reg] = value;
            break;
        case 0x20:
            sgi_edlc_mcast_lsb[reg] = value;
            break;
        case 0x40:
            switch (reg) {
            case 0:
            case 1:
                sgi_edlc_mcast_msb[reg] = value;
                break;
            case 2:
                sgi_edlc_pktgap = value;
                break;
            case 3:
                sgi_edlc_ctl = value;
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
        break;
    case 6:
        sgi_edlc_rx_command = value;
        sgi_edlc_update_irq();
        break;
    case 7:
        /* WHY: TXC bit 0x08 is INTGOOD enable, not a send strobe (IRIX seeq.h
         * SEQ_XC_INTGOOD; iris seeq8003.rs:530-534 latches tx_cmd only). The
         * true TX trigger is the HPC TX-DMA ACTIVE edge in sgi_hpc3_enet_tx_start. */
        sgi_edlc_tx_command = value;
        sgi_edlc_update_irq();
        break;
    }
}

static uint8_t sgi_edlc_fifo_read(void)
{
    if (sgi_edlc_rx_pos >= sgi_edlc_rx_length) {
        return 0;
    }
    return sgi_edlc_rx_fifo[sgi_edlc_rx_pos++];
}

static void sgi_edlc_fifo_write(uint8_t value)
{
    if (sgi_edlc_tx_length < sizeof(sgi_edlc_tx_fifo)) {
        sgi_edlc_tx_fifo[sgi_edlc_tx_length++] = value;
    }
}

static void sgi_edlc_reset(void)
{
    /* WHY: CH_RESET clears commands and marks both status OLD (iris
     * seeq8003.rs:446-456); station/filter programming survives reset. */
    sgi_edlc_rx_status = 0x80;
    sgi_edlc_tx_status = 0x80;
    sgi_edlc_rx_command = 0;
    sgi_edlc_tx_command = 0;
    sgi_edlc_rx_length = 0;
    sgi_edlc_rx_pos = 0;
    sgi_edlc_tx_length = 0;
    sgi_hpc3_enet_rx_active = false;
    sgi_hpc3_enet_rx_ready = false;
    sgi_hpc3_enet_dma.rx_ctrl = 0;
    sgi_hpc3_enet_dma.tx_ctrl = 0;
    sgi_edlc_update_irq();
    sgi_hpc3_enet_update_irq();
}

static void sgi_hpc3_enet_write16(hwaddr addr, uint16_t value)
{
    value = cpu_to_be16(value);
    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                        &value, sizeof(value));
}

static void sgi_hpc3_enet_fetch_rx_descriptor(void)
{
    sgi_hpc3_enet_dma.cbdp = sgi_hpc3_enet_dma.rx_nbdp;
    sgi_hpc3_enet_dma.cbp = sgi_hpc3_dma_read32(sgi_hpc3_enet_dma.cbdp);
    sgi_hpc3_enet_dma.bc = sgi_hpc3_dma_read32(sgi_hpc3_enet_dma.cbdp + 4);
    sgi_hpc3_enet_dma.rx_nbdp = sgi_hpc3_dma_read32(sgi_hpc3_enet_dma.cbdp + 8);
    if ((sgi_hpc3_enet_dma.bc & 0x3fff) >= 2) {
        sgi_hpc3_enet_dma.cbp += 2;
        sgi_hpc3_enet_dma.bc -= 2;
    }
}

/* WHY: pure level, never latched (iris: enet DMA channels carry no callback,
 * the SEEQ line is the only interrupt source). It clears itself when the
 * frame is delivered, the receiver is disabled, or RBO is set, so the guest
 * always has a clear path and the boot cannot wedge. */
static void sgi_hpc3_enet_update_irq(void)
{
    qemu_set_irq(sgi_ioc2_enet_hpc_irq,
                 sgi_hpc3_enet_rx_ready && sgi_hpc3_enet_rx_active &&
                 !(sgi_hpc3_enet_dma.rx_ctrl & 0x800));
}

static void sgi_hpc3_enet_rx_start(void)
{
    /* WHY: ACTIVE edge with AMASK clear (iris hpc3.rs:987-1001). A frame held
     * while the receiver was off is delivered now, and the TAP queue is
     * re-polled so frames that arrived while disabled are not lost. */
    if (!sgi_hpc3_enet_rx_active) {
        return;
    }
    if (sgi_ethernet_nic) {
        qemu_flush_queued_packets(qemu_get_queue(sgi_ethernet_nic));
    }
    if (sgi_hpc3_enet_rx_ready) {
        sgi_hpc3_enet_rx_ready_event();
    }
}

static void sgi_hpc3_enet_rx_ready_event(void)
{
    size_t remaining;

    if (sgi_hpc3_enet_rx_in_event) {
        return;
    }
    if (!sgi_hpc3_enet_rx_active || !sgi_hpc3_enet_rx_ready) {
        sgi_hpc3_enet_update_irq();
        return;
    }
    sgi_hpc3_enet_rx_in_event = true;
    /* WHY: one frame per descriptor (MAME hpc3.cpp enet_rxrdy_w advances
     * cbdp=nbdp and fetches on every frame; iris hpc3.rs:714-716 fetches on
     * bc_done). The driver tracks HW progress via crbdp, so packing frames
     * into a stale descriptor hides them. An empty ring holds the frame
     * until the driver reposts and re-enables. */
    if (!sgi_hpc3_enet_dma.rx_nbdp) {
        sgi_hpc3_enet_rx_in_event = false;
        sgi_hpc3_enet_update_irq();
        return;
    }
    sgi_hpc3_enet_fetch_rx_descriptor();
    /* WHY: RX DMA layout is [2 pad][frame][status] (iris seeq8003.rs:5-8);
     * the driver derives rlen from the desc+6 writeback and the status byte
     * at eh+rlen (IRIX if_ec2.c ecintr). */
    remaining = sgi_edlc_rx_length - sgi_edlc_rx_pos;
    if ((sgi_hpc3_enet_dma.bc & 0x3fff) < remaining + 1) {
        /* WHY: descriptor too small (MAME hpc3.cpp enet_rx_bc_dec fails):
         * report RBO in both rcvstat and ctl (IRIX ecintr reads hio->ctl &
         * HPC_RBO and hio->rcvstat & HPC_RBO, clears via ctl W1C) and stop
         * the channel. The held frame is dropped; CLRINT re-arms. */
        sgi_hpc3_enet_dma.rx_ctrl |= 0x800U;
        sgi_hpc3_enet_misc |= 0x800U;
        sgi_hpc3_enet_rx_active = false;
        sgi_hpc3_enet_dma.rx_ctrl &= ~0x200U;
        sgi_hpc3_enet_rx_ready = false;
        sgi_edlc_rx_length = 0;
        sgi_edlc_rx_pos = 0;
        sgi_hpc3_enet_rx_in_event = false;
        sgi_edlc_update_irq();
        sgi_hpc3_enet_update_irq();
        return;
    }
    while (remaining && (sgi_hpc3_enet_dma.bc & 0x3fff)) {
            uint8_t value = sgi_edlc_fifo_read();
        MemTxResult result;

        address_space_stb(&address_space_memory, sgi_hpc3_enet_dma.cbp++,
                          value, MEMTXATTRS_UNSPECIFIED, &result);
        sgi_hpc3_enet_dma.bc--;
        remaining--;
    }
    if (!remaining) {
        sgi_hpc3_enet_rx_ready = false;
        if (sgi_hpc3_enet_dma.bc & 0x3fff) {
            MemTxResult result;

            address_space_stb(&address_space_memory,
                              sgi_hpc3_enet_dma.cbp++, sgi_edlc_rx_status,
                              MEMTXATTRS_UNSPECIFIED, &result);
            sgi_hpc3_enet_dma.bc--;
        }
        /* WHY: RX writeback is the low 14-bit remainder so ROWN clears
         * and ecintr collects the frame (iris hpc3.rs:151 ROWN 0x4000;
         * iris hpc3.rs:682 rem = bc & 0x3FFF to crbdp+6). */
        sgi_hpc3_enet_write16(sgi_hpc3_enet_dma.cbdp + 6,
                              (uint16_t)(sgi_hpc3_enet_dma.bc & 0x3fff));
        sgi_edlc_rx_length = 0;
        sgi_edlc_rx_pos = 0;
        if (sgi_hpc3_enet_dma.bc & 0x80000000U) {
            sgi_hpc3_enet_rx_active = false;
            sgi_hpc3_enet_dma.rx_ctrl &= ~0x200U;
        }
        /* WHY: rx status stays NEW (iris seeq8003.rs:695-706) so the EDLC
         * line interrupts the guest; the guest re-arms with a status read
         * or the CLRINT strobe, either of which drops the level. */
        sgi_edlc_update_irq();
        if (sgi_ethernet_nic) {
            qemu_flush_queued_packets(qemu_get_queue(sgi_ethernet_nic));
        }
    }
    sgi_hpc3_enet_rx_in_event = false;
    sgi_hpc3_enet_update_irq();
}

/* WHY: the HPC TX-DMA ACTIVE edge is the only send trigger (iris
 * hpc3.rs:1052-1067, MAME hpc3.cpp hd_enet_w TXC_CA arming enet_transmit).
 * Each EOXP-flagged descriptor completes a packet: send it, report SUCCESS
 * (iris seeq8003.rs:678-691), and write TXD to the packet-FIRST descriptor
 * (iris hpc3.rs:703-708, MAME enet_intr_in_w BC_TXD to cpfbdp), which is what
 * IRIX clean_xd_chain inspects. ACTIVE clears at end of chain; the guest
 * re-kicks per packet, so no completion can pin a level. */
static void sgi_hpc3_enet_tx_start(void)
{
    uint32_t desc;
    uint32_t first;
    unsigned ndesc;

    desc = sgi_hpc3_enet_dma.tx_nbdp;
    if (!desc) {
        sgi_hpc3_enet_dma.tx_ctrl &= ~0x200U;
        sgi_hpc3_enet_update_irq();
        return;
    }
    /* WHY: cpfbdp/ppfxbdp promotion per packet (iris hpc3.rs:446-450). */
    sgi_hpc3_enet_dma.ppfbdp = sgi_hpc3_enet_dma.cpfbdp;
    sgi_hpc3_enet_dma.cpfbdp = desc;
    first = desc;
    sgi_edlc_tx_length = 0;
    ndesc = 0;
    while (desc && ndesc < 1024) {
        uint32_t buffer = sgi_hpc3_dma_read32(desc);
        uint32_t flags = sgi_hpc3_dma_read32(desc + 4);
        uint32_t next = sgi_hpc3_dma_read32(desc + 8);
        uint32_t count = flags & 0x3fff;

        for (uint32_t i = 0; i < count; i++) {
                MemTxResult result;
            uint8_t value = address_space_ldub(&address_space_memory,
                                               buffer + i,
                                               MEMTXATTRS_UNSPECIFIED,
                                               &result);
            sgi_edlc_fifo_write(value);
        }
        ndesc++;
        if (flags & 0x40000000U) {
            if (sgi_ethernet_nic) {
                qemu_send_packet(qemu_get_queue(sgi_ethernet_nic),
                                 sgi_edlc_tx_fifo, sgi_edlc_tx_length);
            }
            sgi_edlc_tx_length = 0;
            sgi_edlc_tx_status = 0x08;
            sgi_hpc3_enet_write16(first + 6, 0x8000);
            sgi_hpc3_enet_dma.ppfbdp = sgi_hpc3_enet_dma.cpfbdp;
            sgi_hpc3_enet_dma.cpfbdp = next;
            first = next;
            sgi_edlc_update_irq();
        }
        desc = next;
        if (flags & 0x80000000U) {
            break;
        }
    }
    sgi_hpc3_enet_dma.tx_nbdp = desc;
    sgi_hpc3_enet_dma.tx_ctrl &= ~0x200U;
    sgi_hpc3_enet_update_irq();
}

static uint64_t sgi_hpc3_misc_read(void *opaque, hwaddr addr, unsigned size)
{
    /* PIO window [0x10000,0x14000) is owned by the sgi-hpc3 device. */
    if (addr >= 0x11800 && addr < 0x11900) {
        return 0;
    }
    if (addr >= 0x14000 && addr < 0x15000) {
        return sgi_hpc3_dma_cfg[((addr - 0x14000) >> 9) & 7];
    }
    if (addr >= 0x15000 && addr < 0x16000) {
        return sgi_hpc3_pio_cfg[((addr - 0x15000) >> 8) & 15];
    }
    if (addr >= 0xc000 && addr < 0xc500) {
        return sgi_edlc_read(addr - 0xc000);
    }
    return 0;
}

static void sgi_hpc3_misc_write(void *opaque, hwaddr addr,
                             uint64_t val, unsigned size)
{
    if (sgi_ip22_trace_enabled()) {
        fprintf(stderr, "HPCMISC-W off=%05x val=%08x sz=%u\n",
                (unsigned)addr, (unsigned)val, size);
    }
    /* PIO window [0x10000,0x14000) is owned by the sgi-hpc3 device. */
    if (addr >= 0x11800 && addr < 0x11900) {
        return;
    }
    if (addr >= 0x14000 && addr < 0x15000) {
        unsigned channel = ((addr - 0x14000) >> 9) & 7;

        sgi_hpc3_dma_cfg[channel] = val;
        sgi_hpc3_pbus_dma[channel].config = val;
        return;
    }
    if (addr >= 0x15000 && addr < 0x16000) {
        sgi_hpc3_pio_cfg[((addr - 0x15000) >> 8) & 15] = val;
        if (sgi_ip22_trace_enabled()) {
            fprintf(stderr, "PIO-CFG ch=%u val=%08x\n",
                    (unsigned)(((addr - 0x15000) >> 8) & 15),
                    (unsigned)val);
        }
        return;
    }
    if (addr >= 0xc000 && addr < 0xc500) {
        sgi_edlc_write(addr - 0xc000, val);
        return;
    }
    if (addr >= 0x15000 && addr < 0x16000) {
        sgi_hpc3_pio_regs[addr - 0x15000] = val;
    }
}

static const MemoryRegionOps sgi_hpc3_misc_ops = {
    .read = sgi_hpc3_misc_read,
    .write = sgi_hpc3_misc_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* The backend only finalizes an open voice when it is closed, which never
 * happens on the quit path, so close it on VM stop (a later push reopens). */
static void sgi_hal2_vm_state(void *opaque, bool running, RunState state)
{
    (void)opaque;
    (void)state;
    if (!running && sgi_hal2_voice) {
        audio_be_close_out(sgi_hal2_be, sgi_hal2_voice);
        sgi_hal2_voice = NULL;
        sgi_hal2_voice_freq = 0;
    }
}

static void sgi_indigo2_board_reset(void *opaque)
{
    /*
     * Board glue here is file-global static state, so reset it explicitly:
     * timers cancelled, status to power-on, IRQs recomputed to idle.
     * The station address is NIC configuration, not guest state, so it
     * is preserved.
     */
    (void)opaque;
    sgi_hpc3_intstat = 0;
    sgi_hpc3_misc = 0;
    memset(&sgi_hpc3_enet_dma, 0, sizeof(sgi_hpc3_enet_dma));
    sgi_hpc3_enet_misc = 0;
    sgi_hpc3_enet_rx_active = false;
    sgi_hpc3_enet_rx_ready = false;
    for (unsigned i = 0; i < ARRAY_SIZE(sgi_hpc3_pbus_dma); i++) {
        if (sgi_hpc3_pbus_dma[i].timer) {
            timer_del(sgi_hpc3_pbus_dma[i].timer);
        }
        sgi_hpc3_pbus_dma[i].current = 0;
        sgi_hpc3_pbus_dma[i].descriptor = 0;
        sgi_hpc3_pbus_dma[i].next = 0;
        sgi_hpc3_pbus_dma[i].flags = 0;
        sgi_hpc3_pbus_dma[i].bytes_left = 0;
        sgi_hpc3_pbus_dma[i].control = 0;
        sgi_hpc3_pbus_dma[i].config = 0;
    }
    memset(sgi_hpc3_dma_cfg, 0, sizeof(sgi_hpc3_dma_cfg));
    memset(sgi_hpc3_pio_cfg, 0, sizeof(sgi_hpc3_pio_cfg));
    memset(sgi_hpc3_pio_regs, 0, sizeof(sgi_hpc3_pio_regs));
    sgi_hal2_reset_state();
    sgi_edlc_reset();
    sgi_hpc3_enet_update_irq();
}

static void mips_sgi_indigo2_init(MachineState *machine)
{
    const char *bios_name = TARGET_BIG_ENDIAN ? "sgi_bios.bin" : "sgiel_bios.bin";
    MemoryRegion *address_space = get_system_memory();
    char *filename;
    int bios_size;
    Clock *cpuclk;
    MIPSCPU *cpu;
    MIPSCPUClass *mcc;
    CPUMIPSState *env;
    MemoryRegion *isa_mem = g_new(MemoryRegion, 1);
    MemoryRegion *isa_io = g_new(MemoryRegion, 1);
    MemoryRegion *bios = g_new(MemoryRegion, 1);
    DeviceState *dev;
    DeviceState *ioc2dev;
    DeviceState *mcdev;
    SysBusDevice *sysbus;
    MMIOKBDState *i8042;
    MemoryRegion *hpc_pbus_dma = g_new(MemoryRegion, 1);
    MemoryRegion *hpc_misc = g_new(MemoryRegion, 1);
    MemoryRegion *eisa_io = g_new(MemoryRegion, 1);
    MemoryRegion *vino_stub = g_new(MemoryRegion, 1);
    MemoryRegion *i8042_alias = g_new(MemoryRegion, 1);
    NICInfo *nd;
    DeviceState *rc4030;
    rc4030_dma *dmas;
    IOMMUMemoryRegion *rc4030_dma_mr;
    DeviceState *scsi_ors[2];
    int n;

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    /* Indy (IP24) is an R4000 at 200MHz (MAME indy_4020); Indigo2 (IP22)
     * runs its R4400-class CPU at 150MHz with a stock R4000 stand-in. */
    bool is_indy = !!object_dynamic_cast(OBJECT(machine),
                                        MACHINE_TYPE_NAME("indy"));
    clock_set_hz(cpuclk, is_indy ? 200000000ULL : 150000000ULL);

    g_free(sgi_hal2_audiodev);
    sgi_hal2_audiodev = machine->audiodev ? g_strdup(machine->audiodev) : NULL;

    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk, TARGET_BIG_ENDIAN);
    env = &cpu->env;
    qemu_register_reset(main_cpu_reset, cpu);
    qemu_register_reset(sgi_indigo2_board_reset, NULL);
    qemu_add_vm_change_state_handler(sgi_hal2_vm_state, NULL);

    mcc = MIPS_CPU_GET_CLASS(cpu);
    mcc->no_data_aborts = true;

    memory_region_add_subregion(address_space, SGI_MEM_BASE, machine->ram);
    memory_region_init_io(eisa_io, NULL, &eisa_io_ops, NULL,
                          "sgi-ip22-eisa-io", 0x20000);
    if (is_indy) {
        /* IP24 maps VINO video capture at 0x00080000 (MAME ip24_map); no
         * VINO model exists yet, so answer all-ones (no device) there. */
        memory_region_init_io(vino_stub, NULL, &eisa_io_ops, NULL,
                              "sgi-ip24-vino-stub", 0x20000);
        memory_region_add_subregion(address_space, 0x00080000, vino_stub);
    } else {
        memory_region_add_subregion(address_space, 0x00080000, eisa_io);
    }
    for (n = 0; n < ARRAY_SIZE(sgi_hpc3_pbus_dma); n++) {
        sgi_hpc3_pbus_dma[n].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                   sgi_hpc3_pbus_tick,
                                                   &sgi_hpc3_pbus_dma[n]);
    }
    /*
     * No fixed VDMA alias: the MC maps PROM-programmed banks from `-m`;
     * a hardcoded window would back uninstalled RAM and break the probe.
     */

    dev = qdev_new(TYPE_SGI_MC);
    object_property_set_link(OBJECT(dev), "ram", OBJECT(machine->ram),
                             &error_fatal);
    /* Indy (IP24) has no EISA; reprograms MC System ID bit 4 (MAME mc). */
    qdev_prop_set_bit(dev, "eisa-present", !is_indy);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    mcdev = dev;
    memory_region_add_subregion(address_space, SGI_MC_BASE,
                                sysbus_mmio_get_region(sysbus, 0));

    /*
     * PROM at 0x1fc00000 only (MAME ip24_base_map:234 maps user1 once,
     * read-only, there). The old 0xfff00000 mirror let the CPU execute
     * the PROM from a second address, so the PROM's own relocation and
     * pointer patching diverged from MAME.
     */
    memory_region_init_rom(bios, NULL, "sgi_indigo2.bios", INDIGO2_BIOS_SIZE, &error_fatal);
    memory_region_add_subregion(address_space, 0x1fc00000ULL, bios);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware ?: bios_name);
    if (filename) {
        bios_size = load_image_targphys(filename, 0x1fc00000ULL, INDIGO2_BIOS_SIZE, NULL);
        g_free(filename);
    } else {
        bios_size = -1;
    }
    if ((bios_size < 0 || bios_size > INDIGO2_BIOS_SIZE) && machine->firmware && !qtest_enabled()) {
        error_report("Could not load SGI bios '%s'", machine->firmware);
        exit(1);
    }

    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    rc4030 = rc4030_init(&dmas, &rc4030_dma_mr);
    sysbus = SYS_BUS_DEVICE(rc4030);
    /* No rc4030 on IP22 (MAME has none): it is a QEMU DMA backend only.
     * Its timer/jazz IRQs stay unconnected so they can never raise IP6/IP3,
     * which on real hardware belong to other sources. */
    /* IOC2 (PIO6 base + PIO4 INT2) drives CPU INT0/INT1 directly, as in
     * MAME. The ESCC/i8042 sub-windows stay on top of PIO6; the rc4030 is
     * only a QEMU DMA backend and must not gate IP22 interrupts. */
    /* Indy (IP24) uses the GUINNESS IOC2: interrupt registers in the PIO6
     * window (MAME ioc2_guinness_device::map), no PIO4 INT2 window.
     * Indigo2 (IP22) uses FULL_HOUSE with the separate INT2 window. */
    dev = qdev_new(TYPE_SGI_IOC2);
    qdev_prop_set_bit(dev, "guinness", is_indy);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    /*
     * MAME indy_indigo2.cpp:258-261 pio6_map:
     *   map(0x00, 0x2f).m("ioc2", ioc2_guinness_device::map)
     *                    .umask16(0x00ff);
     * installed on AS_PIO6 (indy_indigo2.cpp:381).  MAME reaches the
     * device through AS_PIO6, which hpc3.cpp's 32-bit pio_data_r() drives
     * as `read_word(offset & 0xff, 0x00ff) & 0xff`; QEMU has no separate
     * pio6 address space, so the PIO window dispatcher decodes the same
     * dword index and calls into the IOC2 device itself.  Handing that
     * device to the dispatcher (rather than mapping sub-windows on top
     * of it) is what keeps the register index and the byte lanes
     * consistent: MAME's window is dword-indexed, and the value only
     * ever lives in the low lane of each dword.
     */
    sgi_hpc3_set_ioc2(DEVICE(dev));
    if (!is_indy) {
        memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x58614,
                                            sysbus_mmio_get_region(sysbus, 0), 5);
        memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x58400,
                                            sysbus_mmio_get_region(sysbus, 1), 5);
    }
    /* MAME ioc2 raise_local_irq(channel) drives m_maincpu->set_input_line(channel);
     * MAME mips3 maps line N to Cause bit 10+N, i.e. channel 0 -> IP2 and
     * channel 1 -> IP3, matching QEMU env->irq[2]/env->irq[3]. */
    qdev_connect_gpio_out(DEVICE(dev), 0, env->irq[2]);
    qdev_connect_gpio_out(DEVICE(dev), 1, env->irq[3]);
    /* MAME ioc2 timer0_int/timer1_int drive CPU lines 2/3 (IP4/IP5). */
    qdev_connect_gpio_out(DEVICE(dev), 2, env->irq[4]);
    qdev_connect_gpio_out(DEVICE(dev), 3, env->irq[5]);
    sgi_ioc2_enet_edlc_irq = qdev_get_gpio_in(DEVICE(dev), 2);
    sgi_ioc2_enet_hpc_irq = qdev_get_gpio_in(DEVICE(dev), 3);
    ioc2dev = dev;
    /* MAME indy_indigo2.cpp:318: MEMCTL DMA-done to IOC2 local0 0x10. */
    qdev_connect_gpio_out(mcdev, 0,
                            qdev_get_gpio_in(ioc2dev, SGI_IOC2_IN_MC_DMA));
    /* Single GIO64 owner (MAME gio64_device); rc4030 MMIO must not shadow it. */
    dev = qdev_new("sgi-gio64");
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    qdev_connect_gpio_out_named(dev, "gio", 0,
                                qdev_get_gpio_in(ioc2dev, 4));
    qdev_connect_gpio_out_named(dev, "gio", 1,
                                qdev_get_gpio_in(ioc2dev, 5));
    qdev_connect_gpio_out_named(dev, "gio", 2,
                                qdev_get_gpio_in(ioc2dev, 6));
    memory_region_add_subregion(address_space, SGI_GIO64_BASE,
                                sysbus_mmio_get_region(sysbus, 0));

    memory_region_add_subregion(address_space, SGI_HPC3_BASE,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(rc4030), 1));
    memory_region_init_io(hpc_pbus_dma, NULL, &sgi_hpc3_pbus_dma_ops,
                          NULL, "sgi-ip22-hpc-pbus-dma", 0x3f000);
    memory_region_add_subregion(address_space, SGI_HPC3_BASE + 0x1000,
                                hpc_pbus_dma);
    memory_region_init_io(hpc_misc, NULL, &sgi_hpc3_misc_ops,
                          NULL, "sgi-ip22-hpc-misc", 0x18000);
    memory_region_add_subregion(address_space, SGI_HPC3_BASE + 0x48000,
                                hpc_misc);

    memory_region_init(isa_io, NULL, "isa-io", SGI_IO_SIZE);
    memory_region_init(isa_mem, NULL, "isa-mem", 0x01000000);
    memory_region_add_subregion(address_space, SGI_IO_BASE, isa_io);
    memory_region_add_subregion(address_space, SGI_IO_BASE + 0x01000000ULL, isa_mem);

    /*
     * The WD33C93 owns its SCSI bus and drives it itself, so the machine no
     * longer instantiates a separate ESP backend and links it in.  That also
     * removes the reason the Indigo2 path had to re-buffer INT (the ESP level
     * held across transfers); the gate is kept so the IOC2 connection is
     * unchanged from the wiring that is known to boot.
     */
    if (!is_indy) {
        DeviceState *scsi_or = qdev_new(TYPE_OR_IRQ);
        qdev_prop_set_uint16(scsi_or, "num-lines", 2);
        qdev_realize_and_unref(scsi_or, NULL, &error_fatal);
        qdev_connect_gpio_out(scsi_or, 0, qdev_get_gpio_in(ioc2dev, 0));
        scsi_ors[0] = scsi_or;
    }
    dev = qdev_new("sgi-wd33c93");
    sgi_wd33c93[0] = SGI_WD33C93(dev);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    sysbus_mmio_map(sysbus, 0, SGI_SCSI_BASE);

    if (is_indy) {
        qdev_connect_gpio_out(DEVICE(sgi_wd33c93[0]), 0,
                              qdev_get_gpio_in(ioc2dev, 0));
    } else {
        /* MAME indy_indigo2.cpp:296: the WD33C93 INT alone drives IOC2 SCSI0. */
        qdev_connect_gpio_out(DEVICE(sgi_wd33c93[0]), 0,
                              qdev_get_gpio_in(scsi_ors[0], 0));
    }

    sgi_wd33c93_scsi_bus_init(sgi_wd33c93[0]);

    /* Indy (IP24) has a single SCSI channel (MAME ip24 has no scsibus2);
     * Indigo2 has a second, initially empty, channel at HPC3 HD1. */
    if (!is_indy) {
        /* MAME indy_indigo2.cpp:440: WD33C93 INT drives IOC2 SCSI1 (ioc2.cpp:738). */
        {
            DeviceState *scsi_or = qdev_new(TYPE_OR_IRQ);
            qdev_prop_set_uint16(scsi_or, "num-lines", 2);
            qdev_realize_and_unref(scsi_or, NULL, &error_fatal);
            qdev_connect_gpio_out(scsi_or, 0, qdev_get_gpio_in(ioc2dev, 1));
            scsi_ors[1] = scsi_or;
        }
        dev = qdev_new("sgi-wd33c93");
        sgi_wd33c93[1] = SGI_WD33C93(dev);
        sysbus = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sysbus, &error_fatal);
        memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x48000,
                                            sysbus_mmio_get_region(sysbus, 0), 2);

        qdev_connect_gpio_out(DEVICE(sgi_wd33c93[1]), 0,
                              qdev_get_gpio_in(scsi_ors[1], 0));
    }

    /* MAME indy_indigo2.cpp:359: HPC3 DMA-done to IOC2 local1 0x10. */
    {
        DeviceState *dma_or = qdev_new(TYPE_OR_IRQ);
        qdev_prop_set_uint16(dma_or, "num-lines", 3);
        qdev_realize_and_unref(dma_or, NULL, &error_fatal);
        qdev_connect_gpio_out_named(DEVICE(sgi_wd33c93[0]), "dma-done", 0,
                                    qdev_get_gpio_in(dma_or, 0));
        if (!is_indy) {
            qdev_connect_gpio_out_named(DEVICE(sgi_wd33c93[1]), "dma-done", 0,
                                        qdev_get_gpio_in(dma_or, 1));
        }
        /* PBUS (audio) descriptor-done joins the same dma_complete level
         * (MAME do_pbus_dma bit 29); the OR reproduces intstat!=0. */
        sgi_hpc3_pbus_done_irq = qdev_get_gpio_in(dma_or, 2);
        qdev_connect_gpio_out(dma_or, 0, qdev_get_gpio_in(ioc2dev, 9));
    }

    dev = qdev_new(TYPE_SGI_HPC3_SCSI);
    object_property_set_link(OBJECT(dev), "wd0", OBJECT(sgi_wd33c93[0]),
                             &error_fatal);
    if (!is_indy) {
        object_property_set_link(OBJECT(dev), "wd1", OBJECT(sgi_wd33c93[1]),
                                 &error_fatal);
    }
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x10000,
                                        sysbus_mmio_get_region(sysbus, 0), 30);
    memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x28000,
                                        sysbus_mmio_get_region(sysbus, 1), 30);
    memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x12000,
                                        sysbus_mmio_get_region(sysbus, 2), 30);
    memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x2a000,
                                        sysbus_mmio_get_region(sysbus, 3), 30);

    /*
     * HPC3 windows: PIO, BBRAM (DS1386 RTC+NVRAM), EEPROM (93C56).
     * Realize before the i8042 below: QEMU input dispatch is
     * first-registered-wins, and the PROM reads its keyboard through HPC3,
     * never through the 8042.
     */
    dev = qdev_new(TYPE_SGI_HPC3);
    qdev_prop_set_chr(dev, "chr", serial_hd(2));
    /* The Indy modelled here is MAME indy_4020: R4000 at 200MHz (4x50MHz),
     * ip24() with DS1386_8K, 16KiB icache/dcache, 50MHz system clock.  Its
     * DS1386_8K gives the 8KiB BBRAM below; the IP22 Indigo2 uses a 64-byte
     * DS1286.  NVRAM seed dir is the MAME machine name, so indy_4020 here. */
    qdev_prop_set_uint32(dev, "bbram-size", is_indy ? 0x2000 : 64);
    qdev_prop_set_string(dev, "nvram-dir", is_indy ? "indy_4020"
                                                   : "indigo2_4415");
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x58000,
                                        sysbus_mmio_get_region(sysbus, 0), 1);
    memory_region_add_subregion(address_space, SGI_BBRAM_BASE,
                                sysbus_mmio_get_region(sysbus, 1));
    memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x30008,
                                        sysbus_mmio_get_region(sysbus, 2), 1);

    /* After sgi-hpc3 (see above): first-registered input handler wins. */
    i8042 = I8042_MMIO(qdev_new(TYPE_I8042_MMIO));
    qdev_prop_set_uint64(DEVICE(i8042), "mask", 1);
    qdev_prop_set_uint32(DEVICE(i8042), "size", 0x1000);
    /*
     * Indy has a keyboard physically attached (like MAME's always-
     * present virtual keyboard, KEYBOARD_ON=1): report its power-on
     * BAT so presence detection sees a keyboard, not an empty port.
     * Same for the PS/2 mouse (MAME hle_mouse.cpp enqueues BAT
     * 0xAA/0x00 at reset): the PROM waits for mouse BAT with status
     * OBF+AUX and declares no mouse after ~500K polls without it.
     */
    qdev_prop_set_bit(DEVICE(i8042), "keyboard-bat", is_indy);
    qdev_prop_set_bit(DEVICE(i8042), "mouse-bat", is_indy);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(i8042), &error_fatal);

    /* MAME ioc2.cpp kbdc_irq merger: kbd+aux IRQs OR into one line. */
    {
        DeviceState *kbdc_or = qdev_new(TYPE_OR_IRQ);
        qdev_prop_set_uint16(kbdc_or, "num-lines", 2);
        qdev_realize_and_unref(kbdc_or, NULL, &error_fatal);
        qdev_connect_gpio_out(DEVICE(i8042), I8042_KBD_IRQ,
                              qdev_get_gpio_in(kbdc_or, 0));
        qdev_connect_gpio_out(DEVICE(i8042), I8042_MOUSE_IRQ,
                              qdev_get_gpio_in(kbdc_or, 1));
        qdev_connect_gpio_out(kbdc_or, 0, qdev_get_gpio_in(ioc2dev, 8));
    }

    memory_region_add_subregion(address_space, 0x80005000,
                                 sysbus_mmio_get_region(SYS_BUS_DEVICE(i8042), 0));
    /*
     * PIO6 0x10/0x11 reach the controller through the window dispatcher
     * (sgi_hpc3_set_pio6_kbd), not through region aliases: the guest polls
     * these registers from byte lane 3, which a byte-sized alias misses and
     * a wider one cannot express because pckbd.c derives data-vs-status from
     * `addr & mask`.  See sgi_hpc3_pio6_kbd_reg().
     */
    sgi_hpc3_set_pio6_kbd(DEVICE(i8042));
    if (!is_indy) {
        memory_region_init_alias(i8042_alias, NULL, "sgi_ip22.i8042_pio6",
                                 sysbus_mmio_get_region(SYS_BUS_DEVICE(i8042), 0),
                                 0, 2);
        memory_region_add_subregion_overlap(address_space, SGI_HPC3_BASE + 0x58610,
                                            i8042_alias, 20);
    }

    /* IOC2-compatible Z8530 SCC in HPC3 PIO6. */
    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "frequency", 10000000);
    qdev_prop_set_uint32(dev, "it_shift", is_indy ? 2 : 0);
    qdev_prop_set_bit(dev, "bit_swap", false);
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_uint32(dev, "chnAtype", 0);
    qdev_prop_set_uint32(dev, "chnBtype", 0);
    sysbus = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sysbus, &error_fatal);
    memory_region_add_subregion_overlap(address_space,
                                        SGI_HPC3_BASE +
                                        (is_indy ? 0x59830 : 0x5860c),
                                        sysbus_mmio_get_region(sysbus, 0), 1000);
    sysbus_connect_irq(sysbus, 0, qdev_get_gpio_in(ioc2dev, 7));

    nd = qemu_find_nic_info("seeq80c03", true, "seeq80c03");
    if (nd) {
        sgi_ethernet_conf.macaddr = nd->macaddr;
        sgi_ethernet_conf.peers.ncs[0] = nd->netdev;
        sgi_ethernet_conf.peers.queues = nd->netdev ? 1 : 0;
        memcpy(sgi_edlc_station, nd->macaddr.a, sizeof(sgi_edlc_station));
        sgi_ethernet_nic = qemu_new_nic(&sgi_edlc_net_info,
                                        &sgi_ethernet_conf,
                                        "seeq80c03", "indigo2",
                                        &sgi_ethernet_guard,
                                        NULL);
        nd->instantiated = 1;
        qemu_format_nic_info_str(qemu_get_queue(sgi_ethernet_nic),
                                 sgi_ethernet_conf.macaddr.a);
    }

    g_free(dmas);
}

static void mips_sgi_indigo2_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Indigo2";
    mc->init = mips_sgi_indigo2_init;
    mc->block_default_type = IF_SCSI;
    mc->no_cdrom = 1;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R4000");
    mc->default_ram_id = "sgi_indigo2.ram";
}

static void mips_sgi_indy_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI Indy";
    mc->init = mips_sgi_indigo2_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R4000SC");
    mc->default_ram_size = 128 * MiB;
    mc->default_ram_id = "sgi_indy.ram";
    machine_add_audiodev_property(mc);
}

static const TypeInfo mips_sgi_indigo2_type = {
    .name = MACHINE_TYPE_NAME("indigo2"),
    .parent = TYPE_MACHINE,
    .class_init = mips_sgi_indigo2_class_init,
};

static const TypeInfo mips_sgi_indy_type = {
    .name = MACHINE_TYPE_NAME("indy"),
    .parent = TYPE_MACHINE,
    .class_init = mips_sgi_indy_class_init,
};

static void mips_sgi_indigo2_machine_init(void)
{
    type_register_static(&mips_sgi_indigo2_type);
    type_register_static(&mips_sgi_indy_type);
}

type_init(mips_sgi_indigo2_machine_init)

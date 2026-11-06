/*
 * SGI Memory Controller (IP22) with DMA engine: control/watchdog/RPSS/
 * arbiter/memcfg registers, 4-entry DMA TLB, and the DMA engine with
 * completion timer. The first status read after a start completes the
 * transfer synchronously, so the PROM always observes started-then-done.
 * GPIO output 0 is DMA-done (MAME int_dma_done_cb parity); the PROM polls
 * and leaves it unconnected on IP22.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/mips/sgi_mc.h"
#include "hw/mips/sgi_hpc3.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define SGI_MC_IOMEM_SIZE 0x20000

typedef struct SgiMcState SgiMcState;

struct SgiMcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion *ram;
    /* MAME mc.cpp: sys_id = 0x03 (rev C) | eisa<<4. Indy has no EISA. */
    bool eisa_present;
    qemu_irq dma_done;
    QEMUTimer *dma_timer;
    uint32_t cpu_control;
    uint32_t watchdog;
    uint32_t gio_arb_param;
    uint32_t arb_cpu_time;
    uint32_t arb_burst_time;
    uint32_t cpu_mem_access_config;
    uint32_t gio_mem_access_config;
    uint32_t rpss_divider;
    uint32_t rpss_counter;
    uint64_t rpss_last_ns;
    uint32_t memcfg[2];
    uint32_t simms;
    uint8_t mc_rev;
    uint32_t dma_regs[6];
    uint32_t dma_run;
    bool dma_run_unread;
    uint32_t dma_mem_addr;
    uint32_t dma_size;
    uint32_t dma_stride;
    uint32_t dma_gio_addr;
    uint32_t dma_mode;
    uint32_t dma_count;
    uint32_t dma_tlb_hi[4];
    uint32_t dma_tlb_lo[4];
    uint32_t dma_int_cause;
    uint32_t dma_control;
    /* MC bank aliases owned by this device (deleted on memcfg rewrite).
     * Several mirrors per rank: an oversized configured window mirrors the
     * installed bytes, which the PROM alias probe relies on to size RAM. */
#define SGI_MC_MIRRORS 128
    MemoryRegion *bank_alias[2][2][2][SGI_MC_MIRRORS];
    MemoryRegion *low_alias[2];
};

#define SGI_MC(obj) OBJECT_CHECK(SgiMcState, (obj), TYPE_SGI_MC)

/*
 * RPSS free-running counter (MAME sgi_mc_device::update_count): the divider
 * prescales the 50MHz clock. Advances with virtual time, not with reads.
 */
#define SGI_MC_CLOCK_HZ 50000000ULL

static void sgi_mc_rpss_update(SgiMcState *state)
{
    uint32_t divide = (state->rpss_divider & 0xff) + 1;
    uint32_t increment = (state->rpss_divider >> 8) & 0xff;
    uint64_t freq = SGI_MC_CLOCK_HZ / divide;
    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed_ns;
    uint64_t ticks;

    if (now_ns < state->rpss_last_ns) {
        state->rpss_last_ns = now_ns;
        return;
    }
    elapsed_ns = now_ns - state->rpss_last_ns;
    if (!freq || !increment || !elapsed_ns) {
        return;
    }
    ticks = elapsed_ns * freq / 1000000000ULL;
    if (!ticks) {
        return;
    }
    state->rpss_last_ns += ticks * 1000000000ULL / freq;
    state->rpss_counter += (uint32_t)(ticks * increment);
}

/* DMA_CAUSE bits (iris mc_vdma.rs). */
#define MC_DMA_CAUSE_FAULT     0x01
#define MC_DMA_CAUSE_TLB_MISS  0x02
#define MC_DMA_CAUSE_CLEAN     0x04
#define MC_DMA_CAUSE_COMPLETE  0x08
#define MC_DMA_CTL_IE          (1U << 4)
#define MC_DMA_CTL_XLATE       (1U << 8)

/*
 * iris mc_vdma.rs dma_xlate_walk: translate one DMA virtual address through
 * the 4-entry uTLB and the page table it names. Without XLATE the address is
 * physical. Returns false with *cause set on a fault; the caller aborts.
 */
static bool sgi_mc_dma_xlate(SgiMcState *state, uint32_t vaddr, bool writing,
                             hwaddr *pa, uint32_t *cause)
{
    uint32_t ctl = state->dma_control;
    bool page_16k;
    bool pte_8byte;
    unsigned page_shift;
    uint32_t page_mask;
    unsigned pte_shift;
    unsigned entry;
    uint32_t pte_base;
    uint32_t vpn_lo;
    uint32_t pte;
    uint8_t pte_bytes[8];
    uint64_t pte64;

    if (!(ctl & MC_DMA_CTL_XLATE)) {
        *pa = vaddr;
        return true;
    }
    /* GIO CTL[1]: page size (0=4KB, 1=16KB). CTL[0]: PTE size (0=4B, 1=8B). */
    page_16k = (ctl & 0x2) != 0;
    pte_8byte = (ctl & 0x1) != 0;
    page_shift = page_16k ? 14 : 12;
    page_mask = page_16k ? 0x3fff : 0xfff;
    pte_shift = pte_8byte ? 3 : 2;

    for (entry = 0; entry < ARRAY_SIZE(state->dma_tlb_hi); entry++) {
        if ((vaddr & 0xffc00000) == (state->dma_tlb_hi[entry] & 0xffc00000)) {
            break;
        }
    }
    if (entry == ARRAY_SIZE(state->dma_tlb_hi)) {
        *cause = MC_DMA_CAUSE_TLB_MISS;
        return false;
    }
    if (!(state->dma_tlb_lo[entry] & 0x2)) {
        *cause = MC_DMA_CAUSE_TLB_MISS;
        return false;
    }
    pte_base = (state->dma_tlb_lo[entry] & 0x03ffffc0) << 6;
    vpn_lo = (vaddr & 0x003fffff) >> page_shift;
    if (!address_space_access_valid(&address_space_memory,
                                    pte_base + (vpn_lo << pte_shift),
                                    1 << pte_shift, false,
                                    MEMTXATTRS_UNSPECIFIED)) {
        *cause = MC_DMA_CAUSE_FAULT;
        return false;
    }
    if (pte_8byte) {
        address_space_read(&address_space_memory,
                           pte_base + (vpn_lo << pte_shift),
                           MEMTXATTRS_UNSPECIFIED, pte_bytes, 8);
        pte64 = ldq_be_p(pte_bytes);
        pte = (uint32_t)pte64;
    } else {
        address_space_read(&address_space_memory,
                           pte_base + (vpn_lo << pte_shift),
                           MEMTXATTRS_UNSPECIFIED, pte_bytes, 4);
        pte = ldl_be_p(pte_bytes);
    }
    if (!(pte & 0x2)) {
        *cause = MC_DMA_CAUSE_FAULT;
        return false;
    }
    if (writing && !(pte & 0x4)) {
        *cause = MC_DMA_CAUSE_CLEAN;
        return false;
    }
    *pa = ((hwaddr)(pte & 0x03ffffc0) << 6) | (vaddr & page_mask);
    return true;
}

/* iris: a fault aborts the transfer, leaves mem_vaddr at the faulting
 * address and raises the cause (and the interrupt when IE is set). */
static void sgi_mc_dma_abort(SgiMcState *state, uint32_t fault_addr,
                             uint32_t cause)
{
    state->dma_mem_addr = fault_addr;
    state->dma_count = 0;
    state->dma_run &= ~0x40;
    state->dma_timer = NULL;
    state->dma_int_cause |= cause;
    if (state->dma_control & MC_DMA_CTL_IE) {
        qemu_set_irq(state->dma_done, 1);
    }
}

static void sgi_mc_dma_complete(void *opaque)
{
    SgiMcState *state = SGI_MC(opaque);
    /* One transfer per start: the DMA timer and a DMA_RUN read can both reach
     * here for the same start. Only a pending transfer (RUN bit 0x40) runs.
     * The "it was running" latch is not cleared here: it belongs to the
     * guest's DMA_RUN read, which can come after the completion timer (the
     * PROM starts a transfer and polls DMA_RUN right after). */
    if (!(state->dma_run & 0x40)) {
        return;
    }
    uint32_t memory_addr = state->dma_mem_addr;
    uint32_t line_count = (state->dma_size >> 16) & 0xffff;
    uint32_t line_width = state->dma_size & 0xffff;
    uint32_t zoom_count = (state->dma_count >> 16) & 0x3ff;
    uint32_t byte_count = state->dma_count & 0xffff;
    uint32_t stride = state->dma_stride & 0xffff;
    bool to_host = state->dma_mode & (1U << 1);
    bool fill = state->dma_mode & (1U << 3);
    bool forwards = state->dma_mode & (1U << 4);
    /* iris VdmaJob::latch: gio_addr = state.gio_adr & !7. The GIO port is
     * always addressed as a whole qword, so the low alignment bits are not
     * part of the address (a misaligned port would split the transaction). */
    uint32_t gio_addr = state->dma_gio_addr & ~7U;

    if (!line_count) {
        line_count = 1;
    }
    if (!zoom_count) {
        zoom_count = 1;
    }
    if (!byte_count) {
        byte_count = line_width;
    }

    for (uint32_t line = 0; line < line_count; line++) {
        for (uint32_t zoom = 0; zoom < zoom_count; zoom++) {
            uint32_t remaining = byte_count;

            while (remaining) {
                uint32_t length = MIN(remaining, 8U);
                uint8_t buffer[8] = { 0 };
                hwaddr translated;
                uint32_t fault_cause = 0;

                if (!sgi_mc_dma_xlate(state, memory_addr, to_host,
                                      &translated, &fault_cause)) {
                    sgi_mc_dma_abort(state, memory_addr, fault_cause);
                    return;
                }

                if (to_host && fill) {
                    uint32_t fill_value = state->dma_gio_addr;
                    for (uint32_t i = 0; i < length; i++) {
                        buffer[i] = fill_value >> ((7 - i) * 8);
                    }
                    address_space_write(&address_space_memory, translated,
                                        MEMTXATTRS_UNSPECIFIED, buffer, length);
                } else if (to_host) {
                    /* iris dma_generic_bytes: the GIO side of a transfer is
                     * always a whole 64-bit transaction. The read direction
                     * consumes a full qword and scatters only `length` valid
                     * bytes into memory. */
                    address_space_read(&address_space_memory,
                                       gio_addr,
                                       MEMTXATTRS_UNSPECIFIED, buffer, 8);
                    address_space_write(&address_space_memory, translated,
                                        MEMTXATTRS_UNSPECIFIED, buffer, length);
                } else {
                    /* Mem -> GIO: pack a ragged tail into a full qword
                     * (buffer is zero-initialised, so the tail is zero-filled
                     * exactly as iris does) and issue ONE 8-byte write.
                     * Writing only `length` bytes makes the memory core split
                     * a short tail (454 % 8 == 6) into 4+2 transactions; each
                     * fragment re-arms the REX3 host port, so a line costs 58
                     * draws instead of 57 and the stream runs one qword ahead
                     * per row: the +8 px/row shear down the SysMgr pane. */
                    address_space_read(&address_space_memory, translated,
                                       MEMTXATTRS_UNSPECIFIED, buffer, length);
                    address_space_write(&address_space_memory,
                                        gio_addr,
                                        MEMTXATTRS_UNSPECIFIED, buffer, 8);
                }
                memory_addr += forwards ? length : -length;
                remaining -= length;
            }
            if (zoom + 1 < zoom_count) {
                memory_addr += forwards ? -byte_count : byte_count;
            }
        }
        memory_addr += stride;
    }

    state->dma_mem_addr = memory_addr;
    state->dma_count = 0;
    state->dma_run = (state->dma_run & ~0x40) | 0x08;
    state->dma_timer = NULL;
    /* iris: COMPLETE is a cause bit on every successful transfer; IE only
     * gates the interrupt (polled drivers read the cause bit). */
    state->dma_int_cause |= MC_DMA_CAUSE_COMPLETE;
    if (state->dma_control & MC_DMA_CTL_IE) {
        qemu_set_irq(state->dma_done, 1);
    }
}

static void sgi_mc_dma_start(SgiMcState *state)
{
    state->dma_run |= 0x40;
    state->dma_run_unread = true;
    if (!state->dma_timer) {
        state->dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        sgi_mc_dma_complete, state);
    }
    timer_mod_ns(state->dma_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
}

static void sgi_mc_unmap_banks(SgiMcState *state, unsigned reg)
{
    for (unsigned bank = 0; bank < 2; bank++) {
        for (unsigned rank = 0; rank < 2; rank++) {
            for (unsigned mirror = 0; mirror < SGI_MC_MIRRORS; mirror++) {
                MemoryRegion **slot =
                    &state->bank_alias[reg][bank][rank][mirror];

                if (*slot) {
                    memory_region_del_subregion(get_system_memory(), *slot);
                    object_unparent(OBJECT(*slot));
                    g_free(*slot);
                    *slot = NULL;
                }
            }
        }
    }
    if (state->low_alias[reg]) {
        memory_region_del_subregion(get_system_memory(),
                                    state->low_alias[reg]);
        object_unparent(OBJECT(state->low_alias[reg]));
        g_free(state->low_alias[reg]);
        state->low_alias[reg] = NULL;
    }
}

/*
 * Installed RAM per MC bank, from the SIMM nibbles the PROM probes.
 *
 * MAME mc.cpp exposes these as the user-configurable SIMMS port (per
 * bank nibble: 1=4x1M, a=4x2M, 3=4x4M, c=4x8M, 5=4x16M, e=4x32M); the
 * installed size per rank is 1U << (field + 21 - rank). By default this
 * machine carries 4x32M dual-rank SIMMs in banks A+B (128MB each, 256MB
 * total); the `simms` property (settable via -global) overrides the whole
 * nibble set to model other SIMM populations. (MAME's own default is bank
 * A 4x4M only; its ip24 0x03 default covers the VALID register, a
 * different port.) The machine's `-m` size only bounds the backing store;
 * the PROM sizes memory from these registers.
 */
#define SGI_MC_SIMMS_DEFAULT 0x00ee

static void sgi_mc_installed_bank(SgiMcState *state, unsigned bank,
                                  hwaddr ram_size,
                                  hwaddr *start, hwaddr *size,
                                  unsigned *rank)
{
    uint32_t simms = state->simms;
    unsigned field = (simms >> (bank * 4)) & 0x7;
    unsigned inst_rank = (simms >> (bank * 4 + 3)) & 1;
    hwaddr inst_size = 1ULL << (field + 21 - inst_rank);

    /* MAME: an empty SIMM nibble resets the bank, so it stays unmapped. */
    if (!field) {
        *start = 0;
        *size = 0;
        *rank = 0;
        return;
    }

    *start = (hwaddr)bank * 128 * MiB;
    *size = inst_size;
    *rank = inst_rank;

    /* The backing store is still -m sized; clamp to what it can hold. */
    if (*start + *size > ram_size) {
        *size = *start < ram_size ? ram_size - *start : 0;
    }
}

static void sgi_mc_map_banks(SgiMcState *state, unsigned reg, uint32_t data)
{
    unsigned bank;
    hwaddr ram_size = state->ram ? memory_region_size(state->ram) : 0;

    /* MAME memcfg_w: remove the previous mapping for this register first. */
    sgi_mc_unmap_banks(state, reg);

    for (bank = 0; bank < 2; bank++) {
        unsigned valid_shift = 29 - bank * 16;
        unsigned base_shift = 16 - bank * 16;
        unsigned size_shift = 24 - bank * 16;
        unsigned rank_shift = 30 - bank * 16;
        unsigned bank_index = reg * 2 + bank;
        hwaddr base;
        hwaddr rank_size;
        unsigned conf_ranks;
        hwaddr inst_start;
        hwaddr inst_size;
        unsigned inst_rank;
        hwaddr inst_rank_size;
        unsigned ranks;
        hwaddr size;

        if (!(data & (1U << valid_shift))) {
            continue;
        }
        if (!state->ram || !ram_size) {
            continue;
        }

        /*
         * Empty banks stay unmapped so the presence probe fails there;
         * partial banks map min(configured, installed) per rank, mirrored
         * across the window, so the alias probe sizes the bank down.
         */
        base = ((data >> base_shift) & 0xff) << 22;
        conf_ranks = ((data >> rank_shift) & 1) + 1;
        rank_size = (((data >> size_shift) & 0x1f) + 1) <<
                    (22 - ((data >> rank_shift) & 1));
        if (!rank_size) {
            continue;
        }
        sgi_mc_installed_bank(state, bank_index, ram_size,
                              &inst_start, &inst_size, &inst_rank);
        if (!inst_size) {
            continue;
        }
        /* inst_size is already per rank (MAME memcfg inst_size); the extra
         * shift used to quarter dual-rank banks a second time. */
        inst_rank_size = inst_size;
        if (!inst_rank_size) {
            continue;
        }
        ranks = MIN(conf_ranks, inst_rank + 1);
        size = MIN(rank_size, inst_rank_size);
        if (!size) {
            continue;
        }

        for (unsigned rank = 0; rank < ranks; rank++) {
            hwaddr rank_base = base + rank * rank_size;
            hwaddr source = inst_start + rank * size;
            hwaddr mirrors = rank_size / size;
            unsigned copied = 0;

            if (!mirrors) {
                mirrors = 1;
            }
            for (hwaddr mirror = 0; mirror < mirrors &&
                     copied < SGI_MC_MIRRORS; mirror++) {
                hwaddr rank_offset = mirror * size;
                MemoryRegion *alias;

                if (rank_offset + size > rank_size) {
                    break;
                }
                alias = g_new(MemoryRegion, 1);
                memory_region_init_alias(alias, NULL, "sgi-mc.bank",
                                         state->ram, source, size);
                state->bank_alias[reg][bank][rank][copied++] = alias;
                memory_region_add_subregion_overlap(get_system_memory(),
                                                    rank_base + rank_offset,
                                                    alias, 1);
            }
        }
        if (base == 0x08000000ULL) {
            hwaddr low_size = MIN(inst_size, 512 * KiB);
            MemoryRegion *low_alias;

            if (low_size) {
                low_alias = g_new(MemoryRegion, 1);
                memory_region_init_alias(low_alias, NULL, "sgi-mc.low",
                                         state->ram, inst_start, low_size);
                state->low_alias[reg] = low_alias;
                memory_region_add_subregion_overlap(get_system_memory(), 0,
                                                    low_alias, 1);
            }
        }
    }
}

static uint64_t sgi_mc_read_inner(void *opaque, hwaddr addr, unsigned size)
{
    /* iris mc.rs: offset & !4 (bit 2 is an alias). The driver starts DMA
     * through the aliases (0x2004 memory address, 0x202c start). */
    addr &= ~(hwaddr)4;
    SgiMcState *state = SGI_MC(opaque);

    switch (addr) {
    case 0x0000:
    case 0x0004:
        return state->cpu_control;
    case 0x0010:
        return state->watchdog;
    case 0x001c:
    case 0x0018:
        /* System ID: rev C 0x03 by default, bit 4 = EISA present (IP22
         * only). Big-endian kernels read it at 0x1c (mc.h SYSID),
         * little-endian at 0x18; report rev C on both so mc_rev_level is 3
         * and the kernel does not warn about pre-rev-C MC with 16M DRAM
         * parts. The `mc-rev` property overrides the revision nibble (e.g.
         * rev E 0x05 makes the kernel use its >=5 sizing shift). Note that
         * 0x13 on Indy misidentifies the board as IP22 from the very first
         * PROM probe (wrong HAL2/IOC2 tables, wrong ARCS inventory, kernel
         * never programs Local masks), which is why the EISA bit is kept
         * clear on Indy. */
        return state->mc_rev | (state->eisa_present ? 0x10 : 0x00);
    case 0x0028:
    case 0x002c:
        return state->rpss_divider;
    case 0x0080:
    case 0x0084:
        return state->gio_arb_param;
    case 0x0088:
    case 0x008c:
        return state->arb_cpu_time;
    case 0x0098:
    case 0x009c:
        return state->arb_burst_time;
    case 0x00d0:
    case 0x00d4:
        return state->cpu_mem_access_config;
    case 0x00d8:
    case 0x00dc:
        return state->gio_mem_access_config;
    case 0x00c0:
    case 0x00c4:
        return state->memcfg[0];
    case 0x00c8:
    case 0x00cc:
        return state->memcfg[1];
    case 0x1000:
    case 0x1004:
        sgi_mc_rpss_update(state);
        return state->rpss_counter;
    case 0x2004:
        return state->dma_regs[0];
    case 0x2000:
    case 0x2008:
        return state->dma_mem_addr;
    case 0x2010:
        return state->dma_size;
    case 0x2018:
        return state->dma_stride;
    case 0x2020:
    case 0x2028:
        return state->dma_gio_addr;
    case 0x2030:
        return state->dma_mode;
    case 0x2038:
        return state->dma_count;
    case 0x0160:
        return state->dma_int_cause;
    case 0x0168:
        return state->dma_control;
    case 0x0180:
        return state->dma_tlb_hi[0];
    case 0x0188:
        return state->dma_tlb_lo[0];
    case 0x0190:
        return state->dma_tlb_hi[1];
    case 0x0198:
        return state->dma_tlb_lo[1];
    case 0x01a0:
        return state->dma_tlb_hi[2];
    case 0x01a8:
        return state->dma_tlb_lo[2];
    case 0x01b0:
        return state->dma_tlb_hi[3];
    case 0x01b8:
        return state->dma_tlb_lo[3];
    case 0x2040:
        return 0;
    case 0x2014:
        return state->dma_regs[1];
    case 0x201c:
        return state->dma_regs[2];
    case 0x202c:
        return state->dma_regs[3];
    case 0x2034:
        return state->dma_regs[4];
    case 0x2048:
    case 0x204c: {
        if (state->dma_run_unread) {
            state->dma_run_unread = false;
            if (state->dma_timer) {
                timer_del(state->dma_timer);
                state->dma_timer = NULL;
            }
            sgi_mc_dma_complete(state);
            /* Report RUNNING on the first sample (the PROM aborts if it
             * never observes RUN), but keep the real COMPLETE/error bits:
             * the timer may already have finished the transfer, and a
             * bare 0x40 would tell the kernel a done transfer is still
             * running (VDMA TIMEOUT, status 40). */
            return state->dma_run | 0x40;
        }
        return state->dma_run;
    }
    default:
        return 0;
    }
}

/*
 * MC tracing, capped per offset like the PIO dispatch traces: the kernel
 * polls MC registers during bring-up, so an uncapped trace floods and its
 * I/O load slows the vCPU enough to change guest-visible timing.
 */
#define SGI_MC_TRACE_PER_OFFSET 2000
static unsigned sgi_mc_trace_count[4096];

static bool sgi_mc_trace_wanted(hwaddr addr)
{
    unsigned idx = addr & 0xfff;

    if (sgi_mc_trace_count[idx] >= SGI_MC_TRACE_PER_OFFSET) {
        return false;
    }
    sgi_mc_trace_count[idx]++;
    return true;
}

static uint64_t sgi_mc_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = sgi_mc_read_inner(opaque, addr, size);

    if (sgi_ip22_trace_enabled() && sgi_mc_trace_wanted(addr)) {
        fprintf(stderr, "MC-R off=%04x sz=%u val=%016llx\n",
                (unsigned)addr, size, (unsigned long long)val);
    }
    return val;
}

static void sgi_mc_write_inner(void *opaque, hwaddr addr,
                               uint64_t val, unsigned size)
{
    /* iris mc.rs: offset & !4 (bit 2 is an alias). The driver starts DMA
     * through the aliases (0x2004 memory address, 0x202c start). */
    addr &= ~(hwaddr)4;
    SgiMcState *state = SGI_MC(opaque);

    switch (addr) {
    case 0x0000:
    case 0x0004:
        state->cpu_control = val;
        break;
    case 0x0010:
        state->watchdog = 0;
        break;
    case 0x0028:
    case 0x002c:
        sgi_mc_rpss_update(state);
        state->rpss_divider = val;
        break;
    case 0x0080:
    case 0x0084:
        state->gio_arb_param = val;
        break;
    case 0x0088:
    case 0x008c:
        state->arb_cpu_time = val;
        break;
    case 0x0098:
    case 0x009c:
        state->arb_burst_time = val;
        break;
    case 0x00d0:
    case 0x00d4:
        state->cpu_mem_access_config = val;
        break;
    case 0x00d8:
    case 0x00dc:
        state->gio_mem_access_config = val;
        break;
    case 0x00c0:
    case 0x00c4:
        state->memcfg[0] = val;
        sgi_mc_map_banks(state, 0, val);
        break;
    case 0x00c8:
    case 0x00cc:
        state->memcfg[1] = val;
        sgi_mc_map_banks(state, 1, val);
        break;
    case 0x0160:
        state->dma_int_cause = val;
        if (state->dma_int_cause == 0) {
            qemu_set_irq(state->dma_done, 0);
        }
        break;
    case 0x0168:
        state->dma_control = val;
        break;
    case 0x0180:
        state->dma_tlb_hi[0] = val;
        break;
    case 0x0188:
        state->dma_tlb_lo[0] = val;
        break;
    case 0x0190:
        state->dma_tlb_hi[1] = val;
        break;
    case 0x0198:
        state->dma_tlb_lo[1] = val;
        break;
    case 0x01a0:
        state->dma_tlb_hi[2] = val;
        break;
    case 0x01a8:
        state->dma_tlb_lo[2] = val;
        break;
    case 0x01b0:
        state->dma_tlb_hi[3] = val;
        break;
    case 0x01b8:
        state->dma_tlb_lo[3] = val;
        break;
    case 0x2004:
        state->dma_regs[0] = val;
        break;
    case 0x2000:
    case 0x2008:
        state->dma_mem_addr = val;
        break;
    case 0x2010:
        state->dma_size = val;
        state->dma_count = (state->dma_count & 0xffff0000) | (val & 0xffff);
        break;
    case 0x2018:
        state->dma_stride = val;
        state->dma_count = (state->dma_count & 0x0000ffff) |
                           (val & 0x03ff0000);
        break;
    case 0x2020:
        state->dma_gio_addr = val;
        break;
    case 0x2028:
        state->dma_gio_addr = val;
        sgi_mc_dma_start(state);
        break;
    case 0x2030:
        state->dma_mode = val;
        break;
    case 0x2038:
        state->dma_count = val;
        break;
    case 0x2040:
        if (val & 1) {
            sgi_mc_dma_start(state);
        }
        break;
    case 0x2014:
        state->dma_regs[1] = val;
        break;
    case 0x201c:
        state->dma_regs[2] = val;
        break;
    case 0x202c:
        state->dma_regs[3] = val;
        sgi_mc_dma_start(state);
        break;
    case 0x2070:
        state->dma_gio_addr = val;
        state->dma_size = 0x0001000c;
        state->dma_stride = 0x00010000;
        state->dma_count = 0x0001000c;
        state->dma_mode = 0x00000028;
        sgi_mc_dma_start(state);
        break;
    case 0x2034:
        state->dma_regs[4] = val;
        break;
    case 0x204c:
        state->dma_run = val;
        break;
    default:
        break;
    }
}

static void sgi_mc_write(void *opaque, hwaddr addr,
                         uint64_t val, unsigned size)
{
    if (sgi_ip22_trace_enabled() && sgi_mc_trace_wanted(addr)) {
        fprintf(stderr, "MC-W off=%04x sz=%u val=%016llx\n",
                (unsigned)addr, size, (unsigned long long)val);
    }
    sgi_mc_write_inner(opaque, addr, val, size);
}

static const MemoryRegionOps sgi_mc_ops = {
    .read = sgi_mc_read,
    .write = sgi_mc_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static const VMStateDescription vmstate_sgi_mc = {
    .name = TYPE_SGI_MC,
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cpu_control, SgiMcState),
        VMSTATE_UINT32(watchdog, SgiMcState),
        VMSTATE_UINT32(gio_arb_param, SgiMcState),
        VMSTATE_UINT32(arb_cpu_time, SgiMcState),
        VMSTATE_UINT32(arb_burst_time, SgiMcState),
        VMSTATE_UINT32(cpu_mem_access_config, SgiMcState),
        VMSTATE_UINT32(gio_mem_access_config, SgiMcState),
        VMSTATE_UINT32(rpss_divider, SgiMcState),
        VMSTATE_UINT32(rpss_counter, SgiMcState),
        VMSTATE_UINT64(rpss_last_ns, SgiMcState),
        VMSTATE_UINT32_ARRAY(memcfg, SgiMcState, 2),
        VMSTATE_UINT32_ARRAY(dma_regs, SgiMcState, 6),
        VMSTATE_UINT32(dma_run, SgiMcState),
        VMSTATE_BOOL(dma_run_unread, SgiMcState),
        VMSTATE_UINT32(dma_mem_addr, SgiMcState),
        VMSTATE_UINT32(dma_size, SgiMcState),
        VMSTATE_UINT32(dma_stride, SgiMcState),
        VMSTATE_UINT32(dma_gio_addr, SgiMcState),
        VMSTATE_UINT32(dma_mode, SgiMcState),
        VMSTATE_UINT32(dma_count, SgiMcState),
        VMSTATE_UINT32_ARRAY(dma_tlb_hi, SgiMcState, 4),
        VMSTATE_UINT32_ARRAY(dma_tlb_lo, SgiMcState, 4),
        VMSTATE_UINT32(dma_int_cause, SgiMcState),
        VMSTATE_UINT32(dma_control, SgiMcState),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_mc_reset(DeviceState *device)
{
    SgiMcState *state = SGI_MC(device);

    state->cpu_control = 0;
    state->watchdog = 0;
    state->gio_arb_param = 0;
    state->arb_cpu_time = 0;
    state->arb_burst_time = 0;
    state->cpu_mem_access_config = 0;
    state->gio_mem_access_config = 0;
    /* MAME device_reset value; zero would stall the counter. */
    state->rpss_divider = 0x0104;
    state->rpss_counter = 0;
    state->rpss_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memset(state->memcfg, 0, sizeof(state->memcfg));
    memset(state->dma_regs, 0, sizeof(state->dma_regs));
    state->dma_run = 0;
    state->dma_run_unread = false;
    state->dma_mem_addr = 0;
    state->dma_size = 0;
    state->dma_stride = 0;
    state->dma_gio_addr = 0;
    state->dma_mode = 0;
    state->dma_count = 0;
    memset(state->dma_tlb_hi, 0, sizeof(state->dma_tlb_hi));
    memset(state->dma_tlb_lo, 0, sizeof(state->dma_tlb_lo));
    state->dma_int_cause = 0;
    state->dma_control = 0;
    if (state->dma_timer) {
        /* A pending completion must not scribble RAM after reset. */
        timer_del(state->dma_timer);
    }
    /* MAME clears the DMA-done line on reset; drop any pending level. */
    qemu_set_irq(state->dma_done, 0);
    /* MAME memcfg_w(0, 0) + memcfg_w(1, 0) plus zeroed RAM, so the PROM
     * re-sizes memory from scratch like a cold boot. */
    if (state->ram) {
        memset(memory_region_get_ram_ptr(state->ram), 0,
               memory_region_size(state->ram));
    }
    sgi_mc_unmap_banks(state, 0);
    sgi_mc_unmap_banks(state, 1);
}

static void sgi_mc_init(Object *object)
{
    SgiMcState *state = SGI_MC(object);

    memory_region_init_io(&state->iomem, object, &sgi_mc_ops, state,
                          TYPE_SGI_MC, SGI_MC_IOMEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &state->iomem);
    qdev_init_gpio_out(DEVICE(object), &state->dma_done, 1);
}

static const Property sgi_mc_properties[] = {
    DEFINE_PROP_LINK("ram", SgiMcState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_BOOL("eisa-present", SgiMcState, eisa_present, true),
    DEFINE_PROP_UINT32("simms", SgiMcState, simms, SGI_MC_SIMMS_DEFAULT),
    DEFINE_PROP_UINT8("mc-rev", SgiMcState, mc_rev, 0x03),
};

static void sgi_mc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->vmsd = &vmstate_sgi_mc;
    device_class_set_legacy_reset(device_class, sgi_mc_reset);
    device_class_set_props(device_class, sgi_mc_properties);
}

static const TypeInfo sgi_mc_type = {
    .name = TYPE_SGI_MC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SgiMcState),
    .instance_init = sgi_mc_init,
    .class_init = sgi_mc_class_init,
};

static void sgi_mc_register_types(void)
{
    type_register_static(&sgi_mc_type);
}

type_init(sgi_mc_register_types)

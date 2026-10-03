#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/aio.h"
#include "qemu/main-loop.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/scsi/sgi_wd33c93.h"
#include "hw/scsi/esp.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/address-spaces.h"

extern hwaddr mips_cpu_get_phys_addr_debug(CPUState *cpu, vaddr addr);
extern hwaddr mips_cpu_get_phys_page_any_asid(CPUState *cs, vaddr addr);
void mips_dump_tlb_match(CPUState *cs, vaddr addr);

#define SGI_WD_STATUS_SELECT_SUCCESS 0x11
#define SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS 0x16
#define SGI_WD_STATUS_TRANSFER_SUCCESS 0x18
#define SGI_WD_STATUS_SELECTION_TIMEOUT 0x42
#define SGI_WD_STATUS_REQ 0x88
#define SGI_WD_PHASE_DATA_IN 0x01
#define SGI_WD_PHASE_STATUS 0x03
#define SGI_WD_PHASE_MSG_OUT 0x06
#define SGI_WD_PHASE_RECEIVE_STATUS 0x47
#define SGI_WD_PHASE_STATUS_RECEIVED 0x50
#define SGI_WD_PHASE_COMMAND_COMPLETE 0x60

/* MAME wd33c9x.cpp:207-232: COMMAND_CC masks seven bits; SBT is bit 7. */
#define SGI_WD_COMMAND_CC 0x7f
#define SGI_WD_COMMAND_CC_RESET 0x00
#define SGI_WD_COMMAND_CC_SELECT_ATN 0x06
#define SGI_WD_COMMAND_CC_SELECT 0x07
#define SGI_WD_COMMAND_CC_SELECT_ATN_TRANSFER 0x08
#define SGI_WD_COMMAND_CC_SELECT_TRANSFER 0x09
#define SGI_WD_COMMAND_CC_TRANSFER_INFO 0x20
#define SGI_WD_COMMAND_SBT 0x80
struct SGIWD33C93State {
    SysBusDevice parent_obj;
    MemoryRegion regs;
    ESPState *esp;
    qemu_irq irq;
    qemu_irq dma_done_irq;
    uint8_t wd_regs[32];
    uint8_t register_select;
    uint8_t auxiliary_status;
    uint8_t scsi_status;
    bool command_active;
    bool selection_status_pending;
    bool message_phase;
    uint32_t message_remaining;
    bool dma_armed;
    bool kick_traced;
    uint32_t dma_moved;
    uint32_t dma_got;
    uint32_t dma_dropped;
    uint8_t completion_phase;
    uint8_t irq_fifo[8];
    uint8_t irq_fifo_pos;
    uint8_t irq_fifo_size;
    bool data_status_queued;
    uint32_t dma_next_desc;
    uint32_t dma_current;
    uint32_t dma_count;
    uint32_t dma_control;
    uint32_t dma_desc_flags;
    bool dma_irq;
    bool dma_active;
    bool irq_level;
    bool irq_initiator;
    QEMUTimer *progress_timer;
    uint32_t inq_patch_va;
    bool inq_patch_pending;
};

static void sgi_wd_irq_fifo_reset(SGIWD33C93State *state)
{
    state->irq_fifo_pos = 0;
    state->irq_fifo_size = 0;
    state->data_status_queued = false;
    state->completion_phase = 0;
}

static void sgi_wd_irq_fifo_push(SGIWD33C93State *state, uint8_t status)
{
    if (state->irq_fifo_size == ARRAY_SIZE(state->irq_fifo)) {
        return;
    }
    if (state->irq_fifo_size &&
        state->irq_fifo[(state->irq_fifo_pos + state->irq_fifo_size - 1) & 7] == status) {
        return;
    }
    state->irq_fifo[(state->irq_fifo_pos + state->irq_fifo_size) & 7] = status;
    state->irq_fifo_size++;
}

static uint8_t sgi_wd_irq_fifo_peek(SGIWD33C93State *state)
{
    return state->irq_fifo_size ? state->irq_fifo[state->irq_fifo_pos] : 0;
}

static void sgi_wd_irq_fifo_pop(SGIWD33C93State *state)
{
    if (state->irq_fifo_size) {
        state->irq_fifo_pos = (state->irq_fifo_pos + 1) & 7;
        state->irq_fifo_size--;
    }
}

/*
 * MAME wd33c9x.cpp decrement_transfer_count(): after any completed transfer
 * (including a single-byte message transfer) the count registers read back 0.
 */
static void sgi_wd_decrement_transfer_count(SGIWD33C93State *state)
{
    uint32_t count;

    if (!state->wd_regs[0x12] && !state->wd_regs[0x13] && !state->wd_regs[0x14]) {
        return;
    }
    count = ((uint32_t)state->wd_regs[0x12] << 16) |
            ((uint32_t)state->wd_regs[0x13] << 8) |
            state->wd_regs[0x14];
    count--;
    state->wd_regs[0x12] = count >> 16;
    state->wd_regs[0x13] = count >> 8;
    state->wd_regs[0x14] = count;
}

static void sgi_wd_update_irq(SGIWD33C93State *state)
{
    bool level = false;

    /* MAME wd33c9x.cpp:958: FINISHED returns to disconnected mode. */
    if (!state->command_active) {
        state->irq_initiator = false;
    }
    if (state->auxiliary_status & 0x80) {
        state->auxiliary_status &= ~0x80;
    }
    if (state->irq_fifo_size) {
        state->scsi_status = sgi_wd_irq_fifo_peek(state);
        state->auxiliary_status |= 0x80;
        /* MAME wd33c9x.cpp:1643: EDI/IDI silence SELECT phases mid-transfer. */
        level = true;
        if (state->irq_initiator &&
            ((state->wd_regs[0x18] & SGI_WD_COMMAND_CC) ==
             SGI_WD_COMMAND_CC_SELECT_ATN_TRANSFER ||
             (state->wd_regs[0x18] & SGI_WD_COMMAND_CC) ==
             SGI_WD_COMMAND_CC_SELECT_TRANSFER)) {
            if (state->scsi_status == 0x85 &&
                !(state->wd_regs[0x01] & 0x04)) {
                level = false;
            }
            if (state->scsi_status == 0x16 &&
                (state->wd_regs[0x01] & 0x08)) {
                level = false;
            }
        }
    }
    if (level != state->irq_level) {
        state->irq_level = level;
        qemu_set_irq(state->irq, level);
    }
}

/* MAME hpc3.cpp:969-971,991-993: desc bit29 raises SCSI DMA-done. */
static void sgi_wd_update_dma_irq(SGIWD33C93State *state)
{
    qemu_set_irq(state->dma_done_irq, state->dma_irq);
}

static void sgi_wd_reset_state(SGIWD33C93State *state)
{
    uint8_t own_id = state->wd_regs[0];
    memset(state->wd_regs, 0, sizeof(state->wd_regs));
    /* MAME wd33c9x.cpp:421-424: 0x1a-0x1e read back 0xff. */
    memset(&state->wd_regs[0x1a], 0xff, 5);
    /* MAME wd33c9x.cpp:738-754: soft reset preserves OWN_ID. */
    state->wd_regs[0] = own_id;
    state->register_select = 0;
    state->auxiliary_status = 0x80;
    /* MAME wd33c9x.cpp:433: reset drops the IRQ line. */
    state->irq_level = false;
    qemu_set_irq(state->irq, 0);
    state->scsi_status = SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS;
    state->command_active = false;
    state->selection_status_pending = false;
    state->message_phase = false;
    state->message_remaining = 0;
    state->dma_armed = false;
    state->completion_phase = 0;
    state->dma_next_desc = 0;
    state->dma_current = 0;
    state->dma_count = 0;
    state->dma_moved = 0;
    state->dma_got = 0;
    state->dma_dropped = 0;
    state->dma_control = 0;
    state->dma_desc_flags = 0;
    state->dma_irq = false;
    state->dma_active = false;
    state->inq_patch_va = 0;
    state->inq_patch_pending = false;
    if (state->progress_timer) {
        timer_del(state->progress_timer);
    }
    sgi_wd_irq_fifo_reset(state);
    /* MAME wd33c9x.cpp:751: RESET_EAF when OWN_ID EAF is set. */
    sgi_wd_irq_fifo_push(state, (own_id & 0x08) ? 0x01 : 0x00);
    if (state->esp) {
        esp_hard_reset(state->esp);
    }
    sgi_wd_update_irq(state);
    sgi_wd_update_dma_irq(state);
}

static void sgi_wd_reset(DeviceState *device)
{
    sgi_wd_reset_state(SGI_WD33C93(device));
}

/* Board tracing (off by default; set SGI_TRACE=1 in the environment to
 * re-enable all board-trace lines in this file). */
static bool sgi_wd_trace_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("SGI_TRACE") ? 1 : 0;
    }
    return cached != 0;
}

/*
 * Per-key cap for the per-access traces: the kernel polls the auxiliary
 * status register in a tight loop and streams SCSI DMA bytes, so an
 * uncapped trace floods stderr and its I/O load slows the vCPU enough to
 * change guest-visible timing.  Keys 5-7 are per-site totals for the
 * per-byte DMA fallback traces (every byte is a distinct address).
 */
#define SGI_WD_TRACE_PER_KEY 5000
static unsigned sgi_wd_trace_keys[32][9];

static bool sgi_wd_trace_wanted(unsigned site, unsigned addr)
{
    if (site >= 9 || (addr & 0x1f) >= 32) {
        return true;
    }
    if (sgi_wd_trace_keys[addr & 0x1f][site] >= SGI_WD_TRACE_PER_KEY) {
        return false;
    }
    sgi_wd_trace_keys[addr & 0x1f][site]++;
    return true;
}

/* R4400 32-bit view: kseg0-3 (0x80000000+) all decode to PA 0-0x1fffffff
 * via SegCtl; below 0x80000000 the address is physical (bank aliases at
 * 0x08000000/0x0c000000 included). Mirrors MAME's MMU descriptor fetch. */
static hwaddr sgi_wd_pa(hwaddr addr)
{
    if (addr >= 0x80000000U) {
        if (sgi_wd_trace_enabled() && addr >= 0xc0000000U) {
            fprintf(stderr, "SGIWDPA in=%016llx out=%08llx\n",
                    (unsigned long long)addr,
                    (unsigned long long)(addr & 0x1fffffffU));
        }
        return addr & 0x1fffffffU;
    }
    return addr;
}

static uint32_t sgi_wd_dma_read32(hwaddr addr)
{
    /* Descriptors/data below 0x80000000 are already physical (MC bank
     * aliases, exactly like MAME's raw AS_PROGRAM dispatch): use them
     * directly so DMA can never depend on TLB/ASID state.  KSEG
     * KSEG2 addresses go through the CPU translation (debug, then any-ASID
     * soft-TLB) with SegCtl masking as the last resort.  KSEG0/1 are
     * direct-mapped and must never consult the TLB: stale entries from
     * other address spaces otherwise divert DMA to the wrong page. */
    CPUState *cs = qemu_get_cpu(0);
    if (cs && addr >= 0x80000000U) {
        uint8_t b[4];
        if (cpu_memory_rw_debug(cs, (vaddr)addr, b, sizeof(b), 0) == 0) {
            return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                   ((uint32_t)b[2] << 8) | (uint32_t)b[3];
        }
    }

    uint32_t value = 0;
    hwaddr pa;

    if (cs && addr >= 0xc0000000U) {
        hwaddr tpa = mips_cpu_get_phys_page_any_asid(cs, addr);

        if (tpa != (hwaddr)-1) {
            pa = tpa;
        } else {
            pa = sgi_wd_pa(addr);
        }
    } else {
        pa = sgi_wd_pa(addr);
    }

    /* Board trace: descriptor dword resolved via physical fallback */
    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "DMA32R-FB va=%08x pa=%08x\n",
                (unsigned)addr, (unsigned)pa);
    }
    address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                       &value, sizeof(value));
    return be32_to_cpu(value);
}

static void sgi_wd_dma_fetch_descriptor(SGIWD33C93State *state)
{
    uint32_t desc = state->dma_next_desc;

    state->dma_current = sgi_wd_dma_read32(desc);
    state->dma_desc_flags = sgi_wd_dma_read32(desc + 4);
    state->dma_next_desc = sgi_wd_dma_read32(desc + 8);
    state->dma_count = state->dma_desc_flags & 0x3fff;
}

static hwaddr sgi_wd_dma_address(SGIWD33C93State *state, hwaddr addr)
{
    /* Keep virtual for cpu_memory_rw_debug; only the byte-lane xor applies. */
    if (state->dma_control & 0x02) {
        addr ^= 3;
    }
    return addr;
}

static bool sgi_wd_dma_next(SGIWD33C93State *state)
{
    if (state->dma_count) {
        return true;
    }
    /* MAME hpc3.cpp:963-983: IRQ on bit 29 at chain end. */
    if (state->dma_desc_flags & (1U << 29)) {
        state->dma_irq = true;
        sgi_wd_update_dma_irq(state);
    }
    if (state->dma_desc_flags & (1U << 31)) {
        state->dma_control &= ~0x10U;
        state->dma_active = false;
        return false;
    }
    sgi_wd_dma_fetch_descriptor(state);
    return state->dma_count != 0;
}

/* MAME wd33c9x.cpp:1127,1199: DRQ only when CONTROL DMA != POLLED. */
static bool sgi_wd_wd_dma_mode(SGIWD33C93State *state)
{
    return (state->wd_regs[0x01] & 0xe0) != 0;
}

/*
 * One residual test for both completion sites (the FC poll and the live
 * status-register read).  Bulk transfers wait for every byte before GOOD is
 * reported; INQUIRY/MODE SENSE/REQUEST SENSE may legitimately under-fill.
 * A dropped tail is counted (dma_dropped) but never refuses completion:
 * MAME completes such commands normally, and withholding GOOD wedged the
 * driver into a bus-reset loop in testing.
 */
static bool sgi_wd_bulk_residual(SGIWD33C93State *state, uint8_t op)
{
    if (op == 0x12 || op == 0x1a || op == 0x03) {
        return false;
    }
    return state->dma_count ||
        state->wd_regs[0x12] || state->wd_regs[0x13] || state->wd_regs[0x14];
}

/* MAME hpc3.cpp:985-1000: flush raises IRQ on bit 29, drops ENABLE. */
static void sgi_wd_dma_flush(SGIWD33C93State *state)
{
if (state->dma_desc_flags & (1U << 29)) {
                state->dma_irq = true;
                sgi_wd_update_dma_irq(state);
            }
            state->dma_control &= ~0x10U;
            state->dma_active = false;
    state->dma_control &= ~(0x10U | 0x08U);
}

/*
 * Mirror HPC3 DMA progress into the WD transfer-count registers the PROM
 * polls as the residual count.
 */
static void sgi_wd_dma_bump_wd_count(SGIWD33C93State *state)
{
    uint32_t wd_count = ((uint32_t)state->wd_regs[0x12] << 16) |
                        ((uint32_t)state->wd_regs[0x13] << 8) |
                        state->wd_regs[0x14];

    if (wd_count != 0) {
        wd_count--;
        state->wd_regs[0x12] = wd_count >> 16;
        state->wd_regs[0x13] = wd_count >> 8;
        state->wd_regs[0x14] = wd_count;
    }
}

static void sgi_wd_dma_read(void *opaque, uint8_t *buf, int len)
{
    SGIWD33C93State *state = opaque;
    CPUState *cs = qemu_get_cpu(0);

    while (len > 0 && sgi_wd_dma_next(state)) {
        uint8_t v = 0;
        bool ok = false;
        if (cs && state->dma_current >= 0x80000000U) {
            hwaddr va = sgi_wd_dma_address(state, state->dma_current);
            if (cpu_memory_rw_debug(cs, (vaddr)va, &v, 1, 0) == 0) {
                ok = true;
            }
        }
        if (!ok) {
            MemTxResult result;
            hwaddr pa;

            /*
             * Same as the descriptor fetch: below 0xc0000000 the address
             * is unmapped (KSEG0/1 direct, below 0x80000000 already
             * physical); only TLB-mapped KSEG2 consults the translation.
             */
            if (cs && state->dma_current >= 0xc0000000U) {
                hwaddr tpa = mips_cpu_get_phys_page_any_asid(
                    cs, sgi_wd_dma_address(state, state->dma_current));

                if (tpa != (hwaddr)-1) {
                    pa = tpa;
                } else {
                    pa = sgi_wd_pa(state->dma_current);
                }
            } else {
                pa = sgi_wd_pa(state->dma_current);
            }
            if (state->dma_control & 0x02) {
                pa ^= 3;
            }
            /* Board trace: guest data-out byte fell back to physical read */
            if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(5, 0)) {
                fprintf(stderr, "WDR-FB va=%08x pa=%08x\n",
                        (unsigned)state->dma_current, (unsigned)pa);
            }
            v = address_space_ldub(&address_space_memory, pa,
                                   MEMTXATTRS_UNSPECIFIED, &result);
        }
        *buf++ = v;
        state->dma_current++;
        state->dma_count--;
        state->dma_got++;
        len--;
        sgi_wd_dma_bump_wd_count(state);
    }
    /* Bytes the ESP asked for but the chain could not place. */
    state->dma_dropped += len;
}

static void sgi_wd_dma_write(void *opaque, uint8_t *buf, int len)
{
    SGIWD33C93State *state = opaque;
    CPUState *cs = qemu_get_cpu(0);

    /*
     * The ESP is the bus master during its data-in phase: the guest has
     * already armed the WD transfer count (registers 0x12-0x14, 24 bit) for
     * the whole command and the ESP drives REQ/ACK for each chunk itself.
     *
     * dma_count, by contrast, is the HPC3 channel the *guest's own* copy
     * engine uses.  It reaches us stale: sgi_wd_start_transfer() only clears
     * it when the channel is idle, the guest rarely reprograms the 14 bit
     * 0x1000 register for the SCSI path, and when it uses the SGI descriptor
     * chain the count is a per-descriptor slice.  Gating the ESP's bytes on
     * it via sgi_wd_dma_next() therefore truncates every transfer that is
     * bigger than one chunk: the loop stops at the descriptor boundary, the
     * ESP's TC never reaches zero, no function-complete is ever raised, and
     * the command hangs until the driver gives up.  That was every READ(10)
     * after the first 512 byte one, which is what left the kernel unable to
     * find its root filesystem.
     *
     * So let the ESP's request stand: top the count up to what it is asking
     * for, and keep walking the chain as before for the guest's own DMA.
     */
    if (len > state->dma_count) {
        state->dma_count = len;
    }
    state->dma_moved += len;
    while (len > 0 && sgi_wd_dma_next(state)) {
        uint8_t v = *buf++;
        bool ok = false;
        if (cs && state->dma_current >= 0x80000000U) {
            hwaddr va = sgi_wd_dma_address(state, state->dma_current);
            int rr = cpu_memory_rw_debug(cs, (vaddr)va, &v, 1, 1);
            if (rr == 0) {
                ok = true;
            }
            if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(6, 0) &&
                (state->dma_current & 0xfe000000U) == 0x08000000U) {
                fprintf(stderr, "WDPN va=%08x rwret=%d tpa=%08x\n",
                        (unsigned)va, rr,
                        (unsigned)mips_cpu_get_phys_addr_debug(cs, va));
            }
        }
        if (!ok) {
            MemTxResult result;
            hwaddr pa;

            /*
             * Below 0xc0000000 the address is unmapped (KSEG0/1 direct,
             * below 0x80000000 already physical per MAME raw-space
             * semantics): use it directly so DMA can never depend on
             * TLB/ASID state.  Only KSEG2 consults the CPU translation.
             */
            if (cs && state->dma_current >= 0xc0000000U) {
                hwaddr tpa = mips_cpu_get_phys_page_any_asid(
                    cs, sgi_wd_dma_address(state, state->dma_current));

                if (tpa != (hwaddr)-1) {
                    pa = tpa;
                } else {
                    pa = sgi_wd_pa(state->dma_current);
                }
            } else {
                pa = sgi_wd_pa(state->dma_current);
            }
            if (state->dma_control & 0x02) {
                pa ^= 3;
            }
            address_space_stb(&address_space_memory, pa, v,
                              MEMTXATTRS_UNSPECIFIED, &result);
            /* Board trace: guest data-in byte fell back to physical write */
            if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(7, 0)) {
                fprintf(stderr, "WDW-FB va=%08x pa=%08x\n",
                        (unsigned)state->dma_current, (unsigned)pa);
            }
        }
        state->dma_current++;
        state->dma_count--;
        state->dma_got++;
        len--;
        sgi_wd_dma_bump_wd_count(state);
    }
    /* Bytes the ESP asked for but the chain could not place. */
    state->dma_dropped += len;
}

void sgi_wd33c93_dma_reg_write(SGIWD33C93State *state, hwaddr addr,
                               uint32_t value)
{
    if (!state) {
        return;
    }
    switch (addr) {
    case 0x2000:
    case 0x00:
        state->dma_current = value;
        break;
    case 0x2004:
    case 0x04:
        state->dma_next_desc = value;
        break;
    case 0x3000:
    case 0x1000:
        state->dma_count = value & 0x3fff;
        break;
    case 0x3004:
    case 0x1004: {
        /* MAME hpc3.cpp:414-442: WRMASK keeps ENABLE/active. */
        bool was_active = state->dma_active;
        if (value & 0x20) {
            state->dma_control = (value & ~(0x01U | 0x10U | 0x20U)) |
                                 (state->dma_control & 0x10U);
            if (was_active) {
                state->dma_control |= 0x10U;
            }
        } else {
            state->dma_control = value & ~(0x01U | 0x20U);
            state->dma_active = (state->dma_control & 0x10U) != 0;
        }
        if (value & 0x08) {
            sgi_wd_dma_flush(state);
        }
        /* MAME hpc3.cpp:429-431: fetch on rising ENABLE, not every write. */
        if (!was_active && state->dma_active) {
            sgi_wd_dma_fetch_descriptor(state);
        }
        break;
    }
    }
}

uint32_t sgi_wd33c93_dma_reg_read(SGIWD33C93State *state, hwaddr addr)
{
    if (!state) {
        return 0;
    }
    switch (addr) {
    case 0x2000:
    case 0x00:
        return state->dma_current;
    case 0x2004:
    case 0x04:
        return state->dma_next_desc;
    case 0x3000:
    case 0x1000:
        /* MAME hpc3.cpp:275-282: live count plus descriptor upper flags. */
        return (state->dma_count & 0x3fff) |
               (state->dma_desc_flags & 0xffffc000U);
    case 0x3004:
    case 0x1004: {
        uint32_t ret = state->dma_control;
        /* MAME hpc3.cpp:287-300: IRQ bit presents and clears on read. */
        if (state->dma_irq) {
            ret |= 0x01U;
            state->dma_irq = false;
            sgi_wd_update_dma_irq(state);
        }
        /*
         * The PROM polls for bit 0x10 clear at chain end; reflect the drained
         * state here since no further ESP call updates it.
         */
        if (state->dma_count == 0) {
            state->dma_control &= ~0x10U;
            state->dma_active = false;
            ret &= ~0x10U;
        }
        return ret;
    }
    default:
        return 0;
    }
}

static unsigned sgi_wd_cdb_length(uint8_t opcode)
{
    /* MAME wd33c9x.cpp:1744-1749: only groups 1 and 5 are long. */
    switch (opcode >> 5) {
    case 1:
        return 10;
    case 5:
        return 12;
    default:
        return 6;
    }
}

static void sgi_wd_update_status(SGIWD33C93State *state);

/*
 * MAME wd33c9x.cpp:735 sets BSY for every command above DISCONNECT, and
 * :799-806 gives SELECT (0x07) / SELECT_ATN (0x06) the selection state
 * machine.  This model has no SCSI bus phases, so resolve the selection
 * against the target list directly: a device matching DESTINATION_ID
 * answers SELECT_TRANSFER_SUCCESS, a missing one SELECTION_TIMEOUT.
 *
 * The kernel only uses SELECT_ATN_TRANSFER (0x08, see sgi_wd_start_transfer)
 * for the *first* command after reset; every later command is preceded by
 * SELECT_ATN.  With that command unhandled the auxiliary status never leaves
 * 0x00, the driver's wait for BSY/INT never ends, and every subsequent
 * command dies on its 60 second timeout ("SCSI cmd=0x28 timeout after
 * 60 sec. Resetting SCSI bus").  BSY is held by command_active, because
 * sgi_wd_update_status() drops BSY whenever no command is in flight.
 */
/*
 * MAME wd33c9x.cpp:1169-1180, DISC_SEL_ARBITRATION: a plain SELECT_ATN or
 * SELECT arbitrates and then waits for the target to assert REQ, so the
 * controller reports selection success *and* the phase the target wants next
 * as two separate interrupts.  Both must be queued: reading the SCSI Status
 * Register pops the IRQ FIFO entry (MAME indir_reg_r calls update_irq()), and
 * the PROM's wait loop only leaves via aux & INT.  With a single entry queued
 * the FIFO empties on the first read, INT drops, and the SGI PROM's poll
 * (0xfc1f320, returning status|0x1000 on timeout) makes 0xfc1f24c report the
 * selection as failed -- the "SYNC negotiation error" on screen.
 */
static void sgi_wd_start_select(SGIWD33C93State *state)
{
    uint8_t dest = state->wd_regs[0x15] & BUSID_DID;
    bool present = scsi_device_find(&state->esp->bus, 0, dest, 0) != NULL;
    uint8_t status = present ? SGI_WD_STATUS_SELECT_SUCCESS
                             : SGI_WD_STATUS_SELECTION_TIMEOUT;

    state->command_active = true;
    state->dma_armed = false;
    state->irq_initiator = true;
    state->auxiliary_status |= 0x20;            /* BSY */
    state->completion_phase = 0;
    state->message_phase = false;
    state->message_remaining = 0;
    sgi_wd_irq_fifo_reset(state);
    sgi_wd_irq_fifo_push(state, status);
    if (present) {
        /* Target answers an ATN selection with MESSAGE OUT (IDENTIFY). */
        sgi_wd_irq_fifo_push(state,
                             SGI_WD_STATUS_REQ | SGI_WD_PHASE_MSG_OUT);
    }
    state->scsi_status = status;
    state->selection_status_pending = false;

    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "SGIWDSEL dest=%u %s -> %02x aux=%02x\n",
                dest, present ? "found" : "missing", status,
                state->auxiliary_status);
    }
    /* MAME FINISHED clears CIP and BSY before the interrupts are reported. */
    state->auxiliary_status &= ~0x20;
    sgi_wd_update_irq(state);
    if (state->progress_timer) {
        timer_mod_ns(state->progress_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

/*
 * MAME wd33c9x.cpp COMMAND_CC_TRANSFER_INFO (0x20): run one message/data
 * transfer over the connection a previous SELECT_ATN left open.  The SGI PROM
 * uses this to push its six-byte WD sync request
 * (80 01 03 01 <period> <offset>) to the target and to receive the reply.
 * MAME samples the live bus phase and sets DBR when the data FIFO is empty
 * (wd33c9x.cpp INIT_XFR / S_PHASE_MSG_OUT); the target then changes phase to
 * STATUS, which ends the transfer with
 * TRANSFER_SUCCESS | S_PHASE_STATUS (0x18 | 0x03) and the target status byte
 * in TARGET_LUN.
 */
static void sgi_wd_start_message_xfer(SGIWD33C93State *state)
{
    uint32_t count = ((uint32_t)state->wd_regs[0x12] << 16) |
                     ((uint32_t)state->wd_regs[0x13] << 8) |
                     state->wd_regs[0x14];

    /* MAME load_transfer_count(): SBT forces 1, a zero count means 1. */
    if (state->wd_regs[0x18] & SGI_WD_COMMAND_SBT) {
        count = 1;
    } else if (!count) {
        count = 1;
    }
    if (!state->command_active) {
        /* MAME fatalerror(): TRANSFER_INFO requires initiator mode. */
        return;
    }
    state->message_phase = true;
    state->message_remaining = count;
    state->auxiliary_status &= ~0x01;           /* DBR */
    state->auxiliary_status |= 0x20;            /* BSY */
    state->auxiliary_status |= 0x01;            /* DBR: FIFO empty */

    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "SGIWDMSG start count=%u aux=%02x\n",
                count, state->auxiliary_status);
    }
    if (state->progress_timer) {
        timer_mod_ns(state->progress_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

static void sgi_wd_message_xfer_progress(SGIWD33C93State *state, uint8_t byte)
{
    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "SGIWDMSG byte=%02x remaining=%u\n",
                byte, state->message_remaining);
    }
    if (state->message_remaining) {
        state->message_remaining--;
    }
    sgi_wd_decrement_transfer_count(state);

    if (state->message_remaining) {
        return;
    }
    /*
     * Target switched to STATUS: report the phase and the target's status
     * byte.  A message-only transfer carries no command, so the target's
     * answer is GOOD unless it failed selection.
     */
    state->message_phase = false;
    state->auxiliary_status &= ~0x01;           /* DBR */
    state->auxiliary_status &= ~0x20;           /* BSY */
    state->wd_regs[0x0f] = 0;                   /* status byte: GOOD */
    state->wd_regs[0x10] = SGI_WD_PHASE_STATUS_RECEIVED;
    sgi_wd_irq_fifo_push(state, SGI_WD_STATUS_TRANSFER_SUCCESS |
                                  SGI_WD_PHASE_STATUS);
    state->scsi_status = sgi_wd_irq_fifo_peek(state);
    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "SGIWDMSG done -> %02x aux=%02x\n",
                state->scsi_status, state->auxiliary_status);
    }
    sgi_wd_update_irq(state);
}

static void sgi_wd_patch_byte(SGIWD33C93State *state, uint32_t va, uint8_t v)
{
    CPUState *cs = qemu_get_cpu(0);
    hwaddr a = sgi_wd_dma_address(state, va);
    if (cs && va >= 0x80000000U &&
        cpu_memory_rw_debug(cs, (vaddr)a, &v, 1, 1) == 0) {
        return;
    }
    MemTxResult r;
    hwaddr pa;

    /* Same split as the DMA data paths: unmapped below 0xc0000000. */
    if (cs && va >= 0xc0000000U) {
        hwaddr tpa = mips_cpu_get_phys_page_any_asid(cs, a);

        if (tpa != (hwaddr)-1) {
            pa = tpa;
        } else {
            pa = sgi_wd_pa(va);
        }
    } else {
        pa = sgi_wd_pa(va);
    }
    if (state->dma_control & 0x02) {
        pa ^= 3;
    }
    address_space_stb(&address_space_memory, pa, v,
                      MEMTXATTRS_UNSPECIFIED, &r);
}

static void sgi_wd_start_transfer(SGIWD33C93State *state)
{
    unsigned cdb_length = sgi_wd_cdb_length(state->wd_regs[3]);
    uint32_t transfer_count = (state->wd_regs[0x12] << 16) |
                               (state->wd_regs[0x13] << 8) |
                               state->wd_regs[0x14];
    unsigned i;

    /* MAME wd33c9x.cpp:1432-1448: SBT forces 1, zero count means 1. */
    if (state->wd_regs[0x18] & 0x80) {
        transfer_count = 1;
    } else if (!transfer_count) {
        transfer_count = 1;
    }

    /*
     * MAME couples the WD controller's data phase to its own transfer
     * count, never to the HPC3 DMA engine. A fresh command inherits no DMA
     * context unless the guest re-armed it here: an over-allocated probe
     * (INQUIRY etc.) leaves a residual dma_count after ENABLE is dropped,
     * and a following no-data command (e.g. TEST UNIT READY) would inherit
     * it and hang the FC completion gate. Drop stale DMA state when the
     * engine is idle.
     */
    if (!state->dma_active) {
        state->dma_count = 0;
        state->dma_current = 0;
    }

    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "Sending command t=%llu:%02x:%d:",
                (unsigned long long)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                state->wd_regs[3], cdb_length);
        for (i = 0; i < cdb_length; i++) {
            fprintf(stderr, " %02x", state->wd_regs[3 + i]);
        }
        fprintf(stderr, " (%u) dma_va=0x%x dma_count=%u\n",
                transfer_count, state->dma_current, state->dma_count);
    }

    esp_hard_reset(state->esp);
    state->command_active = true;
    state->dma_armed = false;
    state->dma_moved = 0;
    state->dma_got = 0;
    state->dma_dropped = 0;
    state->kick_traced = false;
    /* MAME wd33c9x.cpp:1051: SELECT enters initiator mode. */
    state->irq_initiator = true;
    /* MAME wd33c9x.cpp:733-735: Level II commands set BSY. */
    state->auxiliary_status = 0x80 | 0x20;
    sgi_wd_irq_fifo_reset(state);
    state->selection_status_pending = false;
    state->message_phase = false;
    state->message_remaining = 0;
    state->completion_phase = 0;
    state->scsi_status = 0;
    state->wd_regs[0x10] = 0x10;
    state->inq_patch_pending = false;
    if (state->wd_regs[3] == 0x12 && ((state->wd_regs[4] >> 5) & 0x7) == 0 &&
        state->wd_regs[5] == 0 && transfer_count == 64) {
        state->inq_patch_va = state->dma_current;
        state->inq_patch_pending = true;
        /* Board trace: standard INQUIRY LUN0 alloc-64 start */
        if (sgi_wd_trace_enabled()) {
            fprintf(stderr,
                    "INQ-START CDB %02x %02x %02x %02x %02x %02x alloc=%u copy_va=0x%x dmac=%u\n",
                    state->wd_regs[3], state->wd_regs[4], state->wd_regs[5],
                    state->wd_regs[6], state->wd_regs[7], state->wd_regs[8],
                    transfer_count, state->dma_current, state->dma_count);
        }
    }
    /* Board trace: all INQUIRY starts (incl VPD) */
    if (sgi_wd_trace_enabled() && state->wd_regs[3] == 0x12) {
        fprintf(stderr,
                "INQ-ALL CDB %02x %02x %02x %02x %02x %02x alloc=%u copy_va=0x%x\n",
                state->wd_regs[3], state->wd_regs[4], state->wd_regs[5],
                state->wd_regs[6], state->wd_regs[7], state->wd_regs[8],
                transfer_count, state->dma_current);
    }

    /*
     * Latch the transfer count directly.  esp.c:1303 esp_reg_write() drops
     * ESP_TCLO/TCMID/TCHI on the floor: it only clears STAT_TC and never
     * stores the value into either rregs or wregs, so esp_get_tc()
     * (esp.c:208) and esp_get_stc() (esp.c:225) both read 0 afterwards.
     * The ESP then has no idea how many bytes it is supposed to move:
     * esp_dma_ti_check()'s "defer until data is available" checks
     * (esp.c:628) are skipped, the DMA path is never taken for a read whose
     * data arrives late, and the command is completed by the esp.c:1024
     * CMD_SELATN branch with INTR_FC set and zero bytes transferred.
     *
     * wregs is what esp_get_stc() reads, so esp_run_cmd()'s DMA reload
     * (esp.c:1173) picks the count up when TI|DMA is written; rregs is what
     * the guest-visible read path and the decrement logic use.
     */
    state->esp->wregs[ESP_TCLO] = transfer_count;
    state->esp->wregs[ESP_TCMID] = transfer_count >> 8;
    state->esp->wregs[ESP_TCHI] = transfer_count >> 16;
    state->esp->rregs[ESP_TCLO] = transfer_count;
    state->esp->rregs[ESP_TCMID] = transfer_count >> 8;
    state->esp->rregs[ESP_TCHI] = transfer_count >> 16;
    state->esp->tchi_written = true;
    state->esp->rregs[ESP_RSTAT] &= ~STAT_TC;
    state->esp->wregs[ESP_WBUSID] = state->wd_regs[0x15] & BUSID_DID;
    /* MAME wd33c9x.cpp:1761-1763: IDENTIFY with ER bit from SOURCE_ID. */
    esp_reg_write(state->esp, ESP_FIFO, (state->wd_regs[0x0f] & 7) | 0x80 |
                                        ((state->wd_regs[0x16] & 0x80) ? 0x40 : 0));
    for (i = 0; i < cdb_length; i++) {
        esp_reg_write(state->esp, ESP_FIFO, state->wd_regs[3 + i]);
    }
    esp_reg_write(state->esp, ESP_CMD, CMD_SELATN);
    /*
     * Rebase the ESP transfer count on the length the target actually
     * negotiated for this CDB (ti_size), not the WD count registers
     * (0x12-0x14).  The whole SCSI negotiation above ran synchronously
     * inside the SELATN write, so ti_size is final here and no DMA has
     * moved yet (TI|DMA is only written below), making TC pristine.
     *
     * The WD count is not authoritative: the guest programs 0x3f000 for a
     * 512-block WRITE(10) whose CDB asks for 0x40000.  With TC 4096 short,
     * the last buffer arrives with TC < async_len; esp.c then moves a
     * partial tail, leaves async_len != 0 with TC == 0, never raises the
     * scsi_req_continue that would return the buffer, and the driver times
     * out (the same shortfall hung a 512-block READ(10) the other way).
     * ti_size is what the ESP decrements against TC, so drive TC from its
     * magnitude (negative for DATA OUT).  wregs must match: esp_run_cmd()
     * reloads TC from wregs for any command carrying CMD_DMA.
     */
    if (state->esp->ti_size) {
        uint32_t total = state->esp->ti_size < 0 ?
            (uint32_t)(-state->esp->ti_size) : (uint32_t)state->esp->ti_size;
        /*
         * TC stays on the negotiated length even when the WD count is
         * shorter: esp.c wedges with async_len left over when TC runs out
         * first (verified: capping TC hung the driver into a bus reset),
         * and MAME completes such commands normally, so a shortfall is
         * measured (dma_dropped) rather than manufactured or refused.
         */
        state->esp->wregs[ESP_TCLO] = total;
        state->esp->wregs[ESP_TCMID] = total >> 8;
        state->esp->wregs[ESP_TCHI] = total >> 16;
        state->esp->rregs[ESP_TCLO] = total;
        state->esp->rregs[ESP_TCMID] = total >> 8;
        state->esp->rregs[ESP_TCHI] = total >> 16;
        state->esp->rregs[ESP_RSTAT] &= ~STAT_TC;
        if (sgi_wd_trace_enabled()) {
            fprintf(stderr, "WDSYNC tc=%u ti_size=%d op=%02x\n",
                    total, state->esp->ti_size, state->wd_regs[3]);
        }
    }
    if (sgi_wd_wd_dma_mode(state) && state->dma_count && state->esp->async_len) {
        esp_reg_write(state->esp, ESP_CMD, CMD_TI | CMD_DMA);
        state->dma_armed = true;
    }
    /*
     * Progress WD/ESP via timer as well as polls: the QEMU equivalent of
     * MAME's update_step delay. Without it the kernel can park with no
     * further MMIO polls after programming DMA+WD.
     */
    if (state->progress_timer) {
        timer_mod_ns(state->progress_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

static void sgi_wd_update_status(SGIWD33C93State *state)
{
    if (!state->command_active) {
        /* MAME wd33c9x.cpp:968-972: FINISHED clears BSY. */
        state->auxiliary_status &= ~0x20;
        return;
    }
    if (state->message_phase) {
        /*
         * A TRANSFER_INFO message transfer never reaches the ESP: MAME drives
         * the bus directly from step() and only sets DBR, so leave the ESP
         * alone and keep DBR asserted while the guest still owes bytes.
         */
        state->auxiliary_status |= 0x01;
        sgi_wd_update_irq(state);
        return;
    }
    /*
     * Drive WD/ESP to quiescence in a single poll (like MAME's synchronous
     * DRQ drain): kick a pending transfer, collect target status, and report
     * completion, so one guest read that finds async data finishes everything
     * before returning. Bulk READs still gate on FC below.
     */
    for (int iter = 0; iter < 4; iter++) {
    if (sgi_wd_trace_enabled() && !state->kick_traced &&
        state->command_active) {
        state->kick_traced = true;
        fprintf(stderr,
                "WDGATE dmac=%u moved=%u async=%u ti_size=%d armed=%d mode=%d cmd=%02x rintr=%02x tc=%u descf=%08x dctl=%02x dact=%d op=%02x\n",
                state->dma_count, state->dma_moved, state->esp->async_len,
                state->esp->ti_size, state->dma_armed,
                sgi_wd_wd_dma_mode(state), state->esp->rregs[ESP_CMD],
                state->esp->rregs[ESP_RINTR],
                (state->esp->rregs[ESP_TCHI] << 16) |
                (state->esp->rregs[ESP_TCMID] << 8) |
                state->esp->rregs[ESP_TCLO],
                state->dma_desc_flags, state->dma_control, state->dma_active,
                state->wd_regs[3]);
    }
    /* For probe opcodes, run pending BH work inline so a single guest poll
     * that finds async==0 still completes in one pass. */
    {
        uint8_t poll_op = state->wd_regs[3];
        if (poll_op == 0x12 || poll_op == 0x1a || poll_op == 0x03) {
            aio_poll(qemu_get_aio_context(), false);
        }
    }
    /*
     * No completion is pushed on DMA drain alone: the spare status re-armed
     * the fifo and blocked the STATUS collect, hanging bulk READs.
     * Completion is reported exactly once, below, after the ESP raises it.
     */
    if (state->dma_count && state->esp->async_len && !state->dma_armed) {
        /*
         * Kick the ESP transfer once per command (MAME INIT_XFR) to cover the
         * case where the target's data only arrives *after* the command was
         * issued.  QEMU's esp.c runs the DMA path in esp_transfer_data()
         * (esp.c:1062) only when ESP_CMD already reads CMD_TI|CMDDMA when the
         * data lands; with a plain CMD_SELATN left in place it instead takes
         * the esp.c:1024 branch, sets INTR_BS|INTR_FC and the command
         * "completes" without moving a byte -- which is what made the kernel
         * get good statuses for reads that transferred nothing and then panic
         * with "vfs_mountroot: no root found".
         *
         * The kick must be latched in the model: ESP_CMD is write-only, so its
         * readback stays CMD_TI|CMDDMA forever and the old
         * `rregs[ESP_CMD] != (CMD_TI|CMD_DMA)` guard could never re-open once
         * the ESP had been kicked, dead-ending every later chunk.  One kick
         * per command is also what the original comment wanted (a second one
         * reloads TC and discards multi-chunk progress).
         *
         * No IRQ is pushed for DATA_IN: in DMA mode the chip asserts DRQ,
         * never an IRQ, and completion is reported exactly once below.
         * Polled CONTROL stays on the PIO DATA path (MAME wd33c9x RECV).
         */
        if (!sgi_wd_wd_dma_mode(state)) {
            break;
        }
        state->dma_armed = true;
        /* TC was already rebased on |ti_size| at issue time (see WDSYNC);
         * the kick only writes TI|DMA so esp_run_cmd() reloads it. */
        if (sgi_wd_trace_enabled()) {
            fprintf(stderr,
                    "WDKICK tc=%u ti_size=%d async=%u phase=%02x rseq=%02x rintr=%02x\n",
                    (state->esp->rregs[ESP_TCHI] << 16) |
                    (state->esp->rregs[ESP_TCMID] << 8) |
                    state->esp->rregs[ESP_TCLO],
                    state->esp->ti_size, state->esp->async_len,
                    state->esp->rregs[ESP_RSTAT],
                    state->esp->rregs[ESP_RSEQ],
                    state->esp->rregs[ESP_RINTR]);
        }
        esp_reg_write(state->esp, ESP_CMD, CMD_TI | CMD_DMA);
        continue;
    }
    if (state->command_active && !state->esp->async_len &&
               (state->esp->rregs[ESP_RINTR] & INTR_BS) &&
               !(state->esp->rregs[ESP_RINTR] & (INTR_FC | INTR_DC)) &&
               !state->irq_fifo_size &&
               (!state->dma_count || state->wd_regs[3] == 0x12 ||
                state->wd_regs[3] == 0x1a || state->wd_regs[3] == 0x03)) {
        /*
         * ESP stuck at bus-service with no DATA pending: collect target status
         * so the command can reach function-complete. A DMA residual is allowed
         * only for over-allocatable probes (INQUIRY/MODE SENSE/REQUEST SENSE);
         * bulk READs must drain first or a mid-transfer residual completes short.
         */
        esp_reg_write(state->esp, ESP_CMD, CMD_TI);
        continue;
    }
    if ((state->esp->rregs[ESP_RINTR] & INTR_DC) &&
               !state->irq_fifo_size) {
        sgi_wd_irq_fifo_push(state, SGI_WD_STATUS_SELECTION_TIMEOUT);
        state->wd_regs[0x10] = 0;
        /* QEMU ack of MAME's ARB_TIMEOUT_ABORT bus release. */
        esp_reg_read(state->esp, ESP_RINTR);
        state->command_active = false;
        state->auxiliary_status &= ~0x20;
        break;
    }
    if ((state->esp->rregs[ESP_RINTR] & INTR_FC) &&
               !(state->esp->rregs[ESP_RINTR] & INTR_DC) &&
               !state->irq_fifo_size && !state->esp->async_len &&
               fifo8_is_empty(&state->esp->fifo)) {
        /*
         * Function-complete with nothing queued: report SELECT_TRANSFER_SUCCESS
         * with COMMAND_COMPLETE (0x60) at once, like MAME wd33c9x (IRIX polls
         * 0x10 for 0x60 directly, so no intermediate phases). Requires FC, not
         * mere bus-service. Over-allocated probes complete GOOD with residual
         * and clear HPC3 ENABLE (which IRIX polls before accepting 0x60);
         * bulk READs gate on drained DMA/WD here.
         */
        if (!state->completion_phase) {
            uint32_t residCk = ((uint32_t)state->wd_regs[0x12] << 16) |
                               ((uint32_t)state->wd_regs[0x13] << 8) |
                               state->wd_regs[0x14];
            uint8_t opCk = state->wd_regs[3];
            if (sgi_wd_bulk_residual(state, opCk)) {
            /* Bulk with residual pending: more DATA to come, do not report
             * GOOD yet; completion re-raises with drained residual. */
            } else {
            state->completion_phase = 3;
            state->wd_regs[0x0f] = state->esp->status;
            sgi_wd_irq_fifo_push(state,
                                 SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS);
            state->wd_regs[0x10] = SGI_WD_PHASE_COMMAND_COMPLETE;
            /* Board trace: completion state at COMMAND_COMPLETE */
            if (sgi_wd_trace_enabled()) {
                fprintf(stderr,
                        "WDCOMP t=%llu resid=%.6x dma_count=%u moved=%u got=%u drop=%u flags=%08x ctl=%02x aux=%02x act=%d\n",
                        (unsigned long long)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                        residCk, state->dma_count, state->dma_moved,
                        state->dma_got, state->dma_dropped,
                        state->dma_desc_flags,
                        state->dma_control, state->auxiliary_status,
                        state->command_active);
            }
            /* MAME hd.cpp leaves INQUIRY byte7 0; QEMU Sync 0x10 fails IRIX length check. */
            if (state->inq_patch_pending && state->esp->status == 0) {
                static const uint8_t mame_vpd[28] = {
                    0x20, 0x53, 0x45, 0x41, 0x47, 0x41, 0x54, 0x45,
                    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                    0x20, 0x20, 0x53, 0x54, 0x32, 0x32, 0x35, 0x4e,
                    0x31, 0x2e, 0x30, 0x30,
                };
                int pi;
                sgi_wd_patch_byte(state, state->inq_patch_va + 7, 0x00);
                /* EXP: MAME hd returns response-format 1 (CCS); QEMU 2. */
                sgi_wd_patch_byte(state, state->inq_patch_va + 3, 0x01);
                /* EXP: MAME default vendor/product/revision strings. */
                for (pi = 0; pi < 28; pi++) {
                    sgi_wd_patch_byte(state, state->inq_patch_va + 8 + pi,
                                      mame_vpd[pi]);
                }
                state->inq_patch_pending = false;
                /* Board trace: dump COPY after byte7 patch */
                if (sgi_wd_trace_enabled()) {
                    CPUState *cs = qemu_get_cpu(0);
                    uint8_t b[64];
                    for (int ti = 0; ti < 64; ti++) {
                        uint8_t v = 0;
                        hwaddr va = sgi_wd_dma_address(state, state->inq_patch_va + ti);
                        if (cs && cpu_memory_rw_debug(cs, (vaddr)va, &v, 1, 0) == 0) {
                            b[ti] = v;
                        } else {
                            b[ti] = 0xee;
                        }
                    }
                    fprintf(stderr, "INQ-DONE copy_va=0x%x bytes:",
                            state->inq_patch_va);
                    for (int ti = 0; ti < 64; ti++) {
                        fprintf(stderr, " %02x", b[ti]);
                    }
                    fprintf(stderr, " ascii:");
                    for (int ti = 0; ti < 64; ti++) {
                        uint8_t c = b[ti];
                        fprintf(stderr, "%c", (c >= 0x20 && c < 0x7f) ? c : '.');
                    }
                    fprintf(stderr, "\n");
                }
                if (sgi_wd_trace_enabled()) {
                    /*
                     * DIAG: the INQ-DONE read-back above returns 0xee
                     * (cpu_memory_rw_debug missed).  Dump the soft-TLB
                     * entries matching the patch VA plus the current
                     * ASID (implemented in target/mips/system/physaddr.c),
                     * so the next traced run shows whether the kernel
                     * maps the buffer non-identity (data lands on the
                     * wrong page) and whether an ASID mismatch hides the
                     * mapping from the debug translation.
                     */
                    CPUState *cs = qemu_get_cpu(0);

                    if (cs) {
                        mips_dump_tlb_match(cs, state->inq_patch_va);
                    }
                }
                if (sgi_wd_trace_enabled()) {
                    {
                        hwaddr pbase = sgi_wd_pa(state->inq_patch_va);
                        uint8_t pb[64];
                        MemTxResult pr;

                        pr = address_space_read(&address_space_memory, pbase,
                                                MEMTXATTRS_UNSPECIFIED, pb,
                                                sizeof(pb));
                        fprintf(stderr, "INQ-PHYS pa=%08x rx=%d:",
                                (unsigned)pbase, pr != MEMTX_OK);
                        for (int ti = 0; ti < 64; ti++) {
                            fprintf(stderr, " %02x", pb[ti]);
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
            /*
             * With ENABLE cleared, the single poll shows ENABLE clear + 0x60
             * with no second poll needed. Bulk READs never enter here with
             * residual (gated above). An IRQ alone cannot wake the waiter
             * (IOC2 masks stay 0), so the polled bits must already be final.
             * MAME FINISHED clears BSY; the guest reads status next.
             */
            state->command_active = false;
            state->auxiliary_status &= ~0x20;
            /*
             * Chain consumed (MAME hpc3 decrement_chain EOX): drop ENABLE
             * and active so the next transfer refetches its descriptor.
             */
            if (state->dma_desc_flags & (1U << 29)) {
                state->dma_irq = true;
                sgi_wd_update_dma_irq(state);
            }
            state->dma_control &= ~0x10U;
            state->dma_active = false;
            if (state->esp->rregs[ESP_RINTR]) {
                esp_reg_read(state->esp, ESP_RINTR);
            }
            }
        }
    }
        break;
    }
    if ((state->scsi_status & SGI_WD_STATUS_REQ) &&
        !fifo8_is_empty(&state->esp->fifo)) {
        state->auxiliary_status |= 0x01;
    } else {
        state->auxiliary_status &= ~0x01;
    }
    sgi_wd_update_irq(state);
}

void sgi_wd33c93_poll(SGIWD33C93State *state)
{
    if (state) {
        sgi_wd_update_status(state);
    }
}

static void sgi_wd_progress_tick(void *opaque)
{
    SGIWD33C93State *state = opaque;

    sgi_wd_update_status(state);
    if (state->command_active && state->completion_phase != 3 &&
        state->progress_timer) {
        timer_mod_ns(state->progress_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

static uint64_t sgi_wd_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIWD33C93State *state = opaque;

    if (!state->esp) {
        return 0;
    }
    if ((addr & 0xff) == 3) {
        uint8_t aux;
        sgi_wd_update_status(state);
        /*
         * Mask INT while DBR is set (MAME wd33c9x indir_addr_r): during DRQ
         * drain the guest polls aux for DBR, and a spurious INT sends it
         * down the wrong path.
         */
        if (state->auxiliary_status & 0x01) {
            aux = state->auxiliary_status & 0x7f;
        } else {
            aux = state->auxiliary_status;
        }
        if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(0, 0x1f)) {
            fprintf(stderr, "WD33 REG R addr=1f val=%02x\n", aux);
        }
        return aux;
    }
    if ((addr & 0xff) != 7) {
        return 0;
    }
    if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(1, state->register_select)) {
        fprintf(stderr, "WD33 REG R addr=%02x\n", state->register_select & 0x1f);
    }
    switch (state->register_select & 0x1f) {
    case 0x17:
    {
        uint8_t status = sgi_wd_irq_fifo_peek(state);

        if (state->command_active &&
            (state->esp->rregs[ESP_RINTR] & INTR_FC) &&
            !(state->esp->rregs[ESP_RINTR] & INTR_DC) &&
            !state->esp->async_len &&
            fifo8_is_empty(&state->esp->fifo)) {
            /*
             * Transfer fully complete (STATUS phase, nothing pending): report
             * completion live instead of a stale queued data phase. Same
             * residual gating as the FC path: probes may complete with
             * residual, bulk READs must drain.
             */
            uint8_t opLive = state->wd_regs[3];
            if (sgi_wd_bulk_residual(state, opLive)) {
                /* Bulk with residual pending: fall through to normal
                 * fifo consume below; completion will report via the FC
                 * path once drained. */
            } else {
            status = SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS;
            state->scsi_status = status;
            state->auxiliary_status |= 0x80;
            state->auxiliary_status &= ~0x20;
            state->completion_phase = 3;
            state->wd_regs[0x0f] = state->esp->status;
            state->wd_regs[0x10] = SGI_WD_PHASE_COMMAND_COMPLETE;
            if (state->inq_patch_pending && state->esp->status == 0) {
                static const uint8_t mame_vpd[28] = {
                    0x20, 0x53, 0x45, 0x41, 0x47, 0x41, 0x54, 0x45,
                    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                    0x20, 0x20, 0x53, 0x54, 0x32, 0x32, 0x35, 0x4e,
                    0x31, 0x2e, 0x30, 0x30,
                };
                int pi;
                sgi_wd_patch_byte(state, state->inq_patch_va + 7, 0x00);
                /* EXP: MAME hd returns response-format 1 (CCS); QEMU 2. */
                sgi_wd_patch_byte(state, state->inq_patch_va + 3, 0x01);
                /* EXP: MAME default vendor/product/revision strings. */
                for (pi = 0; pi < 28; pi++) {
                    sgi_wd_patch_byte(state, state->inq_patch_va + 8 + pi,
                                      mame_vpd[pi]);
                }
                state->inq_patch_pending = false;
            }
            if (state->dma_desc_flags & (1U << 29)) {
                state->dma_irq = true;
                sgi_wd_update_dma_irq(state);
            }
            state->dma_control &= ~0x10U;
            state->dma_active = false;
            if (state->esp->rregs[ESP_RINTR]) {
                esp_reg_read(state->esp, ESP_RINTR);
            }
            if (state->irq_fifo_size) {
                sgi_wd_irq_fifo_pop(state);
            }
            if (state->selection_status_pending &&
                status == SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS) {
                state->selection_status_pending = false;
            }
            if (status == SGI_WD_STATUS_SELECTION_TIMEOUT) {
                state->command_active = false;
            }
            if (state->completion_phase == 3) {
                state->command_active = false;
            }
            sgi_wd_update_status(state);
            state->register_select = 0x18;
            if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(2, 0x17)) {
                fprintf(stderr, "WD33 REG R addr=17 val=%02x\n", status);
            }
            return status;
            }
        }
        if (state->auxiliary_status & 0x80) {
            state->auxiliary_status &= ~0x80;
        }
        if (state->irq_fifo_size) {
            sgi_wd_irq_fifo_pop(state);
        }
        /*
         * Consuming the status release the latch so the STATUS collect and
         * completion path can run afterwards (MAME wd33c9x update_irq pops
         * the next pending status on each SCSI Status read).
         */
        if (state->selection_status_pending && status == SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS) {
            state->selection_status_pending = false;
        }
        /*
         * Do NOT ack the ESP interrupt on status read: it is the completion
         * signal for transfers still in flight.
         */
        if (status == SGI_WD_STATUS_SELECTION_TIMEOUT) {
            state->command_active = false;
            state->auxiliary_status &= ~0x20;
        }
        if (state->completion_phase == 3) {
            state->command_active = false;
            state->auxiliary_status &= ~0x20;
        }
        sgi_wd_update_status(state);
        state->register_select = 0x18;
        return status;
    }
    case 0x19: {
        uint8_t value;

        if (!(state->auxiliary_status & 0x01)) {
            return 0;
        }
        value = esp_reg_read(state->esp, ESP_FIFO);
        if (fifo8_is_empty(&state->esp->fifo)) {
            state->auxiliary_status &= ~0x01;
        }
        sgi_wd_decrement_transfer_count(state);
        if (state->esp->async_len && !state->dma_count &&
            (state->wd_regs[0x12] | state->wd_regs[0x13] |
             state->wd_regs[0x14])) {
            esp_reg_write(state->esp, ESP_CMD, CMD_TI);
        } else if (!state->dma_count && state->command_active &&
                   fifo8_is_empty(&state->esp->fifo) &&
                   (state->esp->rregs[ESP_RINTR] & INTR_BS) &&
                   !(state->esp->rregs[ESP_RINTR] & INTR_DC) &&
                   (state->wd_regs[0x12] | state->wd_regs[0x13] |
                    state->wd_regs[0x14])) {
            /*
             * PIO mode: FIFO dry but transfer not done; pump the next bytes.
             */
            esp_reg_write(state->esp, ESP_CMD, CMD_TI);
        }
        return value;
    }
    case 0x1f:
        sgi_wd_update_status(state);
        return state->auxiliary_status;
    default:
        if (state->register_select < 0x18) {
            /*
             * IRIX polls COMMAND_PHASE directly for 0x60 and never issues
             * PROM-style status reads; drive progress here too (the QEMU
             * equivalent of HW DRQ). MAME SAT completes with 0x16/0x60 at
             * once, so no TARGET_LUN stepping here.
             */
            sgi_wd_update_status(state);
            uint8_t value = state->wd_regs[state->register_select];

            state->register_select = (state->register_select + 1) & 0x1f;
            if (sgi_wd_trace_enabled() &&
                sgi_wd_trace_wanted(2, state->register_select)) {
                fprintf(stderr, "WD33 REG RV addr=%02x val=%02x\n",
                        (state->register_select - 1) & 0x1f, value);
            }
            return value;
        }
        return 0;
    }
}

static void sgi_wd_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    SGIWD33C93State *state = opaque;

    if ((addr & 0xff) == 3) {
        if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(3, value)) {
            fprintf(stderr, "WD33 ADDR W %02x\n", (unsigned)(value & 0xff));
        }
        state->register_select = value & 0xff;
    } else if ((addr & 0xff) == 7) {
        uint8_t reg = state->register_select & 0x1f;

        if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(4, reg)) {
            fprintf(stderr, "WD33 REG W addr=%02x data=%02x\n",
                    reg, (unsigned)(value & 0xff));
        }
        if (reg == 0x18) {
            uint8_t cc = value & SGI_WD_COMMAND_CC;

            state->wd_regs[reg] = value;
            if (cc == SGI_WD_COMMAND_CC_RESET) {
                sgi_wd_reset_state(state);
            } else if (cc == SGI_WD_COMMAND_CC_SELECT_ATN_TRANSFER ||
                       cc == SGI_WD_COMMAND_CC_SELECT_TRANSFER) {
                sgi_wd_start_transfer(state);
            } else if (cc == SGI_WD_COMMAND_CC_SELECT_ATN ||
                       cc == SGI_WD_COMMAND_CC_SELECT) {
                sgi_wd_start_select(state);
            } else if (cc == SGI_WD_COMMAND_CC_TRANSFER_INFO) {
                sgi_wd_start_message_xfer(state);
            }
        } else if (reg == 0x19) {
            /*
             * MAME indir_reg_w DATA: push the byte and decrement the
             * transfer count.  Only meaningful while a TRANSFER_INFO
             * message transfer is open; the ESP-fronted data paths are
             * driven from the CDB and the HPC3 DMA engine instead.
             */
            if (state->message_phase) {
                sgi_wd_message_xfer_progress(state, value);
            }
        } else if (reg < 0x18) {
            state->wd_regs[reg] = value;
            state->register_select = (reg + 1) & 0x1f;
        }
    }
}

static const MemoryRegionOps sgi_wd_ops = {
    .read = sgi_wd_read,
    .write = sgi_wd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const Property sgi_wd_properties[] = {
    DEFINE_PROP_LINK("esp", SGIWD33C93State, esp, TYPE_ESP,
                     ESPState *),
};

static const VMStateDescription vmstate_sgi_wd = {
    .name = TYPE_SGI_WD33C93,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(wd_regs, SGIWD33C93State, 32),
        VMSTATE_UINT8(register_select, SGIWD33C93State),
        VMSTATE_UINT8(auxiliary_status, SGIWD33C93State),
        VMSTATE_UINT8(scsi_status, SGIWD33C93State),
        VMSTATE_BOOL(command_active, SGIWD33C93State),
        VMSTATE_BOOL(selection_status_pending, SGIWD33C93State),
        VMSTATE_BOOL(message_phase, SGIWD33C93State),
        VMSTATE_UINT32(message_remaining, SGIWD33C93State),
        VMSTATE_UINT8(completion_phase, SGIWD33C93State),
        VMSTATE_UINT8_ARRAY(irq_fifo, SGIWD33C93State, 8),
        VMSTATE_UINT8(irq_fifo_pos, SGIWD33C93State),
        VMSTATE_UINT8(irq_fifo_size, SGIWD33C93State),
        VMSTATE_BOOL(data_status_queued, SGIWD33C93State),
        VMSTATE_UINT32(dma_next_desc, SGIWD33C93State),
        VMSTATE_UINT32(dma_current, SGIWD33C93State),
        VMSTATE_BOOL(dma_armed, SGIWD33C93State),
    VMSTATE_UINT32(dma_count, SGIWD33C93State),
        VMSTATE_UINT32(dma_control, SGIWD33C93State),
        VMSTATE_UINT32(dma_desc_flags, SGIWD33C93State),
        VMSTATE_BOOL(dma_irq, SGIWD33C93State),
        VMSTATE_BOOL(dma_active, SGIWD33C93State),
        VMSTATE_BOOL(irq_level, SGIWD33C93State),
        VMSTATE_BOOL(irq_initiator, SGIWD33C93State),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_wd_realize(DeviceState *device, Error **errp)
{
    SGIWD33C93State *state = SGI_WD33C93(device);

    /* Completion INT output (MAME wd33c9x.h:17 irq_cb); board ORs it with ESP. */
    qdev_init_gpio_out(device, &state->irq, 1);
    /* HPC3 SCSI DMA-done (MAME hpc3 bit29); board ORs to IOC2 local1 0x10. */
    qdev_init_gpio_out_named(device, &state->dma_done_irq, "dma-done", 1);
    state->esp->dma_memory_read = sgi_wd_dma_read;
    state->esp->dma_memory_write = sgi_wd_dma_write;
    state->esp->dma_opaque = state;

    state->progress_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         sgi_wd_progress_tick, state);
    memory_region_init_io(&state->regs, OBJECT(device), &sgi_wd_ops, state,
                          "sgi-wd33c93.regs", 0x8000);
    sysbus_init_mmio(SYS_BUS_DEVICE(device), &state->regs);
    sgi_wd_reset_state(state);
}

static void sgi_wd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->realize = sgi_wd_realize;
    device_class->vmsd = &vmstate_sgi_wd;
    device_class_set_legacy_reset(device_class, sgi_wd_reset);
    device_class_set_props(device_class, sgi_wd_properties);
}

static const TypeInfo sgi_wd_type = {
    .name = TYPE_SGI_WD33C93,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIWD33C93State),
    .class_init = sgi_wd_class_init,
};

static void sgi_wd_register_types(void)
{
    type_register_static(&sgi_wd_type);
}

type_init(sgi_wd_register_types)

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/aio.h"
#include "qemu/main-loop.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/scsi/sgi_wd33c93.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
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
#define SGI_WD_STATUS_UNEXPECTED_SEND_DATA 0x49
#define SGI_WD_STATUS_REQ 0x88
#define SGI_WD_PHASE_TRANSFER_COUNT 0x46
#define SGI_WD_PHASE_COMMAND 0x02
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
    /*
     * Native WD33C93 SCSI target engine.
     *
     * This driver used to delegate command execution to esp.c, which models
     * the AMD/NCR 53C93A -- a different chip.  The fields below replace it
     * outright: the bus, the in-flight request, the internal transfer
     * limiter and the window into the SCSI layer's own buffer.  Nothing here
     * mirrors the ESP; the WD33C93A datasheet is the contract and MAME's
     * wd33c9x.cpp is the behavioural reference.
     */
    SCSIBus bus;
    SCSIDevice *target;
    SCSIRequest *scsi_req;
    int target_xfer;         /* >0 bytes still to read, <0 still to write */
    uint32_t target_tc;      /* internal limiter: |target_xfer| at issue */
    uint8_t *async_buf;      /* window into the SCSI layer's buffer */
    int async_len;           /* valid bytes in that window */
    bool data_ready;         /* SCSI layer has a chunk waiting */
    bool command_done;       /* target reported a final status */
    bool no_target;          /* selection found no device at DESTINATION_ID */
    bool xfer_paused;        /* HPC chain exhausted mid-transfer, await resume */
    uint8_t paused_tid;      /* target the paused transfer belongs to */
    uint8_t last_message;    /* last IDENTIFY accepted from the initiator */
    uint32_t max_xfer;       /* internal per-command transfer limit */
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
    uint8_t pio_log[512];
    uint16_t pio_n;
    bool selection_timeout_armed;
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
    QEMUBH *complete_bh;
    uint32_t inq_patch_va;
    uint32_t inq_patch_alloc;
    bool inq_patch_is_cd;
    bool inq_patch_have_va;
    bool inq_patch_pending;
    bool inq_patch_phantom;
};

static void sgi_wd_target_reset(SGIWD33C93State *state);

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

static void sgi_wd_reset_state(SGIWD33C93State *state, bool power_on)
{
    uint8_t own_id = power_on ? 0 : state->wd_regs[0];
    memset(state->wd_regs, 0, sizeof(state->wd_regs));
    /* MAME wd33c9x.cpp:421-424: 0x1a-0x1e read back 0xff. */
    memset(&state->wd_regs[0x1a], 0xff, 5);
    /* MAME wd33c9x.cpp:738-754: soft reset preserves OWN_ID, but a
     * power-on/MR reset clears it (iris power_on does the same). */
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
    state->pio_n = 0;
    state->dma_control = 0;
    state->dma_desc_flags = 0;
    state->dma_irq = false;
    state->dma_active = false;
    state->inq_patch_va = 0;
    state->inq_patch_alloc = 0;
    state->inq_patch_is_cd = false;
    state->inq_patch_have_va = false;
    state->inq_patch_pending = false;
    state->inq_patch_phantom = false;
    state->selection_timeout_armed = false;
    if (state->progress_timer) {
        timer_del(state->progress_timer);
    }
    /*
     * A bus reset must not leave a SCSI request behind: without this, a
     * late transfer_data()/complete() from the orphaned request would land
     * in whatever command the guest issues next.
     */
    if (state->scsi_req) {
        scsi_req_cancel(state->scsi_req);
    }
    sgi_wd_irq_fifo_reset(state);
    /* MAME wd33c9x.cpp:751: RESET_EAF when OWN_ID EAF is set. */
    sgi_wd_irq_fifo_push(state, (own_id & 0x08) ? 0x01 : 0x00);
    sgi_wd_target_reset(state);
    sgi_wd_update_irq(state);
    sgi_wd_update_dma_irq(state);
}

static void sgi_wd_reset(DeviceState *device)
{
    sgi_wd_reset_state(SGI_WD33C93(device), true);
}

/* Board tracing is compiled out by default (zero run-time overhead).
 * Rebuild with -DSGI_DEBUG to re-enable the SGI_TRACE=1 environment toggle
 * for offline debugging. */
static bool sgi_wd_trace_enabled(void)
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

/* Teardown FLUSH stops the channel but raises no DMA IRQ: IRIX acks the
 * transfer's real IRQ and writes FLUSH to clean up, so firing here leaves
 * the bit stuck (IRQ storm). MAME raises on bit 29; the working iris
 * model deliberately does not.
 */
static void sgi_wd_dma_flush(SGIWD33C93State *state)
{
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

/*
 * Batched DMA copy. The per-byte loops below translate and store one byte at
 * a time. When the byte-lane swizzle is off, a run of bytes that stays in one
 * 4 KiB page of guest virtual address space is contiguous in physical memory,
 * so it can be moved with one memory access. Descriptor bookkeeping is applied
 * once per run. Runs never cross a page or a descriptor boundary; any byte
 * that needs the descriptor walk, a fault, or swizzled lanes goes through the
 * original per-byte code.
 */
static uint32_t sgi_wd_dma_run_len(SGIWD33C93State *state, int len,
                                   uint32_t count_limit)
{
    uint32_t n = MIN((uint32_t)len, count_limit);

    return MIN(n, 0x1000u - (state->dma_current & 0xfffu));
}

static void sgi_wd_dma_run_account(SGIWD33C93State *state, uint32_t n)
{
    uint32_t i;

    state->dma_current += n;
    state->dma_got += n;
    state->dma_count -= n;
    for (i = 0; i < n; i++) {
        sgi_wd_dma_bump_wd_count(state);
    }
}

/* Value of an environment switch: unset, empty or "0" is false, any other
 * number is true. */
static bool sgi_env_flag(const char *name)
{
    const char *e = getenv(name);

    return e && *e && strtoul(e, NULL, 0) != 0;
}

/* SGI_WD_SYNC=1 disables the batched run fast path, forcing every byte
 * through the original per-byte code; SGI_WD_SYNC=0 (the default) keeps it. */
static bool sgi_wd_sync_enabled(void)
{
    static int cached = -1;

    if (cached < 0) {
        cached = sgi_env_flag("SGI_WD_SYNC") ? 1 : 0;
    }
    return cached != 0;
}

/* Returns the number of bytes moved (0 = not handled, use per-byte path). */
static uint32_t sgi_wd_dma_write_run(SGIWD33C93State *state, CPUState *cs,
                                     uint8_t *buf, int len)
{
    uint32_t va = state->dma_current;
    uint32_t n;

    if (sgi_wd_sync_enabled() || (state->dma_control & 0x02) ||
        sgi_wd_trace_enabled()) {
        return 0;
    }
    n = sgi_wd_dma_run_len(state, len, state->dma_count);
    if (n < 2) {
        return 0;
    }
    if (cs && va >= 0x80000000U) {
        if (cpu_memory_rw_debug(cs, (vaddr)va, buf, n, 1) != 0) {
            return 0;
        }
    } else {
        address_space_write(&address_space_memory, sgi_wd_pa(va),
                            MEMTXATTRS_UNSPECIFIED, buf, n);
    }
    sgi_wd_dma_run_account(state, n);
    return n;
}

/* Read direction: stop one byte short of the count so the zero crossing
 * (IRQ, descriptor fetch, EOX) is still handled by the per-byte code. */
static uint32_t sgi_wd_dma_read_run(SGIWD33C93State *state, CPUState *cs,
                                    uint8_t *buf, int len)
{
    uint32_t va = state->dma_current;
    uint32_t n;

    if (sgi_wd_sync_enabled() || (state->dma_control & 0x02) ||
        sgi_wd_trace_enabled() || state->dma_count < 2) {
        return 0;
    }
    n = sgi_wd_dma_run_len(state, len, state->dma_count - 1);
    if (n < 2) {
        return 0;
    }
    if (cs && va >= 0x80000000U) {
        if (cpu_memory_rw_debug(cs, (vaddr)va, buf, n, 0) != 0) {
            return 0;
        }
    } else {
        address_space_read(&address_space_memory, sgi_wd_pa(va),
                           MEMTXATTRS_UNSPECIFIED, buf, n);
    }
    sgi_wd_dma_run_account(state, n);
    return n;
}

static int sgi_wd_dma_read(void *opaque, uint8_t *buf, int len)
{
    SGIWD33C93State *state = opaque;
    CPUState *cs = qemu_get_cpu(0);

    /*
     * MAME hpc3 do_scsi_dma/decrement_chain order: move the byte first,
     * then decrement the 16-bit count and act when it reaches zero.  A
     * freshly fetched zero count therefore underflows to 0xffff and keeps
     * moving instead of ending the chain a descriptor early; only EOX
     * (bit 31) seen at a zero crossing stops the walk.
     */
    int want = len;
    uint32_t start_va = state->dma_current;

    while (len > 0) {
        uint32_t run = sgi_wd_dma_read_run(state, cs, buf, len);
        if (run) {
            buf += run;
            len -= run;
            continue;
        }
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
                /* Raw VA: the lane swizzle applies once, to the physical
                 * result below (MAME hpc3 do_scsi_dma). */
                hwaddr tpa = mips_cpu_get_phys_page_any_asid(
                    cs, state->dma_current);

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
        state->dma_got++;
        len--;
        sgi_wd_dma_bump_wd_count(state);
        /* 16-bit count like MAME's m_count: 0 underflows to 0xffff. */
        state->dma_count = (state->dma_count - 1) & 0xffff;
        if (state->dma_count == 0) {
            /* MAME hpc3.cpp:967-972: IRQ on bit 29 at chain end. */
            if (state->dma_desc_flags & (1U << 29)) {
                state->dma_irq = true;
                sgi_wd_update_dma_irq(state);
            }
            if (state->dma_desc_flags & (1U << 31)) {
                state->dma_control &= ~0x10U;
                state->dma_active = false;
                break;
            }
            sgi_wd_dma_fetch_descriptor(state);
        }
    }
    /*
     * Event-driven completion: bytes just moved, so translate completion
     * in a bottom half instead of waiting for the next poll or timer tick
     * (MAME drains synchronously on DRQ). Scheduling from here is safe
     * (the BH runs later, never nested in ESP), and update_status is a
     * no-op unless ESP shows completion.
     */
    if (len != want && state->command_active && state->complete_bh) {
        qemu_bh_schedule(state->complete_bh);
    }
    /*
     * One line per ESP request, not per byte.  The XFS log is read as a
     * handful of very large transfers, so a short (len != 0) line here
     * means the chain could not place every requested byte.
     */
    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "DMARD va=%08x want=%d moved=%d short=%d "
                "ctl=%02x cnt=%04x bcf=%08x got=%u drop=%u\n",
                (unsigned)start_va, want, want - len, len,
                state->dma_control, state->dma_count,
                state->dma_desc_flags, state->dma_got,
                state->dma_dropped + len);
    }
    /* Bytes the ESP asked for but the chain could not place. */
    state->dma_dropped += len;
    return want - len;
}

static int sgi_wd_dma_write(void *opaque, uint8_t *buf, int len)
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
    /*
     * The HPC descriptor chain is authoritative: bytes are placed only
     * while sgi_wd_dma_next() walks a live chain.  Topping dma_count up to
     * the SCSI layer's request (as the old ESP glue did) walks linearly
     * past chain end into unmapped guest memory instead of pausing for the
     * driver to re-arm DMA, which is how over-long transfers used to
     * corrupt the pages after the buffer.  A short return means EOX with
     * target data still pending; the pump turns that into a pause.
     */
    /*
     * Latch the INQUIRY buffer start: the first DMA write of the command
     * runs at the buffer start by construction, so this VA cannot be stale
     * the way a start-of-command sample can.
     */
    if (state->inq_patch_pending && !state->inq_patch_have_va &&
        state->wd_regs[3] == 0x12 && len > 0) {
        state->inq_patch_va = state->dma_current;
        state->inq_patch_have_va = true;
    }
    int want = len;
    state->dma_moved += len;
    while (len > 0 && sgi_wd_dma_next(state)) {
        uint32_t run = sgi_wd_dma_write_run(state, cs, buf, len);
        if (run) {
            buf += run;
            len -= run;
            continue;
        }
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
                /* Raw VA: single post-translation swizzle, as above. */
                hwaddr tpa = mips_cpu_get_phys_page_any_asid(
                    cs, state->dma_current);

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
    /*
     * Event-driven completion: bytes just moved, so translate completion
     * in a bottom half instead of waiting for the next poll or timer tick
     * (MAME drains synchronously on DRQ). Scheduling from here is safe
     * (the BH runs later, never nested in ESP), and update_status is a
     * no-op unless ESP shows completion.
     */
    if (len != want && state->command_active && state->complete_bh) {
        qemu_bh_schedule(state->complete_bh);
    }
    /* Bytes the ESP asked for but the chain could not place. */
    state->dma_dropped += len;
    return want - len;
}

static void sgi_wd_update_status(SGIWD33C93State *state);
static void sgi_wd_target_continue(SGIWD33C93State *state);
static void sgi_wd_drop_reset_ua(SGIWD33C93State *state);

/* WD33C93A Data Sheet: register 0x15 bits 0-2 are the destination ID. */
#define BUSID_DID 0x07

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
        /*
         * MAME hpc3.cpp:438 drives DMA at the end of this write when DRQ is
         * already asserted. Drive one status poll too, so a program that
         * enables DMA and then parks without further reads still kicks
         * TI|DMA instead of waiting out the backstop timer.
         */
        sgi_wd_update_status(state);
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

static unsigned sgi_wd_cdb_length(const SGIWD33C93State *state,
                                  uint8_t opcode)
{
    /*
     * MAME wd33c9x.cpp set_command_length: with EAF set (OWN_ID bit 3) a
     * Select-And-Transfer takes the command length the host just loaded
     * into OWN_ID; IRIX rewrites it per command (0x06 before 6-byte CDBs,
     * 0x0a before 10-byte ones).  Datasheet Advanced-Mode rule: "the host
     * also loads the expected command length into the OWN ID register...
     * The WD33C93A uses this value."  Without this every undefined-group
     * opcode (READ TOC 0x43, vendor 0x46) decoded as 6 bytes below and was
     * truncated on the wire: READ TOC lost its allocation byte, the target
     * answered with zero data, and the guest's transfer count never
     * drained.
     */
    if (state->wd_regs[0] & 0x08) {
        return state->wd_regs[0] & 0x0f;
    }
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
static void sgi_wd_start_select(SGIWD33C93State *state, uint8_t cc)
{
    uint8_t dest = state->wd_regs[0x15] & BUSID_DID;
    bool present = scsi_device_find(&state->bus, 0, dest, 0) != NULL;
    uint8_t status = present ? SGI_WD_STATUS_SELECT_SUCCESS
                             : SGI_WD_STATUS_SELECTION_TIMEOUT;
    bool atn = cc == SGI_WD_COMMAND_CC_SELECT_ATN;
    sgi_wd_drop_reset_ua(state);

    state->command_active = true;
    state->dma_armed = false;
    state->irq_initiator = true;
    state->auxiliary_status |= 0x20;            /* BSY */
    state->completion_phase = 0;
    state->message_phase = false;
    state->message_remaining = 0;
    sgi_wd_irq_fifo_reset(state);
    if (present) {
        sgi_wd_irq_fifo_push(state, status);
        /*
         * MAME queues REQ with the live transfer phase: MESSAGE OUT
         * (IDENTIFY) for ATN selections, COMMAND phase for bare SELECT
         * (iris branches the same way).
         */
        sgi_wd_irq_fifo_push(state, SGI_WD_STATUS_REQ |
                             (atn ? SGI_WD_PHASE_MSG_OUT
                                  : SGI_WD_PHASE_COMMAND));
        state->scsi_status = status;
    } else {
        /*
         * MAME ARB_TIMEOUT_BUSY waits ~1000 SCSI cycles before giving up;
         * reporting the timeout instantly changes how the PROM paces its
         * missing-device retries. Hold BSY and report on the progress tick.
         */
        state->selection_timeout_armed = true;
        if (state->progress_timer) {
            timer_mod_ns(state->progress_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        }
    }
    state->selection_status_pending = false;

    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "SGIWDSEL dest=%u %s -> %02x aux=%02x\n",
                dest, present ? "found" : "missing", status,
                state->auxiliary_status);
    }
    if (present) {
        /* MAME FINISHED clears CIP and BSY before interrupts are reported. */
        state->auxiliary_status &= ~0x20;
        sgi_wd_update_irq(state);
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
    state->selection_timeout_armed = false;
    state->auxiliary_status &= ~0x01;           /* DBR */
    state->auxiliary_status |= 0x20;            /* BSY */
    state->auxiliary_status |= 0x01;            /* DBR: FIFO empty */

    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "SGIWDMSG start count=%u aux=%02x\n",
                count, state->auxiliary_status);
    }
    /* No timer: a TRANSFER_INFO transfer only moves on guest DATA writes,
     * which drive progress themselves; a tick here is a no-op. */
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
        /* Raw VA: single post-translation swizzle, as in the DMA loops. */
        hwaddr tpa = mips_cpu_get_phys_page_any_asid(cs, va);

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

/*
 * Standard INQUIRY page 0 identity, as MAME returns it for indy_4020.
 * Disk: MAME nscsi hd.cpp built-in Seagate defaults (vendor " SEAGATE",
 * product "          ST225N", revision "1.00", response format 1/CCS).
 * CD: indy attaches NSCSI_CDROM_SGI, Sony "CDU-76S"
 * revision "1.0", response format 2 (SPC-3). Bounded by the initiator's
 * allocation so short allocations cannot scribble past the buffer.
 */
typedef struct SGIWDInquiryIdentity {
    uint8_t format;
    uint8_t id[28];
} SGIWDInquiryIdentity;

static const SGIWDInquiryIdentity sgi_wd_mame_inq_hd = {
    .format = 0x01,
    .id = {
        0x20, 0x53, 0x45, 0x41, 0x47, 0x41, 0x54, 0x45,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x53, 0x54, 0x32, 0x32, 0x35, 0x4e,
        0x31, 0x2e, 0x30, 0x30,
    },
};

static const SGIWDInquiryIdentity sgi_wd_mame_inq_cd = {
    .format = 0x02,
    .id = {
        0x53, 0x6f, 0x6e, 0x79, 0x20, 0x20, 0x20, 0x20,
        0x43, 0x44, 0x55, 0x2d, 0x37, 0x36, 0x53, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x31, 0x2e, 0x30, 0x20,
    },
};

static void sgi_wd_patch_inquiry(SGIWD33C93State *state)
{
    const SGIWDInquiryIdentity *inq;
    uint32_t limit = state->inq_patch_alloc;
    uint32_t i;

    if (!state->inq_patch_pending || !state->inq_patch_have_va) {
        return;
    }
    inq = state->inq_patch_is_cd ? &sgi_wd_mame_inq_cd : &sgi_wd_mame_inq_hd;
    if (limit > 36) {
        limit = 36;
    }
    if (limit > 0 && state->inq_patch_phantom) {
        /* MAME nscsi hd.cpp:162-163: peripheral qualifier 3, device type
         * 0x1f -> byte 0 = 0x7f, "logical unit not present". */
        sgi_wd_patch_byte(state, state->inq_patch_va + 0, 0x7f);
    }
    if (limit > 3) {
        sgi_wd_patch_byte(state, state->inq_patch_va + 3, inq->format);
    }
    if (limit > 7) {
        sgi_wd_patch_byte(state, state->inq_patch_va + 7, 0x00);
    }
    for (i = 0; i < 28 && 8 + i < limit; i++) {
        sgi_wd_patch_byte(state, state->inq_patch_va + 8 + i, inq->id[i]);
    }
    state->inq_patch_pending = false;
    state->inq_patch_phantom = false;
}

/*
 * MAME's disk never reports power-on unit attention (its reset sets none;
 * only medium-change does, impossible with fixed media). QEMU latches
 * UA(RESET) at machine reset, so the first command would CHECK CONDITION
 * where MAME succeeds. Drop it on every command issue: both the split
 * SELECT path and the combined SELECT_ATN_TRANSFER path must clear it,
 * since the PROM uses the combined form.
 */
static void sgi_wd_drop_reset_ua(SGIWD33C93State *state)
{
    uint8_t dest = state->wd_regs[0x15] & BUSID_DID;
    SCSIDevice *dev = scsi_device_find(&state->bus, 0, dest, 0);

    if (dev) {
        dev->unit_attention = SENSE_CODE(NO_SENSE);
    }
}

/*
 * ===========================================================================
 * Native WD33C93 target engine
 *
 * The only thing these routines share with the old esp.c arrangement is the
 * byte mover: sgi_wd_dma_read()/sgi_wd_dma_write() shuttle bytes between guest
 * memory and the buffer the SCSI layer owns.  Ownership of phases, byte
 * counting and completion lives here, in the WD33C93's own terms.
 * ===========================================================================
 */

/*
 * Move one chunk of the data phase.
 *
 * The SCSI layer hands us a window into its own buffer (async_buf/async_len)
 * whenever it has bytes ready; we copy between that window and guest memory
 * and then let the layer refill.  The WD33C93 owns the byte count: every byte
 * moved decrements the transfer counter and the phase ends when it reaches
 * zero (WD33C93A datasheet, TRANSFER COUNT registers auto-decrement).
 */
static void sgi_wd_target_pump(SGIWD33C93State *state)
{
    int moved = 0;

    if (!state->scsi_req || !state->data_ready) {
        return;
    }
    /*
     * While paused the driver owns the next move: it must re-arm HPC DMA
     * and resume with SELECT_ATN_XFER.  Polling register reads reach here
     * constantly; returning silently avoids re-pausing (and re-tracing)
     * until the resume clears the flag.
     */
    if (state->xfer_paused) {
        return;
    }

    while (state->async_len > 0 && state->target_tc > 0 &&
           state->target_xfer != 0) {
        int outstanding = state->target_xfer < 0 ?
                          -state->target_xfer : state->target_xfer;
        int n;

        n = MIN(state->async_len, (int)state->target_tc);
        n = MIN(n, outstanding);
        if (n <= 0) {
            break;
        }
        int placed;

        if (state->target_xfer < 0) {
            /* DATA OUT: guest memory into the SCSI layer's buffer. */
            placed = sgi_wd_dma_read(state, state->async_buf, n);
            state->target_xfer += placed;
        } else {
            /* DATA IN: the SCSI layer's buffer out to guest memory. */
            placed = sgi_wd_dma_write(state, state->async_buf, n);
            state->target_xfer -= placed;
        }
        state->async_buf += placed;
        state->async_len -= placed;
        state->target_tc -= placed;
        moved += placed;
        /*
         * The WD count registers are bumped per byte inside the DMA
         * movers, so nothing more to do here for them.
         */
        if (placed < n) {
            /*
             * HPC chain exhausted (EOX) with target data still pending.
             * Pause like the hardware instead of walking past chain end:
             * keep the in-flight request and the remaining window, and wait
             * for the driver to re-arm HPC DMA and resume with
             * SELECT_ATN_XFER.  The HPC EOX interrupt (bit 29) already
             * fired from dma_next(), which is the driver's cue.  Do NOT
             * ask the SCSI layer for more and do NOT report completion.
             */
            state->xfer_paused = true;
            state->paused_tid = state->wd_regs[0x15] & BUSID_DID;
            /*
             * Tell the driver the transfer stalled mid-phase (Iris raises
             * TRANSFER_COUNT/UNEXPECTED_SEND_DATA here): its unex_info()
             * re-arms HPC DMA for the next chunk and resumes with
             * SELECT_ATN_XFER.  The HPC chain terminator carries no
             * interrupt bit, so without this the driver would never resume.
             */
            state->wd_regs[0x10] = SGI_WD_PHASE_TRANSFER_COUNT;
            sgi_wd_irq_fifo_push(state,
                                 SGI_WD_STATUS_UNEXPECTED_SEND_DATA);
            /*
             * Drop BSY (and CIP) with the interrupt.  handle_intr() spins
             * on CIP|BSY before reading registers; holding BSY while paused
             * wedges it forever since completion never comes.  Iris leaves
             * only INT set here (ASR reads 0x80).  Re-asserted on resume.
             * command_active stays true: the SCSI request is still open.
             */
            state->auxiliary_status &= ~0x30;
            sgi_wd_update_irq(state);
            if (sgi_wd_trace_enabled()) {
                fprintf(stderr,
                        "WDPAUSE op=%02x placed=%d want=%d xfer=%d tc=%u async=%d\n",
                        state->wd_regs[3], placed, n, state->target_xfer,
                        state->target_tc, state->async_len);
            }
            break;
        }
    }

    if (moved && state->command_active && state->complete_bh) {
        qemu_bh_schedule(state->complete_bh);
    }
    /*
     * Window drained: ask the SCSI layer for the next chunk.  It answers
     * either by calling transfer_data() again or, once the command is done,
     * by reaching complete().  Never while paused: the request stays open
     * until the driver resumes us.
     */
    if (!state->xfer_paused && state->async_len <= 0 && state->scsi_req) {
        state->data_ready = false;
        scsi_req_continue(state->scsi_req);
    }
}

static void sgi_wd_target_continue(SGIWD33C93State *state)
{
    if (state->scsi_req) {
        scsi_req_continue(state->scsi_req);
    }
}

static void sgi_wd_target_reset(SGIWD33C93State *state)
{
    state->target = NULL;
    state->scsi_req = NULL;
    state->async_buf = NULL;
    state->async_len = 0;
    state->data_ready = false;
    state->command_done = false;
    state->no_target = false;
    state->xfer_paused = false;
    state->target_xfer = 0;
    state->target_tc = 0;
    state->last_message = 0;
}

static void sgi_wd33c93_transfer_data(SCSIRequest *req, uint32_t len)
{
    SGIWD33C93State *state = req->hba_private;

    /*
     * Only latch the window; do not pump from here.  The SCSI layer calls
     * this synchronously from scsi_req_continue(), which sgi_wd_target_pump()
     * calls once the window drains -- pumping here would recurse until the
     * stack ran out.  The pump is the single driver of the data phase and
     * runs from sgi_wd_update_status() on the guest's next register access.
     */
    state->async_buf = scsi_req_get_buf(req);
    state->async_len = len;
    state->data_ready = true;
}

static void sgi_wd33c93_command_complete(SCSIRequest *req, size_t resid)
{
    SGIWD33C93State *state = req->hba_private;
    bool active = state->command_active;

    state->scsi_req = NULL;
    state->async_buf = NULL;
    state->async_len = 0;
    state->data_ready = false;
    state->target_xfer = 0;
    state->target_tc = 0;
    if (!active) {
        /* Late completion after a bus reset: drop it, do not latch status. */
        return;
    }
    /*
     * MAME wd33c9x.cpp: the target's status byte lands in the TARGET/LUN
     * register (0x0f) before the command phase reports completion.
     */
    state->wd_regs[0x0f] = req->status;
    state->command_done = true;
}

static void sgi_wd33c93_command_cancelled(SCSIRequest *req)
{
    SGIWD33C93State *state = req->hba_private;

    state->scsi_req = NULL;
    state->async_buf = NULL;
    state->async_len = 0;
    state->data_ready = false;
    state->target_xfer = 0;
    state->target_tc = 0;
    if (state->command_active) {
        state->command_done = true;
    }
}

static const struct SCSIBusInfo sgi_wd33c93_bus_info = {
    .tcq = false,
    .max_target = 8,
    .max_lun = 7,

    .transfer_data = sgi_wd33c93_transfer_data,
    .complete = sgi_wd33c93_command_complete,
    .cancel = sgi_wd33c93_command_cancelled,
};

/*
 * Issue one command to the selected target.
 *
 * The CDB comes straight out of the WD33C93 register file (0x03-0x0c hold
 * CDB bytes 0-3 and 4-9 respectively), which is how this controller presents
 * commands to its target -- there is no command FIFO to refill first.
 */
static void sgi_wd_target_start(SGIWD33C93State *state)
{
    uint8_t cdb[16];
    unsigned cdb_len = sgi_wd_cdb_length(state, state->wd_regs[3]);
    unsigned i;
    uint8_t tid = state->wd_regs[0x15] & BUSID_DID;
    int datalen;

    for (i = 0; i < cdb_len && i < sizeof(cdb); i++) {
        cdb[i] = state->wd_regs[3 + i];
    }
    /* MAME wd33c9x.cpp:1761-1763: IDENTIFY carries the LUN plus the
     * source-ID extended-residue bit. */
    state->last_message = (state->wd_regs[0x0f] & 7) | 0x80 |
                          ((state->wd_regs[0x16] & 0x80) ? 0x40 : 0);
    /*
     * MAME nscsi hd.cpp/cd.cpp: an INQUIRY against a LUN the device does
     * not have (only LUN 0 exists here) returns peripheral qualifier 0x7f
     * ("logical unit not present"), so the IRIX driver skips the phantom
     * and stops.  Non-zero identify LUN means the guest is probing a LUN
     * beyond the real one; flag it for the inquiry patch to mask byte 0.
     * The command itself still runs against the LUN-0 device so QEMU
     * produces a well-formed standard response to patch.
     */
    state->inq_patch_phantom = (state->last_message & 7) != 0;

    state->no_target = false;
    state->target = scsi_device_find(&state->bus, 0, tid, 0);
    if (!state->target) {
        /*
         * Nothing at this ID.  Do not complete the command here: use the
         * same deferred selection-timeout report as the split SELECT path,
         * so empty IDs look identical however the PROM selects them.
         */
        state->no_target = true;
        state->scsi_req = NULL;
        state->async_buf = NULL;
        state->async_len = 0;
        state->data_ready = false;
        state->target_xfer = 0;
        state->target_tc = 0;
        state->command_done = false;
        state->selection_timeout_armed = true;
        if (state->progress_timer) {
            timer_mod_ns(state->progress_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        }
        return;
    }

    state->scsi_req = scsi_req_new(state->target, 0, 0, cdb, cdb_len, state);
    datalen = scsi_req_enqueue(state->scsi_req);

    state->target_xfer = datalen;
    /*
     * The transfer counter this phase is bounded by is the length the target
     * actually negotiated for this CDB, not the value the guest programmed in
     * 0x12-0x14: the driver may arm a count that does not match the CDB, and
     * the count registers are only the guest-visible residual that
     * sgi_wd_decrement_transfer_count() walks down as bytes move.
     */
    state->target_tc = datalen < 0 ? (uint32_t)(-datalen) : (uint32_t)datalen;

    if (!state->scsi_req) {
        /* Enqueue completed the command outright (no data phase). */
        state->command_done = true;
    } else if (datalen != 0) {
        /*
         * Open the data phase.  Enqueue alone only tells us how many bytes
         * the target wants; without this continue() the SCSI layer never
         * hands over the first buffer, so the command never completes and
         * the initiator times out.  The continuation arrives synchronously:
         * either transfer_data() latches the window, or complete() fires for
         * a target that already has everything.
         */
        scsi_req_continue(state->scsi_req);
    }
    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "WDCMD op=%02x cdb_len=%u xfer=%d tc=%u tgt=%u\n",
                state->wd_regs[3], cdb_len, datalen, state->target_tc, tid);
    }
}

static void sgi_wd_start_transfer(SGIWD33C93State *state)
{
    unsigned cdb_length = sgi_wd_cdb_length(state, state->wd_regs[3]);
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
    /* A new command supersedes any pending selection timeout. */
    state->selection_timeout_armed = false;

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

    /*
     * Mid-transfer resume: the previous SELECT_ATN_XFER paused with the HPC
     * chain exhausted (WDPAUSE) and the driver has re-armed HPC DMA for the
     * next chunk.  A SELECT_ATN_XFER to the same target continues the
     * in-flight SCSI request instead of starting a new command: keep the
     * request, the remaining window and the transfer counters, and just pump
     * against the freshly programmed DMA chain.  In particular the DMA
     * context above must be left alone (the driver just programmed it) and
     * no new request may be created.
     */
    {
        uint8_t resume_tid = state->wd_regs[0x15] & BUSID_DID;

        if (state->xfer_paused && state->scsi_req &&
            resume_tid == state->paused_tid) {
            state->xfer_paused = false;
            state->command_active = true;
            state->auxiliary_status |= 0x20;
            state->kick_traced = false;
            if (sgi_wd_trace_enabled()) {
                fprintf(stderr,
                        "WDRESUME op=%02x xfer=%d tc=%u async=%d tgt=%u\n",
                        state->wd_regs[3], state->target_xfer,
                        state->target_tc, state->async_len, resume_tid);
            }
            sgi_wd_target_pump(state);
            state->dma_armed = sgi_wd_wd_dma_mode(state) &&
                               state->dma_count && state->target_xfer != 0;
            if (state->progress_timer) {
                timer_mod_ns(state->progress_timer,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            }
            return;
        }
        if (state->xfer_paused) {
            /*
             * A new command arrived for a different target (or with no
             * request) while paused: the disconnected transfer cannot be
             * resumed later by this engine, so drop it loudly rather than
             * leak the request or mix two commands' state.
             */
            if (sgi_wd_trace_enabled()) {
                fprintf(stderr,
                        "WDRESUME-ABANDON paused_tgt=%u new_tgt=%u op=%02x\n",
                        state->paused_tid, resume_tid, state->wd_regs[3]);
            }
            if (state->scsi_req) {
                scsi_req_cancel(state->scsi_req);
            }
            sgi_wd_target_reset(state);
        }
    }

    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "Sending command t=%llu:%02x:%d:tgt=%u:",
                (unsigned long long)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                state->wd_regs[3], cdb_length,
                state->wd_regs[0x15] & BUSID_DID);
        for (i = 0; i < cdb_length; i++) {
            fprintf(stderr, " %02x", state->wd_regs[3 + i]);
        }
        fprintf(stderr, " (%u) dma_va=0x%x dma_count=%u\n",
                transfer_count, state->dma_current, state->dma_count);
    }

    sgi_wd_drop_reset_ua(state);
    state->command_active = true;
    state->dma_armed = false;
    state->dma_moved = 0;
    state->dma_got = 0;
    state->dma_dropped = 0;
    state->pio_n = 0;
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
    state->inq_patch_have_va = false;
    state->inq_patch_phantom = false;
    /*
     * Standard INQUIRY page 0, any LUN (hinv and the kernel use less than
     * 64). Bit 0 of CDB byte 1 is EVPD: a VPD request must keep its
     * vital-product-data payload. The buffer address is NOT captured here
     * (dma_current may be stale for this command); it is latched from the
     * first DMA write callback, which runs at the buffer start by
     * construction.
     */
    if (state->wd_regs[3] == 0x12 && (state->wd_regs[4] & 0x01) == 0 &&
        state->wd_regs[5] == 0) {
        state->inq_patch_alloc = transfer_count;
        {
            uint8_t target_id = state->wd_regs[0x15] & BUSID_DID;
            SCSIDevice *dev = scsi_device_find(&state->bus, 0,
                                               target_id, 0);
            state->inq_patch_is_cd = dev && dev->type == TYPE_ROM;
        }
        state->inq_patch_pending = true;
        if (sgi_wd_trace_enabled()) {
            fprintf(stderr,
                    "INQ-START CDB %02x %02x %02x %02x %02x %02x alloc=%u\n",
                    state->wd_regs[3], state->wd_regs[4], state->wd_regs[5],
                    state->wd_regs[6], state->wd_regs[7], state->wd_regs[8],
                    transfer_count);
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
     * Hand the command to the native target engine.
     *
     * This replaces a long chain of esp.c workarounds.  The IDENTIFY byte and
     * CDB used to be pushed through an ESP command FIFO; ESP negotiated the
     * data length into ti_size; the transfer counter had to be written
     * straight into esp->rregs/wregs because esp_reg_write() discards TC
     * writes (esp.c:1303); and ESP would not enter the data phase unless
     * CMD_TI|CMD_DMA was re-written afterwards, which needed a per-command
     * "kick" latch because ESP_CMD is write-only.  All of that is now the
     * engine's own bookkeeping.
     */
    state->command_done = false;
    sgi_wd_target_start(state);
    state->dma_armed = sgi_wd_wd_dma_mode(state) && state->dma_count &&
                       state->target_xfer != 0;
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
     * Drive the controller to quiescence in a single poll (the QEMU equivalent
     * of MAME's synchronous DRQ drain): move whatever data the target has
     * ready, then report completion, so one guest read that finds pending
     * data finishes the whole command before returning.
     */
    for (int iter = 0; iter < 4; iter++) {
        if (sgi_wd_trace_enabled() && !state->kick_traced &&
            state->command_active) {
            state->kick_traced = true;
            fprintf(stderr,
                    "WDGATE t=%llu dmac=%u moved=%u ready=%d async=%d xfer=%d tc=%u armed=%d mode=%d done=%d op=%02x\n",
                    (unsigned long long)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                    state->dma_count, state->dma_moved, state->data_ready,
                    state->async_len, state->target_xfer, state->target_tc,
                    state->dma_armed, sgi_wd_wd_dma_mode(state),
                    state->command_done, state->wd_regs[3]);
        }

        /*
         * The target has a chunk ready: move it.
         *
         * Deliberately not gated on !command_done.  A single
         * scsi_req_continue() delivers the final data and then reports
         * completion in the same call, so both flags are set together and
         * data_ready is true precisely when the reply is still sitting in the
         * window.  Gating on !command_done dropped that reply on the floor and
         * reported success with zero bytes moved, which the PROM saw as an
         * unresponsive drive.
         */
        if (state->data_ready && state->async_len > 0) {
            sgi_wd_target_pump(state);
            continue;
        }

        /*
         * The target has reported a final status.  Report the command
         * complete, but only once the engine has no data left to move: a
         * mid-transfer completion would surface to the driver as a short
         * transfer.  The guest's own transfer count must not gate this:
         * after final status no more data can ever arrive, so a nonzero
         * count is a permanent residual -- CHECK CONDITION on an unknown
         * opcode, an over-allocated probe, a short read -- which hardware
         * reports alongside completion.  Withholding the completion for
         * it wedged the engine into 10-second timeouts and bus resets
         * (READ TOC truncation, vendor 0x46 at boot) that also killed
         * in-flight commands on other IDs.
         */
        if (state->command_done && !state->irq_fifo_size) {
            uint8_t op = state->wd_regs[3];
            /*
             * The reply must be fully drained out of the window: an
             * undrained INQUIRY reply is exactly the failure that made
             * the PROM report an unresponsive drive.
             */
            bool drained = (state->target_xfer == 0 && state->async_len <= 0 &&
                            !state->data_ready);

            if (drained) {
                state->completion_phase = 3;
                /* The target's status byte was latched by the engine. */
                sgi_wd_irq_fifo_push(state,
                                     SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS);
                state->wd_regs[0x10] = SGI_WD_PHASE_COMMAND_COMPLETE;
                /* Board trace: completion state at COMMAND_COMPLETE */
                if (sgi_wd_trace_enabled()) {
                    fprintf(stderr,
                            "WDCOMP t=%llu resid=%.6x dma_count=%u moved=%u got=%u drop=%u flags=%08x ctl=%02x aux=%02x act=%d status=%02x\n",
                            (unsigned long long)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                            ((uint32_t)state->wd_regs[0x12] << 16) |
                            ((uint32_t)state->wd_regs[0x13] << 8) |
                            state->wd_regs[0x14],
                            state->dma_count, state->dma_moved,
                            state->dma_got, state->dma_dropped,
                            state->dma_desc_flags,
                            state->dma_control, state->auxiliary_status,
                            state->command_active, state->wd_regs[0x0f]);
                }
                /*
                 * Read-only PIO audit: dump what PIO reads returned so the
                 * offline diff covers the non-DMA path too.
                 */
                if (sgi_wd_trace_enabled() && op == 0x28 &&
                    state->pio_n > 0) {
                    uint32_t lba = ((uint32_t)state->wd_regs[5] << 24) |
                                   ((uint32_t)state->wd_regs[6] << 16) |
                                   ((uint32_t)state->wd_regs[7] << 8) |
                                   state->wd_regs[8];
                    uint16_t i;
                    fprintf(stderr, "PIODUMP lba=%u n=%u data=",
                            lba, state->pio_n);
                    for (i = 0; i < state->pio_n; i++) {
                        fprintf(stderr, "%02x", state->pio_log[i]);
                    }
                    fprintf(stderr, "\n");
                }
                /* MAME hd.cpp leaves INQUIRY byte7 0; QEMU Sync 0x10 fails
                 * the IRIX length check. */
                if (state->inq_patch_pending) {
                    sgi_wd_patch_inquiry(state);
                }
                /*
                 * With ENABLE cleared, the single poll shows ENABLE clear +
                 * 0x60 with no second poll needed.  MAME FINISHED clears BSY;
                 * the guest reads status next.
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
                break;
            }
        }
        break;
    }
    /*
     * Data buffer request: the WD33C93 asserts DBR while the initiator still
     * owes data-phase bytes.  While paused DBR must stay clear: the aux
     * register read masks INT behind DBR, which would hide the pause
     * interrupt (0x49) from a polling driver and wedge it until timeout.
     */
    if (state->command_active && state->target_xfer != 0 &&
        !state->xfer_paused) {
        state->auxiliary_status |= 0x01;
    } else {
        state->auxiliary_status &= ~0x01;
    }
    sgi_wd_update_irq(state);
}

void sgi_wd33c93_scsi_bus_init(SGIWD33C93State *state)
{
    /*
     * The machine cannot reach state->bus: the struct is private to this
     * file.  Command-line -drive if=scsi handling is anchored on the bus, so
     * it has to be applied from here.
     */
    scsi_bus_legacy_handle_cmdline(&state->bus);
}

void sgi_wd33c93_poll(SGIWD33C93State *state)
{
    if (state) {
        sgi_wd_update_status(state);
    }
}

static void sgi_wd_complete_bh(void *opaque)
{
    SGIWD33C93State *state = opaque;

    if (sgi_wd_trace_enabled()) {
        fprintf(stderr, "WDBH op=%02x\n", state->wd_regs[3]);
    }
    sgi_wd_update_status(state);
}

static void sgi_wd_progress_tick(void *opaque)
{
    SGIWD33C93State *state = opaque;

    if (state->selection_timeout_armed) {
        /* Deferred missing-target report (MAME ARB_TIMEOUT_BUSY). */
        state->selection_timeout_armed = false;
        state->auxiliary_status &= ~0x20;
        sgi_wd_irq_fifo_push(state, SGI_WD_STATUS_SELECTION_TIMEOUT);
        state->scsi_status = SGI_WD_STATUS_SELECTION_TIMEOUT;
        state->wd_regs[0x10] = 0;
        state->command_active = false;
        sgi_wd_update_irq(state);
    }
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

        if (state->command_active && state->command_done &&
            !state->data_ready) {
            /*
             * Transfer fully complete (STATUS phase, nothing pending): report
             * completion live instead of a stale queued data phase.  No
             * residual gate: after final status the remaining guest count
             * is permanent and must not withhold completion (see the FC
             * path above).
             */
            status = SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS;
            state->scsi_status = status;
            state->auxiliary_status |= 0x80;
            state->auxiliary_status &= ~0x20;
            state->completion_phase = 3;
            state->wd_regs[0x10] = SGI_WD_PHASE_COMMAND_COMPLETE;
            if (state->inq_patch_pending) {
                sgi_wd_patch_inquiry(state);
            }
            if (state->dma_desc_flags & (1U << 29)) {
                state->dma_irq = true;
                sgi_wd_update_dma_irq(state);
            }
            state->dma_control &= ~0x10U;
            state->dma_active = false;
            /*
             * MAME holds a completion status with INT asserted until a
             * later SCSI Status read acks it. Consuming the entry in the
             * same handler that reports it collapses the pulse below what
             * the CPU samples, so hold it instead: the next 0x17 read
             * pops it through the normal path.
             */
            if (!state->irq_fifo_size) {
                sgi_wd_irq_fifo_push(state, status);
            }
            if (state->selection_status_pending &&
                status == SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS) {
                state->selection_status_pending = false;
            }
            if (state->completion_phase == 3) {
                state->command_active = false;
            }
            if (state->command_active && state->target_xfer != 0 &&
                !state->xfer_paused) {
                state->auxiliary_status |= 0x01;
            } else {
                state->auxiliary_status &= ~0x01;
            }
            sgi_wd_update_irq(state);
            state->register_select = 0x18;
            if (sgi_wd_trace_enabled() && sgi_wd_trace_wanted(2, 0x17)) {
                fprintf(stderr, "WD33 REG R addr=17 val=%02x\n", status);
            }
            return status;
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
        if (!state->command_active && !state->irq_fifo_size) {
            /* Drained by this ack: drop INT even if the poll above
             * early-returned (MAME: status-read ack empties the FIFO). */
            sgi_wd_update_irq(state);
        }
        state->register_select = 0x18;
        return status;
    }
    case 0x19: {
        uint8_t value;

        if (!(state->auxiliary_status & 0x01)) {
            return 0;
        }
        /*
         * PIO data path: pop one byte straight from the engine's buffer.
         * Replaces esp_reg_read(ESP_FIFO) plus the two CMD_TI kicks ESP
         * needed to restart its FIFO; asking the target for more and pumping
         * does both in one step.
         */
        if (state->async_len <= 0) {
            return 0;
        }
        value = state->async_buf[0];
        memmove(state->async_buf, state->async_buf + 1,
                state->async_len - 1);
        state->async_len--;
        if (state->target_tc) {
            state->target_tc--;
        }
        if (state->target_xfer) {
            state->target_xfer--;
        }
        /* Read-only PIO audit for offline diff (first sector per command). */
        if (sgi_wd_trace_enabled() && state->pio_n < sizeof(state->pio_log)) {
            state->pio_log[state->pio_n++] = value;
        }
        if (state->async_len == 0) {
            state->data_ready = false;
            state->auxiliary_status &= ~0x01;
            sgi_wd_target_continue(state);
            sgi_wd_target_pump(state);
        }
        sgi_wd_decrement_transfer_count(state);
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
                sgi_wd_reset_state(state, false);
            } else if (cc == SGI_WD_COMMAND_CC_SELECT_ATN_TRANSFER ||
                       cc == SGI_WD_COMMAND_CC_SELECT_TRANSFER) {
                sgi_wd_start_transfer(state);
            } else if (cc == SGI_WD_COMMAND_CC_SELECT_ATN ||
                       cc == SGI_WD_COMMAND_CC_SELECT) {
                sgi_wd_start_select(state, cc);
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
    DEFINE_PROP_UINT32("max-xfer", SGIWD33C93State, max_xfer, 0x40000),
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
    /*
     * Own the SCSI bus directly.  Previously the machine created an ESP and
     * linked it in through the "esp" property, and this driver drove the
     * target through esp.c; the bus is now created here so the addressable
     * name stays scsi.0/scsi.1 for -device scsi-hd,bus=...
     */
    scsi_bus_init(&state->bus, sizeof(state->bus), device,
                  &sgi_wd33c93_bus_info);

    state->progress_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         sgi_wd_progress_tick, state);
    state->complete_bh = qemu_bh_new(sgi_wd_complete_bh, state);
    memory_region_init_io(&state->regs, OBJECT(device), &sgi_wd_ops, state,
                          "sgi-wd33c93.regs", 0x8000);
    sysbus_init_mmio(SYS_BUS_DEVICE(device), &state->regs);
    sgi_wd_reset_state(state, true);
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

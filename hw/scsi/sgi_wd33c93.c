#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/aio.h"
#include "qemu/main-loop.h"
#include "hw/core/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/scsi/sgi_wd33c93.h"
#include "hw/scsi/esp.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/address-spaces.h"

#define SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS 0x16
#define SGI_WD_STATUS_TRANSFER_SUCCESS 0x18
#define SGI_WD_STATUS_SELECTION_TIMEOUT 0x42
#define SGI_WD_STATUS_REQ 0x88
#define SGI_WD_PHASE_DATA_IN 0x01
#define SGI_WD_PHASE_RECEIVE_STATUS 0x47
#define SGI_WD_PHASE_STATUS_RECEIVED 0x50
#define SGI_WD_PHASE_COMMAND_COMPLETE 0x60
struct SGIWD33C93State {
    SysBusDevice parent_obj;
    MemoryRegion regs;
    ESPState *esp;
    uint8_t wd_regs[32];
    uint8_t register_select;
    uint8_t auxiliary_status;
    uint8_t scsi_status;
    bool command_active;
    bool selection_status_pending;
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
    QEMUTimer *progress_timer;
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

static void sgi_wd_update_irq(SGIWD33C93State *state)
{
    if (state->auxiliary_status & 0x80) {
        state->auxiliary_status &= ~0x80;
    }
    if (state->irq_fifo_size) {
        state->scsi_status = sgi_wd_irq_fifo_peek(state);
        state->auxiliary_status |= 0x80;
    }
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
    state->scsi_status = SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS;
    state->command_active = false;
    state->selection_status_pending = false;
    state->completion_phase = 0;
    state->dma_next_desc = 0;
    state->dma_current = 0;
    state->dma_count = 0;
    state->dma_control = 0;
    state->dma_desc_flags = 0;
    state->dma_irq = false;
    state->dma_active = false;
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
}

static void sgi_wd_reset(DeviceState *device)
{
    sgi_wd_reset_state(SGI_WD33C93(device));
}

static uint32_t sgi_wd_dma_read32(hwaddr addr)
{
    /* Descriptors/data are guest virtual (KSEG0/1 plus TLB-mapped KUSEG);
     * read via the CPU MMU, falling back to direct for early boot. */
    CPUState *cs = qemu_get_cpu(0);
    if (cs) {
        uint8_t b[4];
        if (cpu_memory_rw_debug(cs, (vaddr)addr, b, sizeof(b), 0) == 0) {
            return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                   ((uint32_t)b[2] << 8) | (uint32_t)b[3];
        }
    }

    uint32_t value = 0;

    if ((addr & 0xe0000000U) == 0x80000000U ||
        (addr & 0xe0000000U) == 0xa0000000U) {
        addr &= 0x1fffffffU;
    }

    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
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

/* MAME hpc3.cpp:985-1000: flush raises IRQ on bit 29, drops ENABLE. */
static void sgi_wd_dma_flush(SGIWD33C93State *state)
{
    if (state->dma_desc_flags & (1U << 29)) {
        state->dma_irq = true;
    }
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
        if (cs) {
            hwaddr va = sgi_wd_dma_address(state, state->dma_current);
            if (cpu_memory_rw_debug(cs, (vaddr)va, &v, 1, 0) == 0) {
                ok = true;
            }
        }
        if (!ok) {
            MemTxResult result;
            hwaddr pa = state->dma_current;
            if ((pa & 0xe0000000U) == 0x80000000U ||
                (pa & 0xe0000000U) == 0xa0000000U) {
                pa &= 0x1fffffffU;
            }
            if (state->dma_control & 0x02) {
                pa ^= 3;
            }
            v = address_space_ldub(&address_space_memory, pa,
                                   MEMTXATTRS_UNSPECIFIED, &result);
        }
        *buf++ = v;
        state->dma_current++;
        state->dma_count--;
        len--;
        sgi_wd_dma_bump_wd_count(state);
    }
}

static void sgi_wd_dma_write(void *opaque, uint8_t *buf, int len)
{
    SGIWD33C93State *state = opaque;
    CPUState *cs = qemu_get_cpu(0);

    while (len > 0 && sgi_wd_dma_next(state)) {
        uint8_t v = *buf++;
        bool ok = false;
        if (cs) {
            hwaddr va = sgi_wd_dma_address(state, state->dma_current);
            if (cpu_memory_rw_debug(cs, (vaddr)va, &v, 1, 1) == 0) {
                ok = true;
            }
        }
        if (!ok) {
            MemTxResult result;
            hwaddr pa = state->dma_current;
            if ((pa & 0xe0000000U) == 0x80000000U ||
                (pa & 0xe0000000U) == 0xa0000000U) {
                pa &= 0x1fffffffU;
            }
            if (state->dma_control & 0x02) {
                pa ^= 3;
            }
            address_space_stb(&address_space_memory, pa, v,
                              MEMTXATTRS_UNSPECIFIED, &result);
        }
        state->dma_current++;
        state->dma_count--;
        len--;
        sgi_wd_dma_bump_wd_count(state);
    }
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

    esp_hard_reset(state->esp);
    state->command_active = true;
    /* MAME wd33c9x.cpp:733-735: Level II commands set BSY. */
    state->auxiliary_status = 0x80 | 0x20;
    sgi_wd_irq_fifo_reset(state);
    state->selection_status_pending = false;
    state->completion_phase = 0;
    state->scsi_status = 0;
    state->wd_regs[0x10] = 0x10;

    esp_reg_write(state->esp, ESP_TCLO, transfer_count);
    esp_reg_write(state->esp, ESP_TCMID, transfer_count >> 8);
    esp_reg_write(state->esp, ESP_TCHI, transfer_count >> 16);
    state->esp->wregs[ESP_WBUSID] = state->wd_regs[0x15] & BUSID_DID;
    /* MAME wd33c9x.cpp:1761-1763: IDENTIFY with ER bit from SOURCE_ID. */
    esp_reg_write(state->esp, ESP_FIFO, (state->wd_regs[0x0f] & 7) | 0x80 |
                                        ((state->wd_regs[0x16] & 0x80) ? 0x40 : 0));
    for (i = 0; i < cdb_length; i++) {
        esp_reg_write(state->esp, ESP_FIFO, state->wd_regs[3 + i]);
    }
    esp_reg_write(state->esp, ESP_CMD, CMD_SELATN);
    if (sgi_wd_wd_dma_mode(state) && state->dma_count && state->esp->async_len) {
        esp_reg_write(state->esp, ESP_CMD, CMD_TI | CMD_DMA);
    }
    /*
     * Complete small memory-satisfiable probes (INQUIRY/MODE SENSE/REQUEST
     * SENSE) inline: QEMU's ESP answers via BH, after the guest's single
     * poll. Bulk READs stay on the async path.
     */
    {
        uint8_t probe_op = state->wd_regs[3];
        if (probe_op == 0x12 || probe_op == 0x1a || probe_op == 0x03) {
            for (int sync_iter = 0; sync_iter < 8; sync_iter++) {
                aio_poll(qemu_get_aio_context(), false);
                if (sgi_wd_wd_dma_mode(state) && state->dma_count &&
                    state->esp->async_len &&
                    state->esp->rregs[ESP_CMD] != (CMD_TI | CMD_DMA)) {
                    esp_reg_write(state->esp, ESP_CMD, CMD_TI | CMD_DMA);
                }
                sgi_wd_update_status(state);
                if (state->completion_phase == 3) {
                    break;
                }
                if (!state->command_active) {
                    break;
                }
                if (!state->esp->async_len &&
                    !(state->esp->rregs[ESP_RINTR] & (INTR_BS | INTR_FC))) {
                    break;
                }
            }
        }
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
    /*
     * Drive WD/ESP to quiescence in a single poll (like MAME's synchronous
     * DRQ drain): kick a pending transfer, collect target status, and report
     * completion, so one guest read that finds async data finishes everything
     * before returning. Bulk READs still gate on FC below.
     */
    for (int iter = 0; iter < 4; iter++) {
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
    if (state->dma_count && state->esp->async_len &&
        state->esp->rregs[ESP_CMD] != (CMD_TI | CMD_DMA)) {
        /*
         * Kick the ESP transfer once (MAME INIT_XFR); do not re-kick while in
         * TI|DMA (it reloads the count and discards multi-chunk progress).
         * No IRQ is pushed for DATA_IN: in DMA mode the chip asserts DRQ,
         * never an IRQ, and completion is reported exactly once below.
         * Polled CONTROL stays on the PIO DATA path (MAME wd33c9x RECV).
         */
        if (!sgi_wd_wd_dma_mode(state)) {
            break;
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
            bool overAlloc = (opCk == 0x12 || opCk == 0x1a || opCk == 0x03);
            if (!overAlloc && (state->dma_count || residCk)) {
            /* Bulk with residual pending: more DATA to come, do not report
             * GOOD yet; completion re-raises with drained residual. */
            } else {
            state->completion_phase = 3;
            state->wd_regs[0x0f] = state->esp->status;
            sgi_wd_irq_fifo_push(state,
                                 SGI_WD_STATUS_SELECT_TRANSFER_SUCCESS);
            state->wd_regs[0x10] = SGI_WD_PHASE_COMMAND_COMPLETE;
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
        sgi_wd_update_status(state);
        /*
         * Mask INT while DBR is set (MAME wd33c9x indir_addr_r): during DRQ
         * drain the guest polls aux for DBR, and a spurious INT sends it
         * down the wrong path.
         */
        if (state->auxiliary_status & 0x01) {
            return state->auxiliary_status & 0x7f;
        }
        return state->auxiliary_status;
    }
    if ((addr & 0xff) != 7) {
        return 0;
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
            uint32_t residLive = ((uint32_t)state->wd_regs[0x12] << 16) |
                                 ((uint32_t)state->wd_regs[0x13] << 8) |
                                 state->wd_regs[0x14];
            uint8_t opLive = state->wd_regs[3];
            bool overLive = (opLive == 0x12 || opLive == 0x1a ||
                             opLive == 0x03);
            if (!overLive && (state->dma_count || residLive)) {
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
            if (state->dma_desc_flags & (1U << 29)) {
                state->dma_irq = true;
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
        if (state->wd_regs[0x14] || state->wd_regs[0x13] ||
            state->wd_regs[0x12]) {
            uint32_t count = (state->wd_regs[0x12] << 16) |
                             (state->wd_regs[0x13] << 8) |
                             state->wd_regs[0x14];
            count--;
            state->wd_regs[0x12] = count >> 16;
            state->wd_regs[0x13] = count >> 8;
            state->wd_regs[0x14] = count;
        }
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
        state->register_select = value & 0xff;
    } else if ((addr & 0xff) == 7) {
        uint8_t reg = state->register_select & 0x1f;

        if (reg == 0x18) {
            state->wd_regs[reg] = value;
            if ((value & 0x1f) == 0) {
                sgi_wd_reset_state(state);
            } else if ((value & 0x1f) == 8) {
                sgi_wd_start_transfer(state);
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
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(wd_regs, SGIWD33C93State, 32),
        VMSTATE_UINT8(register_select, SGIWD33C93State),
        VMSTATE_UINT8(auxiliary_status, SGIWD33C93State),
        VMSTATE_UINT8(scsi_status, SGIWD33C93State),
        VMSTATE_BOOL(command_active, SGIWD33C93State),
        VMSTATE_BOOL(selection_status_pending, SGIWD33C93State),
        VMSTATE_UINT8(completion_phase, SGIWD33C93State),
        VMSTATE_UINT8_ARRAY(irq_fifo, SGIWD33C93State, 8),
        VMSTATE_UINT8(irq_fifo_pos, SGIWD33C93State),
        VMSTATE_UINT8(irq_fifo_size, SGIWD33C93State),
        VMSTATE_BOOL(data_status_queued, SGIWD33C93State),
        VMSTATE_UINT32(dma_next_desc, SGIWD33C93State),
        VMSTATE_UINT32(dma_current, SGIWD33C93State),
        VMSTATE_UINT32(dma_count, SGIWD33C93State),
        VMSTATE_UINT32(dma_control, SGIWD33C93State),
        VMSTATE_UINT32(dma_desc_flags, SGIWD33C93State),
        VMSTATE_BOOL(dma_irq, SGIWD33C93State),
        VMSTATE_BOOL(dma_active, SGIWD33C93State),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_wd_realize(DeviceState *device, Error **errp)
{
    SGIWD33C93State *state = SGI_WD33C93(device);

    state->esp->dma_memory_read = sgi_wd_dma_read;
    state->esp->dma_memory_write = sgi_wd_dma_write;
    state->esp->dma_opaque = state;

    state->progress_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         sgi_wd_progress_tick, state);
    memory_region_init_io(&state->regs, OBJECT(device), &sgi_wd_ops, state,
                          "sgi-wd33c93.regs", 0x1000);
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

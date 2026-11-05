#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/scsi/scsi.h"
#include "scsi/constants.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "sgi_s1.h"

#define S1_WINDOW_SIZE 0x8000
#define S1_NUM_CHANNELS 3
#define S1_CHAN_STRIDE 0x800
#define S1_CTRL_BASE 0x0000
#define S1_DMA_BASE 0x2000
#define S1_STATUS_CMD 0x6000
#define S1_DMA_INTERRUPT 0x7000
#define S1_INTERRUPT 0x7018
#define S1_PIO_CLEAR 0x7020
#define S1_IBUS_ERR_STAT 0x7028

#define S1_DMA_WRITE_OFF 0x000
#define S1_DMA_READ_OFF 0x008
#define S1_DMA_TRANS_LOW 0x010
#define S1_DMA_TRANS_HIGH 0x018
#define S1_DMA_FLUSH 0x020
#define S1_DMA_CHAN_RESET 0x028

#define S1_SC_INT_CNTRL(_ch) (1u << (20 + (_ch)))
#define S1_SC_INT_CNTRL_ANY (0x7u << 20)
#define S1_SC_DMA(_ch) (1u << (17 + (_ch)))
#define S1_SC_D_ERR(_ch) (1u << (6 + (_ch)))

#define WD95_N_CTL 0
#define WD95_N_ISR 4
#define WD95_N_STOPU 6
#define WD95_N_UEI 8
#define WD95_N_ISRM 10
#define WD95_N_STOPUM 12
#define WD95_N_UEIM 14
#define WD95_S_CDBSIZ1 12
#define WD95_S_CDBSIZ2 14
#define WD95_N_SQADR 20
#define WD95_N_DESTID 26
#define WD95_N_FLAG 30
#define WD95_N_TC_0_7 32
#define WD95_N_TC_8_15 34
#define WD95_N_TC_16_23 36
#define WD95_N_SR 40
#define WD95_N_FIFOS 42
#define WD95_N_DPR 64
#define WD95_S_CVER 72
#define WD95_NUM_REGS 128

#define W95_DPR_R(_reg) (WD95_N_DPR + ((_reg) << 1))

#define W95_CTL_SETUP 0x01
#define W95_CTL_ABORT 0x02
#define W95_CTL_KILL 0x04
#define W95_CTL_LDTC 0x08
#define W95_CTL_LDTCL 0x10

#define W95_ISR_STOPWCS 0x01
#define W95_ISR_INTWCS 0x02
#define W95_ISR_STOPU 0x04
#define W95_ISR_UEI 0x08
#define W95_ISR_BUSYI 0x10
#define W95_ISR_VBUSYI 0x20
#define W95_ISR_SREJ 0x40

#define W95_ISRM_STOPWCSM 0x01
#define W95_ISRM_INTWCSM 0x02
#define W95_ISRM_STOPUM 0x04
#define W95_ISRM_UEIM 0x08
#define W95_ISRM_BUSYIM 0x10
#define W95_ISRM_VBUSYIM 0x20
#define W95_ISRM_SREJM 0x40

#define W95_STPU_ABORTI 0x01
#define W95_STPU_SCSIT 0x02

#define W95_SR_ACTIVE 0x04

#define WCS_I_COMPLETE 0x4e

#define S1_FIFO_INITIAL 512

typedef struct S1Channel {
    SGI_S1State *s;
    uint8_t regs[WD95_NUM_REGS];
    uint8_t setup_regs[WD95_NUM_REGS];
    bool setup;
    uint32_t dma_xlat_lo;
    uint32_t dma_xlat_hi;
    uint32_t dma_offset;
    bool dma_dir_in;
    bool dma_armed;
    uint32_t tc;
    uint32_t tc_left;
    uint32_t xfer_total;
    bool waiting_tc;
    bool need_continue;
    bool target_done;
    bool command_active;
    bool start_pending;
    int complete_status;
    uint32_t complete_resid;
    SCSIRequest *req;
    uint8_t *fifo;
    size_t fifo_len;
    size_t fifo_pos;
    size_t fifo_alloc;
    uint8_t *out_buf;      /* target data-out buffer, valid until continue */
    size_t out_len;
    size_t out_pos;
} S1Channel;

struct SGI_S1State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    SCSIBus bus[S1_NUM_CHANNELS];
    S1Channel chan[S1_NUM_CHANNELS];
    uint32_t status_cmd;
    uint32_t interrupt;
    uint32_t dma_interrupt[S1_NUM_CHANNELS];
    uint32_t ibus_error;
    QEMUBH *bh;
    qemu_irq irq;
    hwaddr dma_alias;
    bool trace;
};

static void s1_start_command(S1Channel *c);
static void s1_out_flush(S1Channel *c);
static void s1_set_dma_error(S1Channel *c);

static bool s1_int0_pending(S1Channel *c)
{
    uint8_t isr = c->regs[WD95_N_ISR];
    uint8_t isrm = c->regs[WD95_N_ISRM];
    uint8_t stopu = c->regs[WD95_N_STOPU];
    uint8_t uei = c->regs[WD95_N_UEI];

    if (isr & isrm &
        (W95_ISR_STOPWCS | W95_ISR_INTWCS | W95_ISR_BUSYI |
         W95_ISR_VBUSYI | W95_ISR_SREJ)) {
        return true;
    }
    if ((isrm & W95_ISRM_STOPUM) &&
        (stopu & c->regs[WD95_N_STOPUM])) {
        return true;
    }
    if ((isrm & W95_ISRM_UEIM) && (uei & c->regs[WD95_N_UEIM])) {
        return true;
    }
    return false;
}

static void s1_update_irq(SGI_S1State *s)
{
    qemu_set_irq(s->irq, !!(s->status_cmd & S1_SC_INT_CNTRL_ANY));
}

static void s1_update_status_bit(S1Channel *c)
{
    uint32_t bit = S1_SC_INT_CNTRL(c - c->s->chan);

    if (s1_int0_pending(c)) {
        c->s->status_cmd |= bit;
    } else {
        c->s->status_cmd &= ~bit;
    }
    s1_update_irq(c->s);
}

static void s1_set_isr(S1Channel *c, uint8_t bits)
{
    c->regs[WD95_N_ISR] |= bits & 0x7f;
    s1_update_status_bit(c);
}

static void s1_set_stopu(S1Channel *c, uint8_t bits)
{
    c->regs[WD95_N_STOPU] |= bits & 0xff;
    if (bits) {
        s1_set_isr(c, W95_ISR_STOPU);
    }
}

static uint32_t s1_get_tc(S1Channel *c)
{
    return ((uint32_t)c->regs[WD95_N_TC_16_23] << 16) |
           ((uint32_t)c->regs[WD95_N_TC_8_15] << 8) |
           c->regs[WD95_N_TC_0_7];
}

static void s1_store_tc(S1Channel *c, uint32_t tc)
{
    tc &= 0xffffff;
    c->regs[WD95_N_TC_0_7] = tc & 0xff;
    c->regs[WD95_N_TC_8_15] = (tc >> 8) & 0xff;
    c->regs[WD95_N_TC_16_23] = (tc >> 16) & 0xff;
}

static void s1_set_tc(S1Channel *c, uint32_t tc)
{
    c->tc = tc & 0xffffff;
    c->tc_left = c->tc;
    s1_store_tc(c, c->tc);
}

static void s1_tc_consume(S1Channel *c, uint32_t n)
{
    bool was_busy;

    if (!n) {
        return;
    }
    if (n > c->tc_left) {
        n = c->tc_left;
    }
    was_busy = c->tc_left != 0;
    c->tc_left -= n;
    s1_store_tc(c, c->tc_left);
    if (was_busy && c->tc_left == 0 && !c->waiting_tc) {
        c->waiting_tc = true;
        if (c->regs[WD95_N_ISRM] & W95_ISRM_BUSYIM) {
            s1_set_isr(c, W95_ISR_BUSYI);
        }
    }
}

static unsigned s1_cdb_length(S1Channel *c, uint8_t op)
{
    unsigned group = op >> 5;
    unsigned len;

    switch (group) {
    case 0:
        return 6;
    case 1:
    case 2:
        return 10;
    case 3:
        len = c->setup_regs[WD95_S_CDBSIZ1] & 0x0f;
        return len + 1;
    case 4:
        len = (c->setup_regs[WD95_S_CDBSIZ1] >> 4) & 0x0f;
        return len + 1;
    case 5:
        return 12;
    case 6:
        len = c->setup_regs[WD95_S_CDBSIZ2] & 0x0f;
        return len + 1;
    default:
        len = (c->setup_regs[WD95_S_CDBSIZ2] >> 4) & 0x0f;
        return len + 1;
    }
}

static uint32_t s1_read_guest32(SGI_S1State *s, hwaddr addr, bool *ok)
{
    uint8_t buf[4];
    MemTxResult res;

    res = address_space_read(&address_space_memory, s->dma_alias + addr,
                             MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));
    if (ok) {
        *ok = res == MEMTX_OK;
    }
    return ldl_be_p(buf);
}

static bool s1_dma_buf(S1Channel *c, uint8_t *buf, size_t len, bool to_guest)
{
    uint64_t table;
    size_t done = 0;

    if (!c->dma_armed) {
        if (c->s->trace) {
            fprintf(stderr, "S1DMA ch%u not armed len=%zu to_guest=%d\n",
                    (unsigned)(c - c->s->chan), len, to_guest);
        }
        return false;
    }

    table = ((uint64_t)c->dma_xlat_hi << 32) | c->dma_xlat_lo;

    while (done < len) {
        uint64_t pos = (uint64_t)c->dma_offset + c->xfer_total + done;
        uint32_t index = pos >> 12;
        uint32_t page_off = pos & 0xfff;
        uint32_t entry;
        uint64_t phys;
        size_t n;
        bool ok;
        MemTxResult res;

        entry = s1_read_guest32(c->s, table + (uint64_t)index * 4, &ok);
        if (!ok) {
            if (c->s->trace) {
                fprintf(stderr, "S1DMA ch%u bad table entry table=%llx idx=%u\n",
                        (unsigned)(c - c->s->chan),
                        (unsigned long long)table, index);
            }
            return false;
        }

        phys = ((uint64_t)(entry >> 4) << 12) | page_off;
        n = MIN(len - done, 0x1000 - page_off);
        if (c->s->trace) {
            fprintf(stderr, "S1DMA ch%u %s table=%llx idx=%u entry=%08x "
                    "phys=%llx off=%x n=%zu\n",
                    (unsigned)(c - c->s->chan), to_guest ? "in" : "out",
                    (unsigned long long)table, index, entry,
                    (unsigned long long)phys, page_off, n);
        }
        {
            hwaddr raw_phys = phys;

            phys += c->s->dma_alias;
            if (to_guest) {
                res = address_space_write(&address_space_memory, phys,
                                          MEMTXATTRS_UNSPECIFIED,
                                          buf + done, n);
            } else {
                res = address_space_read(&address_space_memory, phys,
                                         MEMTXATTRS_UNSPECIFIED,
                                         buf + done, n);
            }
            if (res != MEMTX_OK) {
                return false;
            }
            if (getenv("SGI_S1_VERIFY")) {
                uint8_t vbuf[4096];
                MemTxResult vres;

                if (to_guest) {
                    vres = address_space_read(&address_space_memory,
                                              raw_phys,
                                              MEMTXATTRS_UNSPECIFIED,
                                              vbuf, n);
                    if (vres != MEMTX_OK || memcmp(vbuf, buf + done, n)) {
                        fprintf(stderr, "S1VERIFY ch%u in raw=%llx "
                                "alias=%llx n=%zu mismatch\n",
                                (unsigned)(c - c->s->chan),
                                (unsigned long long)raw_phys,
                                (unsigned long long)phys, n);
                    }
                } else {
                    vres = address_space_read(&address_space_memory,
                                              raw_phys,
                                              MEMTXATTRS_UNSPECIFIED,
                                              vbuf, n);
                    if (vres != MEMTX_OK ||
                        memcmp(vbuf, buf + done, n)) {
                        fprintf(stderr, "S1VERIFY ch%u out raw=%llx "
                                "alias=%llx n=%zu mismatch\n",
                                (unsigned)(c - c->s->chan),
                                (unsigned long long)raw_phys,
                                (unsigned long long)phys, n);
                    }
                }
            }
        }
        done += n;
    }

    c->xfer_total += len;
    return true;
}

static void s1_fifo_reset(S1Channel *c)
{
    c->fifo_len = 0;
    c->fifo_pos = 0;
}

/*
 * The PROM can start a data-out command before it programs the DMA
 * offset.  Buffer the target data and send it once DMA is armed.
 */
static void s1_out_flush(S1Channel *c)
{
    while (c->dma_armed && !c->waiting_tc && c->out_pos < c->out_len &&
           c->tc_left) {
        size_t n = MIN(c->out_len - c->out_pos, c->tc_left);

        if (!s1_dma_buf(c, c->out_buf + c->out_pos, n, false)) {
            s1_set_dma_error(c);
            c->complete_status = 0;
            c->complete_resid = c->tc_left;
            c->target_done = true;
            c->command_active = false;
            c->dma_armed = false;
            qemu_bh_schedule(c->s->bh);
            return;
        }
        c->out_pos += n;
        s1_tc_consume(c, n);
    }
    if (c->out_pos == c->out_len) {
        c->out_pos = 0;
        c->out_len = 0;
        c->out_buf = NULL;
    }
    if (!c->out_len && c->need_continue && c->req) {
        c->need_continue = false;
        scsi_req_continue(c->req);
    }
}

static void s1_fifo_append(S1Channel *c, const uint8_t *buf, size_t len)
{
    size_t used = c->fifo_len - c->fifo_pos;

    if (c->fifo_pos && (c->fifo_pos + len > c->fifo_alloc ||
                        c->fifo_pos > c->fifo_alloc / 2)) {
        memmove(c->fifo, c->fifo + c->fifo_pos, used);
        c->fifo_len = used;
        c->fifo_pos = 0;
    }

    if (c->fifo_len + len > c->fifo_alloc) {
        size_t alloc = MAX(c->fifo_len + len, c->fifo_alloc * 2);

        if (!alloc) {
            alloc = S1_FIFO_INITIAL;
        }
        c->fifo = g_realloc(c->fifo, alloc);
        c->fifo_alloc = alloc;
    }
    memcpy(c->fifo + c->fifo_len, buf, len);
    c->fifo_len += len;
}

static void s1_set_dma_active(S1Channel *c, bool active)
{
    unsigned ch = c - c->s->chan;
    uint32_t bit = S1_SC_DMA(ch);

    if (active) {
        c->s->status_cmd |= bit;
    } else {
        c->s->status_cmd &= ~bit;
    }
    s1_update_irq(c->s);
}

static void s1_set_dma_error(S1Channel *c)
{
    unsigned ch = c - c->s->chan;

    c->s->status_cmd |= S1_SC_D_ERR(ch);
    c->s->status_cmd &= ~S1_SC_DMA(ch);
    s1_update_irq(c->s);
}

static void s1_finish_command(S1Channel *c)
{
    SCSIRequest *req = c->req;

    if (c->s->trace) {
        fprintf(stderr, "S1FIN ch%u status=%d resid=%u tc_left=%u\n",
                (unsigned)(c - c->s->chan), c->complete_status,
                c->complete_resid, c->tc_left);
    }
    c->regs[W95_DPR_R(11)] = c->complete_status & 0xff;
    c->regs[WD95_N_SQADR] = WCS_I_COMPLETE;
    s1_set_tc(c, c->tc_left & 0xffffff);
    c->waiting_tc = false;
    c->need_continue = false;
    c->target_done = false;
    c->command_active = false;
    c->out_pos = 0;
    c->out_len = 0;
    c->out_buf = NULL;
    s1_set_isr(c, W95_ISR_STOPWCS | W95_ISR_INTWCS);
    s1_fifo_reset(c);
    c->req = NULL;
    if (req) {
        scsi_req_unref(req);
    }
}

static void s1_drain_in(S1Channel *c)
{
    while (c->fifo_len > c->fifo_pos) {
        size_t avail = c->fifo_len - c->fifo_pos;
        uint32_t n;

        if (!c->dma_armed) {
            /*
             * The PROM may dispatch the WCS before it programs the DMA
             * offset (scsi_go).  Keep the data buffered and drain it
             * when the DMA offset arrives instead of failing the command.
             */
            return;
        }
        if (c->waiting_tc) {
            return;
        }
        if (c->tc_left == 0) {
            if (c->s->trace) {
                fprintf(stderr, "S1DRAIN ch%u tc exhausted target_done=%d "
                        "fifo=%zu/%zu\n",
                        (unsigned)(c - c->s->chan), c->target_done,
                        c->fifo_pos, c->fifo_len);
            }
            c->waiting_tc = true;
            if (c->regs[WD95_N_ISRM] & W95_ISRM_BUSYIM) {
                s1_set_isr(c, W95_ISR_BUSYI);
            }
            return;
        }
        n = MIN(avail, c->tc_left);
        if (!s1_dma_buf(c, c->fifo + c->fifo_pos, n, true)) {
            s1_set_dma_error(c);
            c->complete_status = 0;
            c->complete_resid = c->tc_left;
            c->target_done = true;
            c->command_active = false;
            c->dma_armed = false;
            s1_fifo_reset(c);
            return;
        }
        c->fifo_pos += n;
        s1_tc_consume(c, n);
    }

    if (c->s->trace) {
        fprintf(stderr, "S1DRAIN ch%u fifo drained target_done=%d\n",
                (unsigned)(c - c->s->chan), c->target_done);
    }
    s1_fifo_reset(c);
    if (c->target_done) {
        return;
    }
    if (c->req) {
        scsi_req_continue(c->req);
    }
}

static void s1_data_bh(void *opaque)
{
    SGI_S1State *s = opaque;
    unsigned i;

    for (i = 0; i < S1_NUM_CHANNELS; i++) {
        S1Channel *c = &s->chan[i];

        if (c->start_pending) {
            c->start_pending = false;
            s1_start_command(c);
            /*
             * Do not fall through into the data path: starting a command
             * can synchronously queue its first I/O, and an immediate
             * scsi_req_continue() would start it twice.
             */
            continue;
        }

        if (!c->req) {
            continue;
        }

        if (c->req->cmd.mode == SCSI_XFER_FROM_DEV) {
            s1_drain_in(c);
            /*
             * A target can complete after the last data byte has been
             * drained even though the TC pipeline went empty first.
             * The PROM does not reload TC for that final chunk, so do
             * not leave the command waiting for a BUSYI reload.
             */
            if (c->target_done && c->fifo_len == c->fifo_pos) {
                s1_finish_command(c);
            }
        } else if (c->req->cmd.mode == SCSI_XFER_TO_DEV) {
            if (c->target_done) {
                s1_finish_command(c);
            } else if (c->out_len) {
                s1_out_flush(c);
            }
        } else if (c->target_done) {
            s1_finish_command(c);
        }
    }
}

static void s1_transfer_data(SCSIRequest *req, uint32_t len)
{
    S1Channel *c = req->hba_private;
    uint8_t *buf = scsi_req_get_buf(req);

    if (!c || !len) {
        return;
    }

    if (c->s->trace) {
        fprintf(stderr, "S1XFER ch%u mode=%d len=%u tc_left=%u target_done=%d "
                "cdb0=%02x\n", (unsigned)(c - c->s->chan), req->cmd.mode,
                len, c->tc_left, c->target_done, req->cmd.buf[0]);
    }

    if (req->cmd.mode == SCSI_XFER_FROM_DEV) {
        s1_fifo_append(c, buf, len);
    } else if (req->cmd.mode == SCSI_XFER_TO_DEV) {
        c->out_buf = buf;
        c->out_len = len;
        c->out_pos = 0;
        c->need_continue = true;
        s1_out_flush(c);
    }

    qemu_bh_schedule(c->s->bh);
}

static void s1_command_complete(SCSIRequest *req, size_t resid)
{
    S1Channel *c = req->hba_private;

    if (!c) {
        return;
    }

    if (c->s->trace) {
        fprintf(stderr, "S1DONE ch%u status=%d resid=%zu len=%zu "
                "fifo=%zu/%zu tc_left=%u\n",
                (unsigned)(c - c->s->chan), req->status, resid,
                (size_t)req->cmd.xfer, c->fifo_pos, c->fifo_len,
                c->tc_left);
    }
    c->target_done = true;
    c->command_active = false;
    c->complete_status = req->status;
    c->complete_resid = resid;
    qemu_bh_schedule(c->s->bh);
}

static void s1_command_cancel(SCSIRequest *req)
{
    S1Channel *c = req->hba_private;

    if (!c) {
        return;
    }

    c->target_done = true;
    c->command_active = false;
    c->complete_status = CHECK_CONDITION;
    c->complete_resid = req->residual;
    qemu_bh_schedule(c->s->bh);
}

static const struct SCSIBusInfo sgi_s1_scsi_info = {
    .tcq = false,
    .max_channel = 0,
    .max_target = 15,
    .max_lun = 7,
    .transfer_data = s1_transfer_data,
    .complete = s1_command_complete,
    .cancel = s1_command_cancel,
};

static void s1_normalize_cdb(uint8_t *cdb, unsigned cdb_len)
{
    if (cdb_len >= 3 &&
        (cdb[0] == MODE_SENSE || cdb[0] == MODE_SENSE_10) &&
        (cdb[2] & 0x3f) == 0) {
        cdb[2] |= MODE_PAGE_ALLS;
    }
}

static void s1_start_command(S1Channel *c)
{
    SGI_S1State *s = c->s;
    uint8_t cdb[16];
    unsigned cdb_len;
    unsigned target;
    unsigned lun;
    unsigned i;
    SCSIDevice *dev;
    SCSIRequest *req;
    int32_t datalen;

    target = c->regs[WD95_N_DESTID] & 0x0f;
    /*
     * The kernel WCS puts the identify message in DPR[0], while the
     * standalone WCS uses DPR[5].  Both encode the LUN in the low three
     * bits; the identify message has bit 7 set.  Prefer DPR[0] so a stale
     * standalone message-out byte in DPR[5] cannot change a kernel LUN.
     */
    if (c->regs[W95_DPR_R(0)] & 0x80) {
        lun = c->regs[W95_DPR_R(0)] & 0x07;
    } else if (c->regs[W95_DPR_R(5)] & 0x80) {
        lun = c->regs[W95_DPR_R(5)] & 0x07;
    } else {
        lun = c->regs[W95_DPR_R(0)] & 0x07;
    }
    cdb_len = s1_cdb_length(c, c->regs[W95_DPR_R(20)]);

    c->regs[WD95_N_ISR] = 0;
    c->regs[WD95_N_STOPU] = 0;
    c->regs[WD95_N_UEI] = 0;
    c->regs[WD95_N_SQADR] = 0;
    s1_update_status_bit(c);
    s1_fifo_reset(c);
    c->waiting_tc = false;
    c->need_continue = false;
    c->target_done = false;
    c->xfer_total = 0;
    c->out_pos = 0;
    c->out_len = 0;
    c->out_buf = NULL;
    c->complete_status = 0;
    c->complete_resid = 0;

    for (i = 0; i < cdb_len; i++) {
        cdb[i] = c->regs[W95_DPR_R(20 + i)];
    }

    c->tc = s1_get_tc(c);
    c->tc_left = c->tc;

    dev = scsi_device_find(&s->bus[c - s->chan], 0, target, lun);
    if (!dev) {
        c->command_active = false;
        s1_set_stopu(c, W95_STPU_SCSIT);
        s1_update_status_bit(c);
        return;
    }

    if (dev->type == TYPE_DISK || dev->type == TYPE_ROM) {
        s1_normalize_cdb(cdb, cdb_len);
    }

    req = scsi_req_new(dev, 0, lun, cdb, cdb_len, c);
    if (!req) {
        c->command_active = false;
        s1_set_stopu(c, W95_STPU_SCSIT);
        s1_update_status_bit(c);
        return;
    }

    c->req = req;
    c->command_active = true;

    if (c->s->trace) {
        fprintf(stderr, "S1CMD ch%u target=%u lun=%u cdb=%02x%02x%02x%02x"
                "%02x%02x tc=%u xfer=%zu\n",
                (unsigned)(c - c->s->chan), target, lun,
                cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5],
                c->tc, (size_t)req->cmd.xfer);
    }

    if (c->tc == 0 && req->cmd.xfer > 0) {
        s1_set_tc(c, req->cmd.xfer);
    }

    datalen = scsi_req_enqueue(req);
    if (c->command_active && c->req && datalen != 0) {
        scsi_req_continue(c->req);
    }
}

static uint32_t s1_reg_read(S1Channel *c, unsigned r)
{
    if (r >= WD95_NUM_REGS) {
        return 0;
    }

    if (r == WD95_N_CTL) {
        return c->regs[r];
    }
    if (c->setup) {
        if (r == WD95_S_CVER) {
            return 0x22;
        }
        return c->setup_regs[r];
    }

    if (r == WD95_N_ISR) {
        uint32_t v = c->regs[r] & 0x7f;

        return v | (s1_int0_pending(c) ? 0x80 : 0);
    }
    if (r == WD95_N_SR) {
        return c->command_active ? W95_SR_ACTIVE : 0;
    }
    if (r == WD95_N_DESTID) {
        return c->regs[r] & 0x0f;
    }
    if (r == WD95_N_FIFOS) {
        return 0;
    }
    return c->regs[r];
}

static void s1_reg_write(S1Channel *c, unsigned r, uint8_t val)
{
    if (r >= WD95_NUM_REGS) {
        return;
    }

    if (r == WD95_N_CTL) {
        c->regs[r] = val;
        c->setup = !!(val & W95_CTL_SETUP);
        if ((val & (W95_CTL_LDTC | W95_CTL_LDTCL)) && c->waiting_tc &&
            c->req) {
            c->tc = s1_get_tc(c);
            c->tc_left = c->tc;
            c->waiting_tc = false;
            qemu_bh_schedule(c->s->bh);
        }
        return;
    }

    /* Setup-mode registers have their own bank; only CTL is shared. */
    if (c->setup) {
        c->setup_regs[r] = val;
        return;
    }

    switch (r) {
    case WD95_N_ISR:
        c->regs[r] &= ~(val & 0x7f);
        s1_update_status_bit(c);
        break;
    case WD95_N_STOPU:
        c->regs[r] &= ~(val & 0xff);
        if (!(c->regs[WD95_N_STOPU] & 0xff)) {
            c->regs[WD95_N_ISR] &= ~W95_ISR_STOPU;
        }
        s1_update_status_bit(c);
        break;
    case WD95_N_UEI:
        c->regs[r] &= ~(val & 0xff);
        if (!(c->regs[WD95_N_UEI] & 0xff)) {
            c->regs[WD95_N_ISR] &= ~W95_ISR_UEI;
        }
        s1_update_status_bit(c);
        break;
    case WD95_N_ISRM:
    case WD95_N_STOPUM:
    case WD95_N_UEIM:
        c->regs[r] = val;
        s1_update_status_bit(c);
        break;
    case WD95_N_TC_0_7:
        c->regs[r] = val;
        c->regs[WD95_N_TC_8_15] = 0;
        c->regs[WD95_N_TC_16_23] = 0;
        break;
    case WD95_N_TC_8_15:
    case WD95_N_TC_16_23:
        c->regs[r] = val;
        break;
    case WD95_N_SQADR:
        c->regs[r] = val;
        if (val == 0 && !c->command_active && !c->target_done &&
            !c->start_pending) {
            /*
             * Real hardware starts the WCS asynchronously.  Defer the
             * command until the current MMIO access has retired so the
             * driver can program the DMA offset (scsi_go) first.
             */
            c->start_pending = true;
            qemu_bh_schedule(c->s->bh);
        }
        break;
    default:
        c->regs[r] = val;
        break;
    }
}

static bool s1_core_read(hwaddr off, unsigned size, uint64_t *data,
                         SGI_S1State *s)
{
    if (off == S1_STATUS_CMD && size == 8) {
        *data = s->status_cmd;
        return true;
    }
    if (off == S1_STATUS_CMD + 4 && size == 4) {
        *data = s->status_cmd;
        return true;
    }
    if (off == S1_INTERRUPT && size == 8) {
        *data = s->interrupt;
        return true;
    }
    if (off == S1_INTERRUPT + 4 && size == 4) {
        *data = s->interrupt;
        return true;
    }
    if (off >= S1_DMA_INTERRUPT && off < S1_INTERRUPT) {
        unsigned ch = (off - S1_DMA_INTERRUPT) / 8;

        if (ch < S1_NUM_CHANNELS &&
            ((off - S1_DMA_INTERRUPT) % 8 == 0 || size == 4)) {
            *data = s->dma_interrupt[ch];
            return true;
        }
    }
    if (off == S1_IBUS_ERR_STAT && size == 8) {
        *data = s->ibus_error;
        return true;
    }
    if (off == S1_IBUS_ERR_STAT + 4 && size == 4) {
        *data = s->ibus_error;
        return true;
    }
    if (off == S1_PIO_CLEAR || off == S1_PIO_CLEAR + 4) {
        *data = 0;
        return true;
    }
    return false;
}

static bool s1_core_write(hwaddr off, unsigned size, uint64_t val,
                          SGI_S1State *s)
{
    if (off == S1_STATUS_CMD && size == 8) {
        s->status_cmd &= ~(uint32_t)val;
        s1_update_irq(s);
        return true;
    }
    if (off == S1_STATUS_CMD + 4 && size == 4) {
        s->status_cmd &= ~(uint32_t)val;
        s1_update_irq(s);
        return true;
    }
    if (off == S1_INTERRUPT && (size == 8 || off == S1_INTERRUPT + 4)) {
        s->interrupt = val;
        return true;
    }
    if (off == S1_INTERRUPT + 4 && size == 4) {
        s->interrupt = val;
        return true;
    }
    if (off >= S1_DMA_INTERRUPT && off < S1_INTERRUPT) {
        unsigned ch = (off - S1_DMA_INTERRUPT) / 8;

        if (ch < S1_NUM_CHANNELS &&
            ((off - S1_DMA_INTERRUPT) % 8 == 0 || size == 4)) {
            s->dma_interrupt[ch] = val;
            return true;
        }
    }
    if (off == S1_IBUS_ERR_STAT && size == 8) {
        s->ibus_error &= ~(uint32_t)val;
        return true;
    }
    if (off == S1_IBUS_ERR_STAT + 4 && size == 4) {
        s->ibus_error &= ~(uint32_t)val;
        return true;
    }
    if (off == S1_PIO_CLEAR || off == S1_PIO_CLEAR + 4) {
        return true;
    }
    return false;
}

static uint64_t s1_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    SGI_S1State *s = opaque;
    uint64_t data = 0;

    if (off < S1_CTRL_BASE + S1_NUM_CHANNELS * S1_CHAN_STRIDE) {
        unsigned ch = off / S1_CHAN_STRIDE;
        hwaddr sub = off % S1_CHAN_STRIDE;

        if (sub >= 4 && (sub & 7) == 4) {
            unsigned r = (sub - 4) >> 3;

            data = s1_reg_read(&s->chan[ch], r);
            if (s->trace) {
                fprintf(stderr, "S1RD ch%u r%u=%02x off=%04x size=%u\n",
                        ch, r, (unsigned)data, (unsigned)off, size);
            }
        }
        return data;
    }

    if (off >= S1_DMA_BASE &&
        off < S1_DMA_BASE + S1_NUM_CHANNELS * S1_CHAN_STRIDE) {
        unsigned rel = off - S1_DMA_BASE;
        unsigned ch = rel / S1_CHAN_STRIDE;
        hwaddr sub = rel % S1_CHAN_STRIDE;
        S1Channel *c = &s->chan[ch];

        switch (sub) {
        case S1_DMA_WRITE_OFF + 4:
        case S1_DMA_READ_OFF + 4:
        case S1_DMA_TRANS_LOW + 4:
            data = c->dma_xlat_lo;
            break;
        case S1_DMA_TRANS_HIGH + 4:
            data = c->dma_xlat_hi;
            break;
        default:
            data = 0;
            break;
        }
        return data;
    }

    if (s1_core_read(off, size, &data, s)) {
        return data;
    }

    return 0;
}

static void s1_mmio_write(void *opaque, hwaddr off, uint64_t val,
                          unsigned size)
{
    SGI_S1State *s = opaque;

    if (off < S1_CTRL_BASE + S1_NUM_CHANNELS * S1_CHAN_STRIDE) {
        unsigned ch = off / S1_CHAN_STRIDE;
        hwaddr sub = off % S1_CHAN_STRIDE;

        if (ch < S1_NUM_CHANNELS && sub >= 4 && (sub & 7) == 4) {
            unsigned r = (sub - 4) >> 3;

            if (s->trace) {
                fprintf(stderr, "S1WR ch%u r%u=%02x off=%04x size=%u\n",
                        ch, r, (unsigned)(val & 0xff), (unsigned)off, size);
            }
            s1_reg_write(&s->chan[ch], r, val & 0xff);
        }
        return;
    }

    if (off >= S1_DMA_BASE &&
        off < S1_DMA_BASE + S1_NUM_CHANNELS * S1_CHAN_STRIDE) {
        unsigned rel = off - S1_DMA_BASE;
        unsigned ch = rel / S1_CHAN_STRIDE;
        hwaddr sub = rel % S1_CHAN_STRIDE;
        S1Channel *c = &s->chan[ch];

        if (s->trace) {
            fprintf(stderr, "S1DMAWR ch%u sub=%04x val=%08x size=%u\n",
                    ch, (unsigned)sub, (unsigned)val, size);
        }
        switch (sub) {
        case S1_DMA_WRITE_OFF + 4:
            c->dma_offset = val & 0xfff;
            c->dma_dir_in = true;
            c->dma_armed = true;
            c->xfer_total = 0;
            s1_set_dma_active(c, true);
            s1_out_flush(c);
            if (c->req &&
                (c->fifo_len > c->fifo_pos || c->target_done)) {
                s1_data_bh(s);
            }
            break;
        case S1_DMA_READ_OFF + 4:
            c->dma_offset = val & 0xfff;
            c->dma_dir_in = false;
            c->dma_armed = true;
            c->xfer_total = 0;
            s1_set_dma_active(c, true);
            s1_out_flush(c);
            if (c->req &&
                (c->fifo_len > c->fifo_pos || c->target_done)) {
                s1_data_bh(s);
            }
            break;
        case S1_DMA_TRANS_LOW + 4:
            c->dma_xlat_lo = val;
            break;
        case S1_DMA_TRANS_HIGH + 4:
            c->dma_xlat_hi = val;
            break;
        case S1_DMA_FLUSH + 4:
            c->dma_armed = false;
            s1_set_dma_active(c, false);
            break;
        case S1_DMA_CHAN_RESET + 4:
            c->dma_armed = false;
            c->xfer_total = 0;
            s1_set_dma_active(c, false);
            break;
        default:
            break;
        }
        return;
    }

    s1_core_write(off, size, val, s);
}

static const MemoryRegionOps s1_mmio_ops = {
    .read = s1_mmio_read,
    .write = s1_mmio_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void s1_channel_reset(S1Channel *c, SGI_S1State *s)
{
    if (c->req) {
        scsi_req_unref(c->req);
    }
    g_free(c->fifo);
    memset(c, 0, sizeof(*c));
    c->s = s;
    c->regs[WD95_N_ISRM] = 0x7f;
    c->regs[WD95_N_STOPUM] = 0x3f;
    c->regs[WD95_N_UEIM] = 0xff;
    /* Group 3/4 and 6/7 CDB lengths are stored as (len - 1) nibbles. */
    c->setup_regs[WD95_S_CDBSIZ1] = 0x9f;
    c->setup_regs[WD95_S_CDBSIZ2] = 0x55;
}

static void s1_reset(DeviceState *dev)
{
    SGI_S1State *s = SGI_S1(dev);
    unsigned i;

    s->status_cmd = 0;
    for (i = 0; i < S1_NUM_CHANNELS; i++) {
        s->chan[i].out_pos = 0;
        s->chan[i].out_len = 0;
        s->chan[i].out_buf = NULL;
    }
    s->interrupt = 0;
    s->ibus_error = 0;
    for (i = 0; i < S1_NUM_CHANNELS; i++) {
        s1_channel_reset(&s->chan[i], s);
    }
    s1_update_irq(s);
}

static const char *const s1_scsi_bus_names[S1_NUM_CHANNELS] = {
    "scsi.0", "scsi.1", "scsi.2",
};

static void s1_realize(DeviceState *dev, Error **errp)
{
    SGI_S1State *s = SGI_S1(dev);
    unsigned i;

    for (i = 0; i < S1_NUM_CHANNELS; i++) {
        s1_channel_reset(&s->chan[i], s);
    }
    {
        const char *trace = getenv("SGI_S1_TRACE");

        s->trace = trace && strcmp(trace, "0") != 0;
    }
    s->bh = qemu_bh_new(s1_data_bh, s);
    for (i = 0; i < S1_NUM_CHANNELS; i++) {
        scsi_bus_init_named(&s->bus[i], sizeof(s->bus[i]), dev,
                            &sgi_s1_scsi_info, s1_scsi_bus_names[i]);
    }
    for (i = 0; i < S1_NUM_CHANNELS; i++) {
        scsi_bus_legacy_handle_cmdline(&s->bus[i]);
    }
}

static void s1_init(Object *obj)
{
    SGI_S1State *s = SGI_S1(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &s1_mmio_ops, s, "sgi-s1.mmio",
                          S1_WINDOW_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_sgi_s1 = {
    .name = TYPE_SGI_S1,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(status_cmd, SGI_S1State),
        VMSTATE_UINT32(interrupt, SGI_S1State),
        VMSTATE_UINT32_ARRAY(dma_interrupt, SGI_S1State, S1_NUM_CHANNELS),
        VMSTATE_UINT32(ibus_error, SGI_S1State),
        VMSTATE_END_OF_LIST()
    },
};

static void s1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s1_realize;
    device_class_set_legacy_reset(dc, s1_reset);
    dc->vmsd = &vmstate_sgi_s1;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo s1_type_info = {
    .name = TYPE_SGI_S1,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGI_S1State),
    .instance_init = s1_init,
    .class_init = s1_class_init,
};

static void s1_register_types(void)
{
    type_register_static(&s1_type_info);
}

type_init(s1_register_types)

SGI_S1State *sgi_s1_attach(MemoryRegion *sysmem, hwaddr base)
{
    DeviceState *dev = qdev_new(TYPE_SGI_S1);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    MemoryRegion *mr;
    SGI_S1State *s = SGI_S1(dev);

    object_property_add_child(OBJECT(qdev_get_machine()), "s1", OBJECT(dev));
    sysbus_realize_and_unref(sbd, &error_fatal);
    mr = sysbus_mmio_get_region(sbd, 0);
    memory_region_add_subregion_overlap(sysmem, base, mr, 2);
    return s;
}

qemu_irq sgi_s1_get_irq(SGI_S1State *s)
{
    return s->irq;
}

void sgi_s1_set_dma_alias(SGI_S1State *s, hwaddr base)
{
    s->dma_alias = base;
}

/*
 * QEMU SGI IP19 (Everest / Challenge-Onyx) skeleton
 *
 * Register map derived from the IRIX 6.5.22 kernel headers:
 *   irix/kern/sys/EVEREST/IP19.h
 *   irix/kern/sys/EVEREST/everest.h
 *   irix/kern/sys/EVEREST/mc3.h
 *   irix/kern/sys/EVEREST/io4.h
 *   irix/kern/sys/EVEREST/evmp.h
 *   irix/kern/sys/EVEREST/evconfig.h
 *
 * This is a skeleton: memory, PROM, CPU slots, the CC local register
 * block, the 16-slot configuration bus, RTC/EAROM stubs and the IO4
 * small-window decode with an EPC DUART console stub.  MC3 bank
 * behaviour, IO4 mapping RAM, GIO/DANG, EPC ethernet and the full
 * interrupt grouping are not modelled yet.
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/mips/mips.h"
#include "hw/mips/sgi_s1.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/mips/cpu.h"
#include "qapi/error.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"

#define IP19_MAX_CPUS       4
#define IP19_MAX_SLOTS      16
#define IP19_CFG_REGS       256

#define IP19_PROM_BASE      0x1fc00000
#define IP19_PROM_SIZE      0x20000
/*
 * Flash EPROM window for the master EPC, from transfer.c copy_prom():
 * phys = ((EPC_REGION + 0x10) << 30) + (padap << 27) + off, with the
 * IO4 PROM at PROM_OFF (4096) in the LO window. Served from io4prom.bin
 * when present, zeros otherwise (PROM reports BADMAGIC gracefully).
 */
#define IP19_LWIN_BASE      0x440000000ULL
#define IP19_LWIN_SIZE      0x40000000
#define IP19_IARAM_SIZE     0x10000
#define IP19_FLASH_BASE     0x448000000ULL
#define IP19_FLASH_SIZE     0x800000
#define IP19_CC_BASE        0x18000000
#define IP19_CC_SIZE        0x08000
#define IP19_CFG_BASE       0x18008000
#define IP19_CFG_SIZE       0x08000
#define IP19_BUSTAG_BASE    0x181c0000
#define IP19_BUSTAG_SIZE    0x100000
#define IP19_WGINPUT_BASE   0x18300000
#define IP19_WGINPUT_SIZE   0x010000
#define IP19_RTC_BASE       0x18020000
#define IP19_RTC_SIZE       0x01000
#define IP19_EAROM_BASE     0x19000000
#define IP19_EAROM_SIZE     0x04000
#define IP19_LED_BASE       0x19004000
#define IP19_LED_SIZE       0x01000
#define IP19_SYNC_BASE      0x1a000100
#define IP19_SYNC_SIZE      0x01000
#define IP19_IO4_BASE       0x10000000
#define IP19_IO4_SIZE       0x08000000
#define IP19_S1_BASE        (IP19_IO4_BASE + (1 << 19) + (4 << 16))
#define IP19_GIO_BASE       0x1f000000
#define IP19_GIO_SIZE       0x00a00000

#define IP19_RTC_HZ         50000000ULL

/* CC local resources, IP19.h */
#define CC_SPNUM            0x008
#define CC_KZRESET          0x010
#define CC_SENDINT          0x100
#define CC_SYSCONFIG        0x200
#define CC_WGDST            0x300
#define CC_WGCNTRL          0x308
#define CC_WGCLEAR          0x340
#define CC_UART_CMD         0x400
#define CC_UART_DATA        0x408
#define CC_IP0              0x800
#define CC_IP1              0x808
#define CC_HPIL             0x820
#define CC_CEL              0x828
#define CC_CIPL0            0x830
#define CC_CIPL124          0x850
#define CC_IGRMASK          0x838
#define CC_ILE              0x840
#define CC_ERTOIP           0x900
#define CC_CERTOIP          0x908
#define CC_ECCSB_DIS        0x910
#define CC_RO_COMPARE       0xa00

#define EV_SLOTNUM_SHFT     2
#define EV_PROCNUM_MASK     0x03
#define EV_SLOTNUM_MASK     0x3c
#define EV_SPNUM_MASK       0x3f

/* Configuration register numbers, IP19.h / mc3.h / io4.h */
#define CFG_BOARD_ENABLE    0x00
#define CFG_BOARD_TYPE      0x01
#define CFG_BOARD_LEVEL     0x02
#define CFG_PROC_BASE       0x10
#define CFG_PROC_STRIDE     0x40
#define CFG_PROC_CCREV      0x19
#define CFG_PROC_CACHE_SZ   0x3e

#define CFG_MC3_BANKENB     0x00
#define CFG_MC3_TYPE        0x01
#define CFG_MC3_REVLEVEL    0x02
#define CFG_MC3_ACCESS      0x03
#define CFG_MC3_MEMERRINT   0x04
#define CFG_MC3_EBUSERRINT  0x05
#define CFG_MC3_BISTRESULT  0x06
#define CFG_MC3_DRSCTIMEOUT 0x07
#define CFG_MC3_EBUSERROR   0x08
#define CFG_MC3_REFRESHCNT  0x09
#define CFG_MC3_LEAFCTLENB  0x0a
#define CFG_MC3_RESET       0x0f
#define CFG_MC3_BANK(_l, _b, _r) (0x10 + (_l) * 0x40 + (_b) * 4 + (_r))
#define MC3_NOBANK          7
#define MC3_BANK_SIZE       0
#define MC3_BANK_BASE       1
#define MC3_BANK_IF         2
#define MC3_BANK_IP         3

#define CFG_IO4_SW          0x01
#define CFG_IO4_REVTYPE     0x01
#define CFG_IO4_ADAP        0x02
#define CFG_IO4_INTRVECTOR  0x03
#define CFG_IO4_IODEV0      0x05
#define CFG_IO4_IODEV1      0x06
#define CFG_IO4_IBUSERROR   0x07
#define CFG_IO4_IBUSERRORCLR 0x08
#define CFG_IO4_EBUSERROR   0x09
#define CFG_IO4_EBUSERRORCLR 0x0a
#define CFG_IO4_RESET       0x0d
#define CFG_IO4_ENDIAN      0x0e
#define CFG_IO4_ETIMEOUT    0x0f
#define CFG_IO4_RTIMEOUT    0x10
#define CFG_IO4_INTRMASK    0x11
#define IO4_TYPE_VALUE      0x03
#define IO4_REV_LEVEL       0x01
#define IO4_ADAP_SCSI       0x0d
#define IO4_ADAP_EPC        0x0e

#define IP19_SLOT_CPU       1
#define IP19_SLOT_MC3       2
#define IP19_SLOT_IO4       3

#define IP19_EPC_REGION     1
#define IP19_EPC_PADAP      1

/* Interrupt level groups used by evintr.c get_intr_level(). */
#define IP19_CAUSE_IP2      0x04
#define IP19_CAUSE_IP3      0x08
#define IP19_CAUSE_IP4      0x10
#define IP19_CAUSE_IP5      0x20
#define IP19_CAUSE_IP6      0x40
#define IP19_CAUSE_IP7      0x80

#define SCC_RR0_TX_EMPTY    0x04

#define TYPE_SGI_IP19_MACHINE MACHINE_TYPE_NAME("ip19")
OBJECT_DECLARE_SIMPLE_TYPE(SGI_IP19State, SGI_IP19_MACHINE)

struct SGI_IP19State {
    MachineState parent_obj;

    MemoryRegion prom;
    MemoryRegion lwin;
    MemoryRegion flash;
    MemoryRegion flash_alias;
    MemoryRegion flash_alias_hi;
    MemoryRegion cc;
    MemoryRegion cfg;
    MemoryRegion bustag;
    MemoryRegion wginput;
    MemoryRegion rtc;
    MemoryRegion earom;
    MemoryRegion led;
    MemoryRegion sync;
    MemoryRegion io4;
    MemoryRegion gio;

    uint64_t cfg_regs[IP19_MAX_SLOTS][IP19_CFG_REGS];

    uint64_t ip[IP19_MAX_CPUS][2];
    uint64_t ile[IP19_MAX_CPUS];
    uint64_t igrmask[IP19_MAX_CPUS];
    uint64_t cel[IP19_MAX_CPUS];
    uint64_t wgdst;
    uint64_t wgcntrl;
    uint64_t cc_ertoip;
    uint64_t cc_eccsb_dis;
    uint64_t pgbren;
    uint64_t epc_imask;
    uint64_t epc_ipend;
    int64_t rtc_epoch_ns;
    uint8_t earom_data[IP19_EAROM_SIZE];
    uint32_t iaram[IP19_IARAM_SIZE / 4];
    uint64_t epc_regs[128];
    uint8_t epc_nvram[4096];
    uint8_t epc_xram_page;
    uint8_t epc_rtc_addr;
    uint8_t epc_rtc_regs[16];
    uint32_t io4_ebus_error;

    qemu_irq cpu_irq[IP19_MAX_CPUS][8];

    CharFrontend chr;
    uint8_t scc_sel[IP19_MAX_CPUS * 2];
    uint8_t epc_uart_fifo[256];
    unsigned epc_uart_head;
    unsigned epc_uart_tail;
    uint8_t cc_uart_rx;
    bool cc_uart_rx_ready;
    uint8_t cc_uart_fifo[64];
    unsigned cc_uart_head;
    unsigned cc_uart_tail;
    uint8_t cc_cmd[8];
    unsigned cc_cmd_len;
};

static unsigned ip19_attached_cpu(MemTxAttrs attrs)
{
    unsigned cpu = attrs.requester_id;

    return cpu < IP19_MAX_CPUS ? cpu : 0;
}

static int ip19_hpil(SGI_IP19State *s, unsigned cpu)
{
    int level;

    for (level = 127; level > 0; level--) {
        if (s->ip[cpu][level >> 6] & (1ULL << (level & 63))) {
            return level;
        }
    }
    return 0;
}

static uint64_t ip19_level_cause(int level)
{
    /*
     * Verified against the PROM (pod_ebus.s: level 1 must raise
     * CAUSE_IP3 = SRB_DEV) and the kernel dispatcher (evintr.c
     * get_intr_level: errors via SRB_ERR, everything device-ish via
     * SRB_DEV with HPIL distinguishing the exact level).
     */
    if (level >= 0x78) {
        return IP19_CAUSE_IP5;              /* ERR */
    }
    return IP19_CAUSE_IP2;                  /* DEV */
}

static void ip19_update_irq(SGI_IP19State *s, unsigned cpu)
{
    int level = ip19_hpil(s, cpu);
    uint64_t cause = 0;
    int ip;

    /*
     * ILE bit 0 is EV_EBUSINT_MASK; the kernel sets it to enable
     * device interrupts.  CEL masks levels at or below the current
     * exception level.
     */
    if (level && level > s->cel[cpu] && (s->ile[cpu] & 1)) {
        cause = ip19_level_cause(level);
    }
    for (ip = 0; ip < 8; ip++) {
        qemu_set_irq(s->cpu_irq[cpu][ip], !!(cause & (1ULL << ip)));
    }
}

static void ip19_set_pending(SGI_IP19State *s, unsigned cpu, unsigned level)
{
    s->ip[cpu][level >> 6] |= 1ULL << (level & 63);
    ip19_update_irq(s, cpu);
}

static void ip19_clear_pending(SGI_IP19State *s, unsigned cpu, unsigned level)
{
    s->ip[cpu][level >> 6] &= ~(1ULL << (level & 63));
    ip19_update_irq(s, cpu);
}

static void ip19_deliver_vector(SGI_IP19State *s, uint64_t vector)
{
    unsigned dest = vector & 0x7f;
    unsigned level = (vector >> 8) & 0x7f;
    unsigned i;

    if (dest == 0x7f) {
        for (i = 0; i < IP19_MAX_CPUS; i++) {
            ip19_set_pending(s, i, level);
        }
    } else if (dest & 0x40) {
        unsigned group = dest & 0x3f;

        for (i = 0; i < IP19_MAX_CPUS; i++) {
            if (group < 64 && (s->igrmask[i] & (1ULL << group))) {
                ip19_set_pending(s, i, level);
            }
        }
    } else {
        unsigned slot = (dest >> 2) & 0xf;
        unsigned target = dest & 0x3;

        if (slot == IP19_SLOT_CPU && target < IP19_MAX_CPUS) {
            ip19_set_pending(s, target, level);
        }
    }
}

static void ip19_cc_uart_push(SGI_IP19State *s, uint8_t ch)
{
    unsigned next = (s->cc_uart_head + 1) % sizeof(s->cc_uart_fifo);

    if (next != s->cc_uart_tail) {
        s->cc_uart_fifo[s->cc_uart_head] = ch;
        s->cc_uart_head = next;
    }
}

static MemTxResult ip19_cc_read(void *opaque, hwaddr off, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    unsigned cpu = ip19_attached_cpu(attrs);
    uint64_t ret;

    switch (off) {
    case CC_SPNUM:
        /* slot/proc as encoded by IP19 PROM/kernel */
        ret = ((uint64_t)IP19_SLOT_CPU << EV_SLOTNUM_SHFT) |
              ((uint64_t)cpu & EV_PROCNUM_MASK);
        break;
    case CC_SYSCONFIG:
        /*
         * Presence bits as seen by IP19.
         * Low bits indicate boards present; high bits give board class info.
         */
        ret = (1ULL << IP19_SLOT_CPU) | (1ULL << IP19_SLOT_MC3) |
              (1ULL << IP19_SLOT_IO4) |
              (1ULL << (16 + IP19_SLOT_CPU)) |
              (1ULL << (32 + IP19_SLOT_MC3));
        break;
    case CC_IP0:
        ret = s->ip[cpu][0];
        break;
    case CC_IP1:
        ret = s->ip[cpu][1];
        break;
    case CC_HPIL:
        ret = ip19_hpil(s, cpu);
        break;
    case CC_CEL:
        ret = s->cel[cpu];
        break;
    case CC_IGRMASK:
        ret = s->igrmask[cpu];
        break;
    case CC_ILE:
        ret = s->ile[cpu];
        break;
    case CC_UART_CMD:
        /* i8251 status: TX ready/empty; RX only when a byte is queued. */
        ret = 0x05 | (s->cc_uart_head != s->cc_uart_tail ? 0x02 : 0);
        break;
    case CC_UART_DATA:
        if (s->cc_uart_head != s->cc_uart_tail) {
            ret = s->cc_uart_fifo[s->cc_uart_tail];
            s->cc_uart_tail = (s->cc_uart_tail + 1) %
                              sizeof(s->cc_uart_fifo);
        } else {
            ret = 0;
        }
        break;
    case CC_ERTOIP:
        ret = s->cc_ertoip;
        break;
    case CC_CERTOIP:
        ret = 0;
        break;
    case CC_ECCSB_DIS:
        ret = s->cc_eccsb_dis;
        break;
    case CC_RO_COMPARE:
        ret = 0;
        break;
    default:
        ret = 0;
        break;
    }
    *data = ret;
    return MEMTX_OK;
}

static MemTxResult ip19_cc_read_with_attrs(void *opaque, hwaddr off, uint64_t *data,
                                          unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    unsigned cpu = ip19_attached_cpu(attrs);

    if (off == CC_RO_COMPARE) {
        /*
         * Read-only view of this CPU's timer comparator, programmed
         * via config space (EV_SET_PROCREG). The PROM writes 0x12345678
         * across CMPREG0-3 and expects it back here.
         */
        unsigned base = CFG_PROC_BASE + cpu * CFG_PROC_STRIDE;

        *data = s->cfg_regs[IP19_SLOT_CPU][base] |
                (s->cfg_regs[IP19_SLOT_CPU][base + 1] << 8) |
                (s->cfg_regs[IP19_SLOT_CPU][base + 2] << 16) |
                (s->cfg_regs[IP19_SLOT_CPU][base + 3] << 24);
        return MEMTX_OK;
    }
    if (off == CC_WGDST) {
        *data = s->wgdst;
        return MEMTX_OK;
    }
    if (off == CC_WGCNTRL) {
        *data = s->wgcntrl;
        return MEMTX_OK;
    }
    if (off == CC_WGCLEAR) {
        *data = 0;
        return MEMTX_OK;
    }

    return ip19_cc_read(opaque, off, data, size, attrs);
}

static MemTxResult ip19_cc_write(void *opaque, hwaddr off, uint64_t val,
                                 unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    unsigned cpu = ip19_attached_cpu(attrs);

    switch (off) {
    case CC_KZRESET:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
    case CC_WGDST:
        s->wgdst = val;
        break;
    case CC_WGCNTRL:
        s->wgcntrl = val;
        break;
    case CC_WGCLEAR:
        break;
    case CC_SENDINT:
        /*
         * EVINTR_VECTOR(level, dest): level = bits 14:8, dest = bits 6:0.
         * dest 0x7f = broadcast, 0x40|group = group-directed
         * (delivered to CPUs with that bit in IGRMASK), otherwise
         * (slot << 2) | cpu.
         */
        ip19_deliver_vector(s, val);
        break;
    case CC_IP0:
        s->ip[cpu][0] = val;
        ip19_update_irq(s, cpu);
        break;
    case CC_IP1:
        s->ip[cpu][1] = val;
        ip19_update_irq(s, cpu);
        break;
    case CC_ERTOIP:
        s->cc_ertoip = val;
        break;
    case CC_CERTOIP:
        s->cc_ertoip = 0;
        break;
    case CC_ECCSB_DIS:
        s->cc_eccsb_dis = val;
        break;
    case CC_CEL:
        s->cel[cpu] = val;
        ip19_update_irq(s, cpu);
        break;
    case CC_CIPL0:
    case CC_CIPL124:
        if (val < 128) {
            ip19_clear_pending(s, cpu, val);
        }
        break;
    case CC_IGRMASK:
        s->igrmask[cpu] = val;
        break;
    case CC_ILE:
        s->ile[cpu] = val;
        ip19_update_irq(s, cpu);
        break;
    case CC_UART_CMD:
        break;
    case CC_UART_DATA: {
        uint8_t ch = val & 0xff;

        qemu_chr_fe_write_all(&s->chr, &ch, 1);

        /*
         * Minimal sysctlr protocol responder: answer the debug-switch
         * query with VDS_NO_DIAGS so bring-up can skip the very slow
         * destructive IO4/map-RAM diagnostics.  Other commands are
         * terminated and ignored.
         */
        if (ch == 0x18) {                 /* SC_ESCAPE */
            s->cc_cmd_len = 0;
            s->cc_cmd[s->cc_cmd_len++] = ch;
        } else if (s->cc_cmd_len) {
            if (s->cc_cmd_len < sizeof(s->cc_cmd)) {
                s->cc_cmd[s->cc_cmd_len++] = ch;
            }
            if (ch == 0x0a) {             /* SC_TERM */
                if (s->cc_cmd_len >= 5 && s->cc_cmd[1] == 0x47 &&
                    s->cc_cmd[2] == 0x44 && s->cc_cmd[3] == 0x44) {
                    static const uint8_t resp[] = {
                        0x52, 0x44, 0x44, '0', '0', '1', '0', 0
                    };
                    for (unsigned i = 0; i < sizeof(resp); i++) {
                        ip19_cc_uart_push(s, resp[i]);
                    }
                }
                s->cc_cmd_len = 0;
            }
        }
        break;
    }
    default:
        break;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_cc_ops = {
    .read_with_attrs = ip19_cc_read_with_attrs,
    .write_with_attrs = ip19_cc_write,
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

static bool ip19_mc3_bank_size_reg(unsigned reg)
{
    unsigned leaf, bank;

    for (leaf = 0; leaf < 2; leaf++) {
        for (bank = 0; bank < 4; bank++) {
            if (reg == (unsigned)CFG_MC3_BANK(leaf, bank, MC3_BANK_SIZE)) {
                return true;
            }
        }
    }
    return false;
}

static MemTxResult ip19_cfg_read(void *opaque, hwaddr off, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    unsigned slot = off >> 11;
    unsigned reg = (off & 0x7ff) >> 3;

    if (slot >= IP19_MAX_SLOTS || reg >= IP19_CFG_REGS) {
        *data = 0;
        return MEMTX_OK;
    }
    if (slot == IP19_SLOT_MC3 && reg == CFG_MC3_EBUSERROR) {
        /* EBUS error register: clear on read, no errors modelled. */
        *data = 0;
        return MEMTX_OK;
    }
    if (slot == IP19_SLOT_IO4) {
        switch (reg) {
        case CFG_IO4_REVTYPE:
            /* Reads always report type/rev, not the SW window value. */
            *data = (IO4_REV_LEVEL << 4) | IO4_TYPE_VALUE;
            return MEMTX_OK;
        case CFG_IO4_IBUSERROR:
        case CFG_IO4_IBUSERRORCLR:
            *data = 0;
            return MEMTX_OK;
        case CFG_IO4_EBUSERROR:
            *data = s->io4_ebus_error;
            return MEMTX_OK;
        case CFG_IO4_EBUSERRORCLR:
            *data = s->io4_ebus_error;
            s->io4_ebus_error = 0;
            return MEMTX_OK;
        default:
            break;
        }
    }
    *data = s->cfg_regs[slot][reg];
    return MEMTX_OK;
}

static MemTxResult ip19_cfg_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    unsigned slot = off >> 11;
    unsigned reg = (off & 0x7ff) >> 3;

    if (slot < IP19_MAX_SLOTS && reg < IP19_CFG_REGS) {
        if (slot == IP19_SLOT_MC3 && reg == CFG_MC3_BISTRESULT) {
            /* BIST completes immediately in the model. */
            s->cfg_regs[slot][reg] = 0;
            return MEMTX_OK;
        }
        if (slot == IP19_SLOT_MC3 &&
            (reg == CFG_MC3_TYPE || reg == CFG_MC3_REVLEVEL ||
             ip19_mc3_bank_size_reg(reg))) {
            /* Read-only MC3 identification: ignore writes. */
            return MEMTX_OK;
        }
        if (slot == IP19_SLOT_IO4 &&
            (reg == CFG_IO4_REVTYPE || reg == CFG_IO4_IODEV0 ||
             reg == CFG_IO4_IODEV1 || reg == CFG_IO4_RESET)) {
            /*
             * REVTYPE/IODEVn are read-only identification; RESET must
             * not wipe the model.
             */
            return MEMTX_OK;
        }
        s->cfg_regs[slot][reg] = val;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_cfg_ops = {
    .read_with_attrs = ip19_cfg_read,
    .write_with_attrs = ip19_cfg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

static MemTxResult ip19_rtc_read(void *opaque, hwaddr off, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    /*
     * The PROM checks that the clock advances across two adjacent reads.
     * QEMU_CLOCK_HOST is system uptime, so subtract the machine-start
     * epoch before scaling; otherwise the 50 MHz multiply overflows.
     */
    uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_HOST) - s->rtc_epoch_ns;

    *data = muldiv64(ns, IP19_RTC_HZ, 1000000000ULL);
    return MEMTX_OK;
}

static MemTxResult ip19_rtc_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_rtc_ops = {
    .read_with_attrs = ip19_rtc_read,
    .write_with_attrs = ip19_rtc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

static MemTxResult ip19_earom_read(void *opaque, hwaddr off, uint64_t *data,
                                   unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;

    if (off < IP19_EAROM_SIZE) {
        *data = s->earom_data[off];
    } else {
        *data = 0;
    }
    return MEMTX_OK;
}

static MemTxResult ip19_earom_write(void *opaque, hwaddr off, uint64_t val,
                                    unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;

    if (off < IP19_EAROM_SIZE) {
        s->earom_data[off] = val & 0xff;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_earom_ops = {
    .read_with_attrs = ip19_earom_read,
    .write_with_attrs = ip19_earom_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
};

/*
 * EPC (Enet/Peripheral Controller) model, from sys/EVEREST/epc.h and the
 * IP19 PROM source (epc_regtest.c, pod_io4.c, init_epcuart.c, epcuart.s).
 *
 * Register access is doubleword-oriented (ld/sd, low 32 bits significant)
 * for the 0xa1xx control block, and byte-oriented (lb/sb) for the DUARTs.
 * DUART channel bases: control = duart + chan + 7 (big-endian byte lane),
 * data = control + 0x10, with chan 0x0 (B) / 0x8 (A).
 */
#define EPC_NVRTC_BASE      0x3000
#define EPC_DUART_BASE      0x4000
#define EPC_DUART_STRIDE    0x1000
#define EPC_DUART_CHAN_A    0x08
#define EPC_CTRL_BASE       0xa100

#define EPC_ISTAT           0xa100
#define EPC_IMSET           0xa108
#define EPC_IMRST           0xa110
#define EPC_CTRL_END        0xa400
#define EPC_PRSTCLR         0xa410
#define EPC_IERR            0xa418
#define EPC_IERRC           0xa420

#define EPC_RR0_TX_EMPTY    0x04
#define EPC_RR0_RX_CHR      0x01

static int ip19_epc_can_receive(void *opaque)
{
    SGI_IP19State *s = opaque;
    unsigned used = (s->epc_uart_head - s->epc_uart_tail) &
                    (sizeof(s->epc_uart_fifo) - 1);

    return sizeof(s->epc_uart_fifo) - 1 - used;
}

static void ip19_epc_receive(void *opaque, const uint8_t *buf, int size)
{
    SGI_IP19State *s = opaque;

    while (size-- > 0) {
        unsigned next = (s->epc_uart_head + 1) & (sizeof(s->epc_uart_fifo) - 1);
        if (next == s->epc_uart_tail) {
            break;
        }
        s->epc_uart_fifo[s->epc_uart_head] = *buf++;
        s->epc_uart_head = next;
    }
}

static void ip19_epc_event(void *opaque, QEMUChrEvent event)
{
}

static void ip19_epc_duart_write(SGI_IP19State *s, unsigned duart,
                                 unsigned chan, bool is_data, uint8_t val)
{
    unsigned idx = (duart & 3) * 2 + chan;

    if (is_data) {
        qemu_chr_fe_write_all(&s->chr, &val, 1);
        return;
    }
    /*
     * A write to the control register is WR0; bits 2:0 select the
     * register returned by the next control read.
     */
    s->scc_sel[idx] = val & 0x07;
}

static uint64_t ip19_epc_duart_read(SGI_IP19State *s, unsigned duart,
                                    unsigned chan, bool is_data)
{
    unsigned idx = (duart & 3) * 2 + chan;
    unsigned rr;

    if (is_data) {
        if (s->epc_uart_head != s->epc_uart_tail) {
            uint8_t ch = s->epc_uart_fifo[s->epc_uart_tail];
            s->epc_uart_tail = (s->epc_uart_tail + 1) &
                               (sizeof(s->epc_uart_fifo) - 1);
            qemu_chr_fe_accept_input(&s->chr);
            return ch;
        }
        return 0;
    }
    rr = s->scc_sel[idx];
    s->scc_sel[idx] = 0;       /* control reads return to RR0 */
    switch (rr) {
    case 0:
        /* RR0: TX empty plus RX-ready when input is queued. */
        return EPC_RR0_TX_EMPTY |
               (s->epc_uart_head != s->epc_uart_tail ? EPC_RR0_RX_CHR : 0);
    case 1:
        /* RR1: all characters completely sent, no errors. */
        return 0x01;
    default:
        return 0;
    }
}

/*
 * EPC interrupt sources (sys/EVEREST/epc.h EPC_INTR_*) and their IID
 * registers. When a pending source is unmasked, the programmed IID
 * vector is delivered like a SENDINT.
 */
static const struct {
    unsigned iid_reg;
    uint64_t src_bit;
} ip19_epc_sources[] = {
    { 0xa118, 0x01 }, /* DUART0 */
    { 0xa120, 0x02 }, /* DUART1/2 */
    { 0xa128, 0x04 }, /* ENET */
    { 0xa130, 0x08 }, /* PROFTIM */
    { 0xa138, 0x10 }, /* SPARE */
    { 0xa140, 0x20 }, /* PPORT */
    { 0xa150, 0x80 }, /* ERROR */
};

static void ip19_epc_update_irq(SGI_IP19State *s)
{
    uint64_t active = s->epc_ipend & s->epc_imask;
    unsigned i;

    if (!active) {
        return;
    }
    for (i = 0; i < sizeof(ip19_epc_sources) / sizeof(ip19_epc_sources[0]);
         i++) {
        if (active & ip19_epc_sources[i].src_bit) {
            unsigned idx =
                (ip19_epc_sources[i].iid_reg - EPC_CTRL_BASE) >> 3;
            uint64_t vector = s->epc_regs[idx];

            if (vector) {
                ip19_deliver_vector(s, vector);
            }
        }
    }
}

static uint64_t ip19_epc_ctrl_read(SGI_IP19State *s, unsigned reg)
{
    unsigned idx;

    switch (reg) {
    case EPC_ISTAT:
        return s->epc_ipend & s->epc_imask;
    case EPC_IMSET:
    case EPC_IMRST:
        return 0;
    case EPC_IERRC:
        s->epc_regs[(EPC_IERR - EPC_CTRL_BASE) >> 3] = 0;
        return 0;
    default:
        break;
    }
    if (reg >= EPC_CTRL_BASE &&
        reg < EPC_CTRL_BASE + sizeof(s->epc_regs)) {
        idx = (reg - EPC_CTRL_BASE) >> 3;
        return s->epc_regs[idx];
    }
    return 0;
}

static void ip19_epc_ctrl_write(SGI_IP19State *s, unsigned reg, uint64_t val)
{
    unsigned idx;

    switch (reg) {
    case EPC_IMSET:
        s->epc_imask |= val;
        ip19_epc_update_irq(s);
        return;
    case EPC_IMRST:
        s->epc_imask &= ~val;
        ip19_epc_update_irq(s);
        return;
    case EPC_ISTAT:
        s->epc_imask = val;
        ip19_epc_update_irq(s);
        return;
    case EPC_PRSTCLR:
    case EPC_IERR:
        idx = (reg - EPC_CTRL_BASE) >> 3;
        s->epc_regs[idx] = val;
        ip19_epc_update_irq(s);
        return;
    default:
        break;
    }
    if (reg >= EPC_CTRL_BASE &&
        reg < EPC_CTRL_BASE + sizeof(s->epc_regs)) {
        idx = (reg - EPC_CTRL_BASE) >> 3;
        s->epc_regs[idx] = val;
        ip19_epc_update_irq(s);
    }
}

static void ip19_io4_bad_ioa(SGI_IP19State *s, MemTxAttrs attrs)
{
    s->io4_ebus_error |= 0x0008 | 0x0001; /* BADIOA | STICKY */
    ip19_set_pending(s, ip19_attached_cpu(attrs), 3); /* IO4ERR_LEVEL */
}

static MemTxResult ip19_io4_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    uint64_t rel = addr;
    unsigned region = (rel >> 19) & 0xf;
    unsigned local = rel & 0xffff;

    if (region == IP19_EPC_REGION && ((rel >> 16) & 7) == 0 &&
        local == 0) {
        ip19_io4_bad_ioa(s, attrs);
        *data = ~0ULL;
        return MEMTX_OK;
    }
    if (region == IP19_EPC_REGION) {
        if (local >= EPC_DUART_BASE &&
            local < EPC_DUART_BASE + 3 * EPC_DUART_STRIDE) {
            unsigned duart = (local - EPC_DUART_BASE) / EPC_DUART_STRIDE;
            unsigned ch_off = (local - EPC_DUART_BASE) % EPC_DUART_STRIDE;
            unsigned chan = (ch_off & EPC_DUART_CHAN_A) ? 1 : 0;
            unsigned lane = ch_off - (chan ? EPC_DUART_CHAN_A : 0);

            if (lane == 0x07) {
                *data = ip19_epc_duart_read(s, duart, chan, false);
                return MEMTX_OK;
            }
            if (lane == 0x17) {
                *data = ip19_epc_duart_read(s, duart, chan, true);
                return MEMTX_OK;
            }
            *data = 0;
            return MEMTX_OK;
        }
        if (local >= EPC_NVRTC_BASE && local < EPC_DUART_BASE) {
            /*
             * Dallas-style NVRAM/RTC. XRAM is paged: byte lane offsets
             * (k << 3) + 7, k == 0x20 selects the page register.
             * RTC bank at +0x200 with indirect ADDR/DATA registers.
             */
            unsigned nvr_off = local - EPC_NVRTC_BASE;
            unsigned k = (nvr_off >> 3) & 0x3f;

            if (nvr_off >= 0x200 && nvr_off < 0x220) {
                if (nvr_off == 0x207) {
                    *data = s->epc_rtc_addr;
                } else if (nvr_off == 0x20f) {
                    *data = s->epc_rtc_regs[s->epc_rtc_addr & 0xf];
                } else {
                    *data = 0;
                }
                return MEMTX_OK;
            }
            if (k == 0x20) {
                *data = s->epc_xram_page;
            } else {
                *data = s->epc_nvram[((s->epc_xram_page << 5) |
                                      (k & 0x1f)) & 0xfff];
            }
            return MEMTX_OK;
        }
        if (local >= EPC_CTRL_BASE &&
            local < EPC_CTRL_BASE + sizeof(s->epc_regs)) {
            *data = ip19_epc_ctrl_read(s, local & ~0x7ULL);
            return MEMTX_OK;
        }
        *data = 0;
        return MEMTX_OK;
    }
    *data = ~0ULL;
    return MEMTX_OK;
}

static MemTxResult ip19_io4_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    uint64_t rel = addr;
    unsigned region = (rel >> 19) & 0xf;
    unsigned local = rel & 0xffff;

    if (region == IP19_EPC_REGION && ((rel >> 16) & 7) == 0 &&
        local == 0) {
        ip19_io4_bad_ioa(s, attrs);
        return MEMTX_OK;
    }
    if (region == IP19_EPC_REGION) {
        if (local >= EPC_DUART_BASE &&
            local < EPC_DUART_BASE + 3 * EPC_DUART_STRIDE) {
            unsigned duart = (local - EPC_DUART_BASE) / EPC_DUART_STRIDE;
            unsigned ch_off = (local - EPC_DUART_BASE) % EPC_DUART_STRIDE;
            unsigned chan = (ch_off & EPC_DUART_CHAN_A) ? 1 : 0;
            unsigned lane = ch_off - (chan ? EPC_DUART_CHAN_A : 0);

            if (lane == 0x07) {
                ip19_epc_duart_write(s, duart, chan, false, val & 0xff);
            } else if (lane == 0x17) {
                ip19_epc_duart_write(s, duart, chan, true, val & 0xff);
            }
        } else if (local >= EPC_NVRTC_BASE && local < EPC_DUART_BASE) {
            unsigned nvr_off = local - EPC_NVRTC_BASE;
            unsigned k = (nvr_off >> 3) & 0x3f;

            if (nvr_off >= 0x200 && nvr_off < 0x220) {
                if (nvr_off == 0x207) {
                    s->epc_rtc_addr = val & 0xff;
                } else if (nvr_off == 0x20f) {
                    s->epc_rtc_regs[s->epc_rtc_addr & 0xf] = val & 0xff;
                }
            } else if (k == 0x20) {
                s->epc_xram_page = val & 0xff;
            } else {
                s->epc_nvram[((s->epc_xram_page << 5) |
                              (k & 0x1f)) & 0xfff] = val & 0xff;
            }
        } else if (local >= EPC_CTRL_BASE &&
                   local < EPC_CTRL_BASE + sizeof(s->epc_regs)) {
            ip19_epc_ctrl_write(s, local & ~0x7ULL, val);
        }
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_io4_ops = {
    .read_with_attrs = ip19_io4_read,
    .write_with_attrs = ip19_io4_write,
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

static MemTxResult ip19_lwin_read(void *opaque, hwaddr off, uint64_t *data,
                                  unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;

    if (off + size <= IP19_IARAM_SIZE && (off & 3) == 0 && size == 4) {
        *data = s->iaram[off / 4];
        return MEMTX_OK;
    }
    *data = 0;
    return MEMTX_OK;
}

static MemTxResult ip19_lwin_write(void *opaque, hwaddr off, uint64_t val,
                                   unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;

    if (off + size <= IP19_IARAM_SIZE && (off & 3) == 0 && size == 4) {
        s->iaram[off / 4] = val;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_lwin_ops = {
    .read_with_attrs = ip19_lwin_read,
    .write_with_attrs = ip19_lwin_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static MemTxResult ip19_gio_read(void *opaque, hwaddr off, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    *data = ~0ULL;
    return MEMTX_OK;
}

static MemTxResult ip19_gio_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_gio_ops = {
    .read_with_attrs = ip19_gio_read,
    .write_with_attrs = ip19_gio_write,
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

static MemTxResult ip19_ram_read(void *opaque, hwaddr off, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    *data = 0;
    return MEMTX_OK;
}

static MemTxResult ip19_ram_write(void *opaque, hwaddr off, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_ram_ops = {
    .read_with_attrs = ip19_ram_read,
    .write_with_attrs = ip19_ram_write,
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

static MemTxResult ip19_sync_read(void *opaque, hwaddr off, uint64_t *data,
                                  unsigned size, MemTxAttrs attrs)
{
    *data = 0;
    return MEMTX_OK;
}

static MemTxResult ip19_sync_write(void *opaque, hwaddr off, uint64_t val,
                                   unsigned size, MemTxAttrs attrs)
{
    /* CC sync signal - acknowledge */
    return MEMTX_OK;
}

static const MemoryRegionOps ip19_sync_ops = {
    .read_with_attrs = ip19_sync_read,
    .write_with_attrs = ip19_sync_write,
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

static void ip19_cfg_defaults(SGI_IP19State *s, MachineState *machine)
{
    unsigned proc;
    uint64_t ram_size = machine->ram_size;
    unsigned mem_size_code = 2; /* reasonable default for 128MB+ */

    if (ram_size >= 256 * MiB) {
        mem_size_code = 3;
    }
    if (ram_size >= 512 * MiB) {
        mem_size_code = 4;
    }
    if (ram_size >= 1024ULL * MiB) {
        mem_size_code = 5;
    }

    s->cfg_regs[IP19_SLOT_CPU][CFG_BOARD_ENABLE] = 0x0f;
    s->cfg_regs[IP19_SLOT_CPU][CFG_BOARD_TYPE] = 0x01; /* IP19 */
    s->cfg_regs[IP19_SLOT_CPU][CFG_BOARD_LEVEL] = 0x03;
    for (proc = 0; proc < machine->smp.cpus && proc < IP19_MAX_CPUS; proc++) {
        unsigned base = CFG_PROC_BASE + proc * CFG_PROC_STRIDE;

        s->cfg_regs[IP19_SLOT_CPU][base + CFG_PROC_CCREV] = 0x1e; /* CC rev 2 */
        s->cfg_regs[IP19_SLOT_CPU][base + CFG_PROC_CACHE_SZ] = mem_size_code;
    }

    /* MC3 on slot 1 */
    s->cfg_regs[IP19_SLOT_MC3][CFG_BOARD_ENABLE] = 0x01;
    s->cfg_regs[IP19_SLOT_MC3][CFG_BOARD_TYPE] = 0x09; /* MC3 */
    s->cfg_regs[IP19_SLOT_MC3][CFG_BOARD_LEVEL] = 0x01;
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_TYPE] = 0x02;
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_REVLEVEL] = 0x03;
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_BISTRESULT] = 0;
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_LEAFCTLENB] = 0x0f;
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_BANKENB] = 0x01;
    /* Bring-up uses one 256 MiB bank until bank interleave is modelled. */
    for (unsigned leaf = 0; leaf < 2; leaf++) {
        for (unsigned bank = 0; bank < 4; bank++) {
            s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_BANK(leaf, bank,
                                                   MC3_BANK_SIZE)] =
                MC3_NOBANK;
        }
    }
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_BANK(0, 0, MC3_BANK_SIZE)] =
        mem_size_code;
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_BANK(0, 0, MC3_BANK_BASE)] = 0x00;
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_BANK(0, 0, MC3_BANK_IF)] = 0x00;
    s->cfg_regs[IP19_SLOT_MC3][CFG_MC3_BANK(0, 0, MC3_BANK_IP)] = 0x00;

    /* IO4 on slot 2 */
    s->cfg_regs[IP19_SLOT_IO4][CFG_BOARD_ENABLE] = 0x01;
    s->cfg_regs[IP19_SLOT_IO4][CFG_BOARD_TYPE] = 0x0a; /* IO4 */
    s->cfg_regs[IP19_SLOT_IO4][CFG_BOARD_LEVEL] = 0x01;
    s->cfg_regs[IP19_SLOT_IO4][CFG_IO4_REVTYPE] =
        (IO4_REV_LEVEL << 4) | IO4_TYPE_VALUE;
    /* IODEV bytes are indexed by physical adapter number. */
    s->cfg_regs[IP19_SLOT_IO4][CFG_IO4_IODEV0] = IO4_ADAP_EPC << 8;
    s->cfg_regs[IP19_SLOT_IO4][CFG_IO4_IODEV1] = IO4_ADAP_SCSI;
}

static void ip19_earom_defaults(SGI_IP19State *s)
{
    memset(s->earom_data, 0, sizeof(s->earom_data));
    /* EV_BE_LOC: EV_EAROM_BYTE0 with the big-endian bit set. */
    s->earom_data[0x00] = 0x0c;
    /* EV_CACHE_SZ_LOC: 4 MiB secondary cache (byte offset 0x128). */
    s->earom_data[0x128] = 22;
    /* EV_EBUSRATE0..3: 50 MHz EBUS. */
    s->earom_data[0x100] = 0x80;
    s->earom_data[0x108] = 0xf0;
    s->earom_data[0x110] = 0xfa;
    s->earom_data[0x118] = 0x02;
    s->earom_data[0x120] = 1;            /* EV_PGBRDEN_LOC */
    s->earom_data[0x190] = 1;            /* EV_ECCENB_LOC */
    /* EV_EPROCRATE0..3. */
    s->earom_data[0x140] = 0x00;
    s->earom_data[0x148] = 0xe1;
    s->earom_data[0x150] = 0x00;
    s->earom_data[0x158] = 0x0f;
    /* EV_RTCFREQ0..3. */
    s->earom_data[0x160] = 0x00;
    s->earom_data[0x168] = 0x2d;
    s->earom_data[0x170] = 0xc6;
    s->earom_data[0x178] = 0x02;
    /* Checksum bytes stay zero so the PROM skips checksum validation. */
}

static void ip19_preload_io4_segments(const char *filename)
{
    g_autofree gchar *data = NULL;
    gsize len = 0;

    if (!filename || !g_file_get_contents(filename, &data, &len, NULL) ||
        len < 0x20 || memcmp(data, "JKSW", 4) != 0 ||
        memcmp(data + 0x18, "SEG2", 4) != 0) {
        return;
    }

    uint32_t nseg = ldl_be_p(data + 0x1c);
    for (uint32_t i = 0; i < nseg; i++) {
        const uint8_t *seg = (const uint8_t *)data + 0x20 + i * 48;
        uint32_t type, off, length;
        uint64_t start;

        if (0x20 + (i + 1) * 48 > len) {
            break;
        }
        type = ldl_be_p(seg);
        off = ldl_be_p(seg + 4);
        start = ldq_be_p(seg + 16);
        length = ldl_be_p(seg + 24);
        if (off + length > len || length > 8 * MiB) {
            continue;
        }
        /* Load MASTER and R4000 (not TFP/R10k) segments. */
        if (type == 0xff01 || (type & 0xff00) == 0x0100) {
            address_space_write(&address_space_memory,
                                start & 0x1fffffff, MEMTXATTRS_UNSPECIFIED,
                                (const uint8_t *)data + off, length);
        }
    }
}

static void sgi_ip19_init(MachineState *machine)
{
    SGI_IP19State *s = SGI_IP19_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Clock *cpuclk;
    int i, j;

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, IP19_RTC_HZ);

    for (i = 0; i < machine->smp.cpus; i++) {
        MIPSCPU *cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk,
                                                  TARGET_BIG_ENDIAN);

        CPU(cpu)->cpu_index = i;
        cpu->env.CP0_Config0 &= ~(1U << 17);
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        for (j = 0; j < 8; j++) {
            s->cpu_irq[i][j] = cpu->env.irq[j];
        }
    }

    qemu_chr_fe_init(&s->chr, serial_hd(0), &error_abort);
    qemu_chr_fe_set_handlers(&s->chr, ip19_epc_can_receive, ip19_epc_receive,
                             ip19_epc_event, NULL, s, NULL, true);
    s->cc_uart_rx = 0;
    s->cc_uart_rx_ready = false;
    s->rtc_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);

    memory_region_add_subregion(sysmem, 0, machine->ram);

    memory_region_init_rom(&s->prom, NULL, "ip19.prom", IP19_PROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, IP19_PROM_BASE, &s->prom);
    {
        const char *firmware = machine->firmware ?: "ip19prom.bin";
        g_autofree char *filename =
            qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware);

        if (filename) {
            load_image_targphys(filename, IP19_PROM_BASE, IP19_PROM_SIZE,
                                NULL);
        } else {
            error_report("ip19: no PROM image '%s'", firmware);
        }
    }

    memory_region_init_io(&s->lwin, NULL, &ip19_lwin_ops, s, "ip19.lwin",
                          IP19_LWIN_SIZE);
    memory_region_add_subregion(sysmem, IP19_LWIN_BASE, &s->lwin);

    memory_region_init_ram(&s->flash, NULL, "ip19.flash", IP19_FLASH_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, IP19_FLASH_BASE, &s->flash);
    memory_region_init_alias(&s->flash_alias, NULL, "ip19.flash-lwin-lo",
                             &s->flash, 0, 0x80000);
    memory_region_add_subregion(&s->lwin, 0x08000000, &s->flash_alias);
    memory_region_init_alias(&s->flash_alias_hi, NULL, "ip19.flash-lwin-hi",
                             &s->flash, 0x80000, IP19_FLASH_SIZE - 0x80000);
    memory_region_add_subregion(&s->lwin, 0x08100000, &s->flash_alias_hi);
    {
        g_autofree char *filename =
            qemu_find_file(QEMU_FILE_TYPE_BIOS, "io4prom.bin");

        if (filename) {
            load_image_targphys(filename, IP19_FLASH_BASE, IP19_FLASH_SIZE,
                                NULL);

        }
    }

    memory_region_init_io(&s->cc, NULL, &ip19_cc_ops, s, "ip19.cc",
                          IP19_CC_SIZE);
    memory_region_add_subregion(sysmem, IP19_CC_BASE, &s->cc);

    ip19_cfg_defaults(s, machine);
    memory_region_init_io(&s->cfg, NULL, &ip19_cfg_ops, s, "ip19.cfg",
                          IP19_CFG_SIZE);
    memory_region_add_subregion(sysmem, IP19_CFG_BASE, &s->cfg);

    memory_region_init_ram(&s->bustag, NULL, "ip19.bustag",
                           IP19_BUSTAG_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, IP19_BUSTAG_BASE, &s->bustag);

    memory_region_init_io(&s->wginput, NULL, &ip19_ram_ops, s, "ip19.wginput",
                          IP19_WGINPUT_SIZE);
    memory_region_add_subregion(sysmem, IP19_WGINPUT_BASE, &s->wginput);

    memory_region_init_io(&s->rtc, NULL, &ip19_rtc_ops, s, "ip19.rtc",
                          IP19_RTC_SIZE);
    memory_region_add_subregion(sysmem, IP19_RTC_BASE, &s->rtc);

    ip19_earom_defaults(s);
    memory_region_init_io(&s->earom, NULL, &ip19_earom_ops, s, "ip19.earom",
                          IP19_EAROM_SIZE);
    memory_region_add_subregion(sysmem, IP19_EAROM_BASE, &s->earom);

    memory_region_init_io(&s->led, NULL, &ip19_ram_ops, s, "ip19.led",
                          IP19_LED_SIZE);
    memory_region_add_subregion(sysmem, IP19_LED_BASE, &s->led);

    memory_region_init_io(&s->sync, NULL, &ip19_sync_ops, s, "ip19.sync",
                          IP19_SYNC_SIZE);
    memory_region_add_subregion(sysmem, IP19_SYNC_BASE, &s->sync);

    memory_region_init_io(&s->io4, NULL, &ip19_io4_ops, s, "ip19.io4",
                          IP19_IO4_SIZE);
    memory_region_add_subregion(sysmem, IP19_IO4_BASE, &s->io4);

    sgi_s1_attach(sysmem, IP19_S1_BASE, blk_by_name("ip19disk"),
                  blk_by_name("ip19cdrom"), blk_by_name("ip19cdrom2"));


    memory_region_init_io(&s->gio, NULL, &ip19_gio_ops, s, "ip19.gio",
                          IP19_GIO_SIZE);
    memory_region_add_subregion(sysmem, IP19_GIO_BASE, &s->gio);
}

static void sgi_ip19_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI IP19 (Everest Challenge/Onyx)";
    mc->init = sgi_ip19_init;
    mc->max_cpus = IP19_MAX_CPUS;
    mc->default_cpus = 1;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R4000SC");
    mc->default_ram_size = 512 * MiB;
    mc->default_ram_id = "sgi_ip19.ram";
}

static const TypeInfo sgi_ip19_type = {
    .name = TYPE_SGI_IP19_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(SGI_IP19State),
    .class_init = sgi_ip19_class_init,
};

static void sgi_ip19_machine_init(void)
{
    type_register_static(&sgi_ip19_type);
}

type_init(sgi_ip19_machine_init)

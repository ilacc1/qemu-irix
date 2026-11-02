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
#include "qemu/bswap.h"
#include "chardev/char-fe.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/mips/mips.h"
#include "hw/mips/sgi_s1.h"
#include "hw/mips/sgi_mc3.h"
#include "hw/mips/sgi_z8530.h"
#include "hw/core/qdev.h"
#include "accel/tcg/cpu-loop.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/cpus.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/mips/cpu.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/mem-reentrancy.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include <time.h>

static time_t ip19_timegm(struct tm *tm)
{
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

static bool ip19_hold_secondaries(void)
{
    static int hold = -1;

    if (hold < 0) {
        const char *e = g_getenv("IP19_HOLD_CPUS");

        hold = e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y');
    }
    return hold;
}

static bool ip19_debug_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = g_getenv("IP19_DEBUG") != NULL;
    }
    return enabled;
}

#define IP19_DPRINT(...)     do { if (ip19_debug_enabled()) fprintf(stderr, "ip19: " __VA_ARGS__); } while (0)

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
/*
 * IO4_CONF_SW (written by IP19prom's initialize_io4()) selects window 1.
 * A window is 8 physical-adapter slots of 64 KiB each, so the only
 * small-window physical range decoded by the IO4 is
 * 0x10080000..0x100fffff.  On a machine larger than 256 MiB that range is
 * also ordinary RAM: the hardware distinguishes the two by cache
 * attribute, with K1/uncached accesses hitting IO4 and cached K0/K2
 * accesses hitting memory.  QEMU has no cache model, so the MIPS
 * translation code redirects uncached references in this range to
 * IP19_IO4_ALIAS_BASE, and the IO4/EPC/S1 model lives at that alias.  The
 * alias deliberately keeps region field 1 so the IO4 register encoding is
 * unchanged.
 */
#define IP19_IO4_BASE       0x10000000
#define IP19_IO4_WINDOW     1
#define IP19_IO4_PADAPS     8
#define IP19_IO4_PHYS_BASE  (IP19_IO4_BASE + (IP19_IO4_WINDOW << 19))
#define IP19_IO4_SIZE       (IP19_IO4_PADAPS << 16)
#define IP19_IO4_ALIAS_BASE 0x400080000ULL
#define IP19_IO4_REGION_BASE IP19_IO4_ALIAS_BASE
#define IP19_EPC_REGION     IP19_IO4_WINDOW
#define IP19_EPC_PADAP      1
#define IP19_S1_PADAP       4
#define IP19_S1_BASE        (IP19_IO4_REGION_BASE + (IP19_S1_PADAP << 16))
#define IP19_S1_SIZE        0x8000
#define IP19_GIO_BASE       0x1f000000
#define IP19_GIO_SIZE       0x00a00000
/*
 * The kernel treats all low physical memory as usable (Everest has no bad
 * pages, see memsupport.c setupbadmem()) and reaches the CC/config/RTC/
 * EAROM/LED/sync/GIO resources through K1 uncached aliases.  DMA and cached
 * kernel references to the same physical addresses are ordinary RAM.  Move
 * the local-resource model to a private physical alias and let the MIPS
 * translation code redirect uncached references there, just as it already
 * does for the IO4 small window.
 */
#define IP19_LOCAL_PHYS_BASE 0x18000000ULL
#define IP19_LOCAL_ALIAS_BASE 0x400100000ULL
#define IP19_LOCAL_ALIAS(_phys) \
    (IP19_LOCAL_ALIAS_BASE + ((_phys) - IP19_LOCAL_PHYS_BASE))

#define IP19_RTC_HZ         47619048ULL
#define IP19_CPU_HZ         500000000ULL

/*
 * IP19 uses R4000SC parts at revision 3.0 or later.  The IP19 kernel is
 * built without JUMP_WAR (see kern/kcommondefs), so coproc_find() panics
 * when CP0 PRId reports a pre-3.0 R4000 (major <= 2, minor <= 2).  QEMU's
 * R4000SC model reports revision 0.0; publish the minimum supported IP19
 * revision instead.
 */
#define IP19_CPU_PRID       0x00000430

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
#define CFG_PROC_CMPREG0    0x10
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

#define CFG_IO4_LW          0x00
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
#define IO4_REV_LEVEL       0x02
#define IO4_ADAP_NULL       0x00
#define IO4_ADAP_SCSI       0x0d
#define IO4_ADAP_SCIP       0x1d
#define IO4_ADAP_EPC        0x0e

#define IP19_SLOT_CPU       1
#define IP19_SLOT_MC3       2
#define IP19_SLOT_IO4       3

#define IO4_EBUSERROR_BADIOA   0x0008
#define IO4_EBUSERROR_STICKY   0x0001
#define IO4_INTRMASK_ERROR     0x0002
#define IO4_INTRMASK_DISABLE   0x0001

/* Interrupt level groups used by evintr.c get_intr_level(). */
#define IP19_CAUSE_IP2      0x04
#define IP19_CAUSE_IP3      0x08
#define IP19_CAUSE_IP4      0x10
#define IP19_CAUSE_IP5      0x20
#define IP19_CAUSE_IP6      0x40
#define IP19_CAUSE_IP7      0x80

/* CC local-interrupt enable bits in EV_ILE. */
#define EV_ILE_EBUSINT      0x01
#define EV_ILE_CMPINT       0x02

#define SCC_RR0_TX_EMPTY    0x04

#define TYPE_SGI_IP19_MACHINE MACHINE_TYPE_NAME("ip19")
OBJECT_DECLARE_SIMPLE_TYPE(SGI_IP19State, SGI_IP19_MACHINE)

typedef struct IP19CmpTimer {
    struct SGI_IP19State *s;
    unsigned cpu;
} IP19CmpTimer;

struct SGI_IP19State {
    MachineState parent_obj;

    MemoryRegion prom;
    MemoryRegion lwin;
    MemoryRegion flash;
    MemoryRegion flash_alias;
    MemoryRegion flash_alias_hi;
    uint8_t *flash_data;
    bool flash_program_setup;
    bool flash_erase_setup;
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

    SgiMC3State *mc3;
    SGI_S1State *s1;
    SgiZ8530State *epc_duart[3];
    uint8_t epc_duart_irq[3];

    uint64_t cfg_regs[IP19_MAX_SLOTS][IP19_CFG_REGS];

    uint64_t ip[IP19_MAX_CPUS][2];
    bool rtc_yield;
    bool rtc_yield_armed;
    int64_t rtc_yield_until_ns;
    uint64_t grp_pending[64][2];
    uint64_t grp_delivered[64][IP19_MAX_CPUS][2];
    /*
     * Reset hold for the secondary slices.  The IP19 PROM arbitration
     * is based on the host RTC and the per-CPU SPNUM timeout, so with
     * more than one runnable vCPU the winner depended on host timing.
     * Keep every non-bootmaster slice halted until CPU 0 has claimed
     * the master group.  A late slice then takes the PROM's slave path.
     */
    bool cpu_held[IP19_MAX_CPUS];
    bool bootmaster_claimed;
    uint64_t ile[IP19_MAX_CPUS];
    uint64_t igrmask[IP19_MAX_CPUS];
    uint64_t cel[IP19_MAX_CPUS];
    uint32_t cmpreg[IP19_MAX_CPUS];
    bool cmpint[IP19_MAX_CPUS];
    QEMUTimer *cmp_timer[IP19_MAX_CPUS];
    IP19CmpTimer cmp_ctx[IP19_MAX_CPUS];
    uint64_t wgdst;
    uint64_t wgcntrl;
    uint64_t cc_ertoip;
    uint64_t cc_eccsb_dis;
    uint64_t pgbren;
    uint64_t epc_imask;
    uint64_t epc_ipend;
    uint64_t epc_prst;
    uint64_t io4_lw;
    uint64_t io4_sw;
    int64_t rtc_epoch_ns;
    uint8_t earom_data[IP19_EAROM_SIZE];
    uint32_t iaram[IP19_IARAM_SIZE / 4];
    uint64_t epc_regs[128];
    uint8_t epc_nvram[4096];
    uint8_t epc_xram_page;
    uint8_t epc_rtc_addr;
    uint8_t epc_rtc_regs[16];
    int64_t epc_rtc_base_ns;
    int64_t epc_rtc_base_sec;
    char sysctlr_serial[16];
    uint32_t io4_ebus_error;
    uint64_t io4_error_vector;

    NICConf epc_nic_conf;
    NICState *nic;
    MemReentrancyGuard epc_reentrancy_guard;
    uint8_t epc_mac[6];

    qemu_irq cpu_irq[IP19_MAX_CPUS][8];

    Chardev *cc_uart_chr;
    uint8_t cc_uart_rx;
    bool cc_uart_rx_ready;
    uint8_t cc_uart_fifo[64];
    unsigned cc_uart_head;
    unsigned cc_uart_tail;
    uint8_t cc_cmd[32];
    unsigned cc_cmd_len;
};

static void ip19_epc_update_irq(SGI_IP19State *s);
static void ip19_set_pending(SGI_IP19State *s, unsigned cpu,
                             unsigned level);
static void ip19_clear_pending(SGI_IP19State *s, unsigned cpu,
                               unsigned level);

static void ip19_s1_irq(void *opaque, int n, int level)
{
    SGI_IP19State *s = opaque;

    if (level) {
        ip19_set_pending(s, 0, 0x4f);
    } else {
        ip19_clear_pending(s, 0, 0x4f);
    }
}

static void ip19_epc_duart_irq(void *opaque, int n, int level)
{
    SGI_IP19State *s = opaque;
    uint64_t pending = 0;

    if (n < 0 || n >= 3) {
        return;
    }
    s->epc_duart_irq[n] = !!level;

    if (s->epc_duart_irq[0]) {
        pending |= 0x01;                    /* EPC_INTR_DUART0 */
    }
    if (s->epc_duart_irq[1] || s->epc_duart_irq[2]) {
        pending |= 0x02;                    /* EPC_INTR_DUART12 */
    }
    s->epc_ipend = (s->epc_ipend & ~0x03ULL) | pending;
    ip19_epc_update_irq(s);
}

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

static uint64_t ip19_rtc_value(SGI_IP19State *s)
{
    /*
     * The PROM checks that the clock advances across two adjacent reads.
     * QEMU_CLOCK_HOST is system uptime, so subtract the machine-start
     * epoch before scaling; otherwise the 50 MHz multiply overflows.
     */
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    uint64_t ns = now - s->rtc_epoch_ns;
    uint64_t hz = IP19_RTC_HZ;
    const char *div = g_getenv("IP19_RTC_DIV");

    if (div) {
        unsigned d = atoi(div);

        if (d > 1) {
            hz /= d;
        }
    }
    return muldiv64(ns, hz, 1000000000ULL);
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
    if (level && level > s->cel[cpu] && (s->ile[cpu] & EV_ILE_EBUSINT)) {
        cause = ip19_level_cause(level);
    }
    /*
     * The CC timer comparator is a separate local interrupt class
     * (EV_CMPINT).  It is not part of EV_IP0/EV_IP1, and the kernel
     * dispatches it through Cause IP3 (SRB_TIMOCLK) to timein().
     */
    if (s->cmpint[cpu] && (s->ile[cpu] & EV_ILE_CMPINT)) {
        cause |= IP19_CAUSE_IP3;
    }
    for (ip = 0; ip < 8; ip++) {
        /*
         * IP7 is the CPU-internal Count/Compare timer.  Never drive it
         * from the system controller: the machine would otherwise clear
         * a pending timer edge whenever it recomputes the CC interrupt
         * state, and the guest clock would stop.
         */
        if (ip == 7) {
            continue;
        }
        qemu_set_irq(s->cpu_irq[cpu][ip], !!(cause & (1ULL << ip)));
    }
}

static void ip19_cmp_expire(void *opaque)
{
    IP19CmpTimer *ctx = opaque;
    SGI_IP19State *s = ctx->s;

    s->cmpint[ctx->cpu] = true;
    ip19_update_irq(s, ctx->cpu);
}

/*
 * The CC has a free-running 50 MHz RTC and a per-processor 32-bit
 * comparator.  When the low 32 bits of the RTC reach CMPREG the CC
 * raises the CMPINT local interrupt; it stays pending until software
 * acknowledges it through EV_CIPL124.
 */
static void ip19_cmp_rearm(SGI_IP19State *s, unsigned cpu)
{
    uint32_t now;
    uint32_t delta;
    int64_t now_ns;
    int64_t fire_ns;

    if (!s->cmp_timer[cpu]) {
        return;
    }
    now = (uint32_t)ip19_rtc_value(s);
    delta = s->cmpreg[cpu] - now;
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);

    if (!delta) {
        delta = 1;
    }
    fire_ns = now_ns + muldiv64(delta, 1000000000ULL, IP19_RTC_HZ);
    timer_mod(s->cmp_timer[cpu], fire_ns);
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

static void ip19_release_secondary_cpus(SGI_IP19State *s)
{
    CPUState *cs;

    CPU_FOREACH(cs) {
        unsigned cpu = cs->cpu_index;

        if (cpu == 0 || cpu >= IP19_MAX_CPUS || !s->cpu_held[cpu]) {
            continue;
        }
        s->cpu_held[cpu] = false;
        cs->halted = 0;
        qemu_cpu_kick(cs);
        IP19_DPRINT("release cpu %u\n", cpu);
    }
}

static void ip19_group_deliver_one(SGI_IP19State *s, unsigned cpu,
                                  unsigned group)
{
    unsigned word;
    uint64_t fresh;

    if (group >= 64 || cpu >= IP19_MAX_CPUS) {
        return;
    }
    for (word = 0; word < 2; word++) {
        fresh = s->grp_pending[group][word] &
                ~s->grp_delivered[group][cpu][word];
        if (fresh) {
            s->ip[cpu][word] |= fresh;
            s->grp_delivered[group][cpu][word] |= fresh;
        }
    }
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
        unsigned word = level >> 6;
        uint64_t bit = 1ULL << (level & 63);

        if (group < 64) {
            /*
             * A group-directed SENDINT is an edge aimed at the group
             * membership as it exists now.  Slaves join their group
             * later during PROM boot, so keep the edge latched and
             * hand it to each CPU when it first joins the group.
             */
            s->grp_pending[group][word] |= bit;
            for (i = 0; i < IP19_MAX_CPUS; i++) {
                s->grp_delivered[group][i][word] &= ~bit;
            }
            for (i = 0; i < IP19_MAX_CPUS; i++) {
                if (s->igrmask[i] & (1ULL << group)) {
                    ip19_set_pending(s, i, level);
                    s->grp_delivered[group][i][word] |= bit;
                }
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

static void ip19_clear_vector(SGI_IP19State *s, uint64_t vector)
{
    unsigned dest = vector & 0x7f;
    unsigned level = (vector >> 8) & 0x7f;
    unsigned i;

    if (dest == 0x7f) {
        for (i = 0; i < IP19_MAX_CPUS; i++) {
            ip19_clear_pending(s, i, level);
        }
    } else if (dest & 0x40) {
        unsigned group = dest & 0x3f;

        for (i = 0; i < IP19_MAX_CPUS; i++) {
            if (group < 64 && (s->igrmask[i] & (1ULL << group))) {
                ip19_clear_pending(s, i, level);
            }
        }
    } else {
        unsigned slot = (dest >> 2) & 0xf;
        unsigned target = dest & 0x3;

        if (slot == IP19_SLOT_CPU && target < IP19_MAX_CPUS) {
            ip19_clear_pending(s, target, level);
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

static void ip19_cc_uart_putc(SGI_IP19State *s, uint8_t ch)
{
    /*
     * The CC UART is a debug console only.  Write directly to the shared
     * host backend; the Z8530 owns the receive side of that chardev.
     */
    if (s->cc_uart_chr) {
        qemu_chr_write(s->cc_uart_chr, &ch, 1, true);
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
        IP19_DPRINT("SPNUM read cpu %u -> 0x%02" PRIx64 "\n", cpu, ret);
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
        if (ip19_debug_enabled()) {
            CPUState *cs;
            uint8_t buf[8];

            CPU_FOREACH(cs) {
                MIPSCPU *mc = MIPS_CPU(cs);

                IP19_DPRINT("KZRESET cpu %u pc %016" PRIx64
                            " sr %08x cause %08x epc %016" PRIx64 "\n",
                            cs->cpu_index, (uint64_t)mc->env.active_tc.PC,
                            mc->env.CP0_Status, mc->env.CP0_Cause,
                            (uint64_t)mc->env.CP0_EPC);
            }
            if (address_space_read(&address_space_memory, 0x3f1dcc,
                                   MEMTXATTRS_UNSPECIFIED, buf, 4) ==
                MEMTX_OK) {
                IP19_DPRINT("KZRESET cb_wait %d\n", (int32_t)ldl_be_p(buf));
            }
            if (address_space_read(&address_space_memory, 0x3fa540,
                                   MEMTXATTRS_UNSPECIFIED, buf, 4) ==
                MEMTX_OK) {
                IP19_DPRINT("KZRESET bootlck %08x\n", ldl_be_p(buf));
            }
            if (address_space_read(&address_space_memory, 0x3fde50,
                                   MEMTXATTRS_UNSPECIFIED, buf, 4) ==
                MEMTX_OK) {
                IP19_DPRINT("KZRESET pdaindr1.CpuId %d\n",
                            (int32_t)ldl_be_p(buf));
            }
        }
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        /*
         * IRIX writes KZRESET and immediately panics if the CPU
         * continues.  Exit the current translation block now so the
         * main loop can perform the reset before the panic executes.
         */
        if (current_cpu) {
            cpu_loop_exit(current_cpu);
        }
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
        /*
         * The bootmaster follows IGRMASK=IGR_MASTER with a level-6
         * broadcast.  Release the held slices only after that edge has
         * been delivered, so the broadcast cannot arrive after a late
         * slice has reached the slave loop (where level 6 means POD).
         */
        if (s->bootmaster_claimed && (val & 0x7f) == 0x7f &&
            ((val >> 8) & 0x7f) == 0x6) {
            ip19_release_secondary_cpus(s);
        }
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
    case CC_CIPL124:
        /* EV_CIPL124 takes a mask of local interrupt classes. */
        if (val & EV_ILE_CMPINT) {
            s->cmpint[cpu] = false;
            ip19_update_irq(s, cpu);
        }
        /* fall through */
    case CC_CIPL0:
        if (val < 128) {
            ip19_clear_pending(s, cpu, val);
        }
        break;
    case CC_IGRMASK:
        if (val == 0) {
            memset(s->grp_pending, 0, sizeof(s->grp_pending));
            memset(s->grp_delivered, 0, sizeof(s->grp_delivered));
        }
        s->igrmask[cpu] = val;
        /*
         * Joining the master group is the bootmaster's claim.  Only
         * CPU 0 can reach this point before the other slices are
         * released, so the election is deterministic.
         */
        IP19_DPRINT("IGRMASK write cpu %u val 0x%02" PRIx64
                    " claimed %d\n", cpu, val, s->bootmaster_claimed);
        /*
         * The CC self-test also writes 0xffff/0xaaaa/0x5555/0 to
         * IGRMASK, so only the exact IGR_MASTER value 1 is a claim.
         * Do not release the held slices here: the bootmaster's next
         * CC write is the BOOT_LEVEL broadcast, and a slice that is
         * released before that broadcast can reach the PROM slave loop
         * first, where BOOT_LEVEL and POD_LEVEL are both 6.
         */
        if (!s->bootmaster_claimed && val == 0x1) {
            s->bootmaster_claimed = true;
        }
        for (unsigned group = 0; group < 64; group++) {
            if (val & (1ULL << group)) {
                ip19_group_deliver_one(s, cpu, group);
            }
        }
        break;
    case CC_ILE:
        s->ile[cpu] = val;
        ip19_update_irq(s, cpu);
        break;
    case CC_UART_CMD:
        break;
    case CC_UART_DATA: {
        uint8_t ch = val & 0xff;

        ip19_cc_uart_putc(s, ch);

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
                    /*
                     * Debug-switch query.  Report VDS_NO_DIAGS so
                     * bring-up skips the very slow destructive
                     * IO4/map-RAM diagnostics.
                     */
                    static const uint8_t resp[] = {
                        0x52, 0x44, 0x44, '0', '0', '1', '0', 0
                    };
                    for (unsigned i = 0; i < sizeof(resp); i++) {
                        ip19_cc_uart_push(s, resp[i]);
                    }
                } else if (s->cc_cmd_len >= 5 && s->cc_cmd[1] == 0x47 &&
                           s->cc_cmd[2] == 0x53 &&
                           s->cc_cmd[3] == 0x53) {
                    /*
                     * SC_GET SC_SERIAL: reply R, sequence, type, then
                     * the NUL-terminated serial number.  The PROM and
                     * kernel both use this to validate the system
                     * controller serial.
                     */
                    unsigned len = strlen(s->sysctlr_serial) + 1;

                    if (len > 10) {
                        len = 10;
                    }
                    ip19_cc_uart_push(s, 0x52);      /* SC_RESP */
                    ip19_cc_uart_push(s, 0x53);      /* SC_SERIAL */
                    ip19_cc_uart_push(s, 0x53);      /* SC_SERIAL */
                    for (unsigned i = 0; i < len; i++) {
                        uint8_t c = i < strlen(s->sysctlr_serial)
                                    ? s->sysctlr_serial[i] : 0;
                        ip19_cc_uart_push(s, c);
                        if (c == 0) {
                            break;
                        }
                    }
                } else if (s->cc_cmd_len >= 4 && s->cc_cmd[1] == 0x53 &&
                           s->cc_cmd[2] == 0x53) {
                    /*
                     * SC_SET SC_SERIAL: store a new NUL-padded serial
                     * (up to the 10-byte protocol limit).
                     */
                    unsigned len = s->cc_cmd_len - 4;
                    unsigned i;

                    if (len > sizeof(s->sysctlr_serial) - 1) {
                        len = sizeof(s->sysctlr_serial) - 1;
                    }
                    for (i = 0; i < len; i++) {
                        s->sysctlr_serial[i] = s->cc_cmd[3 + i];
                    }
                    s->sysctlr_serial[i] = '\0';
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
    if (slot == IP19_SLOT_IO4) {
        switch (reg) {
        case CFG_IO4_LW:
            *data = s->io4_lw;
            return MEMTX_OK;
        case CFG_IO4_SW:
            /*
             * The SW window register left-shifts the window number by
             * eight bits; REVTYPE is the read-only low-byte alias.
             */
            *data = s->io4_sw |
                    ((uint64_t)((IO4_REV_LEVEL << 4) | IO4_TYPE_VALUE));
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
            if (s->io4_error_vector) {
                ip19_clear_vector(s, s->io4_error_vector);
                s->io4_error_vector = 0;
            }
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
        if (slot == IP19_SLOT_CPU && reg >= CFG_PROC_CMPREG0) {
            unsigned proc = (reg - CFG_PROC_CMPREG0) / CFG_PROC_STRIDE;
            unsigned preg = (reg - CFG_PROC_CMPREG0) % CFG_PROC_STRIDE;

            if (proc < IP19_MAX_CPUS && preg <= 3) {
                unsigned base = CFG_PROC_CMPREG0 +
                                proc * CFG_PROC_STRIDE;

                s->cfg_regs[slot][reg] = val;
                s->cmpreg[proc] =
                    (s->cfg_regs[slot][base] & 0xff) |
                    ((s->cfg_regs[slot][base + 1] & 0xff) << 8) |
                    ((s->cfg_regs[slot][base + 2] & 0xff) << 16) |
                    ((s->cfg_regs[slot][base + 3] & 0xff) << 24);
                /*
                 * The kernel writes the LSB alone to disable the timeout
                 * interrupt and writes all four bytes to re-enable it.
                 * Re-arm on every byte so both cases follow the hardware
                 * comparator value.
                 */
                ip19_cmp_rearm(s, proc);
                return MEMTX_OK;
            }
        }
        if (slot == IP19_SLOT_IO4) {
            switch (reg) {
            case CFG_IO4_LW:
                s->io4_lw = val;
                return MEMTX_OK;
            case CFG_IO4_SW:
                /* Window number is stored in bits 20:8, type/rev in 7:0. */
                s->io4_sw = (val & 0x1fffff) << 8;
                return MEMTX_OK;
            case CFG_IO4_IODEV0:
            case CFG_IO4_IODEV1:
            case CFG_IO4_RESET:
                /* IODEVn are read-only identification; RESET is ignored. */
                return MEMTX_OK;
            default:
                break;
            }
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
    uint64_t v = ip19_rtc_value(s);
    unsigned shift;

    /*
     * The CC RTC is a 64-bit big-endian counter.  The IRIX flashio
     * delay code maps it and advances to the second 32-bit word
     * (`rtc++`), i.e. the low half of the counter.  Honour the access
     * offset so that a 4-byte load at +4 returns the fast low word
     * instead of the high word.
     */
    switch (size) {
    case 1:
        shift = 8 * (7 - (off & 7));
        *data = (v >> shift) & 0xff;
        break;
    case 2:
        shift = 8 * (6 - (off & 6));
        *data = (v >> shift) & 0xffff;
        break;
    case 4:
        shift = 8 * (4 - (off & 4));
        *data = (v >> shift) & 0xffffffffu;
        break;
    case 8:
        *data = v;
        break;
    default:
        *data = 0;
        break;
    }
    /*
     * Single-threaded TCG round-robin only switches vCPUs when the
     * running one exits its current TB.  The IRIX AP bootstrap polls
     * the CC RTC while spinning, and the master polls it in its boot
     * timeout loop.  Forcing an exit on an RTC read gives the AP a
     * chance to run in the window between the master servicing its
     * cb_wait request and checking pdaindr[CpuId].
     */
    if (s->rtc_yield && current_cpu) {
        uint64_t pc = (uint64_t)MIPS_CPU(current_cpu)->env.active_tc.PC;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_HOST);
        bool kernel = pc >= 0xa800000000004000ULL &&
                      pc < 0xa800000000400000ULL;

        /*
         * Once a secondary CPU is running the kernel (its frequency
         * calibration polls the RTC), the bootmaster's boot-timeout
         * loop must yield between polls.  It does not otherwise leave
         * its TB in single-threaded TCG.  Arm a single boot window:
         * later kernel RTC traffic from another CPU must not keep
         * forcing the bootmaster out of every us_delay() call.  The
         * installed 6.5.22 kernel links us_delay at
         * 0xa800000000011284.
         */
        if (kernel && current_cpu->cpu_index != 0 && !s->rtc_yield_armed) {
            s->rtc_yield_armed = true;
            s->rtc_yield_until_ns = now + 30 * 1000000000LL;
        }
        if (current_cpu->cpu_index == 0 && kernel &&
            pc >= 0xa800000000011280ULL && pc < 0xa800000000011340ULL &&
            now < s->rtc_yield_until_ns) {
            cpu_exit(current_cpu);
        }
    }
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
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/*
 * The IO4 flash is two ganged Intel 8-bit parts, accessed as 16-bit
 * halfwords.  The IRIX flashio utility (irix/cmd/flashio) issues the
 * Intel command sequences: RESET, SETUP_PROGRAM+data, SETUP_ERASE+ERASE
 * and the PROGRAM/ERASE verify commands.  Model just enough of that
 * behaviour for erase/program/verify to complete; reads always return
 * the array contents.
 */
static uint64_t ip19_flash_read(void *opaque, hwaddr off, unsigned size)
{
    SGI_IP19State *s = opaque;

    off &= IP19_FLASH_SIZE - 1;
    if (off + size > IP19_FLASH_SIZE) {
        return 0;
    }

    switch (size) {
    case 1:
        return s->flash_data[off];
    case 2:
        return lduw_be_p(s->flash_data + off);
    case 4:
        return ldl_be_p(s->flash_data + off);
    case 8:
        return ldq_be_p(s->flash_data + off);
    default:
        return 0;
    }
}

static void ip19_flash_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    SGI_IP19State *s = opaque;

    off &= IP19_FLASH_SIZE - 1;
    if (off + size > IP19_FLASH_SIZE) {
        return;
    }

    if (size == 2) {
        uint16_t v = val & 0xffff;

        if (s->flash_program_setup) {
            stw_be_p(s->flash_data + off, v);
            s->flash_program_setup = false;
            return;
        }

        switch (v) {
        case 0xffff:                    /* RESET */
            s->flash_erase_setup = false;
            return;
        case 0x4040:                    /* SETUP_PROGRAM */
            s->flash_program_setup = true;
            s->flash_erase_setup = false;
            return;
        case 0x2020:                    /* SETUP_ERASE / ERASE */
            if (s->flash_erase_setup) {
                memset(s->flash_data, 0xff, IP19_FLASH_SIZE);
                s->flash_erase_setup = false;
            } else {
                s->flash_erase_setup = true;
            }
            return;
        case 0xa0a0:                    /* ERASE_VERIFY */
        case 0xc0c0:                    /* PROGRAM_VERIFY */
            s->flash_erase_setup = false;
            return;
        default:
            stw_be_p(s->flash_data + off, v);
            return;
        }
    }

    switch (size) {
    case 1:
        s->flash_data[off] = val;
        break;
    case 4:
        stl_be_p(s->flash_data + off, val);
        break;
    case 8:
        stq_be_p(s->flash_data + off, val);
        break;
    default:
        break;
    }
    s->flash_program_setup = false;
    s->flash_erase_setup = false;
}

static const MemoryRegionOps ip19_flash_ops = {
    .read = ip19_flash_read,
    .write = ip19_flash_write,
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
#define EPC_EADDR_BASE      0xa200
#define EPC_TCMD            0xa230
#define EPC_RCMD            0xa238
#define EPC_TBASELO         0xa250
#define EPC_TBASEHI         0xa258
#define EPC_TLIMIT          0xa260
#define EPC_TINDEX          0xa268
#define EPC_TTOP            0xa270
#define EPC_TBPTR           0xa278
#define EPC_TSTAT           0xa280
#define EPC_RBASELO         0xa2a0
#define EPC_RBASEHI         0xa2a8
#define EPC_RLIMIT          0xa2b0
#define EPC_RINDEX          0xa2b8
#define EPC_RTOP            0xa2c0
#define EPC_RBPTR           0xa2c8
#define EPC_RSTAT           0xa2d0

#define EPC_ENET_BUFSZ      2048
#define EPC_ENET_DATA       2
#define EPC_ENET_STAT       2044
#define EPC_ENET_LENMASK    0x7ff
#define EPC_ENET_STSHFT     16
#define EPC_ENET_STMASK     0xff
#define EPC_ENET_ST_GOOD    0x20
#define EPC_ENET_ST_END     0x10
#define EPC_ENET_XS_SUCCESS 0x08
#define EPC_ENET_RCMDRECV   0xc0

#define EPC_ISTAT           0xa100
#define EPC_IMSET           0xa108
#define EPC_IMRST           0xa110
#define EPC_PRST            0xa400
#define EPC_PRSTSET         0xa408
#define EPC_PRSTCLR         0xa410
#define EPC_IERR            0xa418
#define EPC_IERRC           0xa420

#define EPC_INTR_DUART0     0x01
#define EPC_INTR_DUART12    0x02

/* EPC PBUS Ethernet ID PROM (pdev 2). */
#define EPC_ENETPROM_BASE   0x2000

/* Everest NVRAM layout (irix/kern/sys/EVEREST/nvram.h). */
#define EPC_NV_OLD_CHECKSUM     0
#define EPC_NV_REVISION         1
#define EPC_NV_CONSOLE          2
#define EPC_NV_SYSPART          4
#define EPC_NV_OSLOADER         52
#define EPC_NV_OSFILE           70
#define EPC_NV_OSOPTS           98
#define EPC_NV_DBGTTY           121
#define EPC_NV_DISKLESS         141
#define EPC_NV_TZ               142
#define EPC_NV_OSPART           150
#define EPC_NV_AUTOLOAD         198
#define EPC_NV_ROOT             199
#define EPC_NV_NETADDR          221
#define EPC_NV_SCSIHOSTID       272
#define EPC_NV_SGILOGO          273
#define EPC_NV_NONSTOP          275
#define EPC_NV_INVENT_VALID     276
#define EPC_NV_INVENTORY        277
#define EPC_NV_LBAUD            116
#define EPC_NV_RBAUD            790
#define EPC_NV_RESTART          795
#define EPC_NV_SNUMVALID        853
#define EPC_NV_SNUMSIZE         854
#define EPC_NV_SERIALNUM        855
#define EPC_NV_REBOUND          871
#define EPC_NV_NEW_CHECKSUM     872
#define EPC_NV_TOD_CYCLE        3694
#define EPC_NV_LOWFREE          3695
#define EPC_NV_HIGHFREE         4058
#define EPC_NVRAM_SIZE          4096
#define EPC_NV_SERIAL_MAGIC     0x5a

/* Dallas DS1397 RTC register numbers (epc.h). */
#define NVR_SEC                 0x00
#define NVR_MIN                 0x02
#define NVR_HOUR                0x04
#define NVR_WEEKDAY             0x06
#define NVR_DAY                 0x07
#define NVR_MONTH               0x08
#define NVR_YEAR                0x09
#define NVR_RTCA                0x0a
#define NVR_RTCB                0x0b
#define NVR_RTCC                0x0c
#define NVR_RTCD                0x0d
#define NVR_SET                 0x80
#define NVR_DM                  0x04
#define NVR_MILTIME             0x02
#define NVR_OSC_ON              0x20
#define NVR_VRT                 0x80

static uint8_t ip19_bin_to_bcd(unsigned v)
{
    return ((v / 10) << 4) | (v % 10);
}

static unsigned ip19_bcd_to_bin(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0xf);
}

static void ip19_epc_rtc_now(SGI_IP19State *s, struct tm *tm)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    int64_t sec = s->epc_rtc_base_sec +
                  (now - s->epc_rtc_base_ns) / 1000000000LL;
    time_t t = (time_t)sec;

    gmtime_r(&t, tm);
}

static uint8_t ip19_epc_rtc_read_reg(SGI_IP19State *s, unsigned reg)
{
    uint8_t regb = s->epc_rtc_regs[NVR_RTCB];
    struct tm tm;
    unsigned v;

    reg &= 0xf;
    switch (reg) {
    case NVR_RTCA:
        /* Keep the oscillator on; UIP is never asserted by this model. */
        return (s->epc_rtc_regs[reg] & 0x70) | NVR_OSC_ON;
    case NVR_RTCB:
        return regb;
    case NVR_RTCC:
        return 0;
    case NVR_RTCD:
        return NVR_VRT;
    default:
        break;
    }

    /*
     * While the SET bit is asserted the kernel writes the time fields a
     * byte at a time.  Return the staged bytes so read-modify-write
     * sequences see what they just wrote.
     */
    if ((regb & NVR_SET) && reg <= NVR_YEAR) {
        return s->epc_rtc_regs[reg];
    }

    ip19_epc_rtc_now(s, &tm);
    switch (reg) {
    case NVR_SEC:
        v = tm.tm_sec;
        break;
    case NVR_MIN:
        v = tm.tm_min;
        break;
    case NVR_HOUR:
        v = tm.tm_hour;
        break;
    case NVR_WEEKDAY:
        v = tm.tm_wday + 1;      /* DS1397: 1 = Sunday */
        break;
    case NVR_DAY:
        v = tm.tm_mday;
        break;
    case NVR_MONTH:
        v = tm.tm_mon + 1;
        break;
    case NVR_YEAR:
        v = (tm.tm_year + 1900 - 1970) % 100;
        break;
    default:
        /* Alarm registers: plain storage. */
        return s->epc_rtc_regs[reg];
    }

    if (!(regb & NVR_DM) && reg != NVR_WEEKDAY) {
        v = ip19_bin_to_bcd(v);
    }
    if (reg == NVR_HOUR && !(regb & NVR_MILTIME)) {
        unsigned hour = tm.tm_hour;
        bool pm = hour >= 12;

        hour %= 12;
        if (!hour) {
            hour = 12;
        }
        v = (regb & NVR_DM) ? hour : ip19_bin_to_bcd(hour);
        if (pm) {
            v |= 0x80;
        }
    }
    return v & 0xff;
}

static void ip19_epc_rtc_write_reg(SGI_IP19State *s, unsigned reg,
                                   uint8_t val)
{
    reg &= 0xf;

    if (reg == NVR_RTCB && (s->epc_rtc_regs[NVR_RTCB] & NVR_SET) &&
        !(val & NVR_SET)) {
        struct tm tm = { 0 };
        bool dm = s->epc_rtc_regs[NVR_RTCB] & NVR_DM;
        bool mil = s->epc_rtc_regs[NVR_RTCB] & NVR_MILTIME;
        unsigned sec = s->epc_rtc_regs[NVR_SEC];
        unsigned min = s->epc_rtc_regs[NVR_MIN];
        unsigned hour = s->epc_rtc_regs[NVR_HOUR];
        unsigned day = s->epc_rtc_regs[NVR_DAY];
        unsigned mon = s->epc_rtc_regs[NVR_MONTH];
        unsigned year = s->epc_rtc_regs[NVR_YEAR];
        time_t t;

        if (!dm) {
            sec = ip19_bcd_to_bin(sec);
            min = ip19_bcd_to_bin(min);
            hour = ip19_bcd_to_bin(hour & 0x7f);
            day = ip19_bcd_to_bin(day);
            mon = ip19_bcd_to_bin(mon);
            year = ip19_bcd_to_bin(year);
        } else {
            hour &= 0x7f;
        }
        if (!mil) {
            bool pm = s->epc_rtc_regs[NVR_HOUR] & 0x80;

            hour %= 12;
            if (pm) {
                hour += 12;
            }
        }

        tm.tm_sec = sec;
        tm.tm_min = min;
        tm.tm_hour = hour;
        tm.tm_mday = day;
        tm.tm_mon = mon ? mon - 1 : 0;
        tm.tm_year = year + 70;
        tm.tm_isdst = 0;
        t = ip19_timegm(&tm);
        if (t != (time_t)-1) {
            s->epc_rtc_base_sec = t;
            s->epc_rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);
        }
    }

    s->epc_rtc_regs[reg] = val;
    if (reg == NVR_RTCA) {
        s->epc_rtc_regs[reg] |= NVR_OSC_ON;
    }
}

#define EPC_NV_MAX_LEN 64
static const struct {
    unsigned offset;
    unsigned length;
    const char *value;
} ip19_nv_defaults[] = {
    { EPC_NV_CONSOLE,     2,  "d" },
    { EPC_NV_SYSPART,     48, "dksc(0,1,8)" },
    { EPC_NV_OSLOADER,    18, "sash" },
    { EPC_NV_OSFILE,      28, "unix" },
    { EPC_NV_DBGTTY,      20, "multi(0)serial(0)" },
    { EPC_NV_DISKLESS,    1,  "0" },
    { EPC_NV_TZ,          8,  "PST8PDT" },
    { EPC_NV_OSPART,      48, "dksc(0,1,0)" },
    { EPC_NV_AUTOLOAD,    1,  "Y" },
    { EPC_NV_ROOT,        20, "dks0d1s0" },
    { EPC_NV_SCSIHOSTID,  1,  "0" },
    { EPC_NV_SGILOGO,     1,  "y" },
    { EPC_NV_NONSTOP,     1,  "0" },
    { EPC_NV_LBAUD,       5,  "9600" },
    { EPC_NV_RBAUD,       5,  "19200" },
};

static uint8_t ip19_nv_old_checksum(SGI_IP19State *s)
{
    uint8_t checksum = 0xa5;
    unsigned i;

    /*
     * Match IRIX's oldnvchecksum(): it reads byte 0 of each 32-byte
     * XRAM page 32 times and rotates through carry.  The checksum byte
     * itself contributes nothing because it is included an even number
     * of times.
     */
    for (i = 0; i < EPC_NVRAM_SIZE; i++) {
        uint8_t v = s->epc_nvram[i & 0x3e0];

        if (v != EPC_NV_OLD_CHECKSUM) {
            checksum ^= v;
        }
        checksum = (checksum << 1) | (checksum >> 7);
    }
    return checksum;
}

static uint8_t ip19_nv_new_checksum(SGI_IP19State *s)
{
    uint8_t checksum = 0xa5;
    unsigned i;

    for (i = 0; i < EPC_NV_HIGHFREE; i++) {
        if (i != EPC_NV_NEW_CHECKSUM && i != EPC_NV_RESTART) {
            checksum ^= s->epc_nvram[i];
        }
        checksum = (checksum << 1) | (checksum >> 7);
    }
    return checksum;
}


static void ip19_epc_nvram_defaults(SGI_IP19State *s)
{
    static const char serial[] = "S1234567";
    struct tm tm;
    unsigned i, tod_cycle;
    time_t now;

    memset(s->epc_nvram, 0, sizeof(s->epc_nvram));
    for (i = 0; i < ARRAY_SIZE(ip19_nv_defaults); i++) {
        size_t len = strlen(ip19_nv_defaults[i].value);

        if (len > ip19_nv_defaults[i].length) {
            len = ip19_nv_defaults[i].length;
        }
        memcpy(&s->epc_nvram[ip19_nv_defaults[i].offset],
               ip19_nv_defaults[i].value, len);
    }

    s->epc_nvram[EPC_NV_REVISION] = 9; /* PROM-reported NV_BROKEN_REV */

    /* Valid system-controller/NVRAM serial number. */
    s->epc_nvram[EPC_NV_SNUMVALID] = EPC_NV_SERIAL_MAGIC;
    s->epc_nvram[EPC_NV_SNUMSIZE] = sizeof(serial) - 1;
    memcpy(&s->epc_nvram[EPC_NV_SERIALNUM], serial, sizeof(serial));

    /*
     * Seed the TOD leap-cycle byte so rtodc() does not treat the clock
     * as bogus on first read.  It is derived exactly as IRIX does from
     * the two-digit RTC year and month.
     */
    now = time(NULL);
    gmtime_r(&now, &tm);
    tod_cycle = ((((tm.tm_year + 1900) % 100) % 4 == 2 && tm.tm_mon >= 2) ||
                 (((tm.tm_year + 1900) % 100) % 4 == 3) ||
                 (((tm.tm_year + 1900) % 100) % 4 == 0 && tm.tm_mon < 2)) + 1;
    s->epc_nvram[EPC_NV_TOD_CYCLE] = tod_cycle;

    /* Both checksum variants are used by different PROM/kernel vintages. */
    s->epc_nvram[EPC_NV_OLD_CHECKSUM] = 0;
    s->epc_nvram[EPC_NV_OLD_CHECKSUM] = ip19_nv_old_checksum(s);
    s->epc_nvram[EPC_NV_NEW_CHECKSUM] = ip19_nv_new_checksum(s);
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

/*
 * EPC Ethernet scatter/gather engine (if_ee.c).  The controller walks a
 * ring of 2048-byte buffers in guest physical memory.  The guest owns
 * TTOP/RTOP and the model owns TINDEX/RINDEX; each packet starts at
 * offset 2 and the transmit length is a little-endian halfword at offset
 * 0, while the receive status is a big-endian word at offset 2044.
 */
static uint64_t ip19_epc_get_reg(SGI_IP19State *s, unsigned reg)
{
    return s->epc_regs[(reg - EPC_CTRL_BASE) >> 3];
}

static void ip19_epc_set_reg(SGI_IP19State *s, unsigned reg, uint64_t val)
{
    s->epc_regs[(reg - EPC_CTRL_BASE) >> 3] = val;
}

static unsigned ip19_epc_enet_nbufs(uint64_t limit)
{
    return 1U << ((limit & 7) + 1);
}

static uint64_t ip19_epc_enet_base(SGI_IP19State *s, unsigned lo_reg,
                                   unsigned hi_reg)
{
    return ip19_epc_get_reg(s, lo_reg) |
           (ip19_epc_get_reg(s, hi_reg) << 32);
}

static void ip19_epc_dma_read(hwaddr addr, void *buf, unsigned len)
{
    if (address_space_read(&address_space_memory, addr,
                           MEMTXATTRS_UNSPECIFIED, buf, len) != MEMTX_OK) {
        memset(buf, 0, len);
    }
}

static void ip19_epc_dma_write(hwaddr addr, const void *buf, unsigned len)
{
    address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
}

static void ip19_epc_enet_tx_pump(SGI_IP19State *s)
{
    uint64_t base = ip19_epc_enet_base(s, EPC_TBASELO, EPC_TBASEHI);
    unsigned nbufs = ip19_epc_enet_nbufs(ip19_epc_get_reg(s, EPC_TLIMIT));
    unsigned mask = nbufs - 1;
    unsigned tindex = ip19_epc_get_reg(s, EPC_TINDEX) & mask;
    unsigned ttop = ip19_epc_get_reg(s, EPC_TTOP) & mask;
    uint8_t packet[EPC_ENET_BUFSZ];

    if (!base) {
        /* The driver has not programmed the ring; drop queued entries. */
        ip19_epc_set_reg(s, EPC_TINDEX, ttop);
        return;
    }

    while (tindex != ttop) {
        uint64_t buf = base + (uint64_t)tindex * EPC_ENET_BUFSZ;
        uint8_t hdr[2];
        uint8_t status[4];
        unsigned len;

        ip19_epc_dma_read(buf, hdr, sizeof(hdr));
        len = hdr[0] | (hdr[1] << 8);
        if (len >= 14 && len <= sizeof(packet)) {
            ip19_epc_dma_read(buf + EPC_ENET_DATA, packet, len);
            if (s->nic) {
                qemu_send_packet(qemu_get_queue(s->nic), packet, len);
            }
        }

        stl_be_p(status, EPC_ENET_XS_SUCCESS << EPC_ENET_STSHFT);
        ip19_epc_dma_write(buf + EPC_ENET_STAT, status, sizeof(status));
        tindex = (tindex + 1) & mask;
    }
    ip19_epc_set_reg(s, EPC_TINDEX, tindex);
}

static bool ip19_epc_enet_rx_ready(SGI_IP19State *s)
{
    uint64_t base = ip19_epc_enet_base(s, EPC_RBASELO, EPC_RBASEHI);
    unsigned nbufs;
    unsigned mask;
    unsigned rindex;
    unsigned rtop;

    if (!s->nic || !base) {
        return false;
    }
    if (!(ip19_epc_get_reg(s, EPC_RCMD) & EPC_ENET_RCMDRECV)) {
        return false;
    }

    nbufs = ip19_epc_enet_nbufs(ip19_epc_get_reg(s, EPC_RLIMIT));
    mask = nbufs - 1;
    rindex = ip19_epc_get_reg(s, EPC_RINDEX) & mask;
    rtop = ip19_epc_get_reg(s, EPC_RTOP) & mask;
    return ((rtop - rindex) & mask) != 0;
}

static bool ip19_epc_enet_can_receive(NetClientState *nc)
{
    SGI_IP19State *s = qemu_get_nic_opaque(nc);

    return ip19_epc_enet_rx_ready(s);
}

static ssize_t ip19_epc_enet_receive(NetClientState *nc, const uint8_t *buf,
                                     size_t size)
{
    SGI_IP19State *s = qemu_get_nic_opaque(nc);
    uint64_t base = ip19_epc_enet_base(s, EPC_RBASELO, EPC_RBASEHI);
    unsigned nbufs = ip19_epc_enet_nbufs(ip19_epc_get_reg(s, EPC_RLIMIT));
    unsigned mask = nbufs - 1;
    unsigned rindex = ip19_epc_get_reg(s, EPC_RINDEX) & mask;
    uint64_t dst = base + (uint64_t)rindex * EPC_ENET_BUFSZ;
    uint8_t packet[EPC_ENET_BUFSZ];
    uint8_t hdr[2];
    uint8_t status[4];
    uint32_t rstat;
    unsigned len;

    if (!ip19_epc_enet_rx_ready(s) || size < 6 || size > 1514) {
        /* Returning the size drops an unusable frame. */
        return size;
    }

    len = MAX(size, 60);
    memcpy(packet, buf, size);
    memset(packet + size, 0, len - size);

    hdr[0] = len & 0xff;
    hdr[1] = (len >> 8) & 0xff;
    rstat = ((EPC_ENET_ST_GOOD | EPC_ENET_ST_END) << EPC_ENET_STSHFT) |
            (len & EPC_ENET_LENMASK);

    ip19_epc_dma_write(dst, hdr, sizeof(hdr));
    ip19_epc_dma_write(dst + EPC_ENET_DATA, packet, len);
    stl_be_p(status, rstat);
    ip19_epc_dma_write(dst + EPC_ENET_STAT, status, sizeof(status));

    ip19_epc_set_reg(s, EPC_RINDEX, (rindex + 1) & mask);
    s->epc_ipend |= 0x04;             /* EPC_INTR_ENET */
    ip19_epc_update_irq(s);
    return size;
}

static NetClientInfo ip19_epc_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = ip19_epc_enet_can_receive,
    .receive = ip19_epc_enet_receive,
};

static uint64_t ip19_epc_ctrl_read(SGI_IP19State *s, unsigned reg)
{
    unsigned idx;

    switch (reg) {
    case EPC_ISTAT:
        return s->epc_ipend & s->epc_imask;
    case EPC_IMSET:
    case EPC_IMRST:
        return 0;
    case EPC_TINDEX:
        return ip19_epc_get_reg(s, EPC_TINDEX);
    case EPC_RINDEX:
        /* Polling the receive index acknowledges the EPC ENET source. */
        s->epc_ipend &= ~0x04;        /* EPC_INTR_ENET */
        return ip19_epc_get_reg(s, EPC_RINDEX);
    case EPC_PRST:
        return s->epc_prst;
    case EPC_IERRC:
        idx = (EPC_IERR - EPC_CTRL_BASE) >> 3;
        {
            uint64_t value = s->epc_regs[idx];

            s->epc_regs[idx] = 0;
            return value;
        }
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
    case EPC_ISTAT:
        /* ISTAT is status-only; mask updates go through IMSET/IMRST. */
        return;
    case EPC_IMSET:
        s->epc_imask |= val;
        ip19_epc_update_irq(s);
        return;
    case EPC_IMRST:
        s->epc_imask &= ~val;
        ip19_epc_update_irq(s);
        return;
    case EPC_PRSTSET:
        s->epc_prst |= val;
        return;
    case EPC_PRSTCLR:
        s->epc_prst &= ~val;
        return;
    case EPC_IERR:
    case EPC_IERRC:
        idx = (EPC_IERR - EPC_CTRL_BASE) >> 3;
        s->epc_regs[idx] = (reg == EPC_IERRC) ? 0 : val;
        return;
    case EPC_TTOP:
        ip19_epc_set_reg(s, EPC_TTOP, val);
        ip19_epc_enet_tx_pump(s);
        return;
    case EPC_TBASELO:
    case EPC_TLIMIT:
        ip19_epc_set_reg(s, reg, val);
        ip19_epc_enet_tx_pump(s);
        return;
    case EPC_RTOP:
        ip19_epc_set_reg(s, EPC_RTOP, val);
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    case EPC_RCMD:
        ip19_epc_set_reg(s, EPC_RCMD, val);
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
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

static unsigned ip19_io4_padap_type(SGI_IP19State *s, unsigned padap)
{
    uint64_t reg;

    if (padap >= 8) {
        return IO4_ADAP_NULL;
    }
    reg = s->cfg_regs[IP19_SLOT_IO4][padap < 4 ? CFG_IO4_IODEV0
                                              : CFG_IO4_IODEV1];
    return (reg >> ((padap & 3) * 8)) & 0xff;
}

static bool ip19_io4_padap_enabled(SGI_IP19State *s, unsigned padap)
{
    return padap < 8 &&
           (s->cfg_regs[IP19_SLOT_IO4][CFG_IO4_ADAP] & (1ULL << padap));
}

static void ip19_io4_bad_ioa(SGI_IP19State *s)
{
    uint64_t mask = s->cfg_regs[IP19_SLOT_IO4][CFG_IO4_INTRMASK];
    uint64_t vector = s->cfg_regs[IP19_SLOT_IO4][CFG_IO4_INTRVECTOR];

    s->io4_ebus_error |= IO4_EBUSERROR_BADIOA | IO4_EBUSERROR_STICKY;

    if ((mask & IO4_INTRMASK_ERROR) && !(mask & IO4_INTRMASK_DISABLE) &&
        vector) {
        s->io4_error_vector = vector;
        ip19_deliver_vector(s, vector);
    }
}

static MemTxResult ip19_io4_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    hwaddr abs = IP19_IO4_REGION_BASE + addr;
    unsigned region = (abs >> 19) & 0xf;
    unsigned padap = (addr >> 16) & 7;
    unsigned local = addr & 0xffff;
    unsigned type;

    *data = ~0ULL;

    if (region != IP19_EPC_REGION) {
        ip19_io4_bad_ioa(s);
        return MEMTX_OK;
    }

    type = ip19_io4_padap_type(s, padap);
    if (padap == IP19_S1_PADAP) {
        /*
         * S1 is a higher-priority subregion of the machine address
         * space.  Only holes outside its register window reach the IO4
         * decoder.
         */
        if (!ip19_io4_padap_enabled(s, padap) ||
            (type != IO4_ADAP_SCSI && type != IO4_ADAP_SCIP) ||
            local >= IP19_S1_SIZE) {
            ip19_io4_bad_ioa(s);
        }
        return MEMTX_OK;
    }
    if (padap != IP19_EPC_PADAP || type != IO4_ADAP_EPC ||
        !ip19_io4_padap_enabled(s, padap)) {
        ip19_io4_bad_ioa(s);
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
                *data = ip19_epc_rtc_read_reg(s, s->epc_rtc_addr);
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

    if (local >= EPC_ENETPROM_BASE &&
        local < EPC_ENETPROM_BASE + 6 * 16) {
        /*
         * EPC Ethernet ID PROM.  if_eeedtinit() reads 32-bit words at
         * each 16-byte slot plus 4 and takes the low byte; the PROM
         * uses the byte lane at +7.  Both are the same station address.
         */
        unsigned lane = local & 0xf;

        unsigned slot = (local - EPC_ENETPROM_BASE) >> 4;

        if ((lane == 4 || lane == 7) && slot < sizeof(s->epc_mac)) {
            /* if_ee reads slot 5 first, so the PROM stores MAC in reverse. */
            *data = s->epc_mac[sizeof(s->epc_mac) - 1 - slot];
        } else {
            *data = 0;
        }
        return MEMTX_OK;
    }

    /* EPC PROM and unimplemented PBUS devices. */
    *data = 0;
    return MEMTX_OK;
}

static MemTxResult ip19_io4_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size, MemTxAttrs attrs)
{
    SGI_IP19State *s = opaque;
    hwaddr abs = IP19_IO4_REGION_BASE + addr;
    unsigned region = (abs >> 19) & 0xf;
    unsigned padap = (addr >> 16) & 7;
    unsigned local = addr & 0xffff;
    unsigned type;

    if (region != IP19_EPC_REGION) {
        ip19_io4_bad_ioa(s);
        return MEMTX_OK;
    }

    type = ip19_io4_padap_type(s, padap);
    if (padap == IP19_S1_PADAP) {
        if (!ip19_io4_padap_enabled(s, padap) ||
            (type != IO4_ADAP_SCSI && type != IO4_ADAP_SCIP) ||
            local >= IP19_S1_SIZE) {
            ip19_io4_bad_ioa(s);
        }
        return MEMTX_OK;
    }
    if (padap != IP19_EPC_PADAP || type != IO4_ADAP_EPC ||
        !ip19_io4_padap_enabled(s, padap)) {
        ip19_io4_bad_ioa(s);
        return MEMTX_OK;
    }

    if (local >= EPC_NVRTC_BASE && local < EPC_DUART_BASE) {
        unsigned nvr_off = local - EPC_NVRTC_BASE;
        unsigned k = (nvr_off >> 3) & 0x3f;

        if (nvr_off >= 0x200 && nvr_off < 0x220) {
            if (nvr_off == 0x207) {
                s->epc_rtc_addr = val & 0xff;
            } else if (nvr_off == 0x20f) {
                ip19_epc_rtc_write_reg(s, s->epc_rtc_addr, val & 0xff);
            }
        } else if (k == 0x20) {
            s->epc_xram_page = val & 0xff;
        } else {
            s->epc_nvram[((s->epc_xram_page << 5) |
                          (k & 0x1f)) & 0xfff] = val & 0xff;
        }
        return MEMTX_OK;
    }
    if (local >= EPC_CTRL_BASE &&
        local < EPC_CTRL_BASE + sizeof(s->epc_regs)) {
        ip19_epc_ctrl_write(s, local & ~0x7ULL, val);
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
    unsigned slices = MIN(machine->smp.cpus, IP19_MAX_CPUS);

    /*
     * A_ENABLE is the slice vector for this IP19 board.  One 2-CPU
     * board has slices 0-1 enabled (0x03); a 4-CPU board uses 0x0f.
     * Do not advertise unpopulated slices as enabled.
     */
    s->cfg_regs[IP19_SLOT_CPU][CFG_BOARD_ENABLE] = (1U << slices) - 1;
    s->cfg_regs[IP19_SLOT_CPU][CFG_BOARD_TYPE] = 0x01; /* IP19 */
    s->cfg_regs[IP19_SLOT_CPU][CFG_BOARD_LEVEL] = 0x03;
    for (proc = 0; proc < slices; proc++) {
        unsigned base = CFG_PROC_BASE + proc * CFG_PROC_STRIDE;

        s->cfg_regs[IP19_SLOT_CPU][base + CFG_PROC_CCREV] = 0x1e; /* CC rev 2 */
        /*
         * EV_CACHE_SZ code 1 == 4 MiB secondary cache, matching the
         * EAROM EV_CACHE_SZ_LOC value used by the PROM and kernel.
         */
        s->cfg_regs[IP19_SLOT_CPU][base + CFG_PROC_CACHE_SZ] = 0x01;
    }

    /* IO4 on slot 2 */
    s->cfg_regs[IP19_SLOT_IO4][CFG_BOARD_ENABLE] = 0x01;
    s->cfg_regs[IP19_SLOT_IO4][CFG_BOARD_TYPE] = 0x0a; /* IO4 */
    s->cfg_regs[IP19_SLOT_IO4][CFG_BOARD_LEVEL] = 0x21;
    s->io4_lw = IP19_EPC_REGION;
    s->io4_sw = (uint64_t)IP19_EPC_REGION << 8;
    s->cfg_regs[IP19_SLOT_IO4][CFG_IO4_ADAP] =
        (1ULL << IP19_EPC_PADAP) | (1ULL << IP19_S1_PADAP);
    s->cfg_regs[IP19_SLOT_IO4][CFG_IO4_INTRMASK] = IO4_INTRMASK_ERROR;
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

extern uint64_t sgi_idle_pc;
extern int sgi_idle_on;
extern int sgi_idle_sleep;

/*
 * The stock IP19 PROM has two emulator-visible quirks: its large-window
 * helpers truncate an XKPHYS address with 32-bit add, and its cache tests
 * use physical 0 as scratch.  It also has no safe late-secondary-CPU wait.
 * Apply the known instruction fixups to the loaded RAM image, but only when
 * the original word is present.  The PROM file is never modified, and an
 * already-updated/rebuilt PROM is left alone.
 */
static void ip19_prom_compat_fixups(SGI_IP19State *s)
{
    static const struct {
        uint32_t offset;
        uint32_t old_word;
        uint32_t new_word;
    } fixups[] = {
        { 0x00cb4, 0x3c048000, 0x3c048080 },
        { 0x016a8, 0x15000029, 0x15000043 },
        { 0x016c4, 0x15000022, 0x1500003c },
        { 0x01764, 0x3c048000, 0x3c048080 },
        { 0x01be0, 0x0ff00668, 0x10000033 },
        { 0x01e88, 0x00852020, 0x0085202d },
        { 0x01e98, 0x00852020, 0x0085202d },
        { 0x01eac, 0x00852020, 0x0085202d },
        { 0x01ebc, 0x00852020, 0x0085202d },
        { 0x01ed0, 0x00852020, 0x0085202d },
        { 0x01ee0, 0x00852020, 0x0085202d },
        { 0x01ef4, 0x00852020, 0x0085202d },
        { 0x01f04, 0x00852020, 0x0085202d },
        { 0x024ac, 0x3c048000, 0x3c048080 },
        { 0x03b58, 0x3c0a8000, 0x3c098080 },
        { 0x03b90, 0x3c0a8000, 0x3c098080 },
        { 0x03d84, 0x3c0a8000, 0x3c098080 },
        { 0x03da0, 0x3c0a8000, 0x3c098080 },
        { 0x03dd8, 0x3c0a8000, 0x3c098080 },
        { 0x03df8, 0x3c0a8000, 0x3c098080 },
        { 0x03ed0, 0x3c098000, 0x3c098080 },
        { 0x03f98, 0x3c098000, 0x3c098080 },
        { 0x0e640, 0x24080078, 0x240800f3 },
        { 0x0efdc, 0x0ff02e05, 0x00000000 },
        { 0x0efec, 0x0ff02e05, 0x00000000 },
        { 0x0effc, 0x0ff02e05, 0x00000000 },
        { 0x0f000, 0x3c058000, 0x3c058080 },
        { 0x0f008, 0x0ff02e05, 0x00000000 },
        { 0x0f018, 0x0ff02e05, 0x00000000 },
        { 0x0f024, 0x0ff02e05, 0x00000000 },
    };
    uint8_t *prom = memory_region_get_ram_ptr(&s->prom);
    unsigned i;
    unsigned applied = 0;

    for (i = 0; i < ARRAY_SIZE(fixups); i++) {
        uint8_t *word = prom + fixups[i].offset;

        if (ldl_be_p(word) == fixups[i].old_word) {
            stl_be_p(word, fixups[i].new_word);
            applied++;
        }
    }
    IP19_DPRINT("PROM compatibility fixups applied: %u\n", applied);
}

static void sgi_ip19_init(MachineState *machine)
{
    SGI_IP19State *s = SGI_IP19_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Clock *cpuclk;
    int i, j;

    /*
     * IRIX 6.5 IP19 idles by polling local_idle rather than using WAIT.
     * Reuse the existing idle-spin halt hook with the IP19 address; the
     * default in target/mips remains the Indy IP24 address for other
     * machines.
     */
    sgi_idle_pc = 0xa8000000000ea174ULL;
    /*
     * The IP19 local_idle halt/sleep path can lose the CC scheduler
     * clock on both CPUs.  Leave it disabled by default; IP19_IDLE=1
     * enables the old experimental path for testing.
     */
    sgi_idle_on = g_getenv("IP19_IDLE") != NULL;
    {
        const char *idle_sleep = g_getenv("SGI_IDLE_SLEEP");

        sgi_idle_sleep = idle_sleep &&
                         !(idle_sleep[0] == '0' ||
                           idle_sleep[0] == 'n' ||
                           idle_sleep[0] == 'N');
    }

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, IP19_CPU_HZ);

    for (i = 0; i < machine->smp.cpus; i++) {
        MIPSCPU *cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk,
                                                  TARGET_BIG_ENDIAN);

        /*
         * IRIX 6.5.22 IP19 reaches the K2 refill handler before any
         * KPTE/kptbl TLB entry is wired.  Let the CPU synthesize that
         * entry for this machine only; Indy's R4000SC stays unchanged.
         */
        cpu->ip19_kpte_workaround = true;
        /*
         * IP19 uses R4000/R4400 CPUs whose Context and XContext have a
         * shared PTEBase field.  IRIX 6.5.22 writes the KPTE base only to
         * Context, while its utlbmiss handler reads XContext; model the
         * shared field for this machine only.
         */
        cpu->ip19_pte_base_shared = true;
        /*
         * Route K1 (uncached) accesses to the IO4 small window to the
         * private alias, leaving cached references to 0x10080000.. as
         * ordinary RAM on 512 MiB machines.
         */
        cpu->ip19_io4_window = true;
        cpu->env.CP0_PRid = IP19_CPU_PRID;
        CPU(cpu)->cpu_index = i;
        /*
         * Hold the secondary slices out of reset until the bootmaster
         * claims the master group.  start_powered_off makes the generic
         * CPU reset keep them halted on machine reset as well.
         */
        CPU(cpu)->start_powered_off = ip19_hold_secondaries() && (i != 0);
        CPU(cpu)->halted = ip19_hold_secondaries() && (i != 0);
        s->cpu_held[i] = ip19_hold_secondaries() && (i != 0);
        IP19_DPRINT("init cpu %d index %u held %d\n", i,
                    CPU(cpu)->cpu_index, s->cpu_held[i]);
        cpu->env.CP0_Config0 &= ~(1U << 17);
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        for (j = 0; j < 8; j++) {
            s->cpu_irq[i][j] = cpu->env.irq[j];
        }
    }

    for (i = 0; i < 3; i++) {
        qemu_irq irq;

        s->epc_duart[i] = sgi_z8530_create(i == 2 ? serial_hd(0) : NULL,
                                           NULL);
        irq = qemu_allocate_irq(ip19_epc_duart_irq, s, i);
        qdev_connect_gpio_out_named(DEVICE(s->epc_duart[i]), "irq", 0, irq);
    }
    /* CC UART is a sysctlr command channel, not the user console. */
    s->cc_uart_chr = NULL;
    s->epc_prst = 0;
    s->cc_uart_rx = 0;
    s->cc_uart_rx_ready = false;
    s->rtc_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    /*
     * MIPS64 is still single-threaded TCG, so the master and AP share
     * one host thread.  Without an explicit yield in the bootmaster's
     * us_delay() loop the AP can be too slow to set pdaindr[CpuId]
     * before the kernel's boot timeout expires.  Enable the yield by
     * default for SMP; IP19_RTC_YIELD=0 provides an escape hatch for
     * bisection.
     */
    {
        const char *yield_env = g_getenv("IP19_RTC_YIELD");

        s->rtc_yield = machine->smp.cpus > 1 &&
                       (!yield_env || atoi(yield_env) != 0);
    }
    s->rtc_yield_armed = false;
    s->rtc_yield_until_ns = 0;
    s->epc_rtc_base_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    s->epc_rtc_base_sec = (int64_t)time(NULL);
    memset(s->epc_rtc_regs, 0, sizeof(s->epc_rtc_regs));
    s->epc_rtc_regs[NVR_RTCA] = NVR_OSC_ON;
    s->epc_rtc_regs[NVR_RTCB] = NVR_DM | NVR_MILTIME;
    s->epc_rtc_regs[NVR_RTCD] = NVR_VRT;
    g_strlcpy(s->sysctlr_serial, "S1234567", sizeof(s->sysctlr_serial));
    ip19_epc_nvram_defaults(s);
    /* SGI OUI; replaced by an explicit -nic mac= if one is supplied. */
    s->epc_mac[0] = 0x08;
    s->epc_mac[1] = 0x00;
    s->epc_mac[2] = 0x69;
    s->epc_mac[3] = 0x19;
    s->epc_mac[4] = 0x00;
    s->epc_mac[5] = 0x01;

    for (i = 0; i < IP19_MAX_CPUS; i++) {
        s->cmp_ctx[i].s = s;
        s->cmp_ctx[i].cpu = i;
        s->cmp_timer[i] = timer_new_ns(QEMU_CLOCK_HOST, ip19_cmp_expire,
                                       &s->cmp_ctx[i]);
    }

    s->mc3 = sgi_mc3_create(machine->ram, machine->ram_size,
                            SGI_MC3_INTLV_STABLE);
    /*
     * MC3 memory spans the low physical window, but the CC, IO4, GIO,
     * EAROM and PROM devices also decode inside it.  Keep the memory
     * region below those device windows so their subregions win no
     * matter which order the machine's regions are added in.
     */
    memory_region_add_subregion_overlap(sysmem, 0,
                                        sgi_mc3_memory_region(s->mc3), -1);

    /*
     * Keep the PROM in writable RAM: the kernel treats all low physical
     * memory as usable, including the PROM window, and DMA/cached
     * references must reach the same bytes as code fetch and K1 accesses.
     */
    memory_region_init_ram(&s->prom, NULL, "ip19.prom", IP19_PROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, IP19_PROM_BASE, &s->prom);
    {
        const char *firmware = machine->firmware ?: "ip19prom.bin";
        g_autofree char *filename =
            qemu_find_file(QEMU_FILE_TYPE_BIOS, firmware);

        if (filename) {
            g_autofree gchar *prom_buf = NULL;
            gsize prom_len = 0;

            if (g_file_get_contents(filename, &prom_buf, &prom_len, NULL)) {
                memcpy(memory_region_get_ram_ptr(&s->prom), prom_buf,
                       MIN(prom_len, IP19_PROM_SIZE));
            } else {
                error_report("ip19: cannot read PROM image '%s'", filename);
            }
            ip19_prom_compat_fixups(s);
        } else {
            error_report("ip19: no PROM image '%s'", firmware);
        }
    }

    memory_region_init_io(&s->lwin, NULL, &ip19_lwin_ops, s, "ip19.lwin",
                          IP19_LWIN_SIZE);
    memory_region_add_subregion(sysmem, IP19_LWIN_BASE, &s->lwin);

    s->flash_data = g_malloc(IP19_FLASH_SIZE);
    memset(s->flash_data, 0xff, IP19_FLASH_SIZE);
    memory_region_init_io(&s->flash, NULL, &ip19_flash_ops, s, "ip19.flash",
                          IP19_FLASH_SIZE);
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
            g_autofree gchar *buf = NULL;
            gsize len = 0;

            if (g_file_get_contents(filename, &buf, &len, NULL)) {
                memcpy(s->flash_data, buf, MIN(len, IP19_FLASH_SIZE));
            }
        }
    }

    memory_region_init_io(&s->cc, NULL, &ip19_cc_ops, s, "ip19.cc",
                          IP19_CC_SIZE);
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_CC_BASE), &s->cc);

    ip19_cfg_defaults(s, machine);
    memory_region_init_io(&s->cfg, NULL, &ip19_cfg_ops, s, "ip19.cfg",
                          IP19_CFG_SIZE);
    memory_region_add_subregion(&s->cfg, IP19_SLOT_MC3 << 11,
                                sgi_mc3_config_region(s->mc3));
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_CFG_BASE), &s->cfg);

    memory_region_init_ram(&s->bustag, NULL, "ip19.bustag",
                           IP19_BUSTAG_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_BUSTAG_BASE), &s->bustag);

    memory_region_init_io(&s->wginput, NULL, &ip19_ram_ops, s, "ip19.wginput",
                          IP19_WGINPUT_SIZE);
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_WGINPUT_BASE), &s->wginput);

    memory_region_init_io(&s->rtc, NULL, &ip19_rtc_ops, s, "ip19.rtc",
                          IP19_RTC_SIZE);
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_RTC_BASE), &s->rtc);

    ip19_earom_defaults(s);
    memory_region_init_io(&s->earom, NULL, &ip19_earom_ops, s, "ip19.earom",
                          IP19_EAROM_SIZE);
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_EAROM_BASE), &s->earom);

    memory_region_init_io(&s->led, NULL, &ip19_ram_ops, s, "ip19.led",
                          IP19_LED_SIZE);
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_LED_BASE), &s->led);

    memory_region_init_io(&s->sync, NULL, &ip19_sync_ops, s, "ip19.sync",
                          IP19_SYNC_SIZE);
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_SYNC_BASE), &s->sync);

    memory_region_init_io(&s->io4, NULL, &ip19_io4_ops, s, "ip19.io4",
                          IP19_IO4_SIZE);
    for (i = 0; i < 3; i++) {
        memory_region_add_subregion(&s->io4,
            (IP19_EPC_PADAP << 16) + EPC_DUART_BASE +
            i * EPC_DUART_STRIDE,
            sgi_z8530_memory_region(s->epc_duart[i]));
    }
    memory_region_add_subregion(sysmem, IP19_IO4_REGION_BASE, &s->io4);

    s->s1 = sgi_s1_attach(sysmem, IP19_S1_BASE, blk_by_name("ip19disk"),
                          blk_by_name("ip19cdrom"), blk_by_name("ip19cdrom2"));
    {
        qemu_irq irq = qemu_allocate_irq(ip19_s1_irq, s, 0);

        qdev_connect_gpio_out_named(DEVICE(s->s1), "sysbus-irq", 0, irq);
    }


    memory_region_init_io(&s->gio, NULL, &ip19_gio_ops, s, "ip19.gio",
                          IP19_GIO_SIZE);
    memory_region_add_subregion(sysmem, IP19_LOCAL_ALIAS(IP19_GIO_BASE), &s->gio);

    /*
     * Attach a host net backend when the user supplied -nic/-net nic.
     * With no backend the EPC Ethernet registers still behave normally,
     * but packets are dropped, preserving the offline default.
     */
    {
        NICInfo *nd = qemu_find_nic_info("epc-enet", true, NULL);

        if (nd) {
            static const uint8_t zero[6] = { 0 };
            static const uint8_t qemu_default[5] =
                { 0x52, 0x54, 0x00, 0x12, 0x34 };

            if (!memcmp(nd->macaddr.a, zero, sizeof(zero)) ||
                !memcmp(nd->macaddr.a, qemu_default,
                        sizeof(qemu_default))) {
                memcpy(nd->macaddr.a, s->epc_mac, sizeof(s->epc_mac));
            } else {
                memcpy(s->epc_mac, nd->macaddr.a, sizeof(s->epc_mac));
            }
            s->epc_nic_conf.macaddr = nd->macaddr;
            s->epc_nic_conf.peers.ncs[0] = nd->netdev;
            s->epc_nic_conf.peers.queues = nd->netdev ? 1 : 0;
            s->nic = qemu_new_nic(&ip19_epc_net_info, &s->epc_nic_conf,
                                  "epc-enet", "ip19",
                                  &s->epc_reentrancy_guard, s);
            nd->instantiated = 1;
            qemu_format_nic_info_str(qemu_get_queue(s->nic),
                                     s->epc_nic_conf.macaddr.a);
        }
    }
}

static void sgi_ip19_reset(MachineState *machine, ResetType type)
{
    SGI_IP19State *s = SGI_IP19_MACHINE(machine);
    CPUState *cs;
    int i;

    if (ip19_debug_enabled()) {
        uint8_t buf[8];

        CPU_FOREACH(cs) {
            MIPSCPU *mc = MIPS_CPU(cs);

            IP19_DPRINT("reset-in cpu %u pc %016" PRIx64
                        " sr %08x cause %08x epc %016" PRIx64 "\n",
                        cs->cpu_index, (uint64_t)mc->env.active_tc.PC,
                        mc->env.CP0_Status, mc->env.CP0_Cause,
                        (uint64_t)mc->env.CP0_EPC);
        }
        if (address_space_read(&address_space_memory, 0x3f1dcc,
                               MEMTXATTRS_UNSPECIFIED, buf, 4) == MEMTX_OK) {
            IP19_DPRINT("reset-in cb_wait %d\n", (int32_t)ldl_be_p(buf));
        }
        if (address_space_read(&address_space_memory, 0x3fa540,
                               MEMTXATTRS_UNSPECIFIED, buf, 4) == MEMTX_OK) {
            IP19_DPRINT("reset-in bootlck %08x\n", ldl_be_p(buf));
        }
        if (address_space_read(&address_space_memory, 0x3fde50,
                               MEMTXATTRS_UNSPECIFIED, buf, 4) == MEMTX_OK) {
            IP19_DPRINT("reset-in pdaindr1.CpuId %d\n",
                        (int32_t)ldl_be_p(buf));
        }
    }

    qemu_devices_reset(type);

    /*
     * The CC/IP state is machine state, not device state.  Clear it on
     * reset so a reboot starts from a clean interrupt configuration and
     * so the bootmaster has to claim the master group again.
     */
    memset(s->ip, 0, sizeof(s->ip));
    memset(s->grp_pending, 0, sizeof(s->grp_pending));
    memset(s->grp_delivered, 0, sizeof(s->grp_delivered));
    memset(s->ile, 0, sizeof(s->ile));
    memset(s->igrmask, 0, sizeof(s->igrmask));
    memset(s->cel, 0, sizeof(s->cel));
    memset(s->epc_regs, 0, sizeof(s->epc_regs));
    s->epc_imask = 0;
    s->epc_ipend = 0;
    s->epc_prst = 0;
    s->bootmaster_claimed = false;
    s->rtc_yield_armed = false;
    s->rtc_yield_until_ns = 0;
    /*
     * Rebase the PROM's 2-second arbitration barrier at reset.  QEMU
     * sets the initial epoch in sgi_ip19_init, but machine construction
     * can take a while before the vCPUs start.  Starting the epoch here
     * guarantees CPU 0 reaches the barrier before it expires; the held
     * secondary slices are released after the barrier and take the
     * late-start slave path.
     */
    s->rtc_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);

    for (i = 0; i < IP19_MAX_CPUS; i++) {
        if (s->cmp_timer[i]) {
            timer_del(s->cmp_timer[i]);
        }
        s->cmpint[i] = false;
        s->cmpreg[i] = 0;
        s->cpu_held[i] = ip19_hold_secondaries() &&
                         i != 0 && i < machine->smp.cpus;
    }

    /*
     * mips_cpu_reset_hold() restores CP0 PRId from the CPU model.  Keep
     * the IP19 revision visible after every machine reset as well.
     */
    CPU_FOREACH(cs) {
        unsigned cpu = cs->cpu_index;

        MIPS_CPU(cs)->env.CP0_PRid = IP19_CPU_PRID;
        if (cpu != 0 && cpu < IP19_MAX_CPUS && s->cpu_held[cpu]) {
            cs->halted = 1;
        }
        IP19_DPRINT("reset cpu %u held %d halted %d\n", cpu,
                    cpu < IP19_MAX_CPUS ? s->cpu_held[cpu] : 0,
                    cs->halted);
    }
}

static void sgi_ip19_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "SGI IP19 (Everest Challenge/Onyx)";
    mc->init = sgi_ip19_init;
    mc->reset = sgi_ip19_reset;
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

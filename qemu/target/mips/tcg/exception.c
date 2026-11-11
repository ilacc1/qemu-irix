/*
 *  MIPS Exceptions processing helpers for QEMU.
 *
 *  Copyright (c) 2004-2005 Jocelyn Mayer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "internal.h"
#include "exec/helper-proto.h"
#include "exec/translation-block.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/tb-flush.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "qemu/timer.h"

target_ulong exception_resume_pc(CPUMIPSState *env)
{
    target_ulong bad_pc;
    target_ulong isa_mode;

    isa_mode = !!(env->hflags & MIPS_HFLAG_M16);
    bad_pc = env->active_tc.PC | isa_mode;
    if (env->hflags & MIPS_HFLAG_BMASK) {
        /*
         * If the exception was raised from a delay slot, come back to
         * the jump.
         */
        bad_pc -= (env->hflags & MIPS_HFLAG_B16 ? 2 : 4);
    }

    return bad_pc;
}

void helper_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                                int error_code)
{
    do_raise_exception_err(env, exception, error_code, 0);
}

void helper_raise_exception(CPUMIPSState *env, uint32_t exception)
{
    do_raise_exception(env, exception, GETPC());
}

void helper_raise_exception_debug(CPUMIPSState *env)
{
    do_raise_exception(env, EXCP_DEBUG, 0);
}

static void raise_exception(CPUMIPSState *env, uint32_t exception)
{
    do_raise_exception(env, exception, 0);
}

void helper_wait(CPUMIPSState *env)
{
    CPUState *cs = env_cpu(env);

    cs->halted = 1;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_WAKE);
    /*
     * Last instruction in the block, PC was updated before
     * - no need to recover PC and icount.
     */
    raise_exception(env, EXCP_HLT);
}

/*
 * Idle-spin halt (local, never upstream).  IRIX idles by polling kernel
 * state without WAIT, pinning a host core while the guest reports idle.
 * The translator emits a call to helper_sgi_idle_check() for the
 * instruction at sgi_idle_pc; after sustained consecutive hits we halt
 * exactly like WAIT (a timer tick always wakes us).  No guest changes.
 *
 * sgi_idle_pc is not a per-kernel constant.  It is 0 (not known) until
 * sgi_idle_detect_enable() learns it at run time, see below.
 * SGI_IDLE_PC=<addr> forces it (and disables detection).
 * SGI_IDLE_SPIN=0 disables the halt; SGI_IDLE_THRESH tunes it.
 * SGI_IDLE_SLEEP=1 uses host sleeps instead of the halt.
 */
uint64_t sgi_idle_pc;
uint32_t sgi_idle_thresh = 100;
int sgi_idle_on = 1;
int sgi_idle_sleep;

/*
 * These are read by every vCPU (the translator, and the idle helper on
 * the vCPU's own thread) and written by the detector or by the startup
 * constructor, so all accesses go through the atomic helpers below.
 */
uint64_t sgi_idle_get_pc(void)
{
    return qatomic_read(&sgi_idle_pc);
}

uint32_t sgi_idle_get_thresh(void)
{
    return qatomic_read(&sgi_idle_thresh);
}

int sgi_idle_get_on(void)
{
    return qatomic_read(&sgi_idle_on);
}

int sgi_idle_get_sleep(void)
{
    return qatomic_read(&sgi_idle_sleep);
}
/*
 * Idle-loop detection, independent of the guest kernel.
 *
 * While sgi_idle_pc is 0, every external interrupt taken by a vCPU on a
 * machine that enabled detection records the PC it interrupted.  An idle
 * kernel spends most of its time polling one loop whose back edge is a
 * conditional branch, so the hottest sampled backward branch lies on the
 * idle path; its target is the loop head where the halt is placed (see
 * sgi_idle_pick_branch).  Samples are grouped in 10 s epochs, and a head
 * is adopted once the same target recurs for six consecutive epochs.
 * Adoption flushes the TB cache so the translator hooks the PC.
 */
#define SGI_IDLE_SLOTS       1024
#define SGI_IDLE_HIST        6
#define SGI_IDLE_MIN_HITS    20      /* branch samples needed in every epoch */
#define SGI_IDLE_EPOCH_NS    (10 * NANOSECONDS_PER_SECOND)
#define SGI_IDLE_REGULAR_NS  (60 * NANOSECONDS_PER_SECOND)

typedef struct {
    uint64_t pc;
    uint64_t hits;
} SgiIdleSlot;

typedef struct {
    SgiIdleSlot slot[SGI_IDLE_SLOTS];
    uint64_t samples;     /* all samples, including ones with no free slot */
} SgiIdleTable;

typedef struct {
    uint64_t pc;          /* loop head (backward-branch target), 0 if none */
    uint64_t hits;        /* samples that interrupted at the branch */
    uint64_t samples;
} SgiIdleEpoch;

static QemuMutex sgi_idle_lock;
static int sgi_idle_detecting;     /* set by sgi_idle_detect_enable() */
static bool sgi_idle_started;
static int64_t sgi_idle_t0;        /* detection start (host clock, ns) */
static int64_t sgi_idle_epoch_t0;  /* current epoch start */
static SgiIdleTable sgi_idle_cur;
static SgiIdleEpoch sgi_idle_hist[SGI_IDLE_HIST];
static unsigned sgi_idle_nepoch;   /* completed epochs */

static void sgi_idle_table_add(SgiIdleTable *t, uint64_t pc)
{
    unsigned i = (unsigned)((((pc >> 2) * 0x9E3779B97F4A7C15ULL)) >> 54);

    t->samples++;
    for (unsigned n = 0; n < SGI_IDLE_SLOTS; n++) {
        SgiIdleSlot *s = &t->slot[(i + n) % SGI_IDLE_SLOTS];

        if (s->hits == 0) {
            s->pc = pc;
            s->hits = 1;
            return;
        }
        if (s->pc == pc) {
            s->hits++;
            return;
        }
    }
}

/*
 * Is @w a conditional or likely branch?  Sets @off to its 16-bit offset.
 * Covers BEQ/BNE/BLEZ/BGTZ and their likely forms, REGIMM conditional
 * branches (including the "and link" forms) and BC1.  Jumps are not
 * included: they never close a loop by themselves.
 */
static bool sgi_idle_is_branch(uint32_t w, int64_t *off)
{
    uint32_t op = w >> 26;
    uint32_t rt = (w >> 16) & 0x1f;

    switch (op) {
    case 4: case 5: case 6: case 7:          /* BEQ BNE BLEZ BGTZ */
    case 20: case 21: case 22: case 23:      /* likely forms */
        break;
    case 1:                                  /* REGIMM */
        if (rt > 3 && (rt < 16 || rt > 19)) {
            return false;
        }
        break;
    case 17:                                 /* COP1 */
        if (((w >> 21) & 0x1f) != 8) {       /* BC1 */
            return false;
        }
        break;
    default:
        return false;
    }
    *off = (int16_t)(w & 0xffff);
    return true;
}

/*
 * Pick the hottest sampled backward branch.  An idle kernel polls in a
 * loop whose back edge is a conditional branch, so the branch that most
 * often interrupts the CPU is on the idle path.  Its target is the loop
 * head: the instruction every iteration passes through, where the halt
 * belongs.  Reading guest memory through the debug accessor cannot raise
 * a guest exception from the interrupt path.
 */
static bool sgi_idle_pick_branch(CPUState *cs, uint64_t *head,
                                 uint64_t *branch_hits)
{
    uint64_t best_hits = 0;

    for (unsigned i = 0; i < SGI_IDLE_SLOTS; i++) {
        const SgiIdleSlot *s = &sgi_idle_cur.slot[i];
        uint8_t code[4];
        int64_t off;
        uint64_t target;

        if (!s->hits || s->hits <= best_hits) {
            continue;
        }
        if (cpu_memory_rw_debug(cs, s->pc, code, sizeof(code), false) != 0) {
            continue;
        }
        if (!sgi_idle_is_branch(ldl_be_p(code), &off)) {
            continue;
        }
        target = s->pc + 4 + (off << 2);
        if (target >= s->pc || s->pc - target > 0x10000) {
            continue;
        }
        best_hits = s->hits;
        *head = target;
        *branch_hits = s->hits;
    }
    return best_hits > 0;
}

/*
 * Close the current epoch.  If a hot backward branch was sampled, its
 * target becomes the candidate loop head.  Samples lost to table overflow
 * stay in the denominator, which keeps the decision conservative.
 */
static void sgi_idle_close_epoch(CPUState *cs)
{
    SgiIdleEpoch *e = &sgi_idle_hist[sgi_idle_nepoch % SGI_IDLE_HIST];
    uint64_t head = 0;
    uint64_t bhits = 0;

    e->samples = sgi_idle_cur.samples;
    e->pc = 0;
    e->hits = 0;
    if (sgi_idle_pick_branch(cs, &head, &bhits)) {
        e->pc = head;
        e->hits = bhits;
    }
    sgi_idle_nepoch++;
    memset(&sgi_idle_cur, 0, sizeof(sgi_idle_cur));
    sgi_idle_epoch_t0 += SGI_IDLE_EPOCH_NS;
}

static bool sgi_idle_decide(int64_t elapsed, uint64_t *pc)
{
    const SgiIdleEpoch *last;
    uint64_t total = 0;

    if (sgi_idle_nepoch < SGI_IDLE_HIST || elapsed < SGI_IDLE_REGULAR_NS) {
        return false;
    }
    last = &sgi_idle_hist[(sgi_idle_nepoch - 1) % SGI_IDLE_HIST];
    if (!last->pc) {
        return false;
    }
    for (unsigned i = 0; i < SGI_IDLE_HIST; i++) {
        const SgiIdleEpoch *e = &sgi_idle_hist[i];

        if (e->pc != last->pc || e->hits < SGI_IDLE_MIN_HITS) {
            return false;
        }
        total += e->samples;
    }
    if (total < 4000) {
        return false;
    }
    *pc = last->pc;
    return true;
}

/* Called on the vCPU thread when an external interrupt is taken. */
void sgi_idle_sample(CPUMIPSState *env)
{
    uint64_t pc = 0;
    bool closed = false;
    bool decided = false;
    int64_t now, elapsed;

    if (!qatomic_read(&sgi_idle_detecting)) {
        return;
    }

    qemu_mutex_lock(&sgi_idle_lock);
    if (!sgi_idle_detecting) {
        qemu_mutex_unlock(&sgi_idle_lock);
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    elapsed = now - sgi_idle_t0;
    while (now - sgi_idle_epoch_t0 >= SGI_IDLE_EPOCH_NS) {
        sgi_idle_close_epoch(env_cpu(env));
        closed = true;
    }
    if (closed && sgi_idle_decide(elapsed, &pc)) {
        qatomic_set(&sgi_idle_pc, pc);
        qatomic_set(&sgi_idle_detecting, 0);
        decided = true;
    }
    if (!decided) {
        sgi_idle_table_add(&sgi_idle_cur, exception_resume_pc(env));
    }
    qemu_mutex_unlock(&sgi_idle_lock);

    if (decided) {
        info_report("sgi idle: learned idle-loop PC 0x%" PRIx64
                    " after %" PRId64 " ms", pc, elapsed / 1000000);
        /* Translations made before this PC was known lack the hook. */
        queue_tb_flush(env_cpu(env));
    }
}

/*
 * Start learning the idle PC.  Called by machines that run an IRIX-like
 * polling kernel.  Does nothing if the PC is already known (SGI_IDLE_PC)
 * or the halt is disabled (SGI_IDLE_SPIN=0).  Detection starts once per
 * process; it is not repeated on guest reset.
 */
void sgi_idle_detect_enable(void)
{
    if (!sgi_idle_get_on() || sgi_idle_get_pc()) {
        return;
    }
    qemu_mutex_lock(&sgi_idle_lock);
    if (!sgi_idle_started) {
        sgi_idle_started = true;
        sgi_idle_t0 = qemu_clock_get_ns(QEMU_CLOCK_HOST);
        sgi_idle_epoch_t0 = sgi_idle_t0;
        qatomic_set(&sgi_idle_detecting, 1);
    }
    qemu_mutex_unlock(&sgi_idle_lock);
}

/*
 * Discard all idle-loop monitoring state and start learning again.  Called
 * on a system reset so a stale learned idle PC or partial sample history
 * from the previous boot cannot make a fresh kernel halt or sleep.
 */
void sgi_idle_reset(void)
{
    qemu_mutex_lock(&sgi_idle_lock);
    qatomic_set(&sgi_idle_pc, 0);
    qatomic_set(&sgi_idle_detecting, 0);
    sgi_idle_started = false;
    sgi_idle_t0 = 0;
    sgi_idle_epoch_t0 = 0;
    memset(&sgi_idle_cur, 0, sizeof(sgi_idle_cur));
    memset(sgi_idle_hist, 0, sizeof(sgi_idle_hist));
    sgi_idle_nepoch = 0;
    qemu_mutex_unlock(&sgi_idle_lock);

    sgi_idle_detect_enable();
}

static void __attribute__((constructor)) sgi_idle_config(void)
{
    const char *e;

    qemu_mutex_init(&sgi_idle_lock);
    e = getenv("SGI_IDLE_PC");
    if (e && e[0]) {
        qatomic_set(&sgi_idle_pc, strtoull(e, NULL, 0));
    }
    e = getenv("SGI_IDLE_THRESH");
    if (e && e[0]) {
        qatomic_set(&sgi_idle_thresh, (uint32_t)strtoul(e, NULL, 0));
    }
    e = getenv("SGI_IDLE_SPIN");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N')) {
        qatomic_set(&sgi_idle_on, 0);
    }
    e = getenv("SGI_IDLE_SLEEP");
    if (e && e[0]) {
        qatomic_set(&sgi_idle_sleep,
                    !(e[0] == '0' || e[0] == 'n' || e[0] == 'N'));
    }
    if (!sgi_idle_get_thresh()) {
        qatomic_set(&sgi_idle_on, 0);
    }
}
/* PROM menu poll loop: spins with interrupts masked, so halting is unsafe
 * (nothing guarantees a wakeup). Yield the host instead; guest-visible:
 * nothing but slower spinning. SGI_PROM_PC=0 or SGI_PROM_SPIN=0 disables. */
uint64_t sgi_prom_pc;
uint32_t sgi_prom_thresh = 1000;
int sgi_prom_on = 1;

uint64_t sgi_prom_get_pc(void)
{
    return qatomic_read(&sgi_prom_pc);
}

uint32_t sgi_prom_get_thresh(void)
{
    return qatomic_read(&sgi_prom_thresh);
}

int sgi_prom_get_on(void)
{
    return qatomic_read(&sgi_prom_on);
}

/* Host-side tuning only (no guest-visible effect): parsed once at startup
 * so SGI_PROM_PC retargets work even before the default PC ever executes. */
static void __attribute__((constructor)) sgi_prom_config(void)
{
    const char *e;

    e = getenv("SGI_PROM_PC");
    if (e && e[0]) {
        qatomic_set(&sgi_prom_pc, strtoull(e, NULL, 0));
    }
    e = getenv("SGI_PROM_THRESH");
    if (e && e[0]) {
        qatomic_set(&sgi_prom_thresh, (uint32_t)strtoul(e, NULL, 0));
    }
    e = getenv("SGI_PROM_SPIN");
    if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N')) {
        qatomic_set(&sgi_prom_on, 0);
    }
    if (!sgi_prom_get_pc() || !sgi_prom_get_thresh()) {
        qatomic_set(&sgi_prom_on, 0);
    }
}

void helper_sgi_prom_check(CPUMIPSState *env)
{
    struct timespec ts = { 0, 500000 };

    if (!sgi_prom_get_on()) {
        return;
    }
    if (++env->sgi_prom_count < sgi_prom_get_thresh()) {
        return;
    }
    env->sgi_prom_count = 0;
    nanosleep(&ts, NULL);
}

void helper_sgi_idle_check(CPUMIPSState *env)
{
    uint32_t status;

    if (!sgi_idle_get_on()) {
        return;
    }
    if (++env->sgi_idle_count < sgi_idle_get_thresh()) {
        return;
    }
    if (sgi_idle_get_sleep()) {
        int64_t deadline = g_get_monotonic_time() + 5000;

        env->sgi_idle_count = 0;
        while (g_get_monotonic_time() < deadline) {
            int64_t left = deadline - g_get_monotonic_time();
            struct timespec idle_ts = {
                left / 1000000,
                (left % 1000000) * 1000,
            };

            if (!nanosleep(&idle_ts, NULL) || errno != EINTR) {
                break;
            }
        }
        return;
    }
    /*
     * Only sleep when a timer tick is guaranteed to wake us: interrupts
     * enabled, not in an exception, timer unmasked. Otherwise keep
     * spinning (safe fallback, never hangs).
     */
    status = (uint32_t)env->CP0_Status;
    if ((status & 1) && !(status & 6) && (status & 0x8000)) {
        CPUState *cs = env_cpu(env);

        env->sgi_idle_count = 0;
        cs->halted = 1;
        cpu_reset_interrupt(cs, CPU_INTERRUPT_WAKE);
        raise_exception(env, EXCP_HLT);
    }
}

void mips_cpu_synchronize_from_tb(CPUState *cs, const TranslationBlock *tb)
{
    CPUMIPSState *env = cpu_env(cs);

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    env->active_tc.PC = tb->pc;
    env->hflags &= ~MIPS_HFLAG_BMASK;
    env->hflags |= tb->flags & MIPS_HFLAG_BMASK;
}

static const char * const excp_names[EXCP_LAST + 1] = {
    [EXCP_RESET] = "reset",
    [EXCP_SRESET] = "soft reset",
    [EXCP_DSS] = "debug single step",
    [EXCP_DINT] = "debug interrupt",
    [EXCP_NMI] = "non-maskable interrupt",
    [EXCP_MCHECK] = "machine check",
    [EXCP_EXT_INTERRUPT] = "interrupt",
    [EXCP_DFWATCH] = "deferred watchpoint",
    [EXCP_DIB] = "debug instruction breakpoint",
    [EXCP_IWATCH] = "instruction fetch watchpoint",
    [EXCP_AdEL] = "address error load",
    [EXCP_AdES] = "address error store",
    [EXCP_TLBF] = "TLB refill",
    [EXCP_IBE] = "instruction bus error",
    [EXCP_DBp] = "debug breakpoint",
    [EXCP_SYSCALL] = "syscall",
    [EXCP_BREAK] = "break",
    [EXCP_CpU] = "coprocessor unusable",
    [EXCP_RI] = "reserved instruction",
    [EXCP_OVERFLOW] = "arithmetic overflow",
    [EXCP_TRAP] = "trap",
    [EXCP_FPE] = "floating point",
    [EXCP_DDBS] = "debug data break store",
    [EXCP_DWATCH] = "data watchpoint",
    [EXCP_LTLBL] = "TLB modify",
    [EXCP_TLBL] = "TLB load",
    [EXCP_TLBS] = "TLB store",
    [EXCP_DBE] = "data bus error",
    [EXCP_DDBL] = "debug data break load",
    [EXCP_THREAD] = "thread",
    [EXCP_MDMX] = "MDMX",
    [EXCP_C2E] = "precise coprocessor 2",
    [EXCP_CACHE] = "cache error",
    [EXCP_TLBXI] = "TLB execute-inhibit",
    [EXCP_TLBRI] = "TLB read-inhibit",
    [EXCP_MSADIS] = "MSA disabled",
    [EXCP_MSAFPE] = "MSA floating point",
    [EXCP_SEMIHOST] = "Semihosting",
};

const char *mips_exception_name(int32_t exception)
{
    if (exception < 0 || exception > EXCP_LAST) {
        return "unknown";
    }
    return excp_names[exception];
}

void do_raise_exception_err(CPUMIPSState *env, uint32_t exception,
                            int error_code, uintptr_t pc)
{
    CPUState *cs = env_cpu(env);

    qemu_log_mask(CPU_LOG_INT, "%s: %d (%s) %d\n",
                  __func__, exception, mips_exception_name(exception),
                  error_code);
    cs->exception_index = exception;
    env->error_code = error_code;

    cpu_loop_exit_restore(cs, pc);
}

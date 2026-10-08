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

/* SGI IP24 idle-spin quirk (local, never upstream): IRIX idles by
 * spinning on a kernel poll loop without WAIT, pinning a host core at
 * 100% while the guest reports 95% idle. The translator emits a call
 * here whenever it translates the known idle-loop PC; after sustained
 * consecutive hits we halt exactly like WAIT (a timer tick always wakes
 * us). Genuine lock waits are short and never reach the threshold, so
 * they stay fast. No guest modification; sleeps are always safe.
 * SGI_IDLE_PC=0 or SGI_IDLE_SPIN=0 disables; SGI_IDLE_THRESH tunes. */
uint64_t sgi_idle_pc = 0xffffffff8815dd24ULL;
uint32_t sgi_idle_thresh = 100;
int sgi_idle_on = 1;

void helper_sgi_idle_check(CPUMIPSState *env)
{
    static int cfg_done = 0;
    static uint32_t idle_count = 0;
    uint32_t status;

    if (!cfg_done) {
        const char *e;

        cfg_done = 1;
        e = getenv("SGI_IDLE_PC");
        if (e && e[0]) {
            sgi_idle_pc = strtoull(e, NULL, 0);
        }
        e = getenv("SGI_IDLE_THRESH");
        if (e && e[0]) {
            sgi_idle_thresh = (uint32_t)strtoul(e, NULL, 0);
        }
        e = getenv("SGI_IDLE_SPIN");
        if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N')) {
            sgi_idle_on = 0;
        }
        if (!sgi_idle_pc || !sgi_idle_thresh) {
            sgi_idle_on = 0;
        }
    }
    if (!sgi_idle_on) {
        return;
    }
    if (++idle_count < sgi_idle_thresh) {
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

        idle_count = 0;
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

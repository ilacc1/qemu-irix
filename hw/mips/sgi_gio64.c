#include "qemu/osdep.h"
#include <stdlib.h>
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/thread.h"
#include "ui/console.h"

#define TYPE_SGI_GIO64 "sgi-gio64"
OBJECT_DECLARE_SIMPLE_TYPE(SGIGIO64State, SGI_GIO64)

#define GIO64_SLOT_SIZE 0x400000
#define GIO64_BUS_SIZE 0xa00000
#define XL24_REX3_OFFSET 0x0f0000
#define XL24_REX3_SIZE 0x2000
#define XL24_WIDTH 1288
#define XL24_HEIGHT 1024
#define XL24_STRIDE 1344
/* RB2 pixel store holds one 32-bit RGBCI word per pixel (MAME rb2_device:
 * 8-bit CI occupies the low byte, 12-bit the low 12 bits, 24-bit direct
 * the low 24 bits as 0x00BBGGRR). 8MB covers 1344*1024 words. */
#define XL24_VRAM_SIZE (8 * 1024 * 1024)
#define XL24_GLOBAL_MASK 0x00ffffff
#define REX3_STATUS_VRINT (1U << 5)
#define REX3_STATUS_VIDEOINT (1U << 6)
#define REX3_STATUS_BACKBUSY (1U << 4)

/* Board tracing (off by default; set SGI_TRACE=1 to re-enable). Capped
 * per offset like the PIO/MC traces: the kernel probes XL24 registers
 * during console init, and a tight status poll would otherwise flood.
 * Framebuffer pixels move via the REX3 engine, not MMIO, so MMIO volume
 * stays small; the cap is a backstop. */
#define SGI_GIO_TRACE_PER_OFFSET 2000
static unsigned sgi_gio_trace_count[4096];

/* Diagnostic probes (scanout mode decode + REX3 parameter dumps) are
 * compiled out by default; see the note on sgi_gio_trace_enabled().
 * Rebuild with -DSGI_DEBUG and run with SGI_GIO_DBG=1 to enable. */
static bool sgi_gio_dbg(void)
{
#ifdef SGI_DEBUG
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("SGI_GIO_DBG") ? 1 : 0;
    }
    return cached != 0;
#else
    return false;
#endif
}

/* TEMP primitive logger (env SGI_GIO_PRIMLOG=1). Records unique REX3 draw
 * primitives and, crucially, commands that store ZERO pixels despite a
 * non-empty bbox (silently dropped draws). Default off; remove once the
 * dropped-draw path is fixed. */
static int sgi_pl_on = -1;
static uint64_t sgi_pl_stores;
static uint64_t sgi_pl_zerodropped;
static int sgi_pl_zerologged;
typedef struct {
    uint8_t op, am, plane, flags;
    uint32_t ci;
    uint64_t n;
    int x0, y0, x1, y1;
} SgiPlKey;
static SgiPlKey sgi_pl_keys[128];
static int sgi_pl_nkeys;
static uint8_t sgi_pl_prevop, sgi_pl_prevam, sgi_pl_prevplane,
    sgi_pl_prevflags;
static int sgi_pl_prevx0, sgi_pl_prevy0, sgi_pl_prevx1, sgi_pl_prevy1;
static uint64_t sgi_pl_prevs0;
static bool sgi_pl_haveprev;
static int sgi_pl_actx0, sgi_pl_acty0, sgi_pl_actx1, sgi_pl_acty1;
static bool sgi_pl_actvalid;
static uint64_t sgi_pl_misaddr;
static int sgi_pl_mislogged;
static int sgi_pl_wx, sgi_pl_wy;
static uint32_t sgi_pl_prevm0, sgi_pl_prevm1, sgi_pl_prevpat;
static uint32_t sgi_pl_prevci, sgi_pl_prevback, sgi_pl_prevvram;
static int sgi_pl_prevxsave, sgi_pl_prevysave;
static int sgi_pl_looplogged;
static uint64_t sgi_pl_cmd_s0;
/* Per-opcode dropped-draw counters (op>=2 only; op==1 reads store nothing
 * by design and must not consume the log quota). */
static uint64_t sgi_pl_zerodropped_op[4];
static unsigned sgi_pl_zerologged_op[4];
static bool sgi_pl_enabled(void)
{
    if (sgi_pl_on < 0) {
        sgi_pl_on = getenv("SGI_GIO_PRIMLOG") ? 1 : 0;
    }
    return sgi_pl_on != 0;
}

static void sgi_pl_note(uint8_t op, uint8_t am, uint8_t plane, uint8_t flags,
                        int x0, int y0, int x1, int y1)
{
    int i;

    /* Deferred: the previous command's stores are now final. */
    if (sgi_pl_haveprev) {
        uint64_t st = sgi_pl_stores - sgi_pl_prevs0;

        if (st == 0 && sgi_pl_prevx1 >= sgi_pl_prevx0 &&
            sgi_pl_prevy1 >= sgi_pl_prevy0 && sgi_pl_prevop >= 2) {
            sgi_pl_zerodropped++;
            sgi_pl_zerodropped_op[sgi_pl_prevop & 3]++;
            if (sgi_pl_zerologged_op[sgi_pl_prevop & 3] < 400) {
                sgi_pl_zerologged++;
                sgi_pl_zerologged_op[sgi_pl_prevop & 3]++;
                fprintf(stderr, "ZEROSTORE op=%u am=%u plane=%u flags=%02x "
                        "bbox=(%d,%d)-(%d,%d) m0=%08x m1=%08x pat=%08x "
                        "ci=%06x win=(%d,%d) xsave=%d\n", sgi_pl_prevop,
                        sgi_pl_prevam, sgi_pl_prevplane, sgi_pl_prevflags,
                        sgi_pl_prevx0, sgi_pl_prevy0, sgi_pl_prevx1,
                        sgi_pl_prevy1, sgi_pl_prevm0, sgi_pl_prevm1,
                        sgi_pl_prevpat, sgi_pl_prevci & 0xffffff,
                        sgi_pl_wx, sgi_pl_wy, sgi_pl_prevxsave);
            }
        } else if (st > 0 && sgi_pl_actvalid) {
            /* Intended bbox is in draw coords; actual stores are in fb
             * coords. Overlap check catches systematically misaddressed
             * draws (stores land nowhere near the target). */
            int ix0 = sgi_pl_prevx0 + sgi_pl_wx;
            int iy0 = sgi_pl_prevy0 + sgi_pl_wy;
            int ix1 = sgi_pl_prevx1 + sgi_pl_wx;
            int iy1 = sgi_pl_prevy1 + sgi_pl_wy;
            int nx0 = ix0 < ix1 ? ix0 : ix1;
            int nx1 = ix0 < ix1 ? ix1 : ix0;
            int ny0 = iy0 < iy1 ? iy0 : iy1;
            int ny1 = iy0 < iy1 ? iy1 : iy0;

            if (sgi_pl_actx1 < nx0 || sgi_pl_actx0 > nx1 ||
                sgi_pl_acty1 < ny0 || sgi_pl_acty0 > ny1) {
                sgi_pl_misaddr++;
                if (sgi_pl_mislogged < 20) {
                    sgi_pl_mislogged++;
                    fprintf(stderr, "MISADDR op=%u am=%u plane=%u flags=%02x "
                            "want=(%d,%d)-(%d,%d) got=(%d,%d)-(%d,%d) "
                            "m0=%08x m1=%08x pat=%08x xsave=%d\n",
                            sgi_pl_prevop, sgi_pl_prevam, sgi_pl_prevplane,
                            sgi_pl_prevflags, ix0, iy0, ix1, iy1,
                            sgi_pl_actx0, sgi_pl_acty0,
                            sgi_pl_actx1, sgi_pl_acty1,
                            sgi_pl_prevm0, sgi_pl_prevm1, sgi_pl_prevpat,
                            sgi_pl_prevxsave);
                }
            }
        }
    }
    sgi_pl_actvalid = false;
    for (i = 0; i < sgi_pl_nkeys; i++) {
        if (sgi_pl_keys[i].op == op && sgi_pl_keys[i].am == am &&
            sgi_pl_keys[i].plane == plane && sgi_pl_keys[i].flags == flags &&
            sgi_pl_keys[i].ci == sgi_pl_prevci) {
            sgi_pl_keys[i].n++;
            return;
        }
    }
    if (sgi_pl_nkeys < 128) {
        SgiPlKey *k = &sgi_pl_keys[sgi_pl_nkeys++];

        k->op = op; k->am = am; k->plane = plane; k->flags = flags;
        k->ci = sgi_pl_prevci;
        k->n = 1; k->x0 = x0; k->y0 = y0; k->x1 = x1; k->y1 = y1;
        fprintf(stderr, "PRIM op=%u am=%u plane=%u flags=%02x "
                "bbox=(%d,%d)-(%d,%d) ci=%06x back=%06x vram=%06x m1=%08x\n",
                op, am, plane, flags, x0, y0, x1, y1,
                sgi_pl_prevci & 0xffffff, sgi_pl_prevback & 0xffffff,
                sgi_pl_prevvram & 0xffffff, sgi_pl_prevm1);
    }
    sgi_pl_prevop = op; sgi_pl_prevam = am; sgi_pl_prevplane = plane;
    sgi_pl_prevflags = flags;
    sgi_pl_prevx0 = x0; sgi_pl_prevy0 = y0;
    sgi_pl_prevx1 = x1; sgi_pl_prevy1 = y1;
    sgi_pl_prevs0 = sgi_pl_stores;
    sgi_pl_haveprev = true;
}

/* TEMP full-refresh diagnostic (env SGI_GIO_FULLREFRESH=1). Forces a full
 * re-raster every frame so CPU-written VRAM (which never sets damage)
 * becomes visible. Decisive test for "content in VRAM but stale on screen".
 * Default off; remove once the damage gap is fixed. */
static int sgi_gio_fullrefresh = -1;
static bool sgi_gio_fullrefresh_enabled(void)
{
    if (sgi_gio_fullrefresh < 0) {
        sgi_gio_fullrefresh = getenv("SGI_GIO_FULLREFRESH") ? 1 : 0;
    }
    return sgi_gio_fullrefresh != 0;
}

/* TEMP fill-path statistics (env SGI_GIO_FILLSTATS=1). Cheap always-on
 * counters, reported at most every 2s; remove once fills are fixed. */
static uint64_t sgi_fs_cmd[4][8];
static uint64_t sgi_fs_fast, sgi_fs_fastpx, sgi_fs_slowpx, sgi_fs_hostpx;
static uint64_t sgi_fs_hostwrites, sgi_fs_hostgos, sgi_fs_hostmultigos;
static uint64_t sgi_fs_batchgos, sgi_fs_batchwords;
static int64_t sgi_fs_last_ms;
static int sgi_fs_on = -1;
typedef struct { uint32_t m0, m1; uint64_t n, px; } SGIFSMode;
static SGIFSMode sgi_fs_modes[64];
static int sgi_fs_nmodes;
static uint32_t sgi_fs_lsseen[32];
static int sgi_fs_nls;
static bool sgi_fs_enabled(void)
{
    if (sgi_fs_on < 0) {
        sgi_fs_on = getenv("SGI_GIO_FILLSTATS") ? 1 : 0;
    }
    return sgi_fs_on != 0;
}

/* TEMP command trace (env SGI_GIO_CMDT=1). Logs the wallpaper host block
 * GOs: entry coordinates + exit cursor, to diff against iris block.log. */
static int sgi_cmdt_on = -1;
static uint64_t sgi_cmdt_n;
static bool sgi_cmdt_enabled(void)
{
    if (sgi_cmdt_on < 0) {
        sgi_cmdt_on = getenv("SGI_GIO_CMDT") ? 1 : 0;
    }
    return sgi_cmdt_on != 0;
}

/* TEMP guest-read probe (reported with FILLSTATS). Counts STATUS (flow
 * control), USER_STATUS, DCBDATA and HOSTRW reads per window: a guest
 * that spins on status/backbusy or retries reads instead of issuing
 * draws shows up here. Zero overhead when FILLSTATS is off. */
static uint64_t sgi_rd_status, sgi_rd_user, sgi_rd_dcb, sgi_rd_host;

/* TEMP VRAM peek (env SGI_GIO_VRAMPEEK=1). Dumps one VRAM+aux rect
 * (hardcoded below) from the next frame: ground truth for WHERE draws
 * really land, independent of all counters and logs. One-shot. */
static int sgi_vrampeek_on = -1;
static bool sgi_vrampeek_done;
static bool sgi_vrampeek_enabled(void)
{
    if (sgi_vrampeek_on < 0) {
        sgi_vrampeek_on = getenv("SGI_GIO_VRAMPEEK") ? 1 : 0;
    }
    return sgi_vrampeek_on != 0;
}

/* TEMP scanout-map probe (env SGI_GIO_MODELOG=1). Logs XMAP mode-table
 * writes and, once, the full per-row DID->mode table used by scanout.
 * GL draws land correctly but the viewport stays black: this shows what
 * scanout does with those rows (wrong entry? wrong buffer? CI vs RGB?).
 * Default off. */
static int sgi_modlog_on = -1;
static bool sgi_modlog_dumped;
static unsigned sgi_modlog_seg_n;
static bool sgi_modlog_seg_armed;
static bool sgi_modlog_enabled(void)
{
    if (sgi_modlog_on < 0) {
        sgi_modlog_on = getenv("SGI_GIO_MODELOG") ? 1 : 0;
    }
    return sgi_modlog_on != 0;
}

/* TEMP RGB-command probe (env SGI_GIO_GLPROBE=1). Logs full addressing
 * state for RGBMODE draws (GL): bbox, xywin, smasks, clip, writemask.
 * GL stores millions of pixels that never appear; this shows WHERE they
 * go and under what clip/mask. Capped; default off. */
static int sgi_glprobe_on = -1;
static bool sgi_glprobe_enabled(void)
{
    if (sgi_glprobe_on < 0) {
        sgi_glprobe_on = getenv("SGI_GIO_GLPROBE") ? 1 : 0;
    }
    return sgi_glprobe_on != 0;
}

static void sgi_fs_report(void)
{
    int op, am;

    fprintf(stderr, "FILLSTATS");
    for (op = 0; op < 4; op++) {
        for (am = 0; am < 8; am++) {
            if (sgi_fs_cmd[op][am]) {
                fprintf(stderr, " c%d%d=%llu", op, am,
                        (unsigned long long)sgi_fs_cmd[op][am]);
            }
        }
    }
    fprintf(stderr, " fast=%llu fastpx=%llu slowpx=%llu hostpx=%llu"
            " hostw=%llu hostgo=%llu hostmul=%llu batchgo=%llu batchw=%llu\n",
            (unsigned long long)sgi_fs_fast,
            (unsigned long long)sgi_fs_fastpx,
            (unsigned long long)sgi_fs_slowpx,
            (unsigned long long)sgi_fs_hostpx,
            (unsigned long long)sgi_fs_hostwrites,
            (unsigned long long)sgi_fs_hostgos,
            (unsigned long long)sgi_fs_hostmultigos,
            (unsigned long long)sgi_fs_batchgos,
            (unsigned long long)sgi_fs_batchwords);
    {
        int i;
        for (i = 0; i < sgi_fs_nmodes; i++) {
            fprintf(stderr, "MODEFS m0=%08x m1=%08x n=%llu px=%llu\n",
                    sgi_fs_modes[i].m0, sgi_fs_modes[i].m1,
                    (unsigned long long)sgi_fs_modes[i].n,
                    (unsigned long long)sgi_fs_modes[i].px);
        }
    }
    memset(sgi_fs_cmd, 0, sizeof(sgi_fs_cmd));
    sgi_fs_fast = sgi_fs_fastpx = sgi_fs_slowpx = sgi_fs_hostpx = 0;
    sgi_fs_hostwrites = sgi_fs_hostgos = sgi_fs_hostmultigos = 0;
    sgi_fs_batchgos = sgi_fs_batchwords = 0;
    sgi_fs_nmodes = 0;
    if (sgi_rd_status || sgi_rd_user || sgi_rd_dcb || sgi_rd_host) {
        fprintf(stderr, "READS status=%llu user=%llu dcb=%llu host=%llu\n",
                (unsigned long long)sgi_rd_status,
                (unsigned long long)sgi_rd_user,
                (unsigned long long)sgi_rd_dcb,
                (unsigned long long)sgi_rd_host);
        sgi_rd_status = sgi_rd_user = sgi_rd_dcb = sgi_rd_host = 0;
    }
    if (sgi_pl_enabled()) {
        fprintf(stderr, "PRIMDROP op1=%llu op2=%llu op3=%llu logged=%d\n",
                (unsigned long long)sgi_pl_zerodropped_op[1],
                (unsigned long long)sgi_pl_zerodropped_op[2],
                (unsigned long long)sgi_pl_zerodropped_op[3],
                sgi_pl_zerologged);
    }
}

/* Board tracing is compiled out by default (zero run-time overhead).
 * Rebuild with -DSGI_DEBUG to re-enable the SGI_TRACE=1 environment toggle
 * for offline debugging. */
static bool sgi_gio_trace_enabled(void)
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

static bool sgi_gio_trace_wanted(hwaddr addr)
{
    unsigned idx = addr & 0xfff;

    if (sgi_gio_trace_count[idx] >= SGI_GIO_TRACE_PER_OFFSET) {
        return false;
    }
    sgi_gio_trace_count[idx]++;
    return true;
}
/* DCB slave addresses (MAME newport.h dcb_addr_t). Only the GFX slot card
 * (XL24) is present; EXP0/EXP1 are empty bus timeouts. */
enum {
    DCB_ADDR_VC2 = 0,
    DCB_ADDR_CMAP01 = 1,
    DCB_ADDR_CMAP0 = 2,
    DCB_ADDR_CMAP1 = 3,
    DCB_ADDR_XMAP01 = 4,
    DCB_ADDR_XMAP0 = 5,
    DCB_ADDR_XMAP1 = 6,
    DCB_ADDR_RAMDAC = 7,
    DCB_ADDR_CC1 = 8,
    DCB_ADDR_AB1 = 9,
    DCB_ADDR_PCD = 12,
};
/* VC2 register indices (newport.cpp vc2_device::write case list). */
enum {
    VC2_REG_VID_ENTRY = 0x00,
    VC2_REG_CURSOR_ENTRY = 0x01,
    VC2_REG_CURSOR_X = 0x02,
    VC2_REG_CURSOR_Y = 0x03,
    VC2_REG_DID_ENTRY = 0x05,
    VC2_REG_SCANLINE_LEN = 0x06,
    VC2_REG_RAM_ADDR = 0x07,
    VC2_REG_DID_FRAME = 0x0e,
    VC2_REG_DID_LINE = 0x0f,
    VC2_REG_DISPLAY_CTRL = 0x10,
};
/* VC2 display-control cursor bits (newport.h cursor_* enum). */
#define VC2_CURSOR_FUNC_ENABLE (1U << 4)
#define VC2_CURSOR_ENABLE (1U << 7)
#define VC2_CURSOR_MODE_GLYPH 0U
#define VC2_CURSOR_MODE_BIT (1U << 8)
#define VC2_CURSOR_SIZE_64 (1U << 9)
/* Hardware-cursor x origin bias, in pixels. The VC2 fetch window is
 * [CURSOR_X-31, CURSOR_X] with no bias (MAME newport.cpp get_cursor_pixel),
 * but IRIX places the drawn pointer a further 7px left of that window's
 * right edge; without it the arrow sits ~7px right of the click point.
 * iris derives this as hpos_to_visible(40) - 31 - 2 = 7 (disp.rs
 * decode_vc2_timings), where the -2 is IRIX's own two blank left columns. */
#define SGI_GIO64_CURSOR_X_ADJUST 7
/* XL24 (Newport) IDs. XMAP9 rev 3 matches MAME's xl24 wiring; CMAP rev 0x74
 * is what the PROM needs: high nibble 7 for the graphics info, low 3 bits
 * >= 2 for VC2 programming (MAME's 0x02 selects the wrong video tables). */
#define XL24_XMAP_REVISION 3
#define XL24_CMAP_REVISION 0x74
#define XL24_CMAP_STATUS 8

struct SGIGIO64State {
    SysBusDevice parent_obj;
    /* Surface-draw worker (see sgi_gio64_go). */
    QemuThread gfx_thread;
    QemuMutex gfx_mtx;
    QemuCond gfx_cond_work;
    QemuCond gfx_cond_idle;
    bool gfx_pending;
    bool gfx_busy;
    bool gfx_ready;
    MemoryRegion bus;
    /* RB2 pixel store, internal only: like MAME's card, the CPU has no
     * linear VRAM window; pixels move via REX3 draw/host-dataport. */
    MemoryRegion vram;
    /* RB2 aux/CID plane (overlay/popup/CID bits, MAME m_cidaux). REX3
     * writes with draw_mode1 planes 4/5/6 land here; the scanout
     * composites popup/overlay/underlay from it. Same geometry as vram. */
    MemoryRegion aux;
    MemoryRegion xl24;
    /* Open-bus for undecoded GIO ranges: reads return all-1s so PROM
     * polling terminates, like unwired MAME PIO. */
    MemoryRegion gio_unmapped;
    QemuConsole *console;
    qemu_irq gio_irq[3];
    /* Frame timer: 60Hz VRINT while VC2 display control bit 0 is set
     * (MAME screen vblank chain). The REX3 status bit and the GIO_INT_2
     * line to the IOC2 both stay latched until the guest reads the REX3
     * STATUS register (MAME newport.cpp:1700-1706 assert, 2151-2152
     * clear); there is no frame-end deassert. */
    QEMUTimer *vblank_timer;
    /* 1ms DCB timeout: clears BACKBUSY after a PCD access, matching
     * MAME's m_dcb_timeout_timer. */
    QEMUTimer *dcb_timer;
    uint32_t rex3_config;
    uint32_t rex3_status;
    uint32_t rex3_regs[XL24_REX3_SIZE / sizeof(uint32_t)];
    uint64_t host_dataport;
    /* HOSTRW word staging. A PIO write is one word drained by its GO
     * (host_len == 1); a DMA-batched transfer stages N words that a single
     * GO drains as a run (iris rex3.rs:3172-3206, rex3_generic.rs:1171-1199).
     * The FM/Rex3 port is 64 bits, so only whole qwords stage. */
    uint64_t host_words[16384];
    uint32_t host_staged;
    uint32_t host_len;
    uint32_t host_cursor;
    uint32_t draw_mode1;
    uint32_t draw_mode0;
    /* REX3 geometry/state file (MAME newport_base_device::rex3_t). Start/end
     * pairs must convert exactly: the PROM round-trips them in diagnostics. */
    int32_t x_start;
    int32_t y_start;
    int32_t x_end;
    int32_t y_end;
    int16_t x_start_frac;
    int16_t y_start_frac;
    int16_t x_end_frac;
    int16_t y_end_frac;
    int16_t x_start_i;
    int16_t y_start_i;
    int16_t x_end_i;
    int16_t y_end_i;
    uint32_t xy_start_i;
    uint32_t xy_end_i;
    uint32_t x_start_end_i;
    uint32_t x_start_f;
    uint32_t y_start_f;
    uint32_t x_end_f;
    uint32_t y_end_f;
    int16_t x_save;
    uint32_t xy_move;
    uint32_t bres_d;
    uint32_t bres_s1;
    uint32_t bres_octant_inc1;
    uint32_t bres_round_inc2;
    uint32_t bres_e1;
    uint32_t bres_s2;
    uint32_t a_weight0;
    uint32_t a_weight1;
    uint32_t ls_mode;
    uint32_t ls_pattern_saved;
    uint32_t color_vram;
    uint32_t zero_overflow;
    uint32_t top_scanline;
    uint32_t xy_window;
    int16_t x_window;
    int16_t y_window;
    uint32_t setup;
    int32_t x_move;
    int32_t y_move;
    uint32_t xy_start;
    uint32_t xy_end;
    uint32_t color_i;
    uint32_t write_mask;
    uint32_t dcb_mode;
    uint32_t dcb_reg_select;
    uint32_t dcb_slave_select;
    uint32_t dcb_data_msw;
    uint32_t dcb_data_lsw;
    uint32_t dcb_mask;
    uint32_t line_stipple_pattern;
    uint32_t z_pattern;
    /* Live pattern cursors (iris advance_lspat/zpat + rex3.pdf 3.8.1).
     * Reset to 31 on DOSETUP only; block rows restart them (iris
     * draw_block_g); lsrcount continues across rows. */
    uint8_t pat_bit;
    uint8_t zpat_bit;
    uint32_t color_back;
    uint8_t host_shift;
    int32_t curr_color_red;
    int32_t curr_color_green;
    int32_t curr_color_blue;
    int32_t curr_color_alpha;
    int32_t slope_red;
    int32_t slope_green;
    int32_t slope_blue;
    int32_t slope_alpha;
    uint32_t alpha_ref;
    uint32_t step_z;
    uint32_t z_value;
    uint32_t z_slope;
    uint32_t zbuffer[XL24_STRIDE * XL24_HEIGHT];
    uint8_t plane_depth;
    uint8_t host_depth;
    /* Per-head CMAP/XMAP state (head 0 drives the display). Broadcast slaves
     * program both heads. Palette words are 0x00RRGGBB (MAME cmap order). */
    uint32_t cmap_palette[2][65536];
    uint16_t cmap_index[2];
    uint32_t ramdac_lut_r[256];
    uint32_t ramdac_lut_g[256];
    uint32_t ramdac_lut_b[256];
    uint8_t ramdac_index;
    uint32_t xmap_mode[2][32];
    uint8_t xmap_mode_index[2];
    uint32_t xmap_config[2];
    uint32_t xmap_cursor_cmap[2];
    uint32_t xmap_popup_cmap[2];
    uint16_t vc2_regs[32];
    uint16_t vc2_ram[0x8000];
    uint16_t vc2_ram_addr;
    uint8_t vc2_reg_idx;
    /* VC2 video-timing readout (MAME vc2_device + newport.cpp:724-775).
     * Visible size is (x1-x0) x (y1-y0); zero means not yet programmed. */
    int readout_x0;
    int readout_y0;
    int readout_x1;
    int readout_y1;
    uint32_t screenmask_x[5];
    uint32_t screenmask_y[5];
    uint32_t clip_mode;
    uint32_t draw_color;
    /* Set whenever a GIO write or REX3 command can change what is scanned
     * out. The UI refresh calls gfx_update ~30x/s even when the guest is
     * idle; skipping the full 1280x1024 re-raster (and the console blit)
     * when this is clear removes that cost without changing the output. */
    bool display_dirty;
    /* Damage tracking for partial scanout: REX3 draws union their
     * framebuffer rect here, while palette/XMAP/VC2/RAMDAC writes fall
     * back to a full frame via dmg_full. */
    bool dmg_full;
    bool dmg_valid;
    int dmg_x0;
    int dmg_y0;
    int dmg_x1;
    int dmg_y1;
    /* Cached RB2 store pointers, hoisted out of the per-command path
     * (refreshed on realize/migration; never guest-visible). */
    uint32_t *vram_ptr;
    uint32_t *aux_ptr;
    /* Reusable Scr2Scr snapshot, grown on demand instead of per command. */
    uint32_t *blt_buf;
    size_t blt_len;
};

/* Union raw framebuffer coords into the damage rect (over-cover is safe,
 * under-cover leaves stale pixels). Clamped to the RB2 store. */
static void sgi_gio64_damage_union(SGIGIO64State *state, int x0, int y0,
                                   int x1, int y1)
{
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    if (x1 < 0 || y1 < 0 || x0 >= XL24_STRIDE || y0 >= XL24_HEIGHT) {
        return;
    }
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    x1 = MIN(x1, XL24_STRIDE - 1);
    y1 = MIN(y1, XL24_HEIGHT - 1);
    state->display_dirty = true;
    if (!state->dmg_valid) {
        state->dmg_valid = true;
        state->dmg_x0 = x0;
        state->dmg_y0 = y0;
        state->dmg_x1 = x1;
        state->dmg_y1 = y1;
    } else {
        state->dmg_x0 = MIN(state->dmg_x0, x0);
        state->dmg_y0 = MIN(state->dmg_y0, y0);
        state->dmg_x1 = MAX(state->dmg_x1, x1);
        state->dmg_y1 = MAX(state->dmg_y1, y1);
    }
}

/* Latch a draw-coords bbox (REX3 coords need the window offset applied,
 * like put_pixel does) into the damage rect. */
static void sgi_gio64_damage_rect(SGIGIO64State *state, int x0, int y0,
                                  int x1, int y1)
{
    int wx = (int)state->x_window - 0x1000;
    int wy = (int)state->y_window - 0x1000;

    sgi_gio64_damage_union(state, x0 + wx, y0 + wy, x1 + wx, y1 + wy);
}

/* Config writes (palette/XMAP/VC2/RAMDAC) reinterpret existing pixels, so
 * only a full re-raster is correct. */
static void sgi_gio64_damage_full(SGIGIO64State *state)
{
    state->display_dirty = true;
    state->dmg_full = true;
}

/* Cursor moves only repaint the small fetch window (see cursor_pixel). */
static void sgi_gio64_damage_cursor(SGIGIO64State *state)
{
    int cx = state->vc2_regs[VC2_REG_CURSOR_X] - SGI_GIO64_CURSOR_X_ADJUST;
    int cy = state->vc2_regs[VC2_REG_CURSOR_Y];

    sgi_gio64_damage_union(state, cx - 31, cy - 31, cx, cy);
}

/* Migration dirty tracking scoped to the written rect (draw coords, window
 * offset applied like put_pixel). Whole-VRAM marks churn the dirty bitmap
 * on every small command; the rect covers every pixel put_pixel can store
 * (clip/Z/stipple only shrink it). */
static void sgi_gio64_fb_dirty(SGIGIO64State *state, bool aux,
                               int x0, int y0, int x1, int y1)
{
    MemoryRegion *mr = aux ? &state->aux : &state->vram;
    hwaddr base, top;

    x0 += (int)state->x_window - 0x1000;
    x1 += (int)state->x_window - 0x1000;
    y0 += (int)state->y_window - 0x1000;
    y1 += (int)state->y_window - 0x1000;
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    x1 = MIN(x1, XL24_STRIDE - 1);
    y1 = MIN(y1, XL24_HEIGHT - 1);
    if (x0 > x1 || y0 > y1) {
        return;
    }
    base = ((hwaddr)y0 * XL24_STRIDE + x0) * 4;
    top = ((hwaddr)y1 * XL24_STRIDE + x1) * 4 + 4;
    memory_region_set_dirty(mr, base, top - base);
}

/* VC2 video-timing decode (MAME vc2_device::decode_vt_line/update_screen_size,
 * newport.cpp:669-775). Visible size comes from bit 7 of the decoded pixels,
 * not from SCANLINE_LEN, which MAME never uses for sizing. */
static void sgi_gio64_decode_vt_line(SGIGIO64State *state, uint32_t *vt_table,
                                     uint32_t line, uint32_t line_seq_ptr)
{
    bool eol_sa = false;
    bool eol_sb = false;
    uint32_t vt_entry = 0;
    uint32_t i = 0;

    do {
        vt_entry &= ~(0x7fU << 14);
        vt_entry |= (state->vc2_ram[line_seq_ptr & 0x7fff] & 0x7f) << 14;
        eol_sa = (state->vc2_ram[line_seq_ptr & 0x7fff] >> 15) & 1;
        if (!(state->vc2_ram[line_seq_ptr & 0x7fff] >> 7 & 1)) {
            eol_sb = (state->vc2_ram[(line_seq_ptr + 1) & 0x7fff] >> 15) & 1;
            vt_entry &= 0x7fU << 14;
            vt_entry |= (state->vc2_ram[(line_seq_ptr + 1) & 0x7fff] &
                         0x7f00) >> 1;
            vt_entry |= state->vc2_ram[(line_seq_ptr + 1) & 0x7fff] & 0x7f;
            line_seq_ptr += 2;
        } else {
            eol_sb = true;
            line_seq_ptr++;
        }
        for (uint32_t j = 0;
             j < ((state->vc2_ram[line_seq_ptr & 0x7fff] & 0x7f00) >> 7);
             i++, j++) {
            uint8_t sa;
            uint8_t sb;
            uint8_t sc;

            if (i >= 2048) {
                break;
            }
            sa = ((vt_entry >> 14) & 0x7f) << 1;
            sb = ((vt_entry >> 7) & 0x7f) << 1;
            sc = ((vt_entry >> 0) & 0x7f) << 1;
            vt_table[line * 2048 + i] =
                0xff000000U | (sa << 16) | (sb << 8) | sc;
        }
    } while (!(eol_sa && eol_sb));
}

static void sgi_gio64_update_screen_size(SGIGIO64State *state)
{
    uint32_t *vt_table = g_malloc0(sizeof(uint32_t) * 2048 * 2048);
    uint32_t curr_vt_entry = state->vc2_regs[VC2_REG_VID_ENTRY];
    uint32_t line_counter = 0;
    uint32_t line_seq_len = 0;
    bool x_started = false;
    bool y_started = false;
    bool x_done = false;
    bool y_done = false;
    bool done = false;

    do {
        uint32_t line_seq_ptr =
            state->vc2_ram[curr_vt_entry & 0x7fff];

        line_seq_len = state->vc2_ram[(curr_vt_entry + 1) & 0x7fff];
        if (line_seq_len) {
            for (uint32_t i = 0; i < line_seq_len; i++, line_counter++) {
                if (line_counter >= 2048) {
                    break;
                }
                sgi_gio64_decode_vt_line(state, vt_table, line_counter,
                                         line_seq_ptr);
            }
        }
        curr_vt_entry = (curr_vt_entry + 2) & 0x7fff;
    } while (line_seq_len != 0 && line_counter < 2048);

    state->readout_x0 = 0;
    state->readout_y0 = 0;
    state->readout_x1 = 0;
    state->readout_y1 = 0;
    for (int y = 0; y < 2048 && !done; y++) {
        uint32_t *src = &vt_table[y * 2048];

        for (int x = 0; x < 2048 && !done; x++) {
            if (*src & (1U << 7)) {
                if (!x_started) {
                    x_started = true;
                    state->readout_x0 = x;
                }
                if (!y_started) {
                    y_started = true;
                    state->readout_y0 = y;
                }
            } else {
                if (x_started && !x_done) {
                    state->readout_x1 = x;
                    x_done = true;
                }
                if (y_started && !y_done && x == state->readout_x0) {
                    state->readout_y1 = y;
                    y_done = true;
                }
            }
            done = x_done && y_done;
            src++;
        }
    }
    /* MAME newport.cpp:693 reads each run length from the word after the
     * entry, so the last visible run absorbs the next state's count: 18px
     * of background. Drop them to show the mode the guest drew (1024/1280). */
    if (x_done && y_done && state->readout_x1 - state->readout_x0 > 18) {
        state->readout_x1 -= 18;
    }
    g_free(vt_table);
}

static unsigned sgi_gio64_display_width(SGIGIO64State *state)
{
    int w = state->readout_x1 - state->readout_x0;

    /* MAME initial screen is 1288x1024 before VID_ENTRY programs VT
     * (newport.cpp:4480-4481). Zero/invalid readout falls back there. */
    if (w == 1288 || w == 1024) {
        return w;
    }
    if (w > 0 && w <= XL24_STRIDE) {
        return w;
    }
    return XL24_WIDTH;
}

static unsigned sgi_gio64_display_height(SGIGIO64State *state)
{
    int h = state->readout_y1 - state->readout_y0;

    if (h == 1024 || h == 768) {
        return h;
    }
    if (h > 0 && h <= XL24_HEIGHT) {
        return h;
    }
    return XL24_HEIGHT;
}

/* TEMP write attribution (env SGI_GIO_ROWMAP=1). Tracks per-pixel the last
 * writer's mode0/low-byte and, separately, the last zpattern-op2 writer.
 * Used to tell whether the settled-desktop blue bands were painted by the
 * zpattern pass itself (pattern/phase bug) or overwritten later, or never
 * covered at all. Remove before commit. */
static int sgi_rowmap_on = -1;
static unsigned sgi_rowmap_tick;
static uint64_t sgi_row_pix[XL24_HEIGHT];
static uint64_t sgi_row_zpat[XL24_HEIGHT];
static uint32_t *sgi_px_lastmode;
static uint32_t *sgi_px_zmode;
static uint8_t *sgi_px_lastval;
static uint8_t *sgi_px_zval;

static bool sgi_rowmap_enabled(void)
{
    if (sgi_rowmap_on < 0) {
        sgi_rowmap_on = getenv("SGI_GIO_ROWMAP") ? 1 : 0;
    }
    return sgi_rowmap_on != 0;
}

static void sgi_px_ensure(void)
{
    size_t n = (size_t)XL24_STRIDE * XL24_HEIGHT;

    if (!sgi_px_lastmode) {
        sgi_px_lastmode = g_malloc0(n * sizeof(uint32_t));
        sgi_px_zmode = g_malloc0(n * sizeof(uint32_t));
        sgi_px_lastval = g_malloc0(n);
        sgi_px_zval = g_malloc0(n);
    }
}

static inline void sgi_row_note(uint32_t mode0, int x, int y,
                                uint32_t val, int n)
{
    bool is_zpat = (mode0 & 3) == 2 &&
                   (mode0 & ((1U << 12) | (1U << 13)));
    int i;

    if (n <= 0 || y < 0 || y >= XL24_HEIGHT) {
        return;
    }
    sgi_row_pix[y] += n;
    if (is_zpat) {
        sgi_row_zpat[y] += n;
    }
    sgi_px_ensure();
    for (i = 0; i < n; i++) {
        int xx = x + i;
        size_t o;

        if (xx < 0 || xx >= XL24_STRIDE) {
            continue;
        }
        o = (size_t)y * XL24_STRIDE + xx;
        sgi_px_lastmode[o] = mode0;
        sgi_px_lastval[o] = val & 0xff;
        if (is_zpat) {
            sgi_px_zmode[o] = mode0;
            sgi_px_zval[o] = val & 0xff;
        }
    }
}

static void sgi_gio64_rowmap_dump(SGIGIO64State *state)
{
    static const int cols[] = { 5, 100, 640, 1200, 1275 };
    uint32_t *vram = state->vram_ptr;
    int y, run0, ci;
    char prev = 0;

    fprintf(stderr, "ROWMAP-BEGIN\n");
    run0 = 0;
    for (y = 0; y <= XL24_HEIGHT; y++) {
        char c;

        if (y == XL24_HEIGHT) {
            c = 1;
        } else if (sgi_row_zpat[y]) {
            c = 'Z';
        } else if (sgi_row_pix[y]) {
            c = 'z';
        } else {
            c = '.';
        }
        if (c != prev) {
            if (prev) {
                fprintf(stderr, "ROWMAP-CLASS %c %d %d\n", prev, run0, y - 1);
            }
            prev = c;
            run0 = y;
        }
    }
    for (ci = 0; ci < (int)ARRAY_SIZE(cols); ci++) {
        int x = cols[ci];
        char pv = 0;
        uint32_t plm = 0, pzm = 0;
        uint8_t plv = 0, pzv = 0;

        run0 = 0;
        for (y = 0; y <= XL24_HEIGHT; y++) {
            char c = 1;
            uint32_t lm = 0, zm = 0;
            uint8_t lv = 0, zv = 0;

            if (y < XL24_HEIGHT) {
                size_t o = (size_t)y * XL24_STRIDE + x;

                lv = vram[o] & 0xff;
                lm = sgi_px_lastmode[o];
                lv = sgi_px_lastval[o];
                zm = sgi_px_zmode[o];
                zv = sgi_px_zval[o];
                c = (char)((lv << 1) & 0x7f);
            }
            if (y == XL24_HEIGHT || lm != plm || lv != plv ||
                (zm ? 1 : 0) != (pzm ? 1 : 0)) {
                if (y > 0) {
                    fprintf(stderr,
                            "COL %d y %d %d vram=%02x last=%08x lv=%02x "
                            "z=%d zm=%08x zv=%02x\n",
                            x, run0, y - 1, (unsigned)(vram[(size_t)run0 *
                            XL24_STRIDE + x] & 0xff), plm, plv,
                            pzm ? 1 : 0, pzm, pzv);
                }
                run0 = y;
                plm = lm;
                plv = lv;
                pzm = zm;
                pzv = zv;
            }
            (void)c;
            (void)pv;
        }
    }
    fprintf(stderr, "ROWMAP-END\n");
}

static void sgi_gio64_vblank(void *opaque)
{
    SGIGIO64State *state = opaque;

    if (sgi_rowmap_enabled() && (++sgi_rowmap_tick % 300) == 0) {
        sgi_gio64_rowmap_dump(state);
    }

    /* Raise VRINT only while VC2 display control bit 0 is set (MAME
     * newport.cpp:816-824, 1700-1706). MAME's vrint_w only ever asserts:
     * the STATUS bit AND the GIO interrupt<2> line stay latched until the
     * REX3 STATUS register is read (newport.cpp:2151-2152 clears both), it
     * is NOT released at frame end. Holding the line is also what iris does
     * (vblank_cb(true) on assert, vblank_cb(false) on the STATUS read; see
     * docs/interrupt_map.md). Self-clearing here would lose the interrupt
     * whenever the guest does not service it within the pulse. */
    if (state->vc2_regs[VC2_REG_DISPLAY_CTRL] & 1) {
        state->rex3_status |= REX3_STATUS_VRINT;
        qemu_set_irq(state->gio_irq[2], 1);
    }
    timer_mod_ns(state->vblank_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 1000000000ULL / 60);
}

static void sgi_gio64_dcb_timeout(void *opaque)
{
    SGIGIO64State *state = opaque;

    state->rex3_status &= ~REX3_STATUS_BACKBUSY;
}

static void sgi_gio64_pcd_access(SGIGIO64State *state)
{
    /* Presenter not connected: BACKBUSY for 1ms (MAME m_dcb_timeout_timer). */
    state->rex3_status |= REX3_STATUS_BACKBUSY;
    timer_mod_ns(state->dcb_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000ULL);
}

/* REX3 coordinate conversion (MAME write_x_start/write_y_start/write_x_end/
 * write_y_end). Raw values carry 11 fractional bits; _i is the integer pixel
 * coordinate the rasterizer consumes. */
static void sgi_gio64_write_x_start(SGIGIO64State *state, int32_t val)
{
    state->x_start = val & 0x07ffff80;
    state->x_start_frac = (val >> 7) & 0xf;
    state->x_start_i = (int16_t)(val >> 11);
    state->x_start_f = (uint32_t)val & 0x007fff80;
    state->xy_start_i = (state->xy_start_i & 0x0000ffff) |
                        ((uint32_t)(uint16_t)state->x_start_i << 16);
}

static void sgi_gio64_write_y_start(SGIGIO64State *state, int32_t val)
{
    state->y_start = val & 0x07ffff80;
    state->y_start_frac = (val >> 7) & 0xf;
    state->y_start_i = (int16_t)(val >> 11);
    state->y_start_f = (uint32_t)val & 0x007fff80;
    state->xy_start_i = (state->xy_start_i & 0xffff0000) |
                        (uint16_t)state->y_start_i;
}

static void sgi_gio64_write_x_end(SGIGIO64State *state, int32_t val)
{
    state->x_end = val & 0x07ffff80;
    state->x_end_frac = (val >> 7) & 0xf;
    state->x_end_i = (int16_t)(val >> 11);
    state->x_end_f = (uint32_t)val & 0x007fff80;
    state->xy_end_i = (state->xy_end_i & 0x0000ffff) |
                      ((uint32_t)(uint16_t)state->x_end_i << 16);
}

static void sgi_gio64_write_y_end(SGIGIO64State *state, int32_t val)
{
    state->y_end = val & 0x07ffff80;
    state->y_end_frac = (val >> 7) & 0xf;
    state->y_end_i = (int16_t)(val >> 11);
    state->y_end_f = (uint32_t)val & 0x007fff80;
    state->xy_end_i = (state->xy_end_i & 0xffff0000) |
                      (uint16_t)state->y_end_i;
}

/* 20-bit sign extension for the setup coordinates (MAME util::sext). */
static int32_t sgi_gio64_sext20(uint32_t value)
{
    return (int32_t)(value & 0x80000 ? value | 0xfff00000 : value);
}

/* REX3 iterator setup (MAME get_octant/do_setup, newport.cpp:2693-2739).
 * With Draw Mode 0 bit 5 set, recompute the octant direction bits from the
 * current coordinates first: a stale YDEC bit would step a rectangle the
 * wrong way and paint rows y1..0 instead of y1..y2. Octant {XMAJOR,XDEC,YDEC}. */
static void sgi_gio64_rex3_setup(SGIGIO64State *state)
{
    int32_t x1 = sgi_gio64_sext20((uint32_t)state->x_start >> 7);
    int32_t y1 = sgi_gio64_sext20((uint32_t)state->y_start >> 7);
    int32_t x2 = sgi_gio64_sext20((uint32_t)state->x_end >> 7);
    int32_t y2 = sgi_gio64_sext20((uint32_t)state->y_end >> 7);
    uint8_t octant = 0;

    if (y1 > y2) {
        octant |= 1 << 0; /* YDEC */
    }
    if (x1 > x2) {
        octant |= 1 << 1; /* XDEC */
    }
    if (abs(x1 - x2) > abs(y1 - y2)) {
        octant |= 1 << 2; /* XMAJOR */
    }
    state->bres_octant_inc1 &= ~(0x7U << 24);
    state->bres_octant_inc1 |= (uint32_t)octant << 24;
}

static uint32_t sgi_gio64_high_word(uint64_t value, unsigned size, hwaddr addr)
{
    if (size == 8) {
        return addr & 4 ? value : value >> 32;
    }
    return addr & 4 ? 0 : value;
}

static uint32_t sgi_gio64_low_word(uint64_t value, unsigned size, hwaddr addr)
{
    if (size == 8) {
        return addr & 4 ? value >> 32 : value;
    }
    return addr & 4 ? value : 0;
}

static uint8_t sgi_gio64_clamp_color(int32_t value)
{
    /* MAME get_rgb_color / iris clamp_color_component: mask the integer
     * to 9 bits first. DDA currents legitimately carry overflow/high
     * bits (e.g. red 0x90xxxx has bit23 set); without the mask those
     * slam every channel to 255 (redscale). Negative (bit31) or
     * >= 0x180 reads as 0; above 0xff saturates. */
    uint32_t v = ((uint32_t)value >> 11) & 0x1ff;

    if (v >= 0x180 || value < 0) {
        return 0;
    }
    return v > 0xff ? 0xff : (uint8_t)v;
}

/* MAME newport s_host_shifts (dataport shift step per host depth). */
static unsigned sgi_gio64_host_shift(unsigned host_depth)
{
    static const unsigned steps[] = { 8, 8, 16, 32 };

    return steps[host_depth & 3];
}

/* iris loads hostrw[host_cursor] whenever the shifter is empty; here a
 * fresh word is signalled by the shift being back at its initial 64-step
 * value (host_advance wraps to exactly this). */
static unsigned sgi_gio64_host_initial_shift(SGIGIO64State *state)
{
    return 64 - sgi_gio64_host_shift(state->host_depth);
}

/* len > 1 means a batched transfer: a PIO write is len == 1 and reproduces
 * one-word-per-GO exactly (iris rex3_generic.rs:1171-1175). */

static void sgi_gio64_host_stage(SGIGIO64State *state, uint64_t word)
{
    if (state->host_staged < ARRAY_SIZE(state->host_words)) {
        state->host_words[state->host_staged++] = word;
    }
}

/* MAME get_host_color shift advance: only packed transfers consume the
 * dataport; unpacked transfers reuse the same chunk every pixel. Uses
 * signed arithmetic like MAME (never underflows the stored uint8_t:
 * a would-be negative shift resets instead). */
static void sgi_gio64_host_advance(SGIGIO64State *state, unsigned width)
{
    bool rwpacked = (state->draw_mode1 >> 7) & 1;
    bool rwdouble = (state->draw_mode1 >> 10) & 1;
    int shift = (int)state->host_shift;

    if (!rwpacked) {
        return;
    }
    if ((rwdouble && shift > 0) || shift > 32) {
        shift -= (int)width;
        if (shift < 0) {
            shift = 64 - (int)width;
        }
    } else {
        shift = 64 - (int)width;
    }
    state->host_shift = (uint8_t)shift;
}

static uint32_t sgi_gio64_host_color(SGIGIO64State *state)
{
    /* MAME s_color_masks / s_host_shifts (value mask and shift step per
     * host depth). */
    static const uint32_t masks[] = { 0xf, 0xff, 0xfff, 0xffffffff };
    static const unsigned widths[] = { 8, 8, 16, 32 };
    unsigned width = widths[state->host_depth & 3];
    uint32_t value;

    /* Refill the shifter from the staged run when it just wrapped and the
     * batch still has words left (iris rex3_generic.rs:1181-1199). */
    if (state->host_len > 0 &&
        state->host_shift == sgi_gio64_host_initial_shift(state) &&
        state->host_cursor < state->host_len) {
        state->host_dataport = state->host_words[state->host_cursor++];
    }
    value = (state->host_dataport >> state->host_shift) &
            masks[state->host_depth & 3];
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    unsigned convert_index =
        ((state->host_depth & 3) << 2) | (state->plane_depth & 3);

    /* MAME get_host_color convert matrix diagonal (host depth matches
     * plane depth): no conversion, the raw value passes through. This
     * preserves CI palette indices (RGB assembly would destroy them). */
    if (convert_index == 0 || convert_index == 5 ||
        convert_index == 10 || convert_index == 15) {
        sgi_gio64_host_advance(state, width);
        if ((state->draw_mode1 >> 11) & 1) {
            value = __builtin_bswap32(value);
        }
        return value;
    }

    switch (state->host_depth) {
    case 0:
        red = (value & 1) ? 255 : 0;
        green = ((value >> 1) & 3) * 85;
        blue = (value & 8) ? 255 : 0;
        break;
    case 1:
        red = (value & 7) * 255 / 7;
        green = ((value >> 3) & 7) * 255 / 7;
        blue = ((value >> 6) & 3) * 255 / 3;
        break;
    case 2:
        red = (value & 0xf) * 17;
        green = ((value >> 4) & 0xf) * 17;
        blue = ((value >> 8) & 0xf) * 17;
        break;
    default:
        red = value & 0xff;
        green = (value >> 8) & 0xff;
        blue = (value >> 16) & 0xff;
        break;
    }
    sgi_gio64_host_advance(state, width);
    if (state->plane_depth == 3) {
        /* 24-bit direct: keep the full assembled color (0x00BBGGRR, MAME
         * get_host_color layout); the store expands/masks per depth. */
        uint32_t rgb = (blue << 16) | (green << 8) | red;

        if ((state->draw_mode1 >> 11) & 1) {
            rgb = __builtin_bswap32(rgb);
        }
        return rgb;
    }
    if ((state->draw_mode1 >> 11) & 1) {
        uint32_t rgb = (blue & 0xc0) | ((green & 0xe0) >> 2) |
                       ((red & 0xe0) >> 5);

        return __builtin_bswap32(rgb);
    }
    return (blue & 0xc0) | ((green & 0xe0) >> 2) | ((red & 0xe0) >> 5);
}

/* 4x4 Bayer matrix (MAME s_bayer == iris BAYER_PACKED). */
static const uint8_t sgi_gio64_bayer[4][4] = {
    { 0, 12, 3, 15 }, { 8, 4, 11, 7 },
    { 2, 14, 1, 13 }, { 10, 6, 9, 5 },
};

static uint32_t sgi_gio64_shaded_color(SGIGIO64State *state, int x, int y)
{
    uint32_t red = sgi_gio64_clamp_color(state->curr_color_red);
    uint32_t green = sgi_gio64_clamp_color(state->curr_color_green);
    uint32_t blue = sgi_gio64_clamp_color(state->curr_color_blue);
    uint32_t alpha = sgi_gio64_clamp_color(state->curr_color_alpha);

    if (!((state->draw_mode1 >> 15) & 1)) {
        return state->color_i & 0xff;
    }
    if ((state->draw_mode1 >> 16) & 1) {
        /* Dithered RGB (MAME get_rgb_color dither path): round each
         * channel against the Bayer threshold. */
        uint32_t bayer = sgi_gio64_bayer[x & 3][y & 3];

        switch ((state->draw_mode1 >> 3) & 3) {
        case 0: {
            /* uint8_t wrap matches MAME exactly. */
            uint8_t sr = (red >> 3) - (red >> 4);
            uint8_t sg = (green >> 2) - (green >> 4);
            uint8_t sb = (blue >> 3) - (blue >> 4);
            uint32_t dr = (sr >> 4) & 1;
            uint32_t dg = (sg >> 4) & 3;
            uint32_t db = (sb >> 4) & 1;

            if ((sr & 0xf) > bayer) {
                dr++;
            }
            if ((sg & 0xf) > bayer) {
                dg++;
            }
            if ((sb & 0xf) > bayer) {
                db++;
            }
            if (dr > 1) {
                dr = 1;
            }
            if (dg > 3) {
                dg = 3;
            }
            if (db > 1) {
                db = 1;
            }
            uint32_t color = (db << 3) | (dg << 1) | dr;
            return alpha | (color << 4) | color;
        }
        case 1: {
            /* uint8_t wrap matches MAME exactly. */
            uint8_t sr = (red >> 1) - (red >> 4);
            uint8_t sg = (green >> 1) - (green >> 4);
            uint8_t sb = (blue >> 2) - (blue >> 4);
            uint32_t dr = (sr >> 4) & 7;
            uint32_t dg = (sg >> 4) & 7;
            uint32_t db = (sb >> 4) & 3;

            if ((sr & 0xf) > bayer) {
                dr++;
            }
            if ((sg & 0xf) > bayer) {
                dg++;
            }
            if ((sb & 0xf) > bayer) {
                db++;
            }
            if (dr > 7) {
                dr = 7;
            }
            if (dg > 7) {
                dg = 7;
            }
            if (db > 3) {
                db = 3;
            }
            return alpha | (db << 6) | (dg << 3) | dr;
        }
        case 2: {
            uint32_t sr = red - (red >> 4);
            uint32_t sg = green - (green >> 4);
            uint32_t sb = blue - (blue >> 4);
            uint32_t dr = (sr >> 4) & 15;
            uint32_t dg = (sg >> 4) & 15;
            uint32_t db = (sb >> 4) & 15;

            if ((sr & 0xf) > bayer) {
                dr++;
            }
            if ((sg & 0xf) > bayer) {
                dg++;
            }
            if ((sb & 0xf) > bayer) {
                db++;
            }
            if (dr > 15) {
                dr = 15;
            }
            if (dg > 15) {
                dg = 15;
            }
            if (db > 15) {
                db = 15;
            }
            uint32_t color = (db << 8) | (dg << 4) | dr;
            return alpha | (color << 12) | color;
        }
        default:
            return alpha | (blue << 16) | (green << 8) | red;
        }
    }
    if (((state->draw_mode1 >> 3) & 3) == 2) {
        return ((blue & 0xf0) << 4) | (green & 0xf0) |
               ((red & 0xf0) >> 4);
    }
    if (((state->draw_mode1 >> 3) & 3) == 3) {
        return (blue << 16) | (green << 8) | red | (alpha << 24);
    }
    return ((blue & 0xc0) | ((green & 0xe0) >> 2) |
            ((red & 0xe0) >> 5));
}

static int32_t sgi_gio64_sext_bits(uint32_t value, unsigned bits)
{
    return (int32_t)(value << (32 - bits)) >> (32 - bits);
}

static void sgi_gio64_iterate_shade(SGIGIO64State *state)
{
    /* MAME iterate_shade: slopes are 24-bit (red) / 20-bit (green, blue,
     * alpha) two's complement. Adding the raw 32-bit stored int adds
     * +8M for negative slopes and slams the channel to full within a
     * pixel or two; sign-extend first. */
    if (state->slope_red & 0x7fffff) {
        state->curr_color_red +=
            sgi_gio64_sext_bits((uint32_t)state->slope_red, 24);
    }
    if (state->slope_green & 0x7ffff) {
        state->curr_color_green +=
            sgi_gio64_sext_bits((uint32_t)state->slope_green, 20);
    }
    if (state->slope_blue & 0x7ffff) {
        state->curr_color_blue +=
            sgi_gio64_sext_bits((uint32_t)state->slope_blue, 20);
    }
    if (state->slope_alpha & 0x7ffff) {
        state->curr_color_alpha +=
            sgi_gio64_sext_bits((uint32_t)state->slope_alpha, 20);
    }
}

static uint8_t sgi_gio64_blend_index(SGIGIO64State *state, uint8_t source,
                                     uint8_t destination)
{
    uint32_t src_rgb = state->cmap_palette[0][source];
    uint32_t dst_rgb = state->cmap_palette[0][destination];
    int source_factor = (state->draw_mode1 >> 19) & 7;
    int dest_factor = (state->draw_mode1 >> 22) & 7;
    int alpha = ((state->draw_mode1 >> 27) & 1) ?
                ((src_rgb >> 24) & 0xff) : 255;
    /* Pens are 0x00RRGGBB (MAME cmap set_pen_color order). */
    int sr = (src_rgb >> 16) & 0xff;
    int sg = (src_rgb >> 8) & 0xff;
    int sb = src_rgb & 0xff;
    int dr = (dst_rgb >> 16) & 0xff;
    int dg = (dst_rgb >> 8) & 0xff;
    int db = dst_rgb & 0xff;
    int sf = source_factor == 0 ? 0 : source_factor == 1 ? 255 :
             source_factor == 2 ? dr : source_factor == 3 ? 255 - dr :
             source_factor == 4 ? alpha : 255 - alpha;
    int df = dest_factor == 0 ? 0 : dest_factor == 1 ? 255 :
             dest_factor == 2 ? sr : dest_factor == 3 ? 255 - sr :
             dest_factor == 4 ? alpha : 255 - alpha;
    int r = (sr * sf + dr * df) / 255;
    int g = (sg * sf + dg * df) / 255;
    int b = (sb * sf + db * df) / 255;

    return ((b & 0xc0) | ((g & 0xe0) >> 2) | ((r & 0xe0) >> 5));
}

/* MAME newport pixel_clip_pass verbatim: mask 0 (clip bit 0) tests the
 * pre-window coordinates against the raw registers; masks 1-4 (bits 1-4)
 * test post-window coordinates against registers minus 0x1000, passing
 * if inside at least one enabled mask. int16_t like MAME. */
static bool sgi_gio64_clip_mask0(SGIGIO64State *state, int x, int y)
{
    int16_t min_x = (int16_t)(state->screenmask_x[0] >> 16);
    int16_t min_y = (int16_t)(state->screenmask_y[0] >> 16);
    int16_t max_x = (int16_t)state->screenmask_x[0];
    int16_t max_y = (int16_t)state->screenmask_y[0];

    return x >= min_x && y >= min_y && x <= max_x && y <= max_y;
}

static bool sgi_gio64_clip_masks14(SGIGIO64State *state, int x, int y)
{
    unsigned bit;

    for (bit = 1; bit < 5; bit++) {
        int16_t min_x, min_y, max_x, max_y;

        if (!(state->clip_mode & (1U << bit))) {
            continue;
        }
        min_x = (int16_t)(state->screenmask_x[bit] >> 16) - 0x1000;
        min_y = (int16_t)(state->screenmask_y[bit] >> 16) - 0x1000;
        max_x = (int16_t)state->screenmask_x[bit] - 0x1000;
        max_y = (int16_t)state->screenmask_y[bit] - 0x1000;
        if (x >= min_x && y >= min_y && x <= max_x && y <= max_y) {
            return true;
        }
    }
    return false;
}

/* Pattern prologue (iris pixel_draw + rex3.pdf 3.8.1): ENZPATTERN and
 * ENLSPATTERN are independent. A zero bit rejects unless its opaque bit
 * substitutes COLORBACK. Written iff (LSOPAQUE|LSPAT)&(ZOPAQUE|ZPAT). */
static bool sgi_gio64_pattern_pass(SGIGIO64State *state, uint32_t mode0,
                                   bool *use_bg)
{
    *use_bg = false;
    if ((mode0 & (1U << 12)) &&
        !((state->z_pattern >> state->zpat_bit) & 1)) {
        if (mode0 & (1U << 16)) {
            *use_bg = true;
        } else {
            return false;
        }
    }
    if ((mode0 & (1U << 13)) &&
        !((state->line_stipple_pattern >> state->pat_bit) & 1)) {
        if (mode0 & (1U << 17)) {
            *use_bg = true;
        } else {
            return false;
        }
    }
    return true;
}

/* Pattern advance (iris advance_zpat/advance_lspat): ZPAT rotates 32;
 * LSPAT recirculates over LSLENGTH+17 bits with LSREPEAT-per-bit tracked
 * by live LSRCOUNT. Advances only when enabled; LSRCOUNT is register
 * state, never reset on GO. */
static void sgi_gio64_pattern_advance(SGIGIO64State *state, uint32_t mode0)
{
    if (mode0 & (1U << 12)) {
        state->zpat_bit = (state->zpat_bit - 1) & 31;
    }
    if (mode0 & (1U << 13)) {
        unsigned lsrcount = state->ls_mode & 0xff;
        unsigned lsrepeat = (state->ls_mode >> 8) & 0xff;

        if (lsrepeat == 0) {
            lsrepeat = 1;
        }
        if (lsrcount == 0) {
            unsigned length = ((state->ls_mode >> 24) & 0xf) + 17;
            unsigned wrap = 32 - length;

            state->ls_mode = (state->ls_mode & ~0xffu) | (lsrepeat - 1);
            if (state->pat_bit == wrap) {
                state->pat_bit = 31;
            } else {
                state->pat_bit = (state->pat_bit - 1) & 31;
            }
        } else {
            state->ls_mode = (state->ls_mode & ~0xffu) | (lsrcount - 1);
        }
    }
}

/* Alpha-vs-ALPHAREF compare (iris afunc): 0x7 disables. Operators
 * 0-6 are never/lt/eq/le/gt/ne/ge. MAME has no compare. */
static bool sgi_gio64_afunc(SGIGIO64State *state, unsigned src_alpha)
{
    unsigned cmp = (state->draw_mode1 >> 12) & 7;
    unsigned ref = state->alpha_ref & 0xff;

    switch (cmp) {
    case 0:
        return false;
    case 1:
        return src_alpha < ref;
    case 2:
        return src_alpha == ref;
    case 3:
        return src_alpha <= ref;
    case 4:
        return src_alpha > ref;
    case 5:
        return src_alpha != ref;
    case 6:
        return src_alpha >= ref;
    default:
        return true;
    }
}

static uint32_t sgi_gio64_rop(uint8_t func, uint32_t src, uint32_t dst)
{
    /* 16 raster ops from drawmode1[31:28] (Linux newport.h, same table as
     * MAME rb2 logic_pixel), applied to whole 32-bit RGBCI words. The ops
     * are bitwise, so the low byte of an 8-bit draw matches the old 8-bit
     * ROP exactly. */
    switch (func & 0xf) {
    case 0x0:
        return 0x000000;
    case 0x1:
        return src & dst;
    case 0x2:
        return src & ~dst;
    case 0x3:
        return src;
    case 0x4:
        return ~src & dst;
    case 0x5:
        return dst;
    case 0x6:
        return src ^ dst;
    case 0x7:
        return src | dst;
    case 0x8:
        return ~(src | dst);
    case 0x9:
        return ~(src ^ dst);
    case 0xa:
        return ~dst;
    case 0xb:
        return src | ~dst;
    case 0xc:
        return ~src;
    case 0xd:
        return ~src | dst;
    case 0xe:
        return ~(src & dst);
    default:
        return 0xffffff;
    }
}

static uint32_t sgi_gio64_expand_pixel(SGIGIO64State *state, uint32_t color)
{
    /* MAME rb2 expand_to_all_lanes: replicate by draw depth, then shift
     * by plane (overlay <<8, popup <<2, CID/RGB unshifted). */
    switch (state->plane_depth) {
    case 0: /* 4bpp */
        color |= color << 4;
        color |= color << 8;
        color |= color << 16;
        break;
    case 1: /* 8bpp */
        color |= color << 8;
        color |= color << 16;
        break;
    case 2: /* 12bpp */
        color |= color << 12;
        break;
    default: /* 24bpp */
        break;
    }
    switch (state->draw_mode1 & 7) {
    case 4: /* overlay planes */
        return color << 8;
    case 5: /* popup planes */
        return color << 2;
    default: /* RGB/CI, RGBA, CID */
        return color;
    }
}

static uint32_t sgi_gio64_blend_rgb(uint32_t src, uint32_t dst,
                                    unsigned coverage)
{
    /* Per-channel lerp used for antialiased (coverage<255) 24-bit pixels;
     * the 8-bit path keeps the legacy byte lerp in put_pixel. */
    unsigned shi = (src >> 16) & 0xff, mid = (src >> 8) & 0xff;
    unsigned slo = src & 0xff;
    unsigned dhi = (dst >> 16) & 0xff, dmg = (dst >> 8) & 0xff;
    unsigned dlo = dst & 0xff;

    shi = (shi * coverage + dhi * (255 - coverage)) / 255;
    mid = (mid * coverage + dmg * (255 - coverage)) / 255;
    slo = (slo * coverage + dlo * (255 - coverage)) / 255;
    return (shi << 16) | (mid << 8) | slo;
}

static uint32_t sgi_gio64_blend_factor(unsigned sel, unsigned c,
                                         unsigned a)
{
    switch (sel) {
    case 0:
        return 0;
    case 1:
        return 255;
    case 2:
        return c;
    case 3:
        return 255 - c;
    case 4:
        return a;
    case 5:
        return 255 - a;
    default:
        return 0;
    }
}

static uint32_t sgi_gio64_blend_rgb24(SGIGIO64State *state, uint32_t src,
                                      uint32_t dst)
{
    /* Integer blend (iris blend): Cb = Cs*Fs + Cd*Fd over 255. Words are
     * 0x00BBGGRR (byte0=R). BLENDALPHA (bit27) substitutes source-alpha
     * (else 1) for the SOURCE factor only; the destination factor keeps
     * the real alpha (iris blendalpha rule). BACKBLEND (bit25) uses
     * COLORBACK as the destination. */
    uint32_t sa_real = (src >> 24) & 0xff;
    uint32_t sa_src = (state->draw_mode1 & (1U << 27)) ? sa_real : 255;
    unsigned sfactor = (state->draw_mode1 >> 19) & 7;
    unsigned dfactor = (state->draw_mode1 >> 22) & 7;
    uint32_t out = 0;
    int i;

    if (state->draw_mode1 & (1U << 25)) {
        dst = state->color_back;
    }
    for (i = 0; i < 3; i++) {
        unsigned shift = (unsigned)i * 8;
        unsigned s_c = (src >> shift) & 0xff;
        unsigned d_c = (dst >> shift) & 0xff;
        unsigned sf = sgi_gio64_blend_factor(sfactor, d_c, sa_src);
        unsigned df = sgi_gio64_blend_factor(dfactor, s_c, sa_real);
        unsigned v = (s_c * sf + d_c * df) / 255;

        if (v > 255) {
            v = 255;
        }
        out |= v << shift;
    }
    return out;
}

static void sgi_gio64_put_pixel(SGIGIO64State *state, uint32_t *vram,
                                 uint32_t *aux,
                                 int x, int y, uint32_t color, uint32_t mask,
                                 unsigned coverage)
{
    /* MAME output_pixel applies the window offset to every write and rejects
     * pixels outside the framebuffer before any addressing. Draws with
     * draw_mode1 planes 4/5/6 (overlay/popup/CID) land in the aux plane
     * (MAME m_cidaux); all other planes land in the RGBCI word. The masked
     * store preserves the other lanes, so CI scanout (low byte) is
     * unchanged while 24-bit scanout sees full words. */
    unsigned plane;
    uint32_t *pixel;
    uint32_t dst;
    uint32_t expanded;
    uint32_t out;

    if ((state->clip_mode & 1) &&
        !sgi_gio64_clip_mask0(state, x, y)) {
        return;
    }
    /* YFLIP (iris calculate_fb_address): Y values negated, subtracted
     * from YWIN. Mask0 stays raw; masks 1-4 stay absolute and test the
     * physical coords. */
    x += (int)state->x_window - 0x1000;
    if ((state->draw_mode1 >> 6) & 1) {
        y = (int)state->y_window - y - 0x1000;
    } else {
        y += (int)state->y_window - 0x1000;
    }
    if (x < 0 || x >= XL24_STRIDE || y < 0 || y >= XL24_HEIGHT) {
        return;
    }
    if ((state->clip_mode & 0x1e) &&
        !sgi_gio64_clip_masks14(state, x, y)) {
        return;
    }
    /* CIDMATCH mask of permitted destination CIDs (iris
     * cid_allows_write): enforced for DRAW and Scr2Scr-dest, never for
     * reads. Mask 0xF is test-off. */
    if (((state->clip_mode >> 9) & 0xf) != 0xf) {
        unsigned cid = aux[y * XL24_STRIDE + x] & 3;

        if (!(((state->clip_mode >> 9) & 0xf) & (1U << cid))) {
            return;
        }
    }
    if (state->step_z & 1) {
        uint32_t *z = &state->zbuffer[y * XL24_STRIDE + x];
        if (state->z_value > *z) {
            return;
        }
        if (state->step_z & 2) {
            *z = state->z_value;
        }
    }
    plane = state->draw_mode1 & 7;
    pixel = (plane >= 4 && plane <= 6) ?
            &aux[y * XL24_STRIDE + x] : &vram[y * XL24_STRIDE + x];
    dst = *pixel;
    expanded = sgi_gio64_expand_pixel(state, color);

    if (state->draw_mode1 & (1U << 18)) {
        if (plane >= 4 && plane <= 6) {
            /* Blending aux/CID bits is undefined; keep the ROP result
             * (blended overlay draws are vanishingly rare). */
            out = sgi_gio64_rop((state->draw_mode1 >> 28) & 0xf,
                                expanded, dst);
        } else if (state->plane_depth == 3) {
            out = sgi_gio64_blend_rgb24(state, expanded, dst);
        } else {
            /* Legacy CI blend (palette-based); low byte only, matching the
             * old 8-bit store exactly. */
            out = (dst & ~0xffu) |
                  sgi_gio64_blend_index(state, (uint8_t)expanded,
                                        (uint8_t)dst);
        }
    } else {
        /* Raster op from drawmode1[31:28] (MAME logic_pixel path). */
        out = sgi_gio64_rop((state->draw_mode1 >> 28) & 0xf, expanded, dst);
    }
    if (coverage < 255 && (plane < 4 || plane > 6)) {
        if (state->plane_depth == 3) {
            out = sgi_gio64_blend_rgb(out, dst, coverage);
        } else {
            out = (dst & ~0xffu) |
                  (uint8_t)(((out & 0xff) * coverage +
                             (dst & 0xff) * (255 - coverage)) / 255);
        }
    }
    mask &= XL24_GLOBAL_MASK;
    *pixel = (dst & ~mask) | (out & mask);
    if (sgi_rowmap_enabled()) {
        sgi_row_note(state->draw_mode0, x, y, *pixel, 1);
    }
    if (sgi_pl_enabled()) {
        sgi_pl_stores++;
        if (!sgi_pl_actvalid) {
            sgi_pl_actx0 = sgi_pl_actx1 = x;
            sgi_pl_acty0 = sgi_pl_acty1 = y;
            sgi_pl_actvalid = true;
        } else {
            if (x < sgi_pl_actx0) sgi_pl_actx0 = x;
            if (x > sgi_pl_actx1) sgi_pl_actx1 = x;
            if (y < sgi_pl_acty0) sgi_pl_acty0 = y;
            if (y > sgi_pl_acty1) sgi_pl_acty1 = y;
        }
    }
}

static int sgi_seq_on = -1;
static uint64_t sgi_seq_n;
static bool sgi_seq_enabled(void)
{
    if (sgi_seq_on < 0) {
        sgi_seq_on = getenv("SGI_GIO_SEQLOG") ? 1 : 0;
    }
    return sgi_seq_on != 0;
}

static void sgi_gio64_rex3_command(SGIGIO64State *state)
{
    uint32_t mode0 = state->draw_mode0;
    uint32_t opcode = mode0 & 3;

    if (sgi_seq_enabled()) {
        fprintf(stderr, "SEQ n=%llu m0=%08x m1=%08x pd=%u zpat=%08x "
                "lspat=%08x ls=%08x xs=%d ys=%d xe=%d ye=%d xsv=%d "
                "mv=(%d,%d) hlen=%u hstg=%u hs=%u cfg=%08x "
                "win=(%d,%d) ci=%06x cv=%06x clip=%04x sm=%04x/%04x\n",
                (unsigned long long)sgi_seq_n++, mode0, state->draw_mode1,
                state->plane_depth, state->z_pattern,
                state->line_stipple_pattern, state->ls_mode,
                state->x_start_i, state->y_start_i,
                state->x_end_i, state->y_end_i, (int)state->x_save,
                state->x_move, state->y_move,
                (unsigned)state->host_len, (unsigned)state->host_staged,
                (unsigned)state->host_shift, state->draw_mode1,
                (int)state->x_window - 0x1000, (int)state->y_window - 0x1000,
                state->color_i & 0xffffff, state->color_vram & 0xffffff,
                state->clip_mode & 0x1fff,
                state->screenmask_x[0] & 0xffff, state->screenmask_y[0] & 0xffff);
    }

    /* Arm the host transfer: a loud PIO write is len == 1 (scalar path);
     * a run of HOSTRW writes since the last GO is delivered as one batched
     * transfer the draw walks (iris rex3.rs:3172-3206). */
    if (state->host_staged > 0) {
        state->host_len = state->host_staged;
        state->host_cursor = 0;
        state->host_staged = 0;
        state->host_shift = sgi_gio64_host_initial_shift(state);
    } else {
        state->host_len = 0;
        state->host_cursor = 0;
    }
    if (sgi_pl_enabled() && opcode >= 1 && opcode <= 3) {
        uint8_t flags = ((mode0 >> 6) & 1) | (((mode0 >> 12) & 1) << 1) |
                        (((mode0 >> 13) & 1) << 2) |
                        (((state->draw_mode1 >> 15) & 1) << 3) |
                        (((state->draw_mode1 >> 17) & 1) << 4) |
                        (((mode0 >> 18) & 1) << 5);

        sgi_pl_wx = (int)state->x_window - 0x1000;
        sgi_pl_wy = (int)state->y_window - 0x1000;
        sgi_pl_prevm0 = mode0;
        sgi_pl_prevm1 = state->draw_mode1;
        sgi_pl_prevpat = (mode0 & (1U << 12)) ? state->z_pattern :
                         ((mode0 & (1U << 13)) ? state->line_stipple_pattern :
                          0xffffffffu);
        sgi_pl_prevxsave = (int)state->x_save;
        sgi_pl_prevysave = 0;
        sgi_pl_prevci = state->color_i;
        sgi_pl_prevback = state->color_back;
        sgi_pl_prevvram = state->color_vram;
        /* Sampled (every 1024th RGB command): always fresh, tiny
         * volume, no cap to exhaust. Replaces the per-mode buckets. */
        {
            static uint64_t glprobe_ctr;
            bool take = sgi_glprobe_enabled() &&
                        (state->draw_mode1 & (1U << 15)) &&
                        ((glprobe_ctr++ & 1023) == 0);

            if (take) {
                fprintf(stderr, "GLPROBE op=%u am=%u m0=%08x m1=%08x "
                        "bbox=(%d,%d)-(%d,%d) fullx=(%d,%d) win=(%d,%d) "
                        "move=(%d,%d) clip=%04x sm0=%08x/%08x sm1=%08x/%08x "
                        "wmask=%06x ci=%06x cr=%08x cg=%08x cb=%08x ca=%08x "
                        "sr=%08x sg=%08x sb=%08x sa=%08x\n",
                        opcode, (mode0 >> 2) & 7, mode0, state->draw_mode1,
                        state->x_start_i, state->y_start_i,
                        state->x_end_i, state->y_end_i,
                        state->x_start, state->y_start,
                        (int)state->x_window - 0x1000,
                        (int)state->y_window - 0x1000,
                        state->x_move, state->y_move,
                        state->clip_mode & 0x1fff,
                        state->screenmask_x[0], state->screenmask_y[0],
                        state->screenmask_x[1], state->screenmask_y[1],
                        state->write_mask & 0xffffff,
                        state->color_i & 0xffffff,
                        (uint32_t)state->curr_color_red,
                        (uint32_t)state->curr_color_green,
                        (uint32_t)state->curr_color_blue,
                        (uint32_t)state->curr_color_alpha,
                        (uint32_t)state->slope_red,
                        (uint32_t)state->slope_green,
                        (uint32_t)state->slope_blue,
                        (uint32_t)state->slope_alpha);
            }
        }
        sgi_pl_note((uint8_t)opcode, (uint8_t)((mode0 >> 2) & 7),
                    (uint8_t)(state->plane_depth & 3), flags,
                    state->x_start_i, state->y_start_i,
                    state->x_end_i, state->y_end_i);
    }
    if (sgi_pl_enabled()) {
        /* Reset for this command's stores (after the deferred check). */
        sgi_pl_actvalid = false;
        sgi_pl_cmd_s0 = sgi_pl_stores;
    }
    if (sgi_fs_enabled()) {
        int i;
        sgi_fs_cmd[mode0 & 3][(mode0 >> 2) & 7]++;
        for (i = 0; i < sgi_fs_nmodes; i++) {
            if (sgi_fs_modes[i].m0 == mode0 && sgi_fs_modes[i].m1 == state->draw_mode1) {
                break;
            }
        }
        if (i == sgi_fs_nmodes && sgi_fs_nmodes < 64) {
            sgi_fs_nmodes++;
            sgi_fs_modes[i].n = 0;
            sgi_fs_modes[i].px = 0;
        }
        if (i < sgi_fs_nmodes) {
            sgi_fs_modes[i].m0 = mode0;
            sgi_fs_modes[i].m1 = state->draw_mode1;
            sgi_fs_modes[i].n++;
            sgi_fs_modes[i].px +=
                (uint64_t)(abs(state->x_end_i - state->x_start_i) + 1) *
                (abs(state->y_end_i - state->y_start_i) + 1);
        }
        if ((mode0 & 0x40) && (opcode == 2)) {
            sgi_fs_hostgos++;
            if (state->host_len > 1) {
                sgi_fs_batchgos++;
                sgi_fs_batchwords += state->host_len;
            }
        }
        if ((mode0 & ((1U << 12) | (1U << 13))) && opcode == 2) {
            int k;
            for (k = 0; k < sgi_fs_nls; k++) {
                if (sgi_fs_lsseen[k] == state->ls_mode) {
                    break;
                }
            }
            if (k == sgi_fs_nls && sgi_fs_nls < 32) {
                sgi_fs_lsseen[sgi_fs_nls++] = state->ls_mode;
                fprintf(stderr, "LSFS ls_mode=%04x zpat=%08x lspat=%08x "
                        "m0=%08x m1=%08x ystride=%d\n",
                        state->ls_mode, state->z_pattern,
                        state->line_stipple_pattern, mode0, state->draw_mode1,
                        (int)((mode0 >> 23) & 1));
            }
        }
        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - sgi_fs_last_ms > 2000) {
            sgi_fs_last_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
            sgi_fs_report();
        }
    }
    int x_start = state->x_start_i;
    int y_start = state->y_start_i;
    int x_end = state->x_end_i;
    int y_end = state->y_end_i;
    uint32_t color = state->color_i;
    uint32_t *vram = state->vram_ptr;
    uint32_t *aux = state->aux_ptr;
    uint8_t address_mode = (mode0 >> 2) & 7;
    uint32_t write_mask = state->write_mask;
    uint32_t pattern = mode0 & (1U << 12) ? state->z_pattern :
                       (mode0 & (1U << 13) ? state->line_stipple_pattern :
                        UINT32_MAX);
    /* New primitive (DOSETUP): restart pattern cursors (iris
     * execute_go). Continuation GOs keep them. */
    if (mode0 & (1U << 5)) {
        state->pat_bit = 31;
        state->zpat_bit = 31;
    }
    /* With the Setup bit (mode0[5]) set, Read/Draw/Scr2Scr recompute the
     * octant from the current coordinates first (MAME do_rex3_command). */
    if ((mode0 & (1U << 5)) &&
        (opcode == 1 || opcode == 2 ||
         (opcode == 3 && address_mode <= 1))) {
        sgi_gio64_rex3_setup(state);
    }

    if (sgi_gio_dbg()) {
        static int rex3_rows = 6000;

        if (rex3_rows > 0) {
            rex3_rows--;
            fprintf(stderr, "REX3 op=%u m0=0x%08x am=%u col=%u back=%u "
                    "mask=%06x pat=0x%08x x=%d..%d y=%d..%d win=(%d,%d) clip=0x%04x "
                    "move=(%d,%d) pd=%u vm1=0x%08x color_vram=%06x\n",
                    opcode, mode0, address_mode, state->color_i,
                    state->color_back, write_mask, pattern,
                    x_start, y_start, x_end, y_end,
                    (int)state->x_window - 0x1000,
                    (int)state->y_window - 0x1000,
                    state->clip_mode & 0x1fff,
                    state->x_move, state->y_move, state->plane_depth,
                    state->draw_mode1, state->color_vram);
        }
    }

    if (opcode == 1) {
        uint64_t data = 0;
        unsigned plane = state->draw_mode1 & 7;
        /* MAME do_pixel_read: the source address carries the window
         * offset (with YFLIP negation); reads are never clipped, only
         * bounded (iris pixel_read). */
        const uint32_t *src_plane = (plane >= 4 && plane <= 6) ? aux : vram;
        int read_row = ((state->draw_mode1 >> 6) & 1) ?
                       (int)state->y_window - y_start - 0x1000 :
                       y_start + (int)state->y_window - 0x1000;
        int read_x0 = x_start + (int)state->x_window - 0x1000;
        int read_x1 = x_end + (int)state->x_window - 0x1000;

        if (read_row < 0 || read_row >= XL24_HEIGHT) {
            state->host_dataport = 0;
            state->host_shift = 64 - sgi_gio64_host_shift(state->host_depth);
            return;
        }

        if (state->plane_depth == 3) {
            /* 24-bit: MAME read_pixel returns whole words (src shift 0 on
             * the RGB/CI plane). */
            for (int x = read_x0; x <= read_x1 && x < XL24_STRIDE; x++) {
                if (x >= 0) {
                    data = (data << 32) |
                           src_plane[read_row * XL24_STRIDE + x];
                }
            }
        } else {
            for (int x = read_x0; x <= read_x1 && x < XL24_STRIDE; x++) {
                if (x >= 0) {
                    data = (data << 8) |
                           (uint8_t)src_plane[read_row * XL24_STRIDE + x];
                }
            }
        }
        state->host_dataport = data;
        state->host_shift = 64 - sgi_gio64_host_shift(state->host_depth);
        return;
    }
    if (opcode == 3 && address_mode <= 1) {
        /* Scr2Scr copy (MAME do_rex3_command case 3). Source and dest live
         * in the plane selected by draw_mode1 (RGBCI or aux/CID); the
         * window offset applies to source and dest, like MAME's
         * set_address + output_pixel pair. The snapshot covers only the
         * source rect (not the whole store) and preserves it for
         * overlapping copies. Terminates like iris: a pass without
         * stop_on_x ends the whole copy (single row/pixel). */
        int x_step = (state->bres_octant_inc1 >> 25) & 1 ? -1 : 1;
        int y_step = (state->bres_octant_inc1 >> 24) & 1 ? -1 : 1;
        int win_x = (int)state->x_window - 0x1000;
        int win_y = (int)state->y_window - 0x1000;
        bool stop_on_x = (mode0 >> 8) & 1;
        bool stop_on_y = (mode0 >> 9) & 1;
        int start_x = x_start;
        int start_y = y_start;
        int end_x = x_end + x_step;
        int end_y = y_end + y_step;
        unsigned plane = state->draw_mode1 & 7;
        const uint32_t *src_plane =
            (plane >= 4 && plane <= 6) ? aux : vram;
        int sx0 = MIN(x_start, x_end) + win_x;
        int sx1 = MAX(x_start, x_end) + win_x;
        /* Source read uses the same YFLIP negation as writes (iris
         * calculate_src_address). */
        int sy0 = ((state->draw_mode1 >> 6) & 1) ?
                  (int)state->y_window - MAX(y_start, y_end) - 0x1000 :
                  MIN(y_start, y_end) + win_y;
        int sy1 = ((state->draw_mode1 >> 6) & 1) ?
                  (int)state->y_window - MIN(y_start, y_end) - 0x1000 :
                  MAX(y_start, y_end) + win_y;
        int sw = 0, sh = 0;
        uint32_t *copy = NULL;
        /* Opaque row copies bypass per-pixel ROP/mask/clip/Z (the snapshot
         * already froze the source, like MAME). Rows restart at x_save, so
         * the fast rect needs x_save == x_start. */
        bool row_copy = ((state->draw_mode1 >> 28) & 0xf) == 3 &&
                        !(state->draw_mode1 & (1U << 18)) &&
                        (write_mask & XL24_GLOBAL_MASK) == XL24_GLOBAL_MASK &&
                        !(state->clip_mode & 0x1f) &&
                        (((state->clip_mode >> 9) & 0xf) == 0xf) &&
                        !(state->step_z & 1) &&
                        stop_on_x && stop_on_y &&
                        state->x_save == x_start &&
                        (x_end - x_start) * x_step >= 0 &&
                        (y_end - y_start) * y_step >= 0;

        if (sx1 >= 0 && sy1 >= 0 && sx0 < XL24_STRIDE &&
            sy0 < XL24_HEIGHT) {
            size_t need;

            sx0 = MAX(sx0, 0);
            sy0 = MAX(sy0, 0);
            sx1 = MIN(sx1, XL24_STRIDE - 1);
            sy1 = MIN(sy1, XL24_HEIGHT - 1);
            sw = sx1 - sx0 + 1;
            sh = sy1 - sy0 + 1;
            need = (size_t)sw * sh;
            if (need > state->blt_len) {
                state->blt_buf = g_realloc(state->blt_buf,
                                           need * sizeof(*copy));
                state->blt_len = need;
            }
            copy = state->blt_buf;
            for (int yy = 0; yy < sh; yy++) {
                memcpy(&copy[(size_t)yy * sw],
                       &src_plane[(size_t)(sy0 + yy) * XL24_STRIDE + sx0],
                       (size_t)sw * sizeof(*copy));
            }
        }
        /* Only the moved-to rect changes pixels (rows restart at x_save). */
        sgi_gio64_damage_rect(state,
                              MIN(MIN(start_x, x_end), (int)state->x_save) +
                              state->x_move,
                              MIN(start_y, y_end) + state->y_move,
                              MAX(MAX(start_x, x_end), (int)state->x_save) +
                              state->x_move,
                              MAX(start_y, y_end) + state->y_move);
        if (row_copy) {
            uint32_t *dst_plane =
                (plane >= 4 && plane <= 6) ? aux : vram;

            if (!copy) {
                /* Off-screen source still paints dest with zeros below. */
                sgi_gio64_fb_dirty(state, plane >= 4 && plane <= 6,
                                   MIN(MIN(start_x, x_end),
                                       (int)state->x_save) + state->x_move,
                                   MIN(start_y, y_end) + state->y_move,
                                   MAX(MAX(start_x, x_end),
                                       (int)state->x_save) + state->x_move,
                                   MAX(start_y, y_end) + state->y_move);
                for (int yy = MIN(y_start, y_end) + state->y_move;
                     yy <= MAX(y_start, y_end) + state->y_move; yy++) {
                    int dy = yy + win_y;
                    int dx0 = MIN(x_start, x_end) + state->x_move + win_x;
                    int dx1 = MAX(x_start, x_end) + state->x_move + win_x;

                    if (dy < 0 || dy >= XL24_HEIGHT) {
                        continue;
                    }
                    dx0 = MAX(dx0, 0);
                    dx1 = MIN(dx1, XL24_STRIDE - 1);
                    if (dx0 <= dx1) {
                        memset(&dst_plane[(size_t)dy * XL24_STRIDE + dx0], 0,
                               (size_t)(dx1 - dx0 + 1) * sizeof(*dst_plane));
                    }
                }
            } else {
                /* Full destination span (draw coords); source pixels
                 * outside the snapshot read back as zeros below. */
                int fy0 = MIN(y_start, y_end) + state->y_move;
                int fy1 = MAX(y_start, y_end) + state->y_move;
                int fx0 = MIN(x_start, x_end) + state->x_move;
                int fx1 = MAX(x_start, x_end) + state->x_move;

                sgi_gio64_fb_dirty(state, plane >= 4 && plane <= 6,
                                   fx0, fy0, fx1, fy1);
                for (int yy = fy0; yy <= fy1; yy++) {
                    int dy = yy + win_y;
                    int srow = dy - state->y_move;
                    int cd0 = MAX(fx0 + win_x, 0);
                    int cd1 = MIN(fx1 + win_x, XL24_STRIDE - 1);

                    if (dy < 0 || dy >= XL24_HEIGHT || cd0 > cd1) {
                        continue;
                    }
                    if (srow >= sy0 && srow <= sy1) {
                        /* Source row is live: copy the overlap with the
                         * snapshot, zero the fringes (generic loop reads
                         * zeros outside the source rect). */
                        int ov0 = MAX(sx0 + state->x_move, cd0);
                        int ov1 = MIN(sx1 + state->x_move, cd1);

                        if (ov0 > ov1) {
                            memset(&dst_plane[(size_t)dy * XL24_STRIDE + cd0],
                                   0, (size_t)(cd1 - cd0 + 1) *
                                   sizeof(*dst_plane));
                        } else {
                            if (ov0 > cd0) {
                                memset(&dst_plane[(size_t)dy * XL24_STRIDE +
                                                  cd0], 0,
                                       (size_t)(ov0 - cd0) *
                                       sizeof(*dst_plane));
                            }
                            memcpy(&dst_plane[(size_t)dy * XL24_STRIDE + ov0],
                                   &copy[(size_t)(srow - sy0) * sw +
                                         (ov0 - state->x_move - sx0)],
                                   (size_t)(ov1 - ov0 + 1) *
                                   sizeof(*dst_plane));
                            if (ov1 < cd1) {
                                memset(&dst_plane[(size_t)dy * XL24_STRIDE +
                                                  ov1 + 1], 0,
                                       (size_t)(cd1 - ov1) *
                                       sizeof(*dst_plane));
                            }
                        }
                    } else {
                        memset(&dst_plane[(size_t)dy * XL24_STRIDE + cd0], 0,
                               (size_t)(cd1 - cd0 + 1) * sizeof(*dst_plane));
                    }
                }
            }
            if (sgi_pl_enabled()) {
                static int sgi_pl_op3logged;
                int _bw = abs(x_end - x_start) + 1;
                int _bh = abs(y_end - y_start) + 1;

                if (sgi_pl_op3logged < 2000 && _bw * _bh <= 4000) {
                    sgi_pl_op3logged++;
                    fprintf(stderr, "OP3SMALL m0=%08x m1=%08x "
                            "bbox=(%d,%d)-(%d,%d) fast=1\n",
                            mode0, state->draw_mode1,
                            x_start, y_start, x_end, y_end);
                }
            }
            sgi_gio64_write_x_start(state, state->x_save << 11);
            sgi_gio64_write_y_start(state, (y_end + y_step) << 11);
            return;
        }
        do {
            do {
                int src_x = start_x + win_x;
                int src_y = ((state->draw_mode1 >> 6) & 1) ?
                            (int)state->y_window - start_y - 0x1000 :
                            start_y + win_y;
                uint32_t src = 0;

                if (copy && src_x >= sx0 && src_x <= sx1 &&
                    src_y >= sy0 && src_y <= sy1) {
                    src = copy[(size_t)(src_y - sy0) * sw + (src_x - sx0)];
                }
                sgi_gio64_put_pixel(state, vram, aux,
                                    start_x + state->x_move,
                                    start_y + state->y_move,
                                    src, write_mask, 255);
                start_x += x_step;
            } while (start_x != end_x && stop_on_x);
            if (start_x == end_x) {
                start_x = state->x_save;
                start_y += y_step;
            }
            if (!stop_on_x) {
                break;
            }
        } while (start_y != end_y && stop_on_y);
        if (sgi_pl_enabled()) {
            static int sgi_pl_op3slogged;
            int _bw = abs(x_end - x_start) + 1;
            int _bh = abs(y_end - y_start) + 1;

            if (sgi_pl_op3slogged < 2000 && _bw * _bh <= 4000) {
                sgi_pl_op3slogged++;
                fprintf(stderr, "OP3SMALL m0=%08x m1=%08x "
                        "bbox=(%d,%d)-(%d,%d) fast=0\n",
                        mode0, state->draw_mode1,
                        x_start, y_start, x_end, y_end);
            }
        }
        /* Snapshot lives in the reusable scratch buffer. */
        sgi_gio64_write_x_start(state, start_x << 11);
        sgi_gio64_write_y_start(state, start_y << 11);
        sgi_gio64_fb_dirty(state, plane >= 4 && plane <= 6,
                           MIN(MIN(x_start, x_end), (int)state->x_save) +
                           state->x_move,
                           MIN(y_start, y_end) + state->y_move,
                           MAX(MAX(x_start, x_end), (int)state->x_save) +
                           state->x_move,
                           MAX(y_start, y_end) + state->y_move);
        return;
    }
    /* Opcode 0 is NoOp (pipeline sync); opcodes 1/3 handled above. */
    if (opcode != 2) {
        return;
    }
    if (mode0 & (1U << 18)) {
        color = sgi_gio64_shaded_color(state, x_start, y_start);
    }

    /* Address modes 5-7 are unimplemented in MAME too; log and skip so any
     * future use is visible instead of silently corrupt. */
    if (address_mode >= 5) {
        qemu_log_mask(LOG_UNIMP, "sgi-gio64: unimplemented REX3 draw "
                      "address mode %u\n", address_mode);
        return;
    }

    if (address_mode >= 2 && address_mode <= 4) {
        int x = x_start;
        int y = y_start;
        int dx = abs((int)x_end - x);
        int sx = x < (int)x_end ? 1 : -1;
        int dy = -abs((int)y_end - y);
        int sy = y < (int)y_end ? 1 : -1;
        int error = dx + dy;
        bool skip_first = mode0 & (1U << 10);
        bool skip_last = mode0 & (1U << 11);
        /* Both stop bits clear draws one pixel at start and steps once
         * (MAME do_iline/do_fline); without it a single-pixel Draw paints
         * a full-screen diagonal. */
        bool iterate_one = ((mode0 >> 8) & 3) == 0;
        unsigned point = 0;

        if (iterate_one) {
            if (mode0 & (1U << 6)) {
                color = sgi_gio64_host_color(state);
            } else if ((mode0 & (1U << 18)) ||
                       ((state->draw_mode1 >> 15) & 1)) {
                /* MAME do_iline/do_fline: shaded or RGB lines use the
                 * assembled DDA color, not the (possibly stale)
                 * color_i. No iteration without SHADE. */
                color = sgi_gio64_shaded_color(state, x, y);
            }
            unsigned coverage = 255;
            if (address_mode == 4) {
                int span = MAX(dx, -dy) + 1;
                coverage = 128 + (unsigned)(abs(error) % MAX(span, 1)) *
                           127 / MAX(span, 1);
                coverage = MIN(coverage, 255U);
            }
            /* Lines test patterns like every other primitive (iris pixel
             * path; LSPATTERN is "used mainly for lines"). */
            bool use_bg = false;
            if (sgi_gio64_pattern_pass(state, mode0, &use_bg)) {
                unsigned alpha = (mode0 & (1U << 6)) &&
                                 (mode0 & (1U << 7)) ?
                                 (color >> 24) & 0xff :
                                 sgi_gio64_clamp_color(
                                     state->curr_color_alpha);

                if (sgi_gio64_afunc(state, alpha)) {
                    sgi_gio64_put_pixel(state, vram, aux, x, y,
                                        use_bg ? state->color_back : color,
                                        write_mask, coverage);
                }
            }
            sgi_gio64_pattern_advance(state, mode0);
            /* Single stepped pixel plus its endpoint (MAME steps once). */
            sgi_gio64_damage_rect(state, MIN(x, (int)x_end),
                                  MIN(y, (int)y_end),
                                  MAX(x, (int)x_end), MAX(y, (int)y_end));
            if (mode0 & (1U << 18)) {
                sgi_gio64_iterate_shade(state);
            }
            if (!(x == (int)x_end && y == (int)y_end)) {
                int twice = 2 * error;
                if (twice >= dy) {
                    error += dy;
                    x += sx;
                }
                if (twice <= dx) {
                    error += dx;
                    y += sy;
                }
            }
            sgi_gio64_write_x_start(state, x << 11);
            sgi_gio64_write_y_start(state, y << 11);
            sgi_gio64_fb_dirty(state, (state->draw_mode1 & 7) >= 4 &&
                               (state->draw_mode1 & 7) <= 6,
                               MIN(x, (int)x_end), MIN(y, (int)y_end),
                               MAX(x, (int)x_end), MAX(y, (int)y_end));
            return;
        }

        /* Bresenham walks stay inside the endpoint bbox. */
        sgi_gio64_damage_rect(state, x_start, y_start, x_end, y_end);
        for (;;) {
            bool last = x == (int)x_end && y == (int)y_end;
            if ((!skip_first || point != 0) && (!last || !skip_last)) {
                if (mode0 & (1U << 6)) {
                    color = sgi_gio64_host_color(state);
                } else if ((mode0 & (1U << 18)) ||
                           ((state->draw_mode1 >> 15) & 1)) {
                    /* MAME do_iline/do_fline: shaded or RGB lines use
                     * the assembled DDA color. Iteration below only
                     * runs with SHADE set. */
                    color = sgi_gio64_shaded_color(state, x, y);
                }
                unsigned coverage = 255;
                bool use_bg = false;
                if (address_mode == 4) {
                    int span = MAX(dx, -dy) + 1;
                    coverage = 128 + (unsigned)(abs(error) % MAX(span, 1)) *
                               127 / MAX(span, 1);
                    coverage = MIN(coverage, 255U);
                }
                if (sgi_gio64_pattern_pass(state, mode0, &use_bg)) {
                    unsigned alpha = (mode0 & (1U << 6)) &&
                                     (mode0 & (1U << 7)) ?
                                     (color >> 24) & 0xff :
                                     sgi_gio64_clamp_color(
                                         state->curr_color_alpha);

                    if (sgi_gio64_afunc(state, alpha)) {
                        sgi_gio64_put_pixel(state, vram, aux, x, y,
                                            use_bg ? state->color_back :
                                            color,
                                            write_mask, coverage);
                    }
                }
                if (mode0 & (1U << 18)) {
                    sgi_gio64_iterate_shade(state);
                    color = sgi_gio64_shaded_color(state, x, y);
                }
            }
            sgi_gio64_pattern_advance(state, mode0);
            if (last) {
                break;
            }
            int twice = 2 * error;
            if (twice >= dy) {
                error += dy;
                x += sx;
            }
            if (twice <= dx) {
                error += dx;
                y += sy;
            }
            point++;
        }
        /* MAME updates start to the end, so the next Setup-derived octant
         * sees fresh coordinates instead of a stale start. */
        sgi_gio64_write_x_start(state, x << 11);
        sgi_gio64_write_y_start(state, y << 11);
        sgi_gio64_fb_dirty(state, (state->draw_mode1 & 7) >= 4 &&
                           (state->draw_mode1 & 7) <= 6,
                           x_start, y_start, x_end, y_end);
        return;
    }
    if (address_mode > 1) {
        return;
    }

    int x_step = (state->bres_octant_inc1 >> 25) & 1 ? -1 : 1;
    int y_step = (state->bres_octant_inc1 >> 24) & 1 ? -1 : 1;
    /* Iris: YSTRIDE (mode0 bit23) steps rows by 2 for block fills. */
    if ((mode0 >> 23) & 1) {
        y_step *= 2;
    }
    /* Default color (MAME get_default_color): fast-clear sources the VRAM
     * color, with plane-depth replication. Kept whole (not truncated):
     * 24-bit fills need all three bytes. FASTCLEAR honored only with CID
     * checking off and non-host source (iris + rex3.pdf 3.5.5). */
    bool cid_off = ((state->clip_mode >> 9) & 0xf) == 0xf;
    bool fastclear = ((state->draw_mode1 >> 17) & 1) && cid_off &&
                     !((mode0 >> 6) & 1);
    uint32_t base = fastclear ? state->color_vram : state->color_i;
    bool shade = (mode0 >> 18) & 1;
    bool rgbmode = (state->draw_mode1 >> 15) & 1;
    /* Iteration cap: guarantees termination even if a stale octant points
     * away from the end (MAME shares the bare shape; iris bounds its
     * walkers). Generous enough to never trigger on legitimate fills. */
    unsigned long iter_budget = 4UL * XL24_STRIDE * XL24_HEIGHT;

    switch (state->plane_depth) {
    case 0:
        base = (base & 0xf) * 0x11111111U;
        break;
    case 1:
        base = (base & 0xff) * 0x01010101U;
        break;
    case 2:
        if ((state->draw_mode1 >> 15) & 1) {
            base = ((state->color_vram & 0xf00000) >> 12) |
                   ((state->color_vram & 0xf000) >> 8) |
                   ((state->color_vram & 0xf0) >> 4);
        } else {
            base &= 0xfff;
        }
        base |= base << 12;
        break;
    default:
        base &= 0xffffff;
        break;
    }

    if (opcode == 2 && (mode0 & 0x40) && sgi_cmdt_enabled() && x_start == 0 &&
        y_start < 600 && sgi_cmdt_n < 1000) {
        fprintf(stderr, "CMDT-E n=%llu x=%d y=%d xe=%d ye=%d xs=%d m1=%08x "
                "hl=%d hs=%d st=%d pd=%u dp=%016llx wx=%d wy=%d\n",
                (unsigned long long)sgi_cmdt_n++, x_start, y_start, x_end,
                y_end, (int)state->x_save, state->draw_mode1, state->host_len,
                state->host_shift, state->host_staged, state->plane_depth,
                (unsigned long long)state->host_dataport,
                (int)state->x_window - 0x1000, (int)state->y_window - 0x1000);
    }
    /*
     * Block/span fill (MAME Draw/Block case): inclusive end extended by one
     * step, rows restart from x_save, loops gated by stop_on_x/stop_on_y.
     * Per pixel (MAME): shade-iterate, then host color, else shaded color
     * (unless fast-clear), else the default. Out-of-range rows rely on
     * put_pixel's per-pixel clip (like MAME output_pixel).
     */
    bool stop_on_x = (mode0 >> 8) & 1;
    bool stop_on_y = (mode0 >> 9) & 1;
    int prim_end_x = x_end + x_step;

    /* Iris: length32 clamps spans >= 32 wide, symmetric in direction
     * (MAME's block check lacks abs and is one-sided). */
    if ((mode0 >> 15) & 1) {
        if (abs(x_end - x_start) >= 32) {
            prim_end_x = x_start + 32 * x_step;
        }
    }
    /* Packed host blits consume whole dataports: a loud PIO write is one
     * word, a batched run is host_len words, so the row must stop after
     * that many pixels even when stop_on_x is clear. Bounding by the run
     * (not the corner bbox) is what keeps a batched GO from re-drawing the
     * rest of the row from the stale last word. */
    /* MAME newport.cpp:3358-3366,3430-3438; iris rex3_generic.rs:1484-1488 */
    bool host_packed = (mode0 & (1U << 6)) && ((state->draw_mode1 >> 7) & 1);

    if (host_packed) {
        static const int16_t max_host_lengths[2][4] =
            { { 4, 4, 2, 1 }, { 8, 8, 4, 2 } };
        int per_word = max_host_lengths[(state->draw_mode1 >> 10) & 1]
                                       [(state->draw_mode1 >> 8) & 3];
        int words = state->host_len > 1 ? (int)state->host_len : 1;
        int max_length = per_word * words;
        int length = abs(prim_end_x - x_start);

        stop_on_x = true;
        if (length > max_length) {
            prim_end_x = x_start + x_step * max_length;
        }
    }
    /* MAME lr_abort (mode0 bit19 with negative dx): draw nothing. */
    if (((mode0 >> 19) & 1) && x_step < 0) {
        return;
    }
    /* Fills stay inside the corner bbox (rows restart at x_save). A host
     * blit only touches the clamped transfer span, so bound its damage to
     * that instead of the (often full-screen) bbox. */
    if (((mode0 >> 6) & 1) && ((state->draw_mode1 >> 7) & 1)) {
        sgi_gio64_damage_rect(state, MIN(x_start, prim_end_x),
                              MIN(y_start, y_end),
                              MAX(x_start, prim_end_x), MAX(y_start, y_end));
    } else {
        sgi_gio64_damage_rect(state,
                              MIN(MIN(x_start, x_end), (int)state->x_save),
                              MIN(y_start, y_end),
                              MAX(MAX(x_start, x_end), (int)state->x_save),
                              MAX(y_start, y_end));
    }
    /* Inclusive end, extended by one step like MAME. */
    x_end += x_step;
    y_end += y_step;

    /*
     * Flat-fill fast path: opaque replace, no blend/shade/host/stipple,
     * no clip or Z, single constant color, rows restart from x_start.
     * Inlines the masked store instead of per-pixel put_pixel calls.
     * Rows outside the store are skipped (iris drops them in
     * calculate_fb_address, iris rex3_generic.rs:663); the loop still
     * steps through them so end state matches the slow loop.
     */
    if (((state->draw_mode1 >> 28) & 0xf) == 3 &&
        !(state->draw_mode1 & (1U << 18)) &&
        !(mode0 & ((1U << 18) | (1U << 6))) &&
        !(mode0 & ((1U << 12) | (1U << 13))) &&
        !(state->clip_mode & 0x1f) && cid_off && !(state->step_z & 1) &&
        ((state->draw_mode1 >> 12) & 7) == 7 &&
        !((state->draw_mode1 >> 16) & 1) &&
        state->x_save == x_start && stop_on_x &&
        !((mode0 >> 15) & 1) &&
        (state->draw_mode1 & 7) <= 6) {
        int win_x = (int)state->x_window - 0x1000;
        int win_y = (int)state->y_window - 0x1000;
        /* Inclusive pixel ranges (x_end/y_end are exclusive here). */
        int rx0 = x_step > 0 ? x_start : x_end - x_step;
        int rx1 = x_step > 0 ? x_end - x_step : x_start;
        int ry0 = y_step > 0 ? y_start : y_end - y_step;
        int ry1 = y_step > 0 ? y_end - y_step : y_start;
        unsigned fplane = state->draw_mode1 & 7;
        bool faux = fplane >= 4 && fplane <= 6;
        uint32_t *ffb = faux ? aux : vram;
        uint32_t fmask = write_mask & XL24_GLOBAL_MASK;
        uint32_t fcol = base;
        /* Visible columns (a constant fill covers the same columns on
         * every row, so one clamped range serves all rows). */
        int cx0 = MAX(rx0 + win_x, 0);
        int cx1 = MIN(rx1 + win_x, XL24_STRIDE - 1);
        int fy = y_start;

        /* MAME rgbmode-flat (no shade iteration) uses the assembled
         * shaded color; base already holds the depth-replicated word.
         * Fast-clear sources COLORVRAM, not the (possibly stale) shade
         * DDA. Aux/CID lanes sit shifted inside the word (iris
         * rex3_generic.rs:547); run the same expander put_pixel uses
         * so the store matches the slow loop exactly. */
        if (((state->draw_mode1 >> 17) & 1)) {
            fcol = base;
        } else if ((state->draw_mode1 >> 15) & 1) {
            fcol = sgi_gio64_shaded_color(state, x_start, y_start);
            if (state->plane_depth != 3) {
                fcol = sgi_gio64_expand_pixel(state, fcol);
            }
        }
        if (faux && !((state->draw_mode1 >> 15) & 1 &&
                      state->plane_depth != 3)) {
            fcol = sgi_gio64_expand_pixel(state, fcol);
        }
        fcol &= 0x00ffffff;
        if (rx0 <= rx1 && ry0 <= ry1) {
            int row_w = cx1 - cx0 + 1;

            do {
                int dy = fy + win_y;

                if (dy >= 0 && dy < XL24_HEIGHT && row_w > 0) {
                    uint32_t *dst =
                        &ffb[(size_t)dy * XL24_STRIDE + cx0];
                    int i;

                    if (fmask == 0x00ffffffu) {
                        /* Full-word store, no readback. Byte-replicated
                         * colors (8-bit CI fills) go through memset. */
                        uint8_t b = fcol & 0xff;

                        if (fcol == (uint32_t)b * 0x010101u) {
                            memset(dst, b,
                                   (size_t)row_w * sizeof(*dst));
                        } else {
                            for (i = 0; i < row_w; i++) {
                                dst[i] = fcol;
                            }
                        }
                    } else {
                        /* Read-modify-write (preserves lanes outside the
                         * mask, matching the slow path exactly). */
                        for (i = 0; i < row_w; i++) {
                            uint32_t d = dst[i];

                            dst[i] = (d & ~fmask) | (fcol & fmask);
                        }
                    }
                    if (sgi_rowmap_enabled()) {
                        sgi_row_note(state->draw_mode0, cx0, dy, dst[0], row_w);
                    }
                    if (sgi_pl_enabled()) {
                        sgi_pl_stores += (uint64_t)row_w;
                        if (!sgi_pl_actvalid) {
                            sgi_pl_actx0 = cx0; sgi_pl_actx1 = cx1;
                            sgi_pl_acty0 = sgi_pl_acty1 = dy;
                            sgi_pl_actvalid = true;
                        } else {
                            if (cx0 < sgi_pl_actx0) sgi_pl_actx0 = cx0;
                            if (cx1 > sgi_pl_actx1) sgi_pl_actx1 = cx1;
                            if (dy < sgi_pl_acty0) sgi_pl_acty0 = dy;
                            if (dy > sgi_pl_acty1) sgi_pl_acty1 = dy;
                        }
                    }
                }
                fy += address_mode == 1 ? y_step : 0;
            } while (fy != y_end && stop_on_y && address_mode == 1);
            if (sgi_fs_enabled()) {
                sgi_fs_fast++;
                sgi_fs_fastpx += (uint64_t)(rx1 - rx0 + 1) *
                                 (uint64_t)(ry1 - ry0 + 1);
            }
            /* End state matches the slow loop (x restarted to x_save). */
            sgi_gio64_write_x_start(state, state->x_save << 11);
            sgi_gio64_write_y_start(state, fy << 11);
            sgi_gio64_fb_dirty(state, faux, rx0, ry0, rx1, ry1);
            return;
        }
    }

    if (sgi_pl_enabled() && opcode == 2 && ((mode0 >> 12) & 1) &&
        sgi_pl_looplogged < 5000) {
        sgi_pl_looplogged++;
        fprintf(stderr, "LOOP xs=%d xe=%d ys=%d ye=%d xstep=%d ystep=%d "
                "stopx=%d stopy=%d pe=%d pat=%08x xsave=%d\n",
                x_start, x_end, y_start, y_end, x_step, y_step,
                stop_on_x ? 1 : 0, stop_on_y ? 1 : 0, prim_end_x,
                pattern, (int)state->x_save);
    }
    int x = x_start;
    int y = y_start;
    /* Iris draw_block_g: the shade DDA runs continuously across rows
     * (no per-row base reload). The pattern cursors are NOT restarted
     * here: iris resets them only on DOSETUP (execute_go) and when a row
     * completes (rex3_generic.rs:1434). Restarting at every row top made
     * each one-word-per-GO host fill re-sample the pattern from bit 31,
     * so the same pattern bits landed on the same slot of every 8-pixel
     * word and painted fixed 2px vertical stripes. */
    do {
        do {
            /* No draw-coords range guard (MAME/iris call the pixel body
             * unconditionally): x is pre-window, so a nonzero window
             * offset makes on-screen pixels look out of range and drops
             * them. put_pixel clips the physical coords itself. */
            if (sgi_fs_enabled()) {
                if (mode0 & (1U << 6)) {
                    sgi_fs_hostpx++;
                } else {
                    sgi_fs_slowpx++;
                }
            }
            bool use_bg;

            if (sgi_gio64_pattern_pass(state, mode0, &use_bg)) {
                if (use_bg) {
                    if (sgi_gio64_afunc(state,
                                        (state->color_back >> 24) & 0xff)) {
                        sgi_gio64_put_pixel(state, vram, aux, x, y,
                                            state->color_back, write_mask,
                                            255);
                    }
                } else if (mode0 & (1U << 6)) {
                    /* Host pixel consumed on pattern-pass exactly like
                     * MAME/iris (keeps the FIFO in sync even when the
                     * compare below rejects). */
                    color = sgi_gio64_host_color(state);
                    unsigned alpha = (mode0 & (1U << 7)) ?
                                     (color >> 24) & 0xff :
                                     sgi_gio64_clamp_color(
                                         state->curr_color_alpha);

                    if (sgi_gio64_afunc(state, alpha)) {
                        sgi_gio64_put_pixel(state, vram, aux, x, y, color,
                                            write_mask, 255);
                    }
                } else {
                    uint32_t c = base;

                    if ((shade || rgbmode) && !fastclear) {
                        if (shade) {
                            sgi_gio64_iterate_shade(state);
                        }
                        c = sgi_gio64_shaded_color(state, x, y);
                    }
                    if (sgi_gio64_afunc(state, sgi_gio64_clamp_color(
                                                state->curr_color_alpha))) {
                        sgi_gio64_put_pixel(state, vram, aux, x, y, c,
                                            write_mask, 255);
                    }
                }
            }
            sgi_gio64_pattern_advance(state, mode0);
            x += x_step;
            if (--iter_budget == 0) {
                qemu_log_mask(LOG_UNIMP, "sgi-gio64: span/block budget "
                              "exhausted, aborting fill\n");
                /* Still publish end state + dirty so continuation
                 * coordinates don't go stale for the next primitive. */
                sgi_gio64_write_x_start(state, x << 11);
                sgi_gio64_write_y_start(state, y << 11);
                sgi_gio64_fb_dirty(state, (state->draw_mode1 & 7) >= 4 &&
                                   (state->draw_mode1 & 7) <= 6,
                                   MIN(MIN(x_start, x_end),
                                       (int)state->x_save),
                                   MIN(y_start, y_end),
                                   MAX(MAX(x_start, x_end),
                                       (int)state->x_save),
                                   MAX(y_start, y_end));
                return;
            }
        } while (x != prim_end_x && x != x_end && stop_on_x);
        if ((x_step > 0 && x >= x_end) || (x_step < 0 && x <= x_end)) {
            x = state->x_save;
            y += address_mode == 1 ? y_step : 0;
            /* Iris draw_block_g (rex3_generic.rs:1434-1435): the pattern
             * cursors restart at bit 31 when, and only when, a row
             * completes. lsrcount (LSMODE bits 7:0) keeps running. */
            if (address_mode == 1) {
                state->pat_bit = 31;
                state->zpat_bit = 31;
            }
            /* Iris draw_block_g (rex3_generic.rs:1471-1473): a partial host
             * word at a row boundary ends the primitive instead of being
             * carried (stale) into the next row. Continuing re-reads the
             * drained shifter and smears columns down the rectangle. */
            if (((mode0 >> 6) & 1) && ((state->draw_mode1 >> 7) & 1) &&
                state->host_len <= 1 &&
                state->host_shift != sgi_gio64_host_initial_shift(state)) {
                break;
            }
        } else if (!stop_on_x) {
            /* Single-pixel row: draw once and stop instead of spinning. */
            break;
        } else {
            break;
        }
    } while (y != y_end && stop_on_y && address_mode == 1);
    if (sgi_pl_enabled()) {
        uint64_t st = sgi_pl_stores;
        int wx = (int)state->x_window - 0x1000;
        int wy = (int)state->y_window - 0x1000;
        int ix0 = x_start + wx, ix1 = x_end + wx;
        int iy0 = y_start + wy, iy1 = y_end + wy;
        int nx0 = ix0 < ix1 ? ix0 : ix1, nx1 = ix0 < ix1 ? ix1 : ix0;
        int ny0 = iy0 < iy1 ? iy0 : iy1, ny1 = iy0 < iy1 ? iy1 : iy0;

        /* st is since boot; compare vs snapshot at command top. */
        uint64_t cst = st - sgi_pl_cmd_s0;

        /* Sampled (every 256th): identical drops repeat by the
         * thousand; counting stays exact, printing stays small. */
        static uint64_t synczero_ctr;
        if (cst == 0 && ((synczero_ctr++ & 255) == 0)) {
            fprintf(stderr, "SYNCZERO m0=%08x bbox=(%d,%d)-(%d,%d) pat=%08x "
                    "clip=%04x stepz=%08x sm0=%08x/%08x sm1=%08x/%08x\n",
                    mode0, nx0, ny0, nx1, ny1, pattern,
                    state->clip_mode & 0x1fff, state->step_z,
                    state->screenmask_x[0], state->screenmask_y[0],
                    state->screenmask_x[1], state->screenmask_y[1]);
        } else if (sgi_pl_actvalid &&
                   (sgi_pl_actx1 < nx0 || sgi_pl_actx0 > nx1 ||
                    sgi_pl_acty1 < ny0 || sgi_pl_acty0 > ny1)) {
            fprintf(stderr, "SYNCMIS m0=%08x want=(%d,%d)-(%d,%d) "
                    "got=(%d,%d)-(%d,%d) pat=%08x\n",
                    mode0, nx0, ny0, nx1, ny1,
                    sgi_pl_actx0, sgi_pl_acty0, sgi_pl_actx1, sgi_pl_acty1,
                    pattern);
        } else if (cst > 0 && cst <= 2000 &&
                   (nx1 - nx0 + 1) * (ny1 - ny0 + 1) <= 4000) {
            static int sgi_pl_oklogged;

            if (sgi_pl_oklogged < 2000) {
                sgi_pl_oklogged++;
                fprintf(stderr, "SMALLOK op=2 m0=%08x m1=%08x "
                        "bbox=(%d,%d)-(%d,%d) stores=%llu\n",
                        mode0, state->draw_mode1, nx0, ny0, nx1, ny1,
                        (unsigned long long)cst);
            }
        }
    }
    if (opcode == 2 && (mode0 & 0x40) && sgi_cmdt_enabled()) {
        fprintf(stderr, "CMDT-X x=%d y=%d xs=%d hs=%d\n", x, y,
                (int)state->x_save, state->host_shift);
    }
    sgi_gio64_write_x_start(state, x << 11);
    sgi_gio64_write_y_start(state, y << 11);
    sgi_gio64_fb_dirty(state, (state->draw_mode1 & 7) >= 4 &&
                       (state->draw_mode1 & 7) <= 6,
                       MIN(MIN(x_start, x_end), (int)state->x_save),
                       MIN(y_start, y_end),
                       MAX(MAX(x_start, x_end), (int)state->x_save),
                       MAX(y_start, y_end));
}

/*
 * Surface-draw worker. Surface fills and blits (DRAW with BLOCK or SPAN
 * address mode, no COLORHOST) run on this thread, one job at a time. Every
 * device entry point (REX3 register read/write, scanout, reset) first calls
 * sgi_gio64_gfx_sync, so the vCPU never observes or changes REX3 state while
 * a job is running. Pixel and host-data operations stay synchronous.
 * SGI_GFX_SYNC=1 disables the worker for A/B comparison.
 */
static bool sgi_gfx_threaded;

/* Upper bound on any wait for the worker, so a stuck worker degrades to
 * synchronous drawing instead of wedging the vCPU. */
#define SGI_GFX_WAIT_MS 100

static void sgi_gio64_gfx_sync(SGIGIO64State *s)
{
    qemu_mutex_lock(&s->gfx_mtx);
    while (s->gfx_busy || s->gfx_pending) {
        if (!qemu_cond_timedwait(&s->gfx_cond_idle, &s->gfx_mtx,
                                 SGI_GFX_WAIT_MS)) {
            /* Worker stalled: take over. gfx_busy means it is mid-draw and
             * must not be doubled up on; bailing out keeps the vCPU live. */
            break;
        }
    }
    qemu_mutex_unlock(&s->gfx_mtx);
}

static void *sgi_gio64_gfx_worker(void *opaque)
{
    SGIGIO64State *s = opaque;

    qemu_mutex_lock(&s->gfx_mtx);
    s->gfx_ready = true;
    qemu_cond_signal(&s->gfx_cond_idle);
    for (;;) {
        while (!s->gfx_pending) {
            qemu_cond_wait(&s->gfx_cond_work, &s->gfx_mtx);
        }
        s->gfx_pending = false;
        s->gfx_busy = true;
        qemu_mutex_unlock(&s->gfx_mtx);

        sgi_gio64_rex3_command(s);

        qemu_mutex_lock(&s->gfx_mtx);
        s->gfx_busy = false;
        qemu_cond_broadcast(&s->gfx_cond_idle);
    }
    return NULL;
}

/* Called after a REX3 register write with GO set. */
static void sgi_gio64_go(SGIGIO64State *s)
{
    uint32_t mode0 = s->draw_mode0;
    uint32_t adrmode = (mode0 >> 2) & 7;
    bool surface = (mode0 & 3) == 2 && (adrmode == 0 || adrmode == 1) &&
                   !(mode0 & (1u << 6));

    if (!sgi_gfx_threaded || !surface) {
        sgi_gio64_gfx_sync(s);
        sgi_gio64_rex3_command(s);
        return;
    }
    qemu_mutex_lock(&s->gfx_mtx);
    while (s->gfx_busy || s->gfx_pending) {
        if (!qemu_cond_timedwait(&s->gfx_cond_idle, &s->gfx_mtx,
                                 SGI_GFX_WAIT_MS)) {
            /* Worker stalled: run this draw synchronously instead. */
            qemu_mutex_unlock(&s->gfx_mtx);
            sgi_gio64_rex3_command(s);
            return;
        }
    }
    s->gfx_pending = true;
    qemu_cond_signal(&s->gfx_cond_work);
    qemu_mutex_unlock(&s->gfx_mtx);
}

static uint64_t sgi_gio64_xl24_read_reg(void *opaque, hwaddr addr,
                                        unsigned size)
{
    SGIGIO64State *state = opaque;
    bool high_word = !(addr & 4);
    /* Bit 11 is the REX3 "go" bit (MAME offset & ~(0x800/8)): it re-executes
     * the command, not part of the low register decode. 0x1000+ is a
     * separate block and keeps its full offset. */
    hwaddr reg = addr & ~7ULL;

    if (reg < 0x1000) {
        reg &= ~0x800ULL;
    }

    switch (reg) {
    case 0x0000:
        return size == 8 ? (uint64_t)state->draw_mode1 << 32 | state->draw_mode0 :
               (high_word ? state->draw_mode1 : state->draw_mode0);
    case 0x0008:
        return size == 8 ? (uint64_t)state->ls_mode << 32 |
                                  state->line_stipple_pattern :
               (high_word ? state->ls_mode : state->line_stipple_pattern);
    case 0x0010:
        return size == 8 ? (uint64_t)state->ls_pattern_saved << 32 |
                                  state->z_pattern :
               (high_word ? state->ls_pattern_saved : state->z_pattern);
    case 0x0018:
        return size == 8 ? (uint64_t)state->color_back << 32 |
                                  state->color_vram :
               (high_word ? state->color_back : state->color_vram);
    case 0x0100:
        return size == 8 ? (uint64_t)(uint32_t)state->x_start << 32 |
                                  (uint32_t)state->y_start :
               (high_word ? (uint32_t)state->x_start :
                (uint32_t)state->y_start);
    case 0x0108:
        return size == 8 ? (uint64_t)(uint32_t)state->x_end << 32 |
                                  (uint32_t)state->y_end :
               (high_word ? (uint32_t)state->x_end : (uint32_t)state->y_end);
    case 0x0110:
        return size == 8 ? (uint64_t)(uint16_t)state->x_save << 32 |
                                  state->xy_move :
               (high_word ? (uint16_t)state->x_save : state->xy_move);
    case 0x0118:
        return size == 8 ? (uint64_t)state->bres_d << 32 | state->bres_s1 :
               (high_word ? state->bres_d : state->bres_s1);
    case 0x0120:
        return size == 8 ? (uint64_t)state->bres_octant_inc1 << 32 |
                                  state->bres_round_inc2 :
               (high_word ? state->bres_octant_inc1 :
                state->bres_round_inc2);
    case 0x0128:
        return size == 8 ? (uint64_t)state->bres_e1 << 32 | state->bres_s2 :
               (high_word ? state->bres_e1 : state->bres_s2);
    case 0x0130:
        return size == 8 ? (uint64_t)state->a_weight0 << 32 |
                                  state->a_weight1 :
               (high_word ? state->a_weight0 : state->a_weight1);
    case 0x0138:
        return size == 8 ? (uint64_t)state->x_start_f << 32 |
                                  state->y_start_f :
               (high_word ? state->x_start_f : state->y_start_f);
    case 0x0140:
        return size == 8 ? (uint64_t)state->x_end_f << 32 |
                                  state->y_end_f :
               (high_word ? state->x_end_f : state->y_end_f);
    case 0x0148:
        return size == 8 ? (uint64_t)(uint32_t)state->x_start_i << 32 |
                                  state->x_end_f :
               (high_word ? (uint32_t)state->x_start_i : state->x_end_f);
    case 0x0150:
        return size == 8 ? (uint64_t)state->xy_start_i << 32 |
                                  state->xy_end_i :
               (high_word ? state->xy_start_i : state->xy_end_i);
    case 0x0158:
        return size == 8 ? (uint64_t)state->x_start_end_i << 32 :
               (high_word ? state->x_start_end_i : 0);
    case 0x0220:
        return size == 8 ? (uint64_t)state->write_mask << 32 | state->draw_color :
               (high_word ? state->write_mask : state->draw_color);
    case 0x0230:
        /* Size-4 reads get the addressed half, not the whole register. */
        if (sgi_fs_enabled()) {
            sgi_rd_host++;
        }
        return size == 8 ? state->host_dataport :
               (high_word ? (uint32_t)(state->host_dataport >> 32) :
                (uint32_t)state->host_dataport);
    case 0x0238:
        return size == 8 ? (uint64_t)state->dcb_mode << 32 :
               (high_word ? state->dcb_mode : 0);
    case 0x0240:
        if (sgi_fs_enabled()) {
            sgi_rd_dcb++;
        }
        if (high_word) {
            uint64_t ret = 0;

            switch (state->dcb_slave_select) {
            case DCB_ADDR_VC2:
                if (state->dcb_reg_select == 3) {
                    /* VC2 RAM read with auto-increment + wrap at 0x8000,
                     * like vc2_device::read case 0x03. */
                    ret = (uint64_t)state->vc2_ram[
                        state->vc2_ram_addr] << 32;
                    state->vc2_ram_addr = (state->vc2_ram_addr + 1) & 0x7fff;
                } else if (state->dcb_reg_select == 1) {
                    /* Register read via VC2 internal index (set by a
                     * byte-wide DCB transfer to reg 0). Unknown indices
                     * read 0, like MAME. */
                    switch (state->vc2_reg_idx) {
                    case 0x00 ... 0x10:
                        ret = (uint64_t)state->vc2_regs[
                            state->vc2_reg_idx] << 32;
                        break;
                    case 0x1f:
                        ret = (uint64_t)state->vc2_regs[0x1f] << 32;
                        break;
                    default:
                        break;
                    }
                }
                /* Other VC2 reg_select values are not readable (MAME
                 * logs unknown and returns 0). */
                break;
            case DCB_ADDR_CMAP0:
            case DCB_ADDR_CMAP1: {
                /* Status/revision are head-independent constants. */
                if (state->dcb_reg_select == 4) {
                    ret = (uint64_t)XL24_CMAP_STATUS << 32;
                } else if (state->dcb_reg_select == 6) {
                    ret = (uint64_t)XL24_CMAP_REVISION << 32;
                }
                break;
            }
            case DCB_ADDR_XMAP0:
            case DCB_ADDR_XMAP1: {
                unsigned head = state->dcb_slave_select - DCB_ADDR_XMAP0;

                switch (state->dcb_reg_select) {
                case 0:
                    ret = (uint64_t)state->xmap_config[head] << 32;
                    break;
                case 1:
                    ret = (uint64_t)XL24_XMAP_REVISION << 32;
                    break;
                case 2:
                    ret = (uint64_t)2 << 32;
                    break;
                case 3:
                    ret = (uint64_t)state->xmap_cursor_cmap[head] << 32;
                    break;
                case 4:
                    ret = (uint64_t)state->xmap_popup_cmap[head] << 32;
                    break;
                case 5: {
                    uint8_t idx =
                        (state->xmap_mode_index[head] & 0x7c) >> 2;
                    uint32_t entry = state->xmap_mode[head][idx & 31];

                    switch (state->xmap_mode_index[head] & 3) {
                    case 0:
                        ret = (uint64_t)(entry & 0xff) << 32;
                        break;
                    case 1:
                        ret = (uint64_t)((entry >> 8) & 0xff) << 32;
                        break;
                    case 2:
                        ret = (uint64_t)((entry >> 16) & 0xff) << 32;
                        break;
                    default:
                        break;
                    }
                    break;
                }
                case 7:
                    ret = (uint64_t)state->xmap_mode_index[head] << 32;
                    break;
                default:
                    break;
                }
                break;
            }
            case DCB_ADDR_RAMDAC:
            case DCB_ADDR_CC1:
            case DCB_ADDR_AB1:
                /* Not readable on hardware (MAME logs "not yet implemented"
                 * and returns 0 for the MSW half). */
                break;
            case DCB_ADDR_PCD: /* Presenter not connected; bus timeout */
                sgi_gio64_pcd_access(state);
                break;
            default:
                /* CMAP01/XMAP01 have no read port; combo/unknown slaves
                 * return the last MSW latch like MAME's default case. */
                ret = (uint64_t)state->dcb_data_msw << 32;
                break;
            }
            if (state->dcb_mode & (1U << 3)) {
                state->dcb_reg_select = (state->dcb_reg_select + 1) & 7;
            }
            return size == 8 ? ret : (uint32_t)(ret >> 32);
        }
        return size == 8 ? (uint64_t)state->dcb_data_lsw :
               state->dcb_data_lsw;
    case 0x1330:
        return size == 8 ? (uint64_t)state->rex3_config << 32 :
               (high_word ? state->rex3_config : 0);
    case 0x1338:
    {
        uint32_t status = state->rex3_status;

        if (sgi_fs_enabled()) {
            if (size == 8 || high_word) {
                sgi_rd_status++;
            } else {
                sgi_rd_user++;
            }
        }
        if (size == 8) {
            state->rex3_status &= ~REX3_STATUS_VRINT;
            qemu_set_irq(state->gio_irq[2], 0);
            return (uint64_t)(status | 3) << 32 | state->rex3_status;
        }
        if (high_word) {
            state->rex3_status &= ~REX3_STATUS_VRINT;
            qemu_set_irq(state->gio_irq[2], 0);
            return status | 3;
        }
        return status;
    }
    case 0x0028:
        return size == 8 ? (uint64_t)state->screenmask_x[0] << 32 |
                                  state->screenmask_y[0] :
               (high_word ? state->screenmask_x[0] : state->screenmask_y[0]);
    case 0x0020:
        return size == 8 ? (uint64_t)state->alpha_ref << 32 :
               (high_word ? state->alpha_ref : 0);
    case 0x0030:
        return size == 8 ? (uint64_t)state->setup << 32 :
               (high_word ? state->setup : 0);
    case 0x0200:
        return size == 8 ? (uint64_t)state->curr_color_red << 32 |
                                  (uint32_t)state->curr_color_alpha :
               (high_word ? state->curr_color_red :
                (uint32_t)state->curr_color_alpha);
    case 0x0208:
        return size == 8 ? (uint64_t)state->curr_color_green << 32 |
                                  (uint32_t)state->curr_color_blue :
               (high_word ? state->curr_color_green :
                (uint32_t)state->curr_color_blue);
    case 0x0210:
        return size == 8 ? (uint64_t)state->slope_red << 32 |
                                  (uint32_t)state->slope_alpha :
               (high_word ? state->slope_red :
                (uint32_t)state->slope_alpha);
    case 0x0218:
        return size == 8 ? (uint64_t)state->slope_green << 32 |
                                  (uint32_t)state->slope_blue :
               (high_word ? state->slope_green :
                (uint32_t)state->slope_blue);
    case 0x0228:
        return size == 8 ? (uint64_t)state->zero_overflow << 32 |
                                  (uint32_t)state->slope_red :
               (high_word ? state->zero_overflow :
                (uint32_t)state->slope_red);
    case 0x1300:
    case 0x1308:
    case 0x1310:
    case 0x1318:
        {
            unsigned mask = ((addr & ~7ULL) - 0x1300) / 8 + 1;
            return size == 8 ? (uint64_t)state->screenmask_x[mask] << 32 |
                                      state->screenmask_y[mask] :
                   (high_word ? state->screenmask_x[mask] :
                    state->screenmask_y[mask]);
        }
    case 0x1320:
        return size == 8 ? (uint64_t)state->top_scanline << 32 |
                                  state->xy_window :
               (high_word ? state->top_scanline : state->xy_window);
    case 0x1328:
        return size == 8 ? (uint64_t)state->clip_mode << 32 :
               (high_word ? state->clip_mode : 0);
    default:
        return (uint64_t)state->rex3_regs[(addr & (XL24_REX3_SIZE - 1)) >> 2];
    }
}

static uint64_t sgi_gio64_xl24_read(void *opaque, hwaddr addr, unsigned size)
{
    sgi_gio64_gfx_sync(opaque);
    uint64_t value = sgi_gio64_xl24_read_reg(opaque, addr, size);

    if (sgi_gio_trace_enabled() && sgi_gio_trace_wanted(addr)) {
        fprintf(stderr, "GIO-R off=%05x sz=%u val=%016llx\n",
                (unsigned)addr, size, (unsigned long long)value);
    }
    if (addr & 0x800) {
        sgi_gio64_go(opaque);
    }
    return value;
}

static void sgi_gio64_xl24_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    sgi_gio64_gfx_sync(opaque);
    SGIGIO64State *state = opaque;
    hwaddr reg = addr & ~7ULL;

    if (sgi_seq_enabled()) {
        hwaddr creg = addr & ~0x800ULL;

        if (creg < 0x1000 && (reg & 0x7ff) != 0x230) {
            fprintf(stderr, "SWR n=%llu off=%05x val=%016llx size=%u go=%u\n",
                    (unsigned long long)sgi_seq_n, (unsigned)addr,
                    (unsigned long long)value, (unsigned)size,
                    (unsigned)((addr & 0x800) != 0));
        } else if (reg >= 0x1300 && reg <= 0x1340) {
            /* SWR is gated to <0x1000, hiding SMASK1-4 / CLIPMODE /
             * CONFIG / DCBRESET writes. Log them explicitly. */
            fprintf(stderr, "SMW off=%05x val=%016llx size=%u go=%u\n",
                    (unsigned)addr, (unsigned long long)value, (unsigned)size,
                    (unsigned)((addr & 0x800) != 0));
        }
    }

    /* TEMP: prove whether sub-word (8/16-bit) REX3 writes occur. The
     * half-word helpers clobber sibling bits on sub-word stores; if the
     * guest never uses size<4, decode is safe. Capped; PRIMLOG-gated. */
    if (sgi_pl_enabled() && size < 4) {
        static unsigned sgi_pl_subw;

        if (sgi_pl_subw < 40) {
            sgi_pl_subw++;
            fprintf(stderr, "SUBWORD W off=%05x sz=%u val=%llx "
                    "slave=%u regsel=%u mask=%08x\n",
                    (unsigned)addr, size, (unsigned long long)value,
                    state->dcb_slave_select, state->dcb_reg_select,
                    state->dcb_mask);
        }
    }

    /* Draw-state setup is invisible until a command runs (the go-bit
     * command marks its own damage rect below); only scanout-config or
     * unknown registers need a full re-raster here. */
    if (!(addr & 0x800)) {
        hwaddr cfg = reg < 0x1000 ? reg & ~0x800ULL : reg;

        if (cfg == 0x0240 || (cfg != 0x0000 && cfg != 0x0008 &&
            cfg != 0x0010 && cfg != 0x0018 && cfg != 0x0020 &&
            cfg != 0x0028 && cfg != 0x0030 && cfg != 0x0038 &&
            cfg != 0x0100 && cfg != 0x0108 && cfg != 0x0110 &&
            cfg != 0x0118 && cfg != 0x0120 && cfg != 0x0128 &&
            cfg != 0x0130 && cfg != 0x0138 && cfg != 0x0140 &&
            cfg != 0x0148 && cfg != 0x0150 && cfg != 0x0158 &&
            cfg != 0x0200 && cfg != 0x0208 && cfg != 0x0210 &&
            cfg != 0x0218 && cfg != 0x0220 && cfg != 0x0228 &&
            cfg != 0x0230 && cfg != 0x0238 && cfg != 0x1300 &&
            cfg != 0x1308 && cfg != 0x1310 && cfg != 0x1318 &&
            cfg != 0x1320 && cfg != 0x1328 && cfg != 0x1330 &&
            cfg != 0x1338 && cfg != 0x1340)) {
            /* DCB data hits palette/XMAP/VC2/RAMDAC state; the cursor
             * registers narrow it to the cursor area below, the PCD
             * timeout touches no pixels. Unknown registers stay full. */
            if (cfg != 0x0240) {
                sgi_gio64_damage_full(state);
            }
        }
    }
    if (sgi_gio_trace_enabled() && sgi_gio_trace_wanted(addr)) {
        fprintf(stderr, "GIO-W off=%05x sz=%u val=%016llx\n",
                (unsigned)addr, size, (unsigned long long)value);
    }

    if (reg < 0x1000) {
        reg &= ~0x800ULL;
    }

    switch (reg) {
    case 0x0000: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->draw_mode1 = sgi_gio64_high_word(value, size, addr);
            state->plane_depth = (state->draw_mode1 >> 3) & 3;
            state->host_depth = (state->draw_mode1 >> 8) & 3;
        }
        if (do_low) {
            state->draw_mode0 = sgi_gio64_low_word(value, size, addr);
        }
        break;
    }
    case 0x0120: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            /* MAME masks with 0x070fffff (octant bits 26:24 plus the
             * 20-bit increment). */
            state->bres_octant_inc1 =
                sgi_gio64_high_word(value, size, addr) & 0x070fffff;
        }
        if (do_low) {
            state->bres_round_inc2 =
                sgi_gio64_low_word(value, size, addr) & 0xff1fffff;
        }
        break;
    }
    case 0x0110: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->x_save =
                (int16_t)sgi_gio64_high_word(value, size, addr);
        }
        if (do_low) {
            state->xy_move = sgi_gio64_low_word(value, size, addr);
            state->x_move = (int16_t)(state->xy_move >> 16);
            state->y_move = (int16_t)state->xy_move;
        }
        break;
    }
    case 0x0118: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->bres_d = sgi_gio64_high_word(value, size, addr) & 0x7ffffff;
        }
        if (do_low) {
            state->bres_s1 = sgi_gio64_low_word(value, size, addr) & 0x1ffff;
        }
        break;
    }
    case 0x0128: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->bres_e1 =
                (uint16_t)sgi_gio64_high_word(value, size, addr);
        }
        if (do_low) {
            state->bres_s2 =
                sgi_gio64_low_word(value, size, addr) & 0x3ffffff;
        }
        break;
    }
    case 0x0130: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->a_weight0 = sgi_gio64_high_word(value, size, addr);
        }
        if (do_low) {
            state->a_weight1 = sgi_gio64_low_word(value, size, addr);
        }
        break;
    }
    case 0x0138: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            sgi_gio64_write_x_start(state, (int32_t)(
                sgi_gio64_high_word(value, size, addr) & 0x007fff80));
            state->x_save = state->x_start_i;
        }
        if (do_low) {
            sgi_gio64_write_y_start(state, (int32_t)(
                sgi_gio64_low_word(value, size, addr) & 0x007fff80));
        }
        break;
    }
    /* Full-state coordinate views: unmasked passthrough, with the
     * 0x07ffff80 register mask applied by the write helpers. */
    case 0x0100: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            sgi_gio64_write_x_start(
                state, (int32_t)sgi_gio64_high_word(value, size, addr));
            state->x_save = state->x_start_i;
        }
        if (do_low) {
            sgi_gio64_write_y_start(
                state, (int32_t)sgi_gio64_low_word(value, size, addr));
        }
        break;
    }
    case 0x0108: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            sgi_gio64_write_x_end(
                state, (int32_t)sgi_gio64_high_word(value, size, addr));
        }
        if (do_low) {
            sgi_gio64_write_y_end(
                state, (int32_t)sgi_gio64_low_word(value, size, addr));
        }
        break;
    }
    case 0x0140: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            sgi_gio64_write_x_end(state, (int32_t)(
                sgi_gio64_high_word(value, size, addr) & 0x007fff80));
        }
        if (do_low) {
            sgi_gio64_write_y_end(state, (int32_t)(
                sgi_gio64_low_word(value, size, addr) & 0x007fff80));
        }
        break;
    }
    case 0x0148: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            sgi_gio64_write_x_start(state,
                ((int32_t)(int16_t)sgi_gio64_high_word(value, size, addr))
                << 11);
            state->x_save = state->x_start_i;
        }
        if (do_low) {
            sgi_gio64_write_x_end(state, (int32_t)(
                sgi_gio64_low_word(value, size, addr) & 0x007fff80));
        }
        break;
    }
    case 0x0150: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            uint32_t data32 = sgi_gio64_high_word(value, size, addr);

            sgi_gio64_write_x_start(state,
                ((int32_t)(int16_t)(data32 >> 16)) << 11);
            sgi_gio64_write_y_start(state,
                ((int32_t)(int16_t)data32) << 11);
            state->x_save = state->x_start_i;
            state->xy_start = (uint32_t)(uint16_t)state->x_start_i << 16 |
                              (uint16_t)state->y_start_i;
        }
        if (do_low) {
            uint32_t data32 = sgi_gio64_low_word(value, size, addr);

            sgi_gio64_write_x_end(state,
                ((int32_t)(int16_t)(data32 >> 16)) << 11);
            sgi_gio64_write_y_end(state,
                ((int32_t)(int16_t)data32) << 11);
            state->xy_end = (uint32_t)(uint16_t)state->x_end_i << 16 |
                            (uint16_t)state->y_end_i;
        }
        break;
    }
    case 0x0158: {
        bool do_high = size == 8 || !(addr & 4);

        if (do_high) {
            uint32_t data32 = sgi_gio64_high_word(value, size, addr);

            sgi_gio64_write_x_start(state,
                ((int32_t)(int16_t)(data32 >> 16)) << 11);
            sgi_gio64_write_x_end(state,
                ((int32_t)(int16_t)data32) << 11);
            state->x_save = state->x_start_i;
            state->x_start_end_i = data32;
        }
        break;
    }
    case 0x0220: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            /* MAME masks the write mask to 24 bits. */
            state->write_mask =
                sgi_gio64_high_word(value, size, addr) & 0xffffff;
        }
        if (do_low) {
            state->draw_color = sgi_gio64_low_word(value, size, addr);
            state->color_i = state->draw_color;
            if ((state->draw_mode1 >> 15) & 1) {
                state->curr_color_red = (state->draw_color & 0xff) << 11;
                state->curr_color_green =
                    (state->draw_color & 0xff00) << 3;
                state->curr_color_blue =
                    (state->draw_color & 0xff0000) >> 5;
            }
        }
        break;
    }
    case 0x0228: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->zero_overflow = sgi_gio64_high_word(value, size, addr);
        }
        if (do_low) {
            state->slope_red = sgi_gio64_low_word(value, size, addr);
        }
        break;
    }
    case 0x0230: {
        if (sgi_fs_enabled()) {
            sgi_fs_hostwrites++;
        }
        if (size == 8) {
            state->host_dataport = value;
            /* Whole qwords stage for the next GO's batched transfer; the
             * 32-bit halves stay on the scalar fallback (iris rex3.rs:3210-
             * 3231 arms a single word for those). */
            sgi_gio64_host_stage(state, value);
        } else if (addr & 4) {
            state->host_dataport =
                (state->host_dataport & 0xffffffff00000000ULL) | value;
        } else {
            state->host_dataport =
                (state->host_dataport & 0xffffffffULL) | (value << 32);
        }
        /* Iris hostrw_arm_single: every HOSTRW write, including 32-bit
         * halves, arms a one-word transfer (host_len == 1 at GO).
         * Overwrite slot 0 (never append) so high/low half pairs resolve
         * to the assembled word; leave an in-flight multi-word batch
         * alone. */
        if (state->host_staged <= 1) {
            state->host_words[0] = state->host_dataport;
            state->host_staged = 1;
        }
        /* MAME: host_shift restarts at 64 - s_host_shifts[hostdepth]. */
        state->host_shift = 64 - sgi_gio64_host_shift(state->host_depth);
        break;
    }
    case 0x0008: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->ls_mode =
                sgi_gio64_high_word(value, size, addr) & 0xfffffff;
        }
        if (do_low) {
            state->line_stipple_pattern =
                sgi_gio64_low_word(value, size, addr);
        }
        break;
    }
    case 0x0010: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->ls_pattern_saved =
                sgi_gio64_high_word(value, size, addr);
        }
        if (do_low) {
            state->z_pattern = sgi_gio64_low_word(value, size, addr);
        }
        break;
    }
    case 0x0018: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->color_back = sgi_gio64_high_word(value, size, addr);
        }
        if (do_low) {
            state->color_vram = sgi_gio64_low_word(value, size, addr);
        }
        break;
    }
    case 0x0020: {
        bool do_high = size == 8 || !(addr & 4);

        if (do_high) {
            state->alpha_ref =
                (uint8_t)sgi_gio64_high_word(value, size, addr);
        }
        /* LSW is a GFIFO stall request with no readable state. */
        break;
    }
    case 0x0030: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->setup = sgi_gio64_high_word(value, size, addr);
            /* MAME rex3_w case 0x0030/8 high half runs do_setup(). */
            sgi_gio64_rex3_setup(state);
        }
        if (do_low) {
            state->step_z = 1;
        }
        break;
    }
    case 0x0038: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            /* LSSAVE restores working LSPATTERN/LSRCOUNT from the saved
             * set (LSRCNTSAVE lives in LSMODE bits 23:16). */
            state->line_stipple_pattern = state->ls_pattern_saved;
            state->ls_mode = (state->ls_mode & ~0xffu) |
                             ((state->ls_mode >> 16) & 0xffu);
        }
        if (do_low) {
            /* LSRESTORE saves working LSPATTERN/LSRCOUNT. */
            state->ls_pattern_saved = state->line_stipple_pattern;
            state->ls_mode = (state->ls_mode & ~(0xffu << 16)) |
                             ((state->ls_mode & 0xffu) << 16);
        }
        break;
    }
    case 0x0200: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            /* MAME stores 24 bits (o12.11/o8.11 components). */
            uint32_t data32 = sgi_gio64_high_word(value, size, addr);

            state->curr_color_red = (int32_t)(data32 & 0xffffff);
            if (!((state->draw_mode1 >> 15) & 1)) {
                switch (state->plane_depth) {
                case 0: /* 4bpp */
                    state->color_i = (data32 >> 11) & 0xf;
                    break;
                case 1: /* 8bpp */
                    state->color_i = (data32 >> 11) & 0xff;
                    break;
                case 2: /* 12bpp */
                    state->color_i = (data32 >> 9) & 0xfff;
                    break;
                default:
                    break;
                }
            }
        }
        if (do_low) {
            state->curr_color_alpha =
                sgi_gio64_low_word(value, size, addr) & 0xfffff;
        }
        break;
    }
    case 0x0208: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->curr_color_green =
                sgi_gio64_high_word(value, size, addr) & 0xfffff;
        }
        if (do_low) {
            state->curr_color_blue =
                sgi_gio64_low_word(value, size, addr) & 0xfffff;
        }
        break;
    }
    /* Slope registers use sign-magnitude encoding on write (ported from
     * MAME); 0x0228 LSW is a raw copy instead. */
    case 0x0210: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            uint32_t data32 =
                sgi_gio64_high_word(value, size, addr) & 0x807fffff;
            int32_t temp;

            if (data32 & (1U << 31)) {
                temp = 0x00800000 - (data32 & 0x7fffff);
                temp |= 0x00800000;
            } else {
                temp = data32 & 0x7fffff;
            }
            state->slope_red = temp;
        }
        if (do_low) {
            uint32_t data32 =
                sgi_gio64_low_word(value, size, addr) & 0x8007ffff;
            int32_t temp;

            if (data32 & (1U << 31)) {
                temp = 0x00080000 - (data32 & 0x7ffff);
                temp |= 0x00080000;
            } else {
                temp = data32 & 0x7ffff;
            }
            state->slope_alpha = temp;
        }
        break;
    }
    case 0x0218: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            uint32_t data32 =
                sgi_gio64_high_word(value, size, addr) & 0x8007ffff;
            int32_t temp;

            if (data32 & (1U << 31)) {
                temp = 0x00080000 - (data32 & 0x7ffff);
                temp |= 0x00080000;
            } else {
                temp = data32 & 0x7ffff;
            }
            state->slope_green = temp;
        }
        if (do_low) {
            uint32_t data32 =
                sgi_gio64_low_word(value, size, addr) & 0x8007ffff;
            int32_t temp;

            if (data32 & (1U << 31)) {
                temp = 0x00080000 - (data32 & 0x7ffff);
                temp |= 0x00080000;
            } else {
                temp = data32 & 0x7ffff;
            }
            state->slope_blue = temp;
        }
        break;
    }
    case 0x0238: {
        bool do_high = size == 8 || !(addr & 4);

        if (do_high) {
            uint32_t data = sgi_gio64_high_word(value, size, addr);

            /* MAME decodes the transfer width into a data mask used for
             * VC2 register-select vs RAM-word vs full-register writes. */
            switch (data & 3) {
            case 0x00:
                state->dcb_mask = 0xffffffff;
                break;
            case 0x01:
                state->dcb_mask = 0x000000ff;
                break;
            case 0x02:
                state->dcb_mask = 0x0000ffff;
                break;
            default:
                state->dcb_mask = 0xffffffff;
                break;
            }
            state->dcb_reg_select = (data & 0x00000070) >> 4;
            state->dcb_slave_select = (data & 0x00000780) >> 7;
            state->dcb_mode = data & 0x1fffffff;
        }
        break;
    }
    case 0x0240: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_low) {
            state->dcb_data_lsw = sgi_gio64_low_word(value, size, addr);
        }
        if (!do_high) {
            break;
        }
        state->dcb_data_msw = sgi_gio64_high_word(value, size, addr);
        switch (state->dcb_slave_select) {
        case DCB_ADDR_VC2:
            if (state->dcb_mask == 0x000000ff) {
                if (state->dcb_reg_select == 0) {
                    state->vc2_reg_idx = (uint8_t)state->dcb_data_msw;
                }
            } else if (state->dcb_mask == 0x0000ffff) {
                if (state->dcb_reg_select == 3) {
                    state->vc2_ram[state->vc2_ram_addr] =
                        (uint16_t)state->dcb_data_msw;
                    state->vc2_ram_addr =
                        (state->vc2_ram_addr + 1) & 0x7fff;
                    sgi_gio64_damage_full(state);
                }
            } else {
                if (state->dcb_reg_select == 3) {
                    state->vc2_ram[state->vc2_ram_addr] =
                        state->dcb_data_msw & 0xffff;
                    state->vc2_ram_addr =
                        (state->vc2_ram_addr + 1) & 0x7fff;
                    sgi_gio64_damage_full(state);
                } else if (state->dcb_reg_select == 0) {
                    uint8_t vcreg = state->dcb_data_msw >> 24;

                    if (vcreg < ARRAY_SIZE(state->vc2_regs)) {
                        if (vcreg == VC2_REG_CURSOR_X ||
                            vcreg == VC2_REG_CURSOR_Y ||
                            vcreg == VC2_REG_CURSOR_ENTRY ||
                            vcreg == VC2_REG_DISPLAY_CTRL) {
                            sgi_gio64_damage_cursor(state);
                        }
                        state->vc2_regs[vcreg] =
                            (state->dcb_data_msw >> 8) & 0xffff;
                        if (vcreg == VC2_REG_CURSOR_X ||
                            vcreg == VC2_REG_CURSOR_Y ||
                            vcreg == VC2_REG_CURSOR_ENTRY ||
                            vcreg == VC2_REG_DISPLAY_CTRL) {
                            sgi_gio64_damage_cursor(state);
                        } else {
                            sgi_gio64_damage_full(state);
                        }
                        if (vcreg == 0x07) {
                            state->vc2_ram_addr = state->vc2_regs[vcreg];
                        }
                        /* MAME vc2 write case 0x00 calls
                         * update_screen_size() (newport.cpp:484). */
                        if (vcreg == VC2_REG_VID_ENTRY) {
                            sgi_gio64_update_screen_size(state);
                        }
                    }
                    state->vc2_reg_idx = vcreg;
                }
            }
            break;
        case DCB_ADDR_CMAP01: /* programs both colormaps */
        case DCB_ADDR_CMAP0:
        case DCB_ADDR_CMAP1: {
            unsigned first =
                state->dcb_slave_select == DCB_ADDR_CMAP1 ? 1 : 0;
            unsigned last =
                state->dcb_slave_select == DCB_ADDR_CMAP0 ? 0 : 1;

            for (unsigned head = first; head <= last; head++) {
                if (state->dcb_reg_select == 0) {
                    state->cmap_index[head] = state->dcb_data_msw & 0xffff;
                } else if (state->dcb_reg_select == 2) {
                    /* Pens are 0x00RRGGBB (MAME cmap set_pen_color order). */
                    state->cmap_palette[head][state->cmap_index[head]] =
                        state->dcb_data_msw >> 8;
                    sgi_gio64_damage_full(state);
                    if (state->cmap_index[head] < 0x2000 && sgi_gio_dbg()) {
                        static unsigned npens;
                        if (npens++ < 2500) {
                            fprintf(stderr,
                                    "PEN h=%d idx=%04x rgba=%08x\n",
                                    head, state->cmap_index[head],
                                    state->dcb_data_msw >> 8);
                        }
                    }
                }
            }
            sgi_gio64_damage_full(state);
            break;
        }
        case DCB_ADDR_XMAP01:
        case DCB_ADDR_XMAP0:
        case DCB_ADDR_XMAP1: {
            unsigned first =
                state->dcb_slave_select == DCB_ADDR_XMAP1 ? 1 : 0;
            unsigned last =
                state->dcb_slave_select == DCB_ADDR_XMAP0 ? 0 : 1;

            for (unsigned head = first; head <= last; head++) {
                switch (state->dcb_reg_select) {
                case 0:
                    state->xmap_config[head] = (uint8_t)state->dcb_data_msw;
                    sgi_gio64_damage_full(state);
                    break;
                case 3:
                    state->xmap_cursor_cmap[head] =
                        (uint8_t)state->dcb_data_msw;
                    sgi_gio64_damage_cursor(state);
                    break;
                case 4:
                    state->xmap_popup_cmap[head] =
                        (uint8_t)state->dcb_data_msw;
                    sgi_gio64_damage_full(state);
                    break;
                case 5:
                    /* 32-entry table; mask the index. */
                    state->xmap_mode[head][
                        (state->dcb_data_msw >> 24) & 31] =
                        state->dcb_data_msw & 0xffffff;
                    if (sgi_modlog_enabled()) {
                        fprintf(stderr, "XMAPMODE h=%u idx=%u val=%06x\n",
                                head, (state->dcb_data_msw >> 24) & 31,
                                state->dcb_data_msw & 0xffffff);
                        /* Visual change: re-arm the DID dump so the next
                         * frame captures the live map, not boot zeros. */
                        sgi_modlog_dumped = false;
                        /* One-shot: the first RGB-visual entry write
                         * (Ideas viewport, da09xx -- boot writes
                         * defaults which must NOT arm) re-arms the
                         * segment map so the next frame captures live
                         * segments instead of boot rows. Later swaps
                         * must not reset it. Same for the zero-store
                         * log (its cap would otherwise fill early). */
                        {
                            unsigned idx =
                                (state->dcb_data_msw >> 24) & 31;
                            uint32_t v = state->dcb_data_msw & 0xffffff;
                            if ((idx == 12 || idx == 13) &&
                                (v & 0xffff00) == 0xda0900 &&
                                !sgi_modlog_seg_armed) {
                                sgi_modlog_seg_armed = true;
                                sgi_modlog_seg_n = 0;
                                sgi_pl_zerologged = 0;
                            }
                        }
                    }
                    sgi_gio64_damage_full(state);
                    break;
                case 7:
                    state->xmap_mode_index[head] =
                        (uint8_t)state->dcb_data_msw;
                    break;
                default:
                    break;
                }
            }
            break;
        }
        case DCB_ADDR_RAMDAC:
            if (state->dcb_reg_select == 0) {
                state->ramdac_index = (uint8_t)state->dcb_data_msw;
            } else if (state->dcb_reg_select == 1) {
                /* Per-channel LUTs (MAME ramdac_write), applied after the
                 * palette/direct stage by ramdac_remap. */
                state->ramdac_lut_r[state->ramdac_index] =
                    (uint8_t)(state->dcb_data_msw >> 24) << 16;
                state->ramdac_lut_g[state->ramdac_index] =
                    (uint8_t)(state->dcb_data_msw >> 16) << 8;
                state->ramdac_lut_b[state->ramdac_index] =
                    (uint8_t)(state->dcb_data_msw >> 8);
                state->ramdac_index++;
                sgi_gio64_damage_full(state);
            }
            break;
        case DCB_ADDR_PCD: /* Presenter not connected; bus timeout */
            sgi_gio64_pcd_access(state);
            break;
        default:
            break;
        }
        if (state->dcb_mode & (1U << 3)) {
            state->dcb_reg_select = (state->dcb_reg_select + 1) & 7;
        }
        break;
    }
    case 0x1330: {
        bool do_high = size == 8 || !(addr & 4);

        if (do_high) {
            state->rex3_config =
                sgi_gio64_high_word(value, size, addr) & 0x1fffff;
        }
        break;
    }
    case 0x1338:
        /* REX3 status is read-only; writes are ignored by Newport. */
        break;
    case 0x1340:
        /* Reset DCB bus and flush BFIFO; no readable state. */
        break;
    case 0x0028: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->screenmask_x[0] = sgi_gio64_high_word(value, size, addr);
        }
        if (do_low) {
            state->screenmask_y[0] = sgi_gio64_low_word(value, size, addr);
        }
        break;
    }
    case 0x1300:
    case 0x1308:
    case 0x1310:
    case 0x1318:
        {
            bool do_high = size == 8 || !(addr & 4);
            bool do_low = size == 8 || (addr & 4);
            unsigned mask = ((addr & ~7ULL) - 0x1300) / 8 + 1;

            if (do_high) {
                state->screenmask_x[mask] =
                    sgi_gio64_high_word(value, size, addr);
            }
            if (do_low) {
                state->screenmask_y[mask] =
                    sgi_gio64_low_word(value, size, addr);
            }
        }
        break;
    case 0x1320: {
        bool do_high = size == 8 || !(addr & 4);
        bool do_low = size == 8 || (addr & 4);

        if (do_high) {
            state->top_scanline =
                sgi_gio64_high_word(value, size, addr) & 0x3ff;
        }
        if (do_low) {
            state->xy_window = sgi_gio64_low_word(value, size, addr);
            state->x_window = (int16_t)(state->xy_window >> 16);
            state->y_window = (int16_t)state->xy_window;
        }
        break;
    }
    case 0x1328: {
        bool do_high = size == 8 || !(addr & 4);

        if (do_high) {
            state->clip_mode =
                sgi_gio64_high_word(value, size, addr) & 0x1fff;
        }
        /* LSW is a GFIFO stall request with no readable state. */
        break;
    }
    default:
        state->rex3_regs[(addr & (XL24_REX3_SIZE - 1)) >> 2] = value;
        break;
    }

    if (addr & 0x800) {
        sgi_gio64_go(state);
    }

}

/*
 * 8-byte accesses. The VDMA engine writes one qword per HOSTRW GO (iris
 * dma_write64_bulk); the memory core must not split it into two 32-bit
 * writes, since each half would run its own GO. Only the HOSTRW port takes
 * the whole qword; every other register keeps the 32-bit split (high word
 * first) it always had.
 */
static bool sgi_gio64_is_hostrw_port(hwaddr addr)
{
    return ((addr & 0x1fff) & ~0x800u) == 0x230;
}

static uint64_t sgi_gio64_xl24_read_wrap(void *opaque, hwaddr addr,
                                         unsigned size)
{
    if (size == 8) {
        uint64_t hi = sgi_gio64_xl24_read(opaque, addr, 4);
        uint64_t lo = sgi_gio64_xl24_read(opaque, addr + 4, 4);

        return (hi << 32) | (lo & 0xffffffffu);
    }
    return sgi_gio64_xl24_read(opaque, addr, size);
}

static void sgi_gio64_xl24_write_wrap(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    if (size == 8 && !sgi_gio64_is_hostrw_port(addr)) {
        sgi_gio64_xl24_write(opaque, addr, value >> 32, 4);
        sgi_gio64_xl24_write(opaque, addr + 4, value & 0xffffffffu, 4);
        return;
    }
    sgi_gio64_xl24_write(opaque, addr, value, size);
}

static const MemoryRegionOps sgi_gio64_xl24_ops = {
    .read = sgi_gio64_xl24_read_wrap,
    .write = sgi_gio64_xl24_write_wrap,
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

/* Open-bus response for undecoded GIO offsets (empty EXP slots, reserved
 * GFX ranges). Reads return all-1s so PROM polling terminates, matching
 * unwired MAME address space; writes are ignored. */
/* TEMP VRAM change tracker (env SGI_GIO_VRAMHASH=1). FNV-hashes VRAM+aux
 * at most every 2s and logs on change, distinguishing overdrawn-white
 * (hash shows content then reverts to white) from never-issued (hash never
 * shows content). Capped; default off. */
static int sgi_gio_vramhash_on = -1;
static uint64_t sgi_gio_vramhash_last;
static uint64_t sgi_gio_auxhash_last;
static int64_t sgi_gio_vramhash_ms;
static unsigned sgi_gio_vramhash_n;
static bool sgi_gio_vramhash_enabled(void)
{
    if (sgi_gio_vramhash_on < 0) {
        sgi_gio_vramhash_on = getenv("SGI_GIO_VRAMHASH") ? 1 : 0;
    }
    return sgi_gio_vramhash_on != 0;
}

static uint64_t sgi_gio_fnv(const uint32_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* TEMP unmapped-window trace (env SGI_GIO_UNMAPPED=1). Logs GIO accesses
 * outside the REX3 window (EXP0/EXP1/reserved GFX) so missing-path
 * hypotheses are provable instead of silent. Capped; default off. */
static int sgi_gio_unmapped_on = -1;
static unsigned sgi_gio_unmapped_n;
static bool sgi_gio_unmapped_enabled(void)
{
    if (sgi_gio_unmapped_on < 0) {
        sgi_gio_unmapped_on = getenv("SGI_GIO_UNMAPPED") ? 1 : 0;
    }
    return sgi_gio_unmapped_on != 0;
}

static uint64_t sgi_gio64_unmapped_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    if (sgi_gio_unmapped_enabled() && sgi_gio_unmapped_n < 50) {
        sgi_gio_unmapped_n++;
        fprintf(stderr, "GIOUNMAP R off=%07x sz=%u\n",
                (unsigned)addr, size);
    }
    return size == 8 ? UINT64_MAX : (1ULL << (size * 8)) - 1;
}

static void sgi_gio64_unmapped_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    if (sgi_gio_unmapped_enabled() && sgi_gio_unmapped_n < 50) {
        sgi_gio_unmapped_n++;
        fprintf(stderr, "GIOUNMAP W off=%07x sz=%u val=%016llx\n",
                (unsigned)addr, size, (unsigned long long)value);
    }
}

static const MemoryRegionOps sgi_gio64_unmapped_ops = {
    .read = sgi_gio64_unmapped_read,
    .write = sgi_gio64_unmapped_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* VC2 DID helpers, ported from vc2_device::begin_did_line/
 * next_did_line_entry (newport.cpp:656-667). Regs 0x0e/0x0f mirror the
 * live frame/line pointers. A 0xFFFF line pointer terminates the chain
 * (iris parity); without it we would walk into garbage DIDs. */
static uint16_t sgi_gio64_vc2_begin_did(SGIGIO64State *state, int y)
{
    uint16_t line;

    state->vc2_regs[VC2_REG_DID_FRAME] =
        state->vc2_regs[VC2_REG_DID_ENTRY] + (uint16_t)y;
    line = state->vc2_ram[state->vc2_regs[VC2_REG_DID_FRAME] & 0x7fff];
    state->vc2_regs[VC2_REG_DID_LINE] = line;
    if (line == 0xffff) {
        return 0;
    }
    return state->vc2_ram[line & 0x7fff];
}

static uint16_t sgi_gio64_vc2_next_did(SGIGIO64State *state)
{
    state->vc2_regs[VC2_REG_DID_LINE]++;
    if (state->vc2_regs[VC2_REG_DID_LINE] == 0xffff) {
        return 0;
    }
    return state->vc2_ram[state->vc2_regs[VC2_REG_DID_LINE] & 0x7fff];
}

/* VC2 cursor fetch, ported from vc2_device::is_cursor_active/
 * get_cursor_pixel (newport.cpp:777-814), plus the IRIX x-origin bias
 * SGI_GIO64_CURSOR_X_ADJUST that iris carries as cursor_x_adjust. */
static uint8_t sgi_gio64_cursor_pixel(SGIGIO64State *state, int x, int y)
{
    uint16_t ctrl = state->vc2_regs[VC2_REG_DISPLAY_CTRL];
    uint16_t cx = state->vc2_regs[VC2_REG_CURSOR_X] -
                  SGI_GIO64_CURSOR_X_ADJUST;
    uint16_t cy = state->vc2_regs[VC2_REG_CURSOR_Y];
    bool enabled = (ctrl & VC2_CURSOR_FUNC_ENABLE) &&
                   (ctrl & VC2_CURSOR_ENABLE) &&
                   !(ctrl & VC2_CURSOR_MODE_BIT);
    bool mono;
    int size;
    int shift;
    int address;
    uint16_t entry;

    if (!enabled || x < (int)cx - 31 || x > (int)cx ||
        y < (int)cy - 31 || y > (int)cy) {
        return 0;
    }
    x -= (int)cx - 31;
    y -= (int)cy - 31;
    mono = (ctrl & VC2_CURSOR_SIZE_64) != 0;
    size = mono ? 64 : 32;
    if (x >= size || y >= size) {
        return 0;
    }
    shift = 15 - (x % 16);
    entry = state->vc2_regs[VC2_REG_CURSOR_ENTRY];
    if (mono) {
        address = y * 4 + x / 16;
        return (state->vc2_ram[(entry + address) & 0x7fff] >> shift) & 1;
    }
    address = y * 2 + x / 16;
    return ((state->vc2_ram[(entry + address) & 0x7fff] >> shift) & 1) |
           (((state->vc2_ram[(entry + address + 64) & 0x7fff] >> shift) & 1)
            << 1);
}

/* BGR->RGB converters, ported from MAME convert_*_bgr_to_24bpp_rgb
 * (newport.cpp). Used for XMAP RGB-Map0 pixels. */
static uint32_t sgi_gio64_bgr4_to_rgb(uint8_t pix_in)
{
    uint8_t r = 0xff * ((pix_in >> 0) & 1);
    uint8_t g = (0xaa * ((pix_in >> 2) & 1)) | (0x55 * ((pix_in >> 1) & 1));
    uint8_t b = 0xff * ((pix_in >> 3) & 1);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint32_t sgi_gio64_bgr8_to_rgb(uint8_t pix_in)
{
    uint8_t r = (0x92 * ((pix_in >> 2) & 1)) |
                (0x49 * ((pix_in >> 1) & 1)) |
                (0x24 * ((pix_in >> 0) & 1));
    uint8_t g = (0x92 * ((pix_in >> 5) & 1)) |
                (0x49 * ((pix_in >> 4) & 1)) |
                (0x24 * ((pix_in >> 3) & 1));
    uint8_t b = (0xaa * ((pix_in >> 7) & 1)) |
                (0x55 * ((pix_in >> 6) & 1));

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint32_t sgi_gio64_bgr12_to_rgb(uint16_t pix_in)
{
    uint8_t r = 0x11 * ((pix_in >> 0) & 0xf);
    uint8_t g = 0x11 * ((pix_in >> 4) & 0xf);
    uint8_t b = 0x11 * ((pix_in >> 8) & 0xf);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* RAMDAC gamma remap, ported from newport_base_device::ramdac_remap. */
static uint32_t sgi_gio64_ramdac_remap(SGIGIO64State *state, uint32_t rgb)
{
    uint32_t out = 0xff000000;

    out |= state->ramdac_lut_r[(rgb >> 16) & 0xff];
    out |= state->ramdac_lut_g[(rgb >> 8) & 0xff];
    out |= state->ramdac_lut_b[rgb & 0xff];
    return out;
}

static void sgi_gio64_decode_xmap(uint32_t table_entry, uint8_t *pix_mode,
                                  uint8_t *pix_size, uint8_t *aux_mode,
                                  uint16_t *aux_msb, uint16_t *ci_msb)
{
    *pix_mode = (table_entry >> 8) & 3;
    *pix_size = (table_entry >> 10) & 3;
    *aux_mode = (table_entry >> 16) & 7;
    *aux_msb = (table_entry >> 11) & 0x1f00;
    switch (*pix_mode) {
    case 0:
        *ci_msb = (table_entry & 0xf8) << 5;
        break;
    case 1:
        *ci_msb = 0x1d00;
        break;
    case 2:
        *ci_msb = 0x1e00;
        break;
    default:
        *ci_msb = 0x1f00;
        break;
    }
}

static bool sgi_gio64_update_display(void *opaque)
{
    sgi_gio64_gfx_sync(opaque);
    SGIGIO64State *state = opaque;
    DisplaySurface *surface = qemu_console_surface(state->console);
    uint32_t *pixels = surface_data(surface);
    const uint32_t *vram = state->vram_ptr;
    size_t vram_words = memory_region_size(&state->vram) / sizeof(*vram);
    int width = sgi_gio64_display_width(state);
    int height = sgi_gio64_display_height(state);
    int stride;
    int x, y;
    /* Head 0 drives the display (MAME screen_update uses m_xmap[0] and
     * m_cmap[0]). Pens are 0x00RRGGBB. */
    const uint32_t *cmap = state->cmap_palette[0];
    const uint32_t *auxplane = state->aux_ptr;
    uint16_t cursor_msb =
        (uint16_t)(state->xmap_cursor_cmap[0] & 0xff) << 5;
    uint16_t popup_msb =
        (uint16_t)(state->xmap_popup_cmap[0] & 0xff) << 5;
    bool is_8bpp = (state->xmap_config[0] >> 2) & 1;
    /* XMAP config bit 0 (Pup_En, xmap9.pdf Table 3i): pop-up planes
     * replace overlay/underlay for the whole screen when set; when
     * clear the pup bits are ordinary aux bits. */

    if (surface_width(surface) != width || surface_height(surface) != height) {
        qemu_console_resize(state->console, width, height);
        surface = qemu_console_surface(state->console);
        pixels = surface_data(surface);
        /* Resized surface holds no valid pixels: repaint everything. */
        sgi_gio64_damage_full(state);
    }
    /* TEMP diagnostic: with SGI_GIO_FULLREFRESH=1, re-raster every frame so
     * CPU-written VRAM becomes visible. */
    if (sgi_gio_fullrefresh_enabled()) {
        state->display_dirty = true;
        state->dmg_full = true;
    }
    if (sgi_gio_vramhash_enabled()) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        if (now - sgi_gio_vramhash_ms > 2000) {
            size_t n = (size_t)XL24_STRIDE * XL24_HEIGHT;
            uint64_t vh = sgi_gio_fnv(vram, n);
            uint64_t ah = sgi_gio_fnv(auxplane, n);

            sgi_gio_vramhash_ms = now;
            if (vh != sgi_gio_vramhash_last ||
                ah != sgi_gio_auxhash_last) {
                sgi_gio_vramhash_last = vh;
                sgi_gio_auxhash_last = ah;
                if (sgi_gio_vramhash_n < 100) {
                    sgi_gio_vramhash_n++;
                    fprintf(stderr, "VRAMHASH vram=%016llx aux=%016llx\n",
                            (unsigned long long)vh, (unsigned long long)ah);
                }
            }
        }
    }
    /* Nothing has touched the scanout since the last frame: the surface
     * still holds the last rendered image, so skip both the full-frame
     * re-raster and the console blit. */
    if (!state->display_dirty) {
        return true;
    }
    /* REX3 draws repaint only their damage rect; the per-pixel DID walk
     * stays identical (skipped columns still advance the DID chain), so
     * the output matches a full frame exactly. */
    bool full = state->dmg_full || !state->dmg_valid;
    int rx0 = full ? 0 : MAX(state->dmg_x0, 0);
    int ry0 = full ? 0 : MAX(state->dmg_y0, 0);
    int rx1 = full ? width - 1 : MIN(state->dmg_x1, width - 1);
    int ry1 = full ? height - 1 : MIN(state->dmg_y1, height - 1);

    if (rx0 > rx1 || ry0 > ry1) {
        state->display_dirty = false;
        state->dmg_full = false;
        state->dmg_valid = false;
        return true;
    }
    stride = surface_stride(surface) / sizeof(*pixels);
    bool dbg = sgi_gio_dbg();
    /* One 32-bit RGBCI word per pixel (MAME rb2): 8-bit CI occupies the low
     * byte, 12-bit the low 12 bits, 24-bit direct the low 24 bits as
     * 0x00BBGGRR. A second store holds the aux/CID plane (overlay/popup
     * bits, MAME m_cidaux), composited below. */
    /* Fire every ~30s once the RGB visual exists (same arming as the
     * segment map), so peeks stay fresh instead of going stale. */
    {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        static int64_t peek_last;
        bool peek_due = sgi_vrampeek_enabled() && sgi_modlog_seg_armed &&
                        (!sgi_vrampeek_done || now - peek_last >= 30000);

        if (!peek_due) {
            goto no_peek;
        }
        peek_last = now;
        sgi_vrampeek_done = true;
        size_t vw3 =
            memory_region_size(&state->vram) / sizeof(uint32_t);
        /* GL draw zone (67,56) and Ideas panel corner (700,650). */
        for (int py = 56; py < 60; py++) {
            for (int px = 64; px < 72; px++) {
                size_t o = (size_t)py * XL24_STRIDE + px;
                fprintf(stderr, "PEEK y=%d x=%d vram=%08x aux=%08x\n", py,
                        px, o < vw3 ? state->vram_ptr[o] : 0xeeeeeeee,
                        o < vw3 ? state->aux_ptr[o] : 0xeeeeeeee);
            }
        }
        for (int py = 648; py < 652; py++) {
            for (int px = 696; px < 704; px++) {
                size_t o = (size_t)py * XL24_STRIDE + px;
                fprintf(stderr, "PEEK y=%d x=%d vram=%08x aux=%08x\n", py,
                        px, o < vw3 ? state->vram_ptr[o] : 0xeeeeeeee,
                        o < vw3 ? state->aux_ptr[o] : 0xeeeeeeee);
            }
        }
    no_peek:;
    }
    /* Bound the full-map dumps; swaps re-arm constantly. */
    static unsigned sgi_modlog_ndump;
    if (sgi_modlog_enabled() && !sgi_modlog_dumped &&
        sgi_modlog_ndump < 4) {
        sgi_modlog_ndump++;
        sgi_modlog_dumped = true;
        size_t vwords = memory_region_size(&state->vram) / sizeof(uint32_t);
        fprintf(stderr, "DIDCFG cfg=%02x curcmap=%02x pupcmap=%02x\n",
                state->xmap_config[0] & 0xff,
                state->xmap_cursor_cmap[0] & 0xff,
                state->xmap_popup_cmap[0] & 0xff);
        for (int dy = 0; dy < height; dy += 8) {
            uint16_t did = sgi_gio64_vc2_begin_did(state, dy);
            uint32_t tbl = state->xmap_mode[0][did & 0x1f];
            size_t o64 = (size_t)dy * XL24_STRIDE + 64;
            uint32_t mv = o64 < vwords ? state->vram_ptr[o64] : 0;
            uint32_t av = o64 < vwords ? state->aux_ptr[o64] : 0;
            fprintf(stderr, "DIDROW y=%d did=%04x tbl=%06x pm=%d ps=%d aux=%d "
                    "main=%08x auxw=%08x\n",
                    dy, did, tbl & 0xffffff, (tbl >> 8) & 3,
                    (tbl >> 10) & 3, (tbl >> 16) & 7, mv, av);
        }
    }
    for (y = 0; y < height; y++) {
        uint16_t did = sgi_gio64_vc2_begin_did(state, y);
        uint32_t table = state->xmap_mode[0][did & 0x1f];
        uint8_t pix_mode, pix_size, aux_mode;
        uint16_t aux_msb, ci_msb;

        sgi_gio64_decode_xmap(table, &pix_mode, &pix_size, &aux_mode,
                              &aux_msb, &ci_msb);
        if (dbg && y == 0) {
            static unsigned frame;
            uint16_t did500 = sgi_gio64_vc2_begin_did(state, 500);
            uint32_t table500 = state->xmap_mode[0][did500 & 0x1f];
            uint8_t pm500, ps500, am500;
            uint16_t amsb500, cimsb500;

            sgi_gio64_decode_xmap(table500, &pm500, &ps500, &am500,
                                   &amsb500, &cimsb500);
            if (frame++ < 900) {
                fprintf(stderr, "CMAP f=%u did0=%03x tbl0=%08x "
                        "did500=%03x tbl500=%08x base=%03x "
                        "[00]=%06x [2e]=%06x [30]=%06x [c1]=%06x\n",
                        frame, sgi_gio64_vc2_begin_did(state, 0),
                        state->xmap_mode[0][sgi_gio64_vc2_begin_did(state, 0)
                                             & 0x1f],
                        did500, table500, cimsb500,
                        state->cmap_palette[0][cimsb500],
                        state->cmap_palette[0][cimsb500 | 0x2e],
                        state->cmap_palette[0][cimsb500 | 0x30],
                        state->cmap_palette[0][cimsb500 | 0xc1]);
            }
        }
        if (dbg && (y & 31) == 0) {
            static uint8_t dpm[32] = { [0 ... 31] = 0xff };
            static uint8_t dps[32] = { [0 ... 31] = 0xff };
            static uint8_t dau[32] = { [0 ... 31] = 0xff };
            unsigned slot = (y >> 5) & 31;

            if (dpm[slot] != pix_mode || dps[slot] != pix_size ||
                dau[slot] != aux_mode) {
                dpm[slot] = pix_mode;
                dps[slot] = pix_size;
                dau[slot] = aux_mode;
                fprintf(stderr, "DROW y=%d did=%03x table=%08x pm=%d ps=%d "
                        "aux=%d auxm=%03x cim=%03x\n",
                        y, did, table, pix_mode, pix_size, aux_mode,
                        aux_msb, ci_msb);
            }
        }
        did = sgi_gio64_vc2_next_did(state);
        bool row_hit = full || (y >= ry0 && y <= ry1);

        for (x = 0; x < width; x++) {
            uint32_t rgb = 0;
            bool replaced = false;
            uint8_t cursor;
            uint8_t byte = 0;

            if ((uint16_t)x == (did >> 5)) {
                table = state->xmap_mode[0][did & 0x1f];
                sgi_gio64_decode_xmap(table, &pix_mode, &pix_size,
                                      &aux_mode, &aux_msb, &ci_msb);
                /* TEMP segment probe (with MODELOG): log non-background
                 * segments (GL viewports, windows) with DID, mode and
                 * pixel words. Each row's first segment is the
                 * background; skipping it keeps the cap for windows. */
                {
                    static int seg_row = -1;
                    static bool seg_bg_done;
                    bool log_seg = false;

                    if (y != seg_row) {
                        seg_row = y;
                        seg_bg_done = false;
                    }
                    if (seg_bg_done) {
                        log_seg = true;
                    } else {
                        seg_bg_done = true;
                    }
                    if (log_seg && sgi_modlog_enabled() &&
                        sgi_modlog_seg_n < 600) {
                        size_t so = (size_t)y * XL24_STRIDE + x + 2;
                        size_t vw2 =
                            memory_region_size(&state->vram) /
                            sizeof(uint32_t);
                        sgi_modlog_seg_n++;
                        fprintf(stderr, "SEG y=%d x=%d did=%04x tbl=%06x "
                                "pm=%d ps=%d auxm=%d main=%08x auxw=%08x\n",
                                y, x, did, table & 0xffffff, pix_mode,
                                pix_size, aux_mode,
                                so < vw2 ? state->vram_ptr[so] : 0,
                                so < vw2 ? state->aux_ptr[so] : 0);
                    }
                }
                if (dbg && y == 500) {
                    fprintf(stderr, "DTBL y=500 x=%d did=%03x table=%08x "
                            "pm=%d ps=%d cim=%03x\n", x, did & 0x1f,
                            table, pix_mode, pix_size, ci_msb);
                }
                did = sgi_gio64_vc2_next_did(state);
            }
            if (!row_hit || x < rx0 || x > rx1) {
                continue;
            }
            cursor = sgi_gio64_cursor_pixel(state, x, y);
            if (cursor) {
                rgb = cmap[cursor_msb | cursor];
                replaced = true;
            } else {
                /* MAME screen_update priority: cursor, popup
                 * (cidaux bits 3:2), aux underlay/overlay, main pixel. */
                size_t aoff = (size_t)y * XL24_STRIDE + x;
                uint32_t cidaux = aoff < vram_words ? auxplane[aoff] : 0;

                /* Popup iff pup!=0 (iris compositor: no Pup_En gate;
                 * MAME fires on bits 6/7 with pup==0 (opaque black
                 * pen 0); that mispaints transparent regions black. */
                if ((((cidaux >> 2) & 3) != 0)) {
                    rgb = cmap[popup_msb | ((cidaux >> 2) & 3)];
                    replaced = true;
                } else if (aux_mode != 0) {
                    /* Overlay components only, transparent when zero
                     * (xmap9.pdf 3.3; iris compositor). No underlay:
                     * iris never displays underlay modes (falls through
                     * to main) and is the correct reference. */
                    if (is_8bpp) {
                        switch (aux_mode) {
                        case 2: { /* 2-bit overlay, transparent if zero */
                            uint32_t pix_in = (cidaux >> 8) & 3;

                            if (pix_in) {
                                rgb = cmap[aux_msb | pix_in];
                                replaced = true;
                            }
                            break;
                        }
                        case 6: { /* 1-bit overlay, bit by table bit 1 */
                            uint32_t shift = (table & 2) ? 9 : 8;
                            uint32_t pix_in = (cidaux >> shift) & 1;

                            if (pix_in) {
                                rgb = cmap[aux_msb | pix_in];
                                replaced = true;
                            }
                            break;
                        }
                        case 7: { /* 1-bit overlay, transparent if zero;
                                     * the 1-bit underlay half is a
                                     * main-zero fallback (see below). */
                            uint32_t pix_in = (cidaux >> 8) & 1;

                            if (pix_in) {
                                rgb = cmap[aux_msb | pix_in];
                                replaced = true;
                            }
                            break;
                        }
                        default:
                            break;
                        }
                    } else {
                        switch (aux_mode) {
                        case 2: { /* 8-bit overlay, transparent if zero */
                            uint32_t pix_in =
                                ((cidaux >> 8) & 0xf) |
                                ((cidaux >> 16) & 0xf0);

                            if (pix_in) {
                                rgb = cmap[aux_msb | pix_in];
                                replaced = true;
                            }
                            break;
                        }
                        case 6: { /* 4-bit overlay, shift by table bit 1 */
                            uint32_t shift = (table & 2) ? 12 : 8;
                            uint32_t pix_in = (cidaux >> shift) & 0xf;

                            if (pix_in) {
                                rgb = cmap[aux_msb | pix_in];
                                replaced = true;
                            }
                            break;
                        }
                        case 7: { /* 4-bit overlay, transparent if zero;
                                     * the 4-bit underlay half is a
                                     * main-zero fallback (see below). */
                            uint32_t pix_in = (cidaux >> 8) & 0xf;

                            if (pix_in) {
                                rgb = cmap[aux_msb | pix_in];
                                replaced = true;
                            }
                            break;
                        }
                        default:
                            break;
                        }
                    }
                }
            }
            if (!replaced) {
                size_t off = (size_t)y * XL24_STRIDE + x;
                uint32_t word = off < vram_words ? vram[off] : 0;

                byte = (uint8_t)word;
                switch (pix_mode) {
                case 0: /* CI through the head-0 colormap */
                    switch (pix_size) {
                    case 0: {
                        uint8_t shift = (table & 1) ? 4 : 0;

                        rgb = cmap[ci_msb | ((byte >> shift) & 0x0f)];
                        break;
                    }
                    case 1:
                        rgb = cmap[ci_msb | byte];
                        break;
                    case 2: {
                        /* MAME CI 12bpp: low 12 bits of the word, upper
                         * half selected by table bit 0. */
                        uint8_t shift = (table & 1) ? 12 : 0;
                        uint16_t pix_in = (word >> shift) & 0x0fff;

                        rgb = cmap[(ci_msb & 0x1000) | pix_in];
                        break;
                    }
                    default:
                        /* 24bpp CI view unsupported (MAME skips). */
                        break;
                    }
                    break;
                case 1: /* RGB Map0 */
                case 2: /* RGB Map1 */
                default: /* RGB Map2: iris expands every RGB mode identically */
                    switch (pix_size) {
                    case 0: {
                        uint8_t shift = (table & 1) ? 4 : 0;

                        rgb = sgi_gio64_bgr4_to_rgb(
                            (byte >> shift) & 0x0f);
                        break;
                    }
                    case 1: {
                        /* MAME RGB Map0 8bpp: buffer selected by table bit
                         * 0 (buffer 1 reads bits 8-15). */
                        uint8_t shift = (table & 1) ? 8 : 0;
                        uint8_t pix_in = (word >> shift) & 0xff;

                        rgb = sgi_gio64_bgr8_to_rgb(pix_in);
                        break;
                    }
                    case 2: {
                        /* MAME RGB 12bpp: low 12 bits of the word, upper
                         * half selected by table bit 0. */
                        uint8_t shift = (table & 1) ? 12 : 0;

                        rgb = sgi_gio64_bgr12_to_rgb(
                            (word >> shift) & 0x0fff);
                        break;
                    }
                    default: {
                        /* MAME RGB 24bpp: the word is 0x00BBGGRR. */
                        uint8_t r = (word >> 0) & 0xff;
                        uint8_t g = (word >> 8) & 0xff;
                        uint8_t b = (word >> 16) & 0xff;

                        rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                        break;
                    }
                    }
                    break;
                }
                if (dbg && (y == 320 || y == 500 || y == 832) &&
                    (x & 7) == 0) {
                    static uint8_t pbyte[3][160];
                    unsigned yi = y == 320 ? 0 : (y == 500 ? 1 : 2);

                    if (pbyte[yi][x >> 3] != byte) {
                        pbyte[yi][x >> 3] = byte;
                        fprintf(stderr, "DPX y=%d x=%d raw=%02x rgb=%06x\n",
                                y, x, byte, rgb);
                    }
                }
                if (dbg && y == 500 && x >= 320 && x <= 352) {
                    fprintf(stderr, "DPF y=500 x=%d raw=%02x\n", x, byte);
                }
                if (dbg && y == 832 && x >= 0 && x <= 16) {
                    fprintf(stderr, "DPF y=832 x=%d raw=%02x\n", x, byte);
                }
            }
            pixels[y * stride + x] = sgi_gio64_ramdac_remap(state, rgb);
        }
    }
    state->display_dirty = false;
    state->dmg_full = false;
    state->dmg_valid = false;
    /*
     * Push the surface to the UI; without this the window only repaints on
     * resize. Polled at the GUI refresh interval, so no extra timer needed.
     */
    qemu_console_update(state->console, full ? 0 : rx0, full ? 0 : ry0,
                        full ? width : rx1 - rx0 + 1,
                        full ? height : ry1 - ry0 + 1);
    return true;
}

static int sgi_gio64_post_load(void *opaque, int version_id)
{
    SGIGIO64State *state = opaque;

    state->display_dirty = true;
    state->dmg_full = true;
    /* In-flight host transfer is not migratable; restart clean. */
    state->host_staged = 0;
    state->host_len = 0;
    state->host_cursor = 0;
    /* Timers are transient; re-arm them so VRINT IRQs and the DCB
     * timeout resume after migration. */
    state->vram_ptr = memory_region_get_ram_ptr(&state->vram);
    state->aux_ptr = memory_region_get_ram_ptr(&state->aux);
    if (!state->vblank_timer) {
        state->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           sgi_gio64_vblank, state);
    }
    timer_mod_ns(state->vblank_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 1000000000ULL / 60);
    if (!state->dcb_timer) {
        state->dcb_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        sgi_gio64_dcb_timeout, state);
    }
    /* Readout is derived from VC2 RAM + VID_ENTRY; recompute so migration
     * without readout state still sizes correctly. */
    sgi_gio64_update_screen_size(state);
    return 0;
}

static const VMStateDescription vmstate_sgi_gio64 = {
    .name = "sgi-gio64",
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = sgi_gio64_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(rex3_config, SGIGIO64State),
        VMSTATE_UINT32(rex3_status, SGIGIO64State),
        VMSTATE_UINT32_ARRAY(rex3_regs, SGIGIO64State,
                             XL24_REX3_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT64(host_dataport, SGIGIO64State),
        VMSTATE_UINT32(draw_mode1, SGIGIO64State),
        VMSTATE_UINT32(draw_mode0, SGIGIO64State),
        VMSTATE_INT32(x_start, SGIGIO64State),
        VMSTATE_INT32(y_start, SGIGIO64State),
        VMSTATE_INT32(x_end, SGIGIO64State),
        VMSTATE_INT32(y_end, SGIGIO64State),
        VMSTATE_INT16(x_start_frac, SGIGIO64State),
        VMSTATE_INT16(y_start_frac, SGIGIO64State),
        VMSTATE_INT16(x_end_frac, SGIGIO64State),
        VMSTATE_INT16(y_end_frac, SGIGIO64State),
        VMSTATE_INT16(x_start_i, SGIGIO64State),
        VMSTATE_INT16(y_start_i, SGIGIO64State),
        VMSTATE_INT16(x_end_i, SGIGIO64State),
        VMSTATE_INT16(y_end_i, SGIGIO64State),
        VMSTATE_UINT32(xy_start_i, SGIGIO64State),
        VMSTATE_UINT32(xy_end_i, SGIGIO64State),
        VMSTATE_UINT32(x_start_end_i, SGIGIO64State),
        VMSTATE_UINT32(x_start_f, SGIGIO64State),
        VMSTATE_UINT32(y_start_f, SGIGIO64State),
        VMSTATE_UINT32(x_end_f, SGIGIO64State),
        VMSTATE_UINT32(y_end_f, SGIGIO64State),
        VMSTATE_INT16(x_save, SGIGIO64State),
        VMSTATE_UINT32(xy_move, SGIGIO64State),
        VMSTATE_UINT32(bres_d, SGIGIO64State),
        VMSTATE_UINT32(bres_s1, SGIGIO64State),
        VMSTATE_UINT32(bres_octant_inc1, SGIGIO64State),
        VMSTATE_UINT32(bres_round_inc2, SGIGIO64State),
        VMSTATE_UINT32(bres_e1, SGIGIO64State),
        VMSTATE_UINT32(bres_s2, SGIGIO64State),
        VMSTATE_UINT32(a_weight0, SGIGIO64State),
        VMSTATE_UINT32(a_weight1, SGIGIO64State),
        VMSTATE_UINT32(ls_mode, SGIGIO64State),
        VMSTATE_UINT32(ls_pattern_saved, SGIGIO64State),
        VMSTATE_UINT32(color_vram, SGIGIO64State),
        VMSTATE_UINT32(zero_overflow, SGIGIO64State),
        VMSTATE_UINT32(top_scanline, SGIGIO64State),
        VMSTATE_UINT32(xy_window, SGIGIO64State),
        VMSTATE_INT16(x_window, SGIGIO64State),
        VMSTATE_INT16(y_window, SGIGIO64State),
        VMSTATE_UINT32(setup, SGIGIO64State),
        VMSTATE_INT32(x_move, SGIGIO64State),
        VMSTATE_INT32(y_move, SGIGIO64State),
        VMSTATE_UINT32(xy_start, SGIGIO64State),
        VMSTATE_UINT32(xy_end, SGIGIO64State),
        VMSTATE_UINT32(color_i, SGIGIO64State),
        VMSTATE_UINT32(write_mask, SGIGIO64State),
        VMSTATE_UINT32(dcb_mode, SGIGIO64State),
        VMSTATE_UINT32(dcb_reg_select, SGIGIO64State),
        VMSTATE_UINT32(dcb_slave_select, SGIGIO64State),
        VMSTATE_UINT32(dcb_data_msw, SGIGIO64State),
        VMSTATE_UINT32(dcb_data_lsw, SGIGIO64State),
        VMSTATE_UINT32(dcb_mask, SGIGIO64State),
        VMSTATE_UINT32(line_stipple_pattern, SGIGIO64State),
        VMSTATE_UINT32(z_pattern, SGIGIO64State),
        VMSTATE_UINT32(color_back, SGIGIO64State),
        VMSTATE_UINT8(host_shift, SGIGIO64State),
        VMSTATE_INT32(curr_color_red, SGIGIO64State),
        VMSTATE_INT32(curr_color_green, SGIGIO64State),
        VMSTATE_INT32(curr_color_blue, SGIGIO64State),
        VMSTATE_INT32(curr_color_alpha, SGIGIO64State),
        VMSTATE_INT32(slope_red, SGIGIO64State),
        VMSTATE_INT32(slope_green, SGIGIO64State),
        VMSTATE_INT32(slope_blue, SGIGIO64State),
        VMSTATE_INT32(slope_alpha, SGIGIO64State),
        VMSTATE_UINT32(alpha_ref, SGIGIO64State),
        VMSTATE_UINT32(step_z, SGIGIO64State),
        VMSTATE_UINT32(z_value, SGIGIO64State),
        VMSTATE_UINT32(z_slope, SGIGIO64State),
        VMSTATE_UINT32_ARRAY(zbuffer, SGIGIO64State,
                             XL24_STRIDE * XL24_HEIGHT),
        VMSTATE_UINT8(plane_depth, SGIGIO64State),
        VMSTATE_UINT8(host_depth, SGIGIO64State),
        VMSTATE_2DARRAY(cmap_palette, SGIGIO64State, 2, 65536, 0,
                        vmstate_info_uint32, uint32_t),
        VMSTATE_UINT16_ARRAY(cmap_index, SGIGIO64State, 2),
        VMSTATE_UINT32_ARRAY(ramdac_lut_r, SGIGIO64State, 256),
        VMSTATE_UINT32_ARRAY(ramdac_lut_g, SGIGIO64State, 256),
        VMSTATE_UINT32_ARRAY(ramdac_lut_b, SGIGIO64State, 256),
        VMSTATE_UINT8(ramdac_index, SGIGIO64State),
        VMSTATE_2DARRAY(xmap_mode, SGIGIO64State, 2, 32, 0,
                        vmstate_info_uint32, uint32_t),
        VMSTATE_UINT8_ARRAY(xmap_mode_index, SGIGIO64State, 2),
        VMSTATE_UINT32_ARRAY(xmap_config, SGIGIO64State, 2),
        VMSTATE_UINT32_ARRAY(xmap_cursor_cmap, SGIGIO64State, 2),
        VMSTATE_UINT32_ARRAY(xmap_popup_cmap, SGIGIO64State, 2),
        VMSTATE_UINT16_ARRAY(vc2_regs, SGIGIO64State, 32),
        VMSTATE_UINT16_ARRAY(vc2_ram, SGIGIO64State, 0x8000),
        VMSTATE_UINT16(vc2_ram_addr, SGIGIO64State),
        VMSTATE_UINT8(vc2_reg_idx, SGIGIO64State),
        VMSTATE_UINT32_ARRAY(screenmask_x, SGIGIO64State, 5),
        VMSTATE_UINT32_ARRAY(screenmask_y, SGIGIO64State, 5),
        VMSTATE_UINT32(clip_mode, SGIGIO64State),
        VMSTATE_UINT32(draw_color, SGIGIO64State),
        VMSTATE_END_OF_LIST()
    },
};

static const GraphicHwOps sgi_gio64_graphics_ops = {
    .gfx_update = sgi_gio64_update_display,
};

static void sgi_gio64_init(Object *object)
{
    SGIGIO64State *state = SGI_GIO64(object);

    memory_region_init(&state->bus, object, "sgi-gio64.bus", GIO64_BUS_SIZE);
    /* VRAM is the internal RB2 pixel store, not a CPU window (P0-2). */
    memory_region_init_ram(&state->vram, object, "sgi-gio64.xl24.vram",
                           XL24_VRAM_SIZE, &error_abort);
    memory_region_init_ram(&state->aux, object, "sgi-gio64.xl24.aux",
                           XL24_VRAM_SIZE, &error_abort);
    memory_region_init_io(&state->xl24, object, &sgi_gio64_xl24_ops, state,
                          "sgi-gio64.xl24.rex3", XL24_REX3_SIZE);
    memory_region_add_subregion_overlap(&state->bus, XL24_REX3_OFFSET,
                                        &state->xl24, 10);
    /* The rest of the GIO window (empty EXP slots, reserved GFX offsets)
     * answers open-bus, like undecoded MAME gio64 address space. */
    memory_region_init_io(&state->gio_unmapped, object,
                          &sgi_gio64_unmapped_ops, state,
                          "sgi-gio64.unmapped", GIO64_BUS_SIZE);
    memory_region_add_subregion_overlap(&state->bus, 0,
                                        &state->gio_unmapped, 0);
    /* Power-on defaults (MAME device_reset values); display falls back to
     * 1288x1024 until VID_ENTRY programs VT. */
    memset(state->cmap_palette, 0, sizeof(state->cmap_palette));
    memset(state->cmap_index, 0, sizeof(state->cmap_index));
    memset(state->ramdac_lut_r, 0, sizeof(state->ramdac_lut_r));
    memset(state->ramdac_lut_g, 0, sizeof(state->ramdac_lut_g));
    memset(state->ramdac_lut_b, 0, sizeof(state->ramdac_lut_b));
    memset(state->xmap_mode, 0, sizeof(state->xmap_mode));
    memset(state->vc2_regs, 0, sizeof(state->vc2_regs));
    memset(state->vc2_ram, 0, sizeof(state->vc2_ram));
    memset(state->zbuffer, 0, sizeof(state->zbuffer));
    state->readout_x0 = 0;
    state->readout_y0 = 0;
    state->readout_x1 = 0;
    state->readout_y1 = 0;
    state->rex3_status = 3;
    state->draw_mode1 = 0x3002f001;
    state->plane_depth = (state->draw_mode1 >> 3) & 3;
    state->host_depth = (state->draw_mode1 >> 8) & 3;
    state->dcb_mode = 0x00000780;
    state->dcb_mask = 0xffffffff;
    state->x_window = 0x1000;
    state->y_window = 0x1000;
}

static void sgi_gio64_realize(DeviceState *device, Error **errp)
{
    SGIGIO64State *state = SGI_GIO64(device);

    sgi_gfx_threaded = getenv("SGI_GFX_SYNC") == NULL;
    qemu_mutex_init(&state->gfx_mtx);
    qemu_cond_init(&state->gfx_cond_work);
    qemu_cond_init(&state->gfx_cond_idle);
    qemu_thread_create(&state->gfx_thread, "sgi-gio64-gfx",
                       sgi_gio64_gfx_worker, state, QEMU_THREAD_DETACHED);
    /* Wait until the worker is parked in its loop, so the first GO cannot be
     * queued before the worker exists to consume it. Bounded: if the worker
     * does not start in time, fall back to synchronous draws rather than
     * hang the vCPU. */
    qemu_mutex_lock(&state->gfx_mtx);
    while (!state->gfx_ready) {
        if (!qemu_cond_timedwait(&state->gfx_cond_idle, &state->gfx_mtx,
                                 SGI_GFX_WAIT_MS)) {
            sgi_gfx_threaded = false;
            break;
        }
    }
    qemu_mutex_unlock(&state->gfx_mtx);

    qdev_init_gpio_out_named(device, state->gio_irq, "gio",
                             ARRAY_SIZE(state->gio_irq));
    state->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       sgi_gio64_vblank, state);
    timer_mod_ns(state->vblank_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                 1000000000ULL / 60);
    state->dcb_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    sgi_gio64_dcb_timeout, state);
    state->display_dirty = true;
    state->dmg_full = true;
    state->vram_ptr = memory_region_get_ram_ptr(&state->vram);
    state->aux_ptr = memory_region_get_ram_ptr(&state->aux);
    state->console = qemu_graphic_console_create(device, 0,
                                                 &sgi_gio64_graphics_ops,
                                                 state);
    sysbus_init_mmio(SYS_BUS_DEVICE(device), &state->bus);
}

static void sgi_gio64_reset(DeviceState *device)
{
    sgi_gio64_gfx_sync(SGI_GIO64(device));
    SGIGIO64State *state = SGI_GIO64(device);

    /*
     * Power-on defaults (MAME device_reset values). VRAM/zbuffer survive
     * (MAME does not clear the RB2 stores); IRQs deassert and the DCB
     * timeout cancels, while the vblank timer keeps running.
     */
    state->rex3_config = 0;
    state->rex3_status = 3;
    memset(state->rex3_regs, 0, sizeof(state->rex3_regs));
    state->host_dataport = 0;
    /* A host transfer lives inside one GO, so reset has nothing to resume. */
    state->host_staged = 0;
    state->host_len = 0;
    state->host_cursor = 0;
    state->display_dirty = true;
    state->dmg_full = true;
    state->draw_mode1 = 0x3002f001;
    state->draw_mode0 = 0;
    state->plane_depth = (state->draw_mode1 >> 3) & 3;
    state->host_depth = (state->draw_mode1 >> 8) & 3;
    state->x_start = 0;
    state->y_start = 0;
    state->x_end = 0;
    state->y_end = 0;
    state->x_start_frac = 0;
    state->y_start_frac = 0;
    state->x_end_frac = 0;
    state->y_end_frac = 0;
    state->x_start_i = 0;
    state->y_start_i = 0;
    state->x_end_i = 0;
    state->y_end_i = 0;
    state->xy_start_i = 0;
    state->xy_end_i = 0;
    state->x_start_end_i = 0;
    state->x_start_f = 0;
    state->y_start_f = 0;
    state->x_end_f = 0;
    state->y_end_f = 0;
    state->x_save = 0;
    state->xy_move = 0;
    state->bres_d = 0;
    state->bres_s1 = 0;
    state->bres_octant_inc1 = 0;
    state->bres_round_inc2 = 0;
    state->bres_e1 = 0;
    state->bres_s2 = 0;
    state->a_weight0 = 0;
    state->a_weight1 = 0;
    state->ls_mode = 0;
    state->ls_pattern_saved = 0;
    state->color_vram = 0;
    state->zero_overflow = 0;
    state->top_scanline = 0;
    state->xy_window = 0;
    state->x_window = 0x1000;
    state->y_window = 0x1000;
    state->setup = 0;
    state->x_move = 0;
    state->y_move = 0;
    state->xy_start = 0;
    state->xy_end = 0;
    state->color_i = 0;
    state->write_mask = 0;
    state->dcb_mode = 0x00000780;
    state->dcb_reg_select = 0;
    state->dcb_slave_select = 0;
    state->dcb_data_msw = 0;
    state->dcb_data_lsw = 0;
    state->dcb_mask = 0xffffffff;
    state->line_stipple_pattern = 0;
    state->z_pattern = 0;
    state->pat_bit = 31;
    state->zpat_bit = 31;
    state->color_back = 0;
    state->host_shift = 0;
    state->curr_color_red = 0;
    state->curr_color_green = 0;
    state->curr_color_blue = 0;
    state->curr_color_alpha = 0;
    state->slope_red = 0;
    state->slope_green = 0;
    state->slope_blue = 0;
    state->slope_alpha = 0;
    state->alpha_ref = 0;
    state->step_z = 0;
    state->z_value = 0;
    state->z_slope = 0;
    memset(state->cmap_palette, 0, sizeof(state->cmap_palette));
    memset(state->cmap_index, 0, sizeof(state->cmap_index));
    memset(state->ramdac_lut_r, 0, sizeof(state->ramdac_lut_r));
    memset(state->ramdac_lut_g, 0, sizeof(state->ramdac_lut_g));
    memset(state->ramdac_lut_b, 0, sizeof(state->ramdac_lut_b));
    state->ramdac_index = 0;
    memset(state->xmap_mode, 0, sizeof(state->xmap_mode));
    memset(state->xmap_mode_index, 0, sizeof(state->xmap_mode_index));
    memset(state->xmap_config, 0, sizeof(state->xmap_config));
    memset(state->xmap_cursor_cmap, 0, sizeof(state->xmap_cursor_cmap));
    memset(state->xmap_popup_cmap, 0, sizeof(state->xmap_popup_cmap));
    memset(state->vc2_regs, 0, sizeof(state->vc2_regs));
    memset(state->vc2_ram, 0, sizeof(state->vc2_ram));
    state->vc2_ram_addr = 0;
    state->vc2_reg_idx = 0;
    state->readout_x0 = 0;
    state->readout_y0 = 0;
    state->readout_x1 = 0;
    state->readout_y1 = 0;
    memset(state->screenmask_x, 0, sizeof(state->screenmask_x));
    memset(state->screenmask_y, 0, sizeof(state->screenmask_y));
    state->clip_mode = 0;
    state->draw_color = 0;
    if (state->dcb_timer) {
        timer_del(state->dcb_timer);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(state->gio_irq); i++) {
        qemu_set_irq(state->gio_irq[i], 0);
    }
}

static void sgi_gio64_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->realize = sgi_gio64_realize;
    device_class->vmsd = &vmstate_sgi_gio64;
    device_class_set_legacy_reset(device_class, sgi_gio64_reset);
}

static const TypeInfo sgi_gio64_type = {
    .name = TYPE_SGI_GIO64,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIGIO64State),
    .instance_init = sgi_gio64_init,
    .class_init = sgi_gio64_class_init,
};

static void sgi_gio64_register_types(void)
{
    type_register_static(&sgi_gio64_type);
}

type_init(sgi_gio64_register_types)

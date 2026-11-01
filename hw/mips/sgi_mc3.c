#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "sgi_mc3.h"

#define MC3_CFG_REGS 256
#define MC3_CFG_SIZE (MC3_CFG_REGS * 8)
#define MC3_DEFAULT_RAM_SIZE (512 * MiB)
#define MC3_DEFAULT_DECODE_SIZE (512 * MiB)

#define MC3_BANKENB 0x00
#define MC3_TYPE 0x01
#define MC3_REVLEVEL 0x02
#define MC3_ACCESS 0x03
#define MC3_MEMERRINT 0x04
#define MC3_EBUSERRINT 0x05
#define MC3_BISTRESULT 0x06
#define MC3_DRSCTIMEOUT 0x07
#define MC3_EBUSERROR 0x08
#define MC3_REFRESHCNT 0x09
#define MC3_LEAFCTLENB 0x0a
#define MC3_RESET 0x0f

#define MC3_BANK_BASE 0x10
#define MC3LF_ERROR 0x20
#define MC3LF_ERRORCLR 0x21
#define MC3LF_ERRADDRHI 0x22
#define MC3LF_ERRADDRLO 0x23
#define MC3LF_BIST 0x24
#define MC3LF_SYNDROME0 0x30

#define BANK_SIZE 0
#define BANK_BASE 1
#define BANK_IF 2
#define BANK_IP 3

#define MC3_TYPE_VALUE 0x02
#define MC3_REV_LEVEL 0x03
#define MC3_BIST_MA_REV 0x00030000
#define MC3_ERRINT_ENABLE 0x10000

#define MC3_MEM_ERROR_MULT 0x8
#define MC3_MEM_ERROR_READ_SBE 0x4
#define MC3_MEM_ERROR_READ_MBE 0x2
#define MC3_MEM_ERROR_PWRT_MBE 0x1
#define MC3_MEM_ERROR_IRQ (MC3_MEM_ERROR_PWRT_MBE | \
                           MC3_MEM_ERROR_READ_MBE | \
                           MC3_MEM_ERROR_READ_SBE)

#define MC3_EBUS_ERROR_SENDER_ADDR 0x8
#define MC3_EBUS_ERROR_SENDER_DATA 0x4
#define MC3_EBUS_ERROR_ADDR 0x2
#define MC3_EBUS_ERROR_DATA 0x1

typedef struct SgiMC3Bank {
    uint8_t size_code;
    uint8_t if_code;
    uint8_t ip;
    uint32_t base_reg;
    uint64_t ram_offset;
    uint64_t bytes;
    uint8_t used;
    uint8_t count;
    int8_t next;
} SgiMC3Bank;

struct SgiMC3State {
    DeviceState parent_obj;

    MemoryRegion cfg;
    MemoryRegion mem;
    MemoryRegion fast;
    bool fast_init;
    MemoryRegion *ram;
    MemoryRegion *backing_ram;
    AddressSpace ram_as;
    bool ram_as_init;

    uint64_t ram_size;
    uint64_t configured_size;
    uint64_t decode_size;

    uint8_t bankenb;
    uint8_t leafctl;
    uint8_t access;
    uint32_t memerrint;
    uint32_t ebusint;
    uint32_t bistresult;
    uint32_t drsctimeout;
    uint8_t ebuserror;
    uint32_t board_type;
    uint32_t revlevel;

    uint8_t installed_size[SGI_MC3_NUM_LEAVES][SGI_MC3_BANKS_PER_LEAF];
    SgiMC3Bank bank[SGI_MC3_NUM_LEAVES][SGI_MC3_BANKS_PER_LEAF];
    SgiMC3Bank active[SGI_MC3_NUM_LEAVES][SGI_MC3_BANKS_PER_LEAF];
    uint8_t active_bankenb;

    uint32_t leaf_error[SGI_MC3_NUM_LEAVES];
    uint64_t leaf_erraddr[SGI_MC3_NUM_LEAVES];
    uint16_t syndrome[SGI_MC3_NUM_LEAVES][4];
    uint32_t leaf_bist[SGI_MC3_NUM_LEAVES];

    qemu_irq mem_irq;
    qemu_irq ebus_irq;

    uint64_t prop_ram_size;
    uint8_t prop_interleave;
    uint8_t interleave_mode;
};

static const uint64_t mc3_bank_bytes[8] = {
    16 * MiB, 64 * MiB, 128 * MiB, 256 * MiB,
    256 * MiB, 1024 * MiB, 1024 * MiB, 0,
};

static unsigned mc3_benb(unsigned leaf, unsigned bank)
{
    return 1U << ((((bank & 0x2) >> 1) * 4) + (leaf * 2) + (bank & 1));
}

static SgiMC3Bank *mc3_bank_bpos(SgiMC3State *s, int index)
{
    if (index < 0 || index >= SGI_MC3_NUM_BANKS) {
        return NULL;
    }
    return &s->bank[(index >> 2) & 0x1][index & 0x3];
}

static void mc3_update_irq(SgiMC3State *s)
{
    bool mem = (s->leaf_error[0] | s->leaf_error[1]) & MC3_MEM_ERROR_IRQ;

    qemu_set_irq(s->mem_irq,
                 mem && (s->memerrint & MC3_ERRINT_ENABLE));
    qemu_set_irq(s->ebus_irq,
                 s->ebuserror && (s->ebusint & MC3_ERRINT_ENABLE));
}

static void mc3_update_live_layout(SgiMC3State *s)
{
    uint64_t offset = 0;
    unsigned leaf, bank, n;

    for (n = 0; n < SGI_MC3_NUM_BANKS; n++) {
        SgiMC3Bank *bnk;

        leaf = n & 1;
        bank = n >> 1;
        bnk = &s->bank[leaf][bank];

        if (bnk->size_code == SGI_MC3_NOBANK) {
            bnk->bytes = 0;
            bnk->ram_offset = 0;
            continue;
        }

        bnk->bytes = mc3_bank_bytes[bnk->size_code & 7];
        bnk->ram_offset = offset;
        offset += bnk->bytes;
    }

    s->configured_size = offset;
}

static void mc3_update_fast_path(SgiMC3State *s);

typedef struct MC3CoverageGroup {
    uint64_t base;
    uint64_t end;
    uint64_t bytes;
    unsigned ways;
    unsigned posmask;
} MC3CoverageGroup;

/*
 * The fast alias is only valid if the committed banks cover the complete
 * decode window and every address maps to the same physical backing address.
 * Check the interleave groups rather than walking 4M cache lines.
 */
static bool mc3_has_full_identity_coverage(SgiMC3State *s)
{
    MC3CoverageGroup groups[SGI_MC3_NUM_BANKS];
    unsigned n = 0;
    unsigned leaf, bank, i, j;
    uint64_t end = 0;

    if (!s->decode_size || s->decode_size > s->ram_size) {
        return false;
    }

    for (leaf = 0; leaf < SGI_MC3_NUM_LEAVES; leaf++) {
        for (bank = 0; bank < SGI_MC3_BANKS_PER_LEAF; bank++) {
            SgiMC3Bank *bnk = &s->active[leaf][bank];
            uint64_t base, bank_end;
            unsigned ways;
            unsigned bit = mc3_benb(leaf, bank);

            if (bnk->size_code == SGI_MC3_NOBANK || !bnk->bytes) {
                continue;
            }
            if (s->active_bankenb && !(s->active_bankenb & bit)) {
                continue;
            }
            if (!(s->leafctl & (1U << leaf))) {
                return false;
            }

            base = (uint64_t)bnk->base_reg << 8;
            ways = 1U << (bnk->if_code & 7);
            if (bnk->ip >= ways) {
                return false;
            }
            bank_end = base + bnk->bytes * ways;

            for (i = 0; i < n; i++) {
                if (groups[i].base == base && groups[i].ways == ways &&
                    groups[i].bytes == bnk->bytes) {
                    break;
                }
            }
            if (i == n) {
                if (n == ARRAY_SIZE(groups)) {
                    return false;
                }
                groups[n].base = base;
                groups[n].end = bank_end;
                groups[n].bytes = bnk->bytes;
                groups[n].ways = ways;
                groups[n].posmask = 0;
                n++;
            }
            groups[i].posmask |= 1U << bnk->ip;
        }
    }

    if (!n) {
        return false;
    }

    for (i = 0; i < n; i++) {
        if (groups[i].posmask != (1U << groups[i].ways) - 1) {
            return false;
        }
    }

    for (i = 0; i + 1 < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (groups[j].base < groups[i].base) {
                MC3CoverageGroup tmp = groups[i];

                groups[i] = groups[j];
                groups[j] = tmp;
            }
        }
    }

    for (i = 0; i < n; i++) {
        if (groups[i].base != end) {
            return false;
        }
        end = groups[i].end;
    }

    return end == s->decode_size;
}

static void mc3_update_fast_path(SgiMC3State *s)
{
    if (s->fast_init) {
        memory_region_set_enabled(&s->fast,
                                  mc3_has_full_identity_coverage(s));
    }
}

/*
 * Bank base/if/ip writes only update the live registers.  The layout used
 * for memory accesses is committed when BANKENB is written, matching the
 * PROM's activate_memory() step.  This keeps the old decode alive while a
 * new interleave is programmed, so reconfiguration cannot punch transient
 * holes in the low-memory window.
 */
static void mc3_commit_layout(SgiMC3State *s)
{
    uint64_t max_end = 0;
    unsigned leaf, bank;

    mc3_update_live_layout(s);
    memcpy(s->active, s->bank, sizeof(s->active));
    s->active_bankenb = s->bankenb;

    for (leaf = 0; leaf < SGI_MC3_NUM_LEAVES; leaf++) {
        for (bank = 0; bank < SGI_MC3_BANKS_PER_LEAF; bank++) {
            SgiMC3Bank *bnk = &s->active[leaf][bank];
            uint64_t base, ways, end;

            if (bnk->size_code == SGI_MC3_NOBANK) {
                continue;
            }

            base = (uint64_t)bnk->base_reg << 8;
            ways = 1ULL << (bnk->if_code & 7);
            end = base + bnk->bytes * ways;
            if (end > max_end) {
                max_end = end;
            }
        }
    }

    s->decode_size = max_end ? max_end : s->prop_ram_size;
    if (!s->decode_size) {
        s->decode_size = s->ram_size;
    }
    if (!s->decode_size) {
        s->decode_size = MC3_DEFAULT_DECODE_SIZE;
    }

    if (memory_region_size(&s->mem) != s->decode_size) {
        memory_region_transaction_begin();
        memory_region_set_size(&s->mem, s->decode_size);
        memory_region_transaction_commit();
    }

    mc3_update_fast_path(s);
}

static bool mc3_build_interleave(SgiMC3State *s, int cap, bool stable)
{
    int b, lf, cb, cl;
    bool failed = false;

    if (stable) {
        for (b = 0; b < SGI_MC3_NUM_BANKS; b++) {
            SgiMC3Bank *bnk = mc3_bank_bpos(s, b);

            bnk->used = 0;
            bnk->count = 0;
        }
    }

    for (b = 0; b < SGI_MC3_BANKS_PER_LEAF; b++) {
        for (lf = 0; lf < SGI_MC3_NUM_LEAVES; lf++) {
            int index = (lf << 2) + b;
            SgiMC3Bank *bnk = mc3_bank_bpos(s, index);
            int ilv_size, prev_bank, marked, startleaf;

            if (bnk->used || bnk->size_code == SGI_MC3_NOBANK ||
                !(s->bankenb & mc3_benb(lf, b))) {
                continue;
            }

            ilv_size = 1;
            prev_bank = index;
            bnk->used = 1;
            bnk->next = -1;
            marked = 1 << lf;
            startleaf = lf + 1;

            for (cb = b; cb < SGI_MC3_BANKS_PER_LEAF && ilv_size < cap; cb++) {
                for (cl = startleaf;
                     cl < SGI_MC3_NUM_LEAVES && ilv_size < cap; cl++) {
                    int cindex = (cl << 2) + cb;
                    SgiMC3Bank *cbnk = mc3_bank_bpos(s, cindex);
                    SgiMC3Bank *pbnk = mc3_bank_bpos(s, prev_bank);

                    if (!cbnk->used &&
                        mc3_bank_bytes[cbnk->size_code] ==
                        mc3_bank_bytes[bnk->size_code] &&
                        (s->bankenb & mc3_benb(cl, cb)) &&
                        !(marked & (1 << cl))) {
                        pbnk->next = cindex;
                        cbnk->next = -1;
                        cbnk->used = 1;
                        ilv_size++;
                        prev_bank = cindex;
                        marked |= 1 << cl;
                    }
                }
                startleaf = 0;
            }

            if (ilv_size != cap) {
                if (!stable) {
                    int cur = index;
                    int i;

                    for (i = 0; i < ilv_size; i++) {
                        SgiMC3Bank *cbnk = mc3_bank_bpos(s, cur);
                        int next;

                        cbnk->count = 0;
                        cbnk->used = 2;
                        next = cbnk->next;
                        if (i + 1 < ilv_size) {
                            cur = next;
                        }
                    }
                    failed = true;
                } else {
                    return false;
                }
            } else {
                bnk->count = ilv_size;
            }
        }
    }

    if (failed) {
        for (b = 0; b < SGI_MC3_NUM_BANKS; b++) {
            SgiMC3Bank *bnk = mc3_bank_bpos(s, b);

            if (bnk->used == 2) {
                bnk->used = 0;
            }
        }
        return false;
    }
    return true;
}

static bool mc3_apply_interleave(SgiMC3State *s)
{
    int cap = s->interleave_mode == SGI_MC3_INTLV_ONEWAY ? 1 : 8;
    bool stable = s->interleave_mode != SGI_MC3_INTLV_OPTIMAL;
    int index[SGI_MC3_NUM_BANKS];
    int num_index = 0;
    uint64_t base = 0;
    int i, j, b;

    while (!mc3_build_interleave(s, cap, stable)) {
        cap >>= 1;
        if (!cap) {
            return false;
        }
    }

    for (b = 0; b < SGI_MC3_NUM_BANKS; b++) {
        SgiMC3Bank *bnk = mc3_bank_bpos(s, b);

        if (bnk->count) {
            index[num_index++] = b;
        }
    }

    for (i = 0; i < num_index - 1; i++) {
        int max = i;
        uint64_t size_max =
            mc3_bank_bytes[mc3_bank_bpos(s, index[max])->size_code] *
            mc3_bank_bpos(s, index[max])->count;

        for (j = i + 1; j < num_index; j++) {
            uint64_t size_j =
                mc3_bank_bytes[mc3_bank_bpos(s, index[j])->size_code] *
                mc3_bank_bpos(s, index[j])->count;

            if (size_j > size_max) {
                max = j;
                size_max = size_j;
            }
        }

        if (max != i) {
            int tmp = index[max];

            index[max] = index[i];
            index[i] = tmp;
        }
    }

    for (b = 0; b < num_index; b++) {
        SgiMC3Bank *first = mc3_bank_bpos(s, index[b]);
        unsigned ilf = 0;
        unsigned n = first->count;
        int curr = index[b];
        unsigned ilp;

        while (n >>= 1) {
            ilf++;
        }

        for (ilp = 0; ilp < first->count; ilp++) {
            SgiMC3Bank *bnk = mc3_bank_bpos(s, curr);

            bnk->base_reg = (uint32_t)(base >> 8);
            bnk->if_code = ilf;
            bnk->ip = ilp;
            curr = bnk->next;
        }

        base += mc3_bank_bytes[first->size_code] * first->count;
    }

    mc3_commit_layout(s);
    return true;
}

static bool mc3_split_ram(uint64_t ram_size,
                          uint8_t installed[SGI_MC3_NUM_LEAVES]
                                           [SGI_MC3_BANKS_PER_LEAF])
{
    static const struct {
        unsigned code;
        uint64_t mib;
    } sizes[] = {
        { 0, 16 }, { 1, 64 }, { 2, 128 }, { 3, 256 }, { 5, 1024 },
    };
    uint64_t mib = ram_size / MiB;
    unsigned i, n;

    memset(installed, SGI_MC3_NOBANK, SGI_MC3_NUM_BANKS);
    if (!mib || ram_size % MiB) {
        return false;
    }

    for (i = 0; i < ARRAY_SIZE(sizes); i++) {
        if (mib % sizes[i].mib == 0 && mib / sizes[i].mib <= 4) {
            unsigned count = mib / sizes[i].mib;

            for (n = 0; n < count; n++) {
                installed[n & 1][n >> 1] = sizes[i].code;
            }
            return true;
        }
    }

    for (n = 0; n < SGI_MC3_NUM_BANKS && mib; n++) {
        for (i = ARRAY_SIZE(sizes); i-- > 0; ) {
            if (sizes[i].mib <= mib) {
                installed[n & 1][n >> 1] = sizes[i].code;
                mib -= sizes[i].mib;
                break;
            }
        }
        if (i == (unsigned)-1) {
            break;
        }
    }

    return mib == 0;
}

static void mc3_apply_installed(SgiMC3State *s)
{
    unsigned leaf, bank, n;

    s->bankenb = 0;
    s->leafctl = 0xf;

    for (n = 0; n < SGI_MC3_NUM_BANKS; n++) {
        SgiMC3Bank *bnk;

        leaf = n & 1;
        bank = n >> 1;
        bnk = &s->bank[leaf][bank];
        bnk->size_code = s->installed_size[leaf][bank];
        bnk->base_reg = 0;
        bnk->if_code = 0;
        bnk->ip = 0;
        bnk->used = 0;
        bnk->count = 0;
        bnk->next = -1;

        if (bnk->size_code != SGI_MC3_NOBANK) {
            s->bankenb |= mc3_benb(leaf, bank);
        }
    }
}

static bool mc3_translate(SgiMC3State *s, uint64_t addr, uint64_t *backing)
{
    unsigned leaf, bank;

    for (leaf = 0; leaf < SGI_MC3_NUM_LEAVES; leaf++) {
        if (!(s->leafctl & (1U << leaf))) {
            continue;
        }

        for (bank = 0; bank < SGI_MC3_BANKS_PER_LEAF; bank++) {
            SgiMC3Bank *bnk = &s->active[leaf][bank];
            uint64_t base, ways, span, line, line_in_bank, offset;

            /*
             * The IP19 PROM clears BANKENB while it probes the banks and
             * rebuilds the interleave, but keeps executing from memory.
             * Treat a zero BANKENB as "not yet constrained" so the
             * committed banks remain accessible during that window.
             */
            if (bnk->size_code == SGI_MC3_NOBANK ||
                (s->active_bankenb &&
                 !(s->active_bankenb & mc3_benb(leaf, bank)))) {
                continue;
            }

            base = (uint64_t)bnk->base_reg << 8;
            ways = 1ULL << (bnk->if_code & 7);
            span = bnk->bytes * ways;

            if (addr < base || addr >= base + span) {
                continue;
            }

            line = addr >> 7;
            if (line % ways != bnk->ip) {
                continue;
            }

            line_in_bank = (line - (base >> 7)) / ways;
            offset = line_in_bank * 128 + (addr & 0x7f);
            if (offset >= bnk->bytes) {
                continue;
            }

            /*
             * If the whole decode window fits in the attached RAM region,
             * backing is the same contiguous physical memory.  Keeping the
             * physical address here makes low memory identity and keeps
             * guest data stable across bank reconfiguration.  Only fall
             * back to packing banks when the decode window is larger than
             * the RAM region.
             */
            if (s->decode_size <= s->ram_size) {
                *backing = addr;
                return true;
            }

            offset += bnk->ram_offset;
            if (offset >= s->ram_size) {
                continue;
            }

            *backing = offset;
            return true;
        }
    }

    return false;
}

static MemTxResult mc3_mem_read(void *opaque, hwaddr addr, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    SgiMC3State *s = opaque;
    uint64_t value = 0;
    unsigned i = 0;

    while (i < size) {
        hwaddr a = addr + i;
        unsigned n = MIN(size - i, 128 - (a & 0x7f));
        uint8_t buf[8];
        uint64_t backing;
        unsigned j;

        memset(buf, 0xff, n);
        if (s->ram_as_init && mc3_translate(s, a, &backing)) {
            address_space_read(&s->ram_as, backing, attrs, buf, n);
        }

        for (j = 0; j < n; j++) {
            value = (value << 8) | buf[j];
        }
        i += n;
    }

    *data = value;
    return MEMTX_OK;
}

static MemTxResult mc3_mem_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    SgiMC3State *s = opaque;
    unsigned i = 0;

    while (i < size) {
        hwaddr a = addr + i;
        unsigned n = MIN(size - i, 128 - (a & 0x7f));
        uint8_t buf[8];
        uint64_t backing;
        unsigned j;

        for (j = 0; j < n; j++) {
            unsigned shift = (size - i - j - 1) * 8;

            buf[j] = (value >> shift) & 0xff;
        }

        if (s->ram_as_init && mc3_translate(s, a, &backing)) {
            address_space_write(&s->ram_as, backing, attrs, buf, n);
        }
        i += n;
    }

    return MEMTX_OK;
}

static const MemoryRegionOps mc3_mem_ops = {
    .read_with_attrs = mc3_mem_read,
    .write_with_attrs = mc3_mem_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static bool mc3_leaf_reg(unsigned reg, unsigned *leaf, unsigned *local)
{
    unsigned l;
    unsigned x;

    for (l = 0; l < SGI_MC3_NUM_LEAVES; l++) {
        x = reg - l * 0x40;
        if (x >= MC3_BANK_BASE && x <= MC3LF_SYNDROME0 + 3) {
            *leaf = l;
            *local = x;
            return true;
        }
    }
    return false;
}

static void mc3_board_reset(SgiMC3State *s)
{
    unsigned leaf, bank, n;

    s->access = 0x2;
    s->memerrint = 0x13322;
    s->ebusint = 0x13422;
    s->bistresult = 0;
    s->drsctimeout = 0xfffff;
    s->ebuserror = 0;

    for (n = 0; n < SGI_MC3_NUM_BANKS; n++) {
        leaf = n & 1;
        bank = n >> 1;
        s->bank[leaf][bank].size_code = s->installed_size[leaf][bank];
    }

    for (leaf = 0; leaf < SGI_MC3_NUM_LEAVES; leaf++) {
        s->leaf_error[leaf] = 0;
        s->leaf_erraddr[leaf] = 0;
        s->leaf_bist[leaf] = 0;
        memset(s->syndrome[leaf], 0, sizeof(s->syndrome[leaf]));
    }

    mc3_apply_installed(s);
    mc3_apply_interleave(s);
    mc3_update_irq(s);
}

uint64_t sgi_mc3_config_read(SgiMC3State *s, unsigned reg)
{
    unsigned leaf, local;

    switch (reg) {
    case MC3_BANKENB:
        return s->bankenb;
    case MC3_TYPE:
        return s->board_type;
    case MC3_REVLEVEL:
        return s->revlevel;
    case MC3_ACCESS:
        return s->access;
    case MC3_MEMERRINT:
        return s->memerrint;
    case MC3_EBUSERRINT:
        return s->ebusint;
    case MC3_BISTRESULT:
        return s->bistresult;
    case MC3_DRSCTIMEOUT:
        return s->drsctimeout;
    case MC3_EBUSERROR: {
        uint64_t value = s->ebuserror;

        s->ebuserror = 0;
        mc3_update_irq(s);
        return value;
    }
    case MC3_REFRESHCNT:
        return 0;
    case MC3_LEAFCTLENB:
        return s->leafctl;
    case MC3_RESET:
        return 0;
    default:
        break;
    }

    if (!mc3_leaf_reg(reg, &leaf, &local)) {
        return 0;
    }

    if (local >= MC3_BANK_BASE && local < MC3_BANK_BASE + 0x10) {
        unsigned bank = (local - MC3_BANK_BASE) / 4;
        unsigned bank_reg = (local - MC3_BANK_BASE) % 4;
        SgiMC3Bank *bnk = &s->bank[leaf][bank];

        switch (bank_reg) {
        case BANK_SIZE:
            return bnk->size_code;
        case BANK_BASE:
            return bnk->base_reg;
        case BANK_IF:
            return bnk->if_code;
        case BANK_IP:
            return bnk->ip;
        }
        return 0;
    }

    switch (local) {
    case MC3LF_ERROR:
        return s->leaf_error[leaf];
    case MC3LF_ERRORCLR: {
        uint32_t value = s->leaf_error[leaf];

        s->leaf_error[leaf] = 0;
        mc3_update_irq(s);
        return value;
    }
    case MC3LF_ERRADDRHI:
        return (s->leaf_erraddr[leaf] >> 32) & 0xff;
    case MC3LF_ERRADDRLO:
        return s->leaf_erraddr[leaf] & 0xffffffff;
    case MC3LF_BIST:
        return s->leaf_bist[leaf];
    case MC3LF_SYNDROME0:
    case MC3LF_SYNDROME0 + 1:
    case MC3LF_SYNDROME0 + 2:
    case MC3LF_SYNDROME0 + 3:
        return s->syndrome[leaf][local - MC3LF_SYNDROME0];
    default:
        return 0;
    }
}

void sgi_mc3_config_write(SgiMC3State *s, unsigned reg, uint64_t value)
{
    unsigned leaf, local;

    switch (reg) {
    case MC3_BANKENB:
        s->bankenb = value & 0xff;
        mc3_commit_layout(s);
        return;
    case MC3_TYPE:
    case MC3_REVLEVEL:
        return;
    case MC3_ACCESS:
        s->access = value & 0x7;
        return;
    case MC3_MEMERRINT:
        s->memerrint = value & 0x1ffff;
        mc3_update_irq(s);
        return;
    case MC3_EBUSERRINT:
        s->ebusint = value & 0x1ffff;
        mc3_update_irq(s);
        return;
    case MC3_BISTRESULT:
        s->bistresult = s->bistresult & 0xffff0000;
        s->leaf_bist[0] = 0x3f;
        s->leaf_bist[1] = 0x3f;
        return;
    case MC3_DRSCTIMEOUT:
        s->drsctimeout = value & 0xfffff;
        return;
    case MC3_EBUSERROR:
        s->ebuserror = 0;
        mc3_update_irq(s);
        return;
    case MC3_REFRESHCNT:
        return;
    case MC3_LEAFCTLENB:
        s->leafctl = value & 0xf;
        mc3_update_fast_path(s);
        return;
    case MC3_RESET:
        if (value & 1) {
            mc3_board_reset(s);
        }
        return;
    default:
        break;
    }

    if (!mc3_leaf_reg(reg, &leaf, &local)) {
        return;
    }

    if (local >= MC3_BANK_BASE && local < MC3_BANK_BASE + 0x10) {
        unsigned bank = (local - MC3_BANK_BASE) / 4;
        unsigned bank_reg = (local - MC3_BANK_BASE) % 4;
        SgiMC3Bank *bnk = &s->bank[leaf][bank];

        switch (bank_reg) {
        case BANK_SIZE:
            bnk->size_code = value & 0x7;
            mc3_update_live_layout(s);
            return;
        case BANK_BASE:
            bnk->base_reg = value & 0xffffffff;
            mc3_update_live_layout(s);
            return;
        case BANK_IF:
            bnk->if_code = value & 0x7;
            mc3_update_live_layout(s);
            return;
        case BANK_IP:
            bnk->ip = value & 0xf;
            mc3_update_live_layout(s);
            return;
        }
        return;
    }

    switch (local) {
    case MC3LF_ERRORCLR:
        s->leaf_error[leaf] = 0;
        mc3_update_irq(s);
        return;
    case MC3LF_BIST:
        s->leaf_bist[leaf] = value & 0xffffff;
        return;
    default:
        return;
    }
}

static MemTxResult mc3_cfg_read(void *opaque, hwaddr off, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    SgiMC3State *s = opaque;

    *data = sgi_mc3_config_read(s, off >> 3);
    return MEMTX_OK;
}

static MemTxResult mc3_cfg_write(void *opaque, hwaddr off, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    SgiMC3State *s = opaque;

    sgi_mc3_config_write(s, off >> 3, value);
    return MEMTX_OK;
}

static const MemoryRegionOps mc3_cfg_ops = {
    .read_with_attrs = mc3_cfg_read,
    .write_with_attrs = mc3_cfg_write,
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

void sgi_mc3_attach_ram(SgiMC3State *s, MemoryRegion *ram)
{
    if (s->ram_as_init) {
        address_space_destroy(&s->ram_as);
        s->ram_as_init = false;
    }

    s->backing_ram = ram;
    s->ram_size = ram ? memory_region_size(ram) : 0;

    if (ram) {
        address_space_init(&s->ram_as, ram, "sgi-mc3-ram");
        s->ram_as_init = true;

        if (!s->fast_init) {
            memory_region_init_alias(&s->fast, OBJECT(s), "sgi-mc3.fast",
                                     ram, 0, s->ram_size);
            memory_region_add_subregion_overlap(&s->mem, 0, &s->fast, 0);
            s->fast_init = true;
        }
        mc3_update_fast_path(s);
    }
}

bool sgi_mc3_configure_ram(SgiMC3State *s, uint64_t ram_size,
                           SgiMC3Interleave interleave)
{
    uint8_t installed[SGI_MC3_NUM_LEAVES][SGI_MC3_BANKS_PER_LEAF];

    if (!ram_size) {
        ram_size = s->ram_size ?: s->prop_ram_size;
    }

    if (!mc3_split_ram(ram_size, installed)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mc3: cannot fit %" PRIu64 " bytes into 8 banks\n",
                      ram_size);
        return false;
    }

    memcpy(s->installed_size, installed, sizeof(s->installed_size));
    s->interleave_mode = interleave;
    s->prop_ram_size = ram_size;
    mc3_apply_installed(s);
    return mc3_apply_interleave(s);
}

bool sgi_mc3_configure_bank(SgiMC3State *s, unsigned leaf, unsigned bank,
                            unsigned size_code, uint32_t base_reg,
                            unsigned if_code, unsigned ip)
{
    SgiMC3Bank *bnk;

    if (leaf >= SGI_MC3_NUM_LEAVES || bank >= SGI_MC3_BANKS_PER_LEAF ||
        size_code > SGI_MC3_NOBANK || if_code > 7 || ip > 15) {
        return false;
    }

    bnk = &s->bank[leaf][bank];
    bnk->size_code = size_code;
    bnk->base_reg = base_reg;
    bnk->if_code = if_code;
    bnk->ip = ip;
    s->installed_size[leaf][bank] = size_code;
    if (size_code == SGI_MC3_NOBANK) {
        s->bankenb &= ~mc3_benb(leaf, bank);
    } else {
        s->bankenb |= mc3_benb(leaf, bank);
    }
    mc3_commit_layout(s);
    return true;
}

MemoryRegion *sgi_mc3_config_region(SgiMC3State *s)
{
    return &s->cfg;
}

MemoryRegion *sgi_mc3_memory_region(SgiMC3State *s)
{
    return &s->mem;
}

void sgi_mc3_raise_mem_error(SgiMC3State *s, unsigned leaf, uint32_t status,
                             uint64_t addr, const uint16_t syndromes[4])
{
    if (leaf >= SGI_MC3_NUM_LEAVES) {
        return;
    }

    s->leaf_error[leaf] |= status & 0xf;
    s->leaf_erraddr[leaf] = addr;
    if (syndromes) {
        memcpy(s->syndrome[leaf], syndromes, sizeof(s->syndrome[leaf]));
    }
    mc3_update_irq(s);
}

static void mc3_realize(DeviceState *dev, Error **errp)
{
    SgiMC3State *s = SGI_MC3(dev);
    uint64_t layout_size = s->prop_ram_size;
    SgiMC3Interleave mode = s->prop_interleave;

    if (mode > SGI_MC3_INTLV_OPTIMAL) {
        mode = SGI_MC3_INTLV_STABLE;
    }

    if (s->ram) {
        sgi_mc3_attach_ram(s, s->ram);
    }
    if (!layout_size) {
        layout_size = s->ram_size ?: MC3_DEFAULT_RAM_SIZE;
    }

    if (!sgi_mc3_configure_ram(s, layout_size, mode)) {
        error_setg(errp, "sgi-mc3: unsupported RAM size %" PRIu64,
                   layout_size);
        return;
    }
    mc3_board_reset(s);
}

static void mc3_init(Object *obj)
{
    SgiMC3State *s = SGI_MC3(obj);

    s->board_type = MC3_TYPE_VALUE;
    s->revlevel = MC3_REV_LEVEL;
    memory_region_init_io(&s->cfg, obj, &mc3_cfg_ops, s,
                          "sgi-mc3.cfg", MC3_CFG_SIZE);
    memory_region_init_io(&s->mem, obj, &mc3_mem_ops, s,
                          "sgi-mc3.mem", MC3_DEFAULT_DECODE_SIZE);
    qdev_init_gpio_out_named(DEVICE(obj), &s->mem_irq, "mem-error", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->ebus_irq, "ebus-error", 1);
}

static void mc3_reset(DeviceState *dev)
{
    SgiMC3State *s = SGI_MC3(dev);

    mc3_board_reset(s);
}

static const Property mc3_properties[] = {
    DEFINE_PROP_LINK("ram", SgiMC3State, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("ram-size", SgiMC3State, prop_ram_size,
                       MC3_DEFAULT_RAM_SIZE),
    DEFINE_PROP_UINT8("interleave-mode", SgiMC3State, prop_interleave,
                      SGI_MC3_INTLV_STABLE),
};

static void mc3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mc3_realize;
    device_class_set_legacy_reset(dc, mc3_reset);
    device_class_set_props(dc, mc3_properties);
}

static const TypeInfo mc3_type_info = {
    .name = TYPE_SGI_MC3,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SgiMC3State),
    .instance_init = mc3_init,
    .class_init = mc3_class_init,
};

SgiMC3State *sgi_mc3_create(MemoryRegion *ram, uint64_t ram_size,
                            SgiMC3Interleave interleave)
{
    DeviceState *dev = qdev_new(TYPE_SGI_MC3);

    if (ram) {
        object_property_set_link(OBJECT(dev), "ram", OBJECT(ram),
                                 &error_fatal);
    }
    qdev_prop_set_uint64(dev, "ram-size",
                         ram_size ?: (ram ? memory_region_size(ram) :
                                      MC3_DEFAULT_RAM_SIZE));
    qdev_prop_set_uint8(dev, "interleave-mode", interleave);
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    return SGI_MC3(dev);
}

static void mc3_register_types(void)
{
    type_register_static(&mc3_type_info);
}

type_init(mc3_register_types)

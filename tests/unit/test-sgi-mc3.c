#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "qom/object.h"
#include "hw/mips/sgi_mc3.h"

#define MC3_BANKENB 0x00
#define MC3_TYPE 0x01
#define MC3_REVLEVEL 0x02
#define MC3_ACCESS 0x03
#define MC3_MEMERRINT 0x04
#define MC3_EBUSERRINT 0x05
#define MC3_BISTRESULT 0x06
#define MC3_DRSCTIMEOUT 0x07
#define MC3_EBUSERROR 0x08
#define MC3_LEAFCTLENB 0x0a
#define MC3_LEAF(_leaf, _reg) ((_leaf) * 0x40 + (_reg))
#define MC3_BANK(_leaf, _bnk, _reg) (0x10 + (_leaf) * 0x40 + (_bnk) * 4 + (_reg))

static void test_reset_values(void)
{
    SgiMC3State *s = sgi_mc3_create(NULL, 512 * MiB, SGI_MC3_INTLV_STABLE);

    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_TYPE), ==, 0x02);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_ACCESS), ==, 0x02);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_MEMERRINT), ==, 0x13322);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_EBUSERRINT), ==, 0x13422);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_DRSCTIMEOUT), ==, 0xfffff);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAFCTLENB), ==, 0xf);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANKENB), ==, 0x0f);

    sgi_mc3_config_write(s, MC3_TYPE, 0x55);
    sgi_mc3_config_write(s, MC3_REVLEVEL, 0x55);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_TYPE), ==, 0x02);

    object_unparent(OBJECT(s));
}

static void test_512m_layout(void)
{
    SgiMC3State *s = sgi_mc3_create(NULL, 512 * MiB, SGI_MC3_INTLV_STABLE);
    unsigned leaf, bank;

    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANKENB), ==, 0x0f);

    for (leaf = 0; leaf < SGI_MC3_NUM_LEAVES; leaf++) {
        for (bank = 0; bank < SGI_MC3_BANKS_PER_LEAF; bank++) {
            if (bank < 2) {
                g_assert_cmpuint(sgi_mc3_config_read(s,
                    MC3_BANK(leaf, bank, 0)), ==, 2);
                g_assert_cmpuint(sgi_mc3_config_read(s,
                    MC3_BANK(leaf, bank, 1)), ==,
                    bank ? 0x100000 : 0);
                g_assert_cmpuint(sgi_mc3_config_read(s,
                    MC3_BANK(leaf, bank, 2)), ==, 1);
                g_assert_cmpuint(sgi_mc3_config_read(s,
                    MC3_BANK(leaf, bank, 3)), ==, leaf);
            } else {
                g_assert_cmpuint(sgi_mc3_config_read(s,
                    MC3_BANK(leaf, bank, 0)), ==, SGI_MC3_NOBANK);
            }
        }
    }

    g_assert_true(sgi_mc3_configure_ram(s, 256 * MiB,
                                        SGI_MC3_INTLV_STABLE));
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANKENB), ==, 0x0f);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANK(0, 0, 0)), ==, 1);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANK(1, 1, 0)), ==, 1);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANK(0, 0, 1)), ==, 0);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANK(0, 1, 1)), ==, 0x80000);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANK(1, 2, 0)), ==,
                     SGI_MC3_NOBANK);

    object_unparent(OBJECT(s));
}

static void test_memory_forwarding(void)
{
    uint64_t size = 64 * MiB;
    uint8_t *ram = g_malloc0(size);
    MemoryRegion ram_mr;
    SgiMC3State *s;
    MemoryRegion *mr;
    uint64_t value;

    memset(&ram_mr, 0, sizeof(ram_mr));
    ram_mr.size = int128_make64(size);
    ram_mr.opaque = ram;

    s = sgi_mc3_create(NULL, size, SGI_MC3_INTLV_STABLE);
    sgi_mc3_attach_ram(s, &ram_mr);
    mr = sgi_mc3_memory_region(s);

    mr->ops->write_with_attrs(mr->opaque, 0, 0x1122334455667788ULL, 8,
                              MEMTXATTRS_UNSPECIFIED);
    g_assert_cmpmem(ram, 8, "\x11\x22\x33\x44\x55\x66\x77\x88", 8);

    mr->ops->write_with_attrs(mr->opaque, 128, 0xaabbccddeeff0011ULL, 8,
                              MEMTXATTRS_UNSPECIFIED);
    g_assert_cmpmem(ram + 128, 8,
                    "\xaa\xbb\xcc\xdd\xee\xff\x00\x11", 8);

    mr->ops->write_with_attrs(mr->opaque, 32 * MiB, 0x0102030405060708ULL, 8,
                              MEMTXATTRS_UNSPECIFIED);
    g_assert_cmpmem(ram + 32 * MiB, 8,
                    "\x01\x02\x03\x04\x05\x06\x07\x08", 8);

    mr->ops->write_with_attrs(mr->opaque, 32 * MiB + 128, 0x8877665544332211ULL,
                              8, MEMTXATTRS_UNSPECIFIED);
    g_assert_cmpmem(ram + 32 * MiB + 128, 8,
                    "\x88\x77\x66\x55\x44\x33\x22\x11", 8);

    mr->ops->read_with_attrs(mr->opaque, 0, &value, 8,
                             MEMTXATTRS_UNSPECIFIED);
    g_assert_cmphex(value, ==, 0x1122334455667788ULL);
    mr->ops->read_with_attrs(mr->opaque, 32 * MiB + 128, &value, 8,
                             MEMTXATTRS_UNSPECIFIED);
    g_assert_cmphex(value, ==, 0x8877665544332211ULL);

    g_free(ram);
    object_unparent(OBJECT(s));
}

static void mc3_mem_write(MemoryRegion *mr, uint64_t addr, uint64_t value)
{
    mr->ops->write_with_attrs(mr->opaque, addr, value, 8,
                              MEMTXATTRS_UNSPECIFIED);
}

static uint64_t mc3_mem_read(MemoryRegion *mr, uint64_t addr)
{
    uint64_t value;

    mr->ops->read_with_attrs(mr->opaque, addr, &value, 8,
                             MEMTXATTRS_UNSPECIFIED);
    return value;
}

static void check_full_mapping(SgiMC3Interleave mode, bool identity)
{
    const uint64_t size = 512 * MiB;
    const uint64_t lines = size / 128;
    uint8_t *ram = g_malloc0(size);
    uint8_t *seen = g_malloc0(lines);
    MemoryRegion ram_mr;
    SgiMC3State *s;
    MemoryRegion *mr;
    uint64_t line;

    memset(&ram_mr, 0, sizeof(ram_mr));
    ram_mr.size = int128_make64(size);
    ram_mr.opaque = ram;

    s = sgi_mc3_create(NULL, size, mode);
    sgi_mc3_attach_ram(s, &ram_mr);
    mr = sgi_mc3_memory_region(s);

    for (line = 0; line < lines; line++) {
        mc3_mem_write(mr, line * 128, line + 1);
    }

    for (line = 0; line < lines; line++) {
        g_assert_cmphex(mc3_mem_read(mr, line * 128), ==, line + 1);
    }

    if (identity) {
        for (line = 0; line < lines; line++) {
            g_assert_cmphex(ldq_be_p(ram + line * 128), ==, line + 1);
        }
    }

    /*
     * Every physical line must land in exactly one distinct backing line.
     * If banks overlapped or an interleave position was mishandled, two
     * physical lines would overwrite the same marker and some backing
     * line would be left zero.
     */
    for (line = 0; line < lines; line++) {
        uint64_t marker = ldq_be_p(ram + line * 128);
        uint64_t phys_line;

        g_assert_cmpuint(marker, >=, 1);
        g_assert_cmpuint(marker, <=, lines);
        phys_line = marker - 1;
        g_assert_cmpuint(seen[phys_line], ==, 0);
        seen[phys_line] = 1;
    }

    g_free(seen);
    g_free(ram);
    object_unparent(OBJECT(s));
}

static void test_512m_full_mapping_stable(void)
{
    check_full_mapping(SGI_MC3_INTLV_STABLE, true);
}

static void test_512m_full_mapping_oneway_identity(void)
{
    check_full_mapping(SGI_MC3_INTLV_ONEWAY, true);
}

static void test_bank_enable_and_leafctl(void)
{
    const uint64_t size = 64 * MiB;
    uint8_t *ram = g_malloc0(size);
    MemoryRegion ram_mr;
    SgiMC3State *s;
    MemoryRegion *mr;

    memset(&ram_mr, 0, sizeof(ram_mr));
    ram_mr.size = int128_make64(size);
    ram_mr.opaque = ram;

    s = sgi_mc3_create(NULL, size, SGI_MC3_INTLV_STABLE);
    sgi_mc3_attach_ram(s, &ram_mr);
    mr = sgi_mc3_memory_region(s);

    mc3_mem_write(mr, 0, 0x1122334455667788ULL);
    mc3_mem_write(mr, 128, 0x8877665544332211ULL);

    /*
     * The PROM clears BANKENB while it probes bank sizes and rebuilds the
     * interleave, but keeps executing from memory.  BANKENB == 0 therefore
     * has to leave the installed banks accessible, matching the PROM's
     * assumption that zero means "not constrained".
     */
    sgi_mc3_config_write(s, MC3_BANKENB, 0);
    g_assert_cmphex(mc3_mem_read(mr, 0), ==, 0x1122334455667788ULL);
    g_assert_cmphex(mc3_mem_read(mr, 128), ==, 0x8877665544332211ULL);
    mc3_mem_write(mr, 0, 0xa5a5a5a5a5a5a5a5ULL);
    g_assert_cmphex(ldq_be_p(ram), ==, 0xa5a5a5a5a5a5a5a5ULL);

    sgi_mc3_config_write(s, MC3_BANKENB, 0x0f);
    g_assert_cmphex(mc3_mem_read(mr, 0), ==, 0xa5a5a5a5a5a5a5a5ULL);

    /* Physical 0 is leaf 0, physical 128 is the leaf 1 interleave. */
    sgi_mc3_config_write(s, MC3_LEAFCTLENB, 0x2);
    g_assert_cmphex(mc3_mem_read(mr, 0), ==, 0xffffffffffffffffULL);
    g_assert_cmphex(mc3_mem_read(mr, 128), ==, 0x8877665544332211ULL);

    sgi_mc3_config_write(s, MC3_LEAFCTLENB, 0x1);
    g_assert_cmphex(mc3_mem_read(mr, 0), ==, 0xa5a5a5a5a5a5a5a5ULL);
    g_assert_cmphex(mc3_mem_read(mr, 128), ==, 0xffffffffffffffffULL);

    g_free(ram);
    object_unparent(OBJECT(s));
}

static void test_activation_staging(void)
{
    const uint64_t size = 64 * MiB;
    uint8_t *ram = g_malloc0(size);
    MemoryRegion ram_mr;
    SgiMC3State *s;
    MemoryRegion *mr;

    memset(&ram_mr, 0, sizeof(ram_mr));
    ram_mr.size = int128_make64(size);
    ram_mr.opaque = ram;

    s = sgi_mc3_create(NULL, size, SGI_MC3_INTLV_STABLE);
    sgi_mc3_attach_ram(s, &ram_mr);
    mr = sgi_mc3_memory_region(s);

    mc3_mem_write(mr, 0, 0x1122334455667788ULL);

    /* Bank size changes only become visible in decode on BANKENB commit. */
    sgi_mc3_config_write(s, MC3_BANK(0, 0, 0), SGI_MC3_NOBANK);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BANK(0, 0, 0)), ==,
                     SGI_MC3_NOBANK);
    g_assert_cmphex(mc3_mem_read(mr, 0), ==, 0x1122334455667788ULL);

    sgi_mc3_config_write(s, MC3_BANKENB, 0x0f);
    g_assert_cmphex(mc3_mem_read(mr, 0), ==, 0xffffffffffffffffULL);

    g_free(ram);
    object_unparent(OBJECT(s));
}

static void test_bist_and_errors(void)
{
    SgiMC3State *s = sgi_mc3_create(NULL, 512 * MiB, SGI_MC3_INTLV_STABLE);
    static const uint16_t syndrome[4] = { 0x1111, 0x2222, 0x3333, 0x4444 };

    sgi_mc3_config_write(s, MC3_BISTRESULT, 0xf);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_BISTRESULT) & 3, ==, 0);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAF(0, 0x24)), ==, 0x3f);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAF(1, 0x24)), ==, 0x3f);

    sgi_mc3_raise_mem_error(s, 0, 0x4, 0x1234567890ULL, syndrome);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAF(0, 0x20)), ==, 0x4);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAF(0, 0x22)), ==, 0x12);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAF(0, 0x23)), ==,
                     0x34567890);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAF(0, 0x30)), ==, 0x1111);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAF(0, 0x21)), ==, 0x4);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_LEAF(0, 0x21)), ==, 0);

    sgi_mc3_config_write(s, MC3_EBUSERROR, 0xf);
    g_assert_cmpuint(sgi_mc3_config_read(s, MC3_EBUSERROR), ==, 0);

    object_unparent(OBJECT(s));
}

int main(int argc, char **argv)
{
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
    g_test_add_func("/sgi-mc3/reset-values", test_reset_values);
    g_test_add_func("/sgi-mc3/512m-layout", test_512m_layout);
    g_test_add_func("/sgi-mc3/memory-forwarding", test_memory_forwarding);
    g_test_add_func("/sgi-mc3/512m-full-mapping-stable",
                    test_512m_full_mapping_stable);
    g_test_add_func("/sgi-mc3/512m-full-mapping-oneway-identity",
                    test_512m_full_mapping_oneway_identity);
    g_test_add_func("/sgi-mc3/bank-enable-and-leafctl",
                    test_bank_enable_and_leafctl);
    g_test_add_func("/sgi-mc3/activation-staging",
                    test_activation_staging);
    g_test_add_func("/sgi-mc3/bist-errors", test_bist_and_errors);
    return g_test_run();
}

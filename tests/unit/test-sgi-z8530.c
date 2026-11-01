#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/mips/sgi_z8530.h"

#define RR0_TX_EMPTY 0x04
#define RR0_RX_CHR 0x01
#define RR1_ALL_SENT 0x01
#define RR3_CHNA_TX_IP 0x10
#define RR3_CHNB_TX_IP 0x02
#define RR3_CHNB_RX_IP 0x04

static hwaddr ctrl_off(unsigned channel)
{
    return channel == SGI_Z8530_CHANNEL_A ?
           SGI_Z8530_EPC_CHA_CTRL : SGI_Z8530_EPC_CHB_CTRL;
}

static hwaddr data_off(unsigned channel)
{
    return channel == SGI_Z8530_CHANNEL_A ?
           SGI_Z8530_EPC_CHA_DATA : SGI_Z8530_EPC_CHB_DATA;
}

static void wr(SgiZ8530State *s, unsigned channel, unsigned reg,
               uint8_t value)
{
    hwaddr off = ctrl_off(channel);

    g_assert_cmpuint(reg, <, 16);
    sgi_z8530_write(s, off, reg);
    sgi_z8530_write(s, off, value);
}

static uint8_t rr(SgiZ8530State *s, unsigned channel, unsigned reg)
{
    hwaddr off = ctrl_off(channel);

    g_assert_cmpuint(reg, <, 16);
    sgi_z8530_write(s, off, reg);
    return sgi_z8530_read(s, off);
}

static void test_reset_and_pointer(void)
{
    SgiZ8530State *s = sgi_z8530_create(NULL, NULL);
    unsigned ch;

    for (ch = 0; ch < SGI_Z8530_NUM_CHANNELS; ch++) {
        g_assert_cmphex(rr(s, ch, 0) & RR0_TX_EMPTY, ==, RR0_TX_EMPTY);
        g_assert_cmphex(rr(s, ch, 0) & RR0_RX_CHR, ==, 0);
        g_assert_cmphex(rr(s, ch, 1) & RR1_ALL_SENT, ==, RR1_ALL_SENT);
        g_assert_cmphex(rr(s, ch, 2), ==, 0);
        g_assert_cmphex(rr(s, ch, 3), ==, 0);
        g_assert_cmphex(rr(s, ch, 14), ==, 0);
        g_assert_cmphex(rr(s, ch, 15), ==, 0);
    }

    wr(s, SGI_Z8530_CHANNEL_B, 2, 0x12);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 2), ==, 0x12);

    sgi_z8530_write(s, ctrl_off(SGI_Z8530_CHANNEL_B), 0x10);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 2), ==, 0x12);

    sgi_z8530_write(s, ctrl_off(SGI_Z8530_CHANNEL_B), 2);
    g_assert_cmphex(sgi_z8530_read(s, ctrl_off(SGI_Z8530_CHANNEL_B)), ==,
                    0x12);
    g_assert_cmphex(sgi_z8530_read(s, ctrl_off(SGI_Z8530_CHANNEL_B)) &
                    RR0_TX_EMPTY, ==, RR0_TX_EMPTY);

    wr(s, SGI_Z8530_CHANNEL_A, 7, 0x68);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_A, 14), ==, 0x68);
    wr(s, SGI_Z8530_CHANNEL_A, 15, 0xa5);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_A, 15), ==, 0xa5);

    object_unparent(OBJECT(s));
}

static void test_tx_and_interrupts(void)
{
    SgiZ8530State *s = sgi_z8530_create(NULL, NULL);

    wr(s, SGI_Z8530_CHANNEL_A, 1, 0x02);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_A, 3) & RR3_CHNA_TX_IP, ==,
                    RR3_CHNA_TX_IP);

    wr(s, SGI_Z8530_CHANNEL_B, 1, 0x02);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 3) & RR3_CHNB_TX_IP, ==,
                    RR3_CHNB_TX_IP);

    wr(s, SGI_Z8530_CHANNEL_B, 1, 0x10);
    wr(s, SGI_Z8530_CHANNEL_B, 14, 0x10);
    sgi_z8530_write(s, data_off(SGI_Z8530_CHANNEL_B), 0x5a);

    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 0) & RR0_RX_CHR, ==,
                    RR0_RX_CHR);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 3) & RR3_CHNB_RX_IP, ==,
                    RR3_CHNB_RX_IP);
    g_assert_cmphex(sgi_z8530_read(s, data_off(SGI_Z8530_CHANNEL_B)), ==,
                    0x5a);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 3) & RR3_CHNB_RX_IP, ==, 0);

    wr(s, SGI_Z8530_CHANNEL_B, 14, 0x10);
    sgi_z8530_write(s, data_off(SGI_Z8530_CHANNEL_B), 0xa5);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 8), ==, 0xa5);

    object_unparent(OBJECT(s));
}

static void test_wr9_reset(void)
{
    SgiZ8530State *s = sgi_z8530_create(NULL, NULL);

    wr(s, SGI_Z8530_CHANNEL_A, 2, 0x20);
    wr(s, SGI_Z8530_CHANNEL_B, 2, 0x40);
    wr(s, SGI_Z8530_CHANNEL_B, 15, 0xff);
    wr(s, SGI_Z8530_CHANNEL_B, 9, 0x09);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 2) & 0xf0, ==, 0x40);

    wr(s, SGI_Z8530_CHANNEL_B, 9, 0x40);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 2), ==, 0);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_B, 15), ==, 0);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_A, 2), ==, 0x20);

    wr(s, SGI_Z8530_CHANNEL_A, 9, 0xc8);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_A, 2), ==, 0);
    g_assert_cmphex(rr(s, SGI_Z8530_CHANNEL_A, 0) & RR0_TX_EMPTY, ==,
                    RR0_TX_EMPTY);

    object_unparent(OBJECT(s));
}

int main(int argc, char **argv)
{
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");

    g_test_add_func("/sgi-z8530/reset-pointer", test_reset_and_pointer);
    g_test_add_func("/sgi-z8530/tx-interrupts", test_tx_and_interrupts);
    g_test_add_func("/sgi-z8530/wr9-reset", test_wr9_reset);
    return g_test_run();
}

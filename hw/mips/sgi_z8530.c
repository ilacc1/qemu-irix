#include "qemu/osdep.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "qapi/error.h"

#include "sgi_z8530.h"

#define SCC_NUM_REGS 16
#define SCC_TX_FIFO_SIZE 16
#define SCC_RX_FIFO_SIZE 256

#define SCC_WR1_RX_INT_MSK 0x18
#define SCC_WR1_TX_INT_EN 0x02
#define SCC_WR3_RX_EN 0x01
#define SCC_WR9_HW_RST 0xc0
#define SCC_WR9_RST_CHNA 0x80
#define SCC_WR9_RST_CHNB 0x40
#define SCC_WR9_MIE 0x08
#define SCC_WR9_VIS 0x01
#define SCC_WR14_LCL_LPBK 0x10

#define SCC_RR0_BRK 0x80
#define SCC_RR0_TX_URUN 0x40
#define SCC_RR0_CTS 0x20
#define SCC_RR0_SYNC 0x10
#define SCC_RR0_DCD 0x08
#define SCC_RR0_TX_EMPTY 0x04
#define SCC_RR0_BRG_0_CNT 0x02
#define SCC_RR0_RX_CHR 0x01

#define SCC_RR1_CRC_ERR 0x40
#define SCC_RR1_RX_ORUN_ERR 0x20
#define SCC_RR1_PARITY_ERR 0x10
#define SCC_RR1_ERROR_MASK 0x70
#define SCC_RR1_ALL_SENT 0x01

#define SCC_RR3_CHNA_RX_IP 0x20
#define SCC_RR3_CHNA_TX_IP 0x10
#define SCC_RR3_CHNA_EXT_IP 0x08
#define SCC_RR3_CHNB_RX_IP 0x04
#define SCC_RR3_CHNB_TX_IP 0x02
#define SCC_RR3_CHNB_EXT_IP 0x01

typedef struct SgiZ8530Channel {
    SgiZ8530State *parent;
    unsigned index;
    CharFrontend chr;
    uint8_t wr[SCC_NUM_REGS];
    uint8_t reg_ptr;
    uint8_t rr1;
    uint8_t tx_fifo[SCC_TX_FIFO_SIZE];
    unsigned tx_head;
    unsigned tx_tail;
    unsigned tx_count;
    uint8_t rx_fifo[SCC_RX_FIFO_SIZE];
    unsigned rx_head;
    unsigned rx_tail;
    unsigned rx_count;
    bool tx_ip;
    bool ext_ip;
    bool cts;
    bool dcd;
} SgiZ8530Channel;

struct SgiZ8530State {
    DeviceState parent_obj;

    SgiZ8530Channel ch[SGI_Z8530_NUM_CHANNELS];
    MemoryRegion mmio;
    qemu_irq irq;
    uint8_t wr9;
    bool irq_level;
};

static bool scc_rx_irq_pending(const SgiZ8530Channel *ch)
{
    return ch->rx_count != 0 &&
           (ch->wr[1] & SCC_WR1_RX_INT_MSK) != 0;
}

static bool scc_channel_irq_pending(const SgiZ8530Channel *ch)
{
    return scc_rx_irq_pending(ch) || ch->tx_ip || ch->ext_ip;
}

static void scc_update_irq(SgiZ8530State *s)
{
    bool level = (s->wr9 & SCC_WR9_MIE) != 0 &&
                 (scc_channel_irq_pending(&s->ch[SGI_Z8530_CHANNEL_A]) ||
                  scc_channel_irq_pending(&s->ch[SGI_Z8530_CHANNEL_B]));

    if (level == s->irq_level) {
        return;
    }
    s->irq_level = level;
    qemu_set_irq(s->irq, level);
}

static void scc_tx_flush(SgiZ8530Channel *ch)
{
    while (ch->tx_count != 0) {
        uint8_t byte = ch->tx_fifo[ch->tx_tail];

        if (ch->chr.chr) {
            qemu_chr_fe_write_all(&ch->chr, &byte, 1);
        }
        ch->tx_tail = (ch->tx_tail + 1) % SCC_TX_FIFO_SIZE;
        ch->tx_count--;
    }
}

static void scc_tx_enqueue(SgiZ8530Channel *ch, uint8_t value)
{
    if (ch->tx_count == SCC_TX_FIFO_SIZE) {
        ch->tx_tail = (ch->tx_tail + 1) % SCC_TX_FIFO_SIZE;
        ch->tx_count--;
    }
    ch->tx_fifo[ch->tx_head] = value;
    ch->tx_head = (ch->tx_head + 1) % SCC_TX_FIFO_SIZE;
    ch->tx_count++;

    scc_tx_flush(ch);

    if (ch->wr[1] & SCC_WR1_TX_INT_EN) {
        ch->tx_ip = true;
    }
}

static void scc_rx_push(SgiZ8530Channel *ch, uint8_t value)
{
    if (ch->rx_count == SCC_RX_FIFO_SIZE) {
        ch->rr1 |= SCC_RR1_RX_ORUN_ERR;
        return;
    }
    ch->rx_fifo[ch->rx_head] = value;
    ch->rx_head = (ch->rx_head + 1) % SCC_RX_FIFO_SIZE;
    ch->rx_count++;
}

static uint8_t scc_rx_pop(SgiZ8530Channel *ch)
{
    uint8_t value = 0;

    if (ch->rx_count != 0) {
        value = ch->rx_fifo[ch->rx_tail];
        ch->rx_tail = (ch->rx_tail + 1) % SCC_RX_FIFO_SIZE;
        ch->rx_count--;
        qemu_chr_fe_accept_input(&ch->chr);
    }
    scc_update_irq(ch->parent);
    return value;
}

static void scc_reset_channel(SgiZ8530State *s, unsigned channel)
{
    SgiZ8530Channel *ch = &s->ch[channel];

    memset(ch->wr, 0, sizeof(ch->wr));
    ch->reg_ptr = 0;
    ch->rr1 = 0;
    ch->tx_head = 0;
    ch->tx_tail = 0;
    ch->tx_count = 0;
    ch->rx_head = 0;
    ch->rx_tail = 0;
    ch->rx_count = 0;
    ch->tx_ip = false;
    ch->ext_ip = false;
    ch->cts = true;
    ch->dcd = true;
}

static void scc_wr0_command(SgiZ8530Channel *ch, uint8_t value)
{
    switch (value & 0xf8) {
    case 0x10:
        ch->ext_ip = false;
        break;
    case 0x28:
        ch->tx_ip = false;
        break;
    case 0x30:
        ch->rr1 &= ~SCC_RR1_ERROR_MASK;
        break;
    case 0x38:
        ch->tx_ip = false;
        ch->ext_ip = false;
        break;
    default:
        break;
    }
}

static uint8_t scc_rr0_read(SgiZ8530Channel *ch)
{
    uint8_t value = 0;

    scc_tx_flush(ch);

    if (ch->tx_count == 0) {
        value |= SCC_RR0_TX_EMPTY;
    }
    if (ch->rx_count != 0) {
        value |= SCC_RR0_RX_CHR;
    }
    if (ch->cts) {
        value |= SCC_RR0_CTS;
    }
    if (ch->dcd) {
        value |= SCC_RR0_DCD;
    }
    return value;
}

static uint8_t scc_rr1_read(SgiZ8530Channel *ch)
{
    scc_tx_flush(ch);

    return ch->rr1 |
           (ch->tx_count == 0 ? SCC_RR1_ALL_SENT : 0);
}

static uint8_t scc_rr2_read(SgiZ8530State *s, SgiZ8530Channel *ch)
{
    uint8_t value = ch->wr[2];

    if (ch->index == SGI_Z8530_CHANNEL_B && (s->wr9 & SCC_WR9_VIS)) {
        unsigned status = 0;

        if (scc_rx_irq_pending(ch)) {
            status = 2;
        } else if (ch->tx_ip) {
            status = 4;
        } else if (ch->ext_ip) {
            status = 6;
        }
        value = (value & ~0x0e) | (status << 1);
    }
    return value;
}

static uint8_t scc_rr3_read(SgiZ8530State *s)
{
    SgiZ8530Channel *a = &s->ch[SGI_Z8530_CHANNEL_A];
    SgiZ8530Channel *b = &s->ch[SGI_Z8530_CHANNEL_B];
    uint8_t value = 0;

    if (scc_rx_irq_pending(a)) {
        value |= SCC_RR3_CHNA_RX_IP;
    }
    if (a->tx_ip) {
        value |= SCC_RR3_CHNA_TX_IP;
    }
    if (a->ext_ip) {
        value |= SCC_RR3_CHNA_EXT_IP;
    }
    if (scc_rx_irq_pending(b)) {
        value |= SCC_RR3_CHNB_RX_IP;
    }
    if (b->tx_ip) {
        value |= SCC_RR3_CHNB_TX_IP;
    }
    if (b->ext_ip) {
        value |= SCC_RR3_CHNB_EXT_IP;
    }
    return value;
}

static uint8_t scc_control_read(SgiZ8530State *s, SgiZ8530Channel *ch)
{
    unsigned reg = ch->reg_ptr;

    ch->reg_ptr = 0;

    switch (reg) {
    case 0:
        return scc_rr0_read(ch);
    case 1:
        return scc_rr1_read(ch);
    case 2:
        return scc_rr2_read(s, ch);
    case 3:
        return scc_rr3_read(s);
    case 8:
        return scc_rx_pop(ch);
    case 10:
        return 0;
    case 12:
        return ch->wr[12];
    case 13:
        return ch->wr[13];
    case 14:
        return ch->wr[7];
    case 15:
        return ch->wr[15];
    default:
        return 0;
    }
}

static void scc_write_wr9(SgiZ8530State *s, uint8_t value)
{
    s->wr9 = value & 0x3f;

    if ((value & SCC_WR9_HW_RST) == SCC_WR9_HW_RST) {
        scc_reset_channel(s, SGI_Z8530_CHANNEL_A);
        scc_reset_channel(s, SGI_Z8530_CHANNEL_B);
    } else if (value & SCC_WR9_RST_CHNA) {
        scc_reset_channel(s, SGI_Z8530_CHANNEL_A);
    } else if (value & SCC_WR9_RST_CHNB) {
        scc_reset_channel(s, SGI_Z8530_CHANNEL_B);
    }
    scc_update_irq(s);
}

static void scc_write_reg(SgiZ8530State *s, SgiZ8530Channel *ch,
                          unsigned reg, uint8_t value)
{
    if (reg == 9) {
        scc_write_wr9(s, value);
        return;
    }

    ch->wr[reg] = value;

    switch (reg) {
    case 0:
        scc_wr0_command(ch, value);
        break;
    case 1:
        if (!(value & SCC_WR1_TX_INT_EN)) {
            ch->tx_ip = false;
        } else if (ch->tx_count == 0) {
            ch->tx_ip = true;
        }
        break;
    case 8:
        scc_tx_enqueue(ch, value);
        break;
    default:
        break;
    }

    scc_update_irq(s);
}

static void scc_control_write(SgiZ8530State *s, SgiZ8530Channel *ch,
                              uint8_t value)
{
    unsigned ptr;

    if (ch->reg_ptr != 0) {
        ptr = ch->reg_ptr;
        ch->reg_ptr = 0;
        scc_write_reg(s, ch, ptr, value);
        return;
    }

    if ((value & 0x38) == 0x08) {
        ch->reg_ptr = (value & 0x07) | 0x08;
        return;
    }
    if ((value & 0x38) == 0 && (value & 0x07) != 0) {
        ch->reg_ptr = value & 0x07;
        return;
    }

    scc_write_reg(s, ch, 0, value);
}

static bool scc_decode_offset(hwaddr offset, unsigned *channel, bool *is_data)
{
    if (offset >= 0x20 || (offset & 0x07) != 0x07) {
        return false;
    }

    switch (offset & ~0x07ULL) {
    case 0x00:
        *channel = SGI_Z8530_CHANNEL_B;
        *is_data = false;
        return true;
    case 0x08:
        *channel = SGI_Z8530_CHANNEL_A;
        *is_data = false;
        return true;
    case 0x10:
        *channel = SGI_Z8530_CHANNEL_B;
        *is_data = true;
        return true;
    case 0x18:
        *channel = SGI_Z8530_CHANNEL_A;
        *is_data = true;
        return true;
    default:
        return false;
    }
}

uint64_t sgi_z8530_read(SgiZ8530State *s, hwaddr offset)
{
    unsigned channel;
    bool is_data;

    if (!scc_decode_offset(offset, &channel, &is_data)) {
        return 0;
    }

    if (is_data) {
        return scc_rx_pop(&s->ch[channel]);
    }
    return scc_control_read(s, &s->ch[channel]);
}

void sgi_z8530_write(SgiZ8530State *s, hwaddr offset, uint64_t value)
{
    unsigned channel;
    bool is_data;
    uint8_t val = value;

    if (!scc_decode_offset(offset, &channel, &is_data)) {
        return;
    }

    if (is_data) {
        SgiZ8530Channel *ch = &s->ch[channel];

        scc_tx_enqueue(ch, val);
        if (ch->wr[14] & SCC_WR14_LCL_LPBK) {
            scc_rx_push(ch, val);
        }
        scc_update_irq(s);
        return;
    }

    scc_control_write(s, &s->ch[channel], val);
}

static MemTxResult scc_mem_read(void *opaque, hwaddr offset, uint64_t *data,
                                unsigned size, MemTxAttrs attrs)
{
    *data = sgi_z8530_read(opaque, offset);
    return MEMTX_OK;
}

static MemTxResult scc_mem_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size, MemTxAttrs attrs)
{
    sgi_z8530_write(opaque, offset, value);
    return MEMTX_OK;
}

static const MemoryRegionOps scc_mem_ops = {
    .read_with_attrs = scc_mem_read,
    .write_with_attrs = scc_mem_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static int scc_can_receive(void *opaque)
{
    SgiZ8530Channel *ch = opaque;

    if (!ch->chr.chr) {
        return 0;
    }
    return SCC_RX_FIFO_SIZE - ch->rx_count;
}

static void scc_receive(void *opaque, const uint8_t *buf, int size)
{
    SgiZ8530Channel *ch = opaque;

    while (size-- > 0) {
        scc_rx_push(ch, *buf++);
    }
    scc_update_irq(ch->parent);
}

static void scc_event(void *opaque, QEMUChrEvent event)
{
}

MemoryRegion *sgi_z8530_memory_region(SgiZ8530State *s)
{
    return &s->mmio;
}

qemu_irq sgi_z8530_irq(SgiZ8530State *s)
{
    return s->irq;
}

void sgi_z8530_reset(SgiZ8530State *s)
{
    scc_reset_channel(s, SGI_Z8530_CHANNEL_A);
    scc_reset_channel(s, SGI_Z8530_CHANNEL_B);
    s->wr9 = 0;
    s->irq_level = false;
    qemu_set_irq(s->irq, 0);
}

void sgi_z8530_set_chardev(SgiZ8530State *s, unsigned channel, Chardev *chr)
{
    SgiZ8530Channel *ch;

    if (channel >= SGI_Z8530_NUM_CHANNELS) {
        return;
    }

    ch = &s->ch[channel];
    if (ch->chr.chr == chr) {
        return;
    }

    qemu_chr_fe_deinit(&ch->chr, false);
    if (!chr) {
        return;
    }

    qemu_chr_fe_init(&ch->chr, chr, &error_abort);
    qemu_chr_fe_set_handlers(&ch->chr, scc_can_receive, scc_receive,
                             scc_event, NULL, ch, NULL, true);
}

static void scc_instance_init(Object *obj)
{
    SgiZ8530State *s = SGI_Z8530(obj);
    unsigned i;

    for (i = 0; i < SGI_Z8530_NUM_CHANNELS; i++) {
        s->ch[i].parent = s;
        s->ch[i].index = i;
    }

    memory_region_init_io(&s->mmio, obj, &scc_mem_ops, s,
                          "sgi-z8530", SGI_Z8530_EPC_STRIDE);
    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, "irq", 1);
    sgi_z8530_reset(s);
}

static void scc_instance_finalize(Object *obj)
{
    SgiZ8530State *s = SGI_Z8530(obj);
    unsigned i;

    for (i = 0; i < SGI_Z8530_NUM_CHANNELS; i++) {
        qemu_chr_fe_deinit(&s->ch[i].chr, false);
    }
}

static void scc_device_reset(DeviceState *dev)
{
    sgi_z8530_reset(SGI_Z8530(dev));
}

static void scc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, scc_device_reset);
}

static const TypeInfo scc_type_info = {
    .name = TYPE_SGI_Z8530,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SgiZ8530State),
    .instance_init = scc_instance_init,
    .instance_finalize = scc_instance_finalize,
    .class_init = scc_class_init,
};

static void scc_register_types(void)
{
    type_register_static(&scc_type_info);
}

type_init(scc_register_types)

SgiZ8530State *sgi_z8530_create(Chardev *chr_a, Chardev *chr_b)
{
    DeviceState *dev = qdev_new(TYPE_SGI_Z8530);
    SgiZ8530State *s = SGI_Z8530(dev);

    qdev_realize_and_unref(dev, NULL, &error_fatal);
    sgi_z8530_set_chardev(s, SGI_Z8530_CHANNEL_A, chr_a);
    sgi_z8530_set_chardev(s, SGI_Z8530_CHANNEL_B, chr_b);
    return s;
}

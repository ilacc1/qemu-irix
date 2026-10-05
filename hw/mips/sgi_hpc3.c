/*
 * SGI HPC3 PIO window (IP22): channels selected by (offset>>8)&15.
 * Ch8 carries the console latch (0x33) and the 8042-style kbd/mouse pair
 * (status 0x47 / data 0x43) the PROM PS/2 driver polls; ch0 (HAL2) is a
 * register file; ch6 belongs to sgi-ioc2/ESCC. Unwired channels read 0.
 * The PROM never touches the IOC2 8042, so QEMU input events and chardev
 * bytes are both translated here to AT Set 2 scancodes and AUX-tagged
 * PS/2 mouse packets.
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "chardev/char-fe.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/nvram/eeprom93xx.h"
#include "hw/mips/sgi_hpc3.h"
#include "hw/mips/sgi_ioc2.h"
#include "target/mips/cpu.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "ui/input.h"

#define SGI_HPC3_PIO_SIZE 0x4000
#define SGI_HPC3_BBRAM_SIZE 0x20000

typedef struct SgiHpc3State SgiHpc3State;

struct SgiHpc3State {
    SysBusDevice parent_obj;
    MemoryRegion pio_iomem;
    MemoryRegion bbram_iomem;
    MemoryRegion eeprom_iomem;
    CharFrontend console;
    uint8_t pio0_data[0x100];
    /* MAME pio1_map is plain RAM (PROM scratch zeros + readback). */
    uint8_t pio1_data[0x100];
    /*
     * PIO8 backing store for non-shim offsets (FullHouse lcl bytes at
     * 0x87/0x8F/0x97/0x9B, POST scratch). MAME leaves PIO8 unmapped
     * (all-ones), but QEMU's PROM runs POST (which MAME skips), so the
     * a5/5a readback must pass to avoid its 4070 halt loop; unobserved
     * by MAME either way.
     */
    uint8_t pio8_data[0x100];
    uint8_t pio8_latch33;
    uint8_t pio8_rx_fifo[256];
    unsigned pio8_rx_head;
    unsigned pio8_rx_count;
    /*
     * Unified kbd/mouse output queue. out_aux tags mouse bytes so the
     * status AUX bit always matches the next data byte.
     */
#define SGI_HPC3_OUT_SIZE 2048
    uint8_t out_data[SGI_HPC3_OUT_SIZE];
    uint8_t out_aux[SGI_HPC3_OUT_SIZE];
    unsigned out_head;
    unsigned out_count;
    unsigned out_kbd_count;
    unsigned out_aux_count;
    /* 8042 controller state. */
    uint8_t cmd_byte;
    bool expect_cmd_data;
    bool aux_next;
    bool outbuf_next_kbd;
    bool outbuf_next_aux;
    uint8_t kbd_expect;
    uint8_t mouse_expect;
    /* PS/2 streaming-report enable. Motion packets go out only while set;
     * command replies are always answered, like real hardware. */
    bool mouse_reporting;
    uint8_t mouse_resolution;
    uint8_t mouse_sample_rate;
    uint8_t last_kbd_resp;
    uint8_t last_aux_resp;
    int mouse_x;
    int mouse_y;
    bool mouse_have_pos;
    /* Carry remainders for the mouse gain divider below. */
    int mouse_div_rem_x;
    int mouse_div_rem_y;
    uint8_t mouse_buttons;
    uint8_t bbram[0x2000];
    uint32_t bbram_size;
    char *nvram_dir;
    uint64_t bbram_clock_base_ns;
    time_t bbram_clock_base_time;
    QEMUTimer *bbram_timer;
    eeprom_t *eeprom;
    uint8_t eeprom_ctrl;
};

#define SGI_HPC3(obj) OBJECT_CHECK(SgiHpc3State, (obj), TYPE_SGI_HPC3)

static void sgi_hpc3_out_enqueue(SgiHpc3State *state, uint8_t byte,
                                 bool aux)
{
    unsigned tail;

    if (state->out_count >= ARRAY_SIZE(state->out_data)) {
        return;
    }
    tail = (state->out_head + state->out_count) %
           ARRAY_SIZE(state->out_data);
    state->out_data[tail] = byte;
    state->out_aux[tail] = aux ? 1 : 0;
    state->out_count++;
    if (aux) {
        state->out_aux_count++;
    } else {
        state->out_kbd_count++;
    }
}

static int sgi_hpc3_out_dequeue(SgiHpc3State *state, bool *aux)
{
    uint8_t byte;
    bool is_aux;

    if (!state->out_count) {
        return -1;
    }
    byte = state->out_data[state->out_head];
    is_aux = state->out_aux[state->out_head] != 0;
    state->out_head = (state->out_head + 1) %
                      ARRAY_SIZE(state->out_data);
    state->out_count--;
    if (is_aux) {
        state->out_aux_count--;
    } else {
        state->out_kbd_count--;
    }
    if (aux) {
        *aux = is_aux;
    }
    return byte;
}

static void sgi_hpc3_kbd_enqueue(SgiHpc3State *state, uint8_t byte)
{
    sgi_hpc3_out_enqueue(state, byte, false);
}

static void sgi_hpc3_kbd_respond(SgiHpc3State *state, uint8_t byte)
{
    state->last_kbd_resp = byte;
    sgi_hpc3_out_enqueue(state, byte, false);
}

static void sgi_hpc3_aux_respond(SgiHpc3State *state, uint8_t byte)
{
    state->last_aux_resp = byte;
    sgi_hpc3_out_enqueue(state, byte, true);
}

/*
 * US ASCII to AT Set 2 make code. The PROM keymap is Set 2 indexed, so the
 * text-console fallback must emit Set 2, not Set 1.
 */
static bool sgi_hpc3_ascii_to_set2(uint8_t c, uint8_t *code, bool *shift)
{
    static const struct {
        uint8_t code;
        bool shift;
    } map[128] = {
        [0x09] = { 0x0d, false }, [0x0a] = { 0x5a, false },
        [0x0d] = { 0x5a, false }, [0x1b] = { 0x76, false },
        [' '] = { 0x29, false },
        ['!'] = { 0x16, true }, ['"'] = { 0x52, true },
        ['#'] = { 0x26, true }, ['$'] = { 0x25, true },
        ['%'] = { 0x2e, true }, ['&'] = { 0x3d, true },
        ['\''] = { 0x52, false }, ['('] = { 0x46, true },
        [')'] = { 0x45, true }, ['*'] = { 0x3e, true },
        ['+'] = { 0x55, true }, [','] = { 0x41, false },
        ['-'] = { 0x4e, false }, ['.'] = { 0x49, false },
        ['/'] = { 0x4a, false }, ['0'] = { 0x45, false },
        ['1'] = { 0x16, false }, ['2'] = { 0x1e, false },
        ['3'] = { 0x26, false }, ['4'] = { 0x25, false },
        ['5'] = { 0x2e, false }, ['6'] = { 0x36, false },
        ['7'] = { 0x3d, false }, ['8'] = { 0x3e, false },
        ['9'] = { 0x46, false }, [':'] = { 0x4c, true },
        [';'] = { 0x4c, false }, ['<'] = { 0x41, true },
        ['='] = { 0x55, false }, ['>'] = { 0x49, true },
        ['?'] = { 0x4a, true }, ['@'] = { 0x1e, true },
        ['A'] = { 0x1c, true }, ['B'] = { 0x32, true },
        ['C'] = { 0x21, true }, ['D'] = { 0x23, true },
        ['E'] = { 0x24, true }, ['F'] = { 0x2b, true },
        ['G'] = { 0x34, true }, ['H'] = { 0x33, true },
        ['I'] = { 0x43, true }, ['J'] = { 0x3b, true },
        ['K'] = { 0x42, true }, ['L'] = { 0x4b, true },
        ['M'] = { 0x3a, true }, ['N'] = { 0x31, true },
        ['O'] = { 0x44, true }, ['P'] = { 0x4d, true },
        ['Q'] = { 0x15, true }, ['R'] = { 0x2d, true },
        ['S'] = { 0x1b, true }, ['T'] = { 0x2c, true },
        ['U'] = { 0x3c, true }, ['V'] = { 0x2a, true },
        ['W'] = { 0x1d, true }, ['X'] = { 0x22, true },
        ['Y'] = { 0x35, true }, ['Z'] = { 0x1a, true },
        ['['] = { 0x54, false }, ['\\'] = { 0x5d, false },
        [']'] = { 0x5b, false }, ['^'] = { 0x36, true },
        ['_'] = { 0x4e, true }, ['`'] = { 0x0e, false },
        ['a'] = { 0x1c, false }, ['b'] = { 0x32, false },
        ['c'] = { 0x21, false }, ['d'] = { 0x23, false },
        ['e'] = { 0x24, false }, ['f'] = { 0x2b, false },
        ['g'] = { 0x34, false }, ['h'] = { 0x33, false },
        ['i'] = { 0x43, false }, ['j'] = { 0x3b, false },
        ['k'] = { 0x42, false }, ['l'] = { 0x4b, false },
        ['m'] = { 0x3a, false }, ['n'] = { 0x31, false },
        ['o'] = { 0x44, false }, ['p'] = { 0x4d, false },
        ['q'] = { 0x15, false }, ['r'] = { 0x2d, false },
        ['s'] = { 0x1b, false }, ['t'] = { 0x2c, false },
        ['u'] = { 0x3c, false }, ['v'] = { 0x2a, false },
        ['w'] = { 0x1d, false }, ['x'] = { 0x22, false },
        ['y'] = { 0x35, false }, ['z'] = { 0x1a, false },
        ['{'] = { 0x54, true }, ['|'] = { 0x5d, true },
        ['}'] = { 0x5b, true }, ['~'] = { 0x0e, true },
        [0x7f] = { 0x66, false },
    };

    if (c >= ARRAY_SIZE(map) || !map[c].code) {
        return false;
    }
    *code = map[c].code;
    *shift = map[c].shift;
    return true;
}

static void sgi_hpc3_kbd_type_ascii(SgiHpc3State *state, uint8_t c)
{
    uint8_t code;
    bool shift;

    if (!sgi_hpc3_ascii_to_set2(c, &code, &shift)) {
        return;
    }
    if (shift) {
        sgi_hpc3_kbd_enqueue(state, 0x12);
    }
    sgi_hpc3_kbd_enqueue(state, code);
    sgi_hpc3_kbd_enqueue(state, 0xf0);
    sgi_hpc3_kbd_enqueue(state, code);
    if (shift) {
        sgi_hpc3_kbd_enqueue(state, 0xf0);
        sgi_hpc3_kbd_enqueue(state, 0x12);
    }
}

static void sgi_hpc3_mouse_enqueue(SgiHpc3State *state, uint8_t byte)
{
    sgi_hpc3_out_enqueue(state, byte, true);
}

static void sgi_hpc3_mouse_packet(SgiHpc3State *state, int dx, int dy)
{
    uint8_t b0 = 0x08 | (state->mouse_buttons & 0x07);

    if (dx < -128) {
        dx = -128;
    } else if (dx > 127) {
        dx = 127;
    }
    if (dy < -128) {
        dy = -128;
    } else if (dy > 127) {
        dy = 127;
    }
    if (dx < 0) {
        b0 |= 0x10;
    }
    if (dy < 0) {
        b0 |= 0x20;
    }
    sgi_hpc3_mouse_enqueue(state, b0);
    sgi_hpc3_mouse_enqueue(state, (uint8_t)dx);
    sgi_hpc3_mouse_enqueue(state, (uint8_t)dy);
}

/*
 * PS/2 keyboard device. Every command is ACKed (0xFA), including unknown
 * probe bytes, so PROM init retry loops cannot wedge; F3/ED/F0 stash
 * their continuation byte in kbd_expect.
 */
static void sgi_hpc3_kbd_command(SgiHpc3State *state, uint8_t val)
{
    switch (state->kbd_expect) {
    case 1:
    case 2:
    case 3:
        state->kbd_expect = 0;
        sgi_hpc3_kbd_respond(state, 0xfa);
        return;
    default:
        break;
    }
    switch (val) {
    case 0xff:
        sgi_hpc3_kbd_respond(state, 0xfa);
        sgi_hpc3_kbd_respond(state, 0xaa);
        break;
    case 0xfe:
        sgi_hpc3_kbd_respond(state, state->last_kbd_resp ?
                             state->last_kbd_resp : 0xfa);
        break;
    case 0xf5:
    case 0xf4:
    case 0xf6:
    case 0xf7:
    case 0xf8:
    case 0xf9:
    case 0xfb:
    case 0xfc:
    case 0xfd:
        sgi_hpc3_kbd_respond(state, 0xfa);
        break;
    case 0xf3:
        state->kbd_expect = 1;
        sgi_hpc3_kbd_respond(state, 0xfa);
        break;
    case 0xed:
        state->kbd_expect = 2;
        sgi_hpc3_kbd_respond(state, 0xfa);
        break;
    case 0xf0:
        state->kbd_expect = 3;
        sgi_hpc3_kbd_respond(state, 0xfa);
        break;
    case 0xf2:
        sgi_hpc3_kbd_respond(state, 0xfa);
        sgi_hpc3_kbd_respond(state, 0xab);
        sgi_hpc3_kbd_respond(state, 0x83);
        break;
    case 0xee:
        sgi_hpc3_kbd_respond(state, 0xee);
        break;
    default:
        sgi_hpc3_kbd_respond(state, 0xfa);
        break;
    }
}

/*
 * PS/2 mouse device (data writes following a 0xD4 status write).
 * Spontaneous packets go out only while streaming is enabled (F4);
 * command replies are always answered, like real hardware.
 */
static void sgi_hpc3_aux_command(SgiHpc3State *state, uint8_t val)
{
    if (state->mouse_expect == 1) {
        state->mouse_expect = 0;
        state->mouse_sample_rate = val;
        sgi_hpc3_aux_respond(state, 0xfa);
        return;
    }
    /* MAME hle_mouse.cpp E8: resolution takes one parameter byte. */
    if (state->mouse_expect == 2) {
        state->mouse_expect = 0;
        state->mouse_resolution = val;
        sgi_hpc3_aux_respond(state, 0xfa);
        return;
    }
    switch (val) {
    case 0xff:
        state->mouse_reporting = false;
        /* MAME hle_mouse.cpp defaults on reset. */
        state->mouse_resolution = 2;
        state->mouse_sample_rate = 100;
        sgi_hpc3_aux_respond(state, 0xfa);
        sgi_hpc3_aux_respond(state, 0xaa);
        sgi_hpc3_aux_respond(state, 0x00);
        break;
    case 0xfe:
        sgi_hpc3_aux_respond(state, state->last_aux_resp ?
                             state->last_aux_resp : 0xfa);
        break;
    case 0xf3:
        state->mouse_expect = 1;
        sgi_hpc3_aux_respond(state, 0xfa);
        break;
    case 0xf2:
        sgi_hpc3_aux_respond(state, 0xfa);
        sgi_hpc3_aux_respond(state, 0x00);
        break;
    case 0xe9:
        /* MAME hle_mouse.cpp defaults: mode 0, resolution 2, rate 100. */
        sgi_hpc3_aux_respond(state, 0xfa);
        sgi_hpc3_aux_respond(state, 0x00);
        sgi_hpc3_aux_respond(state, state->mouse_resolution);
        sgi_hpc3_aux_respond(state, state->mouse_sample_rate);
        break;
    case 0xeb:
        sgi_hpc3_aux_respond(state, 0xfa);
        sgi_hpc3_mouse_packet(state, 0, 0);
        break;
    case 0xf4:
        state->mouse_reporting = true;
        sgi_hpc3_aux_respond(state, 0xfa);
        break;
    case 0xf5:
        state->mouse_reporting = false;
        sgi_hpc3_aux_respond(state, 0xfa);
        break;
    case 0xf6:
        state->mouse_reporting = false;
        /* MAME hle_mouse.cpp defaults: mode 0, rate 100, resolution 2. */
        state->mouse_resolution = 2;
        state->mouse_sample_rate = 100;
        sgi_hpc3_aux_respond(state, 0xfa);
        break;
    case 0xe8:
        state->mouse_expect = 2;
        sgi_hpc3_aux_respond(state, 0xfa);
        break;
    case 0xf0:
    case 0xea:
    case 0xec:
    case 0xe6:
    case 0xe7:
        sgi_hpc3_aux_respond(state, 0xfa);
        break;
    default:
        /* MAME hle_mouse.cpp: unknown commands answer 0xfc error. */
        sgi_hpc3_aux_respond(state, 0xfc);
        break;
    }
}

/*
 * 8042 controller commands the PROM probes: 0xAA self-test answers 0x55,
 * 0xAB/0xA9 answer 0x00, 0x20/0x60 read/write the command byte, 0xD2/0xD3
 * queue output bytes, 0xD4 routes to the mouse. Enable/disable are
 * accepted but never gate the FIFOs: the guest polls only, and gating
 * could wedge input if a boot path leaves a device off.
 */
static void sgi_hpc3_ctrl_command(SgiHpc3State *state, uint8_t val)
{
    switch (val) {
    case 0x20:
        sgi_hpc3_kbd_respond(state, state->cmd_byte);
        break;
    case 0x60:
        state->expect_cmd_data = true;
        break;
    case 0xaa:
        sgi_hpc3_kbd_respond(state, 0x55);
        break;
    case 0xab:
    case 0xa9:
        sgi_hpc3_kbd_respond(state, 0x00);
        break;
    case 0xa7:
    case 0xa8:
    case 0xad:
    case 0xae:
        break;
    case 0xd2:
        state->outbuf_next_kbd = true;
        break;
    case 0xd3:
        state->outbuf_next_aux = true;
        break;
    case 0xd4:
        state->aux_next = true;
        break;
    case 0xc0:
        sgi_hpc3_kbd_respond(state, 0x00);
        break;
    default:
        break;
    }
}

static void sgi_hpc3_data_write(SgiHpc3State *state, uint8_t val)
{
    if (state->expect_cmd_data) {
        state->expect_cmd_data = false;
        state->cmd_byte = val;
        return;
    }
    if (state->outbuf_next_kbd) {
        state->outbuf_next_kbd = false;
        sgi_hpc3_kbd_respond(state, val);
        return;
    }
    if (state->outbuf_next_aux) {
        state->outbuf_next_aux = false;
        sgi_hpc3_aux_respond(state, val);
        return;
    }
    if (state->aux_next) {
        state->aux_next = false;
        sgi_hpc3_aux_command(state, val);
        return;
    }
    sgi_hpc3_kbd_command(state, val);
}

static void sgi_hpc3_console_put(SgiHpc3State *state, uint8_t val)
{
    /* 0x33 is the control latch, not TX data: latch it, never emit. */
    state->pio8_latch33 = val;
}

static uint64_t sgi_hpc3_pio8_read(SgiHpc3State *state, unsigned offset)
{
    if (offset == 0x33) {
        /* Bit 2 = TX ready. Bit 0 echoes the 0x30/0x01 strobes two PROM
         * pollers spin on, and reads 0 otherwise so the ROM drain loop
         * exits; a constant either way hangs one of the three. */
        return 0x04 | (state->pio8_latch33 == 0x30 ||
                       state->pio8_latch33 == 0x01 ? 0x01 : 0x00);
    }
    if (offset == 0x43) {
        /* Kbd/mouse data: queued bytes (BAT at reset, then host input)
         * first, then serial RX, else 0x00 (empty: MAME unmapped reads
         * 0, and 0xaa-idle would look like a phantom keypress aborting
         * the PROM countdown to menu). Dequeue order keeps AUX bit. */
        bool aux = false;
        int byte = sgi_hpc3_out_dequeue(state, &aux);

        if (byte >= 0) {
            return (uint64_t)(byte & 0xff);
        }
        if (state->pio8_rx_count > 0) {
            uint8_t c = state->pio8_rx_fifo[state->pio8_rx_head];

            state->pio8_rx_head = (state->pio8_rx_head + 1) %
                                  ARRAY_SIZE(state->pio8_rx_fifo);
            state->pio8_rx_count--;
            return c;
        }
        return 0x00;
    }
    if (offset == 0x47) {
        /* Kbd/mouse status: bit 0 = byte waits on 0x43, bit 5 = head byte
         * is mouse, bit 1 never set (commands complete instantly). The
         * menu wait polls for (status & 0x21). */
        uint64_t v = 0;

        if (state->out_count > 0) {
            v |= 0x01;
            if (state->out_aux[state->out_head]) {
                v |= 0x20;
            }
        }
        return v;
    }
    if (offset == 0x3b) {
        /* Console RX data; 0 when no user byte waits. */
        if (state->pio8_rx_count > 0) {
            uint8_t c = state->pio8_rx_fifo[state->pio8_rx_head];

            state->pio8_rx_head = (state->pio8_rx_head + 1) %
                                  ARRAY_SIZE(state->pio8_rx_fifo);
            state->pio8_rx_count--;
            return c;
        }
        return 0;
    }
    if (offset == 0x3f) {
        /* Console RX status: bit 0 = user byte available. */
        return state->pio8_rx_count > 0 ? 0x01 : 0x00;
    }
    if (offset == 0x50) {
        /* Early-boot kbd window; reads 0xff (open bus) so the PROM poll exits. */
        return 0xff;
    }
    if (offset == 0x58) {
        /*
         * Board ID peek (head byte if queued, else 0). MAME leaves
         * PIO8 unmapped and its unmapped reads return 0x00
         * (addrmap m_unmapval defaults to 0), so 0x00 here IS MAME
         * parity (an earlier all-ones experiment was based on a
         * misread MAME trace and wrongly set is_fullhouse=1).
         */
        if (state->out_count > 0) {
            return (uint64_t)(state->out_data[state->out_head] & 0xff);
        }
        return 0;
    }
    return state->pio8_data[offset];
}

/*
 * PIO dispatch tracing. The kernel polls PIO registers in tight wait
 * loops (e.g. the early boot keyboard-status poll hits one offset
 * 400k+ times), so cap the traces per (channel, offset) pair: a polled
 * offset still shows its loop and rate, while the trace stays bounded
 * and keeps logging distinct accesses through to the stall.
 */
#define SGI_PIO_TRACE_PER_ACCESS 2000
static unsigned sgi_pio_trace_count[16][256];

static bool sgi_pio_trace_wanted(unsigned channel, unsigned offset)
{
    if (sgi_pio_trace_count[channel][offset] >= SGI_PIO_TRACE_PER_ACCESS) {
        return false;
    }
    sgi_pio_trace_count[channel][offset]++;
    return true;
}

/* TEMP DEBUG (SGI_TRACE_PIO=1): log PIO window accesses in the same shape as
 * the MAME hpc3.cpp trace, so the two early boot sequences can be diffed
 * directly and the first divergence located. */
static bool sgi_mamepio_trace_on(void)
{
#ifdef SGI_DEBUG
    static int cached = -1;

    if (cached < 0) {
        cached = getenv("SGI_TRACE_PIO") ? 1 : 0;
    }
    return cached != 0;
#else
    return false;
#endif
}

/*
 * PIO channel 6 is the IOC2.  MAME keeps the device behind its own
 * AS_PIO6 address space, which pio_data_r()/pio_data_w() drive with a
 * dword register index; QEMU has no such space, so the window dispatcher
 * decodes the same index and calls the device directly.  The board hands
 * the device over with sgi_hpc3_set_ioc2() once it has realized it.
 */
static DeviceState *sgi_hpc3_ioc2;
static DeviceState *sgi_hpc3_pio6_kbd;

void sgi_hpc3_set_ioc2(DeviceState *ioc2)
{
    sgi_hpc3_ioc2 = ioc2;
}

void sgi_hpc3_set_pio6_kbd(DeviceState *kbd)
{
    sgi_hpc3_pio6_kbd = kbd;
}

/*
 * MAME's ioc2_device::base_map(), which the GUINNESS map includes, routes
 * PIO6 0x10/0x11 to the PS/2 keyboard controller (m_kbdc).  MAME selects
 * the sub-device by register index, because hpc3.cpp hands the device a
 * dword index and umask16(0x00ff) keeps only the low lane.
 *
 * The guest really does poll these registers from an arbitrary byte lane -
 * the IP24 kernel uses lane 3 - so a region alias cannot express it: a
 * one-byte alias only covers lane 0, and widening it breaks the controller's
 * own offset decode (hw/input/pckbd.c picks data vs status with
 * `addr & mask`, so a wider alias makes lane 3 read status).  Miss both and
 * the status register returns 0 forever, the kernel's PS/2 timeout fires and
 * it reports "PC keyboard/mouse controller diagnostic *FATLED*".  Route the
 * registers by index instead, which is what MAME does.
 */
static bool sgi_hpc3_pio6_kbd_reg(unsigned reg, hwaddr *dev_off)
{
    if (!sgi_hpc3_pio6_kbd || (reg != 0x10 && reg != 0x11)) {
        return false;
    }
    *dev_off = reg - 0x10;         /* 0 = data, 1 = status/command */
    return true;
}

static uint64_t sgi_hpc3_pio6_read_reg(unsigned reg)
{
    hwaddr dev_off;
    uint64_t v = 0;

    if (sgi_hpc3_pio6_kbd_reg(reg, &dev_off)) {
        memory_region_dispatch_read(
            sysbus_mmio_get_region(SYS_BUS_DEVICE(sgi_hpc3_pio6_kbd), 0),
            dev_off, &v, size_memop(1), MEMTXATTRS_UNSPECIFIED);
        return v;
    }
    return sgi_ioc2_pio6_read_byte(sgi_hpc3_ioc2, reg);
}

static void sgi_hpc3_pio6_write_reg(unsigned reg, uint8_t val)
{
    hwaddr dev_off;

    if (sgi_hpc3_pio6_kbd_reg(reg, &dev_off)) {
        memory_region_dispatch_write(
            sysbus_mmio_get_region(SYS_BUS_DEVICE(sgi_hpc3_pio6_kbd), 0),
            dev_off, val, size_memop(1), MEMTXATTRS_UNSPECIFIED);
        return;
    }
    sgi_ioc2_pio6_write_byte(sgi_hpc3_ioc2, reg, val);
}

static uint64_t sgi_hpc3_pio_read_do(void *opaque, hwaddr addr, unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    /*
     * MAME hpc3.cpp:715 pio_data_r(offs_t offset) is a 32-bit handler
     * over map(0x58000, 0x5bfff), so MAME passes a DWORD index: channel
     * = (offset >> 8) & 15, register = offset & 0xff.  The window is
     * therefore channel N = byte 0x400*N .. 0x400*N+0x3ff, NOT byte
     * 0x100*N.  QEMU is handed byte addresses, so convert first.
     */
    unsigned dword = addr >> 2;
    unsigned channel = (dword >> 8) & 15;
    unsigned offset = dword & 0xff;
    uint64_t rv;

    if (sgi_ip22_trace_enabled() && sgi_pio_trace_wanted(channel, offset)) {
        fprintf(stderr, "PIO-DISP-R ch=%u off=%02x\n", channel, offset);
    }
    if (channel >= 10) {
        channel = (channel & 1) ? 9 : 8;
    }

    /* Channel 6 is IOC2, reached by the board through MAME's AS_PIO6.
     * MAME's window is dword-indexed and only the low lane of each dword
     * carries data (see sgi_ioc2_pio6_read_byte()), so build the result
     * one byte at a time: a register is touched only at lane 0. */
    if (channel == 6) {
        uint64_t pio6 = 0;
        unsigned i;

        /* MAME hands the whole DWORD offset to pio_data_r(), which drives
         * the IOC2 with the register index in offset & 0xff and returns
         * the byte in bits 7:0; the lanes above it are the umask16(0x00ff)
         * filler.  A narrow guest access can land in any lane of the dword
         * and still selects that register, so honour it for a single-byte
         * access; for a wider access only lane 0 is unmasked.
         */
        for (i = 0; i < size; i++) {
            if (size == 1) {
                pio6 = sgi_hpc3_pio6_read_reg(((addr + i) >> 2) & 0xff);
            } else if (i == 0) {
                pio6 |= (uint64_t)sgi_hpc3_pio6_read_reg(
                            ((addr + i) >> 2) & 0xff) << (8 * i);
            }
        }
        return pio6;
    }
    if (channel == 8) {
        rv = sgi_hpc3_pio8_read(state, offset);
        return rv;
    }
    if (channel == 0) {
        /* MAME indy_indigo2.cpp:245 pio0_map is the HAL2:
         *   map(0x00, 0xff).rw(m_hal2, hal2_device::read/write)
         * and hal2.h numbers its registers in dwords
         * (STATUS_REG = 0x0010/4 = 4, REVISION_REG = 0x0020/4 = 8), so
         * `offset` is the register index, not a byte address.  Only
         * REVISION_REG is read-only and hard-wired; everything else is
         * the ISR/indirect-register backing store below.
         */
        if (offset == 0x08) {
            /* MAME hal2.cpp: hal2_device::read(REVISION_REG) -> 0x4010. */
            return 0x4010;
        }
        /* The two shims below are not MAME-derived: they stand in for
         * status bytes the guest polls, which MAME reaches through devices
         * this board does not wire up.  They are keyed on the raw byte
         * address, so with the dword decode they now fire only at that one
         * address (the pre-decode byte-offset test also caught 0x13b/0x23b/
         * 0x33b).  Neither is hit in practice - a full early-boot trace shows
         * the only channel 0 reads are byte 0x0010 (register 4) and byte
         * 0x0020 (REVISION_REG) - so keep them as a fallback rather than
         * relying on them. */
        if (addr == 0x3b) {
            /*
             * Serial-console status (PROM 0x31490 autobaud/configure):
             * bit 5 = TX ready so its transmit/wait loops proceed
             * instead of polling forever on an unresponsive status.
             * MAME never executes that code (graphics console skips
             * it), so this is outcome-parity (proceed, no wait).
             */
            return 0x20;
        }
        if (addr == 0x0b) {
            /* Kbd/mouse status mirror: bit 0 = keyboard byte waits,
             * bit 5 = mouse byte waits; 0 when idle. */
            uint64_t v = 0;

            if (state->out_kbd_count > 0) {
                v |= 0x01;
            }
            if (state->out_aux_count > 0) {
                v |= 0x20;
            }
            return v;
        }
        rv = state->pio0_data[offset];
        return rv;
    }
    if (channel == 1) {
        return state->pio1_data[offset];
    }
    return 0;
}

static void sgi_hpc3_pio_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    /* See sgi_hpc3_pio_read_do(): MAME decodes on a DWORD index. */
    unsigned dword = addr >> 2;
    unsigned channel = (dword >> 8) & 15;
    unsigned offset = dword & 0xff;

    if (sgi_ip22_trace_enabled() && sgi_pio_trace_wanted(channel, offset)) {
        fprintf(stderr, "PIO-DISP-W ch=%u off=%02x val=%x\n",
                channel, offset, (unsigned)val);
    }
    if (sgi_mamepio_trace_on()) {
        fprintf(stderr, "QEMU-PIO-W ch=%u off=%02x addr=%04x size=%u val=%08x\n",
                channel, offset, (unsigned)addr, size, (unsigned)val);
    }

    if (channel >= 10) {
        channel = (channel & 1) ? 9 : 8;
    }

    /* Channel 6 is IOC2; same dword/lane rules as the read path, and
     * MAME's pio_data_w() case 0 writes `data & 0x00ff` to the register. */
    if (channel == 6) {
        unsigned i;

        for (i = 0; i < size; i++) {
            if (size == 1 || i == 0) {
                sgi_hpc3_pio6_write_reg(((addr + i) >> 2) & 0xff,
                                       (val >> (8 * i)) & 0xff);
            }
        }
        return;
    }
    if (channel == 8) {
        if (offset == 0x33) {
            sgi_hpc3_console_put(state, val);
        } else if (offset == 0x37) {
            /* Text console data (plain ASCII, unlike the 0x33 latch). */
            uint8_t c = val;

            if (c != 0x00) {
                qemu_chr_fe_write_all(&state->console, &c, 1);
            }
        } else if (offset == 0x43) {
            sgi_hpc3_data_write(state, val & 0xff);
        } else if (offset == 0x47) {
            sgi_hpc3_ctrl_command(state, val & 0xff);
        } else if (offset == 0x50) {
            /* Early-boot window; ignored so no phantom keypress results. */
            (void)val;
        } else if (offset == 0xbb || offset == 0xbf) {
            /*
             * PIO8 serial-config status (PROM 0x31490 autobaud measure):
             * must read back 0 so the combined measurement stays below
             * its 10000 retry threshold; stored nonzeros (e.g. 39 give
             * 0x2727) retry forever. MAME never executes this code, so
             * this is outcome-parity (proceed) rather than value-parity.
             */
            (void)val;
        } else {
            state->pio8_data[offset] = val;
        }
        return;
    }
    if (channel == 0) {
        state->pio0_data[offset] = val;
        return;
    }
    if (channel == 1) {
        state->pio1_data[offset] = val;
        return;
    }
}

static uint64_t sgi_hpc3_pio_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t rv = sgi_hpc3_pio_read_do(opaque, addr, size);

    if (sgi_mamepio_trace_on()) {
        uint32_t pc = 0;

        if (current_cpu) {
            pc = (uint32_t)((ArchCPU *)current_cpu)->env.active_tc.PC;
        }
        unsigned dword = addr >> 2;

        fprintf(stderr, "QEMU-PIO-R ch=%u off=%02x addr=%04x size=%u ret=%08x pc=%08x\n",
                (unsigned)((dword >> 8) & 15), (unsigned)(dword & 0xff),
                (unsigned)addr, size, (unsigned)rv, pc);
    }
    return rv;
}

static const MemoryRegionOps sgi_hpc3_pio_ops = {
    .read = sgi_hpc3_pio_read,
    .write = sgi_hpc3_pio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* MAME time_helper::to_bcd() / from_bcd(), used by ds1386.cpp for its BCD
 * clock registers. */
static uint8_t time_helper_to_bcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}

static uint8_t time_helper_from_bcd(uint8_t value)
{
    return (value >> 4) * 10 + (value & 0x0f);
}

/* DS1386 timekeeper advance rate: 100Hz, per MAME ds1386.cpp:77. */
#define SGI_BBRAM_TICK_NS 10000000ULL

static void sgi_hpc3_bbram_tick(void *opaque);

static uint64_t sgi_bbram_trace_n;

/* TEMPORARY DEBUG (SGI_TRACE_BBRAM=1): log every DS1386 window access so we
 * can see the offsets/sizes the PROM uses for its env image. */
static bool sgi_bbram_trace_on(void)
{
#ifdef SGI_DEBUG
    static int cached = -1;

    if (cached < 0) {
        cached = getenv("SGI_TRACE_BBRAM") ? 1 : 0;
    }
    return cached != 0;
#else
    return false;
#endif
}

static uint64_t sgi_hpc3_bbram_read(void *opaque, hwaddr addr, unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    unsigned reg;
    uint8_t v;

    /* MAME hpc3.cpp:201 registers bbram_r/bbram_w as 32-bit handlers over
     * the whole 128K window, so the devcb_read8 -> ds1386::data_r callback
     * is handed the *DWORD* index, not a byte offset.  ds1386.cpp:
     *   uint8_t ds1386_device::data_r(offs_t offset)
     *   {
     *       if (offset >= m_ram_size) return 0;
     *       return m_ram[offset];
     *   }
     * i.e. one byte, zero-extended, regardless of access width.
     *
     * Getting this wrong is not cosmetic: with byte offsets the IP24 PROM's
     * 32-bit read of 0x1fbe0004 landed on ds1386 register 4 (hours) instead
     * of register 1 (seconds), and its "wait for the clock to change" loop at
     * PROM 0xbfc31248 then spun until the hour field rolled over - an
     * apparent hang.  Confirmed against MAME's own trace, whose offsets are
     * exactly one quarter of ours (QEMU 0x00fb/0x00ff == MAME 0x003e/0x003f).
     */
    reg = addr >> 2;
    if (reg >= state->bbram_size) {
        return 0;
    }
    v = state->bbram[reg];

    if (sgi_bbram_trace_on() && sgi_bbram_trace_n < 4000) {
        sgi_bbram_trace_n++;
        fprintf(stderr, "BBRAM R off=0x%04x size=%u -> 0x%02x\n",
                (unsigned)reg, size, (unsigned)v);
    }
    return v;
}

static uint8_t sgi_hpc3_inc_bcd(uint8_t *v, uint8_t max, uint8_t min_value,
                                uint8_t max_value)
{
    uint8_t value = time_helper_from_bcd(*v) + 1;
    uint8_t limit = time_helper_from_bcd(max_value);

    if (value > limit) {
        *v = time_helper_to_bcd(min_value);
        return 1;
    }
    *v = time_helper_to_bcd(value);
    return 0;
}

/* Mirror of MAME ds1386_device::advance_hundredths() (ds1386.cpp:200). */
static void sgi_hpc3_bbram_tick(void *opaque)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    static const uint8_t daysinmonth[] = {
        0x31, 0x28, 0x31, 0x30, 0x31, 0x30,
        0x31, 0x31, 0x30, 0x31, 0x30, 0x31
    };
    uint8_t carry, month, year, maxdays;

    /* Keep the 100Hz cadence MAME ds1386.cpp:77 gets from its repeating
     * timer; this timer API has no period argument. */
    timer_mod_ns(state->bbram_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SGI_BBRAM_TICK_NS);

    if (state->bbram_size < 0x0b) {
        return;
    }

    carry = sgi_hpc3_inc_bcd(&state->bbram[0x00], 0xff, 0x00, 0x99);
    if (carry) {
        carry = sgi_hpc3_inc_bcd(&state->bbram[0x01], 0x7f, 0x00, 0x59);
    }
    if (carry) {
        carry = sgi_hpc3_inc_bcd(&state->bbram[0x02], 0x7f, 0x00, 0x59);
    }
    if (carry) {
        uint8_t value = 0, min_value = 0, max_value = 0;
        if (state->bbram[0x04] & 0x40) {       /* HOURS_12_24 */
            value = state->bbram[0x04] & 0x1f;
            min_value = 0x01;
            max_value = 0x12;
        } else {
            value = state->bbram[0x04] & 0x3f;
            min_value = 0x00;
            max_value = 0x23;
        }
        carry = sgi_hpc3_inc_bcd(&value, 0x1f, min_value, max_value);
        state->bbram[0x04] &= ~0x1f;
        if (carry) {
            if (state->bbram[0x04] & 0x40) {   /* HOURS_12_24 */
                carry = (state->bbram[0x04] & 0x80) ? 1 : 0;
                state->bbram[0x04] ^= 0x80;
            } else {
                state->bbram[0x04] &= ~0x3f;
            }
        } else {
            if ((state->bbram[0x04] & 0x40) == 0) {
                state->bbram[0x04] &= ~0x3f;
            }
        }
        state->bbram[0x04] |= value;
    }
    if (carry) {
        sgi_hpc3_inc_bcd(&state->bbram[0x06], 0x07, 0x01, 0x07);
        month = time_helper_from_bcd(state->bbram[0x09]);
        year = time_helper_from_bcd(state->bbram[0x0a]);
        if (month == 2 && (year % 4) == 0) {
            maxdays = 0x29;
        } else if (month >= 1 && month <= 12) {
            maxdays = daysinmonth[month - 1];
        } else {
            maxdays = 0x31;
        }
        carry = sgi_hpc3_inc_bcd(&state->bbram[0x08], 0x3f, 0x01, maxdays);
    }
    if (carry) {
        uint8_t enables = state->bbram[0x09] & 0xc0;
        sgi_hpc3_inc_bcd(&state->bbram[0x09], 0x1f, 0x01, 0x12);
        state->bbram[0x09] &= 0x3f;
        state->bbram[0x09] |= enables;
    }
    if (carry) {
        sgi_hpc3_inc_bcd(&state->bbram[0x0a], 0xff, 0x00, 0x99);
    }
}

static void sgi_hpc3_bbram_write_byte(SgiHpc3State *state, hwaddr addr,
                                      uint8_t data)
{
    if (addr >= state->bbram_size) {
        return;
    }
    if (addr >= 0x0e) {
        state->bbram[addr] = data;
        return;
    }
    /* MAME ds1386.cpp data_w: read-only-reserved bits in the clock bytes. */
    switch (addr) {
    case 0x00:                 /* hundredths */
    case 0x03:                 /* minutes alarm */
    case 0x05:                 /* hours alarm */
    case 0x0a:                 /* years */
        state->bbram[addr] = data;
        break;
    case 0x01:                 /* seconds */
    case 0x02:                 /* minutes */
    case 0x04:                 /* hours */
        state->bbram[addr] = data & 0x7f;
        break;
    case 0x06:                 /* days */
        state->bbram[addr] = data & 0x07;
        break;
    case 0x07:                 /* days alarm */
        state->bbram[addr] = data & 0x87;
        break;
    case 0x08:                 /* date */
        state->bbram[addr] = data & 0x3f;
        break;
    case 0x09:                 /* months */
        state->bbram[addr] = data & 0xdf;
        break;
    case 0x0b:                 /* command */
        state->bbram[addr] = data & 0xfc;
        break;
    default:                    /* 0x0c/0x0d watchdog, others */
        state->bbram[addr] = data;
        break;
    }
}

static void sgi_hpc3_bbram_write(void *opaque, hwaddr addr,
                                  uint64_t val, unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    unsigned reg = addr >> 2;
    uint8_t b = (uint8_t)val;

    /* Mirror of the read side: MAME's bbram_w is a 32-bit handler, so the
     * devcb_write8 -> ds1386::data_w callback sees the DWORD index and the
     * low byte of the guest's data. */
    if (sgi_bbram_trace_on() && sgi_bbram_trace_n < 4000) {
        sgi_bbram_trace_n++;
        fprintf(stderr, "BBRAM W off=0x%04x size=%u val=0x%02x\n",
                (unsigned)reg, size, (unsigned)b);
    }
    sgi_hpc3_bbram_write_byte(state, reg, b);
}

static const MemoryRegionOps sgi_hpc3_bbram_ops = {
    .read = sgi_hpc3_bbram_read,
    .write = sgi_hpc3_bbram_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/*
 * 93C56 control at HPC3+0x30008 (MAME hpc3 eeprom_r/w): bit1=CS, bit2=CLK,
 * bit3=DI on write; bit4=DO on read. Reads preserve control bits but DO.
 */
static uint64_t sgi_hpc3_eeprom_read(void *opaque, hwaddr addr, unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);

    if (!state->eeprom) {
        return 0;
    }
    return (state->eeprom_ctrl & ~0x10) |
        ((eeprom93xx_read(state->eeprom) & 1) << 4);
}

static void sgi_hpc3_eeprom_write(void *opaque, hwaddr addr,
                                   uint64_t val, unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);

    if (!state->eeprom) {
        return;
    }
    state->eeprom_ctrl = val;
    eeprom93xx_write(state->eeprom, (val >> 1) & 1,
                     (val >> 2) & 1, (val >> 3) & 1);
}

static const MemoryRegionOps sgi_hpc3_eeprom_ops = {
    .read = sgi_hpc3_eeprom_read,
    .write = sgi_hpc3_eeprom_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int sgi_hpc3_console_can_receive(void *opaque)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    unsigned rx_space = ARRAY_SIZE(state->pio8_rx_fifo) - state->pio8_rx_count;
    /*
     * One serial byte can fan out to 6 kbd scancodes; advertise the tighter
     * bound so a pasted burst never overruns the keyboard FIFO mid-character.
     */
    unsigned kbd_space = (ARRAY_SIZE(state->out_data) - state->out_count);

    if (kbd_space / 6 < rx_space) {
        rx_space = kbd_space / 6;
    }
    return (int)rx_space;
}

static void sgi_hpc3_console_receive(void *opaque, const uint8_t *buf,
                                     int size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    unsigned tail;

    while (size-- > 0) {
        uint8_t c = *buf++;

        if (state->pio8_rx_count < ARRAY_SIZE(state->pio8_rx_fifo)) {
            tail = (state->pio8_rx_head + state->pio8_rx_count) %
                   ARRAY_SIZE(state->pio8_rx_fifo);
            state->pio8_rx_fifo[tail] = c;
            state->pio8_rx_count++;
        }
        /* Also offer typed bytes as Set 2 scancodes (headless fallback). */
        sgi_hpc3_kbd_type_ascii(state, c);
    }
}

/* NVRAM seed resolution + write-back, so PROM env survives relaunch.
 * The seed dir is the MAME machine name for this board (PROM-specific
 * env): indy_4020 for the Indy/IP24 (the R4000/200MHz model this board
 * implements), indigo2_4415 for the IP22. */
static char sgi_hpc3_nvram_eeprom_path[4096];
static char sgi_hpc3_nvram_rtc_path[4096];
static char sgi_hpc3_nvram_dir[128] = "indigo2_4415";
static SgiHpc3State *sgi_hpc3_nvram_state;

static FILE *sgi_hpc3_open_nvram(const char *name, char *out, size_t outlen);
static void sgi_hpc3_note_nvram_paths(const char *eeprom, const char *rtc);
static void sgi_hpc3_note_nvram_state(SgiHpc3State *state);

static void sgi_hpc3_realize(DeviceState *device, Error **errp)
{
    SgiHpc3State *state = SGI_HPC3(device);
    char eeprom_path[4096];
    char rtc_path[4096];
    FILE *eeprom_file;
    FILE *rtc_file;

    /* MAME ds1386.cpp:70 runs advance_hundredths() at 100Hz for the life of
     * the machine, which is what keeps the BCD clock bytes in m_ram live
     * (and what the PROM's clock-running calibration loop watches). */
    state->bbram_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      sgi_hpc3_bbram_tick, state);
    /* MAME ds1386.cpp:77 runs advance_hundredths from a *repeating* 100Hz
     * timer.  Arming this once as a one-shot left the BCD clock frozen
     * after a single tick, so the PROM's wait-for-the-clock-to-change loop
     * at PROM 0xbfc31248 never completed; sgi_hpc3_bbram_tick() re-arms it.
     */
    timer_mod_ns(state->bbram_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SGI_BBRAM_TICK_NS);

    qemu_chr_fe_set_handlers(&state->console,
                             sgi_hpc3_console_can_receive,
                             sgi_hpc3_console_receive,
                             NULL, NULL, state, NULL, true);

    /*
     * No input handler is registered here on purpose.  QEMU delivers
     * each input event to the first matching handler only, so an HPC3
     * handler would shadow the real i8042 keyboard/mouse (whose PIO6
     * registers the PROM actually polls).  Host input therefore flows
     * straight to ps2kbd/ps2mouse; the ch8 8042-style pair stays as an
     * un-driven diagnostic window.
     */

    /* The seed dir is the MAME machine name set by the board (my guest
     * NVRAM path resolution uses this global). */
    if (state->nvram_dir && state->nvram_dir[0]) {
        snprintf(sgi_hpc3_nvram_dir, sizeof(sgi_hpc3_nvram_dir), "%s",
                 state->nvram_dir);
    }

    /* Seed the 93C56 EEPROM from the machine's MAME NVRAM dir. */
    eeprom_file = sgi_hpc3_open_nvram("eeprom", eeprom_path,
                                      sizeof(eeprom_path));
    if (eeprom_file) {
        uint16_t *eeprom_data = eeprom93xx_data(state->eeprom);
        uint8_t eeprom_bytes[256];
        if (fread(eeprom_bytes, sizeof(eeprom_bytes), 1, eeprom_file) == 1) {
            for (unsigned word = 0; word < 128; word++) {
                eeprom_data[word] = eeprom_bytes[word * 2] |
                                     (eeprom_bytes[word * 2 + 1] << 8);
            }
        }
        fclose(eeprom_file);
    }
    if (eeprom_path[0]) {
        sgi_hpc3_note_nvram_paths(eeprom_path, NULL);
    }

    rtc_file = sgi_hpc3_open_nvram("rtc", rtc_path, sizeof(rtc_path));
    if (rtc_file) {
        /* IP22 carries a DS1286 (64 bytes), the Indy/IP24 a DS1386_8K
         * (8KiB, MAME indy_indigo2.cpp); load the whole seed image. */
        size_t got = fread(state->bbram, 1, state->bbram_size, rtc_file);
        if (got > 0) {
            memset(state->bbram + got, 0, state->bbram_size - got);
        }
        fclose(rtc_file);
    } else {
        /* MAME ds1386.cpp nvram_default(): zeros + TE|WAM|TDM at 0x0b. */
        state->bbram[0x0b] = 0x8c;
    }
    if (rtc_path[0]) {
        sgi_hpc3_note_nvram_paths(NULL, rtc_path);
    }
    sgi_hpc3_note_nvram_state(state);
}

static const VMStateDescription vmstate_sgi_hpc3 = {
    .name = TYPE_SGI_HPC3,
    .version_id = 6,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pio0_data, SgiHpc3State, 0x100),
        VMSTATE_UINT8_ARRAY(pio1_data, SgiHpc3State, 0x100),
        VMSTATE_UINT8_ARRAY(pio8_data, SgiHpc3State, 0x100),
        VMSTATE_UINT8(pio8_latch33, SgiHpc3State),
        VMSTATE_UINT8_ARRAY(pio8_rx_fifo, SgiHpc3State, 256),
        VMSTATE_UINT32(pio8_rx_head, SgiHpc3State),
        VMSTATE_UINT32(pio8_rx_count, SgiHpc3State),
        VMSTATE_UINT8_ARRAY(out_data, SgiHpc3State, SGI_HPC3_OUT_SIZE),
        VMSTATE_UINT8_ARRAY(out_aux, SgiHpc3State, SGI_HPC3_OUT_SIZE),
        VMSTATE_UINT32(out_head, SgiHpc3State),
        VMSTATE_UINT32(out_count, SgiHpc3State),
        VMSTATE_UINT32(out_kbd_count, SgiHpc3State),
        VMSTATE_UINT32(out_aux_count, SgiHpc3State),
        VMSTATE_UINT8(cmd_byte, SgiHpc3State),
        VMSTATE_BOOL(expect_cmd_data, SgiHpc3State),
        VMSTATE_BOOL(aux_next, SgiHpc3State),
        VMSTATE_BOOL(outbuf_next_kbd, SgiHpc3State),
        VMSTATE_BOOL(outbuf_next_aux, SgiHpc3State),
        VMSTATE_UINT8(kbd_expect, SgiHpc3State),
        VMSTATE_UINT8(mouse_expect, SgiHpc3State),
        VMSTATE_BOOL(mouse_reporting, SgiHpc3State),
        VMSTATE_UINT8(mouse_resolution, SgiHpc3State),
        VMSTATE_UINT8(mouse_sample_rate, SgiHpc3State),
        VMSTATE_UINT8(last_kbd_resp, SgiHpc3State),
        VMSTATE_UINT8(last_aux_resp, SgiHpc3State),
        VMSTATE_INT32(mouse_x, SgiHpc3State),
        VMSTATE_INT32(mouse_y, SgiHpc3State),
        VMSTATE_BOOL(mouse_have_pos, SgiHpc3State),
        VMSTATE_INT32(mouse_div_rem_x, SgiHpc3State),
        VMSTATE_INT32(mouse_div_rem_y, SgiHpc3State),
        VMSTATE_UINT8(mouse_buttons, SgiHpc3State),
        VMSTATE_UINT8_ARRAY(bbram, SgiHpc3State, 0x2000),
        VMSTATE_UINT8(eeprom_ctrl, SgiHpc3State),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_hpc3_reset(DeviceState *device)
{
    SgiHpc3State *state = SGI_HPC3(device);

    memset(state->pio0_data, 0, sizeof(state->pio0_data));
    state->pio8_latch33 = 0;
    state->pio8_rx_head = 0;
    state->pio8_rx_count = 0;
    state->out_head = 0;
    state->out_count = 0;
    state->out_kbd_count = 0;
    state->out_aux_count = 0;
    state->cmd_byte = 0x65;
    state->expect_cmd_data = false;
    state->aux_next = false;
    state->outbuf_next_kbd = false;
    state->outbuf_next_aux = false;
    state->kbd_expect = 0;
    state->mouse_expect = 0;
    state->mouse_reporting = false;
    state->mouse_resolution = 2;
    state->mouse_sample_rate = 100;
    state->last_kbd_resp = 0;
    state->last_aux_resp = 0;
    state->mouse_x = 0;    state->mouse_y = 0;
    state->mouse_have_pos = false;
    state->mouse_div_rem_x = 0;
    state->mouse_div_rem_y = 0;
    state->mouse_buttons = 0;
    /*
     * NOTE: do NOT pre-queue BAT bytes here. A queued BAT keeps the
     * PIO0+0x0b mirror set, which the PROM menu-input loop waits on
     * forever (nothing consumes it headless), aborting autoboot to a
     * stuck menu. MAME's virtual keyboard BAT works there because its
     * PROM consumes it via PIO6+0x10 polling; QEMU's PROM never touches
     * PIO6, so a queued byte can only wedge the mirror. Presence will
     * have to come from the ARCS/keyboard path, not the data queue.
     */
}

/* NVRAM seed resolution + write-back, so PROM env survives relaunch. */
static FILE *sgi_hpc3_try_nvram(const char *path, char *out, size_t outlen)
{
    FILE *f = fopen(path, "rb");

    if (f && out && outlen > 0) {
        snprintf(out, outlen, "%s", path);
    }
    return f;
}

/* Locate @name inside a read-only reference NVRAM tree.  Nothing under
 * reference/mame is ever opened for writing. */
static char *sgi_hpc3_try_seed(const char *nvram_root, const char *name)
{
    char *path = g_strdup_printf("%s/%s/%s", nvram_root,
                                sgi_hpc3_nvram_dir, name);
    FILE *f = fopen(path, "rb");

    if (!f) {
        g_free(path);
        return NULL;
    }
    fclose(f);
    return path;
}

static char *sgi_hpc3_find_seed(const char *name)
{
    static const char *const roots[] = {
        "reference/mame/run/nvram",
        "../../reference/mame/run/nvram",
    };
    char exe[4096];
    char *found = NULL;
    ssize_t exe_len;
    unsigned i;

    /* 1. Relative to the QEMU executable (qemu/build -> repo root). */
    exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (exe_len > 0) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            char *root;
            *slash = '\0';
            root = g_strdup_printf("%s/../../reference/mame/run/nvram",
                                  exe);
            found = sgi_hpc3_try_seed(root, name);
            g_free(root);
        }
    }
    /* 2. Relative to cwd (repo root via run-irix.sh, or qemu/build). */
    for (i = 0; !found && i < G_N_ELEMENTS(roots); i++) {
        found = sgi_hpc3_try_seed(roots[i], name);
    }
    return found;
}

static bool sgi_hpc3_copy_seed(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out_file;
    char buf[4096];
    size_t n;
    bool ok;

    if (!in) {
        return false;
    }
    out_file = fopen(dst, "wb");
    if (!out_file) {
        fclose(in);
        return false;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out_file) != n) {
            break;
        }
    }
    ok = !ferror(in) && !ferror(out_file);
    fclose(in);
    if (fclose(out_file) != 0) {
        ok = false;
    }
    return ok;
}

static FILE *sgi_hpc3_open_nvram(const char *name, char *out, size_t outlen)
{
    char exe[4096];
    char *work_root = NULL;
    char *work_path;
    char *seed_path = NULL;
    FILE *f;
    ssize_t exe_len;

    out[0] = '\0';

    /* QEMU's own writable copy, kept beside the binary. */
    exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (exe_len > 0) {
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            work_root = g_strdup(exe);
        }
    }
    if (!work_root) {
        work_root = g_strdup("qemu/build");
    }
    work_path = g_strdup_printf("%s/nvram/%s/%s", work_root,
                                sgi_hpc3_nvram_dir, name);

    /* An existing copy wins: it holds the env the PROM already committed. */
    f = sgi_hpc3_try_nvram(work_path, out, outlen);
    if (f) {
        goto out;
    }

    /* First run: seed the copy from the read-only reference tree. */
    seed_path = sgi_hpc3_find_seed(name);
    if (seed_path) {
        char *dir = g_path_get_dirname(work_path);
        if (g_mkdir_with_parents(dir, 0755) == 0) {
            sgi_hpc3_copy_seed(seed_path, work_path);
        }
        g_free(dir);
        g_free(seed_path);
    }
    f = sgi_hpc3_try_nvram(work_path, out, outlen);
    if (!f && outlen > 0) {
        /* No seed anywhere (normal for the DS1286 RTC): still record the
         * working-copy path so BBRAM edits survive the next run. */
        snprintf(out, outlen, "%s", work_path);
    }

 out:
    g_free(work_root);
    g_free(work_path);
    return f;
}

static void sgi_hpc3_write_back_nvram(void)
{
    SgiHpc3State *state = sgi_hpc3_nvram_state;
    FILE *f;

    if (!state) {
        return;
    }
    /* These paths are QEMU's own writable copies under <exe>/nvram/, never
     * the pristine seeds in reference/mame, so persisting PROM env cannot
     * modify the reference tree. */
    if (sgi_hpc3_nvram_eeprom_path[0] && state->eeprom &&
        access(sgi_hpc3_nvram_eeprom_path, W_OK) == 0) {
        uint16_t *data = eeprom93xx_data(state->eeprom);
        uint8_t bytes[256];
        f = fopen(sgi_hpc3_nvram_eeprom_path, "wb");
        if (f) {
            /* MAME file format: native (LE) 16-bit cells. */
            for (unsigned word = 0; word < 128; word++) {
                bytes[word * 2] = data[word] & 0xff;
                bytes[word * 2 + 1] = (data[word] >> 8) & 0xff;
            }
            fwrite(bytes, 1, sizeof(bytes), f);
            fclose(f);
        }
    }
    if (sgi_hpc3_nvram_rtc_path[0] && sgi_hpc3_nvram_state &&
        access(sgi_hpc3_nvram_rtc_path, W_OK) == 0) {
        /* Persist the full DS1286/DS1386_8K image MAME uses. */
        f = fopen(sgi_hpc3_nvram_rtc_path, "wb");
        if (f) {
            fwrite(state->bbram, 1, state->bbram_size, f);
            fclose(f);
        }
    }
}

static void sgi_hpc3_note_nvram_paths(const char *eeprom, const char *rtc)
{
    static bool registered;

    if (eeprom) {
        snprintf(sgi_hpc3_nvram_eeprom_path,
                 sizeof(sgi_hpc3_nvram_eeprom_path), "%s", eeprom);
    }
    if (rtc) {
        snprintf(sgi_hpc3_nvram_rtc_path,
                 sizeof(sgi_hpc3_nvram_rtc_path), "%s", rtc);
    }
    if (!registered) {
        registered = true;
        atexit(sgi_hpc3_write_back_nvram);
    }
}

static void sgi_hpc3_note_nvram_state(SgiHpc3State *state)
{
    sgi_hpc3_nvram_state = state;
}

static void sgi_hpc3_init(Object *object)
{
    SgiHpc3State *state = SGI_HPC3(object);

    memory_region_init_io(&state->pio_iomem, object, &sgi_hpc3_pio_ops,
                          state, TYPE_SGI_HPC3 "-pio", SGI_HPC3_PIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &state->pio_iomem);
    memory_region_init_io(&state->bbram_iomem, object, &sgi_hpc3_bbram_ops,
                          state, TYPE_SGI_HPC3 "-bbram", 0x20000);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &state->bbram_iomem);
    memory_region_init_io(&state->eeprom_iomem, object, &sgi_hpc3_eeprom_ops,
                          state, TYPE_SGI_HPC3 "-eeprom", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(object), &state->eeprom_iomem);

    state->eeprom = eeprom93xx_new(DEVICE(object), 128);
}

static const Property sgi_hpc3_properties[] = {
    DEFINE_PROP_CHR("chr", SgiHpc3State, console),
    DEFINE_PROP_UINT32("bbram-size", SgiHpc3State, bbram_size, 64),
    DEFINE_PROP_STRING("nvram-dir", SgiHpc3State, nvram_dir),
};

static void sgi_hpc3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->vmsd = &vmstate_sgi_hpc3;
    device_class_set_legacy_reset(device_class, sgi_hpc3_reset);
    device_class_set_props(device_class, sgi_hpc3_properties);
    device_class->realize = sgi_hpc3_realize;
}

static const TypeInfo sgi_hpc3_type = {
    .name = TYPE_SGI_HPC3,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SgiHpc3State),
    .instance_init = sgi_hpc3_init,
    .class_init = sgi_hpc3_class_init,
};

static void sgi_hpc3_register_types(void)
{
    type_register_static(&sgi_hpc3_type);
}

type_init(sgi_hpc3_register_types)

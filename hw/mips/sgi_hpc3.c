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
#include "chardev/char-fe.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/nvram/eeprom93xx.h"
#include "hw/mips/sgi_hpc3.h"
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
    QemuInputHandlerState *kbd_hs;
    uint8_t bbram[0x2000];
    uint64_t bbram_clock_base_ns;
    time_t bbram_clock_base_time;
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

static void sgi_hpc3_mouse_move(SgiHpc3State *state, int dx, int dy)
{
    /* The PROM doubles PS/2 motion, so divide host motion by 60 with a
     * 5-point deadzone; remainders carry so slow drifts still arrive.
     * Packets split at 127 like a real mouse, only while streaming is
     * enabled (F4). The ABS/REL baseline lives with the caller so later
     * absolute reports do not jump. */
    int sum_x, sum_y;

    if (!state->mouse_reporting) {
        return;
    }
    sum_x = dx + state->mouse_div_rem_x;
    sum_y = dy + state->mouse_div_rem_y;
    state->mouse_div_rem_x = sum_x;
    state->mouse_div_rem_y = sum_y;
    if (abs(sum_x) <= 5 && abs(sum_y) <= 5) {
        return;
    }
    dx = sum_x / 60;
    dy = sum_y / 60;
    state->mouse_div_rem_x = sum_x % 60;
    state->mouse_div_rem_y = sum_y % 60;
    while (dx != 0 || dy != 0) {
        int step_x = dx < -128 ? -128 : dx > 127 ? 127 : dx;
        int step_y = dy < -128 ? -128 : dy > 127 ? 127 : dy;

        sgi_hpc3_mouse_packet(state, step_x, step_y);
        dx -= step_x;
        dy -= step_y;
    }
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

static void sgi_hpc3_kbd_event(DeviceState *dev, QemuConsole *src,
                               QemuInputEvent *evt)
{
    SgiHpc3State *state = SGI_HPC3(dev);
    uint16_t keycode = 0;

    if (evt->type == INPUT_EVENT_KIND_KEY) {
        /* PROM keymap is AT Set 2 indexed; high byte is the 0xe0 prefix. */
        if (evt->key.key < qemu_input_map_linux_to_atset2_len) {
            keycode = qemu_input_map_linux_to_atset2[evt->key.key];
        }
        if (!keycode) {
            return;
        }
        if (keycode & 0xff00) {
            sgi_hpc3_kbd_enqueue(state, keycode >> 8);
        }
        if (!evt->key.down) {
            sgi_hpc3_kbd_enqueue(state, 0xf0);
        }
        sgi_hpc3_kbd_enqueue(state, keycode & 0xff);
        return;
    }
    if (evt->type == INPUT_EVENT_KIND_ABS) {
        /* Absolute position in screen pixels; negate Y (PS/2 is up-positive). */
        if (evt->abs.axis == INPUT_AXIS_X) {
            if (state->mouse_have_pos) {
                sgi_hpc3_mouse_move(state,
                                    evt->abs.value - state->mouse_x, 0);
            }
            state->mouse_x = evt->abs.value;
            state->mouse_have_pos = true;
        } else if (evt->abs.axis == INPUT_AXIS_Y) {
            if (state->mouse_have_pos) {
                sgi_hpc3_mouse_move(state, 0,
                                    state->mouse_y - evt->abs.value);
            }
            state->mouse_y = evt->abs.value;
            state->mouse_have_pos = true;
        }
        return;
    }
    if (evt->type == INPUT_EVENT_KIND_REL) {
        /* Track REL motion in the ABS baseline so mixed sequences do not jump. */
        if (evt->rel.axis == INPUT_AXIS_X) {
            sgi_hpc3_mouse_move(state, evt->rel.value, 0);
            state->mouse_x += evt->rel.value;
            state->mouse_have_pos = true;
        } else if (evt->rel.axis == INPUT_AXIS_Y) {
            sgi_hpc3_mouse_move(state, 0, -evt->rel.value);
            state->mouse_y += evt->rel.value;
            state->mouse_have_pos = true;
        }
        return;
    }
    if (evt->type == INPUT_EVENT_KIND_BTN) {
        unsigned bit = 0;

        switch (evt->btn.button) {
        case INPUT_BUTTON_LEFT:
            bit = 0x01;
            break;
        case INPUT_BUTTON_MIDDLE:
            bit = 0x04;
            break;
        case INPUT_BUTTON_RIGHT:
            bit = 0x02;
            break;
        default:
            return;
        }
        if (evt->btn.down) {
            state->mouse_buttons |= bit;
        } else {
            state->mouse_buttons &= ~bit;
        }
        /* Latch buttons always; the packet goes out only while reporting. */
        if (state->mouse_reporting) {
            sgi_hpc3_mouse_packet(state, 0, 0);
        }
    }
}

static const QemuInputHandler sgi_hpc3_kbd_handler = {
    .name = "SGI HPC3 Keyboard",
    .mask = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN |
            INPUT_EVENT_MASK_REL | INPUT_EVENT_MASK_ABS,
    .event = sgi_hpc3_kbd_event,
};

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
        /* Kbd/mouse data: queued bytes first, then serial RX, else 0xaa
         * (idle, never a key). Dequeue order keeps the AUX bit accurate. */
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
        return 0xaa;
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
        /* Early-boot peek at the head byte without dequeuing; 0 when idle. */
        if (state->out_count > 0) {
            return (uint64_t)(state->out_data[state->out_head] & 0xff);
        }
        return 0;
    }
    return 0;
}

static uint64_t sgi_hpc3_pio_read(void *opaque, hwaddr addr, unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    unsigned channel = (addr >> 8) & 15;
    unsigned offset = addr & 0xff;
    uint64_t rv;

    if (channel >= 10) {
        channel = (channel & 1) ? 9 : 8;
    }

    /* Channel 6 (IOC2) is owned by the sgi-ioc2 device and the ESCC. */
    if (channel == 6) {
        return 0;
    }
    if (channel == 8) {
        rv = sgi_hpc3_pio8_read(state, offset);
        return rv;
    }
    if (channel == 0) {
        if (offset == 0x3b) {
            return 0;
        }
        if (offset == 0x0b) {
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
    return 0;
}

static void sgi_hpc3_pio_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    unsigned channel = (addr >> 8) & 15;
    unsigned offset = addr & 0xff;

    if (channel >= 10) {
        channel = (channel & 1) ? 9 : 8;
    }

    if (channel == 6) {
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
        }
        return;
    }
    if (channel == 0) {
        state->pio0_data[offset] = val;
        return;
    }
}

static const MemoryRegionOps sgi_hpc3_pio_ops = {
    .read = sgi_hpc3_pio_read,
    .write = sgi_hpc3_pio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static uint8_t sgi_hpc3_bcd(unsigned value)
{
    return (value / 10) * 16 + value % 10;
}

static uint64_t sgi_hpc3_bbram_read(void *opaque, hwaddr addr, unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    uint64_t clock_ns = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    time_t now;
    struct tm current;
    hwaddr reg = size == 4 ? addr >> 2 : addr;

    if (!state->bbram_clock_base_ns) {
        state->bbram_clock_base_ns = clock_ns;
        state->bbram_clock_base_time = g_get_real_time() / G_USEC_PER_SEC;
    }
    now = state->bbram_clock_base_time +
            (clock_ns - state->bbram_clock_base_ns) / 1000000000ULL;
    localtime_r(&now, &current);
    /* MAME ds1386.h REGISTER_*: 0-0x0d clock/alarms/command/watchdog,
     * 0x0e+ user NVRAM; DS1286 is 64 bytes (MAME ds1386.cpp data_r). */
    if (reg >= 64) {
        return 0;
    }
    switch (reg) {
    case 0x00:
        return sgi_hpc3_bcd((g_get_real_time() / 10000) % 100);    case 0x01:
        return sgi_hpc3_bcd(current.tm_sec);
    case 0x02:
        return sgi_hpc3_bcd(current.tm_min);
    case 0x04:
        return sgi_hpc3_bcd(current.tm_hour);
    case 0x06:
        return sgi_hpc3_bcd(current.tm_wday + 1);
    case 0x08:
        return sgi_hpc3_bcd(current.tm_mday);
    case 0x09:
        return sgi_hpc3_bcd(current.tm_mon + 1);
    case 0x0a:
        return sgi_hpc3_bcd(current.tm_year % 100);
    default:
        /* Alarms/command/watchdog/user: stored RAM, not live time. */
        return state->bbram[reg];
    }
}

static void sgi_hpc3_bbram_write(void *opaque, hwaddr addr,
                                  uint64_t val, unsigned size)
{
    SgiHpc3State *state = SGI_HPC3(opaque);
    hwaddr reg = size == 4 ? addr >> 2 : addr;

    /* MAME ds1386.cpp data_w: ignore accesses beyond the 64-byte DS1286. */
    if (reg >= 64) {
        return;
    }
    state->bbram[reg] = val;
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

static void sgi_hpc3_realize(DeviceState *device, Error **errp)
{
    SgiHpc3State *state = SGI_HPC3(device);

    qemu_chr_fe_set_handlers(&state->console,
                             sgi_hpc3_console_can_receive,
                             sgi_hpc3_console_receive,
                             NULL, NULL, state, NULL, true);
    state->kbd_hs = qemu_input_handler_register(device,
                                                &sgi_hpc3_kbd_handler);
}

static void sgi_hpc3_unrealize(DeviceState *device)
{
    SgiHpc3State *state = SGI_HPC3(device);

    g_clear_pointer(&state->kbd_hs, qemu_input_handler_unregister);
}

static const VMStateDescription vmstate_sgi_hpc3 = {
    .name = TYPE_SGI_HPC3,
    .version_id = 6,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pio0_data, SgiHpc3State, 0x100),
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
    state->mouse_x = 0;
    state->mouse_y = 0;
    state->mouse_have_pos = false;
    state->mouse_div_rem_x = 0;
    state->mouse_div_rem_y = 0;
    state->mouse_buttons = 0;
}

/* NVRAM seed resolution + write-back, so PROM env survives relaunch. */
static char sgi_hpc3_nvram_eeprom_path[4096];
static char sgi_hpc3_nvram_rtc_path[4096];
static SgiHpc3State *sgi_hpc3_nvram_state;

static FILE *sgi_hpc3_try_nvram(const char *path, char *out, size_t outlen)
{
    FILE *f = fopen(path, "rb");

    if (f && out && outlen > 0) {
        snprintf(out, outlen, "%s", path);
    }
    return f;
}

static FILE *sgi_hpc3_open_nvram(const char *name, char *out, size_t outlen)
{
    FILE *f;
    ssize_t exe_len;
    char exe[4096];
    char first_candidate[4096];

    first_candidate[0] = '\0';
    out[0] = '\0';
    /* 1. Relative to the QEMU executable (qemu/build -> repo root). */
    exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (exe_len > 0) {
        char *slash;
        exe[exe_len] = '\0';
        slash = strrchr(exe, '/');
        if (slash) {
            char *path;
            *slash = '\0';
            path = g_strdup_printf("%s/../../reference/mame/run/"
                                   "nvram/indigo2_4415/%s", exe, name);
            if (!first_candidate[0]) {
                snprintf(first_candidate, sizeof(first_candidate), "%s",
                         path);
            }
            f = sgi_hpc3_try_nvram(path, out, outlen);
            g_free(path);
            if (f) {
                return f;
            }
        }
    }
    /* 2. Relative to cwd (repo root via run-irix.sh, or qemu/build). */
    {
        static const char *const rels[] = {
            "reference/mame/run/nvram/indigo2_4415/",
            "../../reference/mame/run/nvram/indigo2_4415/",
            NULL,
        };
        for (unsigned i = 0; rels[i]; i++) {
            char *path = g_strdup_printf("%s%s", rels[i], name);
            if (!first_candidate[0]) {
                snprintf(first_candidate, sizeof(first_candidate), "%s",
                         path);
            }
            f = sgi_hpc3_try_nvram(path, out, outlen);
            g_free(path);
            if (f) {
                return f;
            }
        }
    }
    /* No seed found (normal for the DS1286 RTC): still record the path
     * for write-back, or BBRAM edits are lost every run. */
    if (outlen > 0 && first_candidate[0]) {
        snprintf(out, outlen, "%s", first_candidate);
    }
    return NULL;
}

static void sgi_hpc3_write_back_nvram(void)
{
    SgiHpc3State *state = sgi_hpc3_nvram_state;
    FILE *f;

    if (!state) {
        return;
    }
    /* Never overwrite the frozen 0444 reference seeds; they stay
     * read-only and QEMU simply skips write-back when unwritable. */
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
    if (sgi_hpc3_nvram_rtc_path[0] &&
        access(sgi_hpc3_nvram_rtc_path, W_OK) == 0) {
        /* DS1286: persist the 64-byte image MAME uses. */
        f = fopen(sgi_hpc3_nvram_rtc_path, "wb");
        if (f) {
            fwrite(state->bbram, 1, 64, f);
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
    FILE *rtc_file;

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
    {
        /*
         * Seed from the MAME indigo2_4415 NVRAM dir (exe- then cwd-relative);
         * MAME cells are native-endian, so load little-endian to match.
         */
        char eeprom_path[4096];
        FILE *eeprom_file = sgi_hpc3_open_nvram("eeprom", eeprom_path,
                                                sizeof(eeprom_path));
        uint16_t *eeprom_data = eeprom93xx_data(state->eeprom);
        uint8_t eeprom_bytes[256];
        if (eeprom_file && fread(eeprom_bytes, sizeof(eeprom_bytes), 1,
                                 eeprom_file) == 1) {
            for (unsigned word = 0; word < 128; word++) {
                eeprom_data[word] = eeprom_bytes[word * 2] |
                                     (eeprom_bytes[word * 2 + 1] << 8);
            }
        }
        if (eeprom_file) {
            fclose(eeprom_file);
        }
        if (eeprom_path[0]) {
            sgi_hpc3_note_nvram_paths(eeprom_path, NULL);
        }
    }

    {
        char rtc_path[4096];
        rtc_file = sgi_hpc3_open_nvram("rtc", rtc_path, sizeof(rtc_path));
        if (rtc_file) {
            /* IP22 carries a DS1286 (64 bytes, MAME indy_indigo2.cpp), not
             * the Indy/IP24 DS1386_8K; load the seed as-is. */
            uint8_t nvram[64];
            size_t got = fread(nvram, 1, sizeof(nvram), rtc_file);
            if (got > 0) {
                memcpy(state->bbram, nvram, got);
            }
            fclose(rtc_file);
        } else {
            /* MAME ds1386.cpp nvram_default(): zeros + TE|WAM|TDM at 0x0b. */
            state->bbram[0x0b] = 0x8c;
        }
        if (rtc_path[0]) {
            sgi_hpc3_note_nvram_paths(NULL, rtc_path);
        }
    }
    sgi_hpc3_note_nvram_state(state);
}

static const Property sgi_hpc3_properties[] = {
    DEFINE_PROP_CHR("chr", SgiHpc3State, console),
};

static void sgi_hpc3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device_class = DEVICE_CLASS(klass);

    device_class->vmsd = &vmstate_sgi_hpc3;
    device_class_set_legacy_reset(device_class, sgi_hpc3_reset);
    device_class_set_props(device_class, sgi_hpc3_properties);
    device_class->realize = sgi_hpc3_realize;
    device_class->unrealize = sgi_hpc3_unrealize;
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

/*
 * The keyboard, the mouse (port 1) and the joystick (port 2).
 *
 * The keyboard sends each key code through CIA-A's serial port: 7 bits of
 * the key and the up flag, rotated so the flag comes last, each bit
 * inverted (the OS: NOT, then ROR #1). It sends the next only after the
 * computer's handshake (CIA-A's serial port switched to output and back,
 * pulling KDAT low), or after 143 ms without one. At power-on: $FD, $FE
 * (the start and end of the keys held). Ctrl and both Amiga keys reset
 * the machine (warning the OS first: $78).
 *
 * The mouse: JOY0DAT's two 8-bit counters (vertical high), the left button
 * on CIA-A PRA bit 6, the right and middle ones on POTGO's port 1 pins. The
 * joystick: JOY1DAT's direction bits, fire on CIA-A PRA bit 7, the second
 * button on POTGO.
 */
#include <string.h>
#include "amiga_internal.h"

#define QUEUE           32
#define HANDSHAKE_LINES (313 * 143 / 20)    /* 143 ms */
#define KEY_SHIFT       0x60
#define KEY_CTRL        0x63
#define KEY_LAMIGA      0x66
#define KEY_RAMIGA      0x67
#define TYPE_FRAMES     2                   /* a typed key down, then up */

static struct {
    /* the keyboard */
    uint8_t queue[QUEUE];
    volatile uint32_t head, tail;   /* (other tasks put, the emulation takes) */
    bool waiting;                   /* for the handshake */
    int wait_lines;
    uint8_t held[16];               /* keys down (bit per code) */
    volatile bool reset_wanted;
    /* the mouse and the joystick (other tasks write, the machine reads) */
    volatile uint8_t mx, my;
    volatile uint8_t buttons;       /* bit 0 left, 1 right, 2 middle */
    volatile uint8_t joy;
} s_in;

/* Text being typed (amiga_type(), stepped by input_frame()) */
static struct {
    char text[256];
    volatile int len;
    int pos, phase, key;            /* key: the one down (-1: none) */
    bool shift;
} s_type = { .key = -1 };

static void put(uint8_t code)
{
    uint32_t h = s_in.head;
    if (h - s_in.tail < QUEUE) {
        s_in.queue[h % QUEUE] = code;
        __atomic_store_n(&s_in.head, h + 1, __ATOMIC_RELEASE);
    }
}

void input_reset(void)
{
    s_in.tail = s_in.head;
    s_in.waiting = false;
    s_in.wait_lines = 0;
    memset(s_in.held, 0, sizeof(s_in.held));
    s_in.reset_wanted = false;
    put(0xFD);                      /* power-up key stream */
    put(0xFE);                      /* (none held) */
}

void amiga_key(uint8_t code, bool pressed)
{
    code &= 0x7F;
    bool was = (s_in.held[code >> 3] >> (code & 7)) & 1;
    if (was == pressed) {
        return;
    }
    if (pressed) {
        s_in.held[code >> 3] |= (uint8_t)(1u << (code & 7));
    } else {
        s_in.held[code >> 3] &= (uint8_t)~(1u << (code & 7));
    }
    put(code | (pressed ? 0 : 0x80));
    bool ctrl = (s_in.held[KEY_CTRL >> 3] >> (KEY_CTRL & 7)) & 1;
    bool la = (s_in.held[KEY_LAMIGA >> 3] >> (KEY_LAMIGA & 7)) & 1;
    bool ra = (s_in.held[KEY_RAMIGA >> 3] >> (KEY_RAMIGA & 7)) & 1;
    if (pressed && ctrl && la && ra) {
        put(0x78);                  /* reset warning */
        s_in.reset_wanted = true;
    }
}

/* The US keymap: a character's raw code (and Shift), -1 if it has none */
static int ascii_key(char c, bool *shift)
{
    /* (by raw code from $00: the rows $00, $10, $20 and $31 on) */
    static const char plain[] = "`1234567890-=\\\0\0qwertyuiop[]\0\0\0\0asdfghjkl;'\0\0\0\0\0\0zxcvbnm,./";
    static const char shifted[] = "~!@#$%^&*()_+|\0\0QWERTYUIOP{}\0\0\0\0ASDFGHJKL:\"\0\0\0\0\0\0ZXCVBNM<>?";
    *shift = false;
    switch (c) {
    case ' ': return 0x40;
    case '\b': return 0x41;
    case '\t': return 0x42;
    case '\n': return 0x44;
    case '\0': return -1;
    default: break;
    }
    for (int i = 0; i < (int)sizeof(plain) - 1; i++) {
        if (plain[i] == c) {
            return i;
        }
        if (shifted[i] == c) {
            *shift = true;
            return i;
        }
    }
    return -1;
}

void amiga_type(const char *text)
{
    s_type.len = 0;
    size_t n = strlen(text);
    n = n < sizeof(s_type.text) - 1 ? n : sizeof(s_type.text) - 1;
    memcpy(s_type.text, text, n);
    s_type.pos = 0;
    s_type.len = (int)n;
}

bool amiga_typing(void)
{
    return s_type.pos < s_type.len || s_type.key >= 0;
}

/* Once a frame: the typed text's next key down, or the last one up */
void input_frame(void)
{
    if (s_type.phase > 0 && --s_type.phase > 0) {
        return;
    }
    if (s_type.key >= 0) {
        amiga_key((uint8_t)s_type.key, false);
        if (s_type.shift) {
            amiga_key(KEY_SHIFT, false);
        }
        s_type.key = -1;
        s_type.phase = TYPE_FRAMES;
        return;
    }
    while (s_type.pos < s_type.len) {
        bool shift;
        int k = ascii_key(s_type.text[s_type.pos++], &shift);
        if (k < 0) {
            continue;
        }
        if (shift) {
            amiga_key(KEY_SHIFT, true);
        }
        amiga_key((uint8_t)k, true);
        s_type.key = k;
        s_type.shift = shift;
        s_type.phase = TYPE_FRAMES;
        return;
    }
}

bool input_reset_wanted(void)
{
    bool r = s_in.reset_wanted;
    s_in.reset_wanted = false;
    return r;
}

/* CIA-A's serial port went from output back to input: the handshake */
void input_handshake(void)
{
    s_in.waiting = false;
}

/* Once a line: the next key code into CIA-A's serial port */
void input_line(void)
{
    if (s_in.waiting) {
        if (--s_in.wait_lines > 0) {
            return;
        }
        s_in.waiting = false;       /* no handshake: send on */
    }
    if (cia[0].cra & 0x40) {
        return;                     /* (its serial port is an output) */
    }
    uint32_t t = s_in.tail;
    if (t == __atomic_load_n(&s_in.head, __ATOMIC_ACQUIRE)) {
        return;
    }
    uint8_t code = s_in.queue[t % QUEUE];
    s_in.tail = t + 1;
    cia_serial_in(&cia[0], (uint8_t)~((code << 1) | (code >> 7)));
    s_in.waiting = true;
    s_in.wait_lines = HANDSHAKE_LINES;
}

void amiga_mouse(int dx, int dy, uint8_t buttons)
{
    dx = dx < -127 ? -127 : dx > 127 ? 127 : dx;
    dy = dy < -127 ? -127 : dy > 127 ? 127 : dy;
    s_in.mx = (uint8_t)(s_in.mx + dx);
    s_in.my = (uint8_t)(s_in.my + dy);
    s_in.buttons = buttons;
}

/* JOYTEST: the counters' top six bits */
void input_joytest(uint16_t v)
{
    s_in.mx = (uint8_t)((s_in.mx & 3) | (v & 0xFC));
    s_in.my = (uint8_t)((s_in.my & 3) | ((v >> 8) & 0xFC));
}

void amiga_joystick(uint8_t bits)
{
    s_in.joy = bits;
}

uint16_t input_joy0dat(void)
{
    return (uint16_t)(s_in.my << 8 | s_in.mx);
}

/* JOY1DAT: right bit 1, left bit 9, down bit 0 ^ bit 1, up bit 8 ^ bit 9 */
uint16_t input_joy1dat(void)
{
    uint8_t j = s_in.joy;
    uint16_t v = 0;
    if (j & AMIGA_JOY_RIGHT) {
        v |= 0x0002;
    }
    if (j & AMIGA_JOY_LEFT) {
        v |= 0x0200;
    }
    if (((j & AMIGA_JOY_DOWN) != 0) != ((v & 0x0002) != 0)) {
        v |= 0x0001;
    }
    if (((j & AMIGA_JOY_UP) != 0) != ((v & 0x0200) != 0)) {
        v |= 0x0100;
    }
    return v;
}

/* CIA-A PRA bits 7 (port 2 fire) and 6 (port 1: the left button), active low */
uint8_t input_fire(void)
{
    uint8_t v = 0xC0;
    if (s_in.buttons & 1) {
        v &= ~0x40;
    }
    if (s_in.joy & AMIGA_JOY_FIRE) {
        v &= ~0x80;
    }
    return v;
}

/* POTGO's input pins pulled low: port 1's right (DATLY, bit 10) and middle
 * (DATLX, 8) buttons, port 2's second button (DATRY, 14)
 */
uint16_t input_pot_low(void)
{
    uint16_t v = 0;
    if (s_in.buttons & 2) {
        v |= 0x0400;
    }
    if (s_in.buttons & 4) {
        v |= 0x0100;
    }
    if (s_in.joy & AMIGA_JOY_FIRE2) {
        v |= 0x4000;
    }
    return v;
}

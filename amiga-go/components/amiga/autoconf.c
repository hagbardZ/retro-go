/*
 * Autoconfig (Zorro II): the boards in the expansion slots, one after the
 * other at $E80000 until each is given an address or told to shut up.
 *
 * A board's configuration is 16 bytes read as nibbles: logical byte r's
 * high nibble at $E80000 + 4r, its low one at + 2, in bits 7-4 of the byte
 * (D15-D12). All but byte 0 (er_Type) read inverted. The OS writes the
 * board's address (A23-A16) as two nibbles, the low one first ($E8004A),
 * then the high one ($E80048), which configures it; $E8004C shuts it up.
 *
 * The boards: fast RAM (1-8 MB, Kickstart adds it to the free memory),
 * our hard disk board (256 KB with a boot ROM, hdboard.c: there only while
 * a hard disk is in and the Kickstart has no IDE driver, decided as the
 * chain gets to it). Commodore's ID for prototypes ($07DB) as the
 * manufacturer.
 */
#include <string.h>
#include "amiga_internal.h"
#include "hdrom.h"

#define MANUFACTURER    0x07DB
#define PRODUCT_FAST    1
#define PRODUCT_HD      2

typedef struct {
    uint8_t rom[16];                /* the configuration, as the OS reads it (before inverting) */
    uint8_t base_lo;                /* the address's low nibble, written first */
} board_t;

static board_t s_boards[3];
static int s_count, s_current;

/* er_Type's size bits for a power of two from 64 KB to 8 MB */
static uint8_t size_code(uint32_t size)
{
    switch (size) {
    case 0x10000: return 1;
    case 0x20000: return 2;
    case 0x40000: return 3;
    case 0x80000: return 4;
    case 0x100000: return 5;
    case 0x200000: return 6;
    case 0x400000: return 7;
    default: return 0;              /* 8 MB */
    }
}

void autoconf_reset(void)
{
    memset(s_boards, 0, sizeof(s_boards));
    s_count = 0;
    s_current = 0;
    ami.fast_base = 0;
    if (ami.fast_size) {
        board_t *b = &s_boards[s_count++];
        b->rom[0] = 0xC0 | 0x20 | size_code(ami.fast_size);    /* Zorro II, into the memory list */
        b->rom[1] = PRODUCT_FAST;
        b->rom[2] = 0x80;                                       /* the 8 MB space, please */
        b->rom[4] = MANUFACTURER >> 8;
        b->rom[5] = MANUFACTURER & 0xFF;
    }
    board_t *b = &s_boards[s_count++];
    b->rom[0] = 0xC0 | 0x10 | size_code(0x40000);              /* Zorro II, a boot ROM */
    b->rom[1] = PRODUCT_HD;
    b->rom[4] = MANUFACTURER >> 8;
    b->rom[5] = MANUFACTURER & 0xFF;
    b->rom[10] = HDROM_DIAG >> 8;                               /* er_InitDiagVec */
    b->rom[11] = HDROM_DIAG & 0xFF;
}

/* The next board there is (the hard disk board only when wanted) */
static void skip_absent(void)
{
    while (s_current < s_count && s_boards[s_current].rom[1] == PRODUCT_HD && !hdboard_wanted()) {
        s_current++;
    }
}

/* The byte at a config space address (even: its nibble in bits 7-4) */
uint8_t autoconf_read8(uint32_t a)
{
    skip_absent();
    if (s_current >= s_count || (a & 1) || (a & 0xFFFF) >= 0x80) {
        return s_current >= s_count ? 0x00 : 0xFF;
    }
    int r = (a & 0x7F) >> 2;
    uint8_t v = r < 16 ? s_boards[s_current].rom[r] : 0;
    if (r != 0) {
        v = (uint8_t)~v;
    }
    return (uint8_t)(((a & 2) ? v << 4 : v) | 0x0F);
}

uint16_t autoconf_read16(uint32_t a)
{
    return (uint16_t)(autoconf_read8(a & ~1u) << 8 | 0xFF);
}

static void configured(const board_t *b, uint32_t base)
{
    if (b->rom[1] == PRODUCT_FAST) {
        ami.fast_base = base;
        mem_update_map();
    } else if (b->rom[1] == PRODUCT_HD) {
        hdboard_configured(base);
    }
}

void autoconf_write8(uint32_t a, uint8_t v)
{
    skip_absent();
    if (s_current >= s_count || (a & 1)) {
        return;
    }
    board_t *b = &s_boards[s_current];
    switch (a & 0xFF) {
    case 0x4A:
        b->base_lo = v >> 4;
        break;
    case 0x48:
        configured(b, ((uint32_t)(v & 0xF0) | b->base_lo) << 16);
        s_current++;
        break;
    case 0x4C:
        s_current++;                /* shut up */
        break;
    default:
        break;
    }
}

void autoconf_write16(uint32_t a, uint16_t v)
{
    autoconf_write8(a & ~1u, v >> 8);
}

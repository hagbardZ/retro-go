/*
 * Agnus's bitplane DMA and Denise: the picture.
 *
 * The line's bitplanes are fetched at DDFSTRT's position (an event of the
 * frame loop) with the registers as they are then. At the line's end what
 * Denise needs to draw it goes into the line's record: the fetched words,
 * BPLCON0-4, the window, the colours as the line began and each colour
 * write with the beam's position. Three frames of records (the
 * presenter's, the last one finished, the next); a record that would be
 * the same as the one already there isn't written again.
 *
 * The presenter (core 0) draws the records: the planes turned into pixel
 * indices through a table (odd and even planes apart: their scroll delays
 * differ), the colours changed at the pixels the beam was at, the sprites
 * over them as BPLCON2's priorities say.
 *
 * Sprites: their DMA fetches the control words (SPRxPOS, SPRxCTL) on the
 * first line after vertical blank and at each sprite's VSTOP, the data
 * words on the lines from VSTART (also when VSTART was written instead,
 * the DMA off at the first line); written directly, writing DATA arms a
 * sprite and CTL disarms it. Lores pixels, the display window's
 * horizontal coordinates; an odd sprite with ATT set adds its two bits to
 * its even partner's (15 colours). Shown inside the display window only,
 * not above or below it either (BPLCON3's BRDSPRT: in all the border).
 *
 * The picture: lores pixels 108-467 (hires 216-935) of lines 29-312, in
 * hires pixels (lores ones doubled), its lines doubled. Interlaced (LACE
 * in BPLCON0 during the frame, LOF then toggles): each field has its own
 * frames, and the presenter weaves them, the long field's lines even, the
 * short one's odd, once both have been drawn.
 *
 * ECS super-hires (SHRES in BPLCON0, up to 2 planes, 4 pixels a lores
 * one): a frame with such lines is drawn in super-hires pixels (1440
 * wide), the other lines' pixels doubled. Denise looks up a colour a
 * hires pixel: two super-hires pixels a, b take register 4b + a, a its
 * guns' upper 2 bits, b their lower 2 (graphics.library fills the
 * registers so that each pixel gets its colour). The sprites the same
 * from registers 16-31, the two halves of a lores pixel paired, their
 * colour's lower 2 bits; SPRxCTL bit 4 puts them on hires pixels, and
 * sprites 4-7 don't show (as WinUAE has it). Dual playfield is drawn as
 * one playfield there, also as WinUAE does.
 *
 * AGA (the A1200's Alice and Lisa): 256 colours of 24 bits (BPLCON3's
 * bank picks the 32 a register write reaches, LOCT the low nibbles: a
 * write without it sets both), up to 8 planes in any resolution (BPU3),
 * fetched 16, 32 or 64 bits at a time (FMODE), HAM8, BPLCON4's XOR of the
 * pixels and the sprites' colour banks, dual playfield's second colours
 * from BPLCON3's offset, EHB unless BPLCON2 kills it, scroll delays in
 * super-hires pixels, sprites 16, 32 or 64 pixels wide in lores, hires or
 * super-hires pixels and placed to the super-hires pixel (SPRxCTL bits 4
 * and 3), the border blanked. Super-hires pixels have their
 * own colours there. Colours are kept as 24 bits for every chipset; OCS
 * and ECS ones are their 12 bits with each nibble doubled (the same
 * RGB565 as before).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_internal.h"

#define FIRST_LORES     108
#define PIC_W           AMIGA_SCREEN_WIDTH
#define PIC_LINES       AMIGA_SCREEN_LINES
#define FIRST_LINE_PAL  29
#define FIRST_LINE_NTSC 21
#define MAX_CHANGES     160
#define MAX_PLANES      8
/* a plane's words: up to 112 (super-hires fetched 64 bits at a time from
 * DDFSTRT $18 to DDFSTOP $D8)
 */
#define MAX_PLANE_WORDS 112
#define MAX_WORDS       (MAX_PLANES * MAX_PLANE_WORDS)
#define NCOLORS         256             /* AGA's (OCS and ECS use 32) */
#define SPR_WORDS       4               /* a sprite's words a line: 64 pixels (AGA) */
#define WIDE_W          (2 * PIC_W)     /* a frame with super-hires lines */

#define BPLCON0_HIRES   0x8000
#define BPLCON0_SHRES   0x0040          /* ECS (masked off with OCS) */
#define BPLCON0_BPU3    0x0010          /* AGA: 8 planes */
#define BPLCON0_HAM     0x0800
#define BPLCON0_DPF     0x0400
#define BPLCON2_PF2PRI  0x0040
#define BPLCON2_KILLEHB 0x0200          /* AGA: 6 planes are 64 colours */
#define BPLCON3_LOCT    0x0200
#define BPLCON3_BRDRBLNK 0x0020
#define BPLCON3_BRDSPRT 0x0002
#define BPLCON0_ECSENA  0x0001

/* A colour write: from this pixel of the picture on, colour reg (AGA: of
 * the 256) is rgb
 */
typedef struct {
    uint16_t x;
    uint8_t reg;
    uint32_t rgb;
} change_t;

/* A line's record: what Denise draws it from */
typedef struct {
    uint32_t key;                   /* a hash of the rest (0: nothing yet) */
    uint16_t con0, con1, con2, con3, con4, fmode;
    int16_t first;                  /* the first fetched pixel, lores */
    int16_t x0, x1;                 /* the window, in the picture's pixels */
    uint8_t planes, ncolors;        /* planes 0: no bitplanes on this line */
    uint16_t words;
    uint16_t changes;
    uint8_t sprites;                /* shown on this line */
    bool brdsprt;                   /* BPLCON3's BRDSPRT: they show in the border too */
    uint8_t spr_words;              /* each one's words (16 pixels each) */
    uint8_t spr_num[8];             /* the sprite (bit 7: attached to the one before) */
    int16_t spr_x[8];               /* super-hires pixels from the picture's left edge */
    uint16_t spr_a[8][SPR_WORDS], spr_b[8][SPR_WORDS];      /* DATA, DATB */
    uint32_t color[NCOLORS];        /* as the line began (ncolors of them) */
    change_t change[MAX_CHANGES];
    uint16_t data[MAX_WORDS];       /* plane after plane */
} row_t;

typedef struct {
    uint32_t pt;
    uint16_t pos, ctl;
    uint16_t data[SPR_WORDS], datb[SPR_WORDS];
    bool armed;                     /* its data words show on this line */
    uint8_t dma;                    /* 0 idle, 1 waiting for VSTART, 2 showing */
} sprite_t;

static struct {
    /* registers */
    uint16_t bplcon0, bplcon1, bplcon2, bplcon3, bplcon4, fmode;
    sprite_t spr[8];
    uint16_t diwstrt, diwstop, ddfstrt, ddfstop;
    uint16_t diwhigh;               /* ECS: the window's high bits, if written since DIWSTRT/DIWSTOP */
    bool diwhigh_set;
    int16_t mod[2];
    uint32_t pt[MAX_PLANES];
    bool vwin;

    /* this line */
    int fetch_at;
    bool fetched;
    uint16_t con0, con1, con2, con3, con4, fmode_line;
    int first, planes, words;
    uint32_t data_key;
    uint16_t data[MAX_WORDS];
    int changes;
    change_t change[MAX_CHANGES];
    uint32_t late[NCOLORS / 32];    /* colours changed after the log filled */
    uint32_t late_value[NCOLORS];
    int ncolors;                    /* 32, AGA 256 */
    uint32_t color[NCOLORS];        /* 24-bit, as the line began */
    uint32_t pal_hash;              /* their hash (each colour's, XORed) */

    /* collisions */
    uint16_t clxcon, clxcon2, clxdat;
    int clx_frames;                 /* worked out while > 0 (a CLXDAT read sets it) */

    /* the frames: each field's three (an interlaced picture's short
     * frames go to field 1, everything else to field 0)
     */
    row_t *rows;
    uint32_t signature[2][3];
    bool lace[2][3];                /* drawn with BPLCON0's LACE on */
    bool shres[2][3];               /* with super-hires lines */
    uint32_t hash;
    uint32_t state[2];              /* latest | pinned << 2 */
    int writing[2];
    int field;                      /* the one this frame draws */
    bool lace_seen;                 /* LACE on at some point in this frame */
    bool shres_seen;                /* a super-hires line in this frame */
    bool field1;                    /* field 1 drawn since interlace began */
    int first_line;
} s;

static uint8_t s_r5[256], s_g6[256];    /* 8-bit guns to RGB565's 5 and 6 bits */
static uint64_t s_p2c[256];
static uint8_t s_pf1[256], s_pf2[256];

#define LATEST(st)      ((st) & 3u)
#define PINNED(st)      (((st) >> 2) & 3u)

size_t amiga_picture_size(void)
{
    return 6 * PIC_LINES * sizeof(row_t);
}

#define BPLCON0_LACE    0x0004

static row_t *buffer(int field, int b)
{
    return s.rows + (field * 3 + b) * PIC_LINES;
}

/* 12-bit colours as 24 bits, each nibble twice */
static inline uint32_t expand12(uint16_t v)
{
    return ((uint32_t)(v & 0xF00) << 12 | (uint32_t)(v & 0xF00) << 8) | ((v & 0xF0) << 8 | (v & 0xF0) << 4) |
           ((v & 0xF) << 4 | (v & 0xF));
}

static inline uint16_t to565(uint32_t c)
{
    return (uint16_t)(s_r5[(c >> 16) & 0xFF] << 11 | s_g6[(c >> 8) & 0xFF] << 5 | s_r5[c & 0xFF]);
}

/* A colour's part of the palette's hash */
static inline uint32_t color_hash(int i, uint32_t c)
{
    uint32_t h = (c ^ ((uint32_t)i << 24)) * 0x9E3779B1u;
    h ^= h >> 15;
    return h * 0x85EBCA77u;
}

void display_init(void *picture)
{
    for (int c = 0; c < 256; c++) {
        s_r5[c] = (uint8_t)(c * 31 / 255);
        s_g6[c] = (uint8_t)(c * 63 / 255);
    }
    for (int b = 0; b < 256; b++) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) {
            v |= (uint64_t)((b >> (7 - k)) & 1) << (8 * k);
        }
        s_p2c[b] = v;
    }
    /* dual playfield: the odd planes (bits 0, 2, 4, 6) and the even ones */
    for (int v = 0; v < 256; v++) {
        s_pf1[v] = (uint8_t)((v & 1) | ((v >> 1) & 2) | ((v >> 2) & 4) | ((v >> 3) & 8));
        s_pf2[v] = (uint8_t)(((v >> 1) & 1) | ((v >> 2) & 2) | ((v >> 3) & 4) | ((v >> 4) & 8));
    }
    if (picture == NULL) {
        picture = calloc(1, amiga_picture_size());
    }
    s.rows = picture;
}

void display_reset(void)
{
    row_t *rows = s.rows;
    memset(&s, 0, sizeof(s));
    s.rows = rows;
    memset(s.rows, 0, amiga_picture_size());
    s.fetch_at = NEVER;
    s.writing[0] = s.writing[1] = 1;
    s.first_line = ami.ntsc ? FIRST_LINE_NTSC : FIRST_LINE_PAL;
    s.hash = 0x811C9DC5u;
    s.ncolors = ami.aga ? NCOLORS : 32;
    for (int i = 0; i < s.ncolors; i++) {
        s.pal_hash ^= color_hash(i, 0);
    }
    /* (AGA's: dual playfield's second colours from 8, the sprites' from 16) */
    s.bplcon3 = 0x0C00;
    s.bplcon4 = 0x0011;
}

/* ---- Bitplane DMA ---- */

static bool dma_on(void)
{
    return (ami.dmacon & (DMA_EN | DMA_BPL)) == (DMA_EN | DMA_BPL);
}

static int planes_of(uint16_t con0)
{
    int n = (con0 >> 12) & 7;
    if (ami.aga) {
        n |= (con0 & BPLCON0_BPU3) ? 8 : 0;
        return n > 8 ? 8 : n;
    }
    if (n > 6) {
        n = 4;
    }
    if ((con0 & BPLCON0_SHRES) && n > 2) {
        n = 2;
    } else if ((con0 & BPLCON0_HIRES) && n > 4) {
        n = 4;
    }
    return n;
}

/* Words a bitplane fetch takes: 1, AGA's FMODE 2 or 4 (32 or 64 bits) */
static int fetch_width(uint16_t fmode)
{
    static const uint8_t w[4] = { 1, 2, 2, 4 };
    return ami.aga ? w[fmode & 3] : 1;
}

static void ddf(int *start, int *stop)
{
    *start = s.ddfstrt < 0x18 ? 0x18 : s.ddfstrt;
    *stop = s.ddfstop > 0xD8 ? 0xD8 : s.ddfstop;
}

/* When this line's fetch happens (none if its start has passed) */
static void plan(int at)
{
    s.fetch_at = NEVER;
    int vblank_end = ami_ntsc_beam() ? VBLANK_END_NTSC : VBLANK_END_PAL;
    if (s.fetched || !dma_on() || !s.vwin || ami.line < vblank_end || planes_of(s.bplcon0) == 0) {
        return;
    }
    int start, stop;
    ddf(&start, &stop);
    if (stop < start) {
        return;
    }
    int t = ami.line_start + start * 2;
    if (t >= at) {
        s.fetch_at = t;
    }
}

int display_fetch_at(void)
{
    return s.fetch_at;
}

void display_dma_changed(int at)
{
    if (!s.fetched) {
        plan(at);
        ami_cpu_wake();
    }
}

/* The display window: OCS's VSTOP has V8 = !V7 and HSTOP H8 set; ECS's
 * DIWHIGH, written after DIWSTRT and DIWSTOP, gives those bits instead
 */
static void diw(int *vstart, int *vstop, int *hstart, int *hstop)
{
    if (s.diwhigh_set) {
        *vstart = (s.diwstrt >> 8) | (s.diwhigh & 7) << 8;
        *vstop = (s.diwstop >> 8) | ((s.diwhigh >> 8) & 7) << 8;
        *hstart = (s.diwstrt & 0xFF) | ((s.diwhigh & 0x0020) ? 0x100 : 0);
        *hstop = (s.diwstop & 0xFF) | ((s.diwhigh & 0x2000) ? 0x100 : 0);
    } else {
        *vstart = s.diwstrt >> 8;
        *vstop = (s.diwstop >> 8) | ((s.diwstop & 0x8000) ? 0 : 0x100);
        *hstart = s.diwstrt & 0xFF;
        *hstop = (s.diwstop & 0xFF) | 0x100;
    }
}

static int spr_vstart(const sprite_t *sp)
{
    return (sp->pos >> 8) | ((sp->ctl & 4) << 6);
}

static int spr_vstop(const sprite_t *sp)
{
    return (sp->ctl >> 8) | ((sp->ctl & 2) << 7);
}

/* A sprite's words a line: 1, AGA's FMODE 2 or 4 (32 or 64 pixels) */
static int spr_words(void)
{
    static const uint8_t w[4] = { 1, 2, 2, 4 };
    return ami.aga ? w[(s.fmode >> 2) & 3] : 1;
}

/* A sprite's HSTART in super-hires pixels: AGA's SPRxCTL bits 4 and 3 (70
 * and 35 ns), ECS's bit 4 on a super-hires line (shres)
 */
static int spr_hstart(const sprite_t *sp, bool shres)
{
    int x = ((((sp->pos & 0xFF) << 1) | (sp->ctl & 1)) << 2) | ((sp->ctl >> 4) & 1) << 1;
    if (ami.aga) {
        return x | ((sp->ctl >> 3) & 1);
    }
    return shres ? x : x & ~2;
}

/* BPLCON3's BRDSPRT, sprites in the border too (ECS with ECSENA, AGA);
 * bpl: this line's planes were fetched (their latched BPLCONs)
 */
static bool border_sprites(bool bpl)
{
    uint16_t con0 = bpl ? s.con0 : s.bplcon0, con3 = bpl ? s.con3 : s.bplcon3;
    return (con3 & BPLCON3_BRDSPRT) && (ami.aga || (ami.ecs && (con0 & BPLCON0_ECSENA)));
}

/* Denise shows sprites inside the display window: not above or below it
 * (no bitplane data on those lines to enable them; WinUAE's
 * sprites_hidden, the Deniser project's open issue) unless BRDSPRT
 */
static bool sprites_shown(bool bpl)
{
    return s.vwin || border_sprites(bpl);
}

static void spr_fetch_control(sprite_t *sp, int n)
{
    /* (wide: the control words are the first of each of the two fetches) */
    sp->pos = chip_read16(sp->pt);
    sp->ctl = chip_read16(sp->pt + 2 * n);
    sp->pt += 4 * n;
    sp->armed = false;
}

/* The sprites' DMA for this line */
static void sprite_dma(void)
{
    int first = (ami_ntsc_beam() ? VBLANK_END_NTSC : VBLANK_END_PAL) - 1;
    bool dma = (ami.dmacon & (DMA_EN | DMA_SPR)) == (DMA_EN | DMA_SPR);
    int n = spr_words();
    for (int i = 0; i < 8; i++) {
        sprite_t *sp = &s.spr[i];
        if (ami.line == 0) {
            sp->dma = 0;
        }
        if (!dma || ami.line < first) {
            continue;
        }
        if (ami.line == first) {
            spr_fetch_control(sp, n);
            sp->dma = 1;
            continue;
        }
        if (sp->dma == 0) {
            /* no control words at the first line (the DMA was off then):
             * VSTART as written since starts it all the same (Aerial
             * Racers' cars: the DMA on from line 44, the positions written)
             */
            if (ami.line != spr_vstart(sp)) {
                continue;
            }
            sp->dma = 1;
        }
        sp->armed = false;
        if (sp->dma == 1 && ami.line == spr_vstart(sp)) {
            sp->dma = 2;
        }
        if (sp->dma == 2) {
            if (ami.line == spr_vstop(sp)) {
                spr_fetch_control(sp, n);
                sp->dma = 1;
            } else {
                for (int w = 0; w < n; w++) {
                    sp->data[w] = chip_read16(sp->pt + 2 * w);
                    sp->datb[w] = chip_read16(sp->pt + 2 * (n + w));
                }
                sp->pt += 4 * n;
                sp->armed = true;
            }
        }
    }
}

void display_line_start(void)
{
    s.fetched = false;
    s.changes = 0;
    int vstart, vstop, hstart, hstop;
    diw(&vstart, &vstop, &hstart, &hstop);
    if (ami.line == 0) {
        s.vwin = false;
    }
    if (ami.line == vstart) {
        s.vwin = true;
    }
    if (ami.line == vstop) {
        s.vwin = false;
    }
    plan(ami.line_start);
    sprite_dma();
}

/* Fetched in blocks: each plane max(fw, r) words in 8 * that / r colour
 * clocks (fw the words a fetch, r 1, 2, 4 for lores, hires, super-hires:
 * OCS's 8-clock units), whole blocks, the one DDFSTOP falls in too
 * (Kickstart 1.3's hires screens: $3C-$D0 is 40 words; 3.1's $40-$D0 38).
 * AGA in FMODE 3: Workbench 3.1's hires $38-$D8 44 words (its modulo 72,
 * two planes interleaved), Deluxe Pacman's lores $38-$D0 24 (modulo -8).
 * A plane's words; *wb the block's (for the first pixel)
 */
static int fetch_words(uint16_t con0, uint16_t fmode, int *wb_out)
{
    bool shres = con0 & BPLCON0_SHRES;
    bool hires = !shres && (con0 & BPLCON0_HIRES);
    int start, stop;
    ddf(&start, &stop);
    int fw = fetch_width(fmode), r = shres ? 4 : hires ? 2 : 1;
    int wb = fw > r ? fw : r, block = 8 * wb / r;
    int words = ((stop - start + block - 1) / block + 1) * wb;
    *wb_out = wb;
    return words > MAX_PLANE_WORDS ? MAX_PLANE_WORDS : words;
}

/* The bus slots this line's display DMA takes, what the blitter can't
 * have: the bitplane fetch's (a word each, AGA's wide fetches one for 2 or
 * 4 words) and the sprites' 16 after vertical blank
 */
int display_dma_slots(void)
{
    int n = 0;
    if (s.fetch_at != NEVER || s.fetched) {
        uint16_t con0 = s.fetched ? s.con0 : s.bplcon0, fmode = s.fetched ? s.fmode_line : s.fmode;
        int wb;
        n = planes_of(con0) * fetch_words(con0, fmode, &wb) / fetch_width(fmode);
    }
    int first = (ami_ntsc_beam() ? VBLANK_END_NTSC : VBLANK_END_PAL) - 1;
    if ((ami.dmacon & (DMA_EN | DMA_SPR)) == (DMA_EN | DMA_SPR) && ami.line >= first) {
        n += 16;
    }
    return n;
}

AMI_HOT void display_fetch(void)
{
    s.fetch_at = NEVER;
    s.fetched = true;
    s.con0 = s.bplcon0;
    s.con1 = s.bplcon1;
    s.con2 = s.bplcon2;
    s.con3 = s.bplcon3;
    s.con4 = s.bplcon4;
    s.fmode_line = s.fmode;
    bool shres = s.con0 & BPLCON0_SHRES;
    bool hires = !shres && (s.con0 & BPLCON0_HIRES);
    int planes = planes_of(s.con0);
    int start, stop, wb;
    ddf(&start, &stop);
    int fw = fetch_width(s.fmode);
    int words = fetch_words(s.con0, s.fmode, &wb);
    /* the first pixel after DDFSTRT: 16 * fw / wb + 1 lores pixels (OCS's
     * 17, 9, 5; FMODE 3's 17)
     */
    s.first = start * 2 + 16 * fw / wb + 1;
    s.planes = planes;
    s.words = words;

    uint32_t key = 0x811C9DC5u;
    uint16_t *d = s.data;
    uint32_t align = ~(uint32_t)(2 * fw - 1);
    const uint8_t *chip = ami.chip;
    uint32_t mask = ami.chip_mask & ~1u;
    for (int p = 0; p < planes; p++) {
        uint32_t ptr = s.pt[p] & align;
        for (int w = 0; w < words; w++) {
            uint16_t v = *(const uint16_t *)(chip + (ptr & mask));
            ptr += 2;
            key = (key ^ v) * 0x01000193u;
            *d++ = v;
        }
        s.pt[p] = ptr + s.mod[p & 1];
    }
    s.data_key = key;
    /* lores planes 5-6, hires planes 3-4 and super-hires plane 2 fetch in
     * the 68000's (even) slots (mem.c), in each 8-clock unit from DDFSTRT
     * (the HRM's order: lores - 4 6 2 - 3 5 1, hires 4 2 3 1 4 2 3 1); AGA's
     * 68EC020 doesn't wait for them
     */
    if (!ami.aga && planes > (shres ? 1 : hires ? 2 : 4)) {
        int r = shres ? 4 : hires ? 2 : 1, units = words / r;
        int at = ami.line_start + 2 * (start & ~1);
        for (int u = 0; u < units; u++, at += 16) {
            for (int pos = 0; pos < 8; pos += 2) {
                bool taken = shres ? true
                             : hires ? (pos == 2 || pos == 6 || planes >= 4)
                                     : (pos == 6 || (pos == 2 && planes >= 6));
                if (taken) {
                    bus_take(at + 2 * pos);
                }
            }
        }
    }
}

/* ---- Registers ---- */

/* A colour's value after a write of v (AGA: LOCT writes the low nibbles,
 * otherwise both)
 */
static uint32_t written_color(uint32_t now, uint16_t v)
{
    if (ami.aga && (s.bplcon3 & BPLCON3_LOCT)) {
        return (now & 0xF0F0F0) | ((uint32_t)(v & 0xF00) << 8) | ((v & 0xF0) << 4) | (v & 0xF);
    }
    return expand12(v);
}

static void log_color(int reg, uint16_t v, int at)
{
    if (ami.aga) {
        reg += ((s.bplcon3 >> 13) & 7) * 32;
    }
    int h = at / 2 - ami.line_start / 2;
    int x = 4 * h + 6 - 2 * FIRST_LORES;
    x = x < 0 ? 0 : x > PIC_W ? PIC_W : x;
    if (s.changes == MAX_CHANGES) {
        uint32_t now = (s.late[reg >> 5] & (1u << (reg & 31))) ? s.late_value[reg] : s.color[reg];
        s.late[reg >> 5] |= 1u << (reg & 31);
        s.late_value[reg] = written_color(now, v);
        return;
    }
    /* where it goes (the copper's writes can be ahead of the CPU's) */
    int i = s.changes;
    while (i > 0 && s.change[i - 1].x > x) {
        i--;
    }
    /* the same colour again changes nothing */
    uint32_t now = s.color[reg];
    for (int j = i - 1; j >= 0; j--) {
        if (s.change[j].reg == reg) {
            now = s.change[j].rgb;
            break;
        }
    }
    uint32_t c = written_color(now, v);
    if (now == c) {
        return;
    }
    memmove(&s.change[i + 1], &s.change[i], (s.changes - i) * sizeof(change_t));
    s.change[i] = (change_t){ (uint16_t)x, (uint8_t)reg, c };
    s.changes++;
}

void display_write(uint32_t reg, uint16_t v, int at)
{
    if (reg >= 0x180 && reg <= 0x1BE) {
        log_color((reg - 0x180) >> 1, v, at);
        return;
    }
    if (reg >= 0x120 && reg <= 0x13E) {
        sprite_t *sp = &s.spr[(reg - 0x120) >> 2];
        if (reg & 2) {
            sp->pt = (sp->pt & 0xFFFF0000) | (v & 0xFFFE);
        } else {
            sp->pt = (sp->pt & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        }
        return;
    }
    if (reg >= 0x140 && reg <= 0x17E) {
        sprite_t *sp = &s.spr[(reg - 0x140) >> 3];
        switch (reg & 6) {
        case 0: sp->pos = v; break;
        case 2: sp->ctl = v; sp->armed = false; break;
        case 4: sp->data[0] = v; sp->armed = true; break;
        default: sp->datb[0] = v; break;
        }
        return;
    }
    if (reg >= 0x0E0 && reg <= 0x0FE) {
        int p = (reg - 0x0E0) >> 2;
        if (reg & 2) {
            s.pt[p] = (s.pt[p] & 0xFFFF0000) | (v & 0xFFFE);
        } else {
            s.pt[p] = (s.pt[p] & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        }
        return;
    }
    switch (reg) {
    case 0x08E:
        s.diwstrt = v;
        s.diwhigh_set = false;
        break;
    case 0x090:
        s.diwstop = v;
        s.diwhigh_set = false;
        break;
    case 0x1E4:
        if (ami.ecs) {
            s.diwhigh = v;
            s.diwhigh_set = true;
        }
        break;
    case 0x092:
        s.ddfstrt = v & 0xFC;
        display_dma_changed(at);
        break;
    case 0x094:
        s.ddfstop = v & 0xFC;
        break;
    case 0x100:
        s.bplcon0 = ami.ecs ? v : v & ~BPLCON0_SHRES;
        s.lace_seen |= (v & BPLCON0_LACE) != 0;
        display_dma_changed(at);
        break;
    case 0x102:
        s.bplcon1 = v;
        break;
    case 0x104:
        s.bplcon2 = v;
        break;
    case 0x106:
        s.bplcon3 = v;
        break;
    case 0x108:
        s.mod[0] = (int16_t)(v & 0xFFFE);
        break;
    case 0x10A:
        s.mod[1] = (int16_t)(v & 0xFFFE);
        break;
    case 0x10C:
        if (ami.aga) {
            s.bplcon4 = v;
        }
        break;
    case 0x10E:
        if (ami.aga) {
            s.clxcon2 = v;
        }
        break;
    case 0x1FC:
        if (ami.aga) {
            s.fmode = v;
        }
        break;
    default:
        break;
    }
}

/* The display's registers, for logs */
void amiga_display_regs(char *buf, size_t len)
{
    snprintf(buf, len, "BPLCON0-4 %04x %04x %04x %04x %04x FMODE %04x DDF %02x-%02x DIW %04x-%04x/%04x MOD %d %d COLOR0 %06lx",
             s.bplcon0, s.bplcon1, s.bplcon2, s.bplcon3, s.bplcon4, s.fmode, s.ddfstrt, s.ddfstop, s.diwstrt,
             s.diwstop, s.diwhigh_set ? s.diwhigh : 0, s.mod[0], s.mod[1], (unsigned long)s.color[0]);
}

/* ---- Collisions (CLXDAT) ----
 *
 * Worked out at each line's end while a program reads CLXDAT (for a few
 * frames after each read: it costs time, and most never look): the armed
 * sprites against each other and against the playfields, the odd planes
 * against the even ones, inside the display window. A playfield matches
 * where its planes that CLXCON enables (ENBP) have the values it asks for
 * (MVBP: a plane not fetched is 0); a sprite pair is its even sprite, the
 * odd one too with ENSP. Hires and super-hires pixels count for the lores
 * pixel they're in, the sprites' too (AGA's SPRES, their positions' 35 and
 * 70 ns bits). AGA: planes 7 and 8 through CLXCON2 (a CLXCON write clears
 * it), wide sprites all their pixels.
 */
#define CLX_FRAMES      4

uint16_t display_clxdat(void)
{
    uint16_t v = s.clxdat | 0x8000;
    s.clxdat = 0;
    s.clx_frames = CLX_FRAMES;
    return v;
}

void display_clxcon(uint16_t v)
{
    s.clxcon = v;
    s.clxcon2 = 0;
}

/* 16 pixels of plane p's line from pixel idx on (outside it, or a plane
 * not fetched: 0)
 */
static uint16_t plane16(int p, int planes, int idx)
{
    int n = s.words * 16;
    if (p >= planes || idx <= -16 || idx >= n) {
        return 0;
    }
    const uint16_t *d = s.data + p * s.words;
    int w = idx >= 0 ? idx >> 4 : -1, sh = idx - w * 16;
    uint32_t a = w >= 0 ? d[w] : 0, b = (w + 1) * 16 < n ? d[w + 1] : 0;
    return (uint16_t)(((a << 16) | b) >> (16 - sh));
}

/* Where 16 pixels match the odd (*odd) and even (*even) playfield; idx_o
 * and idx_e: their first pixel in the odd and the even planes
 */
static void matches16(int planes, int idx_o, int idx_e, uint16_t *odd, uint16_t *even)
{
    int enbp = ((s.clxcon >> 6) & 63) | ((s.clxcon2 >> 6) & 3) << 6;
    int mvbp = (s.clxcon & 63) | (s.clxcon2 & 3) << 6;
    uint16_t mo = 0xFFFF, me = 0xFFFF;
    for (int p = 0; p < MAX_PLANES; p++) {
        if (!(enbp & (1 << p))) {
            continue;
        }
        uint16_t w = plane16(p, planes, (p & 1) ? idx_e : idx_o);
        w = (mvbp & (1 << p)) ? w : (uint16_t)~w;
        if (p & 1) {
            me &= w;
        } else {
            mo &= w;
        }
    }
    *odd = mo;
    *even = me;
}

/* 32 hires pixels to their 16 lores ones (either of the two) */
static uint16_t lores16(uint32_t v)
{
    v = (v | (v >> 1)) & 0x55555555u;
    v = (v | (v >> 1)) & 0x33333333u;
    v = (v | (v >> 2)) & 0x0F0F0F0Fu;
    v = (v | (v >> 4)) & 0x00FF00FFu;
    return (uint16_t)(v | (v >> 8));
}

/* Lores pixels L..L+15 inside the window (bit 15: L) */
static uint16_t win16(int L, int hstart, int hstop)
{
    uint16_t m = 0;
    for (int i = 0; i < 16; i++) {
        if (L + i >= hstart && L + i < hstop) {
            m |= (uint16_t)(0x8000 >> i);
        }
    }
    return m;
}

/* The playfields' matches at lores pixels L..L+15 */
static void pf_matches(int L, uint16_t *odd, uint16_t *even)
{
    int planes = s.fetched ? s.planes : 0;
    int d1 = s.con1 & 15, d2 = (s.con1 >> 4) & 15;
    if (planes && (s.con0 & BPLCON0_SHRES)) {
        uint16_t o[4], e[4];
        int at = 4 * (L - s.first);
        for (int k = 0; k < 4; k++) {
            matches16(planes, at - 4 * d1 + 16 * k, at - 4 * d2 + 16 * k, &o[k], &e[k]);
        }
        *odd = lores16((uint32_t)lores16((uint32_t)o[0] << 16 | o[1]) << 16 | lores16((uint32_t)o[2] << 16 | o[3]));
        *even = lores16((uint32_t)lores16((uint32_t)e[0] << 16 | e[1]) << 16 | lores16((uint32_t)e[2] << 16 | e[3]));
    } else if (planes && (s.con0 & BPLCON0_HIRES)) {
        uint16_t o1, e1, o2, e2;
        int at = 2 * (L - s.first);
        matches16(planes, at - 2 * d1, at - 2 * d2, &o1, &e1);
        matches16(planes, at - 2 * d1 + 16, at - 2 * d2 + 16, &o2, &e2);
        *odd = lores16((uint32_t)o1 << 16 | o2);
        *even = lores16((uint32_t)e1 << 16 | e2);
    } else {
        matches16(planes, L - s.first - d1, L - s.first - d2, odd, even);
    }
}

static void collide(void)
{
    int vstart, vstop, hstart, hstop;
    diw(&vstart, &vstop, &hstart, &hstop);
    if (hstart >= hstop) {
        return;
    }
    uint16_t clx = s.clxdat;
    /* odd planes against even ones */
    for (int L = hstart; L < hstop && !(clx & 1); L += 16) {
        uint16_t o, e;
        pf_matches(L, &o, &e);
        if (o & e & win16(L, hstart, hstop)) {
            clx |= 1;
        }
    }
    /* the sprite pairs (their sprites' pixels in spans of 16 lores ones: a
     * 64-pixel lores sprite 4) against the playfields: bits 1-4 the odd
     * one, 5-8 the even one
     */
    uint16_t con3 = s.fetched ? s.con3 : s.bplcon3;
    int spres = ami.aga ? (con3 >> 6) & 3 : 0, nw = spr_words();
    bool shres = s.fetched && (s.con0 & BPLCON0_SHRES), shown = sprites_shown(s.fetched);
    int sx[4][8], spans[4] = { 0 };
    uint16_t sm[4][8];
    for (int i = 0; i < 8; i++) {
        const sprite_t *sp = &s.spr[i];
        if (!sp->armed || ((i & 1) && !(s.clxcon & (0x1000 << (i >> 1)))) || !shown) {
            continue;
        }
        uint16_t w[4], m[4];
        for (int k = 0; k < 4; k++) {
            w[k] = k < nw ? (uint16_t)(sp->data[k] | sp->datb[k]) : 0;
        }
        int n = nw;
        if (spres == 2) {           /* hires sprite pixels: two a lores one */
            m[0] = lores16((uint32_t)w[0] << 16 | w[1]);
            m[1] = lores16((uint32_t)w[2] << 16 | w[3]);
            n = (nw + 1) / 2;
        } else if (spres == 3) {    /* super-hires: four */
            m[0] = lores16((uint32_t)lores16((uint32_t)w[0] << 16 | w[1]) << 16 | lores16((uint32_t)w[2] << 16 | w[3]));
            n = 1;
        } else {
            memcpy(m, w, sizeof(m));
        }
        int g = i >> 1, L = spr_hstart(sp, shres) >> 2;
        for (int k = 0; k < n; k++, L += 16) {
            uint16_t mk = m[k] & win16(L, hstart, hstop);
            if (mk == 0) {
                continue;
            }
            sx[g][spans[g]] = L;
            sm[g][spans[g]] = mk;
            spans[g]++;
            uint16_t o, e;
            pf_matches(L, &o, &e);
            clx |= (uint16_t)((mk & o) ? 2 << g : 0);
            clx |= (uint16_t)((mk & e) ? 0x20 << g : 0);
        }
    }
    /* pairs against pairs: bits 9-14 (0-1, 0-2, 0-3, 1-2, 1-3, 2-3) */
    int bit = 9;
    for (int a = 0; a < 4; a++) {
        for (int b = a + 1; b < 4; b++, bit++) {
            for (int i = 0; i < spans[a]; i++) {
                for (int j = 0; j < spans[b]; j++) {
                    int dx = sx[b][j] - sx[a][i];
                    if (dx > -16 && dx < 16) {
                        uint32_t mb = dx >= 0 ? (uint32_t)sm[b][j] >> dx : (uint32_t)sm[b][j] << -dx;
                        clx |= (uint16_t)((sm[a][i] & mb & 0xFFFF) ? 1u << bit : 0);
                    }
                }
            }
        }
    }
    s.clxdat = clx;
}

/* ---- The line's record ---- */

static inline uint32_t mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 0x01000193u;
}

static void window(int *x0, int *x1)
{
    int vstart, vstop, hstart, hstop;
    diw(&vstart, &vstop, &hstart, &hstop);
    int a = 2 * (hstart - FIRST_LORES), b = 2 * (hstop - FIRST_LORES);
    *x0 = a < 0 ? 0 : a > PIC_W ? PIC_W : a;
    *x1 = b < 0 ? 0 : b > PIC_W ? PIC_W : b;
}

static inline void set_line_color(int i, uint32_t c)
{
    if (s.color[i] != c) {
        s.pal_hash ^= color_hash(i, s.color[i]) ^ color_hash(i, c);
        s.color[i] = c;
    }
}

/* The colours as the next line begins: this line's changes applied */
static void apply_changes(void)
{
    for (int i = 0; i < s.changes; i++) {
        set_line_color(s.change[i].reg, s.change[i].rgb);
    }
    for (int w = 0; w < NCOLORS / 32; w++) {
        while (s.late[w]) {
            int b = __builtin_ctz(s.late[w]);
            set_line_color(w * 32 + b, s.late_value[w * 32 + b]);
            s.late[w] &= s.late[w] - 1;
        }
    }
    s.changes = 0;
}

AMI_HOT void display_line_end(void)
{
    int row = ami.line - s.first_line;
    if (row < 0 || row >= PIC_LINES) {
        apply_changes();
        return;
    }
    int x0, x1;
    window(&x0, &x1);
    if (s.clx_frames) {
        collide();
    }
    bool bpl = s.fetched && x0 < x1;
    bool ecs_shres = bpl && !ami.aga && (s.con0 & BPLCON0_SHRES), brd = border_sprites(bpl);
    /* the sprites showing their data on this line */
    int nspr = 0, nw = spr_words();
    uint8_t num[8];
    for (int i = 0; i < (ecs_shres ? 4 : 8) && (x0 < x1 || brd) && sprites_shown(bpl); i++) {
        const sprite_t *sp = &s.spr[i];
        if (sp->armed) {
            num[nspr++] = (uint8_t)(i | ((i & 1) && (sp->ctl & 0x80) ? 0x80 : 0));
        }
    }
    uint32_t h = 0x811C9DC5u;
    if (bpl) {
        h = mix(h, s.data_key);
        h = mix(h, (uint32_t)s.con0 << 16 | s.con1);
        h = mix(h, (uint32_t)(s.first & 0xFFFF));
    }
    if (bpl || nspr) {
        h = mix(h, (uint32_t)s.con2 << 16 | s.bplcon2);
        h = mix(h, (uint32_t)x0 << 16 | (uint32_t)x1);
        h = mix(h, s.pal_hash);
    } else {
        h = mix(h, s.color[0]);
    }
    if (ami.aga) {
        h = mix(h, (uint32_t)(bpl ? s.con3 : s.bplcon3) << 16 | (bpl ? s.con4 : s.bplcon4));
        h = mix(h, (uint32_t)s.fmode << 16 | (bpl ? s.fmode_line : 0));
    }
    if (nspr) {
        h = mix(h, brd);
    }
    for (int i = 0; i < nspr; i++) {
        const sprite_t *sp = &s.spr[num[i] & 7];
        h = mix(h, (uint32_t)num[i] << 24 | (uint32_t)(sp->pos & 0xFF) << 8 | (sp->ctl & 0x19));
        for (int w = 0; w < nw; w++) {
            h = mix(h, (uint32_t)sp->data[w] << 16 | sp->datb[w]);
        }
    }
    for (int i = 0; i < s.changes; i++) {
        h = mix(h, (uint32_t)s.change[i].x << 16 | s.change[i].reg);
        h = mix(h, s.change[i].rgb);
    }
    uint32_t key = h | 1;
    s.hash = mix(s.hash, key);
    s.shres_seen |= bpl && (s.con0 & BPLCON0_SHRES);

    row_t *r = &buffer(s.field, s.writing[s.field])[row];
    if (r->key != key) {
        r->key = key;
        r->con0 = s.con0;
        r->con1 = s.con1;
        r->con2 = s.con2;
        r->con3 = bpl ? s.con3 : s.bplcon3;
        r->con4 = bpl ? s.con4 : s.bplcon4;
        r->fmode = bpl ? s.fmode_line : s.fmode;
        r->first = (int16_t)s.first;
        r->x0 = (int16_t)x0;
        r->x1 = (int16_t)x1;
        r->planes = bpl ? (uint8_t)s.planes : 0;
        r->ncolors = (uint8_t)(s.ncolors - 1);
        r->words = (uint16_t)s.words;
        r->changes = (uint16_t)s.changes;
        r->sprites = (uint8_t)nspr;
        r->brdsprt = brd;
        r->spr_words = (uint8_t)nw;
        for (int i = 0; i < nspr; i++) {
            const sprite_t *sp = &s.spr[num[i] & 7];
            r->spr_num[i] = num[i];
            r->spr_x[i] = (int16_t)(spr_hstart(sp, bpl && (s.con0 & BPLCON0_SHRES)) - 4 * FIRST_LORES);
            memcpy(r->spr_a[i], sp->data, nw * sizeof(uint16_t));
            memcpy(r->spr_b[i], sp->datb, nw * sizeof(uint16_t));
        }
        if (!bpl) {
            r->con2 = s.bplcon2;
        }
        memcpy(r->color, s.color, s.ncolors * sizeof(uint32_t));
        memcpy(r->change, s.change, s.changes * sizeof(change_t));
        if (bpl) {
            memcpy(r->data, s.data, s.planes * s.words * sizeof(uint16_t));
        }
    }
    apply_changes();
}

/* The frame's records become its field's latest; true when it was
 * interlaced (the long frames then alternate with short ones)
 */
bool display_frame_end(void)
{
    if (s.clx_frames) {
        s.clx_frames--;
    }
    int f = s.field, w = s.writing[f];
    bool lace = s.lace_seen;
    s.signature[f][w] = s.hash;
    s.lace[f][w] = lace;
    s.shres[f][w] = s.shres_seen;
    s.shres_seen = false;
    s.hash = 0x811C9DC5u;
    if (f == 1) {
        s.field1 = true;
    } else if (!lace) {
        s.field1 = false;
    }
    uint32_t st = __atomic_load_n(&s.state[f], __ATOMIC_ACQUIRE), want;
    do {
        want = (st & ~3u) | (uint32_t)w;
    } while (!__atomic_compare_exchange_n(&s.state[f], &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    uint32_t pinned = PINNED(want);
    for (int i = 0; i < 3; i++) {
        if (i != w && (uint32_t)i != pinned) {
            s.writing[f] = i;
            break;
        }
    }
    s.lace_seen = (s.bplcon0 & BPLCON0_LACE) != 0;
    return lace;
}

/* The next frame's field: its LOF (a short frame of an interlaced picture
 * draws the odd lines)
 */
void display_next_field(bool lace)
{
    s.field = lace && !ami.lof ? 1 : 0;
    s.first_line = ami_ntsc_beam() ? FIRST_LINE_NTSC : FIRST_LINE_PAL;
}

/* ---- The presenter's side: drawing the records ---- */

/* The pixels of a line's planes from MARGIN on; before it room for the
 * scroll (a super-hires fetch from DDFSTRT $D8: 4 x (108 - 437 - 15), and
 * AGA's delays up to 255 more)
 */
#define MARGIN          1664
#define BUF             (MARGIN + MAX_PLANE_WORDS * 16 + 64)

static struct {
    uint8_t odd[BUF] __attribute__((aligned(8)));
    uint8_t even[BUF] __attribute__((aligned(8)));
    int filled;
    bool aga;                       /* the row being drawn: AGA's */
    bool ehb;                       /* 6 planes are 32 colours and their halves */
    uint8_t bplam;                  /* AGA: XORed into the pixels */
    uint32_t color[NCOLORS];        /* 24-bit */
    uint16_t rgb[NCOLORS];          /* RGB565 */
    uint16_t pix[NCOLORS];          /* a pixel's RGB565 (EHB's halves, BPLAM) */
    uint16_t shi[32], slo[32];      /* ECS super-hires: each register's upper and lower gun bits */
    uint32_t ham;
    const row_t *row;               /* the row in line[] */
    uint32_t key;
    bool wide;                      /* line[] drawn WIDE_W wide */
    uint16_t line[WIDE_W] __attribute__((aligned(4)));
    uint16_t tmp[WIDE_W] __attribute__((aligned(4)));
} d;

/* Half a colour (EHB): OCS's and ECS's halve the 4-bit guns */
static inline uint32_t half(uint32_t c)
{
    if (d.aga) {
        return (c >> 1) & 0x7F7F7F;
    }
    uint16_t c12 = (uint16_t)(((c >> 12) & 0xF00) | ((c >> 8) & 0xF0) | ((c >> 4) & 0xF));
    return expand12((c12 >> 1) & 0x777);
}

static inline void set_color(int reg, uint32_t c)
{
    d.color[reg] = c;
    d.rgb[reg] = to565(c);
    if (d.ehb) {
        if (reg < 32) {
            d.pix[reg] = d.rgb[reg];
            d.pix[reg + 32] = to565(half(c));
        }
    } else {
        d.pix[reg ^ d.bplam] = d.rgb[reg];
    }
}

/* (a and b even) c0 at even pixels, c1 at odd ones */
static void fill2(uint16_t *out, int a, int b, uint16_t c0, uint16_t c1)
{
    uint16_t pair[2] = { c0, c1 };
    uint32_t *o32 = (uint32_t *)out, c2;
    memcpy(&c2, pair, sizeof(c2));
    for (int i = a / 2; i < b / 2; i++) {
        o32[i] = c2;
    }
}

static inline void fill(uint16_t *out, int a, int b, uint16_t c)
{
    fill2(out, a, b, c, c);
}

/* The scroll delays of the odd and even planes, in super-hires pixels
 * (OCS's four bits in lores pixels; AGA's eight, as far as a fetch
 * reaches, at least OCS's 16 lores pixels: Aerial Racers' race sets the
 * top two with 16-bit fetches, and they don't move it)
 */
static void delays(const row_t *r, int *d1, int *d2)
{
    uint16_t c = r->con1;
    *d1 = (c & 15) << 2;
    *d2 = ((c >> 4) & 15) << 2;
    if (d.aga) {
        int res = (r->con0 & BPLCON0_SHRES) ? 4 : (r->con0 & BPLCON0_HIRES) ? 2 : 1;
        int reach = 64 * fetch_width(r->fmode) / res;
        int mask = (reach > 64 ? reach : 64) - 1;
        *d1 = (*d1 | ((c >> 8) & 3) | ((c >> 10) & 3) << 6) & mask;
        *d2 = (*d2 | ((c >> 12) & 3) | ((c >> 14) & 3) << 6) & mask;
    }
}

/* Dual playfield's colour of a pixel (pf2 offset: AGA's BPLCON3, else 8) */
static inline int dpf_index(const row_t *r, int v)
{
    static const uint8_t offset[8] = { 0, 2, 4, 8, 16, 32, 64, 128 };
    int p1 = s_pf1[v], p2 = s_pf2[v];
    int o2 = d.aga ? offset[(r->con3 >> 10) & 7] : 8;
    bool pri2 = r->con2 & BPLCON2_PF2PRI;
    return pri2 ? (p2 ? o2 + p2 : p1) : (p1 ? p1 : p2 ? o2 + p2 : 0);
}

/* HAM: 6 planes (the top two say what the low four do) or AGA's 8 (the
 * low two say what the top six do)
 */
static inline uint32_t ham_pixel(int v, bool ham8)
{
    uint32_t c = d.ham;
    if (ham8) {
        int val = v >> 2;
        switch (v & 3) {
        case 0: c = d.color[val]; break;
        case 1: c = (c & 0xFFFF00) | (uint32_t)(val << 2) | (c & 3); break;
        case 2: c = (c & 0x00FFFF) | (uint32_t)(val << 2) << 16 | (c & 0x030000); break;
        default: c = (c & 0xFF00FF) | (uint32_t)(val << 2) << 8 | (c & 0x0300); break;
        }
    } else {
        int val = v & 15;
        switch ((v >> 4) & 3) {
        case 0: c = d.color[val]; break;
        case 1: c = (c & 0xFFFF00) | (uint32_t)(val * 17); break;
        case 2: c = (c & 0x00FFFF) | (uint32_t)(val * 17) << 16; break;
        default: c = (c & 0xFF00FF) | (uint32_t)(val * 17) << 8; break;
        }
    }
    d.ham = c;
    return c;
}

static void compose(const row_t *r, uint16_t *out, int xa, int xb)
{
    uint16_t con0 = r->con0;
    int d1, d2;
    delays(r, &d1, &d2);
    bool ham = (con0 & BPLCON0_HAM) && r->planes >= 5, ham8 = ham && r->planes == 8;
    if (con0 & BPLCON0_HIRES) {
        const uint8_t *po = d.odd + MARGIN + 2 * (FIRST_LORES - r->first) - d1 / 2;
        const uint8_t *pe = d.even + MARGIN + 2 * (FIRST_LORES - r->first) - d2 / 2;
        if (con0 & BPLCON0_DPF) {
            for (int x = xa; x < xb; x++) {
                out[x] = d.rgb[dpf_index(r, po[x] | pe[x])];
            }
            return;
        }
        if (ham) {
            for (int x = xa; x < xb; x++) {
                out[x] = to565(ham_pixel(po[x] | pe[x], ham8));
            }
            return;
        }
        for (int x = xa; x < xb; x++) {
            out[x] = d.pix[po[x] | pe[x]];
        }
        return;
    }
    const uint8_t *po = d.odd + MARGIN + FIRST_LORES - r->first - d1 / 4;
    const uint8_t *pe = d.even + MARGIN + FIRST_LORES - r->first - d2 / 4;
    uint32_t *o32 = (uint32_t *)out;
    int pa = xa / 2, pb = xb / 2;
    if (con0 & BPLCON0_DPF) {
        for (int p = pa; p < pb; p++) {
            o32[p] = d.rgb[dpf_index(r, po[p] | pe[p])] * 0x10001u;
        }
        return;
    }
    if (ham) {
        for (int p = pa; p < pb; p++) {
            o32[p] = to565(ham_pixel(po[p] | pe[p], ham8)) * 0x10001u;
        }
        return;
    }
    for (int p = pa; p < pb; p++) {
        o32[p] = d.pix[po[p] | pe[p]] * 0x10001u;
    }
}

/* ECS super-hires colours from the registers (see the top): the first
 * pixel of a pair shows shi[], the second slo[], each gun's 2 bits doubled
 */
static void shres_colors(void)
{
    for (int i = 0; i < 32; i++) {
        uint32_t c = d.color[i];
        uint16_t c12 = (uint16_t)(((c >> 12) & 0xF00) | ((c >> 8) & 0xF0) | ((c >> 4) & 0xF));
        uint16_t hi = c12 & 0xCCC, lo = (uint16_t)((c12 & 0x333) << 2);
        d.shi[i] = to565(expand12(hi | hi >> 2));
        d.slo[i] = to565(expand12(lo | lo >> 2));
    }
}

/* The border's colour: colour 0, or black (AGA's BRDRBLNK) */
static inline uint16_t border(const row_t *r)
{
    return d.aga && (r->con3 & BPLCON3_BRDRBLNK) ? 0 : d.rgb[0];
}

static void span(const row_t *r, uint16_t *out, int a, int b)
{
    int ws = a > r->x0 ? a : r->x0, we = b < r->x1 ? b : r->x1;
    uint16_t bc = border(r);
    if (ws >= we) {
        fill(out, a, b, bc);
        return;
    }
    fill(out, a, ws, bc);
    if (r->planes == 0) {
        fill(out, ws, we, d.rgb[0]);
    } else {
        compose(r, out, ws, we);
    }
    fill(out, we, b, bc);
}

/* The playfield's priority code at a picture pixel (BPLCON2: a sprite pair
 * shows in front of a playfield whose code is above the pair's number), or
 * 8 where the playfields are transparent; scale: the picture's pixels a
 * hires one (2 in a super-hires frame)
 */
static int pf_code(const row_t *r, int x, int scale)
{
    int x0 = r->x0 * scale, x1 = r->x1 * scale;
    if (r->planes == 0 || x < x0 || x >= x1) {
        return 8;
    }
    uint16_t con0 = r->con0;
    int d1, d2;
    delays(r, &d1, &d2);
    int v;
    if (con0 & BPLCON0_SHRES) {
        int sx = scale == 2 ? x : 2 * x;
        v = d.odd[MARGIN + 4 * (FIRST_LORES - r->first) - d1 + sx] | d.even[MARGIN + 4 * (FIRST_LORES - r->first) - d2 + sx];
    } else if (con0 & BPLCON0_HIRES) {
        int hx = x / scale;
        v = d.odd[MARGIN + 2 * (FIRST_LORES - r->first) - d1 / 2 + hx] |
            d.even[MARGIN + 2 * (FIRST_LORES - r->first) - d2 / 2 + hx];
    } else {
        int p = x / (2 * scale);
        v = d.odd[MARGIN + FIRST_LORES - r->first - d1 / 4 + p] | d.even[MARGIN + FIRST_LORES - r->first - d2 / 4 + p];
    }
    int pf1p = r->con2 & 7, pf2p = (r->con2 >> 3) & 7;
    if (con0 & BPLCON0_DPF) {
        int p1 = s_pf1[v], p2 = s_pf2[v];
        bool pri2 = r->con2 & BPLCON2_PF2PRI;
        if (pri2 ? p2 : !p1 && p2) {
            return pf2p;
        }
        return p1 ? pf1p : 8;
    }
    return v ? pf2p : 8;
}

/* The sprites over the line: lower numbers in front, then the playfields'
 * priorities. Worked out in super-hires pixels (4 a lores one); a sprite
 * pixel is 4 of them, AGA's hires and super-hires ones (BPLCON3's SPRES)
 * 2 and 1. wide: a super-hires frame (the picture's pixels are those).
 */
#define SPR_UNITS       (4 * 360)

static void draw_sprites(const row_t *r, uint16_t *out, bool wide)
{
    static uint8_t col[SPR_UNITS];      /* colour or 0, per super-hires pixel */
    static uint8_t pair[SPR_UNITS];
    int lo = SPR_UNITS, hi = 0;
    int nw = r->spr_words, width = 16 * nw;
    int spres = d.aga ? (r->con3 >> 6) & 3 : 0;
    int unit = spres == 2 ? 2 : spres == 3 ? 1 : 4;
    int ebank = d.aga ? ((r->con4 >> 4) & 15) * 16 : 16, obank = d.aga ? (r->con4 & 15) * 16 : 16;
    for (int k = r->sprites - 1; k >= 0; k--) {
        int n = r->spr_num[k] & 7;
        bool attached = r->spr_num[k] & 0x80;
        int x = r->spr_x[k];
        const uint16_t *a = r->spr_a[k], *b = r->spr_b[k];
        /* an attached odd sprite: its bits over its partner's (k - 1) */
        const uint16_t *pa = NULL, *pb = NULL;
        if (attached && k > 0 && (r->spr_num[k - 1] & 7) == n - 1) {
            pa = r->spr_a[k - 1];
            pb = r->spr_b[k - 1];
        }
        for (int i = 0; i < width; i++) {
            int bit = 15 - (i & 15), w = i >> 4;
            int v = ((a[w] >> bit) & 1) | (((b[w] >> bit) & 1) << 1);
            int c;
            if (attached) {
                v <<= 2;
                if (pa) {
                    v |= ((pa[w] >> bit) & 1) | (((pb[w] >> bit) & 1) << 1);
                }
                c = obank + v;
            } else {
                c = ((n & 1) ? obank : ebank) + (n >> 1) * 4 + v;
            }
            if (!v) {
                continue;
            }
            for (int u = x + i * unit; u < x + (i + 1) * unit; u++) {
                if (u >= 0 && u < SPR_UNITS) {
                    col[u] = (uint8_t)c;
                    pair[u] = (uint8_t)(n >> 1);
                    lo = u < lo ? u : lo;
                    hi = u > hi ? u : hi;
                }
            }
        }
        if (pa) {
            k--;                        /* (its partner drawn with it) */
        }
    }
    bool ecs_shres = wide && !d.aga && (r->con0 & BPLCON0_SHRES) && r->planes;
    int step = wide ? 1 : 2, scale = wide ? 2 : 1;
    int x0 = r->brdsprt ? 0 : r->x0 * scale, x1 = r->brdsprt ? (wide ? WIDE_W : PIC_W) : r->x1 * scale;
    for (int u = lo; u <= hi; u++) {
        if (!col[u] || u % step) {
            continue;
        }
        int x = u / step;
        if (x < x0 || x >= x1 || pair[u] >= pf_code(r, x, scale)) {
            continue;
        }
        if (ecs_shres) {
            /* a lores pixel's two halves paired */
            int b = u & ~3, v0 = col[b] ? (col[b] - 16) & 3 : 0, v1 = col[b + 2] ? (col[b + 2] - 16) & 3 : 0;
            int reg = 16 + 4 * v1 + v0;
            out[x] = (u & 2) ? d.slo[reg] : d.shi[reg];
        } else {
            out[x] = d.rgb[col[u]];
        }
    }
    if (lo <= hi) {
        memset(col + lo, 0, (size_t)(hi - lo + 1));
    }
}

/* A super-hires line, WIDE_W pixels (the planes already in odd[], even[]):
 * ECS's 4 colours, AGA's own
 */
static void draw_shres(const row_t *r, uint16_t *out)
{
    int d1, d2;
    delays(r, &d1, &d2);
    const uint8_t *po = d.odd + MARGIN + 4 * (FIRST_LORES - r->first) - d1;
    const uint8_t *pe = d.even + MARGIN + 4 * (FIRST_LORES - r->first) - d2;
    int x0 = 2 * r->x0, x1 = 2 * r->x1;
    bool ham = (r->con0 & BPLCON0_HAM) && r->planes >= 5, ham8 = ham && r->planes == 8;
    if (!d.aga) {
        shres_colors();
    }
    int ci = 0, x = 0, n = r->changes;
    while (x < WIDE_W) {
        bool changed = false;
        while (ci < n && 2 * (int)r->change[ci].x <= x) {
            set_color(r->change[ci].reg, r->change[ci].rgb);
            ci++;
            changed = true;
        }
        if (changed && !d.aga) {
            shres_colors();
        }
        /* (ECS: the border's pairs are colour 0's) */
        uint16_t bc0 = d.aga ? border(r) : d.shi[0], bc1 = d.aga ? bc0 : d.slo[0];
        int end = ci < n ? 2 * (int)r->change[ci].x : WIDE_W;
        int ws = x > x0 ? x : x0, we = end < x1 ? end : x1;
        if (ws >= we) {
            fill2(out, x, end, bc0, bc1);
        } else {
            fill2(out, x, ws, bc0, bc1);
            if (!d.aga) {
                for (int i = ws; i < we; i += 2) {
                    int reg = 4 * (po[i + 1] | pe[i + 1]) + (po[i] | pe[i]);
                    out[i] = d.shi[reg];
                    out[i + 1] = d.slo[reg];
                }
            } else if (r->con0 & BPLCON0_DPF) {
                for (int i = ws; i < we; i++) {
                    out[i] = d.rgb[dpf_index(r, po[i] | pe[i])];
                }
            } else if (ham) {
                for (int i = ws; i < we; i++) {
                    out[i] = to565(ham_pixel(po[i] | pe[i], ham8));
                }
            } else {
                for (int i = ws; i < we; i++) {
                    out[i] = d.pix[po[i] | pe[i]];
                }
            }
            fill2(out, we, end, bc0, bc1);
        }
        x = end;
    }
    if (r->sprites) {
        draw_sprites(r, out, true);
    }
}

/* A line, PIC_W pixels or (wide) WIDE_W */
static void draw(const row_t *r, uint16_t *out, bool wide)
{
    if (r->key == 0) {
        memset(out, 0, (wide ? WIDE_W : PIC_W) * sizeof(uint16_t));
        return;
    }
    int ncolors = r->ncolors + 1;
    d.aga = ncolors > 32;
    d.ehb = !d.aga || (r->planes == 6 && !(r->con0 & (BPLCON0_HAM | BPLCON0_DPF)) && !(r->con2 & BPLCON2_KILLEHB));
    d.bplam = d.aga && !d.ehb ? (uint8_t)(r->con4 >> 8) : 0;
    for (int i = 0; i < ncolors; i++) {
        set_color(i, r->color[i]);
    }
    if (r->planes) {
        memset(d.odd + MARGIN, 0, d.filled);
        memset(d.even + MARGIN, 0, d.filled);
        int words = r->words;
        d.filled = words * 16;
        const uint16_t *src = r->data;
        for (int p = 0; p < r->planes; p++) {
            uint64_t *dst = (uint64_t *)(((p & 1) ? d.even : d.odd) + MARGIN);
            for (int w = 0; w < words; w++) {
                uint16_t v = *src++;
                dst[2 * w] |= s_p2c[v >> 8] << p;
                dst[2 * w + 1] |= s_p2c[v & 0xFF] << p;
            }
        }
    }
    d.ham = d.color[0];
    if (r->planes && (r->con0 & BPLCON0_SHRES)) {
        if (wide) {
            draw_shres(r, out);
            return;
        }
        /* (not the presenter's frame's width: every other pixel) */
        draw_shres(r, d.tmp);
        for (int i = 0; i < PIC_W; i++) {
            out[i] = d.tmp[2 * i];
        }
        return;
    }
    int ci = 0, x = 0, n = r->changes;
    while (x < PIC_W) {
        while (ci < n && (int)r->change[ci].x <= x) {
            set_color(r->change[ci].reg, r->change[ci].rgb);
            ci++;
        }
        int end = ci < n ? (int)r->change[ci].x : PIC_W;
        span(r, out, x, end);
        x = end;
    }
    if (r->sprites) {
        draw_sprites(r, out, false);
    }
    if (wide) {
        for (int i = PIC_W - 1; i >= 0; i--) {
            out[2 * i] = out[2 * i + 1] = out[i];
        }
    }
}

/* The presenter's frame of a field: its latest, pinned (the writer then
 * leaves it alone)
 */
static uint32_t pin(int f)
{
    uint32_t st = __atomic_load_n(&s.state[f], __ATOMIC_ACQUIRE), want;
    do {
        want = (st & ~(3u << 2)) | LATEST(st) << 2;
    } while (!__atomic_compare_exchange_n(&s.state[f], &st, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    return LATEST(st);
}

static bool s_weave;                /* (the presenter's) both fields, line by line */
static bool s_wide;                 /* the presenter's frame: WIDE_W wide */
static bool s_dbg_wide;             /* the debugger's (told when it asked the size) */
static uint32_t s_pinned_frame;     /* when the presenter last pinned */
static volatile uint32_t s_draws;   /* lines drawn (the debugger's check) */

static uint32_t pin_latest(bool *wide)
{
    uint32_t b0 = pin(0);
    s_weave = s.lace[0][b0] && s.field1;
    *wide = s.shres[0][b0];
    if (!s_weave) {
        return s.signature[0][b0];
    }
    uint32_t b1 = pin(1);
    *wide |= s.shres[1][b1];
    return s.signature[0][b0] ^ (s.signature[1][b1] * 0x9E3779B1u);
}

uint32_t amiga_screen_signature(void)
{
    s_pinned_frame = ami.frames;
    bool wide;
    uint32_t sig = pin_latest(&wide);
    s_wide = wide;
    return sig ^ (wide ? 0x5A5A5A5Au : 0);
}

int amiga_screen_width(void)
{
    return s_wide ? WIDE_W : PIC_W;
}

static void render(int y, uint16_t *dst, bool wide)
{
    s_draws++;
    int row = y >> 1;
    if (s.rows == NULL || row < 0 || row >= PIC_LINES) {
        memset(dst, 0, (wide ? WIDE_W : PIC_W) * sizeof(uint16_t));
        return;
    }
    int f = s_weave ? y & 1 : 0;
    uint32_t pinned = PINNED(__atomic_load_n(&s.state[f], __ATOMIC_ACQUIRE));
    const row_t *r = &buffer(f, (int)pinned)[row];
    if (r != d.row || r->key != d.key || wide != d.wide) {
        d.row = NULL;               /* (half drawn: see display_dbg_line()) */
        draw(r, d.line, wide);
        d.row = r;
        d.key = r->key;
        d.wide = wide;
    }
    memcpy(dst, d.line, (wide ? WIDE_W : PIC_W) * sizeof(uint16_t));
}

void amiga_render_line_rgb565(int y, uint16_t *dst)
{
    render(y, dst, s_wide);
}

/* The debugger's picture, from its task (on the board below the presenter,
 * on the same core: the presenter may draw in the middle of the debugger's
 * line, never the other way round). The presenter's frame, or the latest
 * when nothing pins (the host build, no presenter). Its width.
 */
int display_dbg_frame(void)
{
    bool wide = s_wide;
    if (ami.frames - s_pinned_frame > 25) {
        pin_latest(&wide);
    }
    s_dbg_wide = wide;
    return wide ? WIDE_W : PIC_W;
}

/* A line for the debugger: drawn again if the presenter drew meanwhile
 * (they share the drawing's state)
 */
void display_dbg_line(int y, uint16_t *dst)
{
    for (int tries = 0; tries < 8; tries++) {
        uint32_t before = s_draws;
        render(y, dst, s_dbg_wide);
        if (s_draws == before + 1) {
            return;
        }
    }
}

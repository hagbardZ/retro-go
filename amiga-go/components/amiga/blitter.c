/*
 * The blitter: area mode (all minterms, the A and B shifts, first and last
 * word masks, descending, inclusive and exclusive fill) and line mode.
 *
 * A blit is done the moment it starts; BBUSY stays set and the interrupt
 * waits for as long as the real one would take: its words times the
 * colour clocks a word needs with those channels, each a bus slot it gets
 * only where the line's other DMA leaves one (refresh, disk, audio,
 * sprites, the bitplanes: 4 hires planes leave none in the fetch window).
 * The copper's few slots aren't counted.
 */
#include "amiga_internal.h"

blitter_t blit;

enum { A, B, C, D };

#define USEA    0x0800
#define USEB    0x0400
#define USEC    0x0200
#define USED    0x0100

#define LINE    0x0001
#define DESC    0x0002
#define FCI     0x0004
#define IFE     0x0008
#define EFE     0x0010

/* Colour clocks a word takes, by BLTCON0's USE bits (D, C, B, A) */
static const uint8_t s_word_cck[16] = { 2, 2, 2, 3, 2, 3, 2, 3, 2, 2, 2, 3, 2, 3, 3, 4 };

/* Fill: [exclusive][carry in][byte] -> the filled byte, bit 8 the carry out */
static uint16_t s_fill[2][2][256];

static void init_fill(void)
{
    for (int ex = 0; ex < 2; ex++) {
        for (int c = 0; c < 2; c++) {
            for (int b = 0; b < 256; b++) {
                int carry = c, out = 0;
                for (int i = 0; i < 8; i++) {
                    int bit = (b >> i) & 1;
                    carry ^= bit;
                    out |= (ex ? carry : (bit | carry)) << i;
                }
                s_fill[ex][c][b] = (uint16_t)(out | (carry << 8));
            }
        }
    }
}

static inline uint16_t minterm(uint8_t lf, uint16_t a, uint16_t b, uint16_t c)
{
    switch (lf) {
    case 0x00: return 0;
    case 0xF0: return a;
    case 0xCC: return b;
    case 0xAA: return c;
    case 0xCA: return (a & b) | (~a & c);       /* cookie cut */
    case 0x0A: return ~a & c;
    case 0xFA: return a | c;
    case 0xEA: return (a & b) | c;
    case 0x5A: return a ^ c;
    case 0xFF: return 0xFFFF;
    default: {
        uint16_t d = 0;
        if (lf & 0x80) d |= a & b & c;
        if (lf & 0x40) d |= a & b & ~c;
        if (lf & 0x20) d |= a & ~b & c;
        if (lf & 0x10) d |= a & ~b & ~c;
        if (lf & 0x08) d |= ~a & b & c;
        if (lf & 0x04) d |= ~a & b & ~c;
        if (lf & 0x02) d |= ~a & ~b & c;
        if (lf & 0x01) d |= ~a & ~b & ~c;
        return d;
    }
    }
}

static int blit_area(void)
{
    uint16_t con0 = blit.con0, con1 = blit.con1;
    bool desc = con1 & DESC;
    int ash = con0 >> 12, bsh = con1 >> 12;
    uint8_t lf = con0 & 0xFF;
    bool fill = (con1 & (IFE | EFE)) != 0;
    int ex = (con1 & EFE) ? 1 : 0;
    int inc = desc ? -2 : 2;
    int32_t mod[4];
    for (int i = 0; i < 4; i++) {
        mod[i] = desc ? -blit.mod[i] : blit.mod[i];
    }
    uint32_t apt = blit.pt[A], bpt = blit.pt[B], cpt = blit.pt[C], dpt = blit.pt[D];
    uint16_t adat = blit.dat[A], bdat = blit.dat[B], cdat = blit.dat[C];
    uint16_t aold = 0, bold = 0;
    uint32_t w = blit.width, h = blit.height;
    bool zero = true;

    for (uint32_t y = 0; y < h; y++) {
        int carry = (con1 & FCI) ? 1 : 0;
        for (uint32_t x = 0; x < w; x++) {
            if (con0 & USEA) {
                adat = chip_read16(apt);
                apt += inc;
            }
            if (con0 & USEB) {
                bdat = chip_read16(bpt);
                bpt += inc;
            }
            if (con0 & USEC) {
                cdat = chip_read16(cpt);
                cpt += inc;
            }
            uint16_t am = adat;
            if (x == 0) {
                am &= blit.afwm;
            }
            if (x == w - 1) {
                am &= blit.alwm;
            }
            uint16_t a, b;
            if (!desc) {
                a = (uint16_t)((((uint32_t)aold << 16) | am) >> ash);
                b = (uint16_t)((((uint32_t)bold << 16) | bdat) >> bsh);
            } else {
                a = (uint16_t)((((uint32_t)am << 16) | aold) >> (16 - ash));
                b = (uint16_t)((((uint32_t)bdat << 16) | bold) >> (16 - bsh));
            }
            aold = am;
            bold = bdat;
            uint16_t d = minterm(lf, a, b, cdat);
            if (fill) {
                uint16_t lo = s_fill[ex][carry][d & 0xFF];
                uint16_t hi = s_fill[ex][lo >> 8][d >> 8];
                carry = hi >> 8;
                d = (uint16_t)((lo & 0xFF) | ((hi & 0xFF) << 8));
            }
            if (d) {
                zero = false;
            }
            if (con0 & USED) {
                chip_write16(dpt, d);
                dpt += inc;
            }
        }
        if (con0 & USEA) {
            apt += mod[A];
        }
        if (con0 & USEB) {
            bpt += mod[B];
        }
        if (con0 & USEC) {
            cpt += mod[C];
        }
        if (con0 & USED) {
            dpt += mod[D];
        }
    }
    blit.pt[A] = apt;
    blit.pt[B] = bpt;
    blit.pt[C] = cpt;
    blit.pt[D] = dpt;
    blit.dat[A] = adat;
    blit.dat[B] = bdat;
    blit.dat[C] = cdat;
    blit.zero = zero;
    return (int)(w * h) * s_word_cck[(con0 >> 8) & 0xF];
}

/*
 * Line mode: a pixel a step along the major axis, a step along the minor
 * one too while the error term (BLTAPTL) isn't negative. BLTCON1's octant
 * bits: SUD (bit 4) the minor steps are vertical (the major axis is x),
 * SUL (bit 3) they go up or left, AUL (bit 2) the major ones do.
 */
static int blit_line(void)
{
    uint16_t con0 = blit.con0, con1 = blit.con1;
    uint8_t lf = con0 & 0xFF;
    int ash = con0 >> 12, bsh = con1 >> 12;
    bool sud = con1 & 0x10, sul = con1 & 0x08, aul = con1 & 0x04;
    bool sing = con1 & 0x02;
    bool sign = con1 & 0x40;
    int16_t err = (int16_t)(blit.pt[A] & 0xFFFF);
    uint32_t cpt = blit.pt[C], dpt = blit.pt[D];
    int32_t row = blit.mod[C];
    bool dotted = false, zero = true;

    for (uint32_t i = 0; i < blit.height; i++) {
        uint16_t a = (uint16_t)(blit.dat[A] >> ash);
        uint16_t b = (blit.dat[B] & (0x8000 >> bsh)) ? 0xFFFF : 0;
        uint16_t c = chip_read16(cpt);
        uint16_t d = minterm(lf, a, b, c);
        if (d) {
            zero = false;
        }
        if (!sing || !dotted) {
            chip_write16(dpt, d);
            dotted = true;
        }
        bsh = (bsh + 1) & 15;

        bool minor = !sign;
        err = (int16_t)(err + (sign ? blit.mod[B] : blit.mod[A]));
        sign = err < 0;
        int dx = 0, dy = 0;
        if (sud) {
            dx = aul ? -1 : 1;
            if (minor) {
                dy = sul ? -1 : 1;
            }
        } else {
            dy = aul ? -1 : 1;
            if (minor) {
                dx = sul ? -1 : 1;
            }
        }
        if (dx > 0 && ++ash == 16) {
            ash = 0;
            cpt += 2;
        }
        if (dx < 0 && --ash < 0) {
            ash = 15;
            cpt -= 2;
        }
        if (dy) {
            cpt += dy < 0 ? -row : row;
            dotted = false;
        }
        dpt = cpt;                  /* (the first pixel goes to BLTDPT, the rest follow C) */
    }
    blit.pt[A] = (blit.pt[A] & 0xFFFF0000) | (uint16_t)err;
    blit.pt[C] = blit.pt[D] = cpt;
    blit.con0 = (uint16_t)((con0 & 0x0FFF) | (ash << 12));
    blit.con1 = (uint16_t)((con1 & 0x0FBF) | (bsh << 12) | (sign ? 0x40 : 0));
    blit.zero = zero;
    return (int)blit.height * 4;
}

/* The slots of this line the blitter may have: its colour clocks less
 * refresh (4), disk (3), audio (one a channel) and the display's
 */
static int line_free(void)
{
    uint16_t dma = (ami.dmacon & DMA_EN) ? ami.dmacon : 0;
    int free = ami_line_cck(ami.line) - 4 - ((dma & DMA_DSK) ? 3 : 0) - __builtin_popcount(dma & DMA_AUD) -
               display_dma_slots();
    return free > 0 ? free : 0;
}

/* Its slots from `from` on, the rest of this line's free ones spread over
 * the line: done in it (the end), or the rest in the next lines
 */
static void plan_end(int from)
{
    int len = 2 * ami_line_cck(ami.line), line_end = ami.line_start + len;
    int span = line_end - from;
    int avail = span > 0 ? (int)((int64_t)line_free() * span / len) : 0;
    if (blit.left <= avail) {
        blit.end = from + (avail > 0 ? (int)((int64_t)blit.left * span / avail) : 0);
        blit.left = 0;
    } else {
        blit.left -= avail;
        blit.end = line_end + 2 * blit.left;    /* (at the earliest: the next lines say) */
    }
}

void blitter_line_start(void)
{
    if (blit.busy && blit.left > 0) {
        plan_end(ami.line_start);
    }
}

static void start(int at)
{
    blit.pending = false;
    int cck = (blit.con1 & LINE) ? blit_line() : blit_area();
    blit.busy = true;
    blit.left = cck;
    plan_end(at + 8);
    /* The 68EC020's slots it takes on average: those its channels use (a
     * line: C and D), all of them with BLTPRI ("nasty"), else it gets one
     * in four it waits for (the 68000 waits access by access: mem.c)
     */
    int used = (blit.con1 & LINE) ? 2 * (int)blit.height
                                  : (int)(blit.width * blit.height) * __builtin_popcount((blit.con0 >> 8) & 0xF);
    int steal = used * SLOT_WAIT;
    if (!(ami.dmacon & DMA_BLTPRI)) {
        steal = steal * 3 / 4;
    }
    blit.steal = ami.cpu_shift ? steal : 0;
    blit.steal_line = cck > 0 ? (int)((int64_t)steal * CYCLES_LINE / (cck * 2)) : steal;
    ami_cpu_wake();
}

int blitter_steal_line(void)
{
    if (blit.steal <= 0) {
        return 0;
    }
    int n = blit.steal < blit.steal_line ? blit.steal : blit.steal_line;
    n = n > 0 ? n : blit.steal;
    blit.steal -= n;
    return n;
}

static bool dma_on(void)
{
    return (ami.dmacon & (DMA_EN | DMA_BLT)) == (DMA_EN | DMA_BLT);
}

static void begin(int at)
{
    if (dma_on()) {
        start(at);
    } else {
        blit.pending = true;
    }
}

void blitter_dma_changed(int at)
{
    if (blit.pending && dma_on()) {
        start(at);
    }
}

void blitter_finish(void)
{
    blit.busy = false;
    ami_set_intreq(INT_BLIT);
}

bool blitter_busy(int at)
{
    return blit.pending || (blit.busy && at < blit.end);
}

static uint32_t set_hi(uint32_t pt, uint16_t v)
{
    return (pt & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
}

static uint32_t set_lo(uint32_t pt, uint16_t v)
{
    return (pt & 0xFFFF0000) | (v & 0xFFFE);
}

void blitter_write(uint32_t reg, uint16_t v, int at)
{
    switch (reg) {
    case 0x040: blit.con0 = v; break;
    case 0x042: blit.con1 = v; break;
    case 0x044: blit.afwm = v; break;
    case 0x046: blit.alwm = v; break;
    case 0x048: blit.pt[C] = set_hi(blit.pt[C], v); break;
    case 0x04A: blit.pt[C] = set_lo(blit.pt[C], v); break;
    case 0x04C: blit.pt[B] = set_hi(blit.pt[B], v); break;
    case 0x04E: blit.pt[B] = set_lo(blit.pt[B], v); break;
    case 0x050: blit.pt[A] = set_hi(blit.pt[A], v); break;
    case 0x052: blit.pt[A] = (blit.pt[A] & 0xFFFF0000) | v; break;   /* (line mode's error term: all 16 bits) */
    case 0x054: blit.pt[D] = set_hi(blit.pt[D], v); break;
    case 0x056: blit.pt[D] = set_lo(blit.pt[D], v); break;
    case 0x058:                     /* BLTSIZE: height (10 bits), width in words (6) */
        blit.height = (v >> 6) ? (v >> 6) : 1024;
        blit.width = (v & 0x3F) ? (v & 0x3F) : 64;
        begin(at);
        break;
    case 0x05A:                     /* BLTCON0L (ECS) */
        if (ami.ecs) {
            blit.con0 = (blit.con0 & 0xFF00) | (v & 0xFF);
        }
        break;
    case 0x05C:                     /* BLTSIZV (ECS) */
        if (ami.ecs) {
            blit.height = (v & 0x7FFF) ? (v & 0x7FFF) : 0x8000;
        }
        break;
    case 0x05E:                     /* BLTSIZH (ECS): starts it */
        if (ami.ecs) {
            blit.width = (v & 0x7FF) ? (v & 0x7FF) : 0x800;
            begin(at);
        }
        break;
    case 0x060: blit.mod[C] = (int16_t)(v & 0xFFFE); break;
    case 0x062: blit.mod[B] = (int16_t)(v & 0xFFFE); break;
    case 0x064: blit.mod[A] = (int16_t)(v & 0xFFFE); break;
    case 0x066: blit.mod[D] = (int16_t)(v & 0xFFFE); break;
    case 0x070: blit.dat[C] = v; break;
    case 0x072: blit.dat[B] = v; break;
    case 0x074: blit.dat[A] = v; break;
    default: break;
    }
}

void blitter_reset(void)
{
    static bool tables;
    if (!tables) {
        init_fill();
        tables = true;
    }
    blit = (blitter_t){ .afwm = 0xFFFF, .alwm = 0xFFFF, .end = NEVER };
}

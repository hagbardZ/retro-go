/*
 * The copper: MOVE, WAIT and SKIP from chip RAM, restarted from COP1LC at
 * each vertical blank.
 *
 * It runs as an event of the frame loop at its own times (a MOVE takes 4
 * colour clocks; a WAIT sleeps until the beam's position, found ahead of
 * time). Writes only the display reads (colours, bitplane pointers and
 * modulos, BPLCON1-3, sprites) are done in a run ahead of the 68000, up to
 * the line's end or its bitplane fetch, each logged with the copper's own
 * position; a write the 68000 could notice waits for it to get there.
 */
#include "amiga_internal.h"

copper_t cop;

bool copper_enabled(void)
{
    return (ami.dmacon & (DMA_EN | DMA_COP)) == (DMA_EN | DMA_COP);
}

void copper_reset(void)
{
    cop.lc[0] = cop.lc[1] = 0;
    cop.pc = 0;
    cop.state = COP_STOPPED;
    cop.at = NEVER;
    cop.danger = false;
}

void copper_vblank(void)
{
    cop.pc = cop.lc[0];
    cop.state = COP_FETCH;
    cop.at = 0;
}

static void jump(int n, int at, bool copper)
{
    cop.pc = cop.lc[n];
    if (copper) {
        return;                     /* copper_run() goes on from there */
    }
    cop.state = COP_FETCH;
    cop.at = at;
    ami_cpu_wake();
}

void copper_write(uint32_t reg, uint16_t v, int at, bool copper)
{
    switch (reg) {
    case 0x02E:                     /* COPCON */
        cop.danger = (v & 2) != 0;
        break;
    case 0x080:
        cop.lc[0] = (cop.lc[0] & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        break;
    case 0x082:
        cop.lc[0] = (cop.lc[0] & 0xFFFF0000) | (v & 0xFFFE);
        break;
    case 0x084:
        cop.lc[1] = (cop.lc[1] & 0xFFFF) | ((uint32_t)(v & 0x1F) << 16);
        break;
    case 0x086:
        cop.lc[1] = (cop.lc[1] & 0xFFFF0000) | (v & 0xFFFE);
        break;
    case 0x088:                     /* COPJMP1, COPJMP2 */
        jump(0, at, copper);
        break;
    case 0x08A:
        jump(1, at, copper);
        break;
    default:
        break;
    }
}

void copper_dma_changed(int at)
{
    if (copper_enabled() && cop.state != COP_STOPPED && cop.at < at) {
        cop.at = at;
        ami_cpu_wake();
    }
}

/* Registers only the display reads: written ahead of the 68000 */
static bool display_only(uint32_t reg)
{
    return (reg >= 0x0E0 && reg <= 0x0F6) ||       /* BPLxPT */
           (reg >= 0x102 && reg <= 0x10A) ||       /* BPLCON1-3, BPLxMOD */
           (reg >= 0x110 && reg <= 0x1BE);         /* BPLxDAT, sprites, colours */
}

/* The beam's position at time t compared with a WAIT's (masked) */
static bool reached(uint16_t ir1, uint16_t ir2, int line, int h)
{
    int vm = 0x80 | ((ir2 >> 8) & 0x7F), hm = ir2 & 0xFE;
    int v = line & 0xFF & vm, tv = (ir1 >> 8) & vm;
    if (v != tv) {
        return v > tv;
    }
    return (h & hm) >= (ir1 & 0xFE & hm);
}

/* When a WAIT ends: the first even colour clock from t on at which the
 * beam has reached its position, or NEVER this frame
 */
static int wait_until(uint16_t ir1, uint16_t ir2, int t)
{
    int line, h;
    ami_beam_at((t + 1) / 2, &line, &h);
    int vm = 0x80 | ((ir2 >> 8) & 0x7F), hm = ir2 & 0xFE;
    int tv = (ir1 >> 8) & vm, th = ir1 & 0xFE & hm;
    h = (h + 1) & ~1;
    for (; line < ami.lines; line++, h = 0) {
        int v = line & 0xFF & vm;
        int start = ami_line_start(line), cck = ami_line_cck(line);
        if (v > tv) {
            return start + h * 2;
        }
        if (v < tv) {
            continue;
        }
        if (hm == 0xFE) {
            if (h < th) {
                h = th;
            }
            if (h < cck) {
                return start + h * 2;
            }
            continue;
        }
        for (; h < cck; h += 2) {
            if ((h & hm) >= th) {
                return start + h * 2;
            }
        }
    }
    return NEVER;
}

static bool legal(uint32_t reg)
{
    if (ami.ecs) {
        return cop.danger || reg >= 0x040;
    }
    return reg >= (cop.danger ? 0x040u : 0x080u);
}

/* Its next event: a WAIT's end, or the next instruction's effect */
int copper_next(void)
{
    if (cop.state == COP_STOPPED || !copper_enabled()) {
        return NEVER;
    }
    return cop.state == COP_WAIT ? cop.at : cop.at + 4;
}

/* An instruction's two even slots: the 68000's lost slot by slot (mem.c),
 * the 68EC020's on average
 */
static void take_slots(int at)
{
    if (ami.cpu_shift) {
        ami.stall += 2 * SLOT_WAIT;
    } else {
        bus_take(at);
        bus_take(at + 4);
    }
}

void copper_run(int now, int limit)
{
    while (copper_next() < limit) {
        if (cop.state == COP_WAIT) {
            if (cop.at > now) {
                return;
            }
            /* blitter-finished disable clear: also wait for the blitter */
            if (!(cop.ir2 & 0x8000) && blitter_busy(cop.at)) {
                cop.at = blit.end;
                continue;
            }
            /* a cycle to wake up before the next fetch: the instruction
             * after WAIT $FFDF compares in line 256, not still in 255 */
            cop.at += 4;
            cop.state = COP_FETCH;
            continue;
        }
        uint16_t ir1 = chip_read16(cop.pc), ir2 = chip_read16(cop.pc + 2);
        if (!(ir1 & 1)) {
            /* MOVE: the write lands as its second word arrives */
            uint32_t reg = ir1 & 0x1FE;
            int when = cop.at + 4;
            if (when > now && !display_only(reg)) {
                return;
            }
            if (!legal(reg)) {
                cop.state = COP_STOPPED;
                cop.at = NEVER;
                return;
            }
            take_slots(cop.at);
            cop.pc += 4;
            cop.at += 8;
            custom_write(reg, ir2, when, true);
            continue;
        }
        take_slots(cop.at);
        cop.pc += 4;
        int t = cop.at + 8;
        if (ir2 & 1) {
            /* SKIP the next instruction once the beam is there */
            int line, h;
            ami_beam_at(t / 2, &line, &h);
            if (reached(ir1, ir2, line, h)) {
                cop.pc += 4;
            }
            cop.at = t;
            continue;
        }
        cop.ir1 = ir1;
        cop.ir2 = ir2;
        cop.at = wait_until(ir1, ir2, t);
        cop.state = cop.at == NEVER ? COP_STOPPED : COP_WAIT;
    }
}

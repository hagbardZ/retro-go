/*
 * The 68000's memory map and the Musashi bus callbacks.
 *
 *   $000000-$1FFFFF  chip RAM (mirrored by its size); the ROM for reads
 *                    while OVL is set (after reset), writes still to RAM
 *   $200000-$9FFFFF  fast RAM once autoconfigured (autoconf.c)
 *   $A00000-$BFFFFF  the CIAs: CIA-A while A12 is low (odd bytes), CIA-B
 *                    while A13 is low (even bytes), a register every 256
 *   $C00000-$D7FFFF  slow RAM (the A500's trapdoor); above it, the custom
 *                    chips' mirror Kickstart sizes it by (1.2 takes any
 *                    space that isn't the mirror for RAM)
 *   $DA0000-$DAFFFF  Gayle's IDE and its registers, $DE1000 its ID (gayle.c;
 *                    the A600's, the others' with a hard disk in)
 *   $DC0000-$DCFFFF  the battery-backed clock (rtc.c)
 *   $DFF000-$DFFFFF  the custom chips (registers mirrored every $200)
 *   $E80000-$E8FFFF  autoconfig: the next board to configure
 *   $F80000-$FFFFFF  Kickstart (256 KB ones mirrored)
 *
 * No bus errors: an Amiga 500's empty space just reads nothing (0 here).
 *
 * The A1200's 68EC020 waits for the chip bus: each access to chip RAM (32
 * bits at once), the custom chips or a CIA costs it WAIT_* of its clocks
 * (s_wait by 64 KB bank, the others in the slow path); the ROM and fast
 * RAM don't, nor instructions from its cache (below). The 68000's waits
 * are counted otherwise (the slots, below).
 */
#include <string.h>
#include "m68k.h"
#include "amiga_internal.h"

static const uint8_t *s_rmap[256];
static uint8_t *s_wmap[256];
static uint8_t s_wait[256];           /* the 68EC020's waits, by bank (0: none) */

#define WAIT_CHIP       8               /* two chip bus cycles (SysInfo: 1.04 times an A1200) */
#define WAIT_CUSTOM     8
#define WAIT_CIA        20

/* ---- The 68000 on the chip bus, slot by slot ----
 *
 * The 68000 has the even colour clocks of a line the DMA leaves it
 * (refresh, disk, audio, sprites and up to 4 lores planes use odd ones):
 * lores planes 5-6, hires 3-4, super-hires 2 and the copper take even ones
 * (display.c and copper.c mark them as they happen), the blitter what it
 * can. Each access of the 68000's to chip RAM, slow RAM or the chips is at
 * its instruction's start plus 4 clocks a bus cycle before it, and waits
 * for the next even slot free; the wait comes off Musashi's remaining
 * cycles, as the 68EC020's waits do (it keeps its averages: s_wait).
 */
#define EVEN_SLOTS      116                 /* (a line's 114) */
#define LINE_CCK        (2 * EVEN_SLOTS)
static bool s_taken[EVEN_SLOTS + 1];        /* this line's even slots the DMA has (h / 2) */
static uint16_t s_wait_at[LINE_CCK];        /* the clocks an access at colour clock h waits */
static bool s_any_taken, s_stale;           /* (the table worked out when it's needed) */
static uint8_t s_contended[256];            /* the 68000's banks that wait: */
#define ON_CHIP_BUS     1                   /* on the chip bus (DMA's slots) */
#define ON_E_CLOCK      2                   /* the CIAs (the E clock) */
static int s_bus_cycle;                     /* this instruction's bus cycles so far */
static int s_after = -1;                    /* Musashi's remaining cycles after the last access */
static int s_last_at, s_last_wait;          /* the last access: when it began, its wait */
#define BLIT_WAIT       6                   /* a blit leaves the 68000 one slot in four (BLTPRI off) */

void bus_line_start(void)
{
    if (s_any_taken) {
        memset(s_taken, 0, sizeof(s_taken));
        s_any_taken = false;
    }
}

/* An even slot taken at `at` (68000 clocks into the frame) */
void bus_take(int at)
{
    int e = (at - ami.line_start) >> 2;     /* (h / 2) */
    if (e >= 0 && e < EVEN_SLOTS) {
        s_taken[e] = true;
        s_any_taken = s_stale = true;
    }
}

/* Each colour clock's wait: past the taken even slots from it on, 4 clocks
 * each. (Not the 2 clocks a 68000 on an odd colour clock would wait for the
 * even one, which a PAL line's 227 bring about from line to line: with them
 * SysInfo and xSysInfo both gave 1.03, but Hyperion's and Lethal Xcess's
 * loaders failed, their CPU too slow; Musashi knows an instruction's bus
 * cycles, not where in it they fall, and the cycle table's rounding to 4
 * keeps the phase.)
 */
static void work_out_waits(void)
{
    uint16_t next = 0;                      /* (the wait at the even slot after) */
    for (int e = EVEN_SLOTS - 1; e >= 0; e--) {
        s_wait_at[2 * e + 1] = next;
        next = s_taken[e] ? (uint16_t)(next + 4) : 0;
        s_wait_at[2 * e] = next;
    }
    s_stale = false;
}

/* The clocks an access at `at` waits for its slot */
static inline int slot_wait(int at)
{
    int h = (at - ami.line_start) >> 1;
    if (h < 0) {
        return 0;
    }
    if (blit.busy && at < blit.end) {
        return (ami.dmacon & DMA_BLTPRI) ? blit.end - at : BLIT_WAIT;
    }
    if (!s_any_taken || h >= LINE_CCK) {
        return 0;
    }
    if (s_stale) {
        work_out_waits();
    }
    return s_wait_at[h];
}

/* A bus cycle of the 68000's: its wait on the chip bus */
static inline void bus_cycle(uint32_t a)
{
    extern int m68ki_initial_cycles, m68ki_remaining_cycles;
    if (m68ki_remaining_cycles != s_after) {
        s_bus_cycle = 0;                    /* (Musashi took an instruction's cycles: the next one) */
    }
    s_last_wait = 0;
    if (s_contended[(a >> 16) & 0xFF] && ami.cpu_running) {
        /* (ami_now() for the 68000: no shift, no remainder) */
        int at = ami.cycle + m68ki_initial_cycles - m68ki_remaining_cycles + 4 * s_bus_cycle;
        s_last_at = at;
        /* (an E clock access begins a bus cycle sooner: the 68000 fetched
         * the opcode during the instruction before; the slots keep their
         * tuning) */
        s_last_wait = s_contended[(a >> 16) & 0xFF] == ON_E_CLOCK ? ami_e_wait(at - 4) : slot_wait(at);
        m68ki_remaining_cycles -= s_last_wait;
    }
    s_bus_cycle++;
    s_after = m68ki_remaining_cycles;
}

/* The 68000 decides on an interrupt as its instruction's last bus cycle
 * begins (WinUAE: 2 clocks into it): one raised while that cycle waits for
 * the DMA comes after the next instruction. (Musashi doesn't place its
 * prefetch at that bus cycle: the instruction's last access stands for it.) Lionheart polls INTREQR for
 * VERTB from slow RAM with exec's VERTB interrupt on; the copper's list at
 * the top of the frame holds the loop's BEQ, and it never saw the bit.
 */
bool bus_waited_over(int t)
{
    return s_last_wait > 0 && s_last_at < t && s_last_at + s_last_wait >= t && s_last_at + s_last_wait <= ami.cycle;
}

static inline uint16_t rd16(const uint8_t *p, uint32_t off)
{
    return *(const uint16_t *)(p + off);
}

static inline void wr16(uint8_t *p, uint32_t off, uint16_t v)
{
    *(uint16_t *)(p + off) = v;
}

void mem_update_map(void)
{
    memset(s_rmap, 0, sizeof(s_rmap));
    memset(s_wmap, 0, sizeof(s_wmap));
    memset(s_wait, 0, sizeof(s_wait));
    memset(s_contended, 0, sizeof(s_contended));
    for (uint32_t b = 0x00; b < 0x20; b++) {
        uint8_t *p = ami.chip + ((b << 16) & (ami.chip_size - 1));
        s_rmap[b] = p;
        s_wmap[b] = p;
        s_wait[b] = ami.cpu_shift ? WAIT_CHIP : 0;
        s_contended[b] = ami.cpu_shift ? 0 : ON_CHIP_BUS;
    }
    /* (the 68000: slow RAM, the chips and their mirrors on the chip bus too;
     * the CIAs on its own side, the E clock's) */
    for (uint32_t b = 0xA0; b < 0xE0 && !ami.cpu_shift; b++) {
        s_contended[b] = b < 0xC0 ? ON_E_CLOCK : (b < 0xD8 || b == 0xDF) ? ON_CHIP_BUS : 0;
    }
    if (ami.ovl) {
        for (uint32_t b = 0x00; b < 0x08; b++) {
            s_rmap[b] = ami.rom + ((b << 16) & (ami.rom_size - 1));
        }
    }
    for (uint32_t b = 0; b < (ami.slow_size >> 16); b++) {
        s_rmap[0xC0 + b] = s_wmap[0xC0 + b] = ami.slow + (b << 16);
    }
    uint32_t board, board_size;
    uint8_t *mem = hdboard_memory(&board, &board_size);
    for (uint32_t b = 0; mem && board && b < (board_size >> 16); b++) {
        s_rmap[(board >> 16) + b] = s_wmap[(board >> 16) + b] = mem + (b << 16);
    }
    for (uint32_t b = 0; ami.fast_base && b < (ami.fast_size >> 16); b++) {
        s_rmap[(ami.fast_base >> 16) + b] = s_wmap[(ami.fast_base >> 16) + b] = ami.fast + (b << 16);
    }
    for (uint32_t b = 0; ami.ext && b < (EXT_ROM_SIZE >> 16); b++) {
        s_rmap[0xE0 + b] = ami.ext + (b << 16);
    }
    for (uint32_t b = 0xF8; b < 0x100; b++) {
        s_rmap[b] = ami.rom + ((b << 16) & (ami.rom_size - 1));
    }
}

/* ---- The CIAs: CIA-A on the data bus's low byte, CIA-B on the high one.
 * A byte write reaches both halves of the bus (the 68000 puts it on both),
 * so it goes to whichever chip is selected.
 */

static uint8_t cia_read8(uint32_t a)
{
    int reg = (a >> 8) & 0xF;
    if (a & 1) {
        return (a & 0x1000) ? 0xFF : cia_read(&cia[0], reg);
    }
    return (a & 0x2000) ? 0xFF : cia_read(&cia[1], reg);
}

static void cia_write8(uint32_t a, uint8_t v)
{
    int reg = (a >> 8) & 0xF;
    if (!(a & 0x1000)) {
        cia_write(&cia[0], reg, v);
    }
    if (!(a & 0x2000)) {
        cia_write(&cia[1], reg, v);
    }
}

/* (slow RAM's banks never get here) */
static inline bool is_custom(uint32_t a)
{
    return (a >= 0xDFF000 && a < 0xE00000) || (a >= 0xC00000 && a < 0xD80000);
}

static inline bool is_cia(uint32_t a)
{
    return a >= 0xA00000 && a < 0xC00000;
}

static inline bool is_rtc(uint32_t a)
{
    return a >= 0xDC0000 && a < 0xDD0000;
}

static inline bool is_autoconf(uint32_t a)
{
    return a >= 0xE80000 && a < 0xE90000;
}

static inline bool is_gayle(uint32_t a)
{
    return ((a >= 0xDA0000 && a < 0xDB0000) || (a >= 0xDE1000 && a < 0xDE2000)) && gayle_present();
}

/* The 68EC020's wait in the slow path: the custom chips' or a CIA's */
static inline void io_wait(uint32_t a)
{
    if (ami.cpu_shift) {
        ami_cpu_wait(is_cia(a) ? WAIT_CIA : is_custom(a) ? WAIT_CUSTOM : 0);
    }
}

static uint16_t slow_read16(uint32_t a)
{
    io_wait(a);
    if (is_custom(a)) {
        return custom_read(a & 0x1FE);
    }
    if (is_rtc(a)) {
        /* (the clock drives 4 data bits; the others float: xSysInfo takes
         * zeros there for an A1000's custom chips mirrored at $DCF004)
         */
        return 0xFFF0 | rtc_read((a >> 2) & 15);
    }
    if (is_cia(a)) {
        int reg = (a >> 8) & 0xF;
        uint16_t hi = (a & 0x2000) ? 0xFF : cia_read(&cia[1], reg);
        uint16_t lo = (a & 0x1000) ? 0xFF : cia_read(&cia[0], reg);
        return (hi << 8) | lo;
    }
    if (is_autoconf(a)) {
        return autoconf_read16(a);
    }
    if (is_gayle(a)) {
        return gayle_read16(a);
    }
    return 0;
}

static uint8_t slow_read8(uint32_t a)
{
    io_wait(a);
    if (is_custom(a)) {
        uint16_t w = custom_read(a & 0x1FE);
        return (a & 1) ? (w & 0xFF) : (w >> 8);
    }
    if (is_cia(a)) {
        return cia_read8(a);
    }
    if (is_rtc(a)) {
        return (a & 1) ? 0xF0 | rtc_read((a >> 2) & 15) : 0xFF;
    }
    if (is_autoconf(a)) {
        return autoconf_read8(a);
    }
    if (is_gayle(a)) {
        return gayle_read8(a);
    }
    return 0;
}

static void slow_write16(uint32_t a, uint16_t v)
{
    io_wait(a);
    if (is_custom(a)) {
        custom_write(a & 0x1FE, v, ami_now(), false);
    } else if (is_cia(a)) {
        int reg = (a >> 8) & 0xF;
        if (!(a & 0x2000)) {
            cia_write(&cia[1], reg, v >> 8);
        }
        if (!(a & 0x1000)) {
            cia_write(&cia[0], reg, v & 0xFF);
        }
    } else if (is_rtc(a)) {
        rtc_write((a >> 2) & 15, v & 0xFF);
    } else if (is_autoconf(a)) {
        autoconf_write16(a, v);
    } else if (is_gayle(a)) {
        gayle_write16(a, v);
    }
}

static void slow_write8(uint32_t a, uint8_t v)
{
    io_wait(a);
    if (is_custom(a)) {
        /* the byte on both halves of the bus: a word to the chips */
        custom_write(a & 0x1FE, (v << 8) | v, ami_now(), false);
    } else if (is_cia(a)) {
        cia_write8(a, v);
    } else if (is_rtc(a)) {
        rtc_write((a >> 2) & 15, v);
    } else if (is_autoconf(a)) {
        autoconf_write8(a, v);
    } else if (is_gayle(a)) {
        gayle_write8(a, v);
    }
}

/* ---- Musashi's callbacks ---- */

/* A read on the 68000's chip bus drives the word too, including opcode
 * fetches from chip/slow RAM. Write-only custom registers then read this
 * word, not just the last custom-register write. Otherwise a byte RAM
 * probe can mistake their mirrors for RAM (Steel Empire's stack at D80000).
 */
static inline uint16_t cpu_bus_word(uint32_t a, uint16_t v)
{
    if (s_contended[a >> 16] == ON_CHIP_BUS) {
        ami.bus = v;
    }
    return v;
}

unsigned int AMI_HOT m68k_read_memory_8(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (!ami.cpu_shift) {
        bus_cycle(a);
    } else if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
    if (p) {
        cpu_bus_word(a, rd16(p, a & 0xFFFE));
        return p[(a & 0xFFFF) ^ 1];
    }
    return slow_read8(a);
}

unsigned int AMI_HOT m68k_read_memory_16(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (!ami.cpu_shift) {
        bus_cycle(a);
    } else if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
    if (p) {
        return cpu_bus_word(a, rd16(p, a & 0xFFFF));
    }
    return slow_read16(a);
}

unsigned int AMI_HOT m68k_read_memory_32(unsigned int a)
{
    const uint8_t *p = s_rmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        if (!ami.cpu_shift) {
            bus_cycle(a);
            bus_cycle(a);
        } else if (s_wait[a >> 16]) {
            ami_cpu_wait(s_wait[a >> 16]);
        }
        return ((uint32_t)rd16(p, a & 0xFFFF) << 16) | cpu_bus_word(a, rd16(p, (a & 0xFFFF) + 2));
    }
    uint32_t hi = m68k_read_memory_16(a);
    return (hi << 16) | m68k_read_memory_16((a + 2) & 0xFFFFFF);
}

/* ---- The 68EC020's instruction cache ----
 *
 * 64 longwords, direct-mapped by address bits 7-2, tagged with the rest,
 * filled while CACR's E bit is set (F freezes it; a write with C set
 * empties it). An instruction word from a longword in it costs no bus
 * time; otherwise the longword is read (the chip bus's wait), once for
 * both its words (the 68020 fetches longwords). Only the time: the words
 * themselves always come from memory (no stale ones after code changes).
 */
#define IC_LINES        64
static uint32_t s_ic_tag[IC_LINES];         /* its longword's address / 4 + 1 (0: empty) */
static uint32_t s_ic_prev = 1;              /* the last instruction word fetched (odd: none) */
static uint32_t s_cacr;

void mem_cacr(uint32_t cacr)
{
    if (cacr != s_cacr && (cacr & 8)) {
        memset(s_ic_tag, 0, sizeof(s_ic_tag));
    }
    s_cacr = cacr;
}

static inline void ic_word(uint32_t a)
{
    uint32_t prev = s_ic_prev;
    s_ic_prev = a;
    if (a == prev + 2 && (a & 2)) {
        return;                     /* the longword's second word: read with the first */
    }
    if (s_cacr & 1) {
        uint32_t lw = (a >> 2) + 1, *t = &s_ic_tag[(a >> 2) & (IC_LINES - 1)];
        if (*t == lw) {
            return;
        }
        if (!(s_cacr & 2)) {
            *t = lw;
        }
    }
    if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
}

/* An instruction word (its wait already taken) */
static inline uint16_t program16(uint32_t a)
{
    const uint8_t *p = s_rmap[a >> 16];
    return p ? rd16(p, a & 0xFFFF) : slow_read16(a);
}

unsigned int AMI_HOT m68k_read_immediate_16(unsigned int a)
{
    if (!ami.cpu_shift) {
        return m68k_read_memory_16(a);
    }
    ic_word(a);
    return program16(a);
}

/* (a long across a 64 KB bank: each word from its own; Darkage's Under
 * Pressure crashed when they both came from the slow path's zeros)
 */
unsigned int AMI_HOT m68k_read_immediate_32(unsigned int a)
{
    if (!ami.cpu_shift) {
        return m68k_read_memory_32(a);
    }
    ic_word(a);
    ic_word((a + 2) & 0xFFFFFF);
    const uint8_t *p = s_rmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        return ((uint32_t)rd16(p, a & 0xFFFF) << 16) | rd16(p, (a & 0xFFFF) + 2);
    }
    return ((uint32_t)program16(a) << 16) | program16((a + 2) & 0xFFFFFF);
}

/* The instruction at pc, then a conditional branch back to it: a loop of
 * the two (custom.c's polls)
 */
bool mem_poll_loop(uint32_t pc)
{
    char text[100];
    uint32_t b = pc + m68k_disassemble(text, pc, ami.cpu_shift ? M68K_CPU_TYPE_68EC020 : M68K_CPU_TYPE_68000);
    uint16_t op = (uint16_t)m68k_read_disassembler_16(b);
    if ((op & 0xF000) != 0x6000 || (op & 0x0F00) < 0x0200) {
        return false;               /* not a Bcc (BRA and BSR aren't) */
    }
    int8_t d8 = (int8_t)(op & 0xFF);
    if (d8 == -1) {
        return false;               /* (68020: a 32-bit displacement) */
    }
    uint32_t target = d8 ? b + 2 + d8 : b + 2 + (int16_t)m68k_read_disassembler_16(b + 2);
    return (target & 0xFFFFFF) == pc;
}

/* PC-relative operands: data (the cache holds instructions only). Musashi
 * hands these over without its 24-bit mask (its data path masks inside):
 * an index going round the top read past s_rmap (Desert Dream crashed)
 */
unsigned int m68k_read_pcrelative_8(unsigned int a)
{
    return m68k_read_memory_8(a & 0xFFFFFF);
}

unsigned int m68k_read_pcrelative_16(unsigned int a)
{
    return m68k_read_memory_16(a & 0xFFFFFF);
}

unsigned int m68k_read_pcrelative_32(unsigned int a)
{
    return m68k_read_memory_32(a & 0xFFFFFF);
}

void AMI_HOT m68k_write_memory_8(unsigned int a, unsigned int v)
{
    uint8_t *p = s_wmap[a >> 16];
    if (!ami.cpu_shift) {
        bus_cycle(a);
    } else if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
    if (p) {
        cpu_bus_word(a, (uint8_t)v * 0x0101u);
        p[(a & 0xFFFF) ^ 1] = v;
        return;
    }
    slow_write8(a, v);
}

void AMI_HOT m68k_write_memory_16(unsigned int a, unsigned int v)
{
    uint8_t *p = s_wmap[a >> 16];
    if (!ami.cpu_shift) {
        bus_cycle(a);
    } else if (s_wait[a >> 16]) {
        ami_cpu_wait(s_wait[a >> 16]);
    }
    if (p) {
        cpu_bus_word(a, v);
        wr16(p, a & 0xFFFF, v);
        return;
    }
    slow_write16(a, v);
}

void AMI_HOT m68k_write_memory_32(unsigned int a, unsigned int v)
{
    uint8_t *p = s_wmap[a >> 16];
    if (p && (a & 0xFFFF) <= 0xFFFC) {
        if (!ami.cpu_shift) {
            bus_cycle(a);
            bus_cycle(a);
        } else if (s_wait[a >> 16]) {
            ami_cpu_wait(s_wait[a >> 16]);
        }
        cpu_bus_word(a, v);
        wr16(p, a & 0xFFFF, v >> 16);
        wr16(p, (a & 0xFFFF) + 2, v & 0xFFFF);
        return;
    }
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16((a + 2) & 0xFFFFFF, v & 0xFFFF);
}

/* ---- Bytes to and from the 68000's memory (byte n of a word at n ^ 1) ---- */

void mem_copy_to(uint32_t a, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++, a++) {
        a &= 0xFFFFFF;
        uint8_t *p = s_wmap[a >> 16];
        if (p) {
            p[(a & 0xFFFF) ^ 1] = src[i];
        } else {
            m68k_write_memory_8(a, src[i]);
        }
    }
}

void mem_copy_from(uint8_t *dst, uint32_t a, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++, a++) {
        a &= 0xFFFFFF;
        const uint8_t *p = s_rmap[a >> 16];
        dst[i] = p ? p[(a & 0xFFFF) ^ 1] : (uint8_t)m68k_read_memory_8(a);
    }
}

/* ---- Without side effects: the debugger, the disassembler ---- */

uint16_t amiga_peek16(uint32_t a)
{
    a &= 0xFFFFFE;
    const uint8_t *p = s_rmap[a >> 16];
    return p ? rd16(p, a & 0xFFFF) : 0;
}

uint8_t amiga_peek8(uint32_t a)
{
    uint16_t w = amiga_peek16(a);
    return (a & 1) ? (w & 0xFF) : (w >> 8);
}

/* n bytes of RAM at a (chip, slow or fast; not ROM or I/O) */
static bool ram_range(uint32_t a, uint32_t n)
{
    if ((a & 1) || n > 0x1000000 - a) {
        return false;
    }
    return a + n <= ami.chip_size || (ami.slow_size && a >= 0xC00000 && a + n <= 0xC00000 + ami.slow_size) ||
           (ami.fast_base && a >= ami.fast_base && a + n <= ami.fast_base + ami.fast_size);
}

static uint32_t peek32(uint32_t a)
{
    return (uint32_t)amiga_peek16(a) << 16 | amiga_peek16(a + 2);
}

/* exec's MemList (ExecBase + $142): MemHeaders (a Node, then mh_First,
 * mh_Lower, mh_Upper, mh_Free at 16, 20, 24, 28), each with its MemChunks
 * (mc_Next, mc_Bytes); the kind by where the memory is
 */
bool amiga_mem_free(amiga_mem_free_t *chip, amiga_mem_free_t *slow, amiga_mem_free_t *fast)
{
    amiga_mem_free_t *kinds[3] = { chip, slow, fast };
    bool known[3] = { true, true, true };
    for (int k = 0; k < 3; k++) {
        memset(kinds[k], 0, sizeof(*kinds[k]));
    }
    uint32_t exec = peek32(4);
    if (!ram_range(exec, 0x150) || peek32(exec + 38) != ~exec) {
        return false;
    }
    bool any = false;
    uint32_t mh = peek32(exec + 0x142);
    for (int n = 0; n < 16 && ram_range(mh, 32) && peek32(mh) != 0; n++, mh = peek32(mh)) {
        uint32_t lower = peek32(mh + 20), upper = peek32(mh + 24);
        int k = lower < 0x200000 ? 0 : lower >= 0xC00000 && lower < 0xD80000 ? 1 : 2;
        amiga_mem_free_t *m = kinds[k];
        m->found = any = true;
        m->free += peek32(mh + 28);
        uint32_t mc = peek32(mh + 16);
        for (int c = 0; mc != 0 && c < 4096; c++, mc = peek32(mc)) {
            if (!ram_range(mc, 8) || mc < lower || mc >= upper) {
                break;
            }
            uint32_t bytes = peek32(mc + 4);
            m->largest = bytes > m->largest ? bytes : m->largest;
        }
        if (mc != 0) {
            known[k] = false;               /* (a list being changed) */
        }
    }
    for (int k = 0; k < 3; k++) {
        if (!known[k]) {
            kinds[k]->largest = 0;
        }
    }
    return any;
}

void amiga_poke8(uint32_t a, uint8_t v)
{
    a &= 0xFFFFFF;
    uint8_t *p = s_wmap[a >> 16];
    if (p) {
        p[(a & 0xFFFF) ^ 1] = v;
    }
}

unsigned int m68k_read_disassembler_16(unsigned int a)
{
    return amiga_peek16(a);
}

unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return ((uint32_t)amiga_peek16(a) << 16) | amiga_peek16(a + 2);
}

/*
 * The Amiga: reset, the frame loop and Paula's interrupt logic.
 *
 * A frame is a line at a time; within a line the 68000 runs up to the next
 * event (the bitplane fetch, the copper, the blitter's end, a CIA timer's
 * underflow) and the event is handled, until the line's end, where Denise
 * draws the line.
 */
#include <stdio.h>
#include <string.h>
#include "m68k.h"
#include "amiga_internal.h"

amiga_t ami;

static const char *s_cpu_name = "68000";

/* ---- Interrupts ---- */

void ami_update_irq(void)
{
    uint16_t active = ami.intena & ami.intreq & 0x3FFF;
    int level = 0;
    if ((ami.intena & INT_INTEN) && active) {
        level = (active & 0x2000) ? 6 :
                (active & 0x1800) ? 5 :
                (active & 0x0780) ? 4 :
                (active & 0x0070) ? 3 :
                (active & 0x0008) ? 2 : 1;
    }
    if (level != ami.ipl) {
        bool raised = level > ami.ipl;
        ami.ipl = level;
        m68k_set_irq(level);
        /* Musashi only samples the interrupt lines as a slice starts */
        if (raised) {
            ami_cpu_wake();
        }
    }
}

void ami_set_intreq(uint16_t bits)
{
    ami.intreq |= bits;
    ami_update_irq();
}

void ami_cpu_wake(void)
{
    if (ami.cpu_running) {
        m68k_end_timeslice();
    }
}

/* ---- Musashi's hooks ---- */

/* ILLEGAL + a function word in the hard disk board's ROM: its native code */
int m68k_host_illegal(int opcode)
{
    return opcode == 0x4AFC && hdboard_native();
}

void (*amiga_exception_hook)(unsigned int vector);

void m68k_host_exception(unsigned int vector)
{
    if (amiga_exception_hook) {
        amiga_exception_hook(vector);
    }
}

void m68k_host_halted(void)
{
    if (ami.halted) {
        return;
    }
    ami.halted = true;
    AMI_LOGW("CPU halted (double bus fault): PC $%06lx, SR $%04x, SSP $%06lx",
             (unsigned long)(m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF),
             m68k_get_reg(NULL, M68K_REG_SR), (unsigned long)m68k_get_reg(NULL, M68K_REG_ISP));
}

static int int_ack(int level)
{
    (void)level;
    return M68K_INT_ACK_AUTOVECTOR;
}

/* RESET: the CIAs (the ROM overlay comes back), the custom chips */
static void reset_instruction(void)
{
    cia_reset();
    ami.ovl = true;
    hdboard_reset();
    autoconf_reset();               /* (the boards too: Kickstart configures them again) */
    gayle_reset();
    mem_update_map();
    custom_reset();
    copper_reset();
    floppy_reset();
}

/* ---- Kickstart ---- */

bool amiga_kickstart_version(const uint8_t *k, uint32_t size, int *major, int *minor)
{
    if ((size != 256 * 1024 && size != 512 * 1024) || k[0] != 0x11 || (k[1] != 0x11 && k[1] != 0x14)) {
        return false;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t w = ((uint32_t)k[i] << 24) | (k[i + 1] << 16) | (k[i + 2] << 8) | k[i + 3];
        uint32_t t = sum + w;
        sum = t + (t < sum);
    }
    if (sum != 0xFFFFFFFF) {
        return false;
    }
    *major = (k[12] << 8) | k[13];
    *minor = (k[14] << 8) | k[15];
    return true;
}

/* ---- Setup and reset ---- */

bool amiga_init(const amiga_config_t *config)
{
    int major, minor;
    if (!amiga_kickstart_version(config->kickstart, config->kickstart_size, &major, &minor)) {
        AMI_LOGW("not a Kickstart image (%lu bytes)", (unsigned long)config->kickstart_size);
        return false;
    }
    memset(&ami, 0, sizeof(ami));
    ami.model = config->model;
    ami.ecs = config->ecs || config->model == AMIGA_A1200;
    ami.aga = config->model == AMIGA_A1200;
    ami.cpu_shift = config->model == AMIGA_A1200 ? 1 : 0;
    ami.ntsc = config->ntsc;
    ami.chip = config->chip;
    ami.chip_size = config->chip_size;
    ami.chip_mask = config->chip_size - 1;
    ami.slow = config->slow;
    ami.slow_size = config->slow ? config->slow_size : 0;
    ami.fast = config->fast;
    ami.fast_size = config->fast ? config->fast_size : 0;
    ami.rom = config->rom;
    ami.rom_size = config->kickstart_size;
    ami.serial = config->serial;
    ami.kick_major = major;
    ami.kick_minor = minor;
    rtc_init(config->rtc_offset, config->rtc_changed, major < 36);

    /* the working copies, each word in host order */
    for (uint32_t i = 0; i < ami.rom_size; i += 2) {
        *(uint16_t *)(ami.rom + i) = (uint16_t)((config->kickstart[i] << 8) | config->kickstart[i + 1]);
    }
    ami.ext = config->extension && config->ext_rom ? config->ext_rom : NULL;
    for (uint32_t i = 0; ami.ext && i < EXT_ROM_SIZE; i += 2) {
        *(uint16_t *)(ami.ext + i) = (uint16_t)((config->extension[i] << 8) | config->extension[i + 1]);
    }

    display_init(config->picture);

    m68k_init();
    /* (the type before the tables' copies: they take its cycle counts) */
    m68k_set_cpu_type(ami.cpu_shift ? M68K_CPU_TYPE_68EC020 : M68K_CPU_TYPE_68000);
    s_cpu_name = ami.cpu_shift ? "68EC020" : "68000";
    /* The 68000's memory cycles fall on the chip bus's slots (2 colour
     * clocks): code running from chip RAM takes whole multiples of 4 clocks
     * an instruction (xSysInfo measured 10.3 MHz on Musashi's bare counts)
     */
    static void *tables;
    if (!tables) {
        tables = config->opcode_tables ? config->opcode_tables : ami_malloc(AMIGA_OPCODE_TABLES_SIZE);
    }
    if (tables) {
        m68k_use_table_copies(tables);
        uint8_t *cycles = (uint8_t *)tables + 0x10000 * sizeof(void *);
        for (int op = 0; op < 0x10000 && !ami.cpu_shift; op++) {
            cycles[op] = (uint8_t)((cycles[op] + 3) & ~3);
        }
    }
    m68k_set_int_ack_callback(int_ack);
    m68k_set_reset_instr_callback(reset_instruction);

    AMI_LOGI("Kickstart %d.%d, %s, %lu KB chip RAM%s%s, %s %s", major, minor, s_cpu_name,
             (unsigned long)(ami.chip_size / 1024), ami.slow_size ? ", 512 KB slow RAM" : "",
             ami.fast_size ? ", fast RAM" : "", ami.aga ? "AGA" : ami.ecs ? "ECS" : "OCS", ami.ntsc ? "NTSC" : "PAL");

    amiga_reset();
    return true;
}

void amiga_reset(void)
{
    memset(ami.chip, 0, ami.chip_size);
    if (ami.slow) {
        memset(ami.slow, 0, ami.slow_size);
    }
    if (ami.fast) {
        memset(ami.fast, 0, ami.fast_size);
    }
    ami.cycle = 0;
    ami.cpu_rem = 0;
    ami.cpu_running = false;
    ami.line = 0;
    ami.line_start = 0;
    ami.lines = ami.ntsc ? 263 : 313;
    ami.lof = true;
    ami.long_lines = false;
    ami.lol0 = false;
    ami.e_phase = 0;
    ami.frames = 0;
    ami.halted = false;
    ami.ipl = 0;
    ami.stall = 0;
    ami.ovl = true;
    hdboard_reset();
    autoconf_reset();
    gayle_reset();
    mem_update_map();
    cia_reset();
    custom_reset();
    copper_reset();
    blitter_reset();
    display_reset();
    floppy_reset();
    input_reset();
    audio_reset();
    m68k_set_irq(0);
    m68k_pulse_reset();
}

/* The reset line (the keyboard's Ctrl-Amiga-Amiga): the CPU and the chips
 * start again, memory stays (Kickstart warm-starts)
 */
static void warm_reset(void)
{
    ami.halted = false;
    ami.ipl = 0;
    ami.ovl = true;
    hdboard_reset();
    autoconf_reset();
    gayle_reset();
    mem_update_map();
    cia_reset();
    custom_reset();
    copper_reset();
    blitter_reset();
    floppy_reset();
    input_reset();
    audio_reset();
    m68k_set_irq(0);
    m68k_pulse_reset();
}

/* ---- The frame loop ---- */

static inline bool on_cpu_bus(uint32_t pc)
{
    pc &= 0xFFFFFF;
    return pc >= 0xF80000 || (ami.fast_base && pc - ami.fast_base < ami.fast_size) ||
           (ami.ext && pc >= 0xE00000 && pc < 0xE00000 + EXT_ROM_SIZE);
}

static void run_cpu(int until)
{
    if (ami.halted) {
        ami.cycle = until;
        return;
    }
    /* clocks the chips' DMA took from the 68000 pass without it; running
     * from fast RAM or the ROM (its own bus) it doesn't wait for them
     */
    if (ami.stall > 0 && on_cpu_bus(m68k_get_reg(NULL, M68K_REG_PC))) {
        ami.stall = 0;
    }
    if (ami.stall > 0) {
        int s = until - ami.cycle;
        s = s < ami.stall ? s : ami.stall;
        ami.cycle += s;
        ami.stall -= s;
        if (ami.cycle >= until) {
            return;
        }
    }
    /* (the 68EC020 runs two of its clocks a 68000 one: the remainder kept;
     * its instruction cache follows CACR as it was) */
    if (ami.cpu_shift) {
        mem_cacr(m68k_get_reg(NULL, M68K_REG_CACR));
    }
    ami.cpu_running = true;
    int used = m68k_execute((until - ami.cycle) << ami.cpu_shift) + ami.cpu_rem;
    ami.cpu_running = false;
    ami.cycle += used >> ami.cpu_shift;
    ami.cpu_rem = used & ((1 << ami.cpu_shift) - 1);
}

static inline int min_int(int a, int b)
{
    return a < b ? a : b;
}

/* The vertical blank interrupt reaches the 68000 a little into line 0
 * (Paula's interrupt lines, the instruction under way): a program waiting
 * in a loop for line 0 sees it (F1 GP Circuits does). Its request shows
 * in INTREQR from the line's start: a program polling INTREQR with the
 * interrupt on sees the bit before the handler clears it (Seeing Is
 * Believing's loader, btst #5,$dff01f / beq, under Kickstart). On the real
 * one an instruction under way as the interrupt comes still finishes with
 * what it read; here whole instructions run between events, so the delay
 * leaves room for a pass of such a loop (20 colour clocks) even after one
 * that ran past the line's start
 */
#define VERTB_DELAY     40
static int s_vertb_at = NEVER;

static int next_event(void)
{
    int t = min_int(display_fetch_at(), copper_next());
    t = min_int(t, s_vertb_at);
    if (blit.busy) {
        t = min_int(t, blit.end);
    }
    t = min_int(t, cia[0].next);
    return min_int(t, cia[1].next);
}

static void run_line(void)
{
    int line_end = ami.line_start + 2 * ami_line_cck(ami.line);
    for (;;) {
        int next = next_event();
        if (next >= line_end) {
            if (ami.cycle >= line_end) {
                break;
            }
            run_cpu(line_end);
            continue;
        }
        if (next > ami.cycle) {
            run_cpu(next);
            continue;
        }
        /* the earliest event is due */
        int ipl = ami.ipl;
        if (next == s_vertb_at) {
            s_vertb_at = NEVER;
            ami_update_irq();           /* (the request as the CPU left it) */
        } else if (next == display_fetch_at()) {
            display_fetch();
        } else if (next == copper_next()) {
            int fetch = display_fetch_at();
            copper_run(ami.cycle, min_int(line_end, fetch));
        } else if (blit.busy && next == blit.end) {
            blitter_finish();
        } else if (next == cia[0].next) {
            cia_sync(&cia[0], ami.cycle);
        } else {
            cia_sync(&cia[1], ami.cycle);
        }
        /* an interrupt raised while the 68000's last bus cycle waited for
         * the DMA: one more instruction first (mem.c, bus_waited_over()) */
        if (ami.ipl > ipl && !ami.cpu_shift && bus_waited_over(next)) {
            m68k_set_irq(ipl);
            run_cpu(ami.cycle + 1);
            m68k_set_irq(ami.ipl);
        }
    }
}

void amiga_run_frame(void)
{
    ami.long_lines = ami_ntsc_beam();
    ami.lines = ami.long_lines ? (ami.lof ? 263 : 262) : (ami.lof ? 313 : 312);
    int frame_cycles = ami_line_start(ami.lines);
#ifndef ESP_PLATFORM
    uint32_t insns_before = ami.insns;
#endif

    for (ami.line = 0; ami.line < ami.lines; ami.line++) {
        ami.line_start = ami_line_start(ami.line);
        if (ami.line == 0) {
            /* the vertical blank */
            copper_vblank();
            ami.intreq |= INT_VERTB;
            s_vertb_at = VERTB_DELAY;
            cia_tod_tick(&cia[0]);
        }
        bus_line_start();
        display_line_start();
        blitter_line_start();
        ami.stall += blitter_steal_line();
        if (ami.stall > 4 * CYCLES_LINE) {
            ami.stall = 4 * CYCLES_LINE;
        }
        run_line();
        display_line_end();
        cia_tod_tick(&cia[1]);
        floppy_sync(ami.line_start + 2 * ami_line_cck(ami.line));
        gayle_line();
        input_line();
        audio_line();
    }

    /* everything that keeps a time moves back by the frame */
    for (int i = 0; i < 2; i++) {
        cia_sync(&cia[i], ami.cycle);
        cia_rebase(&cia[i], frame_cycles);
    }
    floppy_rebase(frame_cycles);
    ami.cycle -= frame_cycles;
    if (blit.busy) {
        blit.end -= frame_cycles;
    }
    if (cop.at != NEVER) {
        cop.at -= frame_cycles;
    }
    ami.e_phase = (ami.e_phase + frame_cycles) % 10;
    ami.lol0 = ami.long_lines && ((ami.lines + ami.lol0) & 1);  /* the next frame's line 0 */
    /* interlace: long and short frames by turns (each field's lines) */
    bool lace = display_frame_end();
    if (lace) {
        ami.lof = !ami.lof;
    }
    display_next_field(lace);
    if (blit.busy && blit.end <= ami.cycle) {
        blitter_finish();
    }
    ami.frames++;
    input_frame();
    space_frame();
    /* the keyboard's reset: a few frames after its warning, for the OS */
    static int reset_in;
    if (input_reset_wanted()) {
        reset_in = 10;
    }
    if (reset_in > 0 && --reset_in == 0) {
        warm_reset();
    }
#ifndef ESP_PLATFORM
    (void)insns_before;
#endif
}

int amiga_screen_hz(void)
{
    return ami_ntsc_beam() ? 60 : 50;
}

int amiga_screen_height(void)
{
    return ami_ntsc_beam() ? AMIGA_SCREEN_LINES_NTSC * 2 : AMIGA_SCREEN_HEIGHT;
}

void amiga_get_status(amiga_status_t *st)
{
    st->frames = ami.frames;
    st->line = ami.line;
    st->cycle = ami_now() - ami.line_start;
    st->pc = m68k_get_reg(NULL, M68K_REG_PC);
    st->sr = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
    st->intena = ami.intena;
    st->intreq = ami.intreq;
    st->dmacon = ami.dmacon;
    st->halted = ami.halted;
    st->insns = ami.insns;
}

/*
 * A machine with nothing but a CPU, 64 KB of RAM and an interrupt every
 * frame, serving the debugger: for trying the VS Code extension without a
 * board or ROMs, and for its tests (they run here in CI).
 *
 *   build/demo [6502|z80] [port]     (default 6502 on port 6502)
 *
 * 50 frames a second of 20000 cycles; the interrupt comes at each frame's
 * start, until a read of $D000 (6502) or an OUT (Z80) takes it back. RAM
 * holds an idle loop that counts in X (6502) or B (Z80); a program loaded
 * by the debugger goes anywhere else. The screen is RAM $0400-$07E7 shown
 * as 40 x 25 grey cells of 8 x 8 pixels; typing puts the characters at
 * $02F0 onwards (a 16-byte ring, $02FF the next free index).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emudbg.h"
#include "m6502.h"
#include "z80.h"

#define FRAME_CYCLES    20000

static uint8_t s_ram[65536];
static uint8_t s_io[256];               /* $D000-$D0FF on the 6502 */
static m6502_t s_cpu;
static bool s_z80;

static uint8_t io_read(void *ctx, uint16_t a)
{
    (void)ctx;
    if (a == 0xD000) {
        s_cpu.irq = 0;                  /* the interrupt is taken back */
    }
    return s_io[a & 0xFF];
}

static void io_write(void *ctx, uint16_t a, uint8_t v)
{
    (void)ctx;
    s_io[a & 0xFF] = v;
}

static uint8_t z_in(uint16_t port, int at)
{
    (void)port;
    (void)at;
    return 0xFF;
}

static void z_out(uint16_t port, uint8_t v, int at)
{
    (void)port;
    (void)v;
    (void)at;
    z80.int_line = 0;
}

static uint8_t z_ack(void)
{
    return 0xFF;
}

static void load_rom(void)
{
    memset(s_ram, 0, sizeof(s_ram));
    if (s_z80) {
        static const uint8_t boot[] = {
            0x31, 0x00, 0xF0,           /* 0000 LD SP,$F000 */
            0xED, 0x56,                 /* 0003 IM 1 */
            0xFB,                       /* 0005 EI */
            0x04,                       /* 0006 idle: INC B */
            0x18, 0xFD,                 /* 0007 JR idle */
        };
        static const uint8_t isr[] = { 0xF5, 0xD3, 0x00, 0xF1, 0xFB, 0xED, 0x4D };  /* PUSH AF, OUT, POP AF, EI, RETI */
        memcpy(s_ram, boot, sizeof(boot));
        memcpy(s_ram + 0x38, isr, sizeof(isr));
    } else {
        static const uint8_t boot[] = {
            0xA2, 0xFF, 0x9A,           /* E000 LDX #$FF, TXS */
            0x58,                       /* E003 CLI */
            0xE8,                       /* E004 idle: INX */
            0x4C, 0x04, 0xE0,           /* E005 JMP idle */
        };
        static const uint8_t isr[] = { 0x48, 0xAD, 0x00, 0xD0, 0x68, 0x40 };      /* PHA, LDA $D000, PLA, RTI */
        memcpy(s_ram + 0xE000, boot, sizeof(boot));
        memcpy(s_ram + 0xE010, isr, sizeof(isr));
        s_ram[0xFFFA] = 0x10; s_ram[0xFFFB] = 0xE0;
        s_ram[0xFFFC] = 0x00; s_ram[0xFFFD] = 0xE0;
        s_ram[0xFFFE] = 0x10; s_ram[0xFFFF] = 0xE0;
    }
}

static uint32_t s_frames;

static void reset(void)
{
    load_rom();
    if (s_z80) {
        z80_reset();
    } else {
        s_cpu.irq = 0;
        m6502_reset(&s_cpu);
    }
}

static void frame(void)
{
    if (s_z80) {
        z80.int_line = 1;
        int t = 0;
        while (t < FRAME_CYCLES) {
            t += z80_step();
        }
    } else {
        s_cpu.irq = 1;
        s_cpu.until = s_cpu.cycles + FRAME_CYCLES;
        m6502_run(&s_cpu);
    }
    s_frames++;
}

static void where(char *buf, size_t len)
{
    snprintf(buf, len, "frame %lu", (unsigned long)s_frames);
}

static void screen(int *w, int *h)
{
    *w = 320;
    *h = 200;
}

static void render_line(int y, uint16_t *dst)
{
    for (int x = 0; x < 320; x++) {
        uint8_t v = s_ram[0x0400 + (y / 8) * 40 + x / 8];
        dst[x] = (uint16_t)((v >> 3) << 11 | (v >> 2) << 5 | (v >> 3));
    }
}

static void type(const char *text)
{
    for (; *text; text++) {
        uint8_t at = s_ram[0x02FF] & 15;
        s_ram[0x02F0 + at] = (uint8_t)*text;
        s_ram[0x02FF] = (uint8_t)(at + 1);
    }
}

static emudbg_cpu_t s_cpus[1];
static emudbg_machine_t s_machine = {
    .cpus = s_cpus,
    .cpu_count = 1,
    .where = where,
    .reset = reset,
    .screen = screen,
    .render_line = render_line,
    .type = type,
};

int main(int argc, char **argv)
{
    s_z80 = argc > 1 && strcmp(argv[1], "z80") == 0;
    int port = argc > 2 ? atoi(argv[2]) : 6502;
    if (s_z80) {
        for (int b = 0; b < Z80_BANKS; b++) {
            z80_rd[b] = z80_wr[b] = s_ram + b * (Z80_BANK_MASK + 1);
        }
        z80_set_bus(&(z80_bus_t){ .in = z_in, .out = z_out, .int_ack = z_ack });
        s_cpus[0] = (emudbg_cpu_t){ .name = "Z80", .arch = &emudbg_arch_z80 };
        s_machine.name = "Demo Z80";
    } else {
        static const m6502_bus_t bus = { .read = io_read, .write = io_write };
        m6502_init(&s_cpu, &bus);
        for (int p = 0; p < 256; p++) {
            s_cpu.rd[p] = s_cpu.wr[p] = p == 0xD0 ? NULL : s_ram + p * 256;
        }
        s_cpus[0] = (emudbg_cpu_t){ .name = "6502", .arch = &emudbg_arch_6502, .core = &s_cpu };
        s_machine.name = "Demo 6502";
    }
    reset();
    emudbg_set_machine(&s_machine);
    if (!emudbg_serve(port)) {
        return 1;
    }
    emudbg_host_loop(frame, 50);
    return 0;
}

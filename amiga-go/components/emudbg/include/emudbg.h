/*
 * emudbg: a debugger for the programs that run in the emulated machines,
 * served over TCP to the VS Code extension (github.com/rlucencic/
 * esp32emu-vscode) or anything else that speaks its protocol
 * (docs/debugger.md). Platform independent: the board serves it from a
 * task on core 0 (emudbg_freertos.c), the machines' host builds from a
 * thread (emudbg_posix.c).
 *
 * The CPU cores call emudbg_hit() before an instruction whose address is
 * set in the map the debugger gave them (NULL: never; a 16-bit CPU's map
 * has a bit per address, a 24-bit CPU's a bit per 256-byte page and the
 * debugger checks the address). To stop, emudbg_hit() blocks the
 * emulation task until the debugger lets it go on: every CPU of the
 * machine stops with it, and the network task answers commands meanwhile
 * (registers, memory, breakpoints). Pausing and single steps give the CPU
 * a map with every bit set.
 *
 * A machine describes its CPUs (emudbg_cpu_t: the core's adapter, from the
 * core's component, plus how to see memory) in emudbg_set_machine(), at
 * every start: the CPUs may differ (C64 or C128) and a start clears the
 * cores' maps.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(EMUDBG_OFF)
#define EMUDBG_ENABLED  0               /* the cores' own tests */
#elif defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if CONFIG_EMUDBG
#define EMUDBG_ENABLED  1
#else
#define EMUDBG_ENABLED  0
#endif
#else
#define EMUDBG_ENABLED  1               /* host builds: always */
#endif

#define EMUDBG_PROTOCOL 1

typedef struct emudbg_cpu emudbg_cpu_t;

/* A register: what the debugger shows and may change */
enum {
    EMUDBG_REG_PC = 0x01,               /* the program counter */
    EMUDBG_REG_SP = 0x02,               /* the stack pointer (step out compares it) */
    EMUDBG_REG_FLAGS = 0x04,            /* a status register: `flags` names its bits */
    EMUDBG_REG_RO = 0x08,               /* read only */
    EMUDBG_REG_FLOAT = 0x10,            /* an IEEE single's bits, shown as a number (wider ones rounded to it) */
};

/* An EMUDBG_REG_FLOAT register with nothing in it (a coprocessor's empty
 * stack register): a signaling NaN the conversions never make
 */
#define EMUDBG_FLOAT_EMPTY  0x7FA00000u

typedef struct {
    const char *name;                   /* "PC", "A", "D0" */
    uint8_t bits;                       /* 1 to 32 */
    uint8_t flags;
    const char *flag_names;             /* EMUDBG_REG_FLAGS: one char per bit, MSB first, '-' unused */
} emudbg_reg_t;

typedef enum {
    EMUDBG_INSN_OTHER,
    EMUDBG_INSN_CALL,                   /* JSR, JSL, CALL, RST, BSR: step over runs to its end */
    EMUDBG_INSN_RETURN,                 /* RTS, RTL, RTI, RET, RETI, RTE: step out stops after one */
} emudbg_insn_t;

/* A CPU core's side (m6502_dbg.c, z80_dbg.c, w65816_dbg.c, m68k_dbg.c, arm2_dbg.c, x86_dbg.c) */
typedef struct {
    const char *arch;                   /* "6502", "65c816", "z80", "68000", "6809", "arm2", "x86" */
    uint8_t addr_bits;                  /* 16, 24 or 26 (the ARM2: maps by page, as 24) */
    const emudbg_reg_t *regs;
    int reg_count;
    uint32_t (*get_reg)(emudbg_cpu_t *cpu, int reg);
    void (*set_reg)(emudbg_cpu_t *cpu, int reg, uint32_t v);
    /* Memory as the CPU sees it, without side effects (I/O reads 0xFF);
     * used when the machine's cpu gives no peek/poke
     */
    uint8_t (*peek)(emudbg_cpu_t *cpu, uint32_t addr);
    void (*poke)(emudbg_cpu_t *cpu, uint32_t addr, uint8_t v);
    /* The instruction at addr into buf (upper case, $ hex); its length */
    int (*disassemble)(emudbg_cpu_t *cpu, uint32_t addr, char *buf, int len);
    emudbg_insn_t (*insn)(emudbg_cpu_t *cpu, uint32_t addr);
    /* Where the interrupt handlers begin now (from the vectors), up to
     * max: steps run through an interrupt that comes meanwhile; the count
     */
    int (*interrupts)(emudbg_cpu_t *cpu, uint32_t *entries, int max);
    /* The stack pointer for steps, comparable across the CPU's stacks
     * (NULL: the EMUDBG_REG_SP register); lower is deeper
     */
    uint32_t (*stack)(emudbg_cpu_t *cpu);
    /* Give the core its map (NULL: no checks) and the cpu for emudbg_hit() */
    void (*set_map)(emudbg_cpu_t *cpu, const uint8_t *map);
} emudbg_arch_t;

/* A CPU of the machine */
struct emudbg_cpu {
    const char *name;                   /* "6502", "1541", "Z80" (unique in the machine) */
    const emudbg_arch_t *arch;
    void *core;                         /* the core's state (m6502_t *, w65816_t *); NULL for z80, m68k */
    /* Memory without side effects, if the core can't see it by itself
     * (the 68000 machines); NULL: the arch's
     */
    uint8_t (*peek)(void *ctx, uint32_t addr);
    void (*poke)(void *ctx, uint32_t addr, uint8_t v);
    void *ctx;
};

typedef struct {
    const char *name;                   /* "Atari 800XL" */
    emudbg_cpu_t *cpus;
    int cpu_count;
    /* Where the machine is, for a stop: "frame 812, line 103, cycle 54" (NULL: none) */
    void (*where)(char *buf, size_t len);
    /* Cold reset, from the emulation task between frames (NULL: none) */
    void (*reset)(void);
    /* The machine's picture (NULL: none): its size now, a line of it in
     * RGB565 (from the network task, as the board's presenter does)
     */
    void (*screen)(int *width, int *height);
    void (*render_line)(int y, uint16_t *dst);
    /* Type text on the machine's keyboard ('\n' is Return; NULL: can't) */
    void (*type)(const char *text);
} emudbg_machine_t;

/* ---- The machine's and the runtime's side ---- */

#if EMUDBG_ENABLED

/* At every (re)start of the machine, after its CPUs are set up (the
 * structure must stay; NULL: no machine)
 */
void emudbg_set_machine(const emudbg_machine_t *machine);

/* After a reset that may have cleared the CPUs' state (maps) */
void emudbg_rearm(void);

/* From the emulation task, after each frame ("run N frames") */
void emudbg_frame_end(void);

/* The runtime pauses the emulation (setup screen, disk change): a stopped
 * machine goes on to the end of its frame, breakpoints are off until
 * emudbg_resume()
 */
void emudbg_suspend(void);
void emudbg_resume(void);

/* Stopped at a breakpoint or step (the emulation task waits in emudbg_hit()) */
bool emudbg_stopped(void);

/* A client is connected */
bool emudbg_connected(void);

/* Serve the protocol on this TCP port (a task or thread of its own) */
bool emudbg_serve(int port);

#ifndef ESP_PLATFORM
/* Host builds: run the machine in real time for the debugger, forever
 * (frame, then emudbg_frame_end(), hz frames a second)
 */
void emudbg_host_loop(void (*frame)(void), int hz);

/* ... with the port from this environment variable (e.g. "XL_DEBUG"), if it's set */
void emudbg_host_run(const char *env, void (*frame)(void), int hz);
#endif

#else

static inline void emudbg_set_machine(const emudbg_machine_t *machine) { (void)machine; }
static inline void emudbg_rearm(void) {}
static inline void emudbg_frame_end(void) {}
static inline void emudbg_suspend(void) {}
static inline void emudbg_resume(void) {}
static inline bool emudbg_stopped(void) { return false; }
static inline bool emudbg_connected(void) { return false; }
static inline bool emudbg_serve(int port) { (void)port; return false; }

#endif

/* ---- The CPU cores' side ---- */

/* The cores' adapters (each in its core's component) */
extern const emudbg_arch_t emudbg_arch_6502;   /* m6502: core = m6502_t * */
extern const emudbg_arch_t emudbg_arch_6809;   /* m6809: core = m6809_t * (6809 or 6309) */
extern const emudbg_arch_t emudbg_arch_65c816; /* w65816: core = w65816_t * */
extern const emudbg_arch_t emudbg_arch_z80;    /* z80 (one, global) */
extern const emudbg_arch_t emudbg_arch_68000;  /* musashi (one, global): the machine gives peek and poke */
extern const emudbg_arch_t emudbg_arch_arm2;   /* arm2: core = arm2_t * */
extern const emudbg_arch_t emudbg_arch_x86;    /* x86: core = x86_t * (addresses linear) */

/* Before the instruction at pc (the core's registers are in its state):
 * may block until the debugger lets the machine go on, and may have
 * changed the registers
 */
void emudbg_hit(emudbg_cpu_t *cpu, uint32_t pc);

/* Is the map's bit for this address set: 16-bit maps by address, 24-bit
 * ones by page
 */
static inline bool emudbg_map16(const uint8_t *map, uint16_t pc)
{
    return (map[pc >> 3] >> (pc & 7)) & 1;
}

static inline bool emudbg_map24(const uint8_t *map, uint32_t pc)
{
    uint32_t page = (pc >> 8) & 0xFFFF;
    return (map[page >> 3] >> (page & 7)) & 1;
}

/* ---- The platform's side (emudbg_freertos.c, emudbg_posix.c) ---- */

void *emudbg_os_alloc(size_t size);    /* zeroed; big buffers (PSRAM) */
void emudbg_os_lock(void);              /* the debugger's state, recursive */
void emudbg_os_unlock(void);
/* The emulation task waits here while stopped (up to ms); the network
 * task wakes it
 */
void emudbg_os_wait(int ms);
void emudbg_os_wake(void);
/* A line to the client (adds '\n'; from any task; dropped if none) */
void emudbg_os_send(const char *line, size_t len);

/* For the platform's server: a client came or went; a command line from it */
void emudbg_client(bool connected);
void emudbg_command(char *line);

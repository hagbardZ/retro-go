/*
 * The debugger against the real 6502 and Z80 cores, on the computer:
 * make -C components/emudbg/test
 *
 * A thread runs a little machine (the "emulation task"); the main thread
 * sends commands as the network task would and checks the answers and the
 * stops: breakpoints, steps into, over and out of calls (one with its
 * parameters inline), interrupts coming during steps, pause, frames,
 * registers, memory, reset.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "emudbg.h"
#include "m6502.h"
#include "z80.h"

/* ---- The platform (emudbg_os_*): answers into a queue ---- */

static pthread_mutex_t s_lock;
static pthread_mutex_t s_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_q_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_w_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_w_cond = PTHREAD_COND_INITIALIZER;
static bool s_woken;
#define QN 256
static char *s_q[QN];
static int s_qh, s_qt;

void *emudbg_os_alloc(size_t size) { return calloc(1, size); }
void emudbg_os_lock(void) { pthread_mutex_lock(&s_lock); }
void emudbg_os_unlock(void) { pthread_mutex_unlock(&s_lock); }

void emudbg_os_wait(int ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += ms * 1000000L;
    t.tv_sec += t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&s_w_lock);
    if (!s_woken) {
        pthread_cond_timedwait(&s_w_cond, &s_w_lock, &t);
    }
    s_woken = false;
    pthread_mutex_unlock(&s_w_lock);
}

void emudbg_os_wake(void)
{
    pthread_mutex_lock(&s_w_lock);
    s_woken = true;
    pthread_cond_broadcast(&s_w_cond);
    pthread_mutex_unlock(&s_w_lock);
}

void emudbg_os_send(const char *line, size_t len)
{
    char *copy = malloc(len + 1);
    memcpy(copy, line, len);
    copy[len] = '\0';
    pthread_mutex_lock(&s_q_lock);
    s_q[s_qt++ % QN] = copy;
    pthread_cond_broadcast(&s_q_cond);
    pthread_mutex_unlock(&s_q_lock);
}

/* The next line (2 s at most) */
static char *next_line(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += 2;
    pthread_mutex_lock(&s_q_lock);
    while (s_qh == s_qt) {
        if (pthread_cond_timedwait(&s_q_cond, &s_q_lock, &t) != 0) {
            pthread_mutex_unlock(&s_q_lock);
            return NULL;
        }
    }
    char *line = s_q[s_qh++ % QN];
    pthread_mutex_unlock(&s_q_lock);
    return line;
}

static int s_failures, s_checks;

#define CHECK(cond, ...) do { s_checks++; if (!(cond)) { s_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                              printf(__VA_ARGS__); printf("\n"); } } while (0)

static int s_id;
static char s_reply[20000];
static char *s_held[8];                 /* events that came before an answer */
static int s_held_count;

/* A command; its answer (events before it are dropped: see expect_stop) */
static const char *cmd(const char *fmt, ...)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "%d ", ++s_id);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    emudbg_command(line);
    char want[32];
    snprintf(want, sizeof(want), "{\"id\":%d,", s_id);
    for (;;) {
        char *l = next_line();
        if (l == NULL) {
            snprintf(s_reply, sizeof(s_reply), "(no answer)");
            return s_reply;
        }
        if (strncmp(l, want, strlen(want)) == 0) {
            snprintf(s_reply, sizeof(s_reply), "%s", l);
            free(l);
            return s_reply;
        }
        /* An event can come before the answer (the machine stopped at once) */
        if (strstr(l, "\"event\"") && s_held_count < 8) {
            s_held[s_held_count++] = l;
        } else {
            free(l);
        }
    }
}

/* The next stop: its pc and reason */
static long expect_stop(char *reason, size_t len)
{
    for (;;) {
        char *l;
        if (s_held_count > 0) {
            l = s_held[0];
            memmove(s_held, s_held + 1, sizeof(s_held[0]) * --s_held_count);
        } else {
            l = next_line();
        }
        if (l == NULL) {
            snprintf(reason, len, "(none)");
            return -1;
        }
        if (strstr(l, "\"event\":\"stopped\"")) {
            const char *r = strstr(l, "\"reason\":\"") + 10;
            size_t k = 0;
            while (r[k] != '"' && k + 1 < len) {
                reason[k] = r[k];
                k++;
            }
            reason[k] = '\0';
            long pc = strtol(strstr(l, "\"pc\":") + 5, NULL, 10);
            free(l);
            return pc;
        }
        free(l);
    }
}

static long stop_after(const char *command, int cpu, char *reason, size_t len)
{
    const char *r = cpu >= 0 ? cmd("%s %d", command, cpu) : cmd("%s", command);
    CHECK(strstr(r, "\"ok\":true"), "%s: %s", command, r);
    return expect_stop(reason, len);
}

/* ---- A 6502 machine: 64 KB RAM, an I/O page at $D000, an IRQ every 37 cycles ---- */

static uint8_t s_ram[65536];
static m6502_t s_cpu;
static volatile bool s_run = true;
static volatile int s_irqs;

static uint8_t io_read(void *ctx, uint16_t a) { (void)ctx; (void)a; return 0; }
static void io_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; (void)a; (void)v; s_cpu.irq = 0; s_irqs++; }

static const uint8_t s_prog6502[] = {
    /* 0200 */ 0x58,                 /* CLI */
    /* 0201 */ 0xA2, 0x00,           /* LDX #0 */
    /* 0203 */ 0x20, 0x10, 0x02,     /* loop: JSR $0210 */
    /* 0206 */ 0xE8,                 /* INX */
    /* 0207 */ 0x4C, 0x03, 0x02,     /* JMP loop */
};
static const uint8_t s_sub6502[] = {
    /* 0210 */ 0xA9, 0x01,           /* LDA #1 */
    /* 0212 */ 0x20, 0x20, 0x02,     /* JSR $0220 (2 bytes inline) */
    /* 0215 */ 0x02, 0x03,
    /* 0217 */ 0xEA,                 /* NOP */
    /* 0218 */ 0x60,                 /* RTS */
};
static const uint8_t s_inline6502[] = {
    /* 0220: return address += 2 */
    0x68, 0x18, 0x69, 0x02, 0xA8, 0x68, 0x69, 0x00, 0x48, 0x98, 0x48, 0x60,
};
static const uint8_t s_irq6502[] = {
    /* 0300 */ 0x48, 0x8D, 0x00, 0xD0, 0x68, 0x40,  /* PHA, STA $D000 (ack), PLA, RTI */
};

static void reset6502(void)
{
    m6502_reset(&s_cpu);
}

static emudbg_cpu_t s_dcpu6502[] = { { .name = "6502", .arch = &emudbg_arch_6502, .core = &s_cpu } };
static const emudbg_machine_t s_m6502 = { .name = "test 6502", .cpus = s_dcpu6502, .cpu_count = 1,
                                          .reset = reset6502 };

static void *run6502(void *arg)
{
    (void)arg;
    while (s_run) {
        for (int k = 0; k < 400; k++) {          /* a "frame" */
            s_cpu.until = s_cpu.cycles + 37;
            m6502_run(&s_cpu);
            s_cpu.irq = 1;
        }
        emudbg_frame_end();
        usleep(500);
    }
    return NULL;
}

static void test6502(void)
{
    memcpy(s_ram + 0x200, s_prog6502, sizeof(s_prog6502));
    memcpy(s_ram + 0x210, s_sub6502, sizeof(s_sub6502));
    memcpy(s_ram + 0x220, s_inline6502, sizeof(s_inline6502));
    memcpy(s_ram + 0x300, s_irq6502, sizeof(s_irq6502));
    s_ram[0xFFFC] = 0x00; s_ram[0xFFFD] = 0x02;
    s_ram[0xFFFE] = 0x00; s_ram[0xFFFF] = 0x03;
    s_ram[0xFFFA] = 0x00; s_ram[0xFFFB] = 0x03;
    static const m6502_bus_t bus = { .read = io_read, .write = io_write };
    m6502_init(&s_cpu, &bus);
    for (int p = 0; p < 256; p++) {
        s_cpu.rd[p] = s_cpu.wr[p] = p == 0xD0 ? NULL : s_ram + p * 256;
    }
    m6502_reset(&s_cpu);
    emudbg_set_machine(&s_m6502);
    emudbg_client(true);

    pthread_t th;
    pthread_create(&th, NULL, run6502, NULL);
    char reason[32];
    const char *r = cmd("hello");
    CHECK(strstr(r, "\"machine\":\"test 6502\"") && strstr(r, "\"arch\":\"6502\""), "hello: %s", r);

    r = cmd("bps 0 0x0210");
    CHECK(strstr(r, "\"count\":1"), "bps: %s", r);
    long pc = expect_stop(reason, sizeof(reason));
    CHECK(pc == 0x210 && strcmp(reason, "breakpoint") == 0, "breakpoint: %lx %s", pc, reason);
    cmd("bps 0");

    /* Many steps: none may stop in the IRQ handler ($0300-$0305) */
    int in_irq = 0, inline_ok = 0, over_ok = 0;
    for (int k = 0; k < 300; k++) {
        r = cmd("regs 0");
        long cur = strtol(strstr(r, "\"regs\":[") + 8, NULL, 10);
        const char *how = k % 3 == 0 ? "step" : "next";
        pc = stop_after(how, 0, reason, sizeof(reason));
        if (pc >= 0x300 && pc < 0x306) {
            in_irq++;
        }
        if (cur == 0x212 && strcmp(how, "next") == 0) {
            inline_ok += pc == 0x217;
        }
        if (cur == 0x203 && strcmp(how, "next") == 0) {
            over_ok += pc == 0x206;
        }
        CHECK(strcmp(reason, "step") == 0, "%s at %lx: reason %s", how, cur, reason);
    }
    CHECK(in_irq == 0, "%d steps stopped in the interrupt handler", in_irq);
    CHECK(inline_ok > 0, "no step over the inline-parameter call checked");
    CHECK(over_ok > 0, "no step over JSR $0210 checked");
    CHECK(s_irqs > 50, "only %d interrupts during the steps", s_irqs);

    /* Step out of the subroutine: back after the JSR */
    cmd("bps 0 0x0217");
    cmd("continue");
    pc = expect_stop(reason, sizeof(reason));
    CHECK(pc == 0x217, "bp 0217: %lx", pc);
    cmd("bps 0");
    pc = stop_after("out", 0, reason, sizeof(reason));
    CHECK(pc == 0x206 && strcmp(reason, "step") == 0, "out: %lx %s", pc, reason);

    /* Registers and memory */
    r = cmd("setreg 0 A 0x5A");
    CHECK(strstr(r, "\"value\":90"), "setreg: %s", r);
    r = cmd("write 0 0x4000 a9ff00");
    CHECK(strstr(r, "\"count\":3"), "write: %s", r);
    r = cmd("read 0 $4000 3");
    CHECK(strstr(r, "\"data\":\"a9ff00\""), "read: %s", r);
    r = cmd("disasm 0 0x0210 3");
    CHECK(strstr(r, "LDA #$01") && strstr(r, "JSR $0220"), "disasm: %s", r);
    r = cmd("regs 9");
    CHECK(strstr(r, "\"ok\":false"), "regs 9: %s", r);

    /* Pause, frames, reset */
    cmd("continue");
    usleep(20000);
    pc = stop_after("pause", -1, reason, sizeof(reason));
    CHECK(strcmp(reason, "pause") == 0 && pc >= 0x200 && pc < 0x306, "pause: %lx %s", pc, reason);
    pc = stop_after("frames 3", -1, reason, sizeof(reason));
    CHECK(strcmp(reason, "frames") == 0, "frames: %s", reason);
    pc = stop_after("reset stop", -1, reason, sizeof(reason));
    CHECK(pc == 0x200 && strcmp(reason, "pause") == 0, "reset stop: %lx %s", pc, reason);
    r = cmd("state");
    CHECK(strstr(r, "\"state\":\"stopped\""), "state: %s", r);

    /* The client goes: the machine runs on */
    emudbg_client(false);
    usleep(20000);
    CHECK(!emudbg_stopped(), "still stopped after the client went");
    s_run = false;
    pthread_join(th, NULL);
}

/* ---- A Z80 machine: IM 1 interrupts, CALL, RST $38 stepped over ---- */

static uint8_t s_zram[65536];
static volatile bool s_zrun = true;

static uint8_t z_in(uint16_t port, int at) { (void)port; (void)at; return 0xFF; }
static void z_out(uint16_t port, uint8_t v, int at) { (void)port; (void)v; (void)at; z80.int_line = 0; }
static uint8_t z_ack(void) { return 0xFF; }

static const uint8_t s_progz80[] = {
    /* 0100 */ 0xED, 0x56,           /* IM 1 */
    /* 0102 */ 0xFB,                 /* EI */
    /* 0103 */ 0xCD, 0x10, 0x01,     /* loop: CALL $0110 */
    /* 0106 */ 0xFF,                 /* RST $38 (the interrupt handler, as a call) */
    /* 0107 */ 0x00,                 /* NOP */
    /* 0108 */ 0x18, 0xF9,           /* JR loop */
};
static const uint8_t s_subz80[] = {
    /* 0110 */ 0x3E, 0x01,           /* LD A,1 */
    /* 0112 */ 0x00, 0x00,           /* NOP NOP */
    /* 0114 */ 0xC9,                 /* RET */
};
static const uint8_t s_isrz80[] = {
    /* 0038 */ 0xF5, 0xD3, 0x00, 0xF1, 0xFB, 0xC9,  /* PUSH AF, OUT ($00),A (ack), POP AF, EI, RET */
};

static void resetz80(void)
{
    z80_reset();
}

static emudbg_cpu_t s_dcpuz80[] = { { .name = "Z80", .arch = &emudbg_arch_z80 } };
static const emudbg_machine_t s_mz80 = { .name = "test Z80", .cpus = s_dcpuz80, .cpu_count = 1,
                                         .reset = resetz80 };

static void *runz80(void *arg)
{
    (void)arg;
    while (s_zrun) {
        for (int k = 0; k < 400; k++) {
            int t = 0;
            while (t < 41) {
                t += z80_step();
            }
            z80.int_line = 1;
        }
        emudbg_frame_end();
        usleep(500);
    }
    return NULL;
}

static void testz80(void)
{
    memcpy(s_zram + 0x100, s_progz80, sizeof(s_progz80));
    memcpy(s_zram + 0x110, s_subz80, sizeof(s_subz80));
    memcpy(s_zram + 0x38, s_isrz80, sizeof(s_isrz80));
    s_zram[0] = 0xC3; s_zram[1] = 0x00; s_zram[2] = 0x01;      /* JP $0100 */
    for (int b = 0; b < Z80_BANKS; b++) {
        z80_rd[b] = z80_wr[b] = s_zram + b * (Z80_BANK_MASK + 1);
    }
    z80_set_bus(&(z80_bus_t){ .in = z_in, .out = z_out, .int_ack = z_ack });
    z80_reset();
    z80.sp.w = 0xF000;
    s_qh = s_qt;
    emudbg_set_machine(&s_mz80);
    emudbg_client(true);
    pthread_t th;
    pthread_create(&th, NULL, runz80, NULL);
    char reason[32];
    cmd("bps 0 0x0103");
    long pc = expect_stop(reason, sizeof(reason));
    CHECK(pc == 0x103, "z80 bp: %lx %s", pc, reason);
    cmd("bps 0");
    int in_isr = 0, rst_ok = 0, call_ok = 0;
    for (int k = 0; k < 300; k++) {
        const char *r = cmd("regs 0");
        long cur = strtol(strstr(r, "\"regs\":[") + 8, NULL, 10);
        const char *how = k % 3 == 0 ? "step" : "next";
        pc = stop_after(how, 0, reason, sizeof(reason));
        if (pc >= 0x38 && pc < 0x3E && !(cur >= 0x38 && cur < 0x3E) && cur != 0x106) {
            in_isr++;
        }
        if (cur == 0x106 && strcmp(how, "next") == 0) {
            rst_ok += pc == 0x107;
        }
        if (cur == 0x103 && strcmp(how, "next") == 0) {
            call_ok += pc == 0x106;
        }
    }
    CHECK(in_isr == 0, "z80: %d steps stopped in the interrupt handler", in_isr);
    CHECK(rst_ok > 0, "z80: step over RST $38 not seen");
    CHECK(call_ok > 0, "z80: step over CALL not seen");
    const char *r = cmd("disasm 0 0x0100 2");
    CHECK(strstr(r, "IM 1") && strstr(r, "EI"), "z80 disasm: %s", r);
    emudbg_client(false);
    s_zrun = false;
    pthread_join(th, NULL);
}

int main(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &attr);
    test6502();
    testz80();
    printf("%d checks, %d failed\n", s_checks, s_failures);
    return s_failures != 0;
}

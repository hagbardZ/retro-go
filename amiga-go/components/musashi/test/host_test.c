/*
 * Host-side checks of the Musashi build used by the emulator.
 * Build and run: make -C components/musashi/test
 */
#include <stdio.h>
#include <string.h>
#include "m68k.h"

#define RAM_SIZE        0x10000
#define BERR_BASE       0xFF8000    /* reads here raise a bus error */
#define IO_TIMESLICE    0xF00000    /* writes here end the timeslice */

static unsigned char ram[RAM_SIZE];
static unsigned int last_ack_level;
static int ack_vector;
static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static unsigned int rd16(unsigned int a) { return (ram[a] << 8) | ram[a + 1]; }
static unsigned int rd32(unsigned int a) { return (rd16(a) << 16) | rd16(a + 2); }
static void wr16(unsigned int a, unsigned int v) { ram[a] = v >> 8; ram[a + 1] = v; }
static void wr32(unsigned int a, unsigned int v) { wr16(a, v >> 16); wr16(a + 2, v); }

unsigned int m68k_read_memory_8(unsigned int a)
{
    if (a >= BERR_BASE && a < BERR_BASE + 0x1000) m68k_bus_error(a, 0);
    return a < RAM_SIZE ? ram[a] : 0;
}
unsigned int m68k_read_memory_16(unsigned int a)
{
    if (a >= BERR_BASE && a < BERR_BASE + 0x1000) m68k_bus_error(a, 0);
    return a < RAM_SIZE ? rd16(a) : 0;
}
unsigned int m68k_read_memory_32(unsigned int a)
{
    if (a >= BERR_BASE && a < BERR_BASE + 0x1000) m68k_bus_error(a, 0);
    return a < RAM_SIZE ? rd32(a) : 0;
}
unsigned int m68k_read_disassembler_16(unsigned int a) { return a < RAM_SIZE ? rd16(a) : 0; }
unsigned int m68k_read_disassembler_32(unsigned int a) { return a < RAM_SIZE ? rd32(a) : 0; }
void m68k_write_memory_8(unsigned int a, unsigned int v) { if (a < RAM_SIZE) ram[a] = v; }
void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    if (a == IO_TIMESLICE) { m68k_end_timeslice(); return; }
    if (a < RAM_SIZE) wr16(a, v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v) { if (a < RAM_SIZE) wr32(a, v); }

/* The machine hooks (m68kconf.h): not used here */
int m68k_host_illegal(int opcode)
{
    return 0;
}

void m68k_host_halted(void)
{
}

void m68k_host_exception(unsigned int vector)
{
}

static int int_ack(int level)
{
    last_ack_level = level;
    m68k_set_irq(0);
    return ack_vector;
}

/* Load code at 0x1000 and reset into it; vectors point at STOP stubs */
static void load(const unsigned short *code, int words)
{
    memset(ram, 0, sizeof(ram));
    wr32(0x000, 0x8000);            /* SSP */
    wr32(0x004, 0x1000);            /* PC */
    wr32(0x008, 0x0600);            /* bus error */
    wr32(0x00C, 0x0700);            /* address error */
    wr32(0x070, 0x0A00);            /* level 4 autovector (VBL) */
    wr32(0x100, 0x0900);            /* vector 64 (MFP) */
    for (unsigned int stub = 0x600; stub <= 0xA00; stub += 0x100) {
        wr16(stub, 0x4E72);         /* STOP #$2700 */
        wr16(stub + 2, 0x2700);
    }
    for (int i = 0; i < words; i++)
        wr16(0x1000 + i * 2, code[i]);
    m68k_pulse_reset();
    m68k_execute(1);                /* swallow the reset cycles */
}

static unsigned int reg(m68k_register_t r) { return m68k_get_reg(NULL, r); }

static void test_cycles_and_timeslice(void)
{
    unsigned short code[110];
    int n = 0;
    for (int i = 0; i < 100; i++) code[n++] = 0x4E71;   /* NOP x100: 400 cycles */
    code[n++] = 0x33FC; code[n++] = 0x1234;             /* MOVE.W #$1234,$F00000.L: 20 cycles */
    code[n++] = 0x00F0; code[n++] = 0x0000;
    code[n++] = 0x4E71;
    load(code, n);

    int used = m68k_execute(100000);
    CHECK(used == 420, "end_timeslice: executed %d cycles, expected 420", used);
    CHECK(reg(M68K_REG_PC) == 0x1000 + 100 * 2 + 8, "PC %06x", reg(M68K_REG_PC));
}

static void test_bus_error(void)
{
    const unsigned short code[] = { 0x3039, 0x00FF, 0x8000, 0x4E71 };  /* MOVE.W $FF8000.L,D0 */
    load(code, 4);
    m68k_execute(1000);

    unsigned int sp = reg(M68K_REG_SP);
    CHECK(reg(M68K_REG_PC) == 0x604, "bus error: PC %06x, expected handler", reg(M68K_REG_PC));
    CHECK(sp == 0x8000 - 14, "bus error: SP %06x, expected 7-word 68000 frame", sp);
    CHECK((rd16(sp) & 0x17) == 0x15, "bus error: status %04x, expected read + supervisor data", rd16(sp));
    CHECK(rd32(sp + 2) == BERR_BASE, "bus error: access address %08x", rd32(sp + 2));
    CHECK(rd16(sp + 6) == 0x3039, "bus error: IR %04x", rd16(sp + 6));
    CHECK(rd16(sp + 8) == 0x2700, "bus error: SR %04x", rd16(sp + 8));
}

static void test_address_error(void)
{
    const unsigned short code[] = { 0x3039, 0x0000, 0x1001, 0x4E71 };  /* MOVE.W $1001.L,D0 */
    load(code, 4);
    m68k_execute(1000);

    unsigned int sp = reg(M68K_REG_SP);
    CHECK(reg(M68K_REG_PC) == 0x704, "address error: PC %06x, expected handler", reg(M68K_REG_PC));
    CHECK(rd32(sp + 2) == 0x1001, "address error: access address %08x", rd32(sp + 2));

    const unsigned short jmp[] = { 0x4EF9, 0x0000, 0x1001 };            /* JMP $1001.L */
    load(jmp, 3);
    m68k_execute(1000);
    CHECK(reg(M68K_REG_PC) == 0x704, "odd opcode fetch: PC %06x, expected handler", reg(M68K_REG_PC));
}

static void test_interrupts(void)
{
    const unsigned short code[] = { 0x46FC, 0x2300, 0x60FE };           /* MOVE #$2300,SR; BRA.S * */

    load(code, 3);
    ack_vector = 64;
    m68k_set_irq(6);
    m68k_execute(1000);
    CHECK(last_ack_level == 6, "vectored: ack level %u", last_ack_level);
    CHECK(reg(M68K_REG_PC) == 0x904, "vectored: PC %06x, expected vector 64 handler", reg(M68K_REG_PC));

    load(code, 3);
    ack_vector = M68K_INT_ACK_AUTOVECTOR;
    m68k_set_irq(4);
    m68k_execute(1000);
    CHECK(reg(M68K_REG_PC) == 0xA04, "autovector: PC %06x, expected level 4 handler", reg(M68K_REG_PC));
}

static void test_disassembler(void)
{
    const unsigned short code[] = { 0x3039, 0x00FF, 0x8000 };
    char buf[100];
    load(code, 3);
    m68k_disassemble(buf, 0x1000, M68K_CPU_TYPE_68000);
    CHECK(strstr(buf, "move.w") != NULL, "disassembly '%s'", buf);
}

static void test_self_modifying_code(void)
{
    /* Change the following MOVEQ. The 68000 has fetched its old word
     * before the write; a taken branch must then fetch the changed word. */
    const unsigned short code[] = {
        0x3081,                 /* MOVE.W D1,(A0) */
        0x7001,                 /* MOVEQ #1,D0 (changed to #2) */
        0x4EF9, 0x0000, 0x1002 /* JMP $1002.L */
    };
    load(code, sizeof(code) / sizeof(code[0]));
    m68k_set_reg(M68K_REG_A0, 0x1002);
    m68k_set_reg(M68K_REG_D1, 0x7002);
    m68k_execute(8);             /* MOVE.W */
    CHECK(rd16(0x1002) == 0x7002, "next opcode was not written");
    m68k_execute(4);             /* MOVEQ */
#ifdef M68K_PREFETCH
    CHECK(reg(M68K_REG_D0) == 1, "prefetched opcode: D0 %u", reg(M68K_REG_D0));
#else
    CHECK(reg(M68K_REG_D0) == 2, "uncached opcode: D0 %u", reg(M68K_REG_D0));
#endif
    m68k_execute(10);            /* JMP */
    m68k_execute(4);             /* changed MOVEQ */
    CHECK(reg(M68K_REG_D0) == 2, "branch used stale opcode: D0 %u", reg(M68K_REG_D0));

    /* An extension word also prefetches the following opcode. */
    const unsigned short immediate[] = { 0x30BC, 0x7002, 0x7001, 0x4E72, 0x2700 };
    load(immediate, sizeof(immediate) / sizeof(immediate[0]));
    m68k_set_reg(M68K_REG_A0, 0x1004);
    m68k_execute(12);            /* MOVE.W #$7002,(A0) */
    m68k_execute(4);             /* MOVEQ */
#ifdef M68K_PREFETCH
    CHECK(reg(M68K_REG_D0) == 1, "extension prefetch: D0 %u", reg(M68K_REG_D0));
#else
    CHECK(reg(M68K_REG_D0) == 2, "extension without prefetch: D0 %u", reg(M68K_REG_D0));
#endif
}

int main(void)
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(int_ack);

    test_cycles_and_timeslice();
    test_bus_error();
    test_address_error();
    test_interrupts();
    test_disassembler();
    test_self_modifying_code();

    printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures != 0;
}

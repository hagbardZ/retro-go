/*
 * The 68000 for the debugger (emudbg.h): registers, the disassembler,
 * calls and returns. One 68000 (Musashi's state is global). Memory is the
 * machine's: it gives peek and poke in its emudbg_cpu_t (without them the
 * debugger reads through m68k_read_disassembler_16() and can't write).
 */
#include "m68k.h"
#include "emudbg.h"

#if defined(M68K_68030)
#define DASM_CPU    M68K_CPU_TYPE_68030
#elif defined(M68K_68020)
#define DASM_CPU    M68K_CPU_TYPE_68020
#else
#define DASM_CPU    M68K_CPU_TYPE_68000
#endif

const uint8_t *volatile m68k_dbg_map;
static emudbg_cpu_t *s_cpu;

static const emudbg_reg_t s_regs[] = {
    { "PC", 24, EMUDBG_REG_PC, NULL },
    { "SR", 16, EMUDBG_REG_FLAGS, "T-S--III---XNZVC" },
    { "D0", 32, 0, NULL }, { "D1", 32, 0, NULL }, { "D2", 32, 0, NULL }, { "D3", 32, 0, NULL },
    { "D4", 32, 0, NULL }, { "D5", 32, 0, NULL }, { "D6", 32, 0, NULL }, { "D7", 32, 0, NULL },
    { "A0", 32, 0, NULL }, { "A1", 32, 0, NULL }, { "A2", 32, 0, NULL }, { "A3", 32, 0, NULL },
    { "A4", 32, 0, NULL }, { "A5", 32, 0, NULL }, { "A6", 32, 0, NULL },
    { "A7", 32, EMUDBG_REG_SP, NULL },
    { "USP", 32, 0, NULL },
    { "SSP", 32, 0, NULL },
};

enum { R_PC, R_SR, R_D0, R_A0 = R_D0 + 8, R_A7 = R_A0 + 7, R_USP, R_SSP };

static m68k_register_t reg_of(int reg)
{
    switch (reg) {
    case R_PC: return M68K_REG_PC;
    case R_SR: return M68K_REG_SR;
    case R_USP: return M68K_REG_USP;
    case R_SSP: return M68K_REG_ISP;
    default: return (m68k_register_t)(M68K_REG_D0 + (reg - R_D0));
    }
}

static uint32_t get_reg(emudbg_cpu_t *cpu, int reg)
{
    (void)cpu;
    uint32_t v = m68k_get_reg(NULL, reg_of(reg));
    return reg == R_PC ? v & 0xFFFFFF : v;
}

static void set_reg(emudbg_cpu_t *cpu, int reg, uint32_t v)
{
    (void)cpu;
    m68k_set_reg(reg_of(reg), v);
}

static uint8_t peek(emudbg_cpu_t *cpu, uint32_t addr)
{
    (void)cpu;
    unsigned int w = m68k_read_disassembler_16(addr & 0xFFFFFE);
    return (uint8_t)(addr & 1 ? w : w >> 8);
}

static void poke(emudbg_cpu_t *cpu, uint32_t addr, uint8_t v)
{
    (void)cpu;
    (void)addr;
    (void)v;
}

static uint8_t peek_cpu(emudbg_cpu_t *cpu, uint32_t addr)
{
    return cpu->peek ? cpu->peek(cpu->ctx, addr & 0xFFFFFF) : peek(cpu, addr);
}

static uint32_t peek32(emudbg_cpu_t *cpu, uint32_t addr)
{
    return (uint32_t)peek_cpu(cpu, addr) << 24 | (uint32_t)peek_cpu(cpu, addr + 1) << 16 |
           (uint32_t)peek_cpu(cpu, addr + 2) << 8 | peek_cpu(cpu, addr + 3);
}

static int disassemble(emudbg_cpu_t *cpu, uint32_t addr, char *buf, int len)
{
    (void)cpu;
    char text[128];
    int n = (int)m68k_disassemble(text, addr & 0xFFFFFF, DASM_CPU);
    int k = 0;
    for (; text[k] && k < len - 1; k++) {
        buf[k] = text[k];
    }
    buf[k] = '\0';
    return n > 0 ? n : 2;
}

static emudbg_insn_t insn(emudbg_cpu_t *cpu, uint32_t addr)
{
    uint16_t op = (uint16_t)(peek_cpu(cpu, addr) << 8 | peek_cpu(cpu, addr + 1));
    if ((op & 0xFFC0) == 0x4E80 || (op & 0xFF00) == 0x6100 || (op & 0xFFF0) == 0x4E40 ||
        (op & 0xF000) == 0xA000 || (op & 0xF000) == 0xF000) {
        return EMUDBG_INSN_CALL;            /* JSR BSR TRAP, line A (the Mac's traps) and F */
    }
    if (op == 0x4E75 || op == 0x4E77 || op == 0x4E73) {
        return EMUDBG_INSN_RETURN;          /* RTS RTR RTE */
    }
    return EMUDBG_INSN_OTHER;
}

static int interrupts(emudbg_cpu_t *cpu, uint32_t *entries, int max)
{
    /* The autovectors (24-31) and the ST's MFP (64-79) */
    int n = 0;
    for (int v = 24; v < 80 && n < max; v++) {
        if (v == 32) {
            v = 64;
        }
        entries[n++] = peek32(cpu, (uint32_t)v * 4) & 0xFFFFFF;
    }
    return n;
}

/* A7 of the current stack, the user's above the supervisor's: a trap or
 * an interrupt from user code goes deeper
 */
static uint32_t stack(emudbg_cpu_t *cpu)
{
    (void)cpu;
    uint32_t a7 = m68k_get_reg(NULL, M68K_REG_A7) & 0xFFFFFF;
    return m68k_get_reg(NULL, M68K_REG_SR) & 0x2000 ? a7 : 0x80000000u | a7;
}

static void set_map(emudbg_cpu_t *cpu, const uint8_t *map)
{
    s_cpu = cpu;
    m68k_dbg_map = map;
}

void m68k_dbg_hit(unsigned int pc)
{
    emudbg_hit(s_cpu, pc & 0xFFFFFF);
}

const emudbg_arch_t emudbg_arch_68000 = {
    .arch = "68000",
    .addr_bits = 24,
    .regs = s_regs,
    .reg_count = sizeof(s_regs) / sizeof(s_regs[0]),
    .get_reg = get_reg,
    .set_reg = set_reg,
    .peek = peek,
    .poke = poke,
    .disassemble = disassemble,
    .insn = insn,
    .interrupts = interrupts,
    .stack = stack,
    .set_map = set_map,
};

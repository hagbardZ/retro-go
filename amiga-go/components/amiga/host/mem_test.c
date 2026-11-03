/* The chip bus as seen through floating custom registers. No ROM needed. */
#include <stdio.h>
#include <string.h>
#include "amiga_internal.h"
#include "m68k.h"

static uint8_t chip[512 * 1024], slow[512 * 1024], rom[256 * 1024];
static int failures;

#define CHECK(cond, message) do { \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s\n", message); } \
} while (0)

void m68k_host_instruction(unsigned int pc) { (void)pc; }

static unsigned floating_word(void) { return m68k_read_memory_16(0xDFF0FA); }

int main(void)
{
    memset(&ami, 0, sizeof(ami));
    ami.chip = chip;
    ami.chip_size = sizeof(chip);
    ami.chip_mask = sizeof(chip) - 1;
    ami.slow = slow;
    ami.slow_size = sizeof(slow);
    ami.rom = rom;
    ami.rom_size = sizeof(rom);
    mem_update_map();

    m68k_write_memory_16(0x1000, 0x6602);
    CHECK(floating_word() == 0x6602, "chip RAM word write drives the bus");
    m68k_write_memory_8(0x1001, 0x55);
    CHECK(floating_word() == 0x5555, "68000 byte write drives both lanes");
    m68k_read_memory_8(0x1000);
    CHECK(floating_word() == 0x6655, "chip RAM byte read drives its whole word");
    m68k_write_memory_32(0x1000, 0x12345678);
    CHECK(floating_word() == 0x5678, "long write leaves its second word on the bus");
    m68k_write_memory_16(0xC01000, 0xABCD);
    m68k_read_memory_16(0x1000);
    CHECK(floating_word() == 0x1234, "chip RAM word read replaces the bus word");
    m68k_read_memory_16(0xC01000);
    CHECK(floating_word() == 0xABCD, "slow RAM reads share the chip bus");
    m68k_read_memory_32(0x1000);
    CHECK(floating_word() == 0x5678, "long read leaves its second word on the bus");
    m68k_read_memory_16(0xF80000);
    CHECK(floating_word() == 0x5678, "ROM reads on the CPU bus leave the chip bus alone");

    /* Steel Empire probes at 1 KB intervals, landing on write-only
     * mirrors. Its following opcode fetch must break the apparent RAM. */
    m68k_write_memory_8(0xC801D8, 0x55);
    CHECK(m68k_read_memory_8(0xC801D8) == 0x55, "floating byte after a custom write");
    m68k_read_memory_16(0xC01000);   /* stand in for the next opcode fetch */
    CHECK(m68k_read_memory_8(0xC801D8) != 0x55, "custom mirror must not pass a RAM probe");

    /* Without individual DMA bus values, OCS's missing ID uses the
     * undriven-bus fallback, independent of the last opcode's bit 2. */
    m68k_write_memory_16(0x1000, 0x51C9);
    m68k_read_memory_16(0x1000);
    CHECK(custom_read(0x07C) == 0xFFFF, "OCS must not identify AGA from an opcode word");
    ami.ecs = true;
    CHECK(custom_read(0x07C) == 0xFFFC, "ECS Denise ID");
    ami.aga = true;
    CHECK(custom_read(0x07C) == 0x00F8, "AGA Lisa ID");

    puts(failures ? "chip bus checks failed" : "all chip bus checks passed");
    return failures != 0;
}

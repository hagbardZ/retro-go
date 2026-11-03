# musashi — 68000 CPU core

[Musashi](https://github.com/kstenerud/Musashi) (MIT licence, see `musashi/readme.txt`)
configured as a plain 68000, shared by the Atari STE (`../esp32ste`) and
Macintosh (`../esp32mac`) emulators, as a 68020 for the Macintosh LC and II
(`../esp32mac2`), or as a 68030 with its MMU for the Macintosh LC II and LC
III (`../esp32mac3`): the `M68K_CPU` choice in this component's Kconfig
(`CONFIG_M68K_68020` or `CONFIG_M68K_68030` in the project's
`sdkconfig.defaults`), `-DM68K_68020` or `-DM68K_68030` in host builds. The
Mac II's 68881 is Musashi's FPU, switched on with `m68k_set_fpu()`; the
68030 always has it. Or the type chosen at run time, 68000 to 68020
(`CONFIG_M68K_RUNTIME`, `-DM68K_RUNTIME`: the Amiga's A500 and A1200 in one
firmware): no `M68K_FIXED_CPU_TYPE`, so the type checks read
`m68k_set_cpu_type()`'s choice; measured for the Amiga's 68000 on the host
and the Tab5, no slower (2026-10-21). Set the type before
`m68k_use_table_copies()` (the copy takes its cycle counts).

## Source

Vendored from Matt Evans' fork, branch `umac-hacks`
(https://github.com/evansm7/Musashi, commit `b3144f1`). The fork adds embedded
options on top of upstream; pico-mac uses it to run a Mac on an RP2040:

- `M68K_DYNAMIC_INSTR_TABLES OFF`: the 64K-entry opcode jump table is generated
  at build time and kept in flash, not built at runtime in 256 KB of RAM
- `M68K_FIXED_CPU_TYPE`: the CPU model is fixed at compile time, so code for
  other models is dropped
- `M68K_FAST_FUNC`: an attribute hook, mapped to `IRAM_ATTR` here

## Local patches (`patches/`, applied on top of `umac-hacks`)

1. `0001` upstream fix b2425b5: `m68k_execute()` returned the wrong cycle count
   after `m68k_end_timeslice()`.
2. `0002` changes for the emulator:
   - `m68kmake` also generates a static `m68ki_static_cycles` table, so cycle
     counting works with static tables. The 68000 row is identical to the
     table upstream builds at runtime.
   - Fixed the fork's runtime builder (`SET_CPU_CYCLECOUNT` wrote to the wrong
     index).
   - 68000 bus errors push the 7-word group 0 frame (the fork only had the 68010
     frame). They also skip the per-instruction register snapshot, because a
     68000 does not restore registers.
   - New `m68k_bus_error(address, write)` raises a bus error from inside a
     memory callback.
   - The fast opcode fetch raises an address error on an odd PC again.
3. `0003` a double bus/address fault halts the CPU without the MAME `$FFFF01`
   read. On the ST that address raises another bus error, so the halt
   recursed until the stack overflowed.

### Changed in place, not yet in `patches/`

- `m68k_set_cpu_type()` (in `m68kcpu.c`): the fork's
  `assert(cpu_type == M68K_FIXED_CPU_TYPE)` compared the public CPU type
  with the internal bit mask (true by chance for the 68000 only); removed.
- The FPU (`m68kfpu.c`, MAME's 68040 FPU): `m68k_set_fpu(on)` (a
  `has_fpu` flag in the CPU state) runs it beside a 68000-68020 too
  (`m68k_in.c`'s two FPU opcode handlers, regenerated); the 68881's
  transcendental functions (FSINH ... FACOS, which the 68040 leaves to
  software) through libm in double precision, and FGETMAN, FSCALE.
  `fatalerror()` logs the first few times instead of calling `exit()`
  (which on a board restarts it).

- `M68K_EMULATE_PREFETCH` (`CONFIG_M68K_PREFETCH`, off by default; the
  Amiga's on): `m68ki_read_opcode_16()` takes the opcode from the prefetch
  too and fetches the word after it, as `m68ki_read_imm_16()` already did
  for extension words (the fork's opcode fetch skipped it). An instruction
  writing over the next one then runs the old word, as on a 68000:
  Copylock-style trace decoders (Razor 1911's Voyage) toggle the next
  instruction and rely on it. `make -C components/musashi/test run-prefetch`
  checks self-modifying opcodes, extension-word prefetch and branch
  refetching with the option on; the default test also checks it off.
- `M68K_HALT_CALLBACK()` (in `m68kcpu.h`, both double-fault halts): tells
  the machine that the CPU halted (`m68k_host_halted()`), so the emulation
  stops running it and logs the fault once.
- `M68K_VECTOR_CALLBACK(v)` (in `m68ki_jump_vector()`): every exception,
  after its frame is pushed (`m68k_host_exception()`). The ST uses it to
  find GEMDOS's memory block (`Getmpb`, trap #13) for the free RAM display.
- `m68k_use_table_copies(buf)` (in `m68kcpu.c`, needs `<string.h>`): runs
  from copies of the opcode jump table and the 68000 cycle table in `buf`
  (0x50000 bytes). On the ESP32 read-only data stays in flash, so the
  tables are copied to PSRAM at start; reading them from flash cost about
  25% of the emulation speed.

- The 68030's MMU (`m68kmmu.h`, rewritten from the MC68030 User's Manual
  with MAME's `m68kmmu.h` to check against; the fork's walked the tables on
  every access, had no transparent translation, read the 030's PMOVE
  TT0/TT1 as SRP/CRP and called `exit()` on anything else): TT0/TT1, TC
  (IS, TIA-TID, PS, SRE, FCL), short and long descriptors, early
  termination, indirect descriptors, U and M written back, a direct-mapped
  ATC of 256 pages; PMOVE (all three formats, FD respected: TC, SRP and CRP
  flush the ATC without it, TT0/TT1 never do), PFLUSH, PLOAD (fills the
  ATC), PTEST (MMUSR, An). Faults are logged and the access goes through
  untranslated: no MMU exceptions, so no virtual memory. Accesses pass
  their function code and direction (`pmmu_translate_addr(addr, fc,
  write)`), a reset turns translation off. **The instructions after a
  PMOVE that changes translation keep the old mapping** up to the end of
  their 16-byte cache line (the first fetch elsewhere ends it, a run of
  PMOVEs keeps the first mapping): Apple's ROMs switch with `pmove
  (a3),tc; jmp (a5)`, where the `jmp` was fetched before and its address
  means something else under the new tables.
- The disassembler's opcode table (`m68kdasm.c`, 256 KB) is allocated
  when it's first used instead of being static: the firmware disassembles
  for the debugger only, and 256 KB of static RAM doesn't fit next to the
  rest (it goes to PSRAM).

`m68kconf.h` turns on `M68K_INSTRUCTION_HOOK` for the debugger (emudbg.h):
before each instruction it tests a pointer, `m68k_dbg_map`, and calls
`m68k_dbg_hit()` (`m68k_dbg.c`) in pages with a breakpoint. Built with
`-DM68K_INSTRUCTION_HOOK=2`, the hook also calls the machine's
`m68k_host_instruction()`: the host harnesses' trace options.

## Layout

| Path | What |
|---|---|
| `m68kconf.h` | Emulator configuration (replaces upstream's) |
| `musashi/` | Vendored core sources, `m68k_in.c` and the `m68kmake.c` generator |
| `generated/` | `m68kops.c/h` produced by `m68kmake`, committed |
| `tools/regen_ops.sh` | Rebuild `generated/` after changing `m68k_in.c` or `m68kmake.c` |
| `test/` | Host tests, run with `make -C components/musashi/test` |

## Host interface

The machine provides the memory callbacks declared in `m68k.h`:
`m68k_read_memory_8/16/32`, `m68k_write_memory_8/16/32` and
`m68k_read_disassembler_16/32`, and the hooks declared in `m68kconf.h`:

- `m68k_host_illegal(opcode)`: illegal opcodes, offered to the machine
  first; nonzero means handled (native calls from the machine's own 68000
  code: the ST's GEMDOS cartridge, the Mac's disk driver)
- `m68k_host_halted()`: a double bus/address fault halted the CPU
- `m68k_host_exception(vector)`: every exception, after its frame is pushed
- `m68k_host_instruction(pc)`: only with `-DM68K_INSTRUCTION_HOOK=2`

For the debugger, `m68k_dbg.c` is the 68000's `emudbg_arch_68000`; the
machine gives it side-effect-free peek and poke functions.

It must also set:

- `m68k_set_int_ack_callback()`: return a vector number (MFP) or
  `M68K_INT_ACK_AUTOVECTOR` (HBL/VBL)
- `m68k_set_reset_instr_callback()`: the `RESET` instruction

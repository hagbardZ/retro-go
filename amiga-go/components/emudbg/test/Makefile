# Host test of the debugger with the 6502 and Z80 cores: make -C components/emudbg/test
#   build/dbg_test                   the debugger's commands and steps against both cores
#   build/demo [6502|z80] [port]     a machine with just a CPU and RAM, serving the debugger
#                                    (the VS Code extension's tests run against it)
BUILD   := build
CFLAGS  := -O2 -g -Wall -Wextra -Wno-unused-parameter -I../include -I../../m6502/include -I../../z80/include
SRCS    := ../emudbg.c ../../m6502/m6502.c ../../m6502/m6502_dbg.c ../../z80/z80.c ../../z80/z80_dbg.c

run: $(BUILD)/dbg_test $(BUILD)/demo
	./$(BUILD)/dbg_test

$(BUILD)/demo: demo.c $(SRCS) ../emudbg_posix.c ../include/emudbg.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) demo.c $(SRCS) ../emudbg_posix.c -lpthread -o $@

$(BUILD)/dbg_test: dbg_test.c $(SRCS) ../include/emudbg.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) dbg_test.c $(SRCS) -lpthread -o $@

clean:
	rm -rf $(BUILD)

.PHONY: run clean

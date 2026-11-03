# Host build of the Amiga core: make -C components/amiga/host run
# KICK defaults to the repo root's roms/amiga-os-130.rom; because -C runs
# make here, a relative KICK= (e.g. roms/x.rom) resolves under this
# directory and must be given as an absolute path instead.
ROOT      := ../../..
MUSASHI   := ../../../../esp32emu/components/musashi
EMUDBG    := ../../../../esp32emu/components/emudbg
AMI       := ..
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DM68K_INSTRUCTION_HOOK=2 -DM68K_RUNTIME -DM68K_NO_BUS_ERRORS -DM68K_PROGRAM_READS -DM68K_PREFETCH -I$(AMI)/include -I$(AMI) -I$(MUSASHI) -I$(MUSASHI)/musashi -I$(MUSASHI)/generated -I$(EMUDBG)/include
KICK      ?= $(ROOT)/roms/amiga-os-130.rom
FRAMES    ?= 500

AMI_SRCS  := $(wildcard $(AMI)/*.c)
M68K_SRCS := $(MUSASHI)/musashi/m68kcpu.c $(MUSASHI)/musashi/m68kdasm.c \
             $(MUSASHI)/musashi/softfloat/softfloat.c $(MUSASHI)/generated/m68kops.c \
             $(MUSASHI)/m68k_dbg.c
AMI_OBJS  := $(patsubst $(AMI)/%.c,$(BUILD)/ami/%.o,$(AMI_SRCS))
M68K_OBJS := $(patsubst $(MUSASHI)/%.c,$(BUILD)/m68k/%.o,$(M68K_SRCS)) \
             $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o

run: $(BUILD)/ami_host
	./$(BUILD)/ami_host $(KICK) $(FRAMES) $(BUILD)/screen.ppm
	sips -s format png $(BUILD)/screen.ppm --out $(BUILD)/screen.png >/dev/null

$(BUILD)/ami_host: host_main.c $(AMI_OBJS) $(M68K_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -lz -o $@

test: $(BUILD)/floppy_test $(BUILD)/mem_test
	./$(BUILD)/floppy_test
	./$(BUILD)/mem_test

$(BUILD)/floppy_test: floppy_test.c $(AMI_OBJS) $(M68K_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -lz -o $@

$(BUILD)/mem_test: mem_test.c $(AMI_OBJS) $(M68K_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -lz -o $@

$(BUILD)/ami/%.o: $(AMI)/%.c $(wildcard $(AMI)/*.h $(AMI)/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m68k/%.o: $(MUSASHI)/%.c $(MUSASHI)/m68kconf.h $(EMUDBG)/include/emudbg.h $(wildcard $(MUSASHI)/musashi/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: run test clean

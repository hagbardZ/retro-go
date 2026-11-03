# Host build of the component with the emulator's m68kconf.h
COMPONENT := ..
BUILD     := build
CFLAGS    := -O2 -I$(COMPONENT) -I$(COMPONENT)/musashi -I$(COMPONENT)/generated -I$(COMPONENT)/../emudbg/include -DEMUDBG_OFF
SRCS      := $(COMPONENT)/musashi/m68kcpu.c $(COMPONENT)/musashi/m68kdasm.c \
             $(COMPONENT)/musashi/softfloat/softfloat.c $(COMPONENT)/generated/m68kops.c
OBJS      := $(patsubst $(COMPONENT)/%.c,$(BUILD)/%.o,$(SRCS))

run: $(BUILD)/host_test
	./$(BUILD)/host_test

run-prefetch:
	$(MAKE) BUILD=build/prefetch CFLAGS="$(CFLAGS) -DM68K_PREFETCH" run

$(BUILD)/host_test: host_test.c $(OBJS)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/%.o: $(COMPONENT)/%.c $(COMPONENT)/m68kconf.h $(wildcard $(COMPONENT)/musashi/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: run run-prefetch clean

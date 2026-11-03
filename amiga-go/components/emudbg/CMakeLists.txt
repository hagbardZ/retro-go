# The debugger for programs in the emulated machines (see include/emudbg.h);
# the CPU cores' adapters are in their components (m6502_dbg.c, ...)
idf_component_register(SRCS "emudbg.c" "emudbg_freertos.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES lwip)

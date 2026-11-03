# The Amiga (platform independent; host build in host/)
idf_component_register(SRCS "amiga.c"
                            "mem.c"
                            "cia.c"
                            "custom.c"
                            "copper.c"
                            "blitter.c"
                            "display.c"
                            "floppy.c"
                            "rtc.c"
                            "input.c"
                            "audio.c"
                            "rp9.c"
                            "autoconf.c"
                            "gayle.c"
                            "hdf.c"
                            "hdboard.c"
                            "space.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES musashi)

# Runs together with the CPU core in the emulation loop
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

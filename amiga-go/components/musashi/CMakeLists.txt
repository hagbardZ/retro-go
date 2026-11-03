# Musashi 68000 core (evansm7 fork, branch umac-hacks, + patches/)
# generated/ holds the m68kmake output; rebuild it with tools/regen_ops.sh.
idf_component_register(SRCS "musashi/m68kcpu.c"
                            "musashi/m68kdasm.c"
                            "musashi/softfloat/softfloat.c"
                            "generated/m68kops.c"
                       INCLUDE_DIRS "." "musashi" "generated")

# Upstream code; keep its warnings out of the build log
target_compile_options(${COMPONENT_LIB} PRIVATE -w)

# The interpreter dominates emulator CPU time: always optimise it,
# even in -Og debug builds
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

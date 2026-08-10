cmake_minimum_required(VERSION 3.16)
file(GLOB_RECURSE helix_srcs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "*.c")
idf_component_register(
    SRCS ${helix_srcs}
    INCLUDE_DIRS "pub" "real" "."
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE -DCONFIG_MP3_DECODER_HELIX=1)

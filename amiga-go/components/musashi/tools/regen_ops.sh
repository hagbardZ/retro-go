#!/bin/sh
# Regenerate generated/m68kops.c and generated/m68kops.h from musashi/m68k_in.c.
# Run after changing m68k_in.c or m68kmake.c. Needs a host C compiler.
set -e

COMPONENT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

${HOST_CC:-cc} -O1 -w -o "$TMP_DIR/m68kmake" "$COMPONENT_DIR/musashi/m68kmake.c"
mkdir -p "$COMPONENT_DIR/generated"
"$TMP_DIR/m68kmake" "$COMPONENT_DIR/generated" "$COMPONENT_DIR/musashi/m68k_in.c"

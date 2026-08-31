#!/bin/sh
# SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Activate the ESP-IDF v6.1-beta1 environment for the esp32s31 target.
#
# Why this exists: v6.1-beta1's own `export.sh` does NOT work in this install.
# It looks for a virtualenv at ~/.espressif/python_env/idf6.1_py3.14_env, while
# the toolchain installer actually placed one under
# ~/.espressif/tools/python/v6.1-beta1/venv, so sourcing it fails with
# "virtual environment not found". Two more traps follow once that is fixed:
#
#   * idf.py's constraint check wants ~/.espressif/espidf.constraints.v6.1.txt,
#     which is not there (the file lives under tools/). IDF_PYTHON_CHECK_CONSTRAINTS=no
#     skips it.
#   * the component manager reads ESP_IDF_VERSION and crashes on None if the
#     variable is unset, because nothing else in this path exports it.
#
# The P4 target is unaffected — it builds under v6.0, whose export.sh is fine:
#     . ~/.espressif/v6.0/esp-idf/export.sh
#
# Usage (must be SOURCED, not executed):
#     . tools/s31-env.sh
#     idf.py -B build.s31 build
#     idf.py -B build.s31 -p /dev/ttyACM0 flash monitor

ESPRESSIF="${ESPRESSIF:-$HOME/.espressif}"
IDF_VER=v6.1-beta1

export IDF_TOOLS_PATH="$ESPRESSIF/tools"
export IDF_PATH="$ESPRESSIF/$IDF_VER/esp-idf"
export IDF_PYTHON_ENV_PATH="$ESPRESSIF/tools/python/$IDF_VER/venv"
export ESP_ROM_ELF_DIR="$ESPRESSIF/tools/esp-rom-elfs/20241011"
export IDF_PYTHON_CHECK_CONSTRAINTS=no
export ESP_IDF_VERSION=6.1

# embedded-zhc is resolved by sibling path, not pinned. CMakeLists already
# defaults to ../embedded-zhc, so leaving this unset still builds; it is set
# here so the value is visible in the banner and survives a build launched from
# somewhere other than the repo root.
#
# $0 is the SHELL when a script is sourced, not the script, so the path cannot
# be derived from it — walk up from $PWD instead.
if [ -z "$EMBEDDED_ZHC_PATH" ]; then
    _p=$PWD
    while [ "$_p" != "/" ]; do
        if [ -d "$_p/embedded-zhc/definitions" ]; then
            export EMBEDDED_ZHC_PATH="$_p/embedded-zhc"
            break
        fi
        _p=$(dirname "$_p")
    done
    unset _p
fi

# The v6.1 tool paths are not on PATH without export.sh, so add them by hand.
# Globs are resolved rather than hardcoded so a toolchain bump does not silently
# leave a stale compiler on PATH.
for _d in \
    "$IDF_PYTHON_ENV_PATH/bin" \
    "$IDF_PATH/tools" \
    "$ESPRESSIF"/tools/ninja/*/ \
    "$ESPRESSIF"/tools/cmake/*/bin \
    "$ESPRESSIF"/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin
do
    [ -d "$_d" ] && PATH="$_d:$PATH"
done
export PATH
unset _d

echo "IDF $IDF_VER for esp32s31"
echo "  IDF_PATH          = $IDF_PATH"
echo "  EMBEDDED_ZHC_PATH = ${EMBEDDED_ZHC_PATH:-<unset>}"
command -v ninja >/dev/null || echo "  WARNING: ninja not found on PATH"
command -v riscv32-esp-elf-gcc >/dev/null || echo "  WARNING: riscv toolchain not found on PATH"

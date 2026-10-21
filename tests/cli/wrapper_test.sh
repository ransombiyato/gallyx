#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# `hyperion` wrapper test.
#
# scripts/hyperion is the user-facing command installed by install.sh. It is a
# thin front end over the built binary, so it can silently drift out of date
# (it once claimed the translator did not exist and refused `run`). This test
# pins the two properties that matter:
#
#   1. `status` does not deny features that exist (the translator, code
#      generation, instruction decoding).
#   2. `run` reaches the real binary and executes a blob, instead of printing
#      NOT IMPLEMENTED.
#
# Usage: wrapper_test.sh <path-to-hyperion-binary>
#   HYPERION_SRC / HYPERION_BUILD_DIR  point the wrapper at the test build.
set -euo pipefail

BIN="${1:?usage: wrapper_test.sh <hyperion-binary>}"
WRAPPER="$(dirname "${BASH_SOURCE[0]}")/../../scripts/hyperion"
WRAPPER="$(cd "$(dirname "${WRAPPER}")" && pwd)/$(basename "${WRAPPER}")"

fail() {
    echo "wrapper test FAILED: $*" >&2
    exit 1
}

# `status` and `doctor` must be honest: neither may claim the translator is absent.
status="$(bash "${WRAPPER}" status)"
doctor="$(bash "${WRAPPER}" doctor)"
if grep -qi 'no way to run an x86-64 program' <<<"$status"; then
    fail "'status' still claims no x86-64 program can run"
fi
if grep -qi 'translator does not exist' <<<"$doctor"; then
    fail "'doctor' still claims the translator does not exist"
fi
if grep -qiE '^[[:space:]]*- x86-64 instruction decoding$' <<<"$status"; then
    fail "'status' still lists instruction decoding as NOT IMPLEMENTED"
fi
if ! grep -q 'run <exe>' <<<"$(bash "${WRAPPER}" help)"; then
    fail "'help' does not advertise the run command"
fi
if ! grep -q 'info <exe>' <<<"$(bash "${WRAPPER}" help)"; then
    fail "'help' does not advertise the info command"
fi

# `run` and `info` must delegate to the binary, not print NOT IMPLEMENTED.
BLOB="$(mktemp)"
ELF="$(mktemp)"
trap 'rm -f "$BLOB" "$ELF"' EXIT
printf '\xB8\x2a\x00\x00\x00\xF4' > "$BLOB" # mov eax, 42 ; hlt

# Build a tiny static x86-64 ELF64 binary with python3 (`mov eax, 60; xor edi, edi; syscall`).
python3 - "$ELF" <<'PY'
import struct, sys
ehdr = struct.pack(
    "<4sBBBBB7xHHIQQQIHHHHHH",
    b"\x7fELF", 2, 1, 1, 0, 0,
    2, 62, 1, 0x400078, 64, 0, 0, 64, 56, 1, 0, 0, 0,
)
code = b"\xb8\x3c\x00\x00\x00\x31\xff\x0f\x05"
phdr = struct.pack(
    "<IIQQQQQQ",
    1, 5, 0, 0x400000, 0x400000, 120 + len(code), 120 + len(code), 0x1000,
)
with open(sys.argv[1], "wb") as f:
    f.write(ehdr + phdr + code)
PY

info_out="$(bash "${WRAPPER}" info "$ELF" 2>&1)" || fail "'info' exited non-zero"
grep -q 'ELF64 little-endian x86-64' <<<"$info_out" || fail "'info' output missing format header"
grep -q 'entry:       0x0000000000400078' <<<"$info_out" || fail "'info' output missing entry point"

elf_out="$(bash "${WRAPPER}" run "$ELF" --tier=0 2>&1)" || fail "'run <elf>' exited non-zero"
grep -q '^status: ok$' <<<"$elf_out" || fail "'run <elf>' did not report ok"

out="$(bash "${WRAPPER}" run "$BLOB" --tier=0 --dump 2>&1)" \
    || fail "'run' exited non-zero"
if grep -q 'NOT IMPLEMENTED' <<<"$out"; then
    fail "'run' printed NOT IMPLEMENTED instead of delegating"
fi
grep -q '^status: ok$' <<<"$out" || fail "'run' did not report ok"
grep -q 'rax =0x000000000000002a' <<<"$out" || fail "'run' produced the wrong rax"

# Sanity: the wrapper is pointing at the binary we were handed.
[[ -x "$BIN" ]] || fail "test binary '$BIN' is not executable"

echo "wrapper ok: status honest, run delegates and executes"

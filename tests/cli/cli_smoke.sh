#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# CLI smoke test.
#
# Drives the real `hyperion run` command end to end: it writes a small x86-64
# blob, runs it through both execution tiers, and requires them to agree on the
# status and the number of guest instructions retired.
#
# On an AArch64 host (or under qemu-user when HYPERION_EMULATOR is set) tier 1
# executes generated code, so this also checks that tier 1 actually translated
# and ran at least one basic block. On an x86-64 host tier 1 cannot execute
# generated code and falls back to the interpreter, saying so on stderr; the
# script records that rather than treating it as a failure.
#
# Usage: cli_smoke.sh <path-to-hyperion-binary>
#   HYPERION_EMULATOR  optional command prefix, e.g. "qemu-aarch64 -L /usr/aarch64-linux-gnu"
set -euo pipefail

BIN="${1:?usage: cli_smoke.sh <hyperion-binary>}"
EMULATOR="${HYPERION_EMULATOR:-}"

BLOB="$(mktemp)"
ERRFILE="$(mktemp)"
trap 'rm -f "$BLOB" "$ERRFILE"' EXIT

# mov ecx, 5 ; add rbx, rax ; dec rcx ; jnz -5 ; hlt
printf '\xB9\x05\x00\x00\x00\x48\x01\xC3\x48\xFF\xC9\x75\xF8\xF4' > "$BLOB"

run_tier() {
    # shellcheck disable=SC2086
    $EMULATOR "$BIN" run "$BLOB" "--tier=$1" --max-steps=100000 2>"$ERRFILE"
}

out0="$(run_tier 0)"
out1="$(run_tier 1)"

for name in 0 1; do
    if [[ $name == 0 ]]; then out="$out0"; else out="$out1"; fi
    if ! grep -q '^status: ok$' <<<"$out"; then
        echo "tier $name did not report ok:" >&2
        echo "$out" >&2
        exit 1
    fi
done

steps0="$(grep -oE '^steps: *[0-9]+' <<<"$out0" | grep -oE '[0-9]+')"
steps1="$(grep -oE '^steps: *[0-9]+' <<<"$out1" | grep -oE '[0-9]+')"
if [[ "$steps0" != "$steps1" ]]; then
    echo "tiers disagree on step count: $steps0 vs $steps1" >&2
    exit 1
fi

# If tier 1 ran generated code (rather than falling back), it must have
# translated a non-zero number of blocks -- otherwise "ok" would be vacuous.
if ! grep -q 'falling back' "$ERRFILE"; then
    blocks="$(sed -n 's/^tier 1: \([0-9][0-9]*\).*/\1/p' <<<"$out1" | head -1)"
    if [[ -z "$blocks" || "$blocks" == "0" ]]; then
        echo "tier 1 claimed ok without translating any block" >&2
        exit 1
    fi
    echo "cli smoke ok: tier 1 executed $blocks basic block(s), steps=$steps0"
else
    echo "cli smoke ok: tier 1 fell back to the interpreter, steps=$steps0"
fi

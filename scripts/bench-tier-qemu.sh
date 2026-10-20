#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Measure Tier 0 (reference interpreter) against Tier 1 (AArch64 translator) on
# an AArch64 host. On an x86-64 development machine there is no AArch64 hardware
# and no way to execute the generated code, so the run is done under qemu-user.
#
# Read the caveat in BENCHMARKS.md before quoting these numbers: under qemu-user
# *both* tiers are additionally translated by qemu, so the ratio is a relative
# sanity signal and not a hardware performance claim. A real measurement requires
# the AArch64 target device.
#
# Usage: scripts/bench-tier-qemu.sh [--iterations N] [--repeats N]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root}/build/cross-bench"
# Prefer the repo-local cached emulator (tools/cache) if present: it survives the
# container resets that wipe /usr, so a benchmark uses the same qemu across
# sessions. HYPERION_QEMU still overrides everything.
cached_qemu="${root}/tools/cache/bin/qemu-aarch64-static"
if [ -n "${HYPERION_QEMU:-}" ]; then
    qemu="${HYPERION_QEMU}"
elif [ -x "${cached_qemu}" ]; then
    qemu="${cached_qemu}"
else
    qemu="qemu-aarch64"
fi
sysroot="${HYPERION_AARCH64_SYSROOT:-/usr/aarch64-linux-gnu}"

iterations=200000
repeats=7
wide=""
while [[ $# -gt 0 ]]; do
    case "$1" in
    --iterations)
        iterations="$2"
        shift 2
        ;;
    --repeats)
        repeats="$2"
        shift 2
        ;;
    --wide)
        wide="--wide"
        shift
        ;;
    *)
        echo "unknown argument: $1" >&2
        exit 2
        ;;
    esac
done

for tool in cmake "$qemu" aarch64-linux-gnu-g++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required tool '$tool' not found" >&2
        exit 1
    fi
done

cmake -S "${root}" -B "${build_dir}" \
    -DCMAKE_TOOLCHAIN_FILE="${root}/cmake/toolchains/aarch64-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DHYPERION_BUILD_BENCHMARKS=ON >/dev/null
cmake --build "${build_dir}" --target hyperion_tier_bench >/dev/null

echo "# host: $(uname -srm)"
echo "# command: ${qemu} -L ${sysroot} hyperion_tier_bench --iterations ${iterations} --repeats ${repeats} ${wide} --json"
"${qemu}" -L "${sysroot}" "${build_dir}/benchmarks/hyperion_tier_bench" \
    --iterations "${iterations}" --repeats "${repeats}" ${wide} --json

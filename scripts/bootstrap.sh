#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Project HYPERION - reproducible environment bootstrap.
#
# Installs the toolchain required for development and for the cross-architecture
# validation pipeline on a Debian/Ubuntu host. Requires sudo on the first run.
#
# This script is idempotent: re-running it is safe.
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "error: this bootstrap script supports apt-based distributions only." >&2
    echo "       install the equivalent packages manually (see README.md)." >&2
    exit 1
fi

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "error: root or sudo is required to install packages." >&2
        exit 1
    fi
fi

echo "==> updating package index"
${SUDO} apt-get update -qq

# Core build + test toolchain, cross-compiler for AArch64 validation, and
# qemu-user to execute the AArch64 artifacts on an x86-64 developer host.
PACKAGES=(
    build-essential
    cmake
    ninja-build
    clang
    clang-format
    clang-tidy
    llvm
    gcc-aarch64-linux-gnu
    g++-aarch64-linux-gnu
    binutils-aarch64-linux-gnu
    qemu-user
    qemu-user-static
    gdb-multiarch
    python3
    python3-venv
)

echo "==> installing: ${PACKAGES[*]}"
DEBIAN_FRONTEND=noninteractive ${SUDO} apt-get install -y -qq "${PACKAGES[@]}"

echo
echo "==> toolchain summary"
for tool in cmake ninja clang clang-format clang-tidy aarch64-linux-gnu-g++ qemu-aarch64; do
    if command -v "${tool}" >/dev/null 2>&1; then
        printf '  %-26s %s\n' "${tool}" "$("${tool}" --version 2>&1 | head -1)"
    else
        printf '  %-26s MISSING\n' "${tool}"
    fi
done

echo
echo "==> caching tools into the repository (survives a container reset)"
"${BASH_SOURCE[0]%/*}/toolcache.sh" populate --with-toolchain

echo
echo "bootstrap complete. Next: scripts/dev.sh bootstrap && scripts/dev.sh test"

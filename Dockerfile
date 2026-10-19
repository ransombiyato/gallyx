# SPDX-License-Identifier: Apache-2.0
# Reproducible HYPERION development environment.
#
# Provides the host toolchain, the AArch64 cross-compiler, and qemu-user so the
# full build/test/cross-test workflow runs identically everywhere:
#
#   docker build -t hyperion-dev .
#   docker run --rm -it -v "$PWD:/src" -w /src hyperion-dev \
#       bash -lc 'scripts/dev.sh bootstrap && scripts/dev.sh build && scripts/dev.sh test && scripts/dev.sh cross-test'
FROM debian:13-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        clang \
        clang-format \
        clang-tidy \
        llvm \
        gcc-aarch64-linux-gnu \
        g++-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        qemu-user \
        qemu-user-static \
        gdb-multiarch \
        git \
        ca-certificates \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Reproduce the UML feasibility measurements recorded in
# docs/architecture-feasibility.md.
#
# This runs a REAL Linux kernel as an unprivileged user-space process, with no
# root and no KVM, and measures whether it can reach a systemd multi-user boot.
# It is evidence-gathering for the architecture decision, not part of the
# translator.
#
# Usage:
#   experiments/uml/run.sh smoke      # boot kernel, prove execution (fast)
#   experiments/uml/run.sh systemd    # full systemd-as-PID-1 boot (slow)
#   experiments/uml/run.sh clean      # remove generated artifacts
#
# Requirements: linux.uml (Debian package user-mode-linux), debootstrap for
# the systemd target. Requires an exec-capable filesystem for TMPDIR; a tmpfs
# mounted noexec (common in containers) will fail the PROT_EXEC mmap check.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="${REPO_ROOT}/.uml-tmp"
ROOTFS_DIR="${REPO_ROOT}/.uml-rootfs"
IMAGE="${WORK_DIR}/rootfs.img"
UML_BIN="${UML_BIN:-$(command -v linux.uml || command -v linux || true)}"
MEM="${UML_MEM:-512M}"
TIMEOUT_SECS="${UML_TIMEOUT:-180}"

log() { printf '[uml] %s\n' "$*"; }
die() { printf '[uml] ERROR: %s\n' "$*" >&2; exit 1; }

require_uml() {
    [ -n "${UML_BIN}" ] || die "linux.uml not found. Install the 'user-mode-linux' package."
    [ -x "${UML_BIN}" ] || die "not executable: ${UML_BIN}"
    mkdir -p "${WORK_DIR}"
    # UML needs a TMPDIR where PROT_EXEC mmap works. tmpfs mounted noexec fails.
    if ! python3 -c 'import mmap; mmap.mmap(-1, 4096, prot=mmap.PROT_READ|mmap.PROT_WRITE|mmap.PROT_EXEC)' 2>/dev/null; then
        die "PROT_EXEC mmap is not permitted in this environment; UML cannot run here."
    fi
}

write_probe() {
    # Probe script runs INSIDE the guest and prints to the console.
    cat > "${WORK_DIR}/probe.sh" <<'PROBE'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
echo "@@@ HYPERION-PROBE-BEGIN @@@"
echo "kernel:        $(uname -r)"
echo "arch:          $(uname -m)"
echo "pid here:      $$"
echo "logical cpus:  $(grep -c ^processor /proc/cpuinfo)"
echo "mem total:     $(grep MemTotal /proc/meminfo)"
for ns in user mount pid uts ipc net; do
  if unshare --$ns true 2>/dev/null; then echo "unshare --$ns: OK"; else echo "unshare --$ns: DENIED"; fi
done
echo "@@@ HYPERION-PROBE-END @@@"
poweroff -f
PROBE
    chmod +x "${WORK_DIR}/probe.sh"
}

cmd_smoke() {
    require_uml
    write_probe
    log "booting UML as uid $(id -u) (no root, no KVM) with hostfs root"
    log "note: hostfs root means the guest sees the HOST filesystem - use the"
    log "      'systemd' target for an isolated measurement."
    TMPDIR="${WORK_DIR}" timeout "${TIMEOUT_SECS}" \
        "${UML_BIN}" rootfstype=hostfs rw root=/dev/root init="${WORK_DIR}/probe.sh" \
        mem="${MEM}" con0=fd:0,fd:1 quiet 2>&1 \
        | sed -n '/HYPERION-PROBE-BEGIN/,/HYPERION-PROBE-END/p'
}

cmd_systemd() {
    require_uml
    command -v debootstrap >/dev/null || die "debootstrap not found (needed to build the guest rootfs)"
    command -v mkfs.ext4 >/dev/null || die "mkfs.ext4 not found (install e2fsprogs)"

    if [ ! -d "${ROOTFS_DIR}" ]; then
        log "building minimal Debian rootfs with systemd (needs root via sudo)"
        mkdir -p "${ROOTFS_DIR}"
        sudo debootstrap --variant=minbase \
            --include=systemd,systemd-sysv,procps,util-linux,dbus \
            trixie "${ROOTFS_DIR}" http://deb.debian.org/debian
    else
        log "reusing existing rootfs at ${ROOTFS_DIR}"
    fi

    log "building ext4 disk image from rootfs (no loop device required)"
    rm -f "${IMAGE}"
    truncate -s 2G "${IMAGE}"
    sudo mkfs.ext4 -q -F -d "${ROOTFS_DIR}" "${IMAGE}"

    # Verify isolation: the guest must see OUR image, not the host root.
    cat > "${WORK_DIR}/identify.sh" <<'IDENT'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo "@@@ HYPERION-IDENT-BEGIN @@@"
echo "kernel:      $(uname -r)"
echo "arch:        $(uname -m)"
echo "root mount:  $(mount | grep ' / ' | head -1)"
echo "debian:      $(cat /etc/debian_version 2>/dev/null)"
if [ -d /workspace/project/hyperion-dbt/.uml-rootfs ]; then
  echo "isolation:   HOST ROOT VISIBLE (bad)"
else
  echo "isolation:   host root not visible (good)"
fi
echo "@@@ HYPERION-IDENT-END @@@"
poweroff -f
IDENT
    chmod +x "${WORK_DIR}/identify.sh"
    sudo debugfs -w -R "write ${WORK_DIR}/identify.sh /identify.sh" "${IMAGE}" >/dev/null 2>&1

    log "confirming guest isolation"
    TMPDIR="${WORK_DIR}" timeout "${TIMEOUT_SECS}" \
        "${UML_BIN}" ubd0="${IMAGE}" root=/dev/ubda rw init=/identify.sh \
        mem="${MEM}" con0=fd:0,fd:1 quiet 2>&1 \
        | sed -n '/HYPERION-IDENT-BEGIN/,/HYPERION-IDENT-END/p'

    log "booting systemd as PID 1 (isolated image, no hostfs)"
    TMPDIR="${WORK_DIR}" timeout "${TIMEOUT_SECS}" \
        "${UML_BIN}" ubd0="${IMAGE}" root=/dev/ubda rw init=/lib/systemd/systemd \
        mem="${MEM}" con0=fd:0,fd:1 > "${WORK_DIR}/systemd-boot.log" 2>&1

    log "systemd identification:"
    grep -aE "systemd\[1\]: (systemd .* running|Detected virtualization|Detected architecture)" \
        "${WORK_DIR}/systemd-boot.log" || log "  (no systemd identification lines found)"
    # systemd's console output embeds ANSI colour codes inside target names, so
    # strip them before matching or the patterns silently fail to match.
    log "targets reached:"
    sed 's/\x1b\[[0-9;]*m//g; s/\r//g' "${WORK_DIR}/systemd-boot.log" \
        | grep -E "Reached target (sysinit|basic|sockets|getty|multi-user|graphical)" \
        || log "  (no target lines found)"
    log "full log: ${WORK_DIR}/systemd-boot.log"
}

cmd_clean() {
    log "removing generated artifacts"
    sudo rm -rf "${ROOTFS_DIR}" "${WORK_DIR}"
}

case "${1:-}" in
    smoke)   cmd_smoke ;;
    systemd) cmd_systemd ;;
    clean)   cmd_clean ;;
    *)
        echo "usage: $0 {smoke|systemd|clean}" >&2
        exit 2
        ;;
esac

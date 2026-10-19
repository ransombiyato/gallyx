# UML feasibility experiment

Reproduces the measurements in
[`docs/architecture-feasibility.md`](../../docs/architecture-feasibility.md):
can a **real Linux kernel** run as an unprivileged user-space process, with no
root and no KVM, and reach a systemd multi-user boot?

This is **evidence gathering for an architecture decision**. It is not part of
the translator and contains no emulation code.

## Why this matters

The project's earlier plan treated kernel-level workloads (systemd, containers,
services) as out of scope. Before accepting that limit, we tested whether a
kernel-as-a-process design could lift it. The result is recorded in the
feasibility document.

## Quick start

```sh
# Fast: boot a kernel and prove guest execution (seconds)
experiments/uml/run.sh smoke

# Slow: build a Debian rootfs and boot systemd as PID 1 (minutes)
experiments/uml/run.sh systemd

# Remove generated artifacts (rootfs, image, logs)
experiments/uml/run.sh clean
```

## Requirements

- Debian/Ubuntu `user-mode-linux` package (provides `linux.uml`).
- `debootstrap` and `mkfs.ext4` (from `e2fsprogs`) for the `systemd` target.
- A `TMPDIR` on a filesystem where `PROT_EXEC` mmap works. **A tmpfs mounted
  `noexec` (common in containers) will fail**, with UML reporting
  `PROT_EXEC mmap in /dev/shm... Operation not permitted`.
- No root and no KVM are needed to *run* UML. `sudo` is used only to build the
  disk image.

## What was measured

See the table in
[`docs/architecture-feasibility.md`](../../docs/architecture-feasibility.md#5-measured-evidence-a-real-kernel-running-unprivileged-uml).
Headline result: systemd 257 started as PID 1 and reached `multi-user.target`
and `graphical.target`, running as uid 10001 with no root and no KVM, from an
isolated ext4 image.

## Limitations of this experiment

- Measured on an **x86-64 Linux host**, not the ARM64 Android target. An
  AArch64 UML port does exist (`zalexdev/linux-um-arm64`, `ARCH=um
  SUBARCH=arm64`), but it has **not** been built or run by this project and has
  **not** been tested on the reference device (Snapdragon 855 / Android 10).
  That remains the open question (R1/R2).
- **No performance was measured.** UML here is single-CPU and every syscall
  crosses a user-space kernel boundary.
- No real workloads were run; this proves a mechanism exists, not that a
  product is compatible.
- UML is a compatibility mechanism, **not a security boundary** — see
  [`SECURITY.md`](../../SECURITY.md) §4.1.

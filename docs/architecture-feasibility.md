# Architecture feasibility investigation

**Status: in progress.** This document evaluates how far HYPERION can go toward
running x86-64 Linux software on AArch64, including workloads that need
kernel-level functionality. It separates four responsibilities that are often
conflated, and it records what has been **measured** versus what is only
**reasoned about**.

Companion documents: [`ARCHITECTURE.md`](../ARCHITECTURE.md),
[`DECISIONS.md`](../DECISIONS.md), [`ROADMAP.md`](../ROADMAP.md).

---

## 1. Why this document exists

An earlier version of the project plan listed "system-mode or kernel emulation"
as an explicit non-goal. That was premature: it silently capped the project's
compatibility ceiling before anyone had measured what the target device can
actually do. This document replaces that assumption with an investigation.

The correction is deliberate but **does not** mean system-mode emulation is now
a supported feature. Nothing here is implemented. It is a scoping and risk
analysis.

---

## 2. Four responsibilities that must not be conflated

| Responsibility | What it means | What it cannot do alone |
| --- | --- | --- |
| **CPU instruction translation** | Convert x86-64 instructions to AArch64 instructions | Nothing about syscalls, processes, or files |
| **Linux syscall translation** | Translate guest syscalls into host syscalls | Cannot create kernel objects the host kernel refuses to create |
| **Kernel emulation** | Provide a kernel interface implemented in user space | Cannot provide a real kernel's process/VM/security semantics |
| **Full-machine emulation** | Emulate a CPU *and* run a real guest kernel on it | Very slow unless hardware virtualization exists |

A translator that does the first two well can run a large class of ordinary
programs. It cannot, by itself, provide a complete Linux system. Conversely,
full-machine emulation provides complete Linux semantics but pays a large
performance cost. The interesting engineering question is how to combine them.

---

## 3. Approaches evaluated

### Approach A — User-mode translation on the host kernel

The guest program runs as a normal AArch64 process; its x86-64 instructions are
translated, and its syscalls are translated and executed by the **host** kernel.

- **Can run x86-64 programs on AArch64:** yes, and this is the only approach
  with a demonstrated track record on Android (FEX-Emu, Box64, QEMU user-mode).
- **Complete Linux userspace / systemd as PID 1:** no. The host kernel is
  Android's. PID 1 is `init`; the guest has no authority over the system.
- **Syscall/ABI compatibility:** good and improvable, but bounded by what the
  host kernel exposes. Bionic vs glibc differences, missing or restricted
  syscalls, and Android's seccomp filter all matter.
- **Isolation, namespaces, mounts, networking:** constrained by the host
  kernel's policy (see §4).
- **Root/KVM:** not required.
- **Performance:** the best of the four, because guest code becomes host code
  and syscalls are mostly host syscalls.
- **Incremental:** yes — this is the natural first implementation.

**Verdict:** necessary foundation, insufficient alone for "complete Linux".

### Approach B — Full-machine (system) emulation

Emulate an x86-64 CPU and boot a real x86-64 Linux kernel inside it.

- **Can run x86-64 programs:** yes.
- **Complete Linux userspace / systemd as PID 1:** yes — the guest kernel is
  real, so semantics are complete.
- **Syscall/ABI compatibility:** excellent, because the guest kernel is a real
  Linux kernel.
- **Root/KVM:** no root needed. But **without KVM the CPU must be interpreted
  or translated in software**, which is the performance problem.
- **Performance:** with TCG-class translation, typically an order of magnitude
  slower than native (literature reports ~10×; some sources report more for
  cross-architecture). With KVM it is near-native — but **KVM is unavailable on
  a non-root Android device** and does not provide cross-architecture
  acceleration anyway (an x86-64 guest on an AArch64 host cannot use KVM).
- **Android constraints:** runs as an ordinary app process, so it inherits the
  same restrictions as Approach A, plus a boot cost and a fixed RAM footprint.

**Verdict:** gives complete Linux semantics at a large performance cost; not
viable as the *primary* fast path, but potentially viable as a *compatibility
fallback*.

### Approach C — Hybrid (recommended direction, unproven)

Fast user-mode translation for the common case, plus a mechanism that supplies
real kernel semantics for workloads that need them.

This is not one design but a family. The candidates, ordered by how much
evidence supports them:

1. **User-mode translation + a real Linux kernel running as a user-space
   process (UML).** See §5 — this is the approach with measured support in this
   investigation.
2. **User-mode translation + guest-kernel syscall interception**, where the
   translator feeds syscalls to a user-space kernel implementation rather than
   to the host kernel (the design QEMU uses for user-mode-with-a-kernel).
3. **Container-style isolation** (namespaces) for filesystem/process
   separation, layered under either of the above.

**Verdict:** the hybrid is where the project's stated goals actually live, and
it must be validated incrementally. §5 records the first measurement.

### Approach D — Other approaches

- **Native AArch64 recompilation** (AOT translation, e.g. Android's own x86
  binary translation in the ART era): fast, but not applicable to arbitrary
  pre-existing x86-64 binaries without a static translation step, and not a
  runtime compatibility mechanism.
- **Kernel port / device driver:** out of scope; requires root and a
  device-specific kernel, and is not a user-space technique.
- **Pure interpretation:** correct and simple, useful as an oracle (this is
  already the project's plan for Phase 2), but far too slow as a product.

---

## 4. Android host-kernel constraints (the real ceiling)

These bound Approaches A and C equally, and are the reason "just add more
translation" does not solve compatibility:

- **SELinux in enforcing mode** confines app processes by policy.
- **seccomp filters** are applied to app processes and deny syscalls the
  sandbox does not need. Some tools cannot run without disabling them.
- **Unprivileged user namespaces** may be disabled, which breaks `unshare`,
  `bwrap`, `proot`-style isolation, and rootless containers.
- **`ptrace`** is restricted, which affects debuggers and some isolation tools.
- **`/dev/kvm`** is not accessible, and would not accelerate cross-architecture
  guests in any case.
- **No root**: mounting filesystems, creating device nodes, changing kernel
  parameters, and binding privileged ports are unavailable.

**Consequence:** on the reference device, *any* approach runs as an ordinary
app. The question is which approach extracts the most capability within that
boundary. This must be measured on the device (see `target-environment.md`);
it cannot be assumed from a desktop Linux host.

---

## 5. Measured evidence: a real kernel running unprivileged (UML)

The strongest way to test the hybrid hypothesis is to run an actual Linux
kernel as an unprivileged user-space process. `user-mode-linux` (UML) is a
Linux port to its own syscall interface that does exactly this.

**Experiment:** run UML as an ordinary unprivileged user (uid 10001, no root,
no KVM), with a real Debian 13 root filesystem on an ext4 disk image, and try
to bring up systemd as PID 1.

**Result — measured on 2026-10-19:**

| Check | Measured result |
| --- | --- |
| Runs as unprivileged user (uid 10001) | **Yes** — no root, no KVM, no `/dev/kvm` |
| Guest kernel version | `6.12.101` (UML build) |
| Guest architecture | `x86_64` |
| Guest root device | `/dev/ubda on / type ext4` (our own image, **isolated** from the host) |
| Guest distro | Debian GNU/Linux 13.7 |
| Host root visible inside guest | **No** (verified isolation) |
| systemd as PID 1 | **Yes** — `systemd 257.13-1~deb13u1 running in system mode` |
| systemd detected virtualization | `uml`, architecture `x86-64` |
| Targets reached | `sysinit`, `basic`, `sockets`, `getty`, `multi-user`, `graphical` |
| Services started | `dbus.service`, `systemd-logind.service`, `systemd-user-sessions.service`, `getty@tty1.service` |
| Namespaces usable *inside* the guest | `unshare --user/--mount/--pid/--uts/--ipc/--net` all **OK** |
| Visible CPUs in guest | 1 |
| Memory | ~500 MB with `mem=512M` |

**Interpretation.** A real Linux kernel, with real kernel semantics and a real
systemd PID 1, can run on an unprivileged host with no KVM. This is direct
evidence that the "systemd as PID 1" and "kernel-dependent workload" goals are
*not* impossible on this class of target — they are achievable in a
kernel-as-a-process design, which is a hybrid, not a user-mode translator.

**Important caveats — do not overclaim:**

1. This was measured on an **x86-64 Linux host**, not on the ARM64 Android
   target. However, the decisive question — whether an AArch64 UML exists at
   all — has since been answered positively: see §5.1. What remains unverified
   is whether it runs on the *specific reference device* (Snapdragon 855,
   Android 10).
2. **Performance was not measured.** UML is single-CPU in this configuration
   and all syscalls cross a user-space kernel boundary. No performance claim is
   made here.
3. This proves *feasibility of a mechanism*, not compatibility of a product.
   Real workloads were not run.
4. The experiment ran on a host whose container had `PROT_EXEC` mmap available.
   The reference device must be measured separately.

### 5.1 AArch64 UML exists (desk research, not yet run on the target)

The blocking question was whether UML's `ARCH=um` has a usable AArch64 port.
UML was x86/x86-64-only for a long time, and as recently as 2022 the Termux
maintainers described an ARM port as "wontfix". That is no longer accurate.

`zalexdev/linux-um-arm64` provides `ARCH=um SUBARCH=arm64`: a Linux kernel
built to run as an ordinary **aarch64 user-space process**, explicitly aimed at
running "a real kernel without root, KVM or a hypervisor" on a phone.

**Verification performed** (repository inspection, 2026-10-19):

| Check | Result |
| --- | --- |
| Repository exists, not a fork, not archived | Yes; 90 stars, 22 forks |
| Contains real architecture code | Yes — `arch/arm64/um/` with `ptrace.c`, `signal.c`, `setjmp_aarch64.S`, `cpuinfo.c`, `os-Linux/`, `shared/` |
| Android-specific support | Yes — `bionic-syms.redef`; documented Android NDK (bionic) build |
| Last activity | 2026-10-17 (2 days before this inspection) |
| Base | Linux `7.2-rc4`, branch `um-arm64`, **38 commits on top** |
| License | GPL-2.0 (as the rest of the kernel) |

**Claims made by that project** (documented, not independently reproduced here):
boot to userspace on Alpine and Debian 12 at 4 KB and 16 KB guest pages;
`dockerd` with overlay2 on cgroup v2 and a working `docker run hello-world`;
userspace TCP/IP via `passt`; USB passthrough on real hardware; syscall
interception via `PTRACE_SYSEMU` with fallbacks for older host kernels.

**Tested devices reported by that project:** Snapdragon 870 / SM8250
("Poco F3", Android 15, kernel 4.19.246) and SM-S948B (Android 16, kernel
6.12.30). **Neither is the HYPERION reference device.**

**Why this matters and what it does not settle.**

- It removes the "no AArch64 UML exists" blocker. Kernel-as-a-process is a
  real, current option rather than a hypothesis.
- It is **out of tree** (38 commits on a kernel tree, not merged upstream).
  That is a maintenance risk for a project that depends on it.
- Snapdragon 855 / Android 10 is **older** than either tested device, and
  Android 10 is stricter than 15/16 in some sandbox respects. Compatibility
  must be **measured on the reference device**, exactly as the project
  directive requires.
- No performance number for the target device exists, and none is implied.

### 5.2 What this means for the recommendation

The kernel-as-a-process tier moves from "unproven idea, do not build" to
"plausible, with a concrete upstream to evaluate". It does **not** become the
primary path, and it is still not implemented. The sequencing in §8 is
unchanged, but R1's status changes from *blocking* to *de-risked, pending
target measurement*.

The reproduction procedure is in [`../experiments/uml/`](../experiments/uml/).

---

## 6. Comparison summary

| Criterion | A: user-mode | B: system-mode | C: hybrid | D: other |
| --- | --- | --- | --- | --- |
| Runs x86-64 programs | Yes | Yes | Yes | Partly |
| Complete Linux userspace | No | Yes | Yes (via kernel-as-process) | No |
| systemd as PID 1 | No | Yes | **Measured yes (UML, x86-64 host)** | No |
| Root / KVM required | No | No (but slow without KVM) | No | Varies |
| Performance potential | High | Low (software CPU) | High for fast path | Low |
| Works under Android sandbox | Constrained | Constrained | Constrained | No |
| Incremental to build | Yes | Yes | Yes | — |

No performance numbers are quoted for the target device because none have been
measured there.

---

## 7. Major risks and open questions

| ID | Risk / question | Why it matters | How to resolve |
| --- | --- | --- | --- |
| R1 | **Status: de-risked.** An AArch64 UML exists (`zalexdev/linux-um-arm64`, `ARCH=um SUBARCH=arm64`), verified to contain real arch code and Android/bionic support. It is **out of tree** and **untested on the reference device** (Snapdragon 855 / Android 10). | If it does not run there, the kernel-as-process tier is unavailable | Build and run it on the reference device; measure boot and syscall cost |
| R2 | Android 10's seccomp/SELinux may block the `ptrace` and mmap behaviour UML requires | Could make kernel-as-process impossible without root | Measure on the reference device; the port documents `PTRACE_SYSEMU` fallbacks for hosts that lack it |
| R3 | Single-CPU limitation of UML | Caps multi-threaded workload performance | Confirmed uniprocessor by the port itself (`CONFIG_CPUSETS` depends on `SMP`); treat as a fixed limit, not a bug |
| R4 | Memory-model emulation cost (x86 TSO on AArch64) | Largest single performance tax in user-mode translation | Phase 5/7 experiments |
| R5 | Syscall-layer breadth (Bionic vs glibc, Android-specific restrictions) | Determines real compatibility, not instruction coverage | Phase 6 feature matrix |
| R6 | Two engines (fast user-mode + kernel-as-process) is a large surface | Risk of neither being finished well | Stage strictly; keep user-mode path independent and shippable |
| R7 | Out-of-tree dependency: the AArch64 UML port is 38 commits ahead of a kernel tree and unmerged | A compatibility tier resting on an unmerged fork is a maintenance liability | Track upstream progress; keep the tier optional and isolated behind an interface |

---

## 8. Recommended architecture direction

1. **Proceed with the user-mode translator as the primary path** (Phases 2–5).
   It is the only path with demonstrated Android feasibility and the only one
   with a high performance ceiling.
2. **Keep the syscall layer cleanly separated** (Phase 6) so that the *same*
   translator can later direct syscalls either to the host kernel or to a
   user-space kernel. This is a design constraint, not a feature.
3. **Treat the kernel-as-process path as a compatibility tier.** It is now
   plausible: an AArch64 UML exists (§5.1). Validate it on the reference device
   before committing to it, and keep it behind an interface so its absence
   costs nothing (R7).
4. **Do not claim systemd, containers, or full Linux compatibility** until
   measured on the reference device. The x86-64 UML result and the AArch64 port
   are evidence of feasibility, not of target support.

---

## 9. What is still unfinished

- R1 is **de-risked but not closed**: the AArch64 UML port has not been built
  or run on the reference device, nor by this project at all. Building it
  cross-arch is a concrete next step.
- No measurements exist from the reference device for any approach.
- The performance cost of UML's syscall boundary is unmeasured.
- The syscall-layer abstraction that would let one translator serve both paths
  has not been designed in detail.
- The licensing and maintenance implications of depending on an unmerged
  kernel branch (R7) have not been assessed.

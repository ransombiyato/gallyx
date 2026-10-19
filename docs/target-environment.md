# Target environment

The reference target and the development/test hosts. Because they differ,
**compatibility is measured, not assumed**.

---

## 1. Reference target device

| Property | Value |
| --- | --- |
| SoC | Qualcomm Snapdragon 855 (Kryo 485) |
| CPU | 4× Cortex-A76 + 4× Cortex-A55, ARMv8.2-A, 64-bit |
| ISA extensions | NEON (ASIMD); no SVE |
| OS | Android 10 (API 29), ARM64 |
| RAM | 8 GB |
| Root | **No** |
| KVM | **Not available** (Android, non-root) |
| Userland | Termux/PRoot-style environment expected |

**Implications for the design:**

- The translator must run **unprivileged** and must not require KVM.
- W^X is enforced; code must be written then made executable (D-010).
- Page size is commonly 4 KiB on this class of device but must be **probed**,
  not assumed (D-008).
- Android's Bionic libc and linker behaviour differ from glibc; the syscall
  layer (Phase 6) must account for this.
- 8 GB RAM and a mobile thermal envelope mean resource bounds matter (R5).

**Status:** no measurements from this device are available in the current
environment (BLOCK-001). Nothing about target performance may be claimed until
measured on real hardware.

---

## 2. Development / CI host (current)

Measured by `hyperion probe` on 2026-10-19:

| Property | Value |
| --- | --- |
| Architecture | **x86-64** |
| CPU | AMD EPYC 9B14 (KVM guest), 4 logical CPUs |
| OS | Debian GNU/Linux 13 (trixie) |
| Kernel | 6.8.0-1055-gke |
| RAM | 15 GiB |
| Page size | 4096 bytes |
| Cache line | 64 bytes |
| Transparent hugepages | enabled |
| W^X assumption | no (x86-64) |

**Implication:** AArch64 artifacts cannot be executed natively here. All
AArch64 validation uses the cross + `qemu-user` pipeline (D-007), which proves
correctness only.

---

## 3. Host matrix (intended)

| Role | Architecture | Native AArch64 execution | Purpose |
| --- | --- | --- | --- |
| Development | x86-64 | No (use qemu) | Fast iteration, cross + qemu validation |
| CI | x86-64 | No (use qemu) | Enforce build/tests/cross-tests |
| AArch64 Linux host | AArch64 | Yes | Native correctness + first real performance numbers |
| Reference target | AArch64 (Android 10) | Yes | Final compatibility and performance evidence |

---

## 4. How to record a new environment

Run `hyperion probe` and `hyperion_bench --json` on the machine and paste the
measured output below with the date and commit SHA. Never transcribe a host
fingerprint from memory or from another machine.

```
### <machine name>
- Date / commit:
- Output of `hyperion probe`:
- Output of `hyperion_bench --json`:
```

---

## 5. Compatibility claim policy

- Correctness on x86-64 + `qemu-user` **does not** imply correctness on the
  target device.
- Performance on any host **does not** transfer to another host.
- A claim about the reference target requires measurements taken **on the
  reference target**, with the command and commit recorded.

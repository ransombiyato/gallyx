# Known issues

Bugs, limitations and blocked work. Newest first within each section. An entry
is removed only when the issue is genuinely resolved, and the resolution is
noted in `CHANGELOG.md`.

---

## Blocked / cannot proceed without external input

| ID | Issue | Needs |
| --- | --- | --- |
| BLOCK-001 | No measurements from the reference target device (Snapdragon 855, Android 10). | Physical access to the device, or a comparable AArch64 Android host. Until then, all target-device claims are unverified. |
| BLOCK-002 | Development host is x86-64, so AArch64 code cannot be executed natively. | Mitigated by the cross + `qemu-user` pipeline (D-007). True AArch64 execution requires AArch64 hardware. |
| BLOCK-003 | The kernel-as-process tier cannot be committed to until the AArch64 UML port is run on the reference device. | AArch64 Android hardware. Until then the tier stays provisional (D-012) and no kernel-level code is written. |
| BLOCK-004 | The environment does not persist between sessions: a fresh container has no toolchain even if `build/` survives. | Run `scripts/bootstrap.sh`. Do not treat stale artifacts as evidence. See `STATUS.md` §2. |

---

## Limitations (by design, current phase)

| ID | Limitation | Notes |
| --- | --- | --- |
| LIM-001 | **No instruction decoding, interpretation or translation.** | Phase 0 is foundation only. Nothing can execute a guest program. |
| LIM-002 | No syscall or ABI layer. | Phase 6. |
| LIM-003 | No translation cache or code generator. | Phases 3–4. |
| LIM-004 | `qemu-user` validates correctness, not performance. | Never quote qemu numbers as performance. |
| LIM-005 | Benchmark harness measures host only. | `guest_execution.available = false` until the translator exists (D-009). |
| LIM-006 | No Windows/macOS guest support. | Out of scope, permanently. |
| LIM-007 | Target device has no KVM and no root; the design must not assume either. | Enforced as a project rule. |
| LIM-008 | **Kernel-level compatibility is investigated, not implemented.** | The user-mode translator cannot provide `systemd` as PID 1, containers, or kernel-dependent services. The kernel-as-process tier is provisional and unvalidated on the target (D-011, D-012). |
| LIM-009 | AArch64 UML is an **out-of-tree** dependency (38 unmerged commits). | If the tier is adopted, it carries a maintenance liability (R7 in `docs/architecture-feasibility.md`). |

---

## Correctness bugs

_None — no execution engine exists yet._

When a bug is found, record: ID, symptom, minimal reproducer, affected commit,
severity (correctness/performance/cosmetic), status, and the test that will
guard against regression.

---

## Toolchain and environment issues

| ID | Issue | Status |
| --- | --- | --- |
| ENV-001 | `g++-aarch64-linux-gnu` is required in addition to `gcc-aarch64-linux-gnu`; installing only the C cross-compiler breaks the cross test with "CXX compiler not found". | Resolved in `scripts/bootstrap.sh`, which installs both. |
| ENV-002 | ASan interacts with custom executable code caches. | Not yet encountered (no code cache). Expected in Phase 3; document per-test exclusions when they arise. |

---

## Open questions

| ID | Question | Impact |
| --- | --- | --- |
| Q-001 | Which upstream components, if any, should be reused rather than built? | Phase 1 decides; affects everything downstream. |
| Q-002 | Custom IR versus direct translation for the first code generator? | Phase 1/2; see D-004 for the current lean. |
| Q-003 | How to model x86 TSO memory ordering on weakly ordered AArch64? | Phase 7; affects correctness of multithreaded guests. |

# BENCHMARKS

Reproducible methodology and measured results.

> **Honesty rule (D-009):** a benchmark report may never contain a fabricated
> number. Metrics that cannot yet be measured are reported as `unavailable`
> with a reason. Every result is attributable to a host, compiler, build
> configuration and methodology.

---

## 1. Why a benchmark harness exists in Phase 0

Measurement infrastructure must exist and be trusted *before* there is anything
to optimize. The Phase 0 harness establishes the reporting format, records the
environment fingerprint, and guards against broken builds. It measures **host**
and **harness** characteristics only.

---

## 2. Environment fingerprint

Every run records and reports:

| Field | Source | Why it matters |
| --- | --- | --- |
| architecture | `host_probe` | x86-64 vs AArch64 changes everything |
| kernel release | `uname` | affects syscall/feature availability |
| logical CPUs | `hardware_concurrency` | affects threading benchmarks |
| page size | `sysconf(_SC_PAGESIZE)` | affects code-cache and mmap design |
| cache-line size | `sysconf(_SC_LEVEL1_DCACHE_LINESIZE)` | affects code alignment |
| build identity | `build_summary()` | compiler + build type affect codegen |
| commit SHA | recorded by the runner | ties results to source |

The machine-readable form is the `host` and `build` objects in the JSON output.

---

## 3. Harness design

`benchmarks/host_bench.cpp`:

1. Probes the host.
2. Runs a **deterministic** integer calibration workload (`xorshift`-style
   mixing) with a known checksum, so a benchmark cannot silently measure a
   broken build.
3. Runs it `--repeats` times, **discards one warm-up sample**, and reports
   min/p25/median/p75/max/mean/stddev plus `iqr_fraction` (the noise indicator).
4. Guards against the optimiser eliminating the timed work: an `asm` barrier
   keeps the computation between the clock reads, and a **plausibility check**
   rejects a result faster than one clock cycle per iteration rather than
   reporting it. See BENCH-001 in `docs/known-issues.md`.
5. Exits non-zero if the checksum is unstable or the timing is implausible, so
   a broken measurement fails the build instead of being published.
6. Emits `guest_execution: { "available": false, "reason": ... }` until the
   translator exists.

Run it:

```bash
scripts/dev.sh bench --iterations 1000000 --repeats 9 --json
```

### JSON schema

```json
{
  "schema": "hyperion.bench.host.v2",
  "build": "hyperion 0.1.0 [release] gcc 14.2.0 c++202002",
  "host": { "arch": "...", "kernel": "...", "logical_cpus": 0,
            "page_size": 0, "cache_line_size": 0 },
  "calibration": {
    "iterations": 0, "repeats_requested": 0, "warmup_discarded": 1,
    "samples": 0,
    "ns_per_op_min": 0.0, "ns_per_op_p25": 0.0, "ns_per_op_median": 0.0,
    "ns_per_op_p75": 0.0, "ns_per_op_max": 0.0, "ns_per_op_mean": 0.0,
    "ns_per_op_stddev": 0.0, "iqr_fraction": 0.0,
    "checksum": "0x...", "checksum_stable": true,
    "measurement_plausible": true
  },
  "guest_execution": { "available": false, "reason": "..." }
}
```

The schema version is incremented on any breaking change to the JSON shape.
`v1` reported a single sample and no spread; `v2` reports the repeated-run
statistics the methodology requires.

---

## 4. Measured results

### 4.1 Host calibration — this development environment

**Measured 2026-10-19.** Host: AMD EPYC 9B14 (x86-64, KVM guest), Debian 13
(trixie), kernel 6.8.0-1055-gke, 4 logical CPUs, page size 4096 B, cache line
64 B. Build: `hyperion 0.1.0 [release] gcc 14.2.0`, RelWithDebInfo, `-O2`.

Command:

```bash
build/dev/benchmarks/hyperion_bench --iterations 1000000 --repeats 9 --json
```

| Statistic | ns/op |
| --- | --- |
| min | 2.055060 |
| p25 | 2.056132 |
| **median** | **2.056794** |
| p75 | 2.079814 |
| max | 2.138529 |
| mean | 2.075143 |
| stddev | 0.030362 |

`samples: 8` (9 requested, 1 warm-up discarded) · `iqr_fraction: 0.0115` ·
`checksum: 0x2d288a32c31afada` (stable) · `measurement_plausible: true`.

The spread is **1.2 %** of the median, so this host is quiet enough for
candidate-vs-baseline comparison at the granularity this project needs.

#### Scaling validation

A calibration figure is only trustworthy if it scales with the work requested.
A workload that is too fast for the iteration count, or that the optimiser
eliminated, would produce a constant total time. Measured:

| Iterations | Median ns/op | Total (s) |
| --- | --- | --- |
| 1,000,000 | 2.0633 | 0.002063 |
| 10,000,000 | 2.0575 | 0.020575 |
| 100,000,000 | 2.0573 | 0.205731 |

Total time scales linearly while ns/op stays flat, as it must for a real
measurement. This check is what exposed **BENCH-001** (see
`docs/known-issues.md`), where an earlier revision of this harness reported a
fabricated ~0.00003 ns/op because the optimiser hoisted the timed workload out
of the repeat loop. The regression is now guarded by the
`bench.timing_not_eliminated` test.

These are **calibration figures for the harness on a virtualized host**, not a
performance claim about HYPERION. They say nothing about translation throughput;
that is measured separately in 4.2.

### 4.2 Guest execution — Tier 0 vs Tier 1 baseline (Phase 5)

The translator exists now, so the guest-execution rows are measurable. The first
Phase 5 task was to record a **baseline before optimising**, exactly as the
roadmap requires.

**Harness.** `benchmarks/tier_bench.cpp` (target `hyperion_tier_bench`). It runs
one fixed guest program through both tiers with the shared methodology: one
discarded warm-up, repeated timed samples, median with IQR spread, and a
correctness check on the final `rdx` so a benchmark cannot measure a broken
translator.

**Workload.** `accumulate rax into rdx rcx times, halt`
(`31 D2 | 48 01 C2 | 48 FF C9 | 75 F8 | F4`), `rax=1`, `rcx=iterations`. Each
iteration retires 3 guest instructions (`add`, `dec`, `jnz`); the numbers below
use 200,000 iterations = 600,002 retired instructions.

**Environment.**

| Field | Value |
| --- | --- |
| Host arch | aarch64 (executed under `qemu-aarch64`) |
| Kernel | 6.8.0-1055-gke |
| Logical CPUs | 4 |
| Page size / cache line | 4096 / 32 bytes |
| Build | RelWithDebInfo, gcc, `HYPERION_BUILD_BENCHMARKS=ON` |
| Command | `scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7` |
| Commit | Phase 5 baseline (this commit) |

**Result (median of 7 samples, 1 warm-up discarded):**

| Tier | ns median | ns / guest instruction | IQR / median | Correct |
| --- | --- | --- | --- | --- |
| Tier 0 (interpreter) | 295,117,038 | 491.86 | 0.018 | yes |
| Tier 1 (translator) | 23,041,650 | 38.40 | 0.008 | yes |

Ratio (Tier 1 / Tier 0): **0.078**, i.e. Tier 1 is about **12.8× faster** than
the reference interpreter on this workload.

**Interpretation, and what this does *not* say.**

- This is the ratio between *Hyperion's own two tiers*. It is a relative sanity
  signal: it confirms the translator's design (one translation reused across a
  hot loop, versus per-instruction interpretation) actually pays off.
- It is **not** a hardware performance claim. Under `qemu-user` both tiers are
  additionally translated by qemu, so absolute ns are meaningless and the ratio
  is distorted by qemu's treatment of the two very different code shapes.
- The reference target (Snapdragon 855 / Android 10) is still **unavailable**;
  see 4.3. Do not assume target performance from these numbers.
- Tier 1 is still very slow in absolute terms: no register allocation and no
  block chaining, so each guest instruction becomes a load/compute/store of
  `GuestState` and every block exit returns to a hash lookup. That is the
  starting point Phase 5 optimises from.

**Reproduce:**

```
scripts/bench-tier-qemu.sh --iterations 200000 --repeats 7
```

#### 4.2.1 Optimization 1 — parity fast path (Phase 5)

**Hypothesis.** Each guest ALU instruction that writes flags emits ~39 AArch64
instructions, of which ~19 are the parity (PF) computation: a per-bit fold plus
its setup. Replacing the bit-by-bit accumulation with a XOR-fold (8→4→2→1 bits)
using AArch64 shifted-register `EOR` cuts that block to 6 emitted instructions.

**Change.** `emit_parity` in `src/core/translator.cpp`; new encoder primitive
`Encoder::eor_shifted`. Encodings verified against `aarch64-linux-gnu-as`
(all four shift types) before use — the first attempt wrongly set bit 21, which
the new encoder test caught.

**Evidence.** Timing under `qemu-user` is noisy and the absolute ns drift
between runs, so the primary evidence is *deterministic*: the generated-code
size for the fixed workload, measured with `TranslationCache::total_code_bytes`.

| Metric | Baseline (`f04c659`) | After parity fast path | Change |
| --- | --- | --- | --- |
| Generated AArch64 bytes (same workload) | 904 | 644 | **−28.8%** |
| Tier 1 ns / guest instruction (interleaved A/B) | 38.20 | 31.50 | −17.5% |
| Tier 1 / Tier 0 ratio (interleaved A/B) | 0.0817 | 0.0733 | −10.3% |

The A/B is *interleaved*: the baseline and current binaries were built from the
same tree state (both `RelWithDebInfo`, `-O2`) and run alternately five times,
so machine drift cancels. Both tiers are correct in every run.

**The delta is fully explained, which is the strongest evidence here.** The
workload retires five flags-writing guest instructions per loop trip (`xor`,
`add`, `dec`, `add`, `dec`). Each parity rewrite removes exactly 13 AArch64
words (19 → 6). Five × 13 = 65 words = 260 bytes, which is precisely the
measured 904 − 644 delta. No other part of the code generator changed size, so
the optimization does exactly what it claims and nothing else.

**Interpretation, and limits.**

- The deterministic −28.8% code size is the trustworthy number. The 17.5%
  wall-clock gain is consistent with it but should not be over-read: the whole
  workload is dominated by `GuestState` load/store, so shrinking the flags
  prologue helps less than the instruction count alone suggests.
- This does **not** change the conclusion of 4.2: Tier 1 is still ~13× the
  interpreter in this workload and still very slow in absolute terms. The next
  structural wins (register allocation, block chaining) are untouched.

**Regression guard.** `translator.translated_code_size_is_bounded` pins the
block count and an upper bound on generated words for a fixed loop, so a future
code-generator change that silently bloats output fails CI even when timing
noise would hide it. The exact byte count is reported by the benchmark
(`tier1_translator.generated_code_bytes`).

**Reproduce:** `scripts/bench-tier-qemu.sh --iterations 200000 --repeats 15`.

### 4.3 Reference target device (Snapdragon 855)

**unavailable.** The reference device is not present in this environment.
Target-device measurements require real hardware and will be recorded in
`docs/target-environment.md` and here when available. Do not assume target
performance from x86-64 host numbers.

---

## 5. Methodology for future phases

1. **Fixed baseline.** Optimization candidates are compared against a frozen,
   committed baseline build; the baseline is never silently changed (Phase 8).
2. **Repeated runs.** Report median and spread, not a single sample; discard a
   warm-up run.
3. **Pinned environment.** Record CPU governor/affinity where controllable;
   note when it is not.
4. **Correctness gate.** A faster result is void unless the correctness suite
   passes on the same build.
5. **No cross-machine comparison.** qemu-user numbers are correctness evidence
   only; performance comparisons require the same physical machine.
6. **Attribution.** Every row links to the commit SHA and the JSON report.

---

## 6. Reporting template

```
### <benchmark name>
- Date / commit:
- Host (arch, kernel, CPU, RAM, page size, cache line):
- Compiler / build type / flags:
- Command:
- Baseline result:
- Candidate result:
- Correctness: <pass/fail, test names>
- Verdict: accept | reject | inconclusive
- Notes:
```

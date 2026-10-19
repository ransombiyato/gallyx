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
3. Reports elapsed time and nanoseconds per operation.
4. Emits `guest_execution: { "available": false, "reason": ... }` until the
   translator exists.

Run it:

```bash
scripts/dev.sh bench --iterations 100000 --json
```

### JSON schema

```json
{
  "schema": "hyperion.bench.host.v1",
  "build": "hyperion 0.1.0 [release] gcc 14.2.0 c++202002",
  "host": { "arch": "...", "kernel": "...", "logical_cpus": 0,
            "page_size": 0, "cache_line_size": 0 },
  "calibration": { "iterations": 0, "seconds": 0.0, "ns_per_op": 0.0,
                   "checksum": "0x..." },
  "guest_execution": { "available": false, "reason": "..." }
}
```

The schema version is incremented on any breaking change to the JSON shape.

---

## 4. Measured results

### 4.1 Host calibration — this development environment

**Measured 2026-10-19.** Host: AMD EPYC 9B14 (x86-64, KVM guest), Debian 13
(trixie), kernel 6.8.0-1055-gke, 4 logical CPUs, 15 GiB RAM, page size 4096 B,
cache line 64 B. Build: `hyperion 0.1.0 [release] gcc 14.2.0`, RelWithDebInfo,
`-O2`.

| Iterations | Elapsed (s) | ns/op | Checksum |
| --- | --- | --- | --- |
| 100,000 | 0.000209790 | 2.0979 | `0x9c29aa3c8d1b6423` |

These are **single-run calibration figures**, not a statistical benchmark, and
they measure the harness on a virtualized CI-class host. They are recorded to
prove the pipeline produces attributable output, not as a performance claim.

### 4.2 Guest execution

| Metric | Result |
| --- | --- |
| Translation throughput | **unavailable** — translator not implemented (Phase 3+) |
| Guest program runtime | **unavailable** — translator not implemented |
| Instruction cache hit rate | **unavailable** — no code cache (Phase 4) |
| Comparison vs FEX/Box64/QEMU | **unavailable** — Phase 1 baseline work |

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

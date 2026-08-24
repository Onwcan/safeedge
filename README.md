# safeedge

Failsafe real-time runtime components for industrial edge devices, in C++20.

A runtime that executes motion commands under a safety supervisor, on Linux, in
containers, with the deadline behaviour measured rather than assumed.

[![CI](https://github.com/Onwcan/safeedge/actions/workflows/ci.yml/badge.svg)](https://github.com/Onwcan/safeedge/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License](https://img.shields.io/badge/license-Apache--2.0-green)

---

## Status

| Work package | Component | State |
|---|---|---|
| WP-08a | `concurrent` — wait-free SPSC ring | **Done** |
| WP-08b | `rt` — deterministic cyclic executor | Next |
| WP-09 | `safety` — IEC 61800-5-2 supervisor, black-channel telegram | Planned |
| WP-10 | `ipc` — zero-copy shared-memory transport | Planned |
| WP-11 | Edge app packaging, observability | Planned |

14 tests, all passing, **including under ThreadSanitizer** — which is the only
result that means anything for a lock-free structure.

---

## Build

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan`, `tsan`, `tidy`. Run the benchmark with:

```bash
cmake --build --preset release --target run_benchmarks
```

---

## `concurrent::SpscRing`

A wait-free single-producer/single-consumer ring for the boundary between the
real-time control thread and everything without a deadline — logging, metrics,
telemetry, diagnostics.

```cpp
#include "safeedge/concurrent/spsc_ring.hpp"

struct Setpoint { std::uint64_t cycle; double joint[6]; };

safeedge::concurrent::SpscRing<Setpoint, 1024> telemetry;

// Real-time thread. Never blocks, never allocates, never enters the kernel.
if (!telemetry.tryPush(current)) {
    ++dropped_samples;   // backpressure is the caller's decision, by design
}

// Service thread.
Setpoint sample;
while (telemetry.tryPop(sample)) { publish(sample); }
```

**Wait-free**, not merely lock-free: bounded instruction count per operation, no
locks, no allocation, no syscalls, no retry loop. That is what makes it legal on
a path where a mutex could invert priorities and an allocation could reach the
kernel. Reasoning in [ADR-0001](docs/adr/0001-wait-free-spsc-ring-for-the-rt-boundary.md);
the memory-ordering pairing is derived line by line in
[ADR-0002](docs/adr/0002-memory-ordering-policy.md).

Exactly one producer thread and one other consumer thread. Two of either is
undefined behaviour — the contract enforces it, not the type system.

---

## Measurements

From `benchmarks/bench_spsc_ring.cpp`, median of 7 repetitions, 24-core host,
CPU pinning verified.

| Configuration | median | min | max |
|---|---:|---:|---:|
| Single thread, push+pop (cache hot) | 1471.6 | 715.2 | 1666.8 |
| Cross-thread, padded + cached positions | **353.9** | 309.0 | 413.4 |
| Cross-thread, shared cache line, no caching | 224.8 | 153.0 | 264.5 |

*M items/s. Layout advantage at the median: **1.57x** (worst repetition 1.17x,
best 2.70x).*

Both rows run the same algorithm and the same memory ordering. The only
differences are the cache-line padding and each side caching the other's
position. That is the entire performance argument for the class, so it is
measured against a control implementation in the same binary rather than
asserted.

### What these numbers are not

This was measured under WSL2. **Everything above roughly p99 characterises the
hypervisor scheduler, not this code**, and is not quoted here as a property of
the queue — handoff p50 lands near 274 ns while p99.9 reaches ~3 ms, and that
tail is the platform descheduling the measuring threads. A defensible tail
figure needs bare-metal Linux with isolated cores, which is what
[`rt-latency-lab`](https://github.com/Onwcan/rt-latency-lab) exists to provide.

The benchmark reports this itself. It detects a virtualised host, verifies
whether CPU pinning actually took effect rather than assuming it, prints the
cost of `steady_clock::now()` so you can see which figures are signal, and
reports min and max alongside every median. An earlier single-shot version of
this benchmark reported the layout advantage as anywhere from 1.19x to 5.27x
depending on which run you kept — which is exactly why it now runs seven times
and shows the spread.

---

## What CI enforces

| Gate | Why |
|---|---|
| GCC + Clang × Debug + Release, `-Werror` | `-Wconversion` and `-Wold-style-cast` fire on different constructs per compiler |
| **ThreadSanitizer** | The only gate that actually validates ADR-0002. TSan models the C++ abstract machine, so it catches ordering bugs that x86-64's strong memory model would hide |
| ASan + UBSan, `-fno-sanitize-recover=all` | A finding fails the build rather than printing a note |
| clang-tidy `--warnings-as-errors=*` | Rule set justified per-exclusion |
| clang-format `--dry-run --Werror` | Formatting never reaches review |
| install + downstream consumer compile | The public CMake contract is tested, not assumed |

### Known gap

The memory ordering is validated on x86-64 only. x86-64 has a strong memory
model that will hide a genuine ordering bug that AArch64 would expose — and
AArch64 is where industrial edge devices increasingly are. Running the
concurrency suite on real AArch64 hardware is outstanding, and it is recorded
as a gap in ADR-0002 rather than left unsaid.

---

## Design principles

1. **Nothing on the deadline path may block, allocate, or enter the kernel.**
   Every component is judged against this before anything else.
2. **Measure, then claim.** No performance statement here is unaccompanied by
   the code that produced it and the conditions it was taken under.
3. **Constrain the problem rather than generalise the solution.** The runtime
   is genuinely SPSC, so the queue is SPSC — which removes the CAS loop, the
   ABA hazard and the reclamation problem outright. Less subtlety underneath a
   safety function is a feature.
4. **A decision without a written alternative is not a decision.** Every ADR
   here names what was rejected and why.

---

## Licence

Apache-2.0. Portfolio work — every design decision here is one I can defend in
review.

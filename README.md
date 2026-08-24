# safeedge

Failsafe real-time runtime components for industrial edge devices, in C++20.

A runtime that executes motion commands under a safety supervisor, on Linux, in
containers — with the deadline behaviour measured rather than assumed, and the
no-allocation rule enforced rather than documented.

[![CI](https://github.com/Onwcan/safeedge/actions/workflows/ci.yml/badge.svg)](https://github.com/Onwcan/safeedge/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License](https://img.shields.io/badge/license-Apache--2.0-green)

---

## Status

| Work package | Component | State |
|---|---|---|
| WP-08a | `concurrent` — wait-free SPSC ring | **Done** |
| WP-08b | `rt` — cyclic executor, latency histogram, allocation guard | **Done** |
| WP-09 | `safety` — IEC 61800-5-2 supervisor, black-channel telegram | Next |
| WP-10 | `ipc` — zero-copy shared-memory transport | Planned |
| WP-11 | Edge app packaging, observability | Planned |

**67 tests**, all passing under Debug, Release, ASan+UBSan and ThreadSanitizer.

---

## Build

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan`, `tsan`, `tidy`.

Before pushing, run the formatter -- CI enforces it:

```bash
scripts/format.sh
```

It pins clang-format 18; a different major version formats differently and CI
will reject the result.

```bash
cmake --build --preset release --target rt_cycle_report
./build/release/examples/rt_cycle_report 1000 5
```

---

## What is here

### `concurrent::SpscRing`

A wait-free single-producer/single-consumer ring for the boundary between the
real-time control thread and everything without a deadline. Bounded instruction
count, no locks, no allocation, no syscalls, no retry loop. Cache-line padded,
with each side caching the other's position.

Memory ordering is acquire/release, derived edge by edge in
[ADR-0002](docs/adr/0002-memory-ordering-policy.md) and validated by
ThreadSanitizer — x86-64 would hide an incorrect pairing.

### `rt::CyclicExecutor`

A fixed-period loop with **absolute-deadline** scheduling. Each deadline comes
from the previous deadline, never from the current time, and the wait is
`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`.

```cpp
CyclicExecutor::Config config;
config.period            = 1ms;
config.thread.policy     = SchedulingPolicy::kFifo;
config.thread.priority   = 80;
config.thread.cpu        = 2;
config.thread.lock_memory = true;

CyclicExecutor executor(config);
const ThreadConfigReport report = executor.configureCallingThread();
if (!report.scheduling_applied) {
    // Do not just carry on. Latency figures gathered without the scheduling
    // you asked for describe the machine's mood, not your code.
    log(report.what());
}

auto control_step = [&](const CycleContext& ctx) { plant.update(ctx); };
executor.run(CycleCallback(control_step));
```

Why this matters is measurable rather than theoretical. Median wakeup delay on
the development host at 1 kHz is **78 µs**. The naive `sleep_for(period)` loop
adds that to the schedule permanently — it would run at ~928 Hz and fall 4.7
seconds behind after one minute, with every individual cycle looking fine.
Absolute deadlines turn that into jitter, which is a measurement, instead of
drift, which is a defect. [ADR-0003](docs/adr/0003-absolute-deadline-scheduling.md).

`CycleCallback` is two pointers, not a `std::function` — a type that *can*
allocate has no business in the signature of a real-time API.

### `rt::NoAllocScope` — the rule, enforced

Linking `safeedge::rt_alloc_guard` replaces the global `operator new` family.
An allocation inside a marked scope aborts, or is counted, by policy.

```cpp
{
    const NoAllocScope no_alloc;
    controller.step();   // aborts if anything in here reaches the allocator
}
```

This exists because the allocation is almost never visible at the call site: a
`std::function` assignment whose capture outgrew the small-object buffer, a
`push_back` that allocates only on the growth step, a `std::string` built on an
error path. Those pass review, pass tests, and fail in the field under load.

It has already earned its place — it is what proves the claims other components
make. `SpscRing` says it never allocates; that is now a passing test rather than
a sentence in an ADR. [ADR-0004](docs/adr/0004-enforcing-the-no-allocation-rule.md).

**Known limit, stated plainly:** only `operator new` is intercepted. A direct
`malloc` from linked-in C code passes through unseen.

### `rt::LatencyHistogram`

Fixed-size, allocation-free, logarithmic bucketing at constant ~3.1% relative
precision. Percentiles are reported as the **upper** bound of the containing
bucket, so a reported p99.9 is a value the true p99.9 cannot exceed — for a
latency budget, erring high is the safe direction.

---

## Measurements

### SPSC ring throughput

Median of 7 repetitions, CPU pinning verified.

| Configuration | median | min | max |
|---|---:|---:|---:|
| Single thread, push+pop (cache hot) | 1471.6 | 715.2 | 1666.8 |
| Cross-thread, padded + cached positions | **353.9** | 309.0 | 413.4 |
| Cross-thread, shared cache line, no caching | 224.8 | 153.0 | 264.5 |

*M items/s. Layout advantage 1.57x at the median.* Both rows run the same
algorithm and the same memory ordering; the only difference is cache-line
layout. That is the entire performance argument for the class, so it is measured
against a control implementation in the same binary rather than asserted.

### Cyclic executor at 1 kHz

Output of `rt_cycle_report 1000 5` on the development host. **Read the
conditions block before the numbers** — that is why the program prints it first.

```
- Scheduling requested: SCHED_FIFO priority 80
- Scheduling obtained:  SCHED_OTHER priority 0  -> DENIED
- CPU pinning:          verified
- Memory locked:        yes (mlockall)
- Allocation guard:     installed
- Virtualised host:     YES
- Diagnostics:          pthread_setschedparam failed (Operation not permitted)

| Measurement (ns)       |     min |     p50 |      p99 |     p99.9 |        max |
|------------------------|---------|---------|----------|-----------|------------|
| wakeup jitter          |   58802 |   77823 |   135167 |    499711 |   38618434 |
| callback execution     |      36 |     103 |      431 |       639 |      18484 |
| deadline to complete   |   58840 |   77823 |   135167 |    499711 |   38619194 |

- Cycles executed:       5000
- Overruns:              2 (0.0400%)
- Deadlines skipped:     51
- Allocation violations: 0
```

Two things worth reading off that table.

**The wakeup jitter is the platform, not the code.** SCHED_FIFO was denied
(no `CAP_SYS_NICE` under WSL2), so the loop ran on the ordinary time-sharing
scheduler. 78 µs of median jitter against a 1 ms period is what that costs.
Callback execution — the part this code is responsible for — sits at 103 ns
median and 639 ns at p99.9. The report says `DENIED` in the header for exactly
this reason: a runtime that believes it has real-time scheduling when it does
not produces latency figures that are fiction, and that fiction surfaces as an
unexplained field failure rather than a test failure.

**Zero allocation violations across 5000 cycles.** The executor holds its own
contract, and the guard was verifiably linked while it did.

Defensible tail figures need tuned bare-metal Linux with isolated cores, which
is what [`rt-latency-lab`](https://github.com/Onwcan/rt-latency-lab) exists to
provide.

---

## What CI enforces

| Gate | Why |
|---|---|
| GCC + Clang × Debug + Release, `-Werror` | `-Wconversion` and `-Wold-style-cast` fire on different constructs per compiler |
| **ThreadSanitizer** | The only gate that validates ADR-0002. TSan models the C++ abstract machine, so it catches ordering bugs x86-64 forgives |
| ASan + UBSan, `-fno-sanitize-recover=all` | A finding fails the build rather than printing a note |
| clang-tidy `--warnings-as-errors=*` | Rule set justified per exclusion |
| clang-format `--dry-run --Werror` | Formatting never reaches review |
| install + downstream consumer compile | The public CMake contract is tested, not assumed |

### Known gaps, stated rather than buried

- **Memory ordering is validated on x86-64 only.** x86-64's strong memory model
  will hide an ordering bug that AArch64 would expose, and AArch64 is where
  industrial edge devices increasingly are.
- **Only `operator new` is guarded**, not `malloc` from C dependencies.
- **The guard cannot see an allocation the compiler elided.** [expr.new]/10
  lets an implementation omit the allocation call when it can prove the result
  is unused, so Release reports fewer violations than Debug. Harmless in
  production — an elided allocation costs nothing — but it means any test
  asserting on allocator behaviour must make the allocation observably
  necessary. Two of these tests did not, passed in Debug, and failed the first
  Release run.
- **No latency number here was taken on real-time-capable hardware.** Every
  figure above is labelled with the conditions it was gathered under, and the
  tail figures are not claimed as properties of this code.

---

## Testing approach

Tests assert **structural** properties that hold on any host — exact cycle
counts, drift-free deadline arithmetic, correct overrun accounting, absence of
allocation, and ordering and loss guarantees under real thread contention.

They deliberately do *not* assert latency thresholds. These tests run on shared,
virtualised CI runners without real-time privileges; a test asserting "jitter
under 50 µs" there fails for reasons unrelated to the code, and a flaky test is
worse than no test because the team learns to re-run it. Latency belongs in the
benchmark and in `rt-latency-lab`, on hardware where the number means something.

Where a tolerance *is* loose, the reason is written down. Where a gap exists, it
is in this README rather than only in the author's head.

---

## Design principles

1. **Nothing on the deadline path may block, allocate, or enter the kernel.**
   Every component is judged against this before anything else — and the
   allocation half is mechanically enforced, not trusted.
2. **Measure, then claim.** No performance statement here is unaccompanied by
   the code that produced it and the conditions it was taken under.
3. **Report what actually happened, not what was requested.** Scheduling
   policy, CPU affinity and memory locking are all read back from the kernel
   and reported, because in containers and VMs they routinely do not apply.
4. **Constrain the problem rather than generalise the solution.** The runtime
   is genuinely SPSC, so the queue is SPSC — which removes the CAS loop, the
   ABA hazard and reclamation outright. Less subtlety under a safety function
   is a feature.
5. **A decision without a written alternative is not a decision.** Every ADR
   names what was rejected and why.

---

## Licence

Apache-2.0. Portfolio work — every design decision here is one I can defend in
review.

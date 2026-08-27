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
| WP-09a | `safety` — black-channel telegram, CRC, fault model | **Done** |
| WP-09b | `safety` — IEC 61800-5-2 supervisor, 1oo2D dual channel | **Done** |
| WP-09c | Requirements traceability, FMEA, CI gate | **Done** |
| WP-10 | `ipc` — zero-copy shared-memory transport | **Done** |
| WP-11 | `edge` — container packaging, metrics, dashboard | **Done** |

| WP-11b | External emergency stop, acknowledgement, timestamped safety signal | **Done** |

**210 tests**, all passing under Debug, Release, ASan+UBSan and ThreadSanitizer,
plus a libFuzzer target on the telegram decoder. The shared-memory tests
genuinely `fork()` rather than simulating a second process with a thread.

**65 requirements**, every one linked to implementing code and verifying tests,
with a CI gate that fails on a broken link.

---

## Run it

```bash
docker compose -f deploy/docker-compose.yml up -d
```

Grafana on `:3000`, Prometheus on `:9090`, the runtime's metrics on `:9100`.

### Stop it, and start it again

```bash
docker compose exec safeedge touch /tmp/safeedge-estop
```

Torque is withheld. Now remove the request:

```bash
docker compose exec safeedge rm /tmp/safeedge-estop
```

**Nothing restarts.** That is not an oversight. IEC 60204-1 requires that
restoring an emergency stop device must not by itself restart a machine, so
coming back needs a second, deliberate act:

```bash
docker compose exec safeedge touch /tmp/safeedge-ack
```

The two inputs behave differently on purpose. The stop is level-triggered,
because it is a *condition*. The acknowledgement is one-shot and consumed on
read, because an acknowledgement left asserted would clear the latch again on
the next cycle — and a fault that cannot stay latched is not latched.

Neither is polled from the control loop. A `stat()` is cheap until the
filesystem decides otherwise — an NFS mount, a full disk, a container layer
under pressure — and a control loop whose period depends on the filesystem is
not a control loop. A watcher thread publishes to an atomic the loop reads for
free.

### The endpoint that makes a stop measurable

```bash
curl -s localhost:9100/safety
# 2 0 472652975507
```

Sequence, torque permitted, and **the monotonic instant the runtime entered this
state**.

That last field exists because the `pickcell` integration proved it was missing.
`/readyz` answers "should traffic come here" and carries no time, so a consumer
polling it can only stamp its own observation — and the reaction time it computes
comes out near zero, which does not mean the link is fast, it means there is
nothing to measure. **A signal that cannot be timed cannot be held to a
deadline.**

With it, the whole column can be measured end to end. safeedge decides, the cell
stops, and the interval between them is a real number: min 5.7 ms, median
48.7 ms, max 80.7 ms across a 100 ms poll — none of which is a property of this
runtime, which decides in about a millisecond. The interval is the link.

The instant is `CLOCK_MONOTONIC`, which shares an epoch across processes on a
machine, and it is served as text rather than as a Prometheus gauge on purpose:
a nanosecond monotonic value is around 1e18, and a gauge rendered to six
significant digits would be wrong by hundreds of millions of nanoseconds.
`/metrics` carries the *age* instead, which is a small number and renders
exactly.

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

To see what the executor actually achieves on your machine:

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

### `safety::SafetySender` / `safety::SafetyReceiver` — the black channel

The transport is assumed to be **entirely untrustworthy** and given no safety
responsibility at all. Every defence lives in a thin layer at each endpoint,
which is what lets safety traffic run over ordinary, uncertified networking
hardware. Same architecture as PROFIsafe, openSAFETY and FSoE.

**PROFIsafe-inspired, not PROFIsafe.** It borrows the mechanisms because they
are the right ones and are documented in IEC 61784-3. It is not conformant, not
certified, and not interoperable with a real PROFIsafe device.

Every fault in the IEC 61784-3 model is defended and injected by a test:

| Fault | Defence |
|---|---|
| Corruption | CRC-32/AUTOSAR, HD=6 |
| Unintended repetition | Consecutive number |
| Incorrect sequence | Consecutive number |
| Loss | Sequence gap, plus watchdog when all traffic stops |
| Unacceptable delay | Watchdog |
| Insertion | Source address folded into the CRC, never transmitted |
| Masquerade | Same |
| Addressing error | Destination address folded into the CRC |

Three of those a CRC alone cannot touch, and they are the ones a home-grown
protocol usually misses. A **replayed** telegram is byte-for-byte valid — the
checksum is correct because the data really is what the producer sent, just not
when it sent it. A **delayed** one is authentic and correctly ordered and simply
describes a world that has moved on. **Masquerade and misaddressing** both look
like perfectly good frames; the defence is that the CRC is seeded with source,
destination and a parameter signature that are *never put on the wire*, so
anything that does not already share them cannot produce a matching checksum.

That is not cryptography — those parameters are commissioning data, not secrets.
It defends against faults, not adversaries.

Any fault **latches** a safe state and `receive()` then hands over nothing until
acknowledged. Deliberately inconvenient: real faults are intermittent, so a
consumer that recovered on the next good telegram would ride through a failing
transceiver indefinitely while the machine ran on data that was only sometimes
trustworthy. [ADR-0005](docs/adr/0005-black-channel-fault-model.md).

### `safety::SafetyStateMachine` — the IEC 61800-5-2 supervisor

Implements **STO, SS1-t, SOS, SLS, SLP**. Explicitly not implemented: SS2, SBC,
SDI, SLI, SMS, SSM, SAR, SLA — each needs drive hardware this does not model.

Evaluation order inside a cycle is fixed and load-bearing:

1. **Unconditional demands** — E-stop, communication loss, watchdog loss. These
   override every state and every request. Not inputs to be arbitrated;
   conditions under which permission is not available to grant.
2. **Fault latch and acknowledgement.**
3. **Per-state monitors.** Before requests, so a host repeatedly requesting the
   function it is currently violating cannot ride through the violation.
4. **Requested transitions.**

Three properties worth calling out:

**Requests are level-triggered.** A function stays active only while still
requested; dropping to `kNone` releases it. The alternative is equally
defensible and the difference is sharp — under edge triggering a silent host
leaves the machine restricted, which is safe; under level triggering it releases
the restriction, which is not. That moves the entire burden of detecting a
silent host onto the watchdog, which is why `communication_ok` and
`heartbeat_ok` are unconditional demands rather than advisory inputs.

**Acknowledgement clears a fault but does not restart motion.** A separate
enable is required. Collapsing the two is how an acknowledgement button becomes
a start button.

**The first cause is retained, not the latest.** Once torque is removed the axis
decelerates and reliably produces follow-on violations; reporting the most
recent one would send a technician to the wrong subsystem.

Every default is fail-safe: a default-constructed `SafetyInputs` describes a
machine with the E-stop pressed and no communication.

### `safety::DualChannelSupervisor` — 1oo2D

**1oo2**: either channel alone can demand the safe reaction. Every field of the
combined output takes the safe direction, so no channel can grant a permission
its peer withholds. **D**: the channels are cross-compared every cycle, so a
channel that has failed in a way it cannot itself detect shows up as a
disagreement.

Disagreement is tolerated briefly — independent sensors have independent noise,
and at a threshold boundary they will legitimately differ for a cycle or two.
Faulting on the first disagreement makes the machine unusable; never faulting
makes the diagnostics decorative.

**The limitation that matters most: both channels run the same code.** Real
1oo2D uses diverse implementations, because two identical ones share identical
systematic faults — a logic error produces the same wrong answer in both, they
agree perfectly, and cross-comparison reports everything is fine. This defends
against random hardware faults and divergent sensor input. It does not defend
against a software defect, and running the same function twice never will.

[ADR-0006](docs/adr/0006-safety-supervisor-and-dual-channel.md).

**None of this is certified, and none of it is a safety case.** A real
installation needs a qualified implementation, an assessed process and a
notified body. What it demonstrates is that the functions, their interactions
and their failure modes are understood.

### `ipc::SharedMemoryRegion` / `ipc::SeqlockSlot`

Two kinds of data move between the runtime's processes, and they want different
structures. **Queued** data, where every item matters, uses `SpscRing`.
**State**, where only the newest value has meaning — current joint position,
current safety state — uses a seqlock. Conflating them is a common and expensive
mistake: a queue applies backpressure to preserve values nobody wants, and on a
real-time producer backpressure is either a blocked deadline or a silent drop.

Measured cross-process round trip, 64-byte messages, 20 000 iterations:

| Transport | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| shared memory (seqlock) | **446 ns** | **587 ns** | **6.3 µs** |
| Unix stream socket | 27 968 ns | 85 025 ns | 241 580 ns |
| pipe | 27 313 ns | 81 959 ns | 229 763 ns |

Tens of times faster at the median — but the tail is the interesting column.
Nearly **three orders of magnitude tighter at p99.9**, and for a cycle that has
to fit inside a deadline, the tail is what decides whether it fits.

Two costs, both real:

**It burns a core.** Shared memory does not make the work cheaper, it moves the
cost from a kernel wakeup to a spinning reader. The benchmark reports the child's
CPU time next to the latency for that reason — and even that understates it,
because the benchmark keeps both sides busy. A consumer polling for messages
arriving every millisecond spins through the other 99% of the time.

**It gives up isolation.** A peer with the region mapped can write anything
anywhere in it, at any time. That is the price of zero copy. Where the peer is
not trusted, the black-channel CRC and sequence number apply exactly as they do
over a network — the safety layer does not care whether the untrusted transport
is Ethernet or a page of memory.

The seqlock is **race-free by construction**: the payload is an array of atomics
copied word by word, not plain memory read while a writer may be writing it. The
textbook version is a data race by the letter of the standard, TSan reports it,
and the usual responses are to suppress the sanitiser or shrug. Reader retries
are bounded, because a writer that died mid-update leaves the counter odd
forever and an unbounded loop would hang a reader inside its own control cycle.

[ADR-0008](docs/adr/0008-shared-memory-transport.md) records the ordering — and
that GCC's ThreadSanitizer cannot instrument `std::atomic_thread_fence`, which
is why the ordering is expressed per-operation rather than with fences.

### The container

The runtime image is **1.51 MB** and contains **exactly one file** — the
statically linked binary. No shell, no package manager, no libc. An image with
no userland cannot have a userland vulnerability, which is a stronger property
than keeping one patched.

CI enforces that rather than asserting it: it unpacks the image layers and fails
if they hold anything but `safeedged`, with an 8 MB budget so a debug tool added
"just for now" fails the build instead of quietly staying.

The cost is real: there is nothing to `docker exec` into. That works here only
because the diagnostics were designed for it — structured JSON logs and a
metrics endpoint carrying everything the runtime knows about itself.

### The conflict worth knowing about

`SCHED_FIFO` needs `CAP_SYS_NICE`. A container that drops every capability
**cannot do real-time scheduling** — it starts, serves metrics, passes its
healthcheck, and silently misses deadlines. Same image, two runs:

| Run | `safeedge_realtime_scheduling_granted` |
|---|---|
| `--cap-drop=ALL` | **0** |
| `--cap-drop=ALL --cap-add=SYS_NICE --ulimit rtprio=99` | **1** |

Security hardening and real-time behaviour are in direct tension, and the
default advice — drop everything — silently costs the runtime the property it
exists for. The compose file adds `SYS_NICE` back deliberately with the reason
written beside it.

The mitigation is not the comment, though. It is that
`safeedge_realtime_scheduling_granted` is the **first metric emitted and the
first, largest panel on the dashboard**: every latency figure next to it is
meaningless when it reads 0, and a dashboard showing microsecond jitter without
it would be actively misleading.

### Liveness is not readiness

`/healthz` asks whether the process is functioning. `/readyz` asks whether it is
fit to be used. A latched safety fault makes it **not ready** but still
**healthy** — because restarting the container would throw away the fault
information an engineer needs, and restart a machine whose safety supervisor had
just decided it should not be running.

`docker stop` completes in **829 ms** against a 10 s grace period. That matters:
SIGKILL would mean the runtime never reaches a safe state, and PID 1 has no
default signal disposition, so the handler has to be installed explicitly.
[ADR-0009](docs/adr/0009-edge-packaging.md).

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

### CRC error detection

Verified rather than asserted: **exhaustive** detection of every single-bit and
every double-bit error in a 32-byte message, heavy sampling at three, four and
five bits, and every burst up to 32 bits. The standard check vector
(`0x1697D06A` for `"123456789"`) is asserted so the implementation is
identifiably CRC-32/AUTOSAR and not something that merely resembles it.

The classic CRC-32 polynomial used by Ethernet and zip drops to HD=4 above 91
bits — four flips can produce a valid checksum. `0xF4ACFB13` holds **HD=6 up to
2048 bits**. For a checksum whose failure mode is "the machine acts on corrupted
motion data", two extra guaranteed bits is not marginal.

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

## Requirements traceability

47 requirements in [`docs/safety-requirements.md`](docs/safety-requirements.md),
each linked to implementing code and verifying tests by source annotations:

```cpp
// @satisfies REQ-SAF-023      in include/ or src/
// @verifies  REQ-SAF-023      in tests/ or fuzz/
```

[`docs/traceability-matrix.md`](docs/traceability-matrix.md) is generated from
those annotations and committed, and CI fails if a requirement has no
implementation link, no verification link, if an annotation or an
[FMEA](docs/fmea.md) row names a requirement that does not exist, or if the
committed matrix is stale.

The unknown-id check is the one that matters. A hand-maintained matrix is
accurate the day it is written and decays from then on; a *generated* one that
cannot detect its own broken links is worse still, because it manufactures
confident evidence nobody has checked. An annotation reading `REQ-SAF-O23`
(letter O for zero) attaches to nothing — so the negative case is exercised
rather than assumed, and produces two failures: the unresolved reference at its
exact line, and the requirement it abandoned.

The [FMEA](docs/fmea.md) deliberately carries **no RPN numbers**. Occurrence
scores need field data this component does not have, and inventing them produces
arithmetic that looks like measurement. Severity is stated qualitatively because
it genuinely is known; detection is stated as the mechanism, which is what a
reviewer actually needs. One row — a shared systematic software defect across
both channels — is listed with **no covering requirement**, because there is
none it could honestly be traced to. [ADR-0007](docs/adr/0007-mechanical-traceability.md).

## What CI enforces

| Gate | Why |
|---|---|
| GCC + Clang × Debug + Release, `-Werror` | `-Wconversion` and `-Wold-style-cast` fire on different constructs per compiler |
| **ThreadSanitizer** | The only gate that validates ADR-0002. TSan models the C++ abstract machine, so it catches ordering bugs x86-64 forgives |
| ASan + UBSan, `-fno-sanitize-recover=all` | A finding fails the build rather than printing a note |
| clang-tidy `--warnings-as-errors=*` | Rule set justified per exclusion |
| clang-format `--dry-run --Werror` | Formatting never reaches review |
| install + downstream consumer compile | The public CMake contract is tested, not assumed |
| requirements traceability | Every requirement implemented and verified; no dangling or invented ids. Scans the Dockerfile and compose file too, because some requirements are implemented there and nowhere else |
| container: layers, size, run, stop | Image holds exactly one file, stays inside an 8 MB budget, serves its endpoints hardened, and honours SIGTERM inside 5 s |
| Trivy scan + SBOM | Nothing to find in an image with no userland — asserted rather than assumed |
| libFuzzer on the telegram decoder | The only code here that parses bytes it did not produce |

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

Apache-2.0. 
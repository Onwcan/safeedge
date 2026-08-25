# ADR-0008: Shared memory for the runtime's internal transport

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

The runtime is several processes: a real-time executor, a safety supervisor, a
telemetry publisher, an edge-platform bridge. They exchange two very different
kinds of data.

**Queued data**, where every item matters — commands, events, log records.
`concurrent::SpscRing` already covers that within a process.

**State**, where only the newest value has any meaning — current joint position,
current safety state, current cycle statistics. A reader that misses three
updates has lost nothing, because the fourth supersedes them all.

Conflating the two is a common and expensive mistake. Using a queue for state
means applying backpressure to preserve values nobody wants, and on a real-time
producer backpressure is either a blocked deadline or a silent drop.

## Decision 1: shared memory, and what it costs

Measured on the development host, 64-byte messages, 20 000 cross-process round
trips (`benchmarks/bench_transports.cpp`):

| Transport | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| shared memory (seqlock) | **446 ns** | **587 ns** | **6.3 µs** |
| Unix stream socket | 27 968 ns | 85 025 ns | 241 580 ns |
| pipe | 27 313 ns | 81 959 ns | 229 763 ns |

Tens of times faster at the median — but the interesting column is the tail.
Shared memory is nearly **three orders of magnitude tighter at p99.9**, and for
a cycle that has to fit inside a deadline, the tail is the number that decides
whether it fits. A median is a nice-to-have; a p99.9 of 240 µs inside a 1 ms
budget is a design constraint.

Two costs, both real, both stated rather than buried:

**It burns a core.** Shared memory does not make the work cheaper; it moves the
cost from a kernel wakeup to a spinning reader. The benchmark reports the child's
CPU time alongside the latency for exactly this reason — and even that figure
*understates* it, because the benchmark keeps both sides continuously busy. A
consumer polling for messages that arrive every millisecond spins through the
other 99% of the time.

**It gives up isolation.** A peer with the region mapped can write anything
anywhere in it, at any time, including mid-read. That is the price of zero copy.
Where the peer is not trusted, the black-channel CRC and consecutive number of
ADR-0005 apply exactly as they do over a network — the safety layer does not
care whether the untrusted transport is Ethernet or a page of memory. This is
recorded as row T-09 in the FMEA with no mitigation at the transport layer,
because there is none.

**gRPC is not in the comparison.** It bundles protobuf serialisation and HTTP/2
framing on top of a transport, so timing it here would measure three things and
attribute the total to one. "What does an RPC framework cost" is a fair
question; it is a different benchmark.

## Decision 2: a seqlock for state, not a queue

`SeqlockSlot<T>` holds one value. A sequence counter brackets every write: odd
means a write is in progress, even means stable. A reader samples the counter,
reads the value, samples again, and retries if they differ.

The writer is **wait-free** — it never waits for a reader, which is what makes
it legal on the deadline path. Readers are lock-free, retrying only under an
actual collision.

**Reader retries are bounded.** A writer that died mid-update — a crashed peer,
which is a real possibility once the writer is another process — leaves the
counter odd forever. An unbounded retry loop would hang the reader inside its
own control cycle. Failing a read is recoverable; hanging on the real-time path
is not.

## Decision 3: the payload is atomic, not plain

The textbook seqlock reads the payload with plain loads while the writer may be
writing it, then discards the result if the counter moved. **That is a data race
by the letter of the standard**, and not merely pedantically: ThreadSanitizer
reports it, and a compiler is entitled to assume it cannot happen, which permits
transformations that make the discard-on-retry logic unsound.

The usual responses are to suppress the sanitizer or to shrug. Neither is
available to code that has to be argued for.

So the payload is stored as an array of `std::atomic<std::uint64_t>` and copied
word by word. Atomic accesses are defined behaviour under concurrent access;
plain ones are not. The cost is a `memcpy` through a stack staging buffer.

### The ordering, and why it is not fences

This originally used `std::atomic_thread_fence`, which is the textbook
formulation. It was abandoned because **GCC's ThreadSanitizer cannot instrument
standalone fences** — it says so via `-Wtsan`, and `-Werror` turns that into a
build failure.

That warning is load-bearing, and worth stating plainly because the instinct is
to silence it. GCC is not reporting a style preference; it is reporting that it
cannot compile the construct correctly under instrumentation. An implementation
that cannot be built under the sanitiser that validates it is a bad trade.

The ordering is therefore expressed per-operation:

- the counter stores are `seq_cst`;
- **the payload word stores are `release`**, not relaxed. A `seq_cst` store is a
  *release* operation, and release constrains what comes before it, never what
  comes after — so a relaxed word store could legally be hoisted above the
  odd-counter marker that precedes it, letting a reader observe new payload
  words while both counter samples still read the old even value. x86-64 would
  never expose this; AArch64 would, and that is where this is headed;
- the payload word loads are `seq_cst`, so they participate in the same total
  order as the two counter samples bracketing them and cannot be sunk past the
  second. An acquire load would not do — acquire constrains what follows it.

## What the torn-value test actually caught

Worth recording accurately, because the first two diagnoses were wrong.

Under TSan the test reported 4596 torn reads. The first hypothesis was the
fences; replacing them changed nothing. The second was the payload ordering;
that changed nothing either. Dumping an actual failing value took under a minute
and settled it: every "torn" payload was **all zeroes** — the pristine,
never-written slot.

The bug was in the test. Its payload type sets `joint[i] = i * 0.125`, so an
all-zero value is not something that type can produce, and the consistency check
correctly rejected it. Under normal timing the writer got there first; under
TSan the startup window widened enough for readers to catch the initial state.

Two things came out of it. The test now seeds a consistent value before readers
start. And the underlying constraint is now documented, because it is real and
applies to anything placed in a seqlock: **the zero value must be a valid value
for the type**, since a reader can always observe the region before the first
write. That is FMEA row T-08.

The ordering changes were kept. They did not fix the observed failure, but the
reasoning for them is independently sound and the hazard they close is genuine.
Saying so is more useful than implying they were the cure.

## Consequences

**Positive**

- Tens of times lower latency and nearly three orders of magnitude tighter tail
  than the kernel-mediated alternatives, measured rather than asserted.
- Race-free under the C++ memory model and clean under TSan, without
  suppressions.
- The primitives are standard-layout and trivially destructible, so they can be
  placed directly in a mapped region — verified by tests that actually `fork()`
  rather than simulating a second process with a thread.

**Negative**

- **No isolation.** Recorded in the FMEA without mitigation.
- **A core is spent** to get the latency, and the benchmark's own CPU figure
  understates that for the reason above.
- Anything in a region must be free of pointers, and both ends must agree on
  layout — in practice, the same compiler and flags. A cross-toolchain
  deployment would need an explicit wire format, which is a real limitation and
  not a hypothetical one.
- `SeqlockSlot` is single-writer. Two writers is undefined behaviour, prevented
  by contract rather than by the type system — the same limitation as
  `SpscRing`, and the same mitigation: review discipline.

**Not done**

An `eventfd`-based notifier, so a reader could block instead of spinning and
trade latency back for CPU. That is the right knob for the non-real-time
consumers — telemetry, logging — and it is the obvious next piece of this
component.

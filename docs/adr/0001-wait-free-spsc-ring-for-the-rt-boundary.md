# ADR-0001: A wait-free SPSC ring at the real-time boundary

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

The runtime has a hard cyclic deadline. A control loop running at 1 kHz has
1 ms to read inputs, evaluate the safety supervisor, compute an output and
publish it — and *every* cycle must fit, not the average one. Missing a cycle
is not a performance regression, it is a safety event that drives the machine
to a safe state.

That loop must still hand data to threads that have no such deadline: logging,
metrics, telemetry to the edge platform, the diagnostic interface. The
mechanism used for that handoff is the single most dangerous piece of
infrastructure in the runtime, because it sits directly on the deadline path.

## Options

### A. `std::mutex` + `std::condition_variable`

Simple, obviously correct, and what most code does.

Disqualifying on the real-time path for three separate reasons. **Priority
inversion**: if a low-priority logging thread holds the mutex when the
`SCHED_FIFO` control thread wants it, the control thread blocks on a thread the
scheduler will not run. Priority inheritance (`PTHREAD_PRIO_INHERIT`) bounds
this but does not remove it. **Syscalls**: an uncontended `pthread_mutex` stays
in userspace, but a contended one enters `futex`, and the cost of that is a
scheduler decision, not a bounded instruction count. **Unbounded blocking**: the
worst case is not a property of the queue, it is a property of whatever else is
running.

### B. A lock-free MPMC queue

General-purpose, and handles the case where more producers appear later.

Rejected as premature. MPMC requires CAS loops, so it is lock-free but not
wait-free — a thread can be made to retry an unbounded number of times under
contention. On the deadline path an unbounded retry loop is the same problem as
a lock wearing a different hat. It also carries the ABA hazard and needs
hazard pointers or epoch reclamation if slots are ever dynamically owned, which
is a large amount of subtlety to place under a safety function.

### C. A wait-free SPSC ring with a fixed, preallocated array

The topology of the runtime is genuinely single-producer/single-consumer: one
real-time thread produces, one service thread consumes. Constraining the
structure to what is actually needed removes the CAS loop entirely — the
producer owns the write position, the consumer owns the read position, and
neither ever needs to compare-and-swap against the other.

The result is wait-free: a bounded instruction count per operation, no locks,
no allocation, no syscalls, no retries.

## Decision

Option C, with four supporting choices:

**Fixed capacity as a template parameter, storage as `std::array`.** All slots
are constructed once when the ring is. Nothing is constructed, destroyed or
allocated while the loop is running, so no operation can enter the allocator or
throw.

**Capacity constrained to a power of two.** The index wrap becomes `& (N-1)`
instead of `% N`. A hardware divide is roughly 20–40 cycles against 1 for a
bitwise `and`. A `static_assert` enforces it, so the constraint fails at compile
time rather than degrading silently.

**Free-running 64-bit positions rather than pre-wrapped indices.** Full and
empty are distinguished by `write - read` rather than by sacrificing a slot, so
the declared capacity is the usable capacity. At 1 kHz a `uint64` position takes
about 585 million years to overflow, and unsigned wraparound would remain
correct anyway since only the difference is used.

**Each side caches the other side's position.** The common case never reads the
other thread's cache line at all; the cross-core load happens only when the ring
*appears* full or empty. This is the single biggest performance decision in the
class and it is why the benchmark compares against a control implementation that
omits it.

**Both states are cache-line aligned.** Without padding the two positions share
a line, every push invalidates the line the consumer is polling, and the queue
becomes slower than a mutex while remaining functionally perfect.

## Consequences

**Positive**

- Wait-free, so the worst case is a property of this code rather than of the
  scheduler.
- Measured at ~354 M items/s cross-thread median, against ~225 M/s for an
  identical algorithm without the padding and caching — a 1.57x layout
  advantage on this host.
- Nothing in the fast path can throw, allocate, or make a syscall, which is
  exactly the set of properties the real-time executor in WP-08 must be able
  to assume.

**Negative**

- **Two producers is undefined behaviour.** The type system does not prevent
  it; only the contract does. This is a real hazard as the runtime grows, and
  the mitigation is currently documentation plus review discipline. If a second
  producer is ever genuinely needed, the answer is a second ring, not a
  relaxation of this one.
- Capacity is fixed at compile time, so a queue that turns out to be too small
  requires a rebuild rather than a configuration change. Accepted: a
  dynamically resizable real-time queue would have to allocate.
- The false-sharing padding costs two cache lines of memory per ring. Irrelevant
  at the number of rings this runtime will have.

**Testing**

Ordering, loss and duplication are asserted with a real producer and a real
consumer thread over hundreds of thousands of items, in three shapes:
balanced, consumer-starved, and producer-starved. A payload wider than a machine
word is used to detect torn or partially published writes. The whole suite runs
under ThreadSanitizer in CI, which is what actually validates the memory
ordering in ADR-0002. The cache-line layout is asserted directly in a unit
test, because a refactor that drops the `alignas` would leave every functional
test passing while destroying the performance the design exists for.

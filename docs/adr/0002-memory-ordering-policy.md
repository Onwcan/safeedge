# ADR-0002: Memory ordering policy

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

`std::memory_order_seq_cst` is the default for every atomic operation in C++,
and it is the right default: it is the only ordering most people can reason
about without error, and on x86-64 it is often nearly free for loads.

It is not free for stores. On x86-64 a sequentially consistent store compiles
to `xchg` or to `mov` followed by `mfence`, costing tens of cycles. On AArch64
— which is where industrial edge devices increasingly are — it becomes `stlr`
with a full barrier, and the gap between `seq_cst` and `release` is wider still.

Weakening an ordering is therefore a real optimisation, and also the single
easiest way to introduce a bug that is invisible in testing, reproduces once a
month in the field, and cannot be debugged from a core dump. This ADR sets when
it is allowed.

## Decision

**Default to `seq_cst`. Weaken only where the weakening can be justified in one
paragraph at the call site, and only where ThreadSanitizer exercises the code
with real contending threads.**

Every relaxed or acquire/release operation in this codebase carries a comment
saying what it is synchronising with. An atomic operation whose ordering cannot
be explained is a defect, regardless of whether it currently passes.

### The pairing used by `SpscRing`

There are exactly two synchronisation edges, one per direction.

**Producer publishes to consumer.**

```
producer:  slots_[i] = value;                                    // (1)
           write_pos.store(w + 1, std::memory_order_release);    // (2)

consumer:  w = write_pos.load(std::memory_order_acquire);        // (3)
           out = std::move(slots_[i]);                           // (4)
```

The release at (2) prevents (1) from being reordered after it, and pairs with
the acquire at (3). When the consumer observes the incremented position, it is
guaranteed to observe the slot write that preceded it. Without the release,
(1) and (2) may be reordered and the consumer can read a slot the producer has
not finished writing — a torn payload, not a missing one, which is why the
test suite pushes a struct wider than a machine word rather than an `int`.

**Consumer releases the slot back to the producer.**

```
consumer:  out = std::move(slots_[i]);                           // (4)
           read_pos.store(r + 1, std::memory_order_release);     // (5)

producer:  r = read_pos.load(std::memory_order_acquire);         // (6)
           slots_[i] = value;                                    // (1, next lap)
```

Symmetric. The release at (5) ensures the producer cannot observe the slot as
reusable before the consumer has finished moving out of it. Omitting this one is
the subtler bug: it only manifests when the ring is full, which is precisely the
condition a lightly loaded test never reaches. The consumer-starved concurrency
test exists to force it.

**Each side reads its own position with `relaxed`.**

```
const std::uint64_t write = producer_.write_pos.load(std::memory_order_relaxed);
```

No ordering is needed because no other thread writes this value. The atomic type
is required only so that the *other* thread's reads are not a data race; from
the owning thread's perspective it is an ordinary variable. Using `acquire` here
would be pure cost with no meaning.

**The cached positions are not atomic at all.** `cached_read_pos` is written and
read only by the producer; `cached_write_pos` only by the consumer. Making them
atomic would imply a cross-thread relationship that does not exist and would
mislead the next reader.

## Consequences

**Positive**

- The publication points are explicit and named, so a reviewer can check the
  pairing without reconstructing the algorithm.
- On AArch64 edge targets the release stores avoid a full barrier per push.

**Negative**

- Weak ordering is not verifiable by inspection alone, and x86-64's strong
  memory model will hide a genuine ordering bug in local testing. Two
  mitigations: ThreadSanitizer in CI, which models the C++ abstract machine
  rather than the host, and a standing intent to run the concurrency suite on
  AArch64 hardware before anything ships. **Until that AArch64 run happens,
  this ADR is validated on x86-64 only, and that is a known gap rather than an
  oversight.**

**Explicitly out of scope**

Lock-free structures needing reclamation — hazard pointers, epochs, RCU — are
not in this codebase and are not planned. Preallocated fixed-capacity storage
sidesteps reclamation entirely, and that is a deliberate limit on how much
subtlety is allowed underneath a safety function.

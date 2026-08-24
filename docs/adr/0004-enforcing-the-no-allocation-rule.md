# ADR-0004: Enforce the no-allocation rule instead of documenting it

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

"No allocation on the real-time path" is the rule every real-time codebase
states and very few enforce. `malloc` may take a lock, walk a free list, split
or coalesce blocks, or call `brk`/`mmap` and enter the kernel. Any of those
inside a 1 ms cycle can blow the deadline.

The reason it keeps happening despite everyone knowing the rule is that the
allocation is rarely visible at the call site:

- `std::function` heap-allocates as soon as the captured state exceeds its
  small-object buffer — roughly two pointers. The code reads as a plain
  assignment.
- `std::vector::push_back` allocates only on the growth step, so it is fine for
  the first thousand cycles and not fine on the one where the buffer fills.
- `std::string` concatenation in a log line that only executes on an error path.
- A `std::shared_ptr` copy that constructs a control block.

Every one of these passes review, passes tests, and fails in the field under
load. Code review does not catch them reliably. Something mechanical has to.

## Options

### A. Documentation and review discipline

Free, and demonstrably insufficient — this is the status quo that produces the
defects above.

### B. Ban the offending types by convention

Forbid `std::function`, `std::vector`, `std::string` in real-time code.
Effective for the known cases, useless against the unknown ones, and it
disallows a great deal of code that would be perfectly safe with a reserve call.
It also cannot be checked automatically without a bespoke clang-tidy rule.

### C. Replace `operator new` and abort inside marked regions

Mark the real-time region with an RAII scope; replace the global `operator new`
family with versions that check a thread-local depth counter first. An
allocation inside the scope aborts the process, or is counted, according to
policy.

Catches the unknown cases as well as the known ones, requires no restriction on
which types may be used, and turns a field failure into a test failure.

## Decision

Option C, with four constraints that make it safe to ship.

**The guard is a separate CMake target.** `safeedge::rt_alloc_guard` contains
only the hooks. Linking it changes allocation behaviour for the entire binary,
which must be a decision by whoever produces the executable, never something a
library imposes on its consumers. A production image can leave it out.

**State lives in `safeedge::rt`, hooks live in the guard.** This split is what
makes the guard genuinely optional. If the counters and the replacement lived
in the same translation unit, every consumer of `safeedge::rt` would carry an
unresolved reference to symbols that only exist when the guard is linked, and
"optional" would be a fiction. The guard sets an installed-flag from a static
initialiser, so `guardIsInstalled()` reports the truth either way.

**The guard must be linked with `WHOLE_ARCHIVE`.** A global `operator new`
replacement resolves no undefined symbol, so an ordinary static-library link
drops the object file entirely. The guard would then not exist — and every test
asserting "zero violations" would pass because nothing was watching. This is the
worst available failure mode, so `tests/CMakeLists.txt` and
`examples/CMakeLists.txt` both use `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>` and
`NoAllocGuardTest.GuardIsActuallyLinked` asserts the flag.

**The whole operator new family is replaced, not just `operator new(size_t)`.**
The array, sized, aligned and nothrow forms are what the compiler actually emits
for over-aligned or trivially-destructible types. Replacing a subset means a
replaced `new` can be paired with a default `delete`, which is undefined
behaviour and typically surfaces as heap corruption under `-O2`.

### Policy

- `kAbort` (default) — terminate immediately. Correct for tests and CI.
- `kCount` — record and continue. Correct in production: a surprise allocation
  is a defect, but killing a running machine over one is worse than counting it
  and letting the safety supervisor decide on a deliberate transition.
- `kIgnore` — suppress the abort; counting continues because it is free.

`AllowAllocScope` suspends the guard for the honest cases: a diagnostic path
that has already missed its deadline and is now assembling an error report.
Making the exemption an explicit, greppable type is the point — an `#ifdef`
would not be.

### The violation handler cannot allocate

It is reached precisely because something allocated when it should not have, so
allocating again to report it could recurse or deadlock. `std::cerr` allocates
on first use and `printf` may allocate an internal buffer, so the message is
formatted into a stack buffer by hand and written with `write(2)`, which has no
userspace state. The thread-locals are constant-initialised PODs for the same
reason: first access to a dynamically-initialised `thread_local` can itself
allocate.

## Consequences

**Positive**

- The executor holds itself to its own contract, verified in
  `CyclicExecutorTest.TheExecutorItselfDoesNotAllocatePerCycle`.
- Claims made by other components become testable rather than rhetorical.
  ADR-0001 asserts that `SpscRing` never allocates; that is now a passing test
  rather than a sentence.
- `LatencyHistogram` exists because a `std::vector` of samples would allocate,
  and the guard proves the replacement actually achieved it.
- The report records the largest violating size, which is usually enough to
  identify the culprit: ~32 bytes is a `std::function` or a small vector,
  multi-kilobyte is a buffer that outgrew its reserve.

**Negative**

- **Only `operator new` is intercepted.** A direct `malloc`, `calloc`,
  `realloc` or `strdup` from C code — inside libc, or in a third-party C
  library — passes through unseen. Catching those needs symbol interposition or
  an `LD_PRELOAD` shim, both of which are fragile and can deadlock when the
  reporting path itself allocates. For the code in this repository, which is
  C++ throughout, `operator new` is the complete surface. **For linked-in C
  dependencies it is not, and that is a real gap rather than a theoretical
  one.**
- **The guard cannot see an allocation the compiler removed.** [expr.new]/10
  permits omitting the allocation call for a new-expression whose result the
  implementation can prove is unused, and both GCC and Clang do this at `-O2`.
  A Release build therefore reports *fewer* violations than a Debug build of
  the same code.

  For production this is harmless — an elided allocation costs nothing at
  runtime, so there is no defect to find. For *tests* it is a trap, and one
  that was walked into: `AllocationInsideAScopeIsCounted` and
  `ArrayAndAlignedFormsAreAlsoIntercepted` both wrote `delete new T`, passed in
  Debug, and failed the first Release run because the allocation they were
  asserting on had been optimised away. They now route the pointer through a
  volatile sink so it escapes and the call survives.

  The general lesson applies beyond these two tests: any test that asserts on
  allocator behaviour must make the allocation observably necessary, and must
  run in Release as well as Debug. The CI matrix covers both for this reason.
- The guard adds a thread-local load and a predictable branch to every
  allocation in the binary, including the ones outside any scope. Negligible,
  but non-zero, and a reason not to ship it in a production image by default.
- Two targets and a `WHOLE_ARCHIVE` link are more build complexity than one
  library. Justified by what silent absence would cost.

**Alternative rejected for later reconsideration**

Running the real-time thread with a custom `std::pmr::memory_resource` that
traps would be more surgical and would not touch the global allocator. It only
covers allocations that route through a PMR-aware container, which the standard
library types used here do not by default, so it would catch strictly less. Worth
revisiting if the codebase ever moves to PMR containers throughout.

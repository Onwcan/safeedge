// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace safeedge::rt {

/// What to do when code inside a NoAllocScope allocates.
enum class AllocationPolicy : std::uint8_t {
  /// Guard installed but passive. Allocations pass through unremarked.
  kIgnore,
  /// Count violations and carry on. The right choice in production: a
  /// surprise allocation is a defect, but killing a running machine over it
  /// is worse than logging it and driving to a safe state deliberately.
  kCount,
  /// Terminate immediately. The right choice in tests and CI, where an
  /// allocation on the real-time path should fail the build loudly rather
  /// than turn into a flaky latency spike nobody can reproduce.
  kAbort,
};

/// Marks a region that must not allocate.
///
/// The problem this solves
/// ----------------------
/// "No allocation on the real-time path" is the rule every real-time codebase
/// states and very few enforce. `malloc` can take a lock, walk a free list,
/// or call `brk`/`mmap` and enter the kernel. Any of those inside a 1 ms cycle
/// can blow the deadline -- and it will happen not in testing but in the field,
/// on the one code path that allocates only when a buffer happens to grow.
///
/// Documentation does not catch that. A guard that aborts does.
///
/// How it works
/// ------------
/// Linking `safeedge::rt_alloc_guard` replaces the global `operator new` family
/// with versions that check a thread-local depth counter first. Constructing a
/// NoAllocScope increments it; destruction decrements. Nesting is allowed.
///
/// What it does NOT catch
/// ----------------------
/// Only C++ `operator new` is intercepted. A direct `malloc`, `calloc`,
/// `realloc` or `strdup` from C code -- inside libc, or a third-party C
/// library -- passes through unseen. Catching those needs symbol interposition
/// or an LD_PRELOAD shim, which is fragile and can deadlock when the reporting
/// path itself allocates. The limit is documented rather than papered over;
/// for the code in this repository, which is C++ throughout, `operator new`
/// is the complete surface.
///
/// Also: this is a *link-time* global replacement. It is deliberately a
/// separate CMake target so that linking it is an explicit decision, and so
/// production binaries can leave it out entirely.
// @satisfies REQ-RT-001
class NoAllocScope {
 public:
  NoAllocScope() noexcept;
  ~NoAllocScope() noexcept;

  NoAllocScope(const NoAllocScope&) = delete;
  NoAllocScope& operator=(const NoAllocScope&) = delete;
  NoAllocScope(NoAllocScope&&) = delete;
  NoAllocScope& operator=(NoAllocScope&&) = delete;
};

/// Temporarily suspends the guard inside a NoAllocScope.
///
/// Needed for the honest cases: a diagnostic path that has already missed its
/// deadline and is now assembling an error report, or setup code that runs
/// inside a scope for structural reasons. Making the exemption explicit and
/// searchable is the point -- an `#ifdef` around the guard would not be.
class AllowAllocScope {
 public:
  AllowAllocScope() noexcept;
  ~AllowAllocScope() noexcept;

  AllowAllocScope(const AllowAllocScope&) = delete;
  AllowAllocScope& operator=(const AllowAllocScope&) = delete;
  AllowAllocScope(AllowAllocScope&&) = delete;
  AllowAllocScope& operator=(AllowAllocScope&&) = delete;
};

struct AllocationReport {
  /// Number of allocations observed inside a NoAllocScope since the last reset.
  std::uint64_t violations{0};
  /// Size of the largest such allocation, in bytes. Useful for identifying it:
  /// a 32-byte violation is usually a std::function or a small vector, a
  /// multi-kilobyte one is usually a buffer that outgrew its reserve.
  std::size_t largest_bytes{0};
};

/// Default is kAbort. Set before entering any real-time section.
void setAllocationPolicy(AllocationPolicy policy) noexcept;
[[nodiscard]] AllocationPolicy allocationPolicy() noexcept;

[[nodiscard]] AllocationReport allocationReport() noexcept;
void resetAllocationReport() noexcept;

/// True when the calling thread is inside an active NoAllocScope.
[[nodiscard]] bool inNoAllocScope() noexcept;

/// True when the global `operator new` replacement is actually linked in.
///
/// Matters because a guard that is absent looks exactly like a guard that
/// found nothing: a test asserting "zero violations" would pass for entirely
/// the wrong reason. The flag is set by a static initialiser inside
/// `safeedge::rt_alloc_guard`, so it reports the truth whether or not that
/// target made it onto the link line.
[[nodiscard]] bool guardIsInstalled() noexcept;

namespace detail {

/// Called by the global `operator new` replacement in the guard target.
/// Returns immediately unless the calling thread is inside an active
/// NoAllocScope, so the cost on a normal allocation is one thread-local load
/// and a predictable branch.
void noteAllocation(std::size_t bytes) noexcept;

/// Called once, from a static initialiser in the guard target, to record that
/// the replacement is present. Keeping the *state* in `safeedge::rt` and only
/// the *hooks* in the guard target is what lets the guard stay genuinely
/// optional: without this split, every consumer of safeedge::rt would carry an
/// unresolved reference to symbols that only exist when the guard is linked.
void markGuardInstalled() noexcept;

}  // namespace detail

}  // namespace safeedge::rt

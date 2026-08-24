// SPDX-License-Identifier: Apache-2.0
//
// State and policy for the allocation guard. Contains no global operator new
// replacement -- that lives in no_alloc_guard_hooks.cpp, in a separate target.
//
// The split is what makes the guard optional. If the replacement and the state
// lived together, every consumer of safeedge::rt would carry an unresolved
// reference to it and the "optional" target would be mandatory in practice.

#include "safeedge/rt/no_alloc_guard.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace safeedge::rt {
namespace {

// Constant-initialised POD thread-locals. Deliberately nothing with a dynamic
// initialiser: first access to a dynamically-initialised thread_local can
// itself allocate, which would make the guard the source of the very violation
// it exists to report.
thread_local int t_no_alloc_depth = 0;
thread_local int t_allow_depth = 0;

// Violation bookkeeping is process-wide rather than per-thread so a supervisory
// thread can read it. Touched only on the abnormal path, so these atomics never
// appear in a healthy cycle.
std::atomic<std::uint64_t> g_violations{0};
std::atomic<std::size_t> g_largest_bytes{0};
std::atomic<AllocationPolicy> g_policy{AllocationPolicy::kAbort};
std::atomic<bool> g_guard_installed{false};

/// Writes without allocating.
///
/// std::cerr, std::printf and friends are all off limits here: iostreams
/// allocates on first use and printf may allocate an internal buffer. We are on
/// a path that exists precisely because something allocated when it should not
/// have -- allocating again to report it could recurse or deadlock. write(2) is
/// a syscall with no userspace state.
void emitRaw(const char* text) noexcept {
#if defined(__unix__) || defined(__APPLE__)
  const std::size_t length = std::strlen(text);
  std::size_t offset = 0;
  while (offset < length) {
    const ssize_t written = ::write(STDERR_FILENO, text + offset, length - offset);
    if (written <= 0) {
      return;
    }
    offset += static_cast<std::size_t>(written);
  }
#else
  (void)text;
#endif
}

/// Renders an unsigned value into a caller-supplied buffer. std::to_string and
/// snprintf are both unavailable to us here for the reason above.
void appendUnsigned(char* buffer, std::size_t capacity, std::size_t& offset,
                    std::uint64_t value) noexcept {
  char digits[24];
  std::size_t count = 0;
  if (value == 0) {
    digits[count++] = '0';
  }
  while (value > 0 && count < sizeof(digits)) {
    digits[count++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  while (count > 0 && offset + 1 < capacity) {
    buffer[offset++] = digits[--count];
  }
  buffer[offset] = '\0';
}

void reportViolation(std::size_t bytes) noexcept {
  g_violations.fetch_add(1, std::memory_order_relaxed);

  std::size_t previous = g_largest_bytes.load(std::memory_order_relaxed);
  while (bytes > previous &&
         !g_largest_bytes.compare_exchange_weak(
             previous, bytes, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }

  if (g_policy.load(std::memory_order_relaxed) != AllocationPolicy::kAbort) {
    return;
  }

  char message[192];
  const char* prefix = "\nsafeedge: FATAL -- allocation of ";
  const std::size_t prefix_length = std::strlen(prefix);
  std::memcpy(message, prefix, prefix_length);
  std::size_t offset = prefix_length;
  appendUnsigned(message, sizeof(message), offset, bytes);

  const char* suffix = " bytes inside a NoAllocScope (real-time path)\n";
  const std::size_t suffix_length = std::strlen(suffix);
  if (offset + suffix_length < sizeof(message)) {
    std::memcpy(message + offset, suffix, suffix_length + 1);
  }
  emitRaw(message);
  std::abort();
}

}  // namespace

NoAllocScope::NoAllocScope() noexcept { ++t_no_alloc_depth; }
NoAllocScope::~NoAllocScope() noexcept { --t_no_alloc_depth; }

AllowAllocScope::AllowAllocScope() noexcept { ++t_allow_depth; }
AllowAllocScope::~AllowAllocScope() noexcept { --t_allow_depth; }

void setAllocationPolicy(AllocationPolicy policy) noexcept {
  g_policy.store(policy, std::memory_order_relaxed);
}

AllocationPolicy allocationPolicy() noexcept {
  return g_policy.load(std::memory_order_relaxed);
}

AllocationReport allocationReport() noexcept {
  return {g_violations.load(std::memory_order_relaxed),
          g_largest_bytes.load(std::memory_order_relaxed)};
}

void resetAllocationReport() noexcept {
  g_violations.store(0, std::memory_order_relaxed);
  g_largest_bytes.store(0, std::memory_order_relaxed);
}

bool inNoAllocScope() noexcept { return t_no_alloc_depth > 0 && t_allow_depth == 0; }

bool guardIsInstalled() noexcept {
  return g_guard_installed.load(std::memory_order_relaxed);
}

namespace detail {

void noteAllocation(std::size_t bytes) noexcept {
  if (t_no_alloc_depth > 0 && t_allow_depth == 0) {
    reportViolation(bytes);
  }
}

void markGuardInstalled() noexcept {
  g_guard_installed.store(true, std::memory_order_relaxed);
}

}  // namespace detail
}  // namespace safeedge::rt

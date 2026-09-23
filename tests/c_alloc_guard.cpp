// SPDX-License-Identifier: Apache-2.0
// Test-only GNU linker wrappers. --wrap redirects references from objects and
// static libraries linked into the test executable; it cannot see calls made
// internally by an already-linked shared library.
#include <cstddef>
#include <limits>

#include "safeedge/rt/no_alloc_guard.hpp"

extern "C" {
void* __real_malloc(std::size_t bytes) noexcept;
void* __real_calloc(std::size_t count, std::size_t bytes) noexcept;
void* __real_realloc(void* pointer, std::size_t bytes) noexcept;
void __real_free(void* pointer) noexcept;

void* __wrap_malloc(std::size_t bytes) noexcept {
  safeedge::rt::detail::noteAllocation(bytes);
  // TSan also reports allocations via __sanitizer_malloc_hook. Suppress that
  // nested observation, while keeping the sanitizer's actual allocator active.
  const safeedge::rt::AllowAllocScope allowed;
  return __real_malloc(bytes);
}

void* __wrap_calloc(std::size_t count, std::size_t bytes) noexcept {
  constexpr auto maximum = std::numeric_limits<std::size_t>::max();
  // Match operator new's attempt-counting policy, including failed requests.
  // Saturating the diagnostic avoids wrapping an overflowing product to zero;
  // the real allocator still receives the original arguments and sets errno.
  const std::size_t total =
      bytes != 0 && count > maximum / bytes ? maximum : count * bytes;
  safeedge::rt::detail::noteAllocation(total);
  const safeedge::rt::AllowAllocScope allowed;
  return __real_calloc(count, bytes);
}

void* __wrap_realloc(void* pointer, std::size_t bytes) noexcept {
  safeedge::rt::detail::noteAllocation(bytes);
  const safeedge::rt::AllowAllocScope allowed;
  return __real_realloc(pointer, bytes);
}

void __wrap_free(void* pointer) noexcept {
  // Free is paired with the real allocator, but is not an allocation and does
  // not increment AllocationReport (matching the operator delete policy).
  __real_free(pointer);
}
}

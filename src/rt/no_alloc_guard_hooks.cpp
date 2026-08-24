// SPDX-License-Identifier: Apache-2.0
//
// Global replacement of the operator new family.
//
// Linking this translation unit changes allocation behaviour for the entire
// binary, which is why it is its own CMake target (safeedge::rt_alloc_guard)
// that has to be asked for explicitly, and why it must be linked with
// WHOLE_ARCHIVE -- a global replacement resolves no undefined symbol, so a
// normal static-library link would simply drop the object and the guard would
// silently not exist.
//
// The whole family is replaced, not just the common two. Mixing a replaced
// operator new with a default operator delete is undefined behaviour, and the
// sized and aligned overloads are what the compiler actually emits for
// over-aligned or trivially-destructible types. Replacing a subset is a classic
// route to heap corruption that only shows up under -O2.

#include <cstdlib>
#include <new>

#include "safeedge/rt/no_alloc_guard.hpp"

namespace {

/// aligned_alloc requires the requested size to be a multiple of the alignment.
std::size_t roundUpTo(std::size_t value, std::size_t alignment) noexcept {
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

/// Records, at static-initialisation time, that the replacement below is
/// present in this binary. Lets a test tell "the guard found nothing" apart
/// from "the guard was never linked".
struct GuardInstaller {
  GuardInstaller() noexcept { safeedge::rt::detail::markGuardInstalled(); }
};

const GuardInstaller g_installer;

}  // namespace

void* operator new(std::size_t bytes) {
  safeedge::rt::detail::noteAllocation(bytes);
  void* pointer = std::malloc(bytes == 0 ? 1 : bytes);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  safeedge::rt::detail::noteAllocation(bytes);
  return std::malloc(bytes == 0 ? 1 : bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t& tag) noexcept {
  return ::operator new(bytes, tag);
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
  safeedge::rt::detail::noteAllocation(bytes);
  const auto align = static_cast<std::size_t>(alignment);
  void* pointer = std::aligned_alloc(align, roundUpTo(bytes == 0 ? 1 : bytes, align));
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
  return ::operator new(bytes, alignment);
}

void* operator new(std::size_t bytes, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  safeedge::rt::detail::noteAllocation(bytes);
  const auto align = static_cast<std::size_t>(alignment);
  return std::aligned_alloc(align, roundUpTo(bytes == 0 ? 1 : bytes, align));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return ::operator new(bytes, alignment, tag);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(pointer);
}

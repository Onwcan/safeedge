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

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SAFEEDGE_THREAD_SANITIZER 1
#endif
#endif

#if defined(__SANITIZE_THREAD__) && !defined(SAFEEDGE_THREAD_SANITIZER)
#define SAFEEDGE_THREAD_SANITIZER 1
#endif

#if defined(SAFEEDGE_TEST_WRAP_C_ALLOCATIONS) && !defined(SAFEEDGE_THREAD_SANITIZER)
// Only the test variant is linked with --wrap=malloc. Bypass that
// wrapper here: operator new already records the allocation, so counting its
// backing malloc again would turn one C++ allocation into two violations.
// The reserved symbol spelling is required by the linker's --wrap interface.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern "C" void* __real_malloc(std::size_t bytes) noexcept;
#endif

namespace {

#if !defined(SAFEEDGE_THREAD_SANITIZER)
void* allocate(std::size_t bytes) noexcept {
#if defined(SAFEEDGE_TEST_WRAP_C_ALLOCATIONS)
  return __real_malloc(bytes);
#else
  return std::malloc(bytes);
#endif
}

/// aligned_alloc requires the requested size to be a multiple of the alignment.
std::size_t roundUpTo(std::size_t value, std::size_t alignment) noexcept {
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}
#endif

/// Records, at static-initialisation time, that the replacement below is
/// present in this binary. Lets a test tell "the guard found nothing" apart
/// from "the guard was never linked".
struct GuardInstaller {
  GuardInstaller() noexcept { safeedge::rt::detail::markGuardInstalled(); }
};

const GuardInstaller g_installer;

}  // namespace

#if defined(SAFEEDGE_THREAD_SANITIZER)

// TSan supplies the global new/delete family itself and defines those symbols
// strongly. Its public allocator hook lets the guard observe each successful
// allocation without competing with, or bypassing, TSan's allocator.
// The Linux interface uses the ordinary C calling convention; spelling the
// published signature here also supports GCC, which does not install LLVM's
// public sanitizer headers.
extern "C" void __sanitizer_malloc_hook(const volatile void*, std::size_t bytes) {
  safeedge::rt::detail::noteAllocation(bytes);
}

#else

void* operator new(std::size_t bytes) {
  safeedge::rt::detail::noteAllocation(bytes);
  void* pointer = allocate(bytes == 0 ? 1 : bytes);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  safeedge::rt::detail::noteAllocation(bytes);
  return allocate(bytes == 0 ? 1 : bytes);
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

#endif

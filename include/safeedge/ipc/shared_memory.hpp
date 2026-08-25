// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace safeedge::ipc {

/// Why a shared-memory operation failed, and which call failed.
///
/// Reported rather than thrown, for the same reason as everywhere else in this
/// runtime: mapping a region is a normal operation that normally fails for
/// mundane, recoverable reasons -- the name is taken, the peer has not started,
/// the tmpfs is full. A caller needs to distinguish those, and an exception
/// type per errno would be worse than the errno.
struct SharedMemoryError {
  /// The libc call that failed: "shm_open", "ftruncate", "mmap", "fstat".
  const char* operation{nullptr};
  int error_number{0};

  [[nodiscard]] bool ok() const noexcept { return operation == nullptr; }
  /// strerror text for `error_number`, or "ok".
  [[nodiscard]] const char* what() const noexcept;
};

/// An RAII POSIX shared-memory mapping.
///
/// Ownership model
/// ---------------
/// Two responsibilities that are usually conflated are kept separate here:
///
/// * **The mapping** is owned by this object and released by the destructor.
/// * **The name** persists in the filesystem until someone calls `unlink()`.
///
/// They are separate because a shared-memory name outlives every process that
/// used it. A runtime that crashes without unlinking leaves the name behind,
/// and the next start finds a region that already exists, possibly the wrong
/// size, possibly containing a half-written state from before the crash. That
/// is a real operational failure mode, not a tidiness concern, so `unlink()` is
/// an explicit act by whichever end owns the lifetime rather than something the
/// destructor guesses at.
///
/// Cross-process constraints
/// -------------------------
/// Anything placed in a region must be free of pointers and of anything holding
/// one. The region is mapped at a different virtual address in each process, so
/// a pointer stored by one is meaningless to the other -- and will not fault,
/// it will simply point somewhere plausible and wrong. Both ends must also
/// agree on layout, which in practice means the same compiler and the same
/// flags.
///
/// **This is not a security boundary.** A peer with the region mapped can write
/// anything anywhere in it, at any time, including while it is being read. That
/// is the price of zero copy: isolation is what was traded away. Where the peer
/// is not trusted, the safety layer's CRC and sequence number apply exactly as
/// they do over a network -- the black channel does not care whether the
/// untrusted transport is Ethernet or a page of memory.
class SharedMemoryRegion {
 public:
  /// Default-constructs an invalid region.
  SharedMemoryRegion() = default;
  ~SharedMemoryRegion();

  SharedMemoryRegion(const SharedMemoryRegion&) = delete;
  SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;
  SharedMemoryRegion(SharedMemoryRegion&& other) noexcept;
  SharedMemoryRegion& operator=(SharedMemoryRegion&& other) noexcept;

  /// Creates a region, failing if the name already exists.
  ///
  /// Exclusive creation is deliberate: silently adopting an existing region is
  /// how a runtime ends up sharing state with the corpse of its previous
  /// instance. A caller that genuinely wants to reclaim a stale name should
  /// `unlink` it first, which makes the decision visible.
  ///
  /// `name` must begin with '/' and contain no other slash -- a POSIX rule that
  /// is validated here rather than left to produce a confusing EINVAL.
  [[nodiscard]] static SharedMemoryRegion create(const char* name, std::size_t bytes);

  /// Opens an existing region. Fails if it is smaller than `bytes`.
  [[nodiscard]] static SharedMemoryRegion openExisting(const char* name,
                                                       std::size_t bytes);

  /// Removes `name` from the filesystem without unmapping anything. Safe to
  /// call from either end and safe to call more than once.
  static void unlinkName(const char* name) noexcept;

  [[nodiscard]] bool valid() const noexcept { return address_ != nullptr; }
  [[nodiscard]] const SharedMemoryError& error() const noexcept { return error_; }

  [[nodiscard]] void* data() noexcept { return address_; }
  [[nodiscard]] const void* data() const noexcept { return address_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  /// Releases the mapping early. The destructor does this too; calling it
  /// explicitly is for cases where the unmap must be ordered against something
  /// else.
  void close() noexcept;

 private:
  void* address_{nullptr};
  std::size_t size_{0};
  SharedMemoryError error_{};
};

}  // namespace safeedge::ipc

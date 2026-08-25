// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "safeedge/concurrent/spsc_ring.hpp"

namespace safeedge::ipc {

using concurrent::kCacheLineSize;

/// A single-writer / multi-reader slot holding the latest value.
///
/// When this is the right structure
/// --------------------------------
/// Not for a queue -- `concurrent::SpscRing` is the queue. This is for state
/// where **only the newest value has any meaning**: the current joint position,
/// the current safety state, the current cycle statistics. A reader that misses
/// three updates has lost nothing, because the fourth supersedes them all.
///
/// That is worth being explicit about, because using a queue for such data is a
/// common and expensive mistake. A queue applies backpressure to preserve
/// values nobody wants, and on a real-time producer backpressure means either a
/// blocked deadline or a silent drop.
///
/// How it works
/// ------------
/// A sequence counter brackets every write. Odd means a write is in progress;
/// even means the value is stable. A reader samples the counter, reads the
/// value, samples again, and retries if the two samples differ. The writer
/// never waits for a reader -- it is wait-free, which is what makes it legal on
/// the real-time path -- and readers are lock-free, retrying only under an
/// actual collision.
///
/// The data race the textbook version has
/// --------------------------------------
/// The classic seqlock reads the payload with plain loads while the writer may
/// be writing it, and discards the result if the counter moved. That is a data
/// race by the letter of the C++ memory model, not merely in theory:
/// ThreadSanitizer reports it, and a compiler is entitled to assume it cannot
/// happen -- which permits transformations that make the discard-on-retry logic
/// unsound.
///
/// The usual responses are to suppress the sanitizer or to shrug. Neither is
/// available to code that has to be argued for.
///
/// So the payload is stored as an array of `std::atomic<std::uint64_t>` and
/// copied word by word with **relaxed** atomic accesses. Relaxed atomics carry
/// no ordering and compile to ordinary loads and stores on every architecture
/// this targets, so the cost is essentially nil -- but they are defined
/// behaviour under concurrent access, which plain loads are not. The ordering
/// that actually matters still comes from the acquire/release fences around the
/// sequence counter.
///
/// The result is a seqlock that is correct by the standard and clean under
/// TSan, for the price of a `memcpy` through a stack staging buffer.
///
/// Shared-memory constraints
/// -------------------------
/// `T` must be trivially copyable and free of pointers: the slot is designed to
/// live in a region mapped at a different address in each process.
template <typename T>
class SeqlockSlot {
  static_assert(std::is_trivially_copyable_v<T>,
                "a seqlock copies with memcpy; T must be trivially copyable");
  static_assert(std::is_standard_layout_v<T>,
                "the slot may live in shared memory; T must have a stable layout");
  static_assert(sizeof(T) > 0);

  static constexpr std::size_t kWordBytes = sizeof(std::uint64_t);
  static constexpr std::size_t kWordCount = (sizeof(T) + kWordBytes - 1) / kWordBytes;

 public:
  using value_type = T;

  SeqlockSlot() = default;
  SeqlockSlot(const SeqlockSlot&) = delete;
  SeqlockSlot& operator=(const SeqlockSlot&) = delete;
  SeqlockSlot(SeqlockSlot&&) = delete;
  SeqlockSlot& operator=(SeqlockSlot&&) = delete;
  ~SeqlockSlot() = default;

  /// Publishes a new value. Wait-free: never blocks, never retries, never
  /// waits for a reader. Exactly one thread or process may call this.
  // @satisfies REQ-IPC-006
  // @satisfies REQ-IPC-008
  void store(const T& value) noexcept {
    const std::uint64_t start = sequence_.load(std::memory_order_relaxed);

    // Odd: a write is in progress. Any reader sampling now will retry.
    //
    // seq_cst rather than a relaxed store followed by a release fence.
    //
    // The fence formulation is the textbook one and is what this originally
    // used. It was abandoned because **GCC's ThreadSanitizer cannot instrument
    // std::atomic_thread_fence** -- it says so via -Wtsan, and -Werror turns
    // that into a build failure. Suppressing the warning is not an option: GCC
    // is reporting that it cannot compile the construct correctly under
    // instrumentation, not that the construct is stylistically unwelcome.
    //
    // An implementation that cannot be built under the sanitiser that validates
    // it is not a good trade, so the ordering is expressed per-operation
    // instead. The cost is a full barrier on each of the two counter stores,
    // which at kilohertz cycle rates is far below the noise floor.
    // See ADR-0008.
    sequence_.store(start + 1, std::memory_order_seq_cst);

    std::array<std::uint64_t, kWordCount> staging{};
    // The explicit void* casts on both memcpy calls in this class silence
    // -Wclass-memaccess. GCC raises it whenever memcpy touches a class type
    // whose default constructor is non-trivial -- which a default member
    // initializer alone is enough to cause -- on the theory that the caller may
    // be bypassing construction.
    //
    // Not the case here: the static_assert at the top of the class is the real
    // guarantee, and trivially copyable is precisely the property that makes
    // memcpy defined for T. The cast tells the compiler the check has already
    // been made; it does not weaken anything, because the assert would have
    // fired first.
    std::memcpy(static_cast<void*>(staging.data()), static_cast<const void*>(&value),
                sizeof(T));
    for (std::size_t index = 0; index < kWordCount; ++index) {
      // Release, not relaxed.
      //
      // A seq_cst store is a *release* operation, and release constrains what
      // comes before it, never what comes after. So a relaxed word store could
      // legally be hoisted above the odd-counter store that precedes it, and a
      // reader could then observe new payload words while both of its counter
      // samples still read the old even value -- accepting a torn snapshot.
      //
      // Making each word store a release operation orders it after the marker
      // and closes that hole. x86-64 would never expose it (its store-store
      // ordering is already total, and a release store is a plain mov there),
      // but AArch64 would, and that is where this is headed.
      words_[index].store(staging[index], std::memory_order_release);
    }

    // Even again: the value is stable. seq_cst orders every word store above
    // before this becomes visible, so a reader that sees this counter sees all
    // of them.
    sequence_.store(start + 2, std::memory_order_seq_cst);
  }

  /// Reads the latest stable value.
  ///
  /// Returns false if `max_attempts` collisions occur without a clean read,
  /// rather than spinning forever. That bound is not a nicety: a writer that
  /// died mid-update -- a crashed peer process, in the shared-memory case --
  /// leaves the counter odd permanently, and an unbounded retry loop would hang
  /// the reader inside its own control cycle. Failing is recoverable; hanging
  /// on the real-time path is not.
  // @satisfies REQ-IPC-006
  // @satisfies REQ-IPC-007
  // @satisfies REQ-IPC-009
  [[nodiscard]] bool tryLoad(T& out, unsigned max_attempts = 32) const noexcept {
    for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
      const std::uint64_t before = sequence_.load(std::memory_order_seq_cst);
      if ((before & 1U) != 0U) {
        continue;  // a write is in flight
      }

      std::array<std::uint64_t, kWordCount> staging{};
      for (std::size_t index = 0; index < kWordCount; ++index) {
        // seq_cst, so these participate in the same total order as the two
        // counter samples that bracket them and cannot be sunk past the second.
        // An acquire load would not do: acquire constrains what follows it, and
        // what is needed here is that these do not drift later.
        //
        // On x86-64 a seq_cst load is a plain mov -- only seq_cst stores need a
        // barrier -- so the reader pays nothing for this.
        staging[index] = words_[index].load(std::memory_order_seq_cst);
      }

      // seq_cst on the second sample too: the word loads above must not be
      // sunk past this check, and an acquire load would not prevent that --
      // acquire constrains what follows it, not what precedes it.
      if (sequence_.load(std::memory_order_seq_cst) == before) {
        std::memcpy(static_cast<void*>(&out), static_cast<const void*>(staging.data()),
                    sizeof(T));
        return true;
      }
    }
    return false;
  }

  /// Number of completed writes. Lets a reader tell a fresh value from a
  /// repeat without comparing payloads, which matters when the payload is
  /// large or when equal values are meaningful.
  [[nodiscard]] std::uint64_t generation() const noexcept {
    return sequence_.load(std::memory_order_acquire) / 2;
  }

  /// True if a write was in progress at the moment of the call. Diagnostic
  /// only -- it is stale the instant it returns.
  [[nodiscard]] bool writeInProgress() const noexcept {
    return (sequence_.load(std::memory_order_relaxed) & 1U) != 0U;
  }

 private:
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "a lock-based atomic would make the writer non-wait-free");

  alignas(kCacheLineSize) mutable std::atomic<std::uint64_t> sequence_{0};
  alignas(kCacheLineSize) std::array<std::atomic<std::uint64_t>, kWordCount> words_{};
};

}  // namespace safeedge::ipc

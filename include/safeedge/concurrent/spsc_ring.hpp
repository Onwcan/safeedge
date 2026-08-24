// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace safeedge::concurrent {

/// Size of a cache line on the x86-64 and AArch64 targets this runtime ships to.
///
/// Deliberately a literal rather than std::hardware_destructive_interference_size:
/// that constant is part of the ABI, GCC warns when it is used across a
/// translation-unit boundary, and its value is fixed at compiler-build time
/// rather than at deployment. For a component that will be cross-compiled to an
/// edge device, a named constant we control is the honest choice. Revisit if a
/// target with 128-byte lines (Apple M-series, some POWER) ever appears.
inline constexpr std::size_t kCacheLineSize = 64;

/// A wait-free single-producer / single-consumer ring buffer.
///
/// Contract
/// --------
/// * Exactly one thread may call tryPush. Exactly one *other* thread may call
///   tryPop. Two producers, or two consumers, is undefined behaviour -- this is
///   not a general MPMC queue and does not pretend to be one.
/// * Both operations are wait-free: bounded instruction count, no locks, no
///   allocation, no syscalls, no unbounded retry loop. That is what makes it
///   legal on the real-time path, where a mutex could invert priorities and an
///   allocation could touch the kernel.
/// * Capacity must be a power of two, so the index wrap is a mask rather than a
///   division. On the hot path a `div` is roughly 20-40 cycles against 1 for
///   `and`, which matters inside a 1 ms control cycle.
///
/// Element storage is a plain std::array, constructed once when the ring is.
/// Slots are assigned over, never constructed or destroyed during operation --
/// no placement new, no manual lifetime management, and nothing that could
/// throw or allocate while the loop is running.
///
/// Memory ordering is argued in docs/adr/0002-memory-ordering-policy.md.
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2, "a ring smaller than two slots cannot buffer");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_nothrow_default_constructible_v<T>,
                "slots are constructed up front; construction must not throw");
  static_assert(std::is_nothrow_move_assignable_v<T> ||
                    std::is_nothrow_copy_assignable_v<T>,
                "handoff must not throw on the real-time path");

 public:
  using value_type = T;

  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;
  ~SpscRing() = default;

  /// Number of elements the ring can hold. All of them: the usual
  /// "sacrifice one slot to distinguish full from empty" trick is unnecessary
  /// because the positions below are free-running counters rather than
  /// pre-wrapped indices.
  static constexpr std::size_t capacity() noexcept { return Capacity; }

  /// Producer side. Returns false if the ring is full; never blocks.
  [[nodiscard]] bool tryPush(const T& value) noexcept { return emplaceFrom(value); }

  [[nodiscard]] bool tryPush(T&& value) noexcept { return emplaceFrom(std::move(value)); }

  /// Consumer side. Returns false if the ring is empty; never blocks.
  [[nodiscard]] bool tryPop(T& out) noexcept {
    const std::uint64_t read = consumer_.read_pos.load(std::memory_order_relaxed);

    // Fast path uses the cached producer position, so the common case never
    // touches the producer cache line at all. Only when the ring *looks* empty
    // do we pay for a cross-core load to refresh it.
    if (read == consumer_.cached_write_pos) {
      consumer_.cached_write_pos = producer_.write_pos.load(std::memory_order_acquire);
      if (read == consumer_.cached_write_pos) {
        return false;  // genuinely empty
      }
    }

    out = std::move(slots_[read & kIndexMask]);
    // Release: the producer must not observe this slot as reusable until the
    // move above has completed.
    consumer_.read_pos.store(read + 1, std::memory_order_release);
    return true;
  }

  /// Approximate occupancy. Safe to call from either side, but the value is
  /// stale the instant it is returned -- for diagnostics and metrics only,
  /// never for control flow.
  [[nodiscard]] std::size_t sizeApprox() const noexcept {
    const std::uint64_t write = producer_.write_pos.load(std::memory_order_acquire);
    const std::uint64_t read = consumer_.read_pos.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
  }

  [[nodiscard]] bool emptyApprox() const noexcept { return sizeApprox() == 0; }
  [[nodiscard]] bool fullApprox() const noexcept { return sizeApprox() >= Capacity; }

 private:
  static constexpr std::uint64_t kIndexMask = Capacity - 1;

  template <typename U>
  [[nodiscard]] bool emplaceFrom(U&& value) noexcept {
    const std::uint64_t write = producer_.write_pos.load(std::memory_order_relaxed);

    if (write - producer_.cached_read_pos >= Capacity) {
      producer_.cached_read_pos = consumer_.read_pos.load(std::memory_order_acquire);
      if (write - producer_.cached_read_pos >= Capacity) {
        return false;  // genuinely full
      }
    }

    slots_[write & kIndexMask] = std::forward<U>(value);
    // Release: the consumer must not observe this position until the write
    // above is visible. This store is the publication point.
    producer_.write_pos.store(write + 1, std::memory_order_release);
    return true;
  }

  // The producer and consumer state live on separate cache lines. Without the
  // padding the two counters share a line, every push invalidates the line the
  // consumer is polling, and throughput collapses under false sharing -- a
  // "lock-free" queue that is slower than a mutex. Benchmarked in
  // benchmarks/bench_spsc_ring.cpp.
  struct alignas(kCacheLineSize) ProducerState {
    std::atomic<std::uint64_t> write_pos{0};
    /// Producer-private snapshot of read_pos. Not atomic: only the producer
    /// reads or writes it.
    std::uint64_t cached_read_pos{0};
  };

  struct alignas(kCacheLineSize) ConsumerState {
    std::atomic<std::uint64_t> read_pos{0};
    /// Consumer-private snapshot of write_pos.
    std::uint64_t cached_write_pos{0};
  };

  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "a lock-based atomic here would make the queue non-wait-free");

  ProducerState producer_{};
  ConsumerState consumer_{};
  alignas(kCacheLineSize) std::array<T, Capacity> slots_{};
};

}  // namespace safeedge::concurrent

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace safeedge::rt {

/// A fixed-size, allocation-free latency histogram with logarithmic bucketing.
///
/// Why not just keep every sample in a vector
/// ------------------------------------------
/// Because recording happens on the real-time path. A vector would allocate on
/// growth, and one `malloc` inside a 1 ms cycle can cost more than the cycle
/// has left -- the measurement would then be creating the very jitter it exists
/// to measure. Storage here is a fixed array sized at compile time, and
/// `record()` is a handful of instructions with no branches on data size.
///
/// Why not a plain linear histogram
/// --------------------------------
/// Latency spans many orders of magnitude: a cycle overrun is interesting at
/// 100 ns and at 100 ms alike. A linear histogram fine enough to resolve
/// nanoseconds would need tens of millions of buckets to reach a second.
/// Logarithmic bucketing gives constant *relative* precision instead, which is
/// what matters when the question is "how bad is the tail".
///
/// Layout (the HdrHistogram idea, simplified)
/// ------------------------------------------
/// * Values below `kLinearLimit` (64 ns) are stored exactly, one bucket each.
///   Sub-microsecond figures are where the interesting detail lives and there
///   are few enough of them to afford exactness.
/// * Above that, each power of two is split into `kSubBucketCount` (32) linear
///   sub-buckets, so relative precision is a constant ~3.1% across the range.
///
/// Percentiles are reported as the **upper** bound of the containing bucket:
/// for a latency budget, rounding up is the conservative direction. A reported
/// p99.9 is therefore a value the true p99.9 is guaranteed not to exceed.
///
/// Not thread-safe. One histogram per thread; merge afterwards with `merge()`.
class LatencyHistogram {
 public:
  /// 32 sub-buckets per octave => worst-case relative error 1/32.
  static constexpr int kSubBucketBits = 5;
  static constexpr std::uint64_t kSubBucketCount = 1ULL << kSubBucketBits;
  /// Below this, buckets are exact single values.
  static constexpr std::uint64_t kLinearLimit = kSubBucketCount * 2;
  /// Shift can run from 1 (values just above kLinearLimit) to 58 (values near
  /// 2^63), giving 58 octaves above the linear region.
  static constexpr std::size_t kOctaves = 58;
  static constexpr std::size_t kBucketCount =
      static_cast<std::size_t>(kLinearLimit) + (kOctaves * kSubBucketCount);

  LatencyHistogram() = default;

  /// Records one sample. Allocation-free, branch-light, O(1).
  /// Negative inputs are clamped to zero -- a negative latency means the clock
  /// went backwards, which is worth counting but not worth crashing over.
  void record(std::int64_t value) noexcept;

  /// Records `count` occurrences of the same value. Used by merge().
  void recordMany(std::int64_t value, std::uint64_t count) noexcept;

  /// Folds another histogram into this one. Both must use the same layout,
  /// which the type system guarantees.
  void merge(const LatencyHistogram& other) noexcept;

  void reset() noexcept;

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] std::int64_t min() const noexcept;
  [[nodiscard]] std::int64_t max() const noexcept { return max_; }
  /// Exact, accumulated from the raw samples rather than the buckets, so it is
  /// not subject to bucketing error.
  [[nodiscard]] double mean() const noexcept;

  /// Upper bound of the bucket containing the requested quantile.
  /// `quantile` is in [0, 1]; values outside are clamped.
  [[nodiscard]] std::int64_t percentile(double quantile) const noexcept;

  [[nodiscard]] std::int64_t p50() const noexcept { return percentile(0.50); }
  [[nodiscard]] std::int64_t p90() const noexcept { return percentile(0.90); }
  [[nodiscard]] std::int64_t p99() const noexcept { return percentile(0.99); }
  [[nodiscard]] std::int64_t p999() const noexcept { return percentile(0.999); }

  /// Bucket index a value would land in. Exposed for testing the layout.
  [[nodiscard]] static std::size_t bucketIndexFor(std::uint64_t value) noexcept;
  /// Inclusive upper bound of the values a bucket accepts.
  [[nodiscard]] static std::uint64_t bucketUpperBound(std::size_t index) noexcept;

 private:
  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t count_{0};
  double sum_{0.0};
  std::int64_t max_{0};
  std::int64_t min_{INT64_MAX};
};

}  // namespace safeedge::rt

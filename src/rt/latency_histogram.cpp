// SPDX-License-Identifier: Apache-2.0
#include "safeedge/rt/latency_histogram.hpp"

#include <algorithm>
#include <bit>

namespace safeedge::rt {

std::size_t LatencyHistogram::bucketIndexFor(std::uint64_t value) noexcept {
  if (value < kLinearLimit) {
    // Exact region: one bucket per nanosecond.
    return static_cast<std::size_t>(value);
  }
  // Position of the most significant set bit. value >= 64 here, so exponent
  // is at least 6 and the shift below is at least 1.
  const int exponent = 63 - std::countl_zero(value);
  const int shift = exponent - kSubBucketBits;
  // value >> shift lands in [kSubBucketCount, 2*kSubBucketCount), so masking
  // off the implicit leading bit leaves a sub-bucket in [0, kSubBucketCount).
  const std::uint64_t sub = (value >> shift) & (kSubBucketCount - 1);
  const auto octave = static_cast<std::uint64_t>(shift - 1);
  return static_cast<std::size_t>(kLinearLimit + (octave * kSubBucketCount) + sub);
}

std::uint64_t LatencyHistogram::bucketUpperBound(std::size_t index) noexcept {
  if (index < kLinearLimit) {
    return index;
  }
  const std::uint64_t offset = index - kLinearLimit;
  const auto shift = static_cast<int>((offset / kSubBucketCount) + 1);
  const std::uint64_t sub = offset % kSubBucketCount;
  const std::uint64_t lower = (kSubBucketCount + sub) << shift;
  // Inclusive upper bound: the last value that still maps to this bucket.
  return lower + (1ULL << shift) - 1;
}

void LatencyHistogram::recordMany(std::int64_t value,
                                  std::uint64_t occurrences) noexcept {
  if (occurrences == 0) {
    return;
  }
  // A negative sample means the clock ran backwards between two reads. That is
  // worth counting -- it says something real about the platform -- but it is
  // not a latency, so it is clamped rather than dropped or trusted.
  const std::int64_t clamped = std::max<std::int64_t>(value, 0);

  const std::size_t index = bucketIndexFor(static_cast<std::uint64_t>(clamped));
  buckets_[index] += occurrences;
  count_ += occurrences;
  sum_ += static_cast<double>(clamped) * static_cast<double>(occurrences);
  max_ = std::max(max_, clamped);
  min_ = std::min(min_, clamped);
}

void LatencyHistogram::record(std::int64_t value) noexcept { recordMany(value, 1); }

void LatencyHistogram::merge(const LatencyHistogram& other) noexcept {
  if (other.count_ == 0) {
    return;
  }
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    buckets_[i] += other.buckets_[i];
  }
  count_ += other.count_;
  sum_ += other.sum_;
  max_ = std::max(max_, other.max_);
  min_ = std::min(min_, other.min_);
}

void LatencyHistogram::reset() noexcept {
  buckets_.fill(0);
  count_ = 0;
  sum_ = 0.0;
  max_ = 0;
  min_ = INT64_MAX;
}

std::int64_t LatencyHistogram::min() const noexcept { return count_ == 0 ? 0 : min_; }

double LatencyHistogram::mean() const noexcept {
  // Accumulated from raw samples, not from bucket midpoints, so the mean is
  // exact even though the percentiles are quantised.
  return count_ == 0 ? 0.0 : sum_ / static_cast<double>(count_);
}

std::int64_t LatencyHistogram::percentile(double quantile) const noexcept {
  if (count_ == 0) {
    return 0;
  }
  const double q = std::clamp(quantile, 0.0, 1.0);

  // Rank of the sample we are looking for, 1-based. Rounding up means p100
  // resolves to the last sample rather than one short of it, and p0 to the
  // first.
  auto target = static_cast<std::uint64_t>(q * static_cast<double>(count_) + 0.5);
  target = std::clamp<std::uint64_t>(target, 1, count_);

  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    seen += buckets_[i];
    if (seen >= target) {
      // Upper bound rather than lower: for a latency budget, erring high is
      // the safe direction. The reported figure is one the true percentile is
      // guaranteed not to exceed.
      const auto bound = static_cast<std::int64_t>(bucketUpperBound(i));
      // Never report more than the largest sample actually seen.
      return std::min(bound, max_);
    }
  }
  return max_;
}

}  // namespace safeedge::rt

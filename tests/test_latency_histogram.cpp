// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "safeedge/rt/latency_histogram.hpp"
#include "safeedge/rt/no_alloc_guard.hpp"

namespace safeedge::rt {
namespace {

// ---------------------------------------------------------------------------
// Bucket layout
// ---------------------------------------------------------------------------

TEST(LatencyHistogram, SmallValuesAreStoredExactly) {
  // Sub-microsecond detail is where the interesting behaviour lives, so the
  // bottom of the range is not quantised at all.
  for (std::uint64_t v = 0; v < LatencyHistogram::kLinearLimit; ++v) {
    EXPECT_EQ(LatencyHistogram::bucketIndexFor(v), v) << "value " << v;
    EXPECT_EQ(LatencyHistogram::bucketUpperBound(static_cast<std::size_t>(v)), v);
  }
}

TEST(LatencyHistogram, BucketIndexIsMonotonicInValue) {
  // A non-monotonic mapping would put a larger latency in an earlier bucket
  // and silently corrupt every percentile. Swept densely at the bottom and
  // across every octave boundary above it.
  std::size_t previous = 0;
  for (std::uint64_t v = 0; v < 100'000; ++v) {
    const std::size_t index = LatencyHistogram::bucketIndexFor(v);
    ASSERT_GE(index, previous) << "index went backwards at value " << v;
    previous = index;
  }
  for (int exponent = 17; exponent < 62; ++exponent) {
    const std::uint64_t base = 1ULL << exponent;
    for (std::uint64_t delta = 0; delta < 4; ++delta) {
      const std::size_t index = LatencyHistogram::bucketIndexFor(base + delta);
      ASSERT_GE(index, previous) << "index went backwards near 2^" << exponent;
      previous = index;
    }
  }
}

TEST(LatencyHistogram, EveryValueLandsWithinItsBucketBounds) {
  for (std::uint64_t v = 0; v < 200'000; ++v) {
    const std::size_t index = LatencyHistogram::bucketIndexFor(v);
    EXPECT_GE(LatencyHistogram::bucketUpperBound(index), v) << "value " << v;
  }
}

TEST(LatencyHistogram, RelativePrecisionHoldsAcrossTheWholeRange) {
  // The design claim is constant relative precision of 1/kSubBucketCount.
  // This asserts it directly rather than trusting the arithmetic.
  for (int exponent = 6; exponent < 62; ++exponent) {
    const std::uint64_t base = 1ULL << exponent;
    for (const std::uint64_t v : {base, base + 1, base + (base / 3), (base * 2) - 1}) {
      const std::size_t index = LatencyHistogram::bucketIndexFor(v);
      const std::uint64_t upper = LatencyHistogram::bucketUpperBound(index);
      ASSERT_GE(upper, v);
      ASSERT_LE((upper - v) * LatencyHistogram::kSubBucketCount, v)
          << "precision worse than 1/" << LatencyHistogram::kSubBucketCount
          << " at value " << v;
    }
  }
}

TEST(LatencyHistogram, LargestRepresentableValueStaysInBounds) {
  // An index that ran off the end of the array would be a silent buffer
  // overrun on the real-time path.
  const std::uint64_t huge = (1ULL << 62) - 1;
  EXPECT_LT(LatencyHistogram::bucketIndexFor(huge), LatencyHistogram::kBucketCount);
  EXPECT_LT(LatencyHistogram::bucketIndexFor(INT64_MAX), LatencyHistogram::kBucketCount);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

TEST(LatencyHistogram, EmptyHistogramReportsZeros) {
  const LatencyHistogram h;
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.min(), 0);
  EXPECT_EQ(h.max(), 0);
  EXPECT_DOUBLE_EQ(h.mean(), 0.0);
  EXPECT_EQ(h.p50(), 0);
  EXPECT_EQ(h.p999(), 0);
}

TEST(LatencyHistogram, ExactInTheLinearRegion) {
  LatencyHistogram h;
  for (int i = 1; i <= 50; ++i) {
    h.record(i);
  }
  EXPECT_EQ(h.count(), 50u);
  EXPECT_EQ(h.min(), 1);
  EXPECT_EQ(h.max(), 50);
  EXPECT_DOUBLE_EQ(h.mean(), 25.5);
  EXPECT_EQ(h.p50(), 25);
  EXPECT_EQ(h.percentile(1.0), 50);
}

TEST(LatencyHistogram, PercentileUsesTheNearestRankCeiling) {
  LatencyHistogram h;
  h.record(10);
  h.record(20);
  h.record(30);

  // ceil(0.34 * 3) == 2. Rounding to the nearest integer would incorrectly
  // select the first sample and under-report the percentile.
  EXPECT_EQ(h.percentile(0.34), 20);
}

TEST(LatencyHistogram, PercentilesTrackAKnownDistribution) {
  // 990 samples at 1000 ns and 10 at 500000 ns. p50 and p90 must land on the
  // body, p99.9 on the tail -- the shape a real cycle-time histogram has.
  LatencyHistogram h;
  h.recordMany(1000, 990);
  h.recordMany(500'000, 10);

  EXPECT_NEAR(static_cast<double>(h.p50()), 1000.0, 1000.0 / 32.0 + 1);
  EXPECT_NEAR(static_cast<double>(h.p90()), 1000.0, 1000.0 / 32.0 + 1);
  EXPECT_GE(h.p999(), 400'000) << "the tail must survive into p99.9";
  EXPECT_EQ(h.max(), 500'000);
}

TEST(LatencyHistogram, PercentileIsNeverBelowTheTrueValue) {
  // The contract: a reported percentile is an upper bound the true percentile
  // cannot exceed. Checked against a brute-force computation over the raw
  // samples.
  std::mt19937 rng(0x5AFEED9E);
  std::lognormal_distribution<double> dist(7.0, 1.4);

  std::vector<std::int64_t> raw;
  LatencyHistogram h;
  raw.reserve(20'000);
  for (int i = 0; i < 20'000; ++i) {
    const auto sample = static_cast<std::int64_t>(dist(rng));
    raw.push_back(sample);
    h.record(sample);
  }
  std::sort(raw.begin(), raw.end());

  for (const double q : {0.5, 0.9, 0.99, 0.999}) {
    const auto rank = static_cast<std::size_t>(q * static_cast<double>(raw.size() - 1));
    const std::int64_t truth = raw[rank];
    const std::int64_t reported = h.percentile(q);
    EXPECT_GE(reported, truth) << "q=" << q << " under-reported";
    // And it must not be wildly above either -- one bucket of slack.
    EXPECT_LE(reported, truth + (truth / 16) + 2) << "q=" << q << " over-reported";
  }
}

TEST(LatencyHistogram, PercentileNeverExceedsTheLargestSample) {
  // Bucket upper bounds can exceed any observed value; reporting one would be
  // technically defensible and practically confusing.
  LatencyHistogram h;
  h.record(1'000'000);
  EXPECT_EQ(h.percentile(1.0), 1'000'000);
  EXPECT_LE(h.p999(), h.max());
}

TEST(LatencyHistogram, NegativeSamplesAreClampedNotTrusted) {
  // A negative delta means the clock ran backwards. Worth counting, not worth
  // treating as a latency and not worth crashing over.
  LatencyHistogram h;
  h.record(-500);
  h.record(100);
  EXPECT_EQ(h.count(), 2u);
  EXPECT_EQ(h.min(), 0);
  EXPECT_EQ(h.max(), 100);
}

TEST(LatencyHistogram, MergeCombinesCountsAndExtremes) {
  LatencyHistogram a;
  LatencyHistogram b;
  a.recordMany(100, 10);
  b.recordMany(9000, 5);
  b.record(3);

  a.merge(b);
  EXPECT_EQ(a.count(), 16u);
  EXPECT_EQ(a.min(), 3);
  EXPECT_EQ(a.max(), 9000);
}

TEST(LatencyHistogram, MergingAnEmptyHistogramChangesNothing) {
  LatencyHistogram a;
  a.recordMany(42, 7);
  const LatencyHistogram empty;
  a.merge(empty);
  EXPECT_EQ(a.count(), 7u);
  EXPECT_EQ(a.min(), 42);
  EXPECT_EQ(a.max(), 42);
}

TEST(LatencyHistogram, ResetReturnsItToTheEmptyState) {
  LatencyHistogram h;
  h.recordMany(1234, 100);
  h.reset();
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.min(), 0);
  EXPECT_EQ(h.max(), 0);
  EXPECT_EQ(h.p50(), 0);
}

TEST(LatencyHistogram, RecordManyMatchesRepeatedRecord) {
  LatencyHistogram bulk;
  LatencyHistogram individual;
  bulk.recordMany(777, 1000);
  for (int i = 0; i < 1000; ++i) {
    individual.record(777);
  }
  EXPECT_EQ(bulk.count(), individual.count());
  EXPECT_EQ(bulk.p50(), individual.p50());
  EXPECT_DOUBLE_EQ(bulk.mean(), individual.mean());
}

// ---------------------------------------------------------------------------
// The property the real-time path actually depends on
// ---------------------------------------------------------------------------

TEST(LatencyHistogram, RecordingDoesNotAllocate) {
  // The entire reason this class exists instead of a std::vector of samples.
  // Verified rather than asserted in a comment: the guard aborts on any
  // allocation between the braces.
  ASSERT_TRUE(guardIsInstalled());
  setAllocationPolicy(AllocationPolicy::kCount);
  resetAllocationReport();

  LatencyHistogram h;  // constructed outside the scope
  {
    const NoAllocScope no_alloc;
    for (int i = 0; i < 10'000; ++i) {
      h.record(i * 37);
    }
    h.record(-1);
    h.recordMany(999'999, 50);
  }

  EXPECT_EQ(allocationReport().violations, 0u)
      << "record() allocated, which defeats the purpose of the class";
  EXPECT_EQ(h.count(), 10'051u);
}

TEST(LatencyHistogram, ReadingStatisticsDoesNotAllocate) {
  // Percentile queries run on the reporting thread, not the RT thread, but a
  // supervisor may want them inside a cycle to decide on a safe-state
  // transition. Cheap to guarantee, so guarantee it.
  setAllocationPolicy(AllocationPolicy::kCount);
  resetAllocationReport();

  LatencyHistogram h;
  h.recordMany(4242, 500);

  std::int64_t sink = 0;
  {
    const NoAllocScope no_alloc;
    sink += h.p50() + h.p90() + h.p99() + h.p999();
    sink += h.min() + h.max() + static_cast<std::int64_t>(h.mean());
  }

  EXPECT_EQ(allocationReport().violations, 0u);
  EXPECT_GT(sink, 0);
}

}  // namespace
}  // namespace safeedge::rt

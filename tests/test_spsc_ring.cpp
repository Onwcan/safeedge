// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

#include "safeedge/concurrent/spsc_ring.hpp"

namespace safeedge::concurrent {
namespace {

// ---------------------------------------------------------------------------
// Single-threaded behaviour
// ---------------------------------------------------------------------------

TEST(SpscRing, StartsEmpty) {
  SpscRing<int, 8> ring;
  EXPECT_TRUE(ring.emptyApprox());
  EXPECT_EQ(ring.sizeApprox(), 0u);
  int out = -1;
  EXPECT_FALSE(ring.tryPop(out));
  EXPECT_EQ(out, -1) << "a failed pop must not touch the output";
}

TEST(SpscRing, PreservesFifoOrder) {
  SpscRing<int, 8> ring;
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(ring.tryPush(i));
  }
  for (int i = 0; i < 8; ++i) {
    int out = -1;
    ASSERT_TRUE(ring.tryPop(out));
    EXPECT_EQ(out, i);
  }
}

TEST(SpscRing, UsesEverySlotNotCapacityMinusOne) {
  // The free-running-counter design gives the full declared capacity. An
  // implementation that pre-wraps its indices has to sacrifice one slot to
  // tell full from empty; this asserts we did not.
  SpscRing<int, 4> ring;
  EXPECT_EQ(ring.capacity(), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.tryPush(i)) << "slot " << i << " should be available";
  }
  EXPECT_FALSE(ring.tryPush(99)) << "ring must be full at declared capacity";
  EXPECT_TRUE(ring.fullApprox());
}

TEST(SpscRing, RejectsPushWhenFullWithoutCorruptingContents) {
  SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.tryPush(i));
  }
  for (int i = 0; i < 20; ++i) {
    EXPECT_FALSE(ring.tryPush(1000 + i));
  }
  for (int i = 0; i < 4; ++i) {
    int out = -1;
    ASSERT_TRUE(ring.tryPop(out));
    EXPECT_EQ(out, i) << "rejected pushes must not have overwritten a slot";
  }
}

TEST(SpscRing, WrapsAroundIndefinitely) {
  // Drives the index mask many times past the physical array length. An
  // off-by-one in the wrap shows up here and nowhere else.
  SpscRing<std::uint64_t, 4> ring;
  for (std::uint64_t i = 0; i < 10'000; ++i) {
    ASSERT_TRUE(ring.tryPush(i));
    std::uint64_t out = 0;
    ASSERT_TRUE(ring.tryPop(out));
    ASSERT_EQ(out, i);
  }
  EXPECT_TRUE(ring.emptyApprox());
}

TEST(SpscRing, InterleavedPartialFillsStayOrdered) {
  SpscRing<int, 8> ring;
  int expected_next = 0;
  int to_push = 0;
  for (int round = 0; round < 500; ++round) {
    const int burst = (round % 5) + 1;
    for (int i = 0; i < burst; ++i) {
      if (ring.tryPush(to_push)) {
        ++to_push;
      }
    }
    const int drain = (round % 3) + 1;
    for (int i = 0; i < drain; ++i) {
      int out = -1;
      if (ring.tryPop(out)) {
        ASSERT_EQ(out, expected_next);
        ++expected_next;
      }
    }
  }
  EXPECT_GT(expected_next, 0);
}

TEST(SpscRing, SizeApproxTracksOccupancy) {
  SpscRing<int, 8> ring;
  EXPECT_EQ(ring.sizeApprox(), 0u);
  ASSERT_TRUE(ring.tryPush(1));
  ASSERT_TRUE(ring.tryPush(2));
  EXPECT_EQ(ring.sizeApprox(), 2u);
  int out = 0;
  ASSERT_TRUE(ring.tryPop(out));
  EXPECT_EQ(ring.sizeApprox(), 1u);
}

// ---------------------------------------------------------------------------
// Element types
// ---------------------------------------------------------------------------

/// Stands in for the setpoint struct the executor will actually carry.
struct Setpoint {
  std::uint64_t cycle{0};
  double joint[6]{};
};

TEST(SpscRing, CarriesAggregateTypes) {
  SpscRing<Setpoint, 16> ring;
  Setpoint in;
  in.cycle = 42;
  in.joint[3] = 1.5;
  ASSERT_TRUE(ring.tryPush(in));

  Setpoint out;
  ASSERT_TRUE(ring.tryPop(out));
  EXPECT_EQ(out.cycle, 42u);
  EXPECT_DOUBLE_EQ(out.joint[3], 1.5);
}

TEST(SpscRing, MovesRatherThanCopiesWhenGivenAnRvalue) {
  SpscRing<std::unique_ptr<int>, 4> ring;
  auto owned = std::make_unique<int>(7);
  const int* raw = owned.get();
  ASSERT_TRUE(ring.tryPush(std::move(owned)));
  EXPECT_EQ(owned, nullptr) << "push of an rvalue should have moved from it";

  std::unique_ptr<int> out;
  ASSERT_TRUE(ring.tryPop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out.get(), raw) << "the same object should come out, not a copy";
  EXPECT_EQ(*out, 7);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

TEST(SpscRing, ProducerAndConsumerStateAreOnDifferentCacheLines) {
  // The whole performance argument for this class rests on the padding. If a
  // refactor drops the alignas, throughput collapses under false sharing while
  // every functional test above still passes -- so the layout is asserted
  // directly rather than left to a benchmark nobody runs.
  SpscRing<int, 8> ring;
  const auto base = reinterpret_cast<std::uintptr_t>(&ring);
  EXPECT_EQ(base % kCacheLineSize, 0u) << "ring must be cache-line aligned";
  EXPECT_GE(sizeof(SpscRing<int, 8>), 3 * kCacheLineSize)
      << "producer state, consumer state and slots must not share a line";
}

// ---------------------------------------------------------------------------
// Concurrency -- these are the tests TSan exists to run
// ---------------------------------------------------------------------------

TEST(SpscRing, ConcurrentHandoffLosesNothingAndReordersNothing) {
  // A real producer and a real consumer on separate threads, spinning against
  // each other. Every value pushed must arrive exactly once, in order. Under
  // TSan this also proves the acquire/release pairing is sufficient: a missing
  // release would be reported as a data race on the slot itself.
  constexpr std::uint64_t kItems = 200'000;
  SpscRing<std::uint64_t, 1024> ring;
  std::atomic<bool> consumer_ready{false};

  std::vector<std::uint64_t> received;
  received.reserve(kItems);

  std::thread consumer([&] {
    consumer_ready.store(true, std::memory_order_release);
    std::uint64_t out = 0;
    while (received.size() < kItems) {
      if (ring.tryPop(out)) {
        received.push_back(out);
      }
    }
  });

  while (!consumer_ready.load(std::memory_order_acquire)) {
    // Deliberately spin rather than sleep: we want the two threads genuinely
    // overlapping, not serialised by a scheduler nap.
  }

  for (std::uint64_t i = 0; i < kItems; ++i) {
    while (!ring.tryPush(i)) {
      // Ring full -- retry. Backpressure is the caller's problem by design.
    }
  }
  consumer.join();

  ASSERT_EQ(received.size(), kItems) << "items were lost";
  for (std::uint64_t i = 0; i < kItems; ++i) {
    ASSERT_EQ(received[i], i) << "reordering or duplication at index " << i;
  }
}

TEST(SpscRing, ConcurrentHandoffSurvivesAConsumerThatFallsBehind) {
  // Exercises the full/empty boundaries repeatedly by making the consumer
  // deliberately slower than the producer, so the ring spends most of its life
  // saturated. That is where a wrap or cache-refresh bug actually shows up.
  constexpr std::uint64_t kItems = 50'000;
  SpscRing<std::uint64_t, 16> ring;  // small on purpose

  std::vector<std::uint64_t> received;
  received.reserve(kItems);

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      while (!ring.tryPush(i)) {
      }
    }
  });

  std::uint64_t out = 0;
  std::uint64_t spin_budget = 0;
  while (received.size() < kItems) {
    if (ring.tryPop(out)) {
      received.push_back(out);
      // Burn a little time so the producer stays ahead.
      for (spin_budget = 0; spin_budget < 40; ++spin_budget) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
      }
    }
  }
  producer.join();

  ASSERT_EQ(received.size(), kItems);
  for (std::uint64_t i = 0; i < kItems; ++i) {
    ASSERT_EQ(received[i], i);
  }
}

TEST(SpscRing, ConcurrentHandoffSurvivesAProducerThatFallsBehind) {
  // The mirror case: the ring spends most of its life empty, so the consumer
  // repeatedly hits the cache-refresh path in tryPop.
  constexpr std::uint64_t kItems = 50'000;
  SpscRing<std::uint64_t, 16> ring;

  std::vector<std::uint64_t> received;
  received.reserve(kItems);

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      for (std::uint64_t s = 0; s < 40; ++s) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
      }
      while (!ring.tryPush(i)) {
      }
    }
  });

  std::uint64_t out = 0;
  while (received.size() < kItems) {
    if (ring.tryPop(out)) {
      received.push_back(out);
    }
  }
  producer.join();

  ASSERT_EQ(received.size(), kItems);
  for (std::uint64_t i = 0; i < kItems; ++i) {
    ASSERT_EQ(received[i], i);
  }
}

TEST(SpscRing, PayloadContentsSurviveTheHandoffIntact) {
  // Ordering is not enough: the *contents* of a slot must be fully visible to
  // the consumer before the position advances. A payload wider than a machine
  // word makes a torn or partially published write detectable.
  constexpr std::uint64_t kItems = 100'000;
  SpscRing<Setpoint, 256> ring;

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      Setpoint sp;
      sp.cycle = i;
      for (int j = 0; j < 6; ++j) {
        sp.joint[j] = static_cast<double>(i) + (j * 0.125);
      }
      while (!ring.tryPush(sp)) {
      }
    }
  });

  Setpoint out;
  for (std::uint64_t i = 0; i < kItems; ++i) {
    while (!ring.tryPop(out)) {
    }
    ASSERT_EQ(out.cycle, i);
    for (int j = 0; j < 6; ++j) {
      ASSERT_DOUBLE_EQ(out.joint[j], static_cast<double>(i) + (j * 0.125))
          << "torn payload at cycle " << i << " joint " << j;
    }
  }
  producer.join();
}

}  // namespace
}  // namespace safeedge::concurrent

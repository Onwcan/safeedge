// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "safeedge/ipc/seqlock_slot.hpp"
#include "safeedge/ipc/shared_memory.hpp"
#include "safeedge/rt/no_alloc_guard.hpp"

namespace safeedge::ipc {
namespace {

/// A payload wide enough that a torn read is detectable.
///
/// Every field carries the same generation number and the last is their sum.
/// A reader that observed part of one write and part of another sees fields
/// that disagree -- which a single-word payload could never reveal, because a
/// single word is written atomically on every architecture here and would make
/// the test pass regardless of whether the seqlock worked at all.
struct WidePayload {
  std::uint64_t generation{0};
  std::uint64_t copy_a{0};
  std::uint64_t copy_b{0};
  std::uint64_t copy_c{0};
  double joint[6]{};
  std::uint64_t checksum{0};

  static WidePayload of(std::uint64_t value) noexcept {
    WidePayload payload;
    payload.generation = value;
    payload.copy_a = value;
    payload.copy_b = value;
    payload.copy_c = value;
    for (int index = 0; index < 6; ++index) {
      payload.joint[index] = static_cast<double>(value) + (index * 0.125);
    }
    payload.checksum = value * 4;
    return payload;
  }

  [[nodiscard]] bool consistent() const noexcept {
    if (copy_a != generation || copy_b != generation || copy_c != generation) {
      return false;
    }
    if (checksum != generation * 4) {
      return false;
    }
    for (int index = 0; index < 6; ++index) {
      if (joint[index] != static_cast<double>(generation) + (index * 0.125)) {
        return false;
      }
    }
    return true;
  }
};

// ---------------------------------------------------------------------------
// Single-threaded behaviour
// ---------------------------------------------------------------------------

TEST(SeqlockSlot, ReadsBackWhatWasWritten) {
  SeqlockSlot<WidePayload> slot;
  slot.store(WidePayload::of(42));

  WidePayload out;
  ASSERT_TRUE(slot.tryLoad(out));
  EXPECT_EQ(out.generation, 42u);
  EXPECT_TRUE(out.consistent());
}

TEST(SeqlockSlot, ReadsTheInitialValueBeforeAnyWrite) {
  const SeqlockSlot<WidePayload> slot;
  WidePayload out = WidePayload::of(999);
  ASSERT_TRUE(slot.tryLoad(out));
  EXPECT_EQ(out.generation, 0u) << "an untouched slot must read as zero, not as garbage";
}

TEST(SeqlockSlot, KeepsOnlyTheLatestValue) {
  // The defining property. This is not a queue: intermediate values are not
  // retained, and that is the point rather than a limitation.
  SeqlockSlot<WidePayload> slot;
  for (std::uint64_t value = 1; value <= 1000; ++value) {
    slot.store(WidePayload::of(value));
  }
  WidePayload out;
  ASSERT_TRUE(slot.tryLoad(out));
  EXPECT_EQ(out.generation, 1000u);
}

TEST(SeqlockSlot, GenerationCountsCompletedWrites) {
  SeqlockSlot<WidePayload> slot;
  EXPECT_EQ(slot.generation(), 0u);
  slot.store(WidePayload::of(1));
  EXPECT_EQ(slot.generation(), 1u);
  slot.store(WidePayload::of(2));
  EXPECT_EQ(slot.generation(), 2u);
}

TEST(SeqlockSlot, GenerationDistinguishesARepeatFromAFreshWrite) {
  // Two identical values are indistinguishable by payload. A reader that needs
  // to know whether anything happened has to look at the generation.
  SeqlockSlot<WidePayload> slot;
  slot.store(WidePayload::of(7));
  const std::uint64_t first = slot.generation();
  slot.store(WidePayload::of(7));
  EXPECT_GT(slot.generation(), first);
}

TEST(SeqlockSlot, AnUncontendedSlotIsNotWriting) {
  SeqlockSlot<WidePayload> slot;
  slot.store(WidePayload::of(3));
  EXPECT_FALSE(slot.writeInProgress());
}

TEST(SeqlockSlot, ZeroAttemptsFailsRatherThanReadingAnyway) {
  // @verifies REQ-IPC-007
  // The retry budget is a real bound, not advisory.
  SeqlockSlot<WidePayload> slot;
  slot.store(WidePayload::of(5));
  WidePayload out;
  EXPECT_FALSE(slot.tryLoad(out, 0));
}

TEST(SeqlockSlot, HandlesPayloadsThatAreNotAWholeNumberOfWords) {
  // sizeof is 12, so the word array holds two words with four bytes of padding
  // the memcpy must not read past.
  struct Odd {
    std::uint32_t a{0};
    std::uint32_t b{0};
    std::uint32_t c{0};
  };
  static_assert(sizeof(Odd) == 12);

  SeqlockSlot<Odd> slot;
  slot.store(Odd{1, 2, 3});
  Odd out{};
  ASSERT_TRUE(slot.tryLoad(out));
  EXPECT_EQ(out.a, 1u);
  EXPECT_EQ(out.b, 2u);
  EXPECT_EQ(out.c, 3u);
}

TEST(SeqlockSlot, HandlesASingleByte) {
  SeqlockSlot<std::uint8_t> slot;
  slot.store(0xAB);
  std::uint8_t out = 0;
  ASSERT_TRUE(slot.tryLoad(out));
  EXPECT_EQ(out, 0xAB);
}

// ---------------------------------------------------------------------------
// Concurrency -- what TSan is here to check
// ---------------------------------------------------------------------------

TEST(SeqlockSlot, ConcurrentReadersNeverObserveATornValue) {
  // @verifies REQ-IPC-006
  // The property the whole design exists for. One writer publishing as fast as
  // it can, several readers sampling as fast as they can. Every value a reader
  // accepts must be internally consistent -- never half of one write and half
  // of the next.
  //
  // Under TSan this is also the check that the word-wise relaxed-atomic payload
  // is genuinely race-free. A textbook seqlock, which reads the payload with
  // plain loads while the writer may be writing it, is reported as a data race
  // here -- correctly, because it is one.
  constexpr std::uint64_t kWrites = 200'000;
  constexpr int kReaders = 3;

  SeqlockSlot<WidePayload> slot;
  // Seed a consistent value before any reader starts.
  //
  // A zero-initialised slot is a legitimate thing for a reader to observe --
  // it is the state before the first write -- but WidePayload::of(0) sets
  // joint[i] = i * 0.125, so all-zeroes is not a value this payload type can
  // produce. Without the seed, readers that start before the first store
  // observe the pristine state and correctly report it as inconsistent.
  //
  // That is worth stating rather than just fixing, because it is a real
  // constraint on anything placed in a seqlock: **the zero value must be a
  // valid value for the type**, since a reader can always observe it.
  slot.store(WidePayload::of(0));

  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> torn{0};
  std::atomic<std::uint64_t> reads{0};

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&] {
      WidePayload observed;
      while (running.load(std::memory_order_relaxed)) {
        if (slot.tryLoad(observed)) {
          reads.fetch_add(1, std::memory_order_relaxed);
          if (!observed.consistent()) {
            torn.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  for (std::uint64_t value = 1; value <= kWrites; ++value) {
    slot.store(WidePayload::of(value));
  }
  running.store(false, std::memory_order_relaxed);
  for (std::thread& reader : readers) {
    reader.join();
  }

  EXPECT_EQ(torn.load(), 0u) << "a reader accepted a torn value";
  EXPECT_GT(reads.load(), 0u) << "no reader ever completed a read";
}

TEST(SeqlockSlot, ValuesObservedByAReaderNeverGoBackwards) {
  // @verifies REQ-IPC-009
  // A seqlock may skip values, never rewind. A reader that saw generation 500
  // must never subsequently accept 400 -- that would mean it read a slot the
  // writer had already moved past.
  constexpr std::uint64_t kWrites = 100'000;
  SeqlockSlot<WidePayload> slot;
  slot.store(WidePayload::of(0));  // see the note in the torn-value test
  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> regressions{0};

  std::thread reader([&] {
    WidePayload observed;
    std::uint64_t highest = 0;
    while (running.load(std::memory_order_relaxed)) {
      if (slot.tryLoad(observed)) {
        if (observed.generation < highest) {
          regressions.fetch_add(1, std::memory_order_relaxed);
        }
        highest = observed.generation;
      }
    }
  });

  for (std::uint64_t value = 1; value <= kWrites; ++value) {
    slot.store(WidePayload::of(value));
  }
  running.store(false, std::memory_order_relaxed);
  reader.join();

  EXPECT_EQ(regressions.load(), 0u);
}

// ---------------------------------------------------------------------------
// In shared memory, across processes
// ---------------------------------------------------------------------------

TEST(SeqlockSlot, WorksInSharedMemoryAcrossAForkedProcess) {
  // @verifies REQ-IPC-006
  // The combination this component exists to support: a slot living in a
  // region mapped at a different address in each process, written by one and
  // read by the other, with no pointers anywhere in the payload.
  const std::string name = "/safeedge_seqlock_" + std::to_string(::getpid());
  SharedMemoryRegion::unlinkName(name.c_str());

  using Slot = SeqlockSlot<WidePayload>;
  SharedMemoryRegion parent = SharedMemoryRegion::create(name.c_str(), sizeof(Slot));
  ASSERT_TRUE(parent.valid()) << parent.error().what();

  auto* slot = new (parent.data()) Slot();
  slot->store(WidePayload::of(1));

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);

  if (child == 0) {
    SharedMemoryRegion mapped =
        SharedMemoryRegion::openExisting(name.c_str(), sizeof(Slot));
    if (!mapped.valid()) {
      ::_exit(2);
    }
    // Already constructed by the parent; the child only reinterprets it.
    auto* shared = static_cast<Slot*>(mapped.data());

    // Wait for the parent to publish the agreed sentinel, verifying every
    // value seen along the way is internally consistent.
    for (int attempt = 0; attempt < 2'000'000; ++attempt) {
      WidePayload observed;
      if (shared->tryLoad(observed)) {
        if (!observed.consistent()) {
          ::_exit(3);
        }
        if (observed.generation == 4242) {
          ::_exit(0);
        }
      }
    }
    ::_exit(4);
  }

  for (std::uint64_t value = 2; value <= 4242; ++value) {
    slot->store(WidePayload::of(value));
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  SharedMemoryRegion::unlinkName(name.c_str());
  ASSERT_TRUE(WIFEXITED(status)) << "child did not exit normally";
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "child exit code: 2=map failed, 3=torn value, 4=never saw the sentinel";
}

// ---------------------------------------------------------------------------
// Real-time safety
// ---------------------------------------------------------------------------

TEST(SeqlockSlot, StoreAndLoadDoNotAllocate) {
  // @verifies REQ-IPC-008
  // @verifies REQ-RT-001
  ASSERT_TRUE(rt::guardIsInstalled());
  rt::setAllocationPolicy(rt::AllocationPolicy::kCount);
  rt::resetAllocationReport();

  SeqlockSlot<WidePayload> slot;
  WidePayload out;
  std::uint64_t accepted = 0;
  {
    const rt::NoAllocScope no_alloc;
    for (std::uint64_t value = 1; value <= 20'000; ++value) {
      slot.store(WidePayload::of(value));
      if (slot.tryLoad(out)) {
        ++accepted;
      }
    }
  }
  EXPECT_EQ(rt::allocationReport().violations, 0u);
  EXPECT_EQ(accepted, 20'000u);
  EXPECT_EQ(out.generation, 20'000u);
  rt::setAllocationPolicy(rt::AllocationPolicy::kAbort);
}

TEST(SeqlockSlot, IsSuitableForSharedMemoryByConstruction) {
  using Slot = SeqlockSlot<WidePayload>;
  // No vtable, no pointers, no non-trivial destructor: the properties that make
  // it safe to place in a region mapped at a different address in each process.
  static_assert(std::is_standard_layout_v<Slot>);
  static_assert(std::is_trivially_destructible_v<Slot>);
  EXPECT_EQ(alignof(Slot) % concurrent::kCacheLineSize, 0u);
  SUCCEED();
}

}  // namespace
}  // namespace safeedge::ipc

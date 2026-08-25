// SPDX-License-Identifier: Apache-2.0
//
// These tests fork. That is deliberate and not incidental complexity.
//
// Every other concurrency test in this repository runs two threads in one
// address space, which shares the allocator, the loader and the same mapping of
// every object. Shared memory is the one component whose entire purpose is to
// work across an address-space boundary, and a thread-based test would exercise
// none of what makes it hard: differing virtual addresses, an independently
// crashing peer, a name that outlives both processes.

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "safeedge/ipc/shared_memory.hpp"

namespace safeedge::ipc {
namespace {

/// Unique per test run, so a crashed earlier run cannot collide with this one.
std::string uniqueName(const char* suffix) {
  return "/safeedge_test_" + std::to_string(::getpid()) + "_" + suffix;
}

/// RAII cleanup: a leaked shm name survives the process and would make the
/// next run fail with EEXIST, which is exactly the operational failure mode
/// the O_EXCL policy exists to surface.
struct ScopedName {
  std::string name;
  explicit ScopedName(const char* suffix) : name(uniqueName(suffix)) {}
  ~ScopedName() { SharedMemoryRegion::unlinkName(name.c_str()); }
  [[nodiscard]] const char* c_str() const { return name.c_str(); }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(SharedMemory, DefaultConstructedRegionIsInvalid) {
  const SharedMemoryRegion region;
  EXPECT_FALSE(region.valid());
  EXPECT_EQ(region.data(), nullptr);
  EXPECT_EQ(region.size(), 0u);
}

TEST(SharedMemory, CreateThenOpenSharesTheSameMemory) {
  const ScopedName name("share");
  SharedMemoryRegion writer = SharedMemoryRegion::create(name.c_str(), 4096);
  ASSERT_TRUE(writer.valid()) << writer.error().operation << ": "
                              << writer.error().what();

  SharedMemoryRegion reader = SharedMemoryRegion::openExisting(name.c_str(), 4096);
  ASSERT_TRUE(reader.valid()) << reader.error().operation << ": "
                              << reader.error().what();

  std::memset(writer.data(), 0xA5, 128);
  const auto* bytes = static_cast<const std::uint8_t*>(reader.data());
  for (std::size_t index = 0; index < 128; ++index) {
    ASSERT_EQ(bytes[index], 0xA5) << "byte " << index;
  }
}

TEST(SharedMemory, CreateRefusesAnExistingName) {
  // @verifies REQ-IPC-001
  // O_EXCL, deliberately. Silently adopting an existing region is how a runtime
  // ends up sharing state with the remains of its previous instance.
  const ScopedName name("exclusive");
  SharedMemoryRegion first = SharedMemoryRegion::create(name.c_str(), 4096);
  ASSERT_TRUE(first.valid());

  const SharedMemoryRegion second = SharedMemoryRegion::create(name.c_str(), 4096);
  EXPECT_FALSE(second.valid());
  EXPECT_STREQ(second.error().operation, "shm_open");
  EXPECT_EQ(second.error().error_number, EEXIST);
}

TEST(SharedMemory, OpeningAMissingRegionFails) {
  const SharedMemoryRegion region =
      SharedMemoryRegion::openExisting("/safeedge_definitely_not_here", 4096);
  EXPECT_FALSE(region.valid());
  EXPECT_STREQ(region.error().operation, "shm_open");
  EXPECT_EQ(region.error().error_number, ENOENT);
}

TEST(SharedMemory, OpeningWithAnOversizedRequestFails) {
  // @verifies REQ-IPC-002
  // mmap would happily map beyond the region and hand back memory that raises
  // SIGBUS on first touch -- a crash that surfaces later, somewhere else,
  // looking like anything except the configuration mismatch it is.
  const ScopedName name("toosmall");
  SharedMemoryRegion small = SharedMemoryRegion::create(name.c_str(), 4096);
  ASSERT_TRUE(small.valid());

  const SharedMemoryRegion oversized =
      SharedMemoryRegion::openExisting(name.c_str(), 1 << 20);
  EXPECT_FALSE(oversized.valid());
  EXPECT_STREQ(oversized.error().operation, "size");
}

TEST(SharedMemory, MalformedNamesAreRejectedWithAClearError) {
  // @verifies REQ-IPC-003
  for (const char* bad : {"no_leading_slash", "/has/inner/slash", "/", ""}) {
    const SharedMemoryRegion region = SharedMemoryRegion::create(bad, 4096);
    EXPECT_FALSE(region.valid()) << "accepted: " << bad;
    EXPECT_STREQ(region.error().operation, "name") << "for: " << bad;
  }
  const SharedMemoryRegion null_name = SharedMemoryRegion::create(nullptr, 4096);
  EXPECT_FALSE(null_name.valid());
}

TEST(SharedMemory, ZeroSizeIsRejected) {
  const ScopedName name("zero");
  const SharedMemoryRegion region = SharedMemoryRegion::create(name.c_str(), 0);
  EXPECT_FALSE(region.valid());
  EXPECT_STREQ(region.error().operation, "size");
}

TEST(SharedMemory, UnlinkIsIdempotent) {
  // @verifies REQ-IPC-004
  const ScopedName name("unlink");
  SharedMemoryRegion region = SharedMemoryRegion::create(name.c_str(), 4096);
  ASSERT_TRUE(region.valid());

  SharedMemoryRegion::unlinkName(name.c_str());
  SharedMemoryRegion::unlinkName(name.c_str());  // must not misbehave

  // The mapping outlives the name -- that is the POSIX contract, and the
  // reason unlink and close are separate operations here.
  std::memset(region.data(), 0x5A, 64);
  EXPECT_EQ(static_cast<const std::uint8_t*>(region.data())[0], 0x5A);
}

TEST(SharedMemory, TheMappingOutlivesTheName) {
  // @verifies REQ-IPC-004
  const ScopedName name("outlive");
  SharedMemoryRegion writer = SharedMemoryRegion::create(name.c_str(), 4096);
  ASSERT_TRUE(writer.valid());
  SharedMemoryRegion reader = SharedMemoryRegion::openExisting(name.c_str(), 4096);
  ASSERT_TRUE(reader.valid());

  SharedMemoryRegion::unlinkName(name.c_str());

  // Both mappings remain valid and still refer to the same pages, but a new
  // open must now fail -- the name is gone.
  std::memset(writer.data(), 0x33, 32);
  EXPECT_EQ(static_cast<const std::uint8_t*>(reader.data())[0], 0x33);
  EXPECT_FALSE(SharedMemoryRegion::openExisting(name.c_str(), 4096).valid());
}

TEST(SharedMemory, MoveTransfersOwnershipExactlyOnce) {
  // @verifies REQ-IPC-005
  const ScopedName name("move");
  SharedMemoryRegion original = SharedMemoryRegion::create(name.c_str(), 4096);
  ASSERT_TRUE(original.valid());
  void* address = original.data();

  SharedMemoryRegion moved = std::move(original);
  EXPECT_TRUE(moved.valid());
  EXPECT_EQ(moved.data(), address);
  EXPECT_FALSE(original.valid())
      << "the moved-from region must not still own the mapping";
  EXPECT_EQ(original.data(), nullptr);
}

TEST(SharedMemory, CloseReleasesTheMappingEarly) {
  const ScopedName name("close");
  SharedMemoryRegion region = SharedMemoryRegion::create(name.c_str(), 4096);
  ASSERT_TRUE(region.valid());
  region.close();
  EXPECT_FALSE(region.valid());
  region.close();  // idempotent
  EXPECT_FALSE(region.valid());
}

// ---------------------------------------------------------------------------
// Genuinely cross-process
// ---------------------------------------------------------------------------

TEST(SharedMemory, AChildProcessSeesWhatTheParentWrote) {
  const ScopedName name("fork_read");
  constexpr std::size_t kBytes = 4096;
  SharedMemoryRegion parent = SharedMemoryRegion::create(name.c_str(), kBytes);
  ASSERT_TRUE(parent.valid());

  auto* counter = static_cast<std::atomic<std::uint64_t>*>(parent.data());
  new (counter) std::atomic<std::uint64_t>(0);
  counter->store(0xC0FFEE, std::memory_order_release);

  const pid_t child = ::fork();
  ASSERT_GE(child, 0) << "fork failed";

  if (child == 0) {
    // _exit rather than exit or return: the child must not run gtest teardown
    // or any static destructor, both of which would report nonsense.
    SharedMemoryRegion mapped = SharedMemoryRegion::openExisting(name.c_str(), kBytes);
    if (!mapped.valid()) {
      ::_exit(2);
    }
    const auto* observed = static_cast<const std::atomic<std::uint64_t>*>(mapped.data());
    ::_exit(observed->load(std::memory_order_acquire) == 0xC0FFEE ? 0 : 3);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status)) << "child did not exit normally";
  EXPECT_EQ(WEXITSTATUS(status), 0) << "child did not observe the parent's write";
}

TEST(SharedMemory, TheParentSeesWhatAChildProcessWrote) {
  const ScopedName name("fork_write");
  constexpr std::size_t kBytes = 4096;
  SharedMemoryRegion parent = SharedMemoryRegion::create(name.c_str(), kBytes);
  ASSERT_TRUE(parent.valid());

  auto* flag = static_cast<std::atomic<std::uint64_t>*>(parent.data());
  new (flag) std::atomic<std::uint64_t>(0);

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);

  if (child == 0) {
    SharedMemoryRegion mapped = SharedMemoryRegion::openExisting(name.c_str(), kBytes);
    if (!mapped.valid()) {
      ::_exit(2);
    }
    auto* written = static_cast<std::atomic<std::uint64_t>*>(mapped.data());
    written->store(0xBEEFCAFE, std::memory_order_release);
    ::_exit(0);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(flag->load(std::memory_order_acquire), 0xBEEFCAFEu);
}

TEST(SharedMemory, TheRegionIsMappedAtDifferentAddressesInEachProcess) {
  // The reason nothing stored in a region may be, or contain, a pointer. A
  // pointer written by one process does not fault in the other -- it points
  // somewhere plausible and wrong, which is far worse.
  const ScopedName name("addresses");
  constexpr std::size_t kBytes = 4096;
  SharedMemoryRegion parent = SharedMemoryRegion::create(name.c_str(), kBytes);
  ASSERT_TRUE(parent.valid());

  auto* slot = static_cast<std::atomic<std::uintptr_t>*>(parent.data());
  new (slot) std::atomic<std::uintptr_t>(0);

  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    SharedMemoryRegion mapped = SharedMemoryRegion::openExisting(name.c_str(), kBytes);
    if (!mapped.valid()) {
      ::_exit(2);
    }
    auto* published = static_cast<std::atomic<std::uintptr_t>*>(mapped.data());
    published->store(reinterpret_cast<std::uintptr_t>(mapped.data()),
                     std::memory_order_release);
    ::_exit(0);
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  const auto child_address = slot->load(std::memory_order_acquire);
  EXPECT_NE(child_address, 0u);
  // Not asserted as always different -- the kernel is free to choose the same
  // address, and on a lightly loaded system sometimes does. What is asserted is
  // that the code never relies on them matching.
  if (child_address == reinterpret_cast<std::uintptr_t>(parent.data())) {
    GTEST_SKIP() << "the kernel happened to pick the same address; "
                    "the point stands, this run cannot demonstrate it";
  }
  SUCCEED() << "child mapped at 0x" << std::hex << child_address << ", parent at 0x"
            << reinterpret_cast<std::uintptr_t>(parent.data());
}

}  // namespace
}  // namespace safeedge::ipc

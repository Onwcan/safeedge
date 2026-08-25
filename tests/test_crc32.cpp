// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <set>
#include <span>
#include <vector>

#include "safeedge/rt/no_alloc_guard.hpp"
#include "safeedge/safety/crc32.hpp"

namespace safeedge::safety {
namespace {

std::vector<std::uint8_t> asBytes(const char* text) {
  const std::size_t length = std::strlen(text);
  return {reinterpret_cast<const std::uint8_t*>(text),
          reinterpret_cast<const std::uint8_t*>(text) + length};
}

// ---------------------------------------------------------------------------
// Conformance
// ---------------------------------------------------------------------------

TEST(Crc32, MatchesTheStandardCheckVector) {
  // @verifies REQ-SAF-002
  // "123456789" is the standard input for identifying a CRC parameter set.
  // If this passes, the implementation is CRC-32/AUTOSAR and not something
  // that merely resembles it -- which matters, because a device at the other
  // end of a real link will be using the specified algorithm and nothing else.
  const auto data = asBytes("123456789");
  EXPECT_EQ(Crc32::compute(data), Crc32::kCheckValue);
}

TEST(Crc32, ReflectedPolynomialIsTheReverseOfTheSpecifiedOne) {
  // The table is built from the reflected form; if the two constants ever
  // disagree the CRC silently becomes a different, non-standard algorithm that
  // still passes every self-consistency test.
  std::uint32_t reversed = 0;
  std::uint32_t source = Crc32::kPolynomial;
  for (int bit = 0; bit < 32; ++bit) {
    reversed = (reversed << 1U) | (source & 1U);
    source >>= 1U;
  }
  EXPECT_EQ(reversed, Crc32::kReflectedPolynomial);
}

TEST(Crc32, EmptyInputYieldsTheFinalXorOfTheInitialValue) {
  EXPECT_EQ(Crc32::compute({}), Crc32::kInitial ^ Crc32::kFinalXor);
}

TEST(Crc32, IncrementalUpdatesMatchASingleShot) {
  // Relied on by the black channel, which feeds the payload, then the status,
  // then the sequence number, then address parameters that are never
  // transmitted -- without ever assembling them into one buffer.
  const auto data = asBytes("the quick brown fox jumps over the lazy dog");

  Crc32 incremental;
  for (const std::uint8_t byte : data) {
    incremental.update(byte);
  }
  EXPECT_EQ(incremental.value(), Crc32::compute(data));
}

TEST(Crc32, SplitUpdatesMatchRegardlessOfChunkBoundaries) {
  const auto data = asBytes("boundaries must not matter to a streaming checksum");
  const std::uint32_t whole = Crc32::compute(data);

  for (std::size_t split = 0; split <= data.size(); ++split) {
    Crc32 crc;
    crc.update(std::span<const std::uint8_t>(data.data(), split));
    crc.update(std::span<const std::uint8_t>(data.data() + split, data.size() - split));
    EXPECT_EQ(crc.value(), whole) << "differed when split at " << split;
  }
}

TEST(Crc32, ValueDoesNotDisturbTheRunningState) {
  // Reading an intermediate value must not apply the final XOR to the state
  // itself, or every subsequent byte would be folded into a corrupted running
  // value.
  const auto first = asBytes("abc");
  const auto second = asBytes("def");

  Crc32 crc;
  crc.update(first);
  const std::uint32_t intermediate = crc.value();
  const std::uint32_t again = crc.value();
  EXPECT_EQ(intermediate, again) << "value() is not idempotent";

  crc.update(second);
  EXPECT_EQ(crc.value(), Crc32::compute(asBytes("abcdef")));
}

TEST(Crc32, ResetReturnsToTheInitialState) {
  Crc32 crc;
  crc.update(asBytes("discard me"));
  crc.reset();
  crc.update(asBytes("123456789"));
  EXPECT_EQ(crc.value(), Crc32::kCheckValue);
}

// ---------------------------------------------------------------------------
// Endianness
// ---------------------------------------------------------------------------

TEST(Crc32, MultiByteUpdatesAreLittleEndianRegardlessOfHost) {
  // A controller and a drive of different endianness must agree on whether a
  // telegram is intact. Asserting the byte order explicitly is what makes that
  // true by construction rather than by both ends happening to be x86.
  Crc32 via_word;
  via_word.update(static_cast<std::uint32_t>(0x12345678U));

  const std::array<std::uint8_t, 4> little_endian{0x78, 0x56, 0x34, 0x12};
  Crc32 via_bytes;
  via_bytes.update(little_endian);

  EXPECT_EQ(via_word.value(), via_bytes.value());
}

TEST(Crc32, SixteenBitUpdatesAreAlsoLittleEndian) {
  Crc32 via_word;
  via_word.update(static_cast<std::uint16_t>(0xABCDU));

  const std::array<std::uint8_t, 2> little_endian{0xCD, 0xAB};
  Crc32 via_bytes;
  via_bytes.update(little_endian);

  EXPECT_EQ(via_word.value(), via_bytes.value());
}

// ---------------------------------------------------------------------------
// Error detection -- the property the polynomial was chosen for
// ---------------------------------------------------------------------------

/// Flips the given bit positions in a copy of `data`.
std::vector<std::uint8_t> withFlippedBits(std::vector<std::uint8_t> data,
                                          std::span<const std::size_t> bits) {
  for (const std::size_t bit : bits) {
    data[bit / 8] = static_cast<std::uint8_t>(data[bit / 8] ^ (1U << (bit % 8)));
  }
  return data;
}

TEST(Crc32, DetectsEverySingleBitError) {
  // @verifies REQ-SAF-002
  // Exhaustive over a 32-byte message: 256 positions.
  std::vector<std::uint8_t> message(32);
  std::iota(message.begin(), message.end(), static_cast<std::uint8_t>(1));
  const std::uint32_t good = Crc32::compute(message);

  for (std::size_t bit = 0; bit < message.size() * 8; ++bit) {
    const std::array<std::size_t, 1> flip{bit};
    EXPECT_NE(Crc32::compute(withFlippedBits(message, flip)), good)
        << "undetected single-bit error at bit " << bit;
  }
}

TEST(Crc32, DetectsEveryDoubleBitError) {
  // @verifies REQ-SAF-002
  // Exhaustive over all 32640 pairs in a 32-byte message. This is where the
  // classic CRC-32 polynomial would still be fine; the interesting case is the
  // next test.
  std::vector<std::uint8_t> message(32);
  std::iota(message.begin(), message.end(), static_cast<std::uint8_t>(1));
  const std::uint32_t good = Crc32::compute(message);
  const std::size_t bits = message.size() * 8;

  for (std::size_t a = 0; a < bits; ++a) {
    for (std::size_t b = a + 1; b < bits; ++b) {
      const std::array<std::size_t, 2> flip{a, b};
      ASSERT_NE(Crc32::compute(withFlippedBits(message, flip)), good)
          << "undetected double-bit error at bits " << a << " and " << b;
    }
  }
}

TEST(Crc32, DetectsUpToFiveBitErrorsWhichIsTheHammingDistanceSixClaim) {
  // @verifies REQ-SAF-002
  // The reason for choosing 0xF4ACFB13 over the familiar 0x04C11DB7 is HD=6 at
  // this length: any five bit errors are guaranteed detected. The classic
  // polynomial drops to HD=4 above 91 bits, meaning four flips can produce a
  // valid checksum.
  //
  // Exhaustive coverage of 5-bit combinations is infeasible, so this samples
  // heavily from a fixed seed. It cannot prove the bound -- that is a property
  // of the polynomial, established in the literature -- but it would reliably
  // catch an implementation that had drifted onto a different polynomial or
  // botched the reflection.
  std::vector<std::uint8_t> message(32);
  std::iota(message.begin(), message.end(), static_cast<std::uint8_t>(1));
  const std::uint32_t good = Crc32::compute(message);
  const std::size_t bits = message.size() * 8;

  std::mt19937 rng(0x5AFE7E00U);
  std::uniform_int_distribution<std::size_t> pick(0, bits - 1);

  for (std::size_t error_count = 3; error_count <= 5; ++error_count) {
    for (int trial = 0; trial < 60'000; ++trial) {
      std::set<std::size_t> chosen;
      while (chosen.size() < error_count) {
        chosen.insert(pick(rng));
      }
      const std::vector<std::size_t> flip(chosen.begin(), chosen.end());
      ASSERT_NE(Crc32::compute(withFlippedBits(message, flip)), good)
          << "undetected " << error_count << "-bit error";
    }
  }
}

TEST(Crc32, DetectsBurstErrorsUpToThirtyTwoBits) {
  // @verifies REQ-SAF-002
  // A CRC of width n detects every burst of n bits or fewer. Bursts are the
  // realistic failure shape on a physical link -- a connector glitch corrupts
  // consecutive bits, not scattered ones.
  std::vector<std::uint8_t> message(32);
  std::iota(message.begin(), message.end(), static_cast<std::uint8_t>(7));
  const std::uint32_t good = Crc32::compute(message);
  const std::size_t bits = message.size() * 8;

  std::mt19937 rng(0xB0157U);
  for (std::size_t burst_length = 1; burst_length <= 32; ++burst_length) {
    for (std::size_t start = 0; start + burst_length <= bits; ++start) {
      std::vector<std::size_t> flip;
      flip.reserve(burst_length);
      // A burst is a corrupted span; at least the first and last bit of it
      // must actually change for it to be a burst of that length.
      flip.push_back(start);
      for (std::size_t offset = 1; offset + 1 < burst_length; ++offset) {
        if ((rng() & 1U) != 0U) {
          flip.push_back(start + offset);
        }
      }
      if (burst_length > 1) {
        flip.push_back(start + burst_length - 1);
      }
      ASSERT_NE(Crc32::compute(withFlippedBits(message, flip)), good)
          << "undetected burst of " << burst_length << " starting at " << start;
    }
  }
}

TEST(Crc32, TruncationIsDetected) {
  // Dropping trailing bytes must change the checksum, or a truncated telegram
  // could validate as a shorter one.
  std::vector<std::uint8_t> message(48);
  std::iota(message.begin(), message.end(), static_cast<std::uint8_t>(3));
  const std::uint32_t full = Crc32::compute(message);

  for (std::size_t shorter = 1; shorter < message.size(); ++shorter) {
    const std::vector<std::uint8_t> truncated(
        message.begin(), message.begin() + static_cast<long>(shorter));
    EXPECT_NE(Crc32::compute(truncated), full)
        << "truncation to " << shorter << " undetected";
  }
}

// ---------------------------------------------------------------------------
// Real-time safety
// ---------------------------------------------------------------------------

TEST(Crc32, ComputingDoesNotAllocate) {
  // @verifies REQ-RT-001
  ASSERT_TRUE(rt::guardIsInstalled());
  rt::setAllocationPolicy(rt::AllocationPolicy::kCount);
  rt::resetAllocationReport();

  std::array<std::uint8_t, 64> message{};
  std::iota(message.begin(), message.end(), static_cast<std::uint8_t>(0));

  // The result of the final iteration is checked against an independently
  // computed value. That keeps the optimiser from discarding the loop AND
  // verifies the answer, which an XOR accumulator compared against zero does
  // neither of reliably -- the first version of this test asserted the
  // accumulator was non-zero and failed when the XOR happened to cancel.
  constexpr int kIterations = 1000;
  std::uint32_t last = 0;
  {
    const rt::NoAllocScope no_alloc;
    for (int i = 0; i < kIterations; ++i) {
      Crc32 crc;
      crc.update(message);
      crc.update(static_cast<std::uint16_t>(i));
      crc.update(static_cast<std::uint32_t>(i));
      last = crc.value();
    }
  }
  EXPECT_EQ(rt::allocationReport().violations, 0u);

  Crc32 expected;
  expected.update(message);
  expected.update(static_cast<std::uint16_t>(kIterations - 1));
  expected.update(static_cast<std::uint32_t>(kIterations - 1));
  EXPECT_EQ(last, expected.value());
  rt::setAllocationPolicy(rt::AllocationPolicy::kAbort);
}

}  // namespace
}  // namespace safeedge::safety

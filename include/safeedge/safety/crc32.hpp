// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace safeedge::safety {

/// CRC-32/AUTOSAR.
///
/// Parameters: poly 0xF4ACFB13, init 0xFFFFFFFF, reflected in and out,
/// final XOR 0xFFFFFFFF. Check value for "123456789" is 0x1697D06A.
///
/// Why this polynomial and not the familiar CRC-32 from zip and Ethernet
/// -------------------------------------------------------------------
/// The safety telegram this protects is tens of bytes, not megabytes, and the
/// property that matters is Hamming distance at that length: how many bit
/// errors can occur before a corrupted frame produces a valid checksum.
///
/// The classic CRC-32 polynomial (0x04C11DB7) has HD=4 for payloads above 91
/// bits -- three bit flips are detected, four can slip through. 0xF4ACFB13 was
/// selected by Koopman specifically for short messages and holds **HD=6 up to
/// 2048 bits**, so five bit errors are still guaranteed detected across any
/// telegram this protocol will ever carry.
///
/// For a checksum whose failure mode is "the machine acts on corrupted motion
/// data", two extra guaranteed bits of detection is not a marginal
/// improvement. It is also the polynomial AUTOSAR standardised for exactly
/// this class of use, which makes it defensible to a reviewer without
/// re-deriving the argument.
///
/// Implementation is table-driven and allocation-free; the table is built at
/// compile time.
class Crc32 {
 public:
  static constexpr std::uint32_t kPolynomial = 0xF4ACFB13U;
  /// The reflected form actually used by the byte-at-a-time algorithm.
  static constexpr std::uint32_t kReflectedPolynomial = 0xC8DF352FU;
  static constexpr std::uint32_t kInitial = 0xFFFFFFFFU;
  static constexpr std::uint32_t kFinalXor = 0xFFFFFFFFU;
  /// CRC of the ASCII string "123456789", the standard vector for this
  /// parameter set. Asserted in the tests.
  static constexpr std::uint32_t kCheckValue = 0x1697D06AU;

  Crc32() = default;

  /// Feeds more data. May be called repeatedly; the running value is retained
  /// so that non-contiguous fields can be covered without copying them into a
  /// staging buffer. That matters here: the black channel deliberately mixes
  /// transmitted bytes with address parameters that are never sent.
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::uint8_t byte) noexcept;
  /// Little-endian, so the result is stable across architectures.
  void update(std::uint16_t value) noexcept;
  void update(std::uint32_t value) noexcept;

  /// Applies the final XOR. Does not modify the running state, so a caller may
  /// take an intermediate value and keep feeding.
  [[nodiscard]] std::uint32_t value() const noexcept { return state_ ^ kFinalXor; }

  void reset() noexcept { state_ = kInitial; }

  /// One-shot convenience for the common case.
  [[nodiscard]] static std::uint32_t compute(std::span<const std::uint8_t> data) noexcept;

 private:
  std::uint32_t state_{kInitial};
};

}  // namespace safeedge::safety

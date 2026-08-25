// SPDX-License-Identifier: Apache-2.0
#include "safeedge/safety/crc32.hpp"

namespace safeedge::safety {
namespace {

/// Byte-at-a-time lookup table for the reflected polynomial, built at compile
/// time so there is no static initialisation order concern and nothing to
/// populate at startup -- the table lands in .rodata.
constexpr std::array<std::uint32_t, 256> buildTable() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t remainder = index;
    for (int bit = 0; bit < 8; ++bit) {
      // Reflected algorithm: shift right, and apply the reversed polynomial
      // when the low bit is set.
      remainder = ((remainder & 1U) != 0U)
                      ? ((remainder >> 1U) ^ Crc32::kReflectedPolynomial)
                      : (remainder >> 1U);
    }
    table[index] = remainder;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = buildTable();

}  // namespace

void Crc32::update(std::uint8_t byte) noexcept {
  const std::uint8_t index = static_cast<std::uint8_t>(state_ ^ byte);
  state_ = (state_ >> 8U) ^ kTable[index];
}

void Crc32::update(std::span<const std::uint8_t> data) noexcept {
  for (const std::uint8_t byte : data) {
    update(byte);
  }
}

void Crc32::update(std::uint16_t value) noexcept {
  // Explicit little-endian decomposition rather than memcpy of the object
  // representation: the CRC must come out identical on a big-endian target,
  // otherwise a controller and a drive of different endianness would disagree
  // about whether a perfectly good telegram is corrupt.
  update(static_cast<std::uint8_t>(value & 0xFFU));
  update(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void Crc32::update(std::uint32_t value) noexcept {
  update(static_cast<std::uint8_t>(value & 0xFFU));
  update(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  update(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  update(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::uint32_t Crc32::compute(std::span<const std::uint8_t> data) noexcept {
  Crc32 crc;
  crc.update(data);
  return crc.value();
}

}  // namespace safeedge::safety

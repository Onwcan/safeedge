// SPDX-License-Identifier: Apache-2.0
#include "safeedge/safety/black_channel.hpp"

#include <algorithm>

namespace safeedge::safety {
namespace {

std::uint32_t readUint32Le(std::span<const std::uint8_t> bytes,
                           std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void writeUint32Le(std::span<std::uint8_t> bytes, std::size_t offset,
                   std::uint32_t value) noexcept {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
  bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

}  // namespace

// @satisfies REQ-SAF-001
// @satisfies REQ-SAF-008
// @satisfies REQ-SAF-009
std::uint32_t computeTelegramCrc(const SafetyAddress& address,
                                 std::span<const std::uint8_t> payload,
                                 std::uint8_t status,
                                 std::uint32_t consecutive_number) noexcept {
  Crc32 crc;
  crc.update(payload);
  crc.update(status);
  crc.update(consecutive_number);

  // The address parameters are folded in but never transmitted. This is the
  // mechanism that turns an integrity check into an authenticity check: a
  // telegram produced by anything that does not already share these values
  // cannot yield a matching CRC, which is what defends insertion, masquerade
  // and misaddressing simultaneously.
  //
  // It is emphatically not cryptography -- the parameters are commissioning
  // data, not secrets, and this defends against faults rather than against a
  // determined attacker with the ability to read the configuration. That
  // distinction is worth keeping straight; a safety layer that is described as
  // providing security tends to be deployed as though it does.
  crc.update(address.source);
  crc.update(address.destination);
  crc.update(address.parameter_signature);
  return crc.value();
}

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

// @satisfies REQ-SAF-016
bool SafetySender::encode(std::span<const std::uint8_t> payload, std::uint8_t status,
                          Telegram& out) noexcept {
  if (payload.size() > kMaxPayloadBytes) {
    return false;
  }

  const std::size_t payload_size = payload.size();
  std::copy(payload.begin(), payload.end(), out.bytes.begin());

  out.bytes[payload_size] = status;
  writeUint32Le(out.bytes, payload_size + kStatusBytes, consecutive_number_);

  const std::uint32_t crc =
      computeTelegramCrc(address_, payload, status, consecutive_number_);
  writeUint32Le(out.bytes, payload_size + kStatusBytes + kConsecutiveNumberBytes, crc);

  out.size = payload_size + kOverheadBytes;

  // Wraps naturally. Zero is skipped so that an all-zeroes frame -- what a dead
  // transport or an uninitialised buffer looks like -- can never be mistaken
  // for a legitimate telegram.
  ++consecutive_number_;
  if (consecutive_number_ == 0) {
    consecutive_number_ = 1;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------

// @satisfies REQ-SAF-012
void SafetyReceiver::latch(TransmissionFault fault) noexcept {
  safe_state_ = true;
  last_fault_ = fault;
}

// @satisfies REQ-SAF-013
// @satisfies REQ-SAF-014
void SafetyReceiver::acknowledgeAndReset(std::int64_t now_ns) noexcept {
  safe_state_ = false;
  last_fault_ = TransmissionFault::kNone;
  synchronised_ = false;
  last_consecutive_number_ = 0;
  // Restart the watchdog window from the acknowledgement, otherwise the
  // consumer would immediately time out again on the strength of however long
  // the operator took to respond.
  last_valid_ns_ = now_ns;
}

// @satisfies REQ-SAF-006
ReceiveStatus SafetyReceiver::poll(std::int64_t now_ns) noexcept {
  if (safe_state_) {
    return ReceiveStatus::kInSafeState;
  }
  if (!synchronised_) {
    // Nothing has been received yet. The watchdog only becomes meaningful once
    // the relationship has produced at least one valid telegram; before that,
    // startup ordering between producer and consumer would otherwise decide
    // whether the machine faults.
    return ReceiveStatus::kValid;
  }
  if (now_ns - last_valid_ns_ > watchdog_ns_) {
    ++diagnostics_.timeouts;
    latch(TransmissionFault::kLoss);
    return ReceiveStatus::kTimeout;
  }
  return ReceiveStatus::kValid;
}

// @satisfies REQ-SAF-010
// @satisfies REQ-SAF-011
// @satisfies REQ-SAF-017
ReceiveStatus SafetyReceiver::receive(std::span<const std::uint8_t> raw,
                                      std::int64_t now_ns,
                                      std::span<std::uint8_t> payload_out,
                                      std::size_t& payload_size,
                                      std::uint8_t& status_out) noexcept {
  payload_size = 0;
  status_out = 0;

  if (safe_state_) {
    // Deliver nothing until acknowledged. Continuing to hand over payloads
    // after a latched fault would make the latch decorative.
    return ReceiveStatus::kInSafeState;
  }

  if (raw.size() < kOverheadBytes || raw.size() > kMaxTelegramBytes) {
    ++diagnostics_.rejected_malformed;
    latch(TransmissionFault::kCorruption);
    return ReceiveStatus::kMalformed;
  }

  const std::size_t received_payload_size = raw.size() - kOverheadBytes;
  if (received_payload_size > payload_out.size()) {
    // The caller's buffer cannot hold it. Treated as malformed rather than
    // truncated: silently delivering a short payload to a safety consumer is
    // how a partial setpoint gets acted on.
    ++diagnostics_.rejected_malformed;
    latch(TransmissionFault::kCorruption);
    return ReceiveStatus::kMalformed;
  }

  const std::span<const std::uint8_t> payload = raw.subspan(0, received_payload_size);
  const std::uint8_t status = raw[received_payload_size];
  const std::uint32_t consecutive_number =
      readUint32Le(raw, received_payload_size + kStatusBytes);
  const std::uint32_t received_crc =
      readUint32Le(raw, received_payload_size + kStatusBytes + kConsecutiveNumberBytes);

  // --- integrity and authenticity -----------------------------------------
  // Checked first. Nothing else in the telegram may be trusted until the CRC
  // has passed, including the consecutive number the sequence logic below
  // depends on.
  const std::uint32_t expected_crc =
      computeTelegramCrc(address_, payload, status, consecutive_number);
  if (received_crc != expected_crc) {
    ++diagnostics_.rejected_crc;
    latch(TransmissionFault::kCorruption);
    return ReceiveStatus::kCrcMismatch;
  }

  // @satisfies REQ-SAF-007
  // --- timeliness ----------------------------------------------------------
  // A telegram can be perfectly authentic, correctly sequenced, and still
  // useless because it describes a world that has moved on. Checked after the
  // CRC so that a corrupt frame is reported as corruption rather than as a
  // delay.
  if (synchronised_ && (now_ns - last_valid_ns_ > watchdog_ns_)) {
    ++diagnostics_.timeouts;
    latch(TransmissionFault::kUnacceptableDelay);
    return ReceiveStatus::kTimeout;
  }

  // @satisfies REQ-SAF-003
  // @satisfies REQ-SAF-004
  // @satisfies REQ-SAF-005
  // @satisfies REQ-SAF-015
  // --- freshness and ordering ---------------------------------------------
  if (!synchronised_) {
    // First telegram of the relationship: adopt whatever sequence the producer
    // is on. There is nothing to compare against yet.
    synchronised_ = true;
  } else {
    // The expected value must mirror the producer's own rule that zero is
    // never used. Deriving it here rather than assuming `last + 1` is not a
    // detail: with a naive increment, the one telegram per wrap where the
    // producer skips zero looks like a gap of two, and the consumer faults.
    // At 1 kHz the counter wraps about every 49 days, so a machine would drop
    // to a safe state roughly every seven weeks of uptime, for no reason
    // anyone at the plant could reproduce. Caught by
    // BlackChannel.SequenceSurvivesWraparound.
    std::uint32_t expected = last_consecutive_number_ + 1;
    if (expected == 0) {
      expected = 1;
    }

    if (consecutive_number != expected) {
      if (consecutive_number == last_consecutive_number_) {
        ++diagnostics_.rejected_repetition;
        latch(TransmissionFault::kUnintendedRepetition);
        return ReceiveStatus::kRepetition;
      }
      // Signed difference from the expected value, so that comparisons stay
      // correct across the 2^32 boundary.
      const auto delta = static_cast<std::int32_t>(consecutive_number - expected);
      if (delta < 0) {
        ++diagnostics_.rejected_out_of_order;
        latch(TransmissionFault::kIncorrectSequence);
        return ReceiveStatus::kOutOfOrder;
      }
      diagnostics_.telegrams_lost += static_cast<std::uint64_t>(delta);
      ++diagnostics_.rejected_sequence_gap;
      latch(TransmissionFault::kLoss);
      return ReceiveStatus::kSequenceGap;
    }
  }

  // --- accept --------------------------------------------------------------
  std::copy(payload.begin(), payload.end(), payload_out.begin());
  payload_size = received_payload_size;
  status_out = status;

  last_consecutive_number_ = consecutive_number;
  last_valid_ns_ = now_ns;
  ++diagnostics_.accepted;
  diagnostics_.last_accepted_consecutive_number = consecutive_number;
  return ReceiveStatus::kValid;
}

}  // namespace safeedge::safety

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "safeedge/safety/crc32.hpp"

namespace safeedge::safety {

/// The transmission fault model of IEC 61784-3, which is what a functional
/// safety communication layer exists to defend against.
///
/// The "black channel" idea is that the transport -- Ethernet, a backplane, a
/// wireless link, shared memory -- is assumed to be entirely untrustworthy and
/// is given no safety responsibility whatsoever. Every defence lives in the
/// safety layer at each end. That is what makes it possible to run safety
/// traffic over standard, uncertified networking hardware, and it is the
/// architecture PROFIsafe, openSAFETY and FSoE all share.
enum class TransmissionFault : std::uint8_t {
  kNone,
  /// Bits flipped in transit. Defended by the CRC.
  kCorruption,
  /// A previously valid telegram delivered again. A stale but perfectly
  /// well-formed setpoint is exactly as dangerous as a corrupted one, and the
  /// CRC cannot see it -- defended by the consecutive number.
  kUnintendedRepetition,
  /// Telegrams arriving out of order. Defended by the consecutive number.
  kIncorrectSequence,
  /// A telegram never arrives. Defended by the consecutive number gap and,
  /// when nothing arrives at all, by the watchdog.
  kLoss,
  /// A telegram arrives, but too late to still be meaningful. Defended by the
  /// watchdog. This is the fault a CRC and a sequence number are both blind
  /// to: the data is authentic and in order, and simply describes a world that
  /// no longer exists.
  kUnacceptableDelay,
  /// A foreign message injected into the stream. Defended by the CRC being
  /// seeded with address parameters that are never transmitted.
  kInsertion,
  /// A non-safety message that happens to look like a safety telegram.
  /// Defended by the same untransmitted seed.
  kMasquerade,
  /// A genuine safety telegram, from a genuine sender, delivered to the wrong
  /// recipient. Defended by the destination address being part of the seed.
  kAddressingError,
};

/// Identity of one end of a safety relationship, equivalent in role to the
/// PROFIsafe F-Parameters.
///
/// None of these fields is transmitted. They are mixed into the CRC at both
/// ends instead, so a telegram forged or misdelivered by anything that does not
/// already know them cannot produce a matching checksum. That single decision
/// is what turns a plain integrity check into a defence against insertion,
/// masquerade and misaddressing at once.
struct SafetyAddress {
  std::uint16_t source{0};
  std::uint16_t destination{0};
  /// Signature over the safety configuration both ends were commissioned with.
  /// A mismatch means the two ends disagree about parameters, and every
  /// telegram between them fails the CRC rather than being acted on.
  std::uint32_t parameter_signature{0};
};

inline constexpr std::size_t kMaxPayloadBytes = 64;
inline constexpr std::size_t kStatusBytes = 1;
inline constexpr std::size_t kConsecutiveNumberBytes = 4;
inline constexpr std::size_t kCrcBytes = 4;
inline constexpr std::size_t kOverheadBytes =
    kStatusBytes + kConsecutiveNumberBytes + kCrcBytes;
inline constexpr std::size_t kMaxTelegramBytes = kMaxPayloadBytes + kOverheadBytes;

/// Wire layout, little-endian throughout:
///
///     [0 .. n-1]        payload
///     [n]               status / control byte
///     [n+1 .. n+4]      consecutive number
///     [n+5 .. n+8]      CRC over payload + status + CN + untransmitted address
struct Telegram {
  std::array<std::uint8_t, kMaxTelegramBytes> bytes{};
  std::size_t size{0};

  [[nodiscard]] std::span<const std::uint8_t> view() const noexcept {
    return {bytes.data(), size};
  }
};

/// Status/control bits. A subset of the PROFIsafe control byte, kept to what
/// this implementation actually acts on rather than copied wholesale.
namespace status_bits {
/// Producer is telling the consumer to substitute fail-safe values.
inline constexpr std::uint8_t kActivateFailSafeValues = 1U << 0U;
/// Producer has detected a fault on its own side.
inline constexpr std::uint8_t kProducerFault = 1U << 1U;
/// Consumer must be acknowledged by an operator before resuming.
inline constexpr std::uint8_t kOperatorAckRequested = 1U << 2U;
}  // namespace status_bits

// ---------------------------------------------------------------------------
// Sender
// ---------------------------------------------------------------------------

/// Builds safety telegrams. Allocation-free and deterministic; safe to call
/// from inside a NoAllocScope on the real-time path.
class SafetySender {
 public:
  explicit SafetySender(SafetyAddress address) noexcept : address_(address) {}

  /// Encodes `payload` into `out`. Returns false only if the payload is larger
  /// than kMaxPayloadBytes, which is a programming error rather than a runtime
  /// condition.
  [[nodiscard]] bool encode(std::span<const std::uint8_t> payload, std::uint8_t status,
                            Telegram& out) noexcept;

  /// The number the next encode() will use.
  [[nodiscard]] std::uint32_t nextConsecutiveNumber() const noexcept {
    return consecutive_number_;
  }

  /// Restarts the sequence. Only legitimate as part of a deliberate,
  /// acknowledged restart of the safety relationship -- doing it while the
  /// consumer is running looks exactly like an attack.
  void restartSequence(std::uint32_t start = 1) noexcept { consecutive_number_ = start; }

  [[nodiscard]] const SafetyAddress& address() const noexcept { return address_; }

 private:
  SafetyAddress address_;
  /// Starts at 1 so that 0 can never be a legitimate value on the wire, which
  /// makes an all-zeroes frame from a dead or disconnected transport
  /// detectable rather than merely improbable.
  std::uint32_t consecutive_number_{1};
};

// ---------------------------------------------------------------------------
// Receiver
// ---------------------------------------------------------------------------

enum class ReceiveStatus : std::uint8_t {
  kValid,
  /// Too short to contain the mandatory fields, or longer than the maximum.
  kMalformed,
  /// CRC did not match. Covers corruption, insertion, masquerade and
  /// misaddressing -- they are indistinguishable at this layer, and all four
  /// have the same correct response.
  kCrcMismatch,
  kRepetition,
  kSequenceGap,
  kOutOfOrder,
  /// Nothing valid arrived inside the watchdog window.
  kTimeout,
  /// A fault has latched; the consumer must be acknowledged before anything
  /// further is delivered.
  kInSafeState,
};

struct ReceiverDiagnostics {
  std::uint64_t accepted{0};
  std::uint64_t rejected_malformed{0};
  std::uint64_t rejected_crc{0};
  std::uint64_t rejected_repetition{0};
  std::uint64_t rejected_sequence_gap{0};
  std::uint64_t rejected_out_of_order{0};
  std::uint64_t timeouts{0};
  /// Total telegrams presumed lost, summed from the gaps observed.
  std::uint64_t telegrams_lost{0};
  std::uint32_t last_accepted_consecutive_number{0};
};

/// Validates safety telegrams and latches a safe state on any fault.
///
/// Latching is the whole point and is deliberately inconvenient. A consumer
/// that silently recovers on the next good telegram would ride through an
/// intermittent fault -- a loose connector, a failing transceiver, a switch
/// dropping frames under load -- and the machine would keep running on data
/// that is only sometimes trustworthy. Recovery therefore requires an explicit
/// acknowledgement, which in a real installation is an operator action taken
/// after someone has looked at the machine.
class SafetyReceiver {
 public:
  SafetyReceiver(SafetyAddress address, std::int64_t watchdog_ns) noexcept
      : address_(address), watchdog_ns_(watchdog_ns) {}

  /// Validates `raw` and, if it is good, copies the payload into
  /// `payload_out`. Allocation-free; safe on the real-time path.
  ///
  /// `now_ns` is a monotonic timestamp supplied by the caller rather than read
  /// from a clock inside, so that the whole fault model can be exercised in
  /// tests at arbitrary simulated times without sleeping.
  /// `payload_size` and `status_out` are reset on every call; their values
  /// describe only that call and must be consumed before the next receive.
  [[nodiscard]] ReceiveStatus receive(std::span<const std::uint8_t> raw,
                                      std::int64_t now_ns,
                                      std::span<std::uint8_t> payload_out,
                                      std::size_t& payload_size,
                                      std::uint8_t& status_out) noexcept;

  /// Checks the watchdog without a telegram to process. A consumer must call
  /// this every cycle: silence is the one fault that generates no event of its
  /// own, and it is also the most common one in the field.
  [[nodiscard]] ReceiveStatus poll(std::int64_t now_ns) noexcept;

  [[nodiscard]] bool inSafeState() const noexcept { return safe_state_; }
  [[nodiscard]] TransmissionFault lastFault() const noexcept { return last_fault_; }

  /// Clears the latch. Models an operator acknowledgement; the sequence is
  /// resynchronised to whatever arrives next.
  void acknowledgeAndReset(std::int64_t now_ns) noexcept;

  [[nodiscard]] const ReceiverDiagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

  [[nodiscard]] const SafetyAddress& address() const noexcept { return address_; }
  [[nodiscard]] std::int64_t watchdogNanos() const noexcept { return watchdog_ns_; }

 private:
  void latch(TransmissionFault fault) noexcept;

  SafetyAddress address_;
  std::int64_t watchdog_ns_;
  std::int64_t last_valid_ns_{0};
  std::uint32_t last_consecutive_number_{0};
  bool synchronised_{false};
  bool safe_state_{false};
  TransmissionFault last_fault_{TransmissionFault::kNone};
  ReceiverDiagnostics diagnostics_{};
};

/// Computes the protecting CRC. Exposed so tests and the fuzzer can build
/// telegrams that are valid in every respect except the one under test --
/// otherwise a test for, say, repetition would be indistinguishable from a
/// test for corruption.
[[nodiscard]] std::uint32_t computeTelegramCrc(const SafetyAddress& address,
                                               std::span<const std::uint8_t> payload,
                                               std::uint8_t status,
                                               std::uint32_t consecutive_number) noexcept;

}  // namespace safeedge::safety

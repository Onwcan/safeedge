// SPDX-License-Identifier: Apache-2.0
//
// Verification of the IEC 61784-3 transmission fault model.
//
// Every fault in that model gets its own test, and the final test in this file
// walks the whole model in one table so that adding a fault type without a
// corresponding defence fails the build. That table is the artefact a reviewer
// actually wants: it says, in one place, which hazards this layer claims to
// handle and which mechanism handles each.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <random>
#include <span>
#include <vector>

#include "safeedge/rt/no_alloc_guard.hpp"
#include "safeedge/safety/black_channel.hpp"

namespace safeedge::safety {
namespace {

constexpr std::int64_t kMillis = 1'000'000;
constexpr std::int64_t kWatchdogNs = 10 * kMillis;

const SafetyAddress kAddress{/*source=*/0x0011, /*destination=*/0x2200,
                             /*parameter_signature=*/0xDEADBEEF};

/// Bundles the receiver with the output buffers, so a test reads as a sequence
/// of deliveries rather than as buffer management.
struct Consumer {
  SafetyReceiver receiver{kAddress, kWatchdogNs};
  std::array<std::uint8_t, kMaxPayloadBytes> payload{};
  std::size_t payload_size{0};
  std::uint8_t status{0};

  ReceiveStatus deliver(std::span<const std::uint8_t> raw, std::int64_t now_ns) {
    return receiver.receive(raw, now_ns, payload, payload_size, status);
  }
  ReceiveStatus deliver(const Telegram& telegram, std::int64_t now_ns) {
    return deliver(telegram.view(), now_ns);
  }
};

std::vector<std::uint8_t> samplePayload(std::size_t size) {
  std::vector<std::uint8_t> data(size);
  std::iota(data.begin(), data.end(), static_cast<std::uint8_t>(0x40));
  return data;
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST(BlackChannel, RoundTripsAPayloadIntact) {
  SafetySender sender(kAddress);
  Consumer consumer;

  const auto payload = samplePayload(16);
  Telegram telegram;
  ASSERT_TRUE(sender.encode(payload, status_bits::kOperatorAckRequested, telegram));

  EXPECT_EQ(consumer.deliver(telegram, kMillis), ReceiveStatus::kValid);
  ASSERT_EQ(consumer.payload_size, payload.size());
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), consumer.payload.begin()));
  EXPECT_EQ(consumer.status, status_bits::kOperatorAckRequested);
  EXPECT_FALSE(consumer.receiver.inSafeState());
}

TEST(BlackChannel, HandlesEveryPayloadSizeFromZeroToTheMaximum) {
  for (std::size_t size = 0; size <= kMaxPayloadBytes; ++size) {
    SafetySender sender(kAddress);
    Consumer consumer;
    const auto payload = samplePayload(size);

    Telegram telegram;
    ASSERT_TRUE(sender.encode(payload, 0, telegram)) << "size " << size;
    EXPECT_EQ(telegram.size, size + kOverheadBytes);
    ASSERT_EQ(consumer.deliver(telegram, kMillis), ReceiveStatus::kValid)
        << "size " << size;
    ASSERT_EQ(consumer.payload_size, size);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), consumer.payload.begin()));
  }
}

TEST(BlackChannel, RejectsAnOversizedPayloadAtEncodeTime) {
  SafetySender sender(kAddress);
  const auto payload = samplePayload(kMaxPayloadBytes + 1);
  Telegram telegram;
  EXPECT_FALSE(sender.encode(payload, 0, telegram));
}

TEST(BlackChannel, DeliversAContinuousStream) {
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  for (int cycle = 0; cycle < 500; ++cycle) {
    Telegram telegram;
    ASSERT_TRUE(sender.encode(payload, 0, telegram));
    ASSERT_EQ(consumer.deliver(telegram, cycle * kMillis), ReceiveStatus::kValid)
        << "cycle " << cycle;
  }
  EXPECT_EQ(consumer.receiver.diagnostics().accepted, 500u);
  EXPECT_FALSE(consumer.receiver.inSafeState());
}

TEST(BlackChannel, ConsecutiveNumberNeverTakesTheValueZero) {
  // @verifies REQ-SAF-016
  // Zero is reserved so that an all-zeroes frame -- a dead transport, an
  // uninitialised buffer, a disconnected cable presenting as silence-with-data
  // -- can never be mistaken for a legitimate telegram.
  SafetySender sender(kAddress);
  sender.restartSequence(0xFFFFFFFEU);

  const auto payload = samplePayload(4);
  std::vector<std::uint32_t> observed;
  for (int i = 0; i < 4; ++i) {
    observed.push_back(sender.nextConsecutiveNumber());
    Telegram telegram;
    ASSERT_TRUE(sender.encode(payload, 0, telegram));
  }
  EXPECT_EQ(observed[0], 0xFFFFFFFEU);
  EXPECT_EQ(observed[1], 0xFFFFFFFFU);
  EXPECT_EQ(observed[2], 1U) << "zero must be skipped on wrap";
  EXPECT_EQ(observed[3], 2U);
}

TEST(BlackChannel, SequenceSurvivesWraparound) {
  // @verifies REQ-SAF-015
  // At 1 kHz the counter wraps roughly every 49 days, which is well inside the
  // uptime of an industrial installation. Wraparound is a real case here, not a
  // theoretical one, and a naive `expected == last + 1` comparison on unsigned
  // values would fault the machine every seven weeks.
  SafetySender sender(kAddress);
  Consumer consumer;
  sender.restartSequence(0xFFFFFFFDU);
  const auto payload = samplePayload(4);

  for (int cycle = 0; cycle < 6; ++cycle) {
    Telegram telegram;
    ASSERT_TRUE(sender.encode(payload, 0, telegram));
    ASSERT_EQ(consumer.deliver(telegram, cycle * kMillis), ReceiveStatus::kValid)
        << "cycle " << cycle;
  }
  EXPECT_FALSE(consumer.receiver.inSafeState());
}

// ---------------------------------------------------------------------------
// Fault 1: corruption
// ---------------------------------------------------------------------------

TEST(BlackChannel, DetectsCorruptionInEveryByteOfTheTelegram) {
  // @verifies REQ-SAF-001
  const auto payload = samplePayload(12);

  for (std::size_t byte_index = 0; byte_index < payload.size() + kOverheadBytes;
       ++byte_index) {
    for (int bit = 0; bit < 8; ++bit) {
      SafetySender sender(kAddress);
      Consumer consumer;
      Telegram telegram;
      ASSERT_TRUE(sender.encode(payload, 0, telegram));

      telegram.bytes[byte_index] =
          static_cast<std::uint8_t>(telegram.bytes[byte_index] ^ (1U << bit));

      EXPECT_EQ(consumer.deliver(telegram, kMillis), ReceiveStatus::kCrcMismatch)
          << "undetected flip of bit " << bit << " in byte " << byte_index;
      EXPECT_TRUE(consumer.receiver.inSafeState());
      EXPECT_EQ(consumer.receiver.lastFault(), TransmissionFault::kCorruption);
    }
  }
}

// ---------------------------------------------------------------------------
// Fault 2: unintended repetition
// ---------------------------------------------------------------------------

TEST(BlackChannel, DetectsAReplayedTelegram) {
  // @verifies REQ-SAF-003
  // A repeated telegram is byte-for-byte valid; the CRC cannot see it. A stale
  // setpoint delivered again is exactly as dangerous as a corrupted one -- the
  // machine acts confidently on a command describing where the world used to
  // be. Only the consecutive number catches this.
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram telegram;
  ASSERT_TRUE(sender.encode(payload, 0, telegram));

  EXPECT_EQ(consumer.deliver(telegram, kMillis), ReceiveStatus::kValid);
  EXPECT_EQ(consumer.deliver(telegram, 2 * kMillis), ReceiveStatus::kRepetition);
  EXPECT_EQ(consumer.payload_size, 0u);
  EXPECT_EQ(consumer.status, 0u);
  EXPECT_TRUE(consumer.receiver.inSafeState());
  EXPECT_EQ(consumer.receiver.lastFault(), TransmissionFault::kUnintendedRepetition);
  EXPECT_EQ(consumer.receiver.diagnostics().rejected_repetition, 1u);
}

// ---------------------------------------------------------------------------
// Fault 3: incorrect sequence
// ---------------------------------------------------------------------------

TEST(BlackChannel, DetectsOutOfOrderDelivery) {
  // @verifies REQ-SAF-004
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram first;
  Telegram second;
  ASSERT_TRUE(sender.encode(payload, 0, first));
  ASSERT_TRUE(sender.encode(payload, 0, second));

  EXPECT_EQ(consumer.deliver(second, kMillis), ReceiveStatus::kValid);
  EXPECT_EQ(consumer.deliver(first, 2 * kMillis), ReceiveStatus::kOutOfOrder);
  EXPECT_TRUE(consumer.receiver.inSafeState());
  EXPECT_EQ(consumer.receiver.lastFault(), TransmissionFault::kIncorrectSequence);
}

// ---------------------------------------------------------------------------
// Fault 4: loss
// ---------------------------------------------------------------------------

TEST(BlackChannel, DetectsASingleLostTelegramFromTheSequenceGap) {
  // @verifies REQ-SAF-005
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram first;
  Telegram dropped;
  Telegram third;
  ASSERT_TRUE(sender.encode(payload, 0, first));
  ASSERT_TRUE(sender.encode(payload, 0, dropped));  // never delivered
  ASSERT_TRUE(sender.encode(payload, 0, third));

  EXPECT_EQ(consumer.deliver(first, kMillis), ReceiveStatus::kValid);
  EXPECT_EQ(consumer.deliver(third, 2 * kMillis), ReceiveStatus::kSequenceGap);
  EXPECT_EQ(consumer.receiver.lastFault(), TransmissionFault::kLoss);
  EXPECT_EQ(consumer.receiver.diagnostics().telegrams_lost, 1u);
}

TEST(BlackChannel, CountsHowManyWereLost) {
  // @verifies REQ-SAF-005
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram first;
  ASSERT_TRUE(sender.encode(payload, 0, first));
  EXPECT_EQ(consumer.deliver(first, kMillis), ReceiveStatus::kValid);

  Telegram scratch;
  for (int i = 0; i < 7; ++i) {
    ASSERT_TRUE(sender.encode(payload, 0, scratch));  // all dropped
  }
  Telegram resumed;
  ASSERT_TRUE(sender.encode(payload, 0, resumed));

  EXPECT_EQ(consumer.deliver(resumed, 2 * kMillis), ReceiveStatus::kSequenceGap);
  EXPECT_EQ(consumer.receiver.diagnostics().telegrams_lost, 7u);
}

TEST(BlackChannel, DetectsTotalSilenceThroughTheWatchdog) {
  // @verifies REQ-SAF-006
  // The fault that generates no event of its own. A sequence gap needs a later
  // telegram to notice; if the producer dies, none ever arrives, and only the
  // watchdog fires. This is also the most common real failure -- a pulled
  // cable, a crashed process, a wedged switch.
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram telegram;
  ASSERT_TRUE(sender.encode(payload, 0, telegram));
  ASSERT_EQ(consumer.deliver(telegram, 0), ReceiveStatus::kValid);

  EXPECT_EQ(consumer.receiver.poll(kWatchdogNs / 2), ReceiveStatus::kValid);
  EXPECT_FALSE(consumer.receiver.inSafeState());

  EXPECT_EQ(consumer.receiver.poll(kWatchdogNs + 1), ReceiveStatus::kTimeout);
  EXPECT_TRUE(consumer.receiver.inSafeState());
  EXPECT_EQ(consumer.receiver.lastFault(), TransmissionFault::kLoss);
}

TEST(BlackChannel, WatchdogDoesNotFireBeforeTheFirstTelegram) {
  // @verifies REQ-SAF-006
  // Otherwise whether the machine faults on startup would depend on which of
  // the producer and consumer happened to be scheduled first.
  Consumer consumer;
  EXPECT_EQ(consumer.receiver.poll(100 * kWatchdogNs), ReceiveStatus::kValid);
  EXPECT_FALSE(consumer.receiver.inSafeState());
}

// ---------------------------------------------------------------------------
// Fault 5: unacceptable delay
// ---------------------------------------------------------------------------

TEST(BlackChannel, DetectsATelegramThatArrivesTooLate) {
  // @verifies REQ-SAF-007
  // Authentic, correctly sequenced, intact -- and useless, because it
  // describes a world that has moved on. Neither the CRC nor the sequence
  // number can see this one.
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram first;
  Telegram late;
  ASSERT_TRUE(sender.encode(payload, 0, first));
  ASSERT_TRUE(sender.encode(payload, 0, late));

  ASSERT_EQ(consumer.deliver(first, 0), ReceiveStatus::kValid);
  EXPECT_EQ(consumer.deliver(late, kWatchdogNs + 1), ReceiveStatus::kTimeout);
  EXPECT_TRUE(consumer.receiver.inSafeState());
  EXPECT_EQ(consumer.receiver.lastFault(), TransmissionFault::kUnacceptableDelay);
}

TEST(BlackChannel, AcceptsATelegramArrivingJustInsideTheWatchdog) {
  // @verifies REQ-SAF-007
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram first;
  Telegram punctual;
  ASSERT_TRUE(sender.encode(payload, 0, first));
  ASSERT_TRUE(sender.encode(payload, 0, punctual));

  ASSERT_EQ(consumer.deliver(first, 0), ReceiveStatus::kValid);
  EXPECT_EQ(consumer.deliver(punctual, kWatchdogNs), ReceiveStatus::kValid);
  EXPECT_FALSE(consumer.receiver.inSafeState());
}

// ---------------------------------------------------------------------------
// Faults 6-8: insertion, masquerade, addressing
// ---------------------------------------------------------------------------

TEST(BlackChannel, RejectsATelegramFromTheWrongSource) {
  // @verifies REQ-SAF-008
  // Insertion. The foreign producer builds a structurally perfect telegram --
  // and cannot match the CRC, because the source address is folded into it and
  // never transmitted.
  SafetyAddress impostor = kAddress;
  impostor.source = 0x9999;

  SafetySender foreign(impostor);
  Consumer consumer;
  Telegram telegram;
  ASSERT_TRUE(foreign.encode(samplePayload(8), 0, telegram));

  EXPECT_EQ(consumer.deliver(telegram, kMillis), ReceiveStatus::kCrcMismatch);
  EXPECT_TRUE(consumer.receiver.inSafeState());
}

TEST(BlackChannel, RejectsATelegramAddressedToSomeoneElse) {
  // @verifies REQ-SAF-009
  // Addressing error: a genuine telegram from a genuine producer, delivered to
  // the wrong consumer. Common in a real installation after a commissioning
  // mistake, and the failure mode is a machine obeying another machine.
  SafetyAddress other_destination = kAddress;
  other_destination.destination = 0x4444;

  SafetySender sender(other_destination);
  Consumer consumer;
  Telegram telegram;
  ASSERT_TRUE(sender.encode(samplePayload(8), 0, telegram));

  EXPECT_EQ(consumer.deliver(telegram, kMillis), ReceiveStatus::kCrcMismatch);
}

TEST(BlackChannel, RejectsATelegramBuiltWithMismatchedConfiguration) {
  // @verifies REQ-SAF-008
  // The two ends were commissioned with different safety parameters. Every
  // telegram between them fails rather than being acted on -- which is the
  // right outcome, because they disagree about what the data means.
  SafetyAddress other_config = kAddress;
  other_config.parameter_signature = 0x12345678;

  SafetySender sender(other_config);
  Consumer consumer;
  Telegram telegram;
  ASSERT_TRUE(sender.encode(samplePayload(8), 0, telegram));

  EXPECT_EQ(consumer.deliver(telegram, kMillis), ReceiveStatus::kCrcMismatch);
}

TEST(BlackChannel, RejectsRandomBytesAsMasquerade) {
  // @verifies REQ-SAF-008
  // Masquerade: ordinary traffic on the shared transport that happens to land
  // in the safety consumer's buffer.
  std::mt19937 rng(0xBADF00D);
  std::uniform_int_distribution<int> byte_value(0, 255);

  int accepted = 0;
  for (int trial = 0; trial < 20'000; ++trial) {
    Consumer consumer;
    std::vector<std::uint8_t> noise(kOverheadBytes + 8);
    for (std::uint8_t& byte : noise) {
      byte = static_cast<std::uint8_t>(byte_value(rng));
    }
    if (consumer.deliver(noise, kMillis) == ReceiveStatus::kValid) {
      ++accepted;
    }
  }
  // A 32-bit CRC gives a false-acceptance probability of 2^-32 per frame.
  // Across 20000 random frames the expected number accepted is 5e-6, so any
  // acceptance at all indicates the check is not actually being applied.
  EXPECT_EQ(accepted, 0);
}

TEST(BlackChannel, RejectsAnAllZeroesFrame) {
  // @verifies REQ-SAF-016
  // What a dead transport, an uninitialised buffer or a disconnected
  // transceiver presents as. It must not validate.
  Consumer consumer;
  const std::vector<std::uint8_t> zeros(kOverheadBytes + 8, 0);
  EXPECT_NE(consumer.deliver(zeros, kMillis), ReceiveStatus::kValid);
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST(BlackChannel, RejectsFramesShorterThanTheMandatoryFields) {
  // @verifies REQ-SAF-010
  for (std::size_t size = 0; size < kOverheadBytes; ++size) {
    Consumer consumer;
    const std::vector<std::uint8_t> too_short(size, 0xAB);
    EXPECT_EQ(consumer.deliver(too_short, kMillis), ReceiveStatus::kMalformed)
        << "size " << size;
  }
}

TEST(BlackChannel, RejectsFramesLongerThanTheMaximum) {
  // @verifies REQ-SAF-010
  Consumer consumer;
  const std::vector<std::uint8_t> too_long(kMaxTelegramBytes + 1, 0xAB);
  EXPECT_EQ(consumer.deliver(too_long, kMillis), ReceiveStatus::kMalformed);
}

TEST(BlackChannel, RefusesToTruncateIntoAnUndersizedCallerBuffer) {
  // @verifies REQ-SAF-010
  // @verifies REQ-SAF-011
  // Silently delivering a short payload to a safety consumer is how half a
  // setpoint gets acted on.
  SafetySender sender(kAddress);
  SafetyReceiver receiver(kAddress, kWatchdogNs);

  Telegram telegram;
  ASSERT_TRUE(sender.encode(samplePayload(32), 0, telegram));

  std::array<std::uint8_t, 8> undersized{};
  std::size_t payload_size = 0;
  std::uint8_t status = 0;
  EXPECT_EQ(receiver.receive(telegram.view(), kMillis, undersized, payload_size, status),
            ReceiveStatus::kMalformed);
  EXPECT_EQ(payload_size, 0u);
}

// ---------------------------------------------------------------------------
// Latching and recovery
// ---------------------------------------------------------------------------

TEST(BlackChannel, StaysInSafeStateEvenWhenGoodTelegramsResume) {
  // @verifies REQ-SAF-012
  // The property that makes the latch worth having. An intermittent fault -- a
  // loose connector, a failing transceiver, a switch dropping frames under
  // load -- produces good telegrams most of the time. A consumer that recovered
  // on the next good one would ride through it indefinitely, running on data
  // that is only sometimes trustworthy.
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram first;
  ASSERT_TRUE(sender.encode(payload, 0, first));
  ASSERT_EQ(consumer.deliver(first, kMillis), ReceiveStatus::kValid);

  Telegram corrupt;
  ASSERT_TRUE(sender.encode(payload, 0, corrupt));
  corrupt.bytes[0] = static_cast<std::uint8_t>(corrupt.bytes[0] ^ 0xFFU);
  ASSERT_EQ(consumer.deliver(corrupt, 2 * kMillis), ReceiveStatus::kCrcMismatch);

  for (int cycle = 3; cycle < 20; ++cycle) {
    Telegram good;
    ASSERT_TRUE(sender.encode(payload, 0, good));
    EXPECT_EQ(consumer.deliver(good, cycle * kMillis), ReceiveStatus::kInSafeState)
        << "recovered without acknowledgement at cycle " << cycle;
    EXPECT_TRUE(consumer.receiver.inSafeState());
  }
}

TEST(BlackChannel, DeliversNoPayloadWhileLatched) {
  // @verifies REQ-SAF-011
  SafetySender sender(kAddress);
  Consumer consumer;

  Telegram corrupt;
  ASSERT_TRUE(sender.encode(samplePayload(8), 0, corrupt));
  corrupt.bytes[2] = static_cast<std::uint8_t>(corrupt.bytes[2] ^ 0x01U);
  ASSERT_EQ(consumer.deliver(corrupt, kMillis), ReceiveStatus::kCrcMismatch);

  Telegram good;
  ASSERT_TRUE(sender.encode(samplePayload(8), 0, good));
  consumer.payload_size = 999;
  EXPECT_EQ(consumer.deliver(good, 2 * kMillis), ReceiveStatus::kInSafeState);
  EXPECT_EQ(consumer.payload_size, 0u) << "a latched consumer must hand over nothing";
}

TEST(BlackChannel, AcknowledgementClearsTheLatchAndResynchronises) {
  // @verifies REQ-SAF-013
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram corrupt;
  ASSERT_TRUE(sender.encode(payload, 0, corrupt));
  corrupt.bytes[1] = static_cast<std::uint8_t>(corrupt.bytes[1] ^ 0x80U);
  ASSERT_EQ(consumer.deliver(corrupt, kMillis), ReceiveStatus::kCrcMismatch);
  ASSERT_TRUE(consumer.receiver.inSafeState());

  consumer.receiver.acknowledgeAndReset(5 * kMillis);
  EXPECT_FALSE(consumer.receiver.inSafeState());
  EXPECT_EQ(consumer.receiver.lastFault(), TransmissionFault::kNone);

  // The sequence has advanced while the consumer was latched; it must adopt
  // whatever the producer is on rather than faulting on the gap.
  Telegram resumed;
  ASSERT_TRUE(sender.encode(payload, 0, resumed));
  EXPECT_EQ(consumer.deliver(resumed, 6 * kMillis), ReceiveStatus::kValid);
}

TEST(BlackChannel, AcknowledgementRestartsTheWatchdogWindow) {
  // @verifies REQ-SAF-014
  // Otherwise the consumer times out again immediately, on the strength of how
  // long the operator took to walk to the panel.
  SafetySender sender(kAddress);
  Consumer consumer;
  const auto payload = samplePayload(8);

  Telegram first;
  ASSERT_TRUE(sender.encode(payload, 0, first));
  ASSERT_EQ(consumer.deliver(first, 0), ReceiveStatus::kValid);
  ASSERT_EQ(consumer.receiver.poll(kWatchdogNs + 1), ReceiveStatus::kTimeout);

  const std::int64_t much_later = 600LL * 1000 * kMillis;  // ten minutes
  consumer.receiver.acknowledgeAndReset(much_later);
  EXPECT_EQ(consumer.receiver.poll(much_later + 1), ReceiveStatus::kValid);
  EXPECT_FALSE(consumer.receiver.inSafeState());
}

// ---------------------------------------------------------------------------
// The fault model as a table
// ---------------------------------------------------------------------------

TEST(BlackChannel, EveryFaultInTheModelHasADefenceThatFires) {
  // @verifies REQ-SAF-001
  // @verifies REQ-SAF-003
  // @verifies REQ-SAF-004
  // @verifies REQ-SAF-005
  // @verifies REQ-SAF-007
  // @verifies REQ-SAF-008
  // @verifies REQ-SAF-009
  // The summary artefact. Each row states a hazard from IEC 61784-3, the
  // mechanism that defends it, and an injector that produces it. If a fault is
  // ever added to TransmissionFault without a row here, this fails to compile
  // -- which is the point of writing it as a table rather than as prose.
  struct Case {
    const char* fault;
    const char* defence;
    ReceiveStatus expected;
    void (*inject)(SafetySender&, Consumer&, Telegram&, std::int64_t&);
  };

  static const auto corrupt = [](SafetySender& s, Consumer& c, Telegram& t,
                                 std::int64_t& at) {
    (void)c;
    (void)s.encode(samplePayload(8), 0, t);
    t.bytes[3] = static_cast<std::uint8_t>(t.bytes[3] ^ 0x20U);
    at = kMillis;
  };
  static const auto repeat = [](SafetySender& s, Consumer& c, Telegram& t,
                                std::int64_t& at) {
    (void)s.encode(samplePayload(8), 0, t);
    (void)c.deliver(t, kMillis);  // first delivery is legitimate
    at = 2 * kMillis;
  };
  static const auto reorder = [](SafetySender& s, Consumer& c, Telegram& t,
                                 std::int64_t& at) {
    Telegram older;
    (void)s.encode(samplePayload(8), 0, older);
    (void)s.encode(samplePayload(8), 0, t);
    (void)c.deliver(t, kMillis);  // deliver the newer one first
    t = older;
    at = 2 * kMillis;
  };
  static const auto lose = [](SafetySender& s, Consumer& c, Telegram& t,
                              std::int64_t& at) {
    Telegram first;
    Telegram dropped;
    (void)s.encode(samplePayload(8), 0, first);
    (void)s.encode(samplePayload(8), 0, dropped);
    (void)s.encode(samplePayload(8), 0, t);
    (void)c.deliver(first, kMillis);
    at = 2 * kMillis;
  };
  static const auto delay = [](SafetySender& s, Consumer& c, Telegram& t,
                               std::int64_t& at) {
    Telegram first;
    (void)s.encode(samplePayload(8), 0, first);
    (void)s.encode(samplePayload(8), 0, t);
    (void)c.deliver(first, 0);
    at = kWatchdogNs + 1;
  };
  static const auto insert = [](SafetySender& s, Consumer& c, Telegram& t,
                                std::int64_t& at) {
    (void)s;
    (void)c;
    SafetyAddress impostor = kAddress;
    impostor.source = 0x7777;
    SafetySender foreign(impostor);
    (void)foreign.encode(samplePayload(8), 0, t);
    at = kMillis;
  };
  static const auto misaddress = [](SafetySender& s, Consumer& c, Telegram& t,
                                    std::int64_t& at) {
    (void)s;
    (void)c;
    SafetyAddress elsewhere = kAddress;
    elsewhere.destination = 0x5555;
    SafetySender sender(elsewhere);
    (void)sender.encode(samplePayload(8), 0, t);
    at = kMillis;
  };

  const std::array<Case, 7> cases{{
      {"corruption", "CRC-32/AUTOSAR, HD=6", ReceiveStatus::kCrcMismatch, corrupt},
      {"unintended repetition", "consecutive number", ReceiveStatus::kRepetition, repeat},
      {"incorrect sequence", "consecutive number", ReceiveStatus::kOutOfOrder, reorder},
      {"loss", "consecutive number gap", ReceiveStatus::kSequenceGap, lose},
      {"unacceptable delay", "watchdog", ReceiveStatus::kTimeout, delay},
      {"insertion", "untransmitted source address in CRC", ReceiveStatus::kCrcMismatch,
       insert},
      {"addressing error", "untransmitted destination in CRC",
       ReceiveStatus::kCrcMismatch, misaddress},
  }};

  for (const Case& scenario : cases) {
    SafetySender sender(kAddress);
    Consumer consumer;
    Telegram telegram;
    std::int64_t at = kMillis;

    scenario.inject(sender, consumer, telegram, at);
    EXPECT_EQ(consumer.deliver(telegram, at), scenario.expected)
        << "fault: " << scenario.fault << " | defence: " << scenario.defence;
    EXPECT_TRUE(consumer.receiver.inSafeState())
        << "fault not latched: " << scenario.fault;
  }
}

// ---------------------------------------------------------------------------
// Robustness against arbitrary input
// ---------------------------------------------------------------------------

TEST(BlackChannel, SurvivesArbitraryMutationOfAValidTelegram) {
  // @verifies REQ-SAF-017
  // The property the libFuzzer target in fuzz/ chases more thoroughly: no
  // input, however malformed, may make the decoder misbehave. Rejecting bad
  // frames is covered exhaustively above; this is the weaker and more
  // important guarantee that nothing reads out of bounds, leaves a payload
  // behind after a rejection, or reports a length inconsistent with the frame.
  //
  // A safety layer that can be crashed by a malformed frame has turned a
  // detectable fault into an undetectable one.
  //
  // Runs under ASan and UBSan in CI, which is what makes it more than a smoke
  // test. GCC has no -fsanitize=fuzzer, so this covers developers on GCC while
  // the real fuzzer runs on Clang.
  // Built in a fixed buffer with an explicit length rather than a resized
  // vector: GCC 15 cannot bound a runtime resize here and emits a false
  // -Wstringop-overflow at -O3, and a fixed buffer is closer to how the
  // decoder is actually called from the runtime anyway.
  constexpr std::size_t kScratchCapacity = kMaxTelegramBytes + 64;
  std::mt19937 rng(0x1337BEEF);
  std::uniform_int_distribution<int> byte_value(0, 255);
  std::uniform_int_distribution<std::size_t> mutation_count(1, 6);
  std::uniform_int_distribution<std::size_t> extension(1, 40);

  SafetySender sender(kAddress);
  const auto payload = samplePayload(16);
  std::array<std::uint8_t, kScratchCapacity> scratch{};

  for (int trial = 0; trial < 50'000; ++trial) {
    Telegram telegram;
    ASSERT_TRUE(sender.encode(payload, 0, telegram));

    std::size_t frame_size = telegram.size;
    std::copy(telegram.bytes.begin(),
              telegram.bytes.begin() + static_cast<long>(frame_size), scratch.begin());

    // Reshape: sometimes truncate, sometimes extend with whatever was already
    // in the buffer -- which models a transport delivering a short read or a
    // frame with trailing garbage.
    const std::size_t shape = rng() % 8;
    if (shape == 0 && frame_size > 1) {
      frame_size = 1 + (rng() % (frame_size - 1));
    } else if (shape == 1) {
      frame_size = std::min(frame_size + extension(rng), kScratchCapacity);
    }

    const std::size_t mutations = mutation_count(rng);
    for (std::size_t i = 0; i < mutations; ++i) {
      scratch[rng() % frame_size] = static_cast<std::uint8_t>(byte_value(rng));
    }

    Consumer consumer;
    consumer.payload_size = 12345;  // poison, so a stale value is detectable
    const ReceiveStatus status = consumer.deliver(
        std::span<const std::uint8_t>(scratch.data(), frame_size), kMillis);

    if (status == ReceiveStatus::kValid) {
      ASSERT_EQ(consumer.payload_size + kOverheadBytes, frame_size)
          << "accepted a frame but reported an inconsistent payload length";
    } else {
      ASSERT_EQ(consumer.payload_size, 0u)
          << "a rejected frame left a payload behind for the caller to act on";
      ASSERT_TRUE(consumer.receiver.inSafeState())
          << "a rejected frame did not latch the safe state";
    }
  }
}

// ---------------------------------------------------------------------------
// Real-time safety
// ---------------------------------------------------------------------------

TEST(BlackChannel, EncodeAndReceiveDoNotAllocate) {
  // @verifies REQ-RT-001
  // The safety layer sits directly on the cyclic executor's deadline path.
  ASSERT_TRUE(rt::guardIsInstalled());
  rt::setAllocationPolicy(rt::AllocationPolicy::kCount);
  rt::resetAllocationReport();

  SafetySender sender(kAddress);
  SafetyReceiver receiver(kAddress, kWatchdogNs);
  std::array<std::uint8_t, kMaxPayloadBytes> payload_in{};
  std::array<std::uint8_t, kMaxPayloadBytes> payload_out{};
  std::iota(payload_in.begin(), payload_in.end(), static_cast<std::uint8_t>(1));
  Telegram telegram;

  std::size_t accepted = 0;
  {
    const rt::NoAllocScope no_alloc;
    for (int cycle = 0; cycle < 2000; ++cycle) {
      std::size_t out_size = 0;
      std::uint8_t status = 0;
      if (!sender.encode(payload_in, 0, telegram)) {
        break;
      }
      if (receiver.receive(telegram.view(), cycle * kMillis, payload_out, out_size,
                           status) == ReceiveStatus::kValid) {
        ++accepted;
      }
      (void)receiver.poll(cycle * kMillis);
    }
  }
  EXPECT_EQ(rt::allocationReport().violations, 0u);
  EXPECT_EQ(accepted, 2000u);
  rt::setAllocationPolicy(rt::AllocationPolicy::kAbort);
}

}  // namespace
}  // namespace safeedge::safety

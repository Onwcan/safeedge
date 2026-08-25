// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "safeedge/rt/no_alloc_guard.hpp"
#include "safeedge/safety/dual_channel.hpp"

namespace safeedge::safety {
namespace {

constexpr std::int64_t kMillis = 1'000'000;
constexpr std::int64_t kTolerance = 5 * kMillis;

DualChannelSupervisor::Config testConfig() {
  DualChannelSupervisor::Config config;
  config.limits.limited_speed = 10.0;
  config.limits.standstill_speed = 0.1;
  config.limits.standstill_window = 0.5;
  config.limits.stop_time_limit_ns = 500 * kMillis;
  config.limits.position_monitoring_enabled = false;
  config.discrepancy_tolerance_ns = kTolerance;
  return config;
}

SafetyInputs healthy(std::int64_t timestamp_ns = 0) {
  SafetyInputs inputs;
  inputs.emergency_stop_asserted = false;
  inputs.communication_ok = true;
  inputs.heartbeat_ok = true;
  inputs.self_test_passed = true;
  inputs.request = SafetyFunction::kNone;
  inputs.timestamp_ns = timestamp_ns;
  return inputs;
}

/// Runs both channels through the self test and into unrestricted motion.
void bringUp(DualChannelSupervisor& supervisor) {
  SafetyInputs inputs = healthy(0);
  inputs.request = SafetyFunction::kSafeTorqueOff;
  (void)supervisor.evaluate(inputs, inputs, 0);
  (void)supervisor.evaluate(healthy(kMillis), healthy(kMillis), kMillis);
}

// ---------------------------------------------------------------------------
// combine(): the 1oo2 property
// ---------------------------------------------------------------------------

TEST(DualChannel, CombineTakesTheSafeDirectionOnEveryField) {
  // @verifies REQ-SAF-040
  SafetyOutputs permissive;
  permissive.torque_permitted = true;
  permissive.motion_permitted = true;
  permissive.speed_limit = std::numeric_limits<double>::infinity();
  permissive.brake_engaged = false;
  permissive.state = SafetyState::kOperational;

  SafetyOutputs restrictive;
  restrictive.torque_permitted = false;
  restrictive.motion_permitted = false;
  restrictive.speed_limit = 0.0;
  restrictive.brake_engaged = true;
  restrictive.state = SafetyState::kSafeTorqueOff;

  for (int order = 0; order < 2; ++order) {
    // Order must not matter. A combination that depended on which channel was
    // passed first would make the safe direction an accident of wiring.
    const SafetyOutputs combined =
        order == 0 ? DualChannelSupervisor::combine(permissive, restrictive)
                   : DualChannelSupervisor::combine(restrictive, permissive);
    EXPECT_FALSE(combined.torque_permitted);
    EXPECT_FALSE(combined.motion_permitted);
    EXPECT_DOUBLE_EQ(combined.speed_limit, 0.0);
    EXPECT_TRUE(combined.brake_engaged);
    EXPECT_EQ(combined.state, SafetyState::kSafeTorqueOff);
  }
}

TEST(DualChannel, CombineTakesTheLowerSpeedLimit) {
  // @verifies REQ-SAF-040
  SafetyOutputs fast;
  fast.speed_limit = 100.0;
  fast.state = SafetyState::kLimitedSpeed;
  SafetyOutputs slow;
  slow.speed_limit = 2.0;
  slow.state = SafetyState::kLimitedSpeed;

  EXPECT_DOUBLE_EQ(DualChannelSupervisor::combine(fast, slow).speed_limit, 2.0);
}

TEST(DualChannel, CombinePreservesAgreementUnchanged) {
  SafetyOutputs both;
  both.torque_permitted = true;
  both.motion_permitted = true;
  both.speed_limit = 7.5;
  both.brake_engaged = false;
  both.state = SafetyState::kLimitedSpeed;

  const SafetyOutputs combined = DualChannelSupervisor::combine(both, both);
  EXPECT_TRUE(combined.torque_permitted);
  EXPECT_TRUE(combined.motion_permitted);
  EXPECT_DOUBLE_EQ(combined.speed_limit, 7.5);
  EXPECT_EQ(combined.state, SafetyState::kLimitedSpeed);
}

TEST(DualChannel, CombinePropagatesAFaultFromEitherChannel) {
  SafetyOutputs clean;
  clean.state = SafetyState::kOperational;
  clean.torque_permitted = true;

  SafetyOutputs faulted;
  faulted.state = SafetyState::kSafeTorqueOff;
  faulted.fault = FaultReason::kSpeedLimitExceeded;
  faulted.fault_latched = true;

  EXPECT_TRUE(DualChannelSupervisor::combine(clean, faulted).fault_latched);
  EXPECT_TRUE(DualChannelSupervisor::combine(faulted, clean).fault_latched);
  EXPECT_EQ(DualChannelSupervisor::combine(faulted, clean).fault,
            FaultReason::kSpeedLimitExceeded);
}

// ---------------------------------------------------------------------------
// One channel is enough to stop the machine
// ---------------------------------------------------------------------------

TEST(DualChannel, EitherChannelAloneCanDemandTheSafeReaction) {
  // @verifies REQ-SAF-040
  for (int failing = 0; failing < 2; ++failing) {
    DualChannelSupervisor supervisor(testConfig());
    bringUp(supervisor);

    SafetyInputs good = healthy(2 * kMillis);
    SafetyInputs stopping = healthy(2 * kMillis);
    stopping.emergency_stop_asserted = true;

    const SafetyOutputs outputs = failing == 0
                                      ? supervisor.evaluate(stopping, good, 2 * kMillis)
                                      : supervisor.evaluate(good, stopping, 2 * kMillis);

    EXPECT_FALSE(outputs.torque_permitted)
        << "channel " << failing << " demanded a stop and was overruled";
    EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  }
}

TEST(DualChannel, AChannelStuckPermittingMotionIsOverruledByItsPeer) {
  // @verifies REQ-SAF-040
  // The failure this architecture exists for: one channel is wrong in the
  // unsafe direction. Its peer withholds permission and the machine stops.
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs stuck_permissive = healthy(2 * kMillis);  // sees nothing wrong
  SafetyInputs sees_overspeed = healthy(2 * kMillis);    // sees the violation
  stuck_permissive.request = SafetyFunction::kSafelyLimitedSpeed;
  sees_overspeed.request = SafetyFunction::kSafelyLimitedSpeed;
  stuck_permissive.speed_magnitude = 1.0;
  sees_overspeed.speed_magnitude = 50.0;

  const SafetyOutputs outputs =
      supervisor.evaluate(stuck_permissive, sees_overspeed, 2 * kMillis);
  EXPECT_FALSE(outputs.torque_permitted);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
}

// ---------------------------------------------------------------------------
// Cross-comparison: the D in 1oo2D
// ---------------------------------------------------------------------------

TEST(DualChannel, BriefDisagreementWithinToleranceIsNotAFault) {
  // @verifies REQ-SAF-042
  // Independent sensors have independent noise. At a threshold boundary the
  // two channels will legitimately disagree for a cycle or two -- one reads
  // 9.99 and the other 10.01 against a limit of 10. Faulting on the first
  // disagreement would make the machine unusable.
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs a = healthy(2 * kMillis);
  SafetyInputs b = healthy(2 * kMillis);
  a.request = SafetyFunction::kSafelyLimitedSpeed;
  b.request = SafetyFunction::kNone;  // disagreeing request -> different states

  (void)supervisor.evaluate(a, b, 2 * kMillis);
  EXPECT_TRUE(supervisor.disagreeing());
  EXPECT_FALSE(supervisor.discrepancyLatched());

  // Still inside the window.
  a.timestamp_ns = 5 * kMillis;
  b.timestamp_ns = 5 * kMillis;
  (void)supervisor.evaluate(a, b, 5 * kMillis);
  EXPECT_FALSE(supervisor.discrepancyLatched());
}

TEST(DualChannel, DisagreementOutlastingTheToleranceBecomesAFault) {
  // @verifies REQ-SAF-041
  // @verifies REQ-SAF-042
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs a = healthy();
  SafetyInputs b = healthy();
  a.request = SafetyFunction::kSafelyLimitedSpeed;
  b.request = SafetyFunction::kNone;

  std::int64_t now = 2 * kMillis;
  (void)supervisor.evaluate(a, b, now);
  ASSERT_TRUE(supervisor.disagreeing());

  now += kTolerance + 1;
  a.timestamp_ns = now;
  b.timestamp_ns = now;
  const SafetyOutputs outputs = supervisor.evaluate(a, b, now);

  EXPECT_TRUE(supervisor.discrepancyLatched());
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_EQ(outputs.fault, FaultReason::kChannelDiscrepancy);
  EXPECT_FALSE(outputs.torque_permitted);
  EXPECT_EQ(supervisor.diagnostics().discrepancy_faults, 1u);
}

TEST(DualChannel, ADiscrepancyFaultIsForcedOntoBothChannels) {
  // @verifies REQ-SAF-043
  // Neither channel can detect this on its own -- each is blind to the other
  // by construction -- so the supervisor has to drive both down. If it only
  // masked its own output, the channels would still believe they were running.
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs a = healthy();
  SafetyInputs b = healthy();
  a.request = SafetyFunction::kSafelyLimitedSpeed;
  b.request = SafetyFunction::kNone;

  std::int64_t now = 2 * kMillis;
  (void)supervisor.evaluate(a, b, now);
  now += kTolerance + 1;
  a.timestamp_ns = now;
  b.timestamp_ns = now;
  (void)supervisor.evaluate(a, b, now);

  EXPECT_EQ(supervisor.channelA().fault(), FaultReason::kChannelDiscrepancy);
  EXPECT_EQ(supervisor.channelB().fault(), FaultReason::kChannelDiscrepancy);
  EXPECT_EQ(supervisor.channelA().state(), SafetyState::kSafeTorqueOff);
  EXPECT_EQ(supervisor.channelB().state(), SafetyState::kSafeTorqueOff);
}

TEST(DualChannel, ResolvingTheDisagreementDoesNotClearTheLatch) {
  // @verifies REQ-SAF-044
  // A discrepancy that resolves itself is exactly the shape of an intermittent
  // fault. Letting agreement alone clear it would mean the machine kept
  // running on a channel pair already caught disagreeing once.
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs a = healthy();
  SafetyInputs b = healthy();
  a.request = SafetyFunction::kSafelyLimitedSpeed;
  b.request = SafetyFunction::kNone;

  std::int64_t now = 2 * kMillis;
  (void)supervisor.evaluate(a, b, now);
  now += kTolerance + 1;
  a.timestamp_ns = now;
  b.timestamp_ns = now;
  (void)supervisor.evaluate(a, b, now);
  ASSERT_TRUE(supervisor.discrepancyLatched());

  // Channels now agree again, but nobody has acknowledged anything.
  for (int cycle = 0; cycle < 50; ++cycle) {
    now += kMillis;
    const SafetyOutputs outputs = supervisor.evaluate(healthy(now), healthy(now), now);
    ASSERT_TRUE(supervisor.discrepancyLatched()) << "cleared without acknowledgement";
    ASSERT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  }
}

TEST(DualChannel, LatchClearsOnlyOnceBothChannelsHaveBeenAcknowledged) {
  // @verifies REQ-SAF-044
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs a = healthy();
  SafetyInputs b = healthy();
  a.request = SafetyFunction::kSafelyLimitedSpeed;
  b.request = SafetyFunction::kNone;

  std::int64_t now = 2 * kMillis;
  (void)supervisor.evaluate(a, b, now);
  now += kTolerance + 1;
  a.timestamp_ns = now;
  b.timestamp_ns = now;
  (void)supervisor.evaluate(a, b, now);
  ASSERT_TRUE(supervisor.discrepancyLatched());

  // Acknowledge only channel A: still latched, because B is still faulted.
  now += kMillis;
  SafetyInputs ack_a = healthy(now);
  ack_a.acknowledge = true;
  (void)supervisor.evaluate(ack_a, healthy(now), now);
  EXPECT_TRUE(supervisor.discrepancyLatched());

  // Now acknowledge both.
  now += kMillis;
  SafetyInputs ack = healthy(now);
  ack.acknowledge = true;
  (void)supervisor.evaluate(ack, ack, now);
  EXPECT_FALSE(supervisor.discrepancyLatched());
  EXPECT_FALSE(supervisor.channelA().faultLatched());
  EXPECT_FALSE(supervisor.channelB().faultLatched());
}

TEST(DualChannel, AcknowledgementStillDoesNotRestartMotion) {
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs a = healthy();
  SafetyInputs b = healthy();
  a.request = SafetyFunction::kSafelyLimitedSpeed;
  b.request = SafetyFunction::kNone;

  std::int64_t now = 2 * kMillis;
  (void)supervisor.evaluate(a, b, now);
  now += kTolerance + 1;
  a.timestamp_ns = now;
  b.timestamp_ns = now;
  (void)supervisor.evaluate(a, b, now);

  now += kMillis;
  SafetyInputs ack = healthy(now);
  ack.acknowledge = true;
  ack.request = SafetyFunction::kNone;  // still asking for motion
  const SafetyOutputs outputs = supervisor.evaluate(ack, ack, now);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_FALSE(outputs.torque_permitted);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

TEST(DualChannel, DiagnosticsRecordDisagreementDuration) {
  // @verifies REQ-SAF-041
  // If the longest observed disagreement sits close to the configured
  // tolerance during normal operation, the tolerance is too tight. That is
  // what this counter is for.
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs a = healthy();
  SafetyInputs b = healthy();
  a.request = SafetyFunction::kSafelyLimitedSpeed;
  b.request = SafetyFunction::kNone;

  std::int64_t now = 2 * kMillis;
  (void)supervisor.evaluate(a, b, now);
  now += 3 * kMillis;
  a.timestamp_ns = now;
  b.timestamp_ns = now;
  (void)supervisor.evaluate(a, b, now);

  EXPECT_EQ(supervisor.diagnostics().disagreeing_cycles, 2u);
  EXPECT_EQ(supervisor.diagnostics().longest_disagreement_ns, 3 * kMillis);
  EXPECT_EQ(supervisor.diagnostics().discrepancy_faults, 0u);
}

TEST(DualChannel, AgreeingChannelsProduceNoDisagreementAtAll) {
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  for (int cycle = 2; cycle < 500; ++cycle) {
    const std::int64_t now = cycle * kMillis;
    SafetyInputs inputs = healthy(now);
    inputs.request = SafetyFunction::kSafelyLimitedSpeed;
    inputs.speed_magnitude = 5.0;
    const SafetyOutputs outputs = supervisor.evaluate(inputs, inputs, now);
    ASSERT_EQ(outputs.state, SafetyState::kLimitedSpeed) << "cycle " << cycle;
  }
  EXPECT_EQ(supervisor.diagnostics().disagreeing_cycles, 0u);
  EXPECT_EQ(supervisor.diagnostics().discrepancy_faults, 0u);
}

TEST(DualChannel, SensorNoiseNearAThresholdDoesNotFaultTheMachine) {
  // @verifies REQ-SAF-042
  // The realistic scenario the tolerance exists for: two sensors straddling a
  // limit, disagreeing for a cycle at a time but never persistently.
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  std::int64_t now = 2 * kMillis;
  for (int cycle = 0; cycle < 200; ++cycle) {
    now += kMillis;
    SafetyInputs a = healthy(now);
    SafetyInputs b = healthy(now);
    a.request = SafetyFunction::kSafelyLimitedSpeed;
    b.request = SafetyFunction::kSafelyLimitedSpeed;
    // Both below the limit; they simply read slightly differently.
    a.speed_magnitude = 9.98;
    b.speed_magnitude = 9.99 + ((cycle % 2 == 0) ? 0.005 : -0.005);
    const SafetyOutputs outputs = supervisor.evaluate(a, b, now);
    ASSERT_EQ(outputs.state, SafetyState::kLimitedSpeed) << "cycle " << cycle;
  }
  EXPECT_FALSE(supervisor.discrepancyLatched());
}

// ---------------------------------------------------------------------------
// Real-time safety
// ---------------------------------------------------------------------------

TEST(DualChannel, EvaluationDoesNotAllocate) {
  // @verifies REQ-RT-001
  ASSERT_TRUE(rt::guardIsInstalled());
  rt::setAllocationPolicy(rt::AllocationPolicy::kCount);
  rt::resetAllocationReport();

  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  int permitted_cycles = 0;
  {
    const rt::NoAllocScope no_alloc;
    for (int cycle = 2; cycle < 5000; ++cycle) {
      const std::int64_t now = cycle * kMillis;
      SafetyInputs a = healthy(now);
      SafetyInputs b = healthy(now);
      a.speed_magnitude = static_cast<double>(cycle % 12);
      b.speed_magnitude = static_cast<double>(cycle % 12) + 0.01;
      a.acknowledge = (cycle % 13) == 0;
      b.acknowledge = a.acknowledge;
      if (supervisor.evaluate(a, b, now).torque_permitted) {
        ++permitted_cycles;
      }
    }
  }
  EXPECT_EQ(rt::allocationReport().violations, 0u);
  EXPECT_GT(permitted_cycles, 0);
  rt::setAllocationPolicy(rt::AllocationPolicy::kAbort);
}

TEST(DualChannel, ResetReturnsBothChannelsToPowerOn) {
  DualChannelSupervisor supervisor(testConfig());
  bringUp(supervisor);

  SafetyInputs stop = healthy(2 * kMillis);
  stop.emergency_stop_asserted = true;
  (void)supervisor.evaluate(stop, stop, 2 * kMillis);
  ASSERT_TRUE(supervisor.channelA().faultLatched());

  supervisor.reset();
  EXPECT_EQ(supervisor.channelA().state(), SafetyState::kSelfTest);
  EXPECT_EQ(supervisor.channelB().state(), SafetyState::kSelfTest);
  EXPECT_FALSE(supervisor.discrepancyLatched());
  EXPECT_EQ(supervisor.diagnostics().disagreeing_cycles, 0u);
}

}  // namespace
}  // namespace safeedge::safety

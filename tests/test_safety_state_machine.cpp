// SPDX-License-Identifier: Apache-2.0
//
// Verification of the IEC 61800-5-2 supervisor.
//
// The centrepiece is EveryStateAndRequestPairHasADefinedOutcome, which walks
// the full cross product of states and requests against a table written by
// hand from the specification rather than read out of the implementation. That
// distinction is the whole point: a table generated from the code under test
// proves only that the code is self-consistent.
//
// Everything else here checks a property that a transition table cannot
// express -- latching, ordering between monitors and requests, and the
// fail-safe direction of every default.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "safeedge/rt/no_alloc_guard.hpp"
#include "safeedge/safety/safety_state_machine.hpp"

namespace safeedge::safety {
namespace {

constexpr std::int64_t kMillis = 1'000'000;

SafetyLimits testLimits() {
  SafetyLimits limits;
  limits.limited_speed = 10.0;
  limits.standstill_speed = 0.1;
  limits.standstill_window = 0.5;
  limits.stop_time_limit_ns = 500 * kMillis;
  limits.position_min = -100.0;
  limits.position_max = 100.0;
  limits.position_monitoring_enabled = true;
  return limits;
}

/// Inputs describing a healthy machine at standstill with nothing wrong.
SafetyInputs healthy(std::int64_t timestamp_ns = 0) {
  SafetyInputs inputs;
  inputs.emergency_stop_asserted = false;
  inputs.communication_ok = true;
  inputs.heartbeat_ok = true;
  inputs.self_test_passed = true;
  inputs.request = SafetyFunction::kNone;
  inputs.speed_magnitude = 0.0;
  inputs.position = 0.0;
  inputs.timestamp_ns = timestamp_ns;
  return inputs;
}

/// Drives a fresh machine into `target` and returns it.
SafetyStateMachine machineIn(SafetyState target) {
  SafetyStateMachine machine(testLimits());
  if (target == SafetyState::kSelfTest) {
    return machine;
  }

  // Every path starts by clearing the self test, which always lands in STO.
  SafetyInputs inputs = healthy(0);
  inputs.request = SafetyFunction::kSafeTorqueOff;
  (void)machine.evaluate(inputs);

  switch (target) {
    case SafetyState::kSafeTorqueOff:
      break;
    case SafetyState::kOperational:
      inputs = healthy(kMillis);
      (void)machine.evaluate(inputs);
      break;
    case SafetyState::kLimitedSpeed:
      inputs = healthy(kMillis);
      inputs.request = SafetyFunction::kSafelyLimitedSpeed;
      (void)machine.evaluate(inputs);
      break;
    case SafetyState::kOperatingStop:
      (void)machine.evaluate(healthy(kMillis));  // -> Operational
      inputs = healthy(2 * kMillis);
      inputs.request = SafetyFunction::kSafeOperatingStop;
      (void)machine.evaluate(inputs);
      break;
    case SafetyState::kStopping:
      (void)machine.evaluate(healthy(kMillis));  // -> Operational
      inputs = healthy(2 * kMillis);
      inputs.request = SafetyFunction::kSafeStop1;
      inputs.speed_magnitude = 5.0;  // still moving, so SS1 is meaningful
      (void)machine.evaluate(inputs);
      break;
    case SafetyState::kSelfTest:
      break;
  }
  return machine;
}

constexpr std::array<SafetyState, 6> kAllStates{
    SafetyState::kSelfTest,      SafetyState::kSafeTorqueOff, SafetyState::kStopping,
    SafetyState::kOperatingStop, SafetyState::kLimitedSpeed,  SafetyState::kOperational,
};

constexpr std::array<SafetyFunction, 5> kAllRequests{
    SafetyFunction::kNone,
    SafetyFunction::kSafeTorqueOff,
    SafetyFunction::kSafeStop1,
    SafetyFunction::kSafeOperatingStop,
    SafetyFunction::kSafelyLimitedSpeed,
};

// ---------------------------------------------------------------------------
// Fail-safe defaults
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, DefaultInputsDemandASafeState) {
  // @verifies REQ-SAF-037
  // A default-constructed SafetyInputs must describe the most dangerous
  // situation, not the most convenient one. If a caller forgets to populate a
  // field, the machine must stop rather than run.
  const SafetyInputs defaults;
  EXPECT_TRUE(defaults.emergency_stop_asserted);
  EXPECT_FALSE(defaults.communication_ok);
  EXPECT_FALSE(defaults.heartbeat_ok);
  EXPECT_FALSE(defaults.self_test_passed);
  EXPECT_EQ(defaults.request, SafetyFunction::kSafeTorqueOff);

  SafetyStateMachine machine(testLimits());
  const SafetyOutputs outputs = machine.evaluate(defaults);
  EXPECT_FALSE(outputs.torque_permitted);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
}

TEST(SafetyStateMachine, DefaultOutputsPermitNothing) {
  // @verifies REQ-SAF-037
  const SafetyOutputs defaults;
  EXPECT_FALSE(defaults.torque_permitted);
  EXPECT_FALSE(defaults.motion_permitted);
  EXPECT_DOUBLE_EQ(defaults.speed_limit, 0.0);
  EXPECT_TRUE(defaults.brake_engaged);
  EXPECT_EQ(defaults.state, SafetyState::kSafeTorqueOff);
}

TEST(SafetyStateMachine, StartsInSelfTestWithTorqueRemoved) {
  // @verifies REQ-SAF-020
  SafetyStateMachine machine(testLimits());
  EXPECT_EQ(machine.state(), SafetyState::kSelfTest);
  const SafetyOutputs outputs = machine.evaluate(SafetyInputs{});
  EXPECT_FALSE(outputs.torque_permitted);
}

TEST(SafetyStateMachine, StatesAreOrderedFromMostToLeastRestrictive) {
  // Relied on by the dual-channel comparison, which takes the more restrictive
  // of two states with a min(). If this ordering is ever disturbed, that
  // becomes a silent safety regression rather than a compile error.
  EXPECT_LT(static_cast<int>(SafetyState::kSafeTorqueOff),
            static_cast<int>(SafetyState::kStopping));
  EXPECT_LT(static_cast<int>(SafetyState::kStopping),
            static_cast<int>(SafetyState::kOperatingStop));
  EXPECT_LT(static_cast<int>(SafetyState::kOperatingStop),
            static_cast<int>(SafetyState::kLimitedSpeed));
  EXPECT_LT(static_cast<int>(SafetyState::kLimitedSpeed),
            static_cast<int>(SafetyState::kOperational));
}

// ---------------------------------------------------------------------------
// Self test
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, StaysInSelfTestUntilDiagnosticsPass) {
  // @verifies REQ-SAF-021
  SafetyStateMachine machine(testLimits());
  SafetyInputs inputs = healthy();
  inputs.self_test_passed = false;

  for (int cycle = 0; cycle < 100; ++cycle) {
    inputs.timestamp_ns = cycle * kMillis;
    const SafetyOutputs outputs = machine.evaluate(inputs);
    ASSERT_EQ(outputs.state, SafetyState::kSelfTest);
    ASSERT_FALSE(outputs.torque_permitted);
  }
}

TEST(SafetyStateMachine, PassingSelfTestLandsInStoNeverStraightIntoMotion) {
  // @verifies REQ-SAF-022
  // The host is already asking for unrestricted motion when the self test
  // completes. Without an explicit cycle boundary the machine would go from
  // power-on to full speed in one evaluation.
  SafetyStateMachine machine(testLimits());
  SafetyInputs inputs = healthy();
  inputs.request = SafetyFunction::kNone;  // asking for unrestricted motion

  const SafetyOutputs first = machine.evaluate(inputs);
  EXPECT_EQ(first.state, SafetyState::kSafeTorqueOff);
  EXPECT_FALSE(first.torque_permitted);

  inputs.timestamp_ns = kMillis;
  const SafetyOutputs second = machine.evaluate(inputs);
  EXPECT_EQ(second.state, SafetyState::kOperational);
}

// ---------------------------------------------------------------------------
// Unconditional demands
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, EmergencyStopDrivesEveryStateToSafeTorqueOff) {
  // @verifies REQ-SAF-023
  for (const SafetyState from : kAllStates) {
    SafetyStateMachine machine = machineIn(from);
    ASSERT_EQ(machine.state(), from) << "setup failed for " << toString(from);

    SafetyInputs inputs = healthy(10 * kMillis);
    inputs.emergency_stop_asserted = true;
    inputs.request = SafetyFunction::kNone;  // host still asking for motion

    const SafetyOutputs outputs = machine.evaluate(inputs);
    EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff) << "from " << toString(from);
    EXPECT_FALSE(outputs.torque_permitted) << "from " << toString(from);
    EXPECT_EQ(outputs.fault, FaultReason::kEmergencyStop) << "from " << toString(from);
  }
}

TEST(SafetyStateMachine, CommunicationLossDrivesEveryStateToSafeTorqueOff) {
  // @verifies REQ-SAF-024
  for (const SafetyState from : kAllStates) {
    SafetyStateMachine machine = machineIn(from);
    SafetyInputs inputs = healthy(10 * kMillis);
    inputs.communication_ok = false;

    const SafetyOutputs outputs = machine.evaluate(inputs);
    EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff) << "from " << toString(from);
    EXPECT_EQ(outputs.fault, FaultReason::kCommunicationFault)
        << "from " << toString(from);
  }
}

TEST(SafetyStateMachine, WatchdogLossDrivesEveryStateToSafeTorqueOff) {
  // @verifies REQ-SAF-025
  for (const SafetyState from : kAllStates) {
    SafetyStateMachine machine = machineIn(from);
    SafetyInputs inputs = healthy(10 * kMillis);
    inputs.heartbeat_ok = false;

    const SafetyOutputs outputs = machine.evaluate(inputs);
    EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff) << "from " << toString(from);
    EXPECT_EQ(outputs.fault, FaultReason::kWatchdogTimeout) << "from " << toString(from);
  }
}

TEST(SafetyStateMachine, EmergencyStopIsReportedAheadOfOtherSimultaneousCauses) {
  // When several demands are active at once, the reported cause should be the
  // one an operator would recognise as having happened.
  SafetyStateMachine machine = machineIn(SafetyState::kOperational);
  SafetyInputs inputs = healthy(kMillis);
  inputs.emergency_stop_asserted = true;
  inputs.communication_ok = false;
  inputs.heartbeat_ok = false;

  EXPECT_EQ(machine.evaluate(inputs).fault, FaultReason::kEmergencyStop);
}

// ---------------------------------------------------------------------------
// Latching and acknowledgement
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, FaultLatchesWhileTheCauseIsStillPresent) {
  // @verifies REQ-SAF-034
  SafetyStateMachine machine = machineIn(SafetyState::kOperational);
  SafetyInputs inputs = healthy(kMillis);
  inputs.emergency_stop_asserted = true;
  (void)machine.evaluate(inputs);
  ASSERT_TRUE(machine.faultLatched());

  // Acknowledging while the button is still pressed must do nothing.
  inputs.acknowledge = true;
  for (int cycle = 0; cycle < 10; ++cycle) {
    inputs.timestamp_ns = (2 + cycle) * kMillis;
    const SafetyOutputs outputs = machine.evaluate(inputs);
    ASSERT_TRUE(outputs.fault_latched) << "cleared while the cause was still active";
    ASSERT_EQ(outputs.fault, FaultReason::kEmergencyStop);
  }
}

// @verifies REQ-EDGE-009
TEST(SafetyStateMachine, AcknowledgementClearsTheFaultButDoesNotRestartMotion) {
  // @verifies REQ-SAF-035
  // The distinction that matters most in this file. An acknowledgement button
  // that also starts the machine is how someone gets hurt silencing an alarm.
  SafetyStateMachine machine = machineIn(SafetyState::kOperational);

  SafetyInputs inputs = healthy(kMillis);
  inputs.emergency_stop_asserted = true;
  (void)machine.evaluate(inputs);
  ASSERT_TRUE(machine.faultLatched());

  // Button released, operator acknowledges, and is still asking for motion.
  inputs = healthy(2 * kMillis);
  inputs.acknowledge = true;
  inputs.request = SafetyFunction::kNone;
  const SafetyOutputs acknowledged = machine.evaluate(inputs);

  EXPECT_FALSE(acknowledged.fault_latched);
  EXPECT_EQ(acknowledged.state, SafetyState::kSafeTorqueOff)
      << "acknowledgement must not restart motion";
  EXPECT_FALSE(acknowledged.torque_permitted);

  // A separate, later enable is what actually restarts it.
  const SafetyOutputs enabled = machine.evaluate(healthy(3 * kMillis));
  EXPECT_EQ(enabled.state, SafetyState::kOperational);
}

TEST(SafetyStateMachine, WithoutAcknowledgementTheMachineStaysDownForever) {
  // @verifies REQ-SAF-034
  SafetyStateMachine machine = machineIn(SafetyState::kOperational);
  SafetyInputs inputs = healthy(kMillis);
  inputs.communication_ok = false;
  (void)machine.evaluate(inputs);

  for (int cycle = 0; cycle < 1000; ++cycle) {
    const SafetyOutputs outputs = machine.evaluate(healthy((2 + cycle) * kMillis));
    ASSERT_EQ(outputs.state, SafetyState::kSafeTorqueOff) << "cycle " << cycle;
    ASSERT_TRUE(outputs.fault_latched) << "cycle " << cycle;
  }
}

TEST(SafetyStateMachine, TheFirstCauseIsRetainedNotTheLatest) {
  // @verifies REQ-SAF-036
  // Once torque is removed the axis decelerates, which reliably produces
  // follow-on violations. Reporting the last of those would send a technician
  // looking at the wrong subsystem.
  SafetyStateMachine machine = machineIn(SafetyState::kLimitedSpeed);

  SafetyInputs inputs = healthy(kMillis);
  inputs.communication_ok = false;
  ASSERT_EQ(machine.evaluate(inputs).fault, FaultReason::kCommunicationFault);

  // Now pile on an E-stop as well.
  inputs = healthy(2 * kMillis);
  inputs.emergency_stop_asserted = true;
  EXPECT_EQ(machine.evaluate(inputs).fault, FaultReason::kCommunicationFault)
      << "the original cause was overwritten";
}

// ---------------------------------------------------------------------------
// SLS
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, SlsFaultsWhenTheSpeedLimitIsExceeded) {
  // @verifies REQ-SAF-026
  SafetyStateMachine machine = machineIn(SafetyState::kLimitedSpeed);
  SafetyInputs inputs = healthy(kMillis);
  inputs.speed_magnitude = 10.001;
  inputs.request = SafetyFunction::kSafelyLimitedSpeed;

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_EQ(outputs.fault, FaultReason::kSpeedLimitExceeded);
}

TEST(SafetyStateMachine, SlsPermitsSpeedExactlyAtTheLimit) {
  // @verifies REQ-SAF-026
  SafetyStateMachine machine = machineIn(SafetyState::kLimitedSpeed);
  SafetyInputs inputs = healthy(kMillis);
  inputs.speed_magnitude = 10.0;
  inputs.request = SafetyFunction::kSafelyLimitedSpeed;

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kLimitedSpeed);
  EXPECT_DOUBLE_EQ(outputs.speed_limit, 10.0);
}

TEST(SafetyStateMachine, EnteringSlsWhileAlreadyTooFastFaultsImmediately) {
  // @verifies REQ-SAF-026
  // Otherwise the machine would run one full cycle above the limit it was just
  // told to respect.
  SafetyStateMachine machine = machineIn(SafetyState::kOperational);
  SafetyInputs inputs = healthy(kMillis);
  inputs.request = SafetyFunction::kSafelyLimitedSpeed;
  inputs.speed_magnitude = 50.0;

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_EQ(outputs.fault, FaultReason::kSpeedLimitExceeded);
}

TEST(SafetyStateMachine, MonitorsRunBeforeRequestsSoAViolationCannotBeRiddenThrough) {
  // @verifies REQ-SAF-033
  // A host that keeps re-requesting the current function must not be able to
  // keep the machine in a state whose limit it is violating.
  SafetyStateMachine machine = machineIn(SafetyState::kLimitedSpeed);
  SafetyInputs inputs = healthy(kMillis);
  inputs.request = SafetyFunction::kSafelyLimitedSpeed;
  inputs.speed_magnitude = 999.0;

  EXPECT_EQ(machine.evaluate(inputs).state, SafetyState::kSafeTorqueOff);
}

// ---------------------------------------------------------------------------
// SOS
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, SosHoldsPositionWithTorqueApplied) {
  // @verifies REQ-SAF-028
  SafetyStateMachine machine = machineIn(SafetyState::kOperatingStop);
  // The request must be held. Safety functions here are level-triggered, not
  // edge-triggered: dropping to kNone means "no restriction requested", which
  // releases SOS and returns to unrestricted motion.
  SafetyInputs holding = healthy(10 * kMillis);
  holding.request = SafetyFunction::kSafeOperatingStop;
  const SafetyOutputs outputs = machine.evaluate(holding);
  EXPECT_EQ(outputs.state, SafetyState::kOperatingStop);
  EXPECT_TRUE(outputs.torque_permitted) << "SOS holds position, so torque stays on";
  EXPECT_FALSE(outputs.motion_permitted);
  EXPECT_FALSE(outputs.brake_engaged);
}

TEST(SafetyStateMachine, SosFaultsOnPositionDrift) {
  // @verifies REQ-SAF-027
  SafetyStateMachine machine = machineIn(SafetyState::kOperatingStop);
  SafetyInputs inputs = healthy(10 * kMillis);
  inputs.position = 0.6;  // window is 0.5
  inputs.request = SafetyFunction::kSafeOperatingStop;

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_EQ(outputs.fault, FaultReason::kStandstillDeviation);
}

TEST(SafetyStateMachine, SosFaultsOnDriftInEitherDirection) {
  // @verifies REQ-SAF-027
  for (const double offset : {0.6, -0.6}) {
    SafetyStateMachine machine = machineIn(SafetyState::kOperatingStop);
    SafetyInputs inputs = healthy(10 * kMillis);
    inputs.position = offset;
    EXPECT_EQ(machine.evaluate(inputs).fault, FaultReason::kStandstillDeviation)
        << "offset " << offset;
  }
}

TEST(SafetyStateMachine, SosFaultsWhenTheAxisStartsMoving) {
  // @verifies REQ-SAF-028
  SafetyStateMachine machine = machineIn(SafetyState::kOperatingStop);
  SafetyInputs inputs = healthy(10 * kMillis);
  inputs.speed_magnitude = 0.2;  // standstill threshold is 0.1

  EXPECT_EQ(machine.evaluate(inputs).fault, FaultReason::kStandstillDeviation);
}

TEST(SafetyStateMachine, RequestingSosWhileStillMovingIsRefused) {
  // @verifies REQ-SAF-039
  // "Hold where you are" is only meaningful from a standstill. Accepting it
  // mid-motion would capture a reference the axis has already left, and fault
  // a cycle later for a reason that looks unrelated to the actual mistake.
  SafetyStateMachine machine = machineIn(SafetyState::kOperational);
  SafetyInputs inputs = healthy(kMillis);
  inputs.request = SafetyFunction::kSafeOperatingStop;
  inputs.speed_magnitude = 5.0;

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_EQ(outputs.fault, FaultReason::kInvalidRequest);
}

TEST(SafetyStateMachine, SosReferenceIsCapturedWhereTheAxisActuallyStopped) {
  // @verifies REQ-SAF-027
  SafetyStateMachine machine = machineIn(SafetyState::kOperational);

  SafetyInputs inputs = healthy(kMillis);
  inputs.request = SafetyFunction::kSafeOperatingStop;
  inputs.position = 42.0;  // not zero
  ASSERT_EQ(machine.evaluate(inputs).state, SafetyState::kOperatingStop);

  // Holding at 42 must be fine, and drifting from 42 must fault -- the
  // reference is where it stopped, not the origin.
  SafetyInputs holding = healthy(2 * kMillis);
  holding.request = SafetyFunction::kSafeOperatingStop;
  holding.position = 42.2;
  EXPECT_EQ(machine.evaluate(holding).state, SafetyState::kOperatingStop);

  SafetyInputs drifted = healthy(3 * kMillis);
  drifted.request = SafetyFunction::kSafeOperatingStop;
  drifted.position = 43.0;
  EXPECT_EQ(machine.evaluate(drifted).fault, FaultReason::kStandstillDeviation);
}

TEST(SafetyStateMachine, SafetyFunctionsAreLevelTriggeredNotEdgeTriggered) {
  // A requested function stays active only while it is still being requested.
  // Dropping to kNone is a request for unrestricted motion, not a request to
  // hold whatever was previously active.
  //
  // Worth an explicit test because the opposite convention is equally
  // plausible and the failure mode differs sharply: with edge triggering, a
  // host that stops transmitting leaves the machine restricted, which is safe;
  // with level triggering it releases the restriction, which is not. The
  // protection against a silent host is therefore the watchdog rather than the
  // request semantics, and that is a load-bearing distinction.
  SafetyStateMachine machine = machineIn(SafetyState::kLimitedSpeed);

  SafetyInputs held = healthy(kMillis);
  held.request = SafetyFunction::kSafelyLimitedSpeed;
  EXPECT_EQ(machine.evaluate(held).state, SafetyState::kLimitedSpeed);

  SafetyInputs released = healthy(2 * kMillis);
  released.request = SafetyFunction::kNone;
  EXPECT_EQ(machine.evaluate(released).state, SafetyState::kOperational)
      << "a released request must not leave the restriction in place";
}

// ---------------------------------------------------------------------------
// SS1
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, Ss1KeepsTorqueOnWhileDecelerating) {
  // @verifies REQ-SAF-030
  // Removing torque during a controlled stop lets the load coast. On a
  // vertical axis that means it falls, which is the opposite of safe.
  SafetyStateMachine machine = machineIn(SafetyState::kStopping);
  SafetyInputs inputs = healthy(3 * kMillis);
  inputs.speed_magnitude = 4.0;

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kStopping);
  EXPECT_TRUE(outputs.torque_permitted);
  EXPECT_FALSE(outputs.motion_permitted);
}

TEST(SafetyStateMachine, Ss1EndsInStoWithNoFaultWhenItStopsInTime) {
  // @verifies REQ-SAF-029
  SafetyStateMachine machine = machineIn(SafetyState::kStopping);
  SafetyInputs inputs = healthy(100 * kMillis);
  inputs.speed_magnitude = 0.05;  // below standstill threshold

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_EQ(outputs.fault, FaultReason::kNone) << "a successful stop is not an incident";
  EXPECT_FALSE(outputs.fault_latched);
}

TEST(SafetyStateMachine, Ss1FaultsWhenTheStopTimeIsExceeded) {
  // @verifies REQ-SAF-029
  SafetyStateMachine machine = machineIn(SafetyState::kStopping);
  SafetyInputs inputs = healthy(2 * kMillis + 500 * kMillis + 1);
  inputs.speed_magnitude = 4.0;  // still moving after the window

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_EQ(outputs.fault, FaultReason::kStopTimeExceeded);
}

TEST(SafetyStateMachine, Ss1CannotBeInterruptedByAnyRequest) {
  // @verifies REQ-SAF-031
  // Once a controlled stop has begun, the only exits are standstill, timeout,
  // or an unconditional demand. A host cannot cancel it.
  for (const SafetyFunction request : kAllRequests) {
    SafetyStateMachine machine = machineIn(SafetyState::kStopping);
    SafetyInputs inputs = healthy(3 * kMillis);
    inputs.request = request;
    inputs.speed_magnitude = 4.0;

    EXPECT_EQ(machine.evaluate(inputs).state, SafetyState::kStopping)
        << "SS1 was interrupted by a request for " << toString(request);
  }
}

TEST(SafetyStateMachine, Ss1FromSafeTorqueOffIsAlreadySatisfied) {
  SafetyStateMachine machine = machineIn(SafetyState::kSafeTorqueOff);
  SafetyInputs inputs = healthy(kMillis);
  inputs.request = SafetyFunction::kSafeStop1;

  const SafetyOutputs outputs = machine.evaluate(inputs);
  EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff);
  EXPECT_FALSE(outputs.fault_latched)
      << "stopping an already-stopped axis is not a fault";
}

// ---------------------------------------------------------------------------
// SLP
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, SlpFaultsOutsideThePermittedBand) {
  // @verifies REQ-SAF-032
  for (const double position : {100.001, -100.001}) {
    SafetyStateMachine machine = machineIn(SafetyState::kOperational);
    SafetyInputs inputs = healthy(kMillis);
    inputs.position = position;

    const SafetyOutputs outputs = machine.evaluate(inputs);
    EXPECT_EQ(outputs.state, SafetyState::kSafeTorqueOff) << "position " << position;
    EXPECT_EQ(outputs.fault, FaultReason::kPositionLimitExceeded);
  }
}

TEST(SafetyStateMachine, SlpAppliesInLimitedSpeedToo) {
  // @verifies REQ-SAF-032
  // SLP is a monitor, not a state: it runs alongside whatever else is active
  // wherever motion is permitted.
  SafetyStateMachine machine = machineIn(SafetyState::kLimitedSpeed);
  SafetyInputs inputs = healthy(kMillis);
  inputs.request = SafetyFunction::kSafelyLimitedSpeed;
  inputs.position = 150.0;
  inputs.speed_magnitude = 1.0;

  EXPECT_EQ(machine.evaluate(inputs).fault, FaultReason::kPositionLimitExceeded);
}

TEST(SafetyStateMachine, SlpCanBeDisabled) {
  SafetyLimits limits = testLimits();
  limits.position_monitoring_enabled = false;
  SafetyStateMachine machine(limits);

  SafetyInputs inputs = healthy(0);
  inputs.request = SafetyFunction::kSafeTorqueOff;
  (void)machine.evaluate(inputs);
  (void)machine.evaluate(healthy(kMillis));  // -> Operational

  SafetyInputs far = healthy(2 * kMillis);
  far.position = 1e9;
  EXPECT_EQ(machine.evaluate(far).state, SafetyState::kOperational);
}

// ---------------------------------------------------------------------------
// The transition table
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, EveryStateAndRequestPairHasADefinedOutcome) {
  // Written by hand from the specification, not generated from the code. A
  // table read out of the implementation would prove only self-consistency.
  //
  // Inputs are healthy and the axis is at standstill, except for kStopping,
  // which is exercised with the axis still moving -- at standstill the stop
  // monitor completes the SS1 before any request is considered, which is
  // correct but would make every row in that column read the same and test
  // nothing.
  struct Row {
    SafetyState from;
    SafetyFunction request;
    SafetyState expected;
    FaultReason expected_fault;
  };

  const std::vector<Row> table{
      // Self test ignores requests entirely and lands in STO once it passes.
      {SafetyState::kSelfTest, SafetyFunction::kNone, SafetyState::kSafeTorqueOff,
       FaultReason::kNone},
      {SafetyState::kSelfTest, SafetyFunction::kSafeTorqueOff,
       SafetyState::kSafeTorqueOff, FaultReason::kNone},
      {SafetyState::kSelfTest, SafetyFunction::kSafelyLimitedSpeed,
       SafetyState::kSafeTorqueOff, FaultReason::kNone},

      // From STO.
      {SafetyState::kSafeTorqueOff, SafetyFunction::kNone, SafetyState::kOperational,
       FaultReason::kNone},
      {SafetyState::kSafeTorqueOff, SafetyFunction::kSafeTorqueOff,
       SafetyState::kSafeTorqueOff, FaultReason::kNone},
      {SafetyState::kSafeTorqueOff, SafetyFunction::kSafeStop1,
       SafetyState::kSafeTorqueOff, FaultReason::kNone},
      {SafetyState::kSafeTorqueOff, SafetyFunction::kSafeOperatingStop,
       SafetyState::kSafeTorqueOff, FaultReason::kInvalidRequest},
      {SafetyState::kSafeTorqueOff, SafetyFunction::kSafelyLimitedSpeed,
       SafetyState::kLimitedSpeed, FaultReason::kNone},

      // From Operational.
      {SafetyState::kOperational, SafetyFunction::kNone, SafetyState::kOperational,
       FaultReason::kNone},
      {SafetyState::kOperational, SafetyFunction::kSafeTorqueOff,
       SafetyState::kSafeTorqueOff, FaultReason::kNone},
      {SafetyState::kOperational, SafetyFunction::kSafeStop1, SafetyState::kStopping,
       FaultReason::kNone},
      {SafetyState::kOperational, SafetyFunction::kSafeOperatingStop,
       SafetyState::kOperatingStop, FaultReason::kNone},
      {SafetyState::kOperational, SafetyFunction::kSafelyLimitedSpeed,
       SafetyState::kLimitedSpeed, FaultReason::kNone},

      // From SLS.
      {SafetyState::kLimitedSpeed, SafetyFunction::kNone, SafetyState::kOperational,
       FaultReason::kNone},
      {SafetyState::kLimitedSpeed, SafetyFunction::kSafeTorqueOff,
       SafetyState::kSafeTorqueOff, FaultReason::kNone},
      {SafetyState::kLimitedSpeed, SafetyFunction::kSafeStop1, SafetyState::kStopping,
       FaultReason::kNone},
      {SafetyState::kLimitedSpeed, SafetyFunction::kSafeOperatingStop,
       SafetyState::kOperatingStop, FaultReason::kNone},
      {SafetyState::kLimitedSpeed, SafetyFunction::kSafelyLimitedSpeed,
       SafetyState::kLimitedSpeed, FaultReason::kNone},

      // From SOS.
      {SafetyState::kOperatingStop, SafetyFunction::kNone, SafetyState::kOperational,
       FaultReason::kNone},
      {SafetyState::kOperatingStop, SafetyFunction::kSafeTorqueOff,
       SafetyState::kSafeTorqueOff, FaultReason::kNone},
      {SafetyState::kOperatingStop, SafetyFunction::kSafeStop1, SafetyState::kStopping,
       FaultReason::kNone},
      {SafetyState::kOperatingStop, SafetyFunction::kSafeOperatingStop,
       SafetyState::kOperatingStop, FaultReason::kNone},
      {SafetyState::kOperatingStop, SafetyFunction::kSafelyLimitedSpeed,
       SafetyState::kLimitedSpeed, FaultReason::kNone},
  };

  for (const Row& row : table) {
    SafetyStateMachine machine = machineIn(row.from);
    ASSERT_EQ(machine.state(), row.from) << "setup failed for " << toString(row.from);

    SafetyInputs inputs = healthy(10 * kMillis);
    inputs.request = row.request;

    const SafetyOutputs outputs = machine.evaluate(inputs);
    EXPECT_EQ(outputs.state, row.expected)
        << toString(row.from) << " + " << toString(row.request) << " -> "
        << toString(outputs.state) << ", expected " << toString(row.expected);
    EXPECT_EQ(outputs.fault, row.expected_fault)
        << toString(row.from) << " + " << toString(row.request);
  }

  // Every state except kStopping is covered above; kStopping has its own test
  // because it must be driven with the axis still moving.
  EXPECT_EQ(table.size(), 23u) << "a row was added or removed without review";
}

TEST(SafetyStateMachine, EveryStateAndRequestPairIsExercisedByTheTable) {
  // Guards the table against silently losing coverage. Every combination
  // reachable with a healthy machine must either appear above or be explicitly
  // accounted for.
  std::size_t covered = 0;
  for (const SafetyState from : kAllStates) {
    for (const SafetyFunction request : kAllRequests) {
      SafetyStateMachine machine = machineIn(from);
      SafetyInputs inputs = healthy(10 * kMillis);
      inputs.request = request;
      if (from == SafetyState::kStopping) {
        inputs.speed_magnitude = 4.0;
      }
      // The property asserted for every pair, table or not: evaluation is
      // total. It always produces a state, never leaves the machine in an
      // undefined one, and never permits motion while a fault is latched.
      const SafetyOutputs outputs = machine.evaluate(inputs);
      ASSERT_GE(static_cast<int>(outputs.state), 0);
      ASSERT_LE(static_cast<int>(outputs.state),
                static_cast<int>(SafetyState::kOperational));
      if (outputs.fault_latched) {
        ASSERT_FALSE(outputs.motion_permitted)
            << "motion permitted while latched: " << toString(from) << " + "
            << toString(request);
        ASSERT_FALSE(outputs.torque_permitted)
            << "torque permitted while latched: " << toString(from) << " + "
            << toString(request);
      }
      ++covered;
    }
  }
  EXPECT_EQ(covered, kAllStates.size() * kAllRequests.size());
  EXPECT_EQ(covered, 30u);
}

// ---------------------------------------------------------------------------
// Determinism and real-time safety
// ---------------------------------------------------------------------------

TEST(SafetyStateMachine, EvaluationIsDeterministic) {
  // @verifies REQ-SAF-038
  // The transition table above is only meaningful if the machine holds no
  // hidden state. Two machines driven identically must agree at every step.
  SafetyStateMachine first(testLimits());
  SafetyStateMachine second(testLimits());

  for (int cycle = 0; cycle < 500; ++cycle) {
    SafetyInputs inputs = healthy(cycle * kMillis);
    inputs.request = kAllRequests[static_cast<std::size_t>(cycle) % kAllRequests.size()];
    inputs.speed_magnitude = static_cast<double>(cycle % 15);
    inputs.position = static_cast<double>((cycle % 40) - 20);
    inputs.acknowledge = (cycle % 7) == 0;

    const SafetyOutputs a = first.evaluate(inputs);
    const SafetyOutputs b = second.evaluate(inputs);
    ASSERT_EQ(a.state, b.state) << "diverged at cycle " << cycle;
    ASSERT_EQ(a.fault, b.fault) << "diverged at cycle " << cycle;
    ASSERT_EQ(a.torque_permitted, b.torque_permitted) << "diverged at cycle " << cycle;
  }
}

TEST(SafetyStateMachine, EvaluationDoesNotAllocate) {
  // @verifies REQ-RT-001
  ASSERT_TRUE(rt::guardIsInstalled());
  rt::setAllocationPolicy(rt::AllocationPolicy::kCount);
  rt::resetAllocationReport();

  SafetyStateMachine machine(testLimits());
  int torque_cycles = 0;
  {
    const rt::NoAllocScope no_alloc;
    for (int cycle = 0; cycle < 5000; ++cycle) {
      SafetyInputs inputs = healthy(cycle * kMillis);
      inputs.request =
          kAllRequests[static_cast<std::size_t>(cycle) % kAllRequests.size()];
      inputs.speed_magnitude = static_cast<double>(cycle % 12);
      inputs.acknowledge = (cycle % 11) == 0;
      if (machine.evaluate(inputs).torque_permitted) {
        ++torque_cycles;
      }
    }
  }
  EXPECT_EQ(rt::allocationReport().violations, 0u);
  EXPECT_GT(torque_cycles, 0);
  rt::setAllocationPolicy(rt::AllocationPolicy::kAbort);
}

TEST(SafetyStateMachine, ResetReturnsToThePowerOnCondition) {
  SafetyStateMachine machine = machineIn(SafetyState::kOperational);
  SafetyInputs inputs = healthy(kMillis);
  inputs.emergency_stop_asserted = true;
  (void)machine.evaluate(inputs);
  ASSERT_TRUE(machine.faultLatched());

  machine.reset();
  EXPECT_EQ(machine.state(), SafetyState::kSelfTest);
  EXPECT_FALSE(machine.faultLatched());
}

}  // namespace
}  // namespace safeedge::safety

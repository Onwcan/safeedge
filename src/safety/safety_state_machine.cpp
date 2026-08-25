// SPDX-License-Identifier: Apache-2.0
#include "safeedge/safety/safety_state_machine.hpp"

#include <cmath>

namespace safeedge::safety {

void SafetyStateMachine::reset() noexcept {
  state_ = SafetyState::kSelfTest;
  fault_ = FaultReason::kNone;
  stop_started_ns_ = 0;
  standstill_reference_ = 0.0;
}

// @satisfies REQ-SAF-036
void SafetyStateMachine::enterSafeTorqueOff(FaultReason reason) noexcept {
  state_ = SafetyState::kSafeTorqueOff;
  // Never overwrite an already-latched fault with a later one. The first cause
  // is the diagnostic worth keeping: once torque is removed the machine
  // decelerates, which reliably produces follow-on violations, and letting
  // those overwrite the original would report "standstill deviation" for an
  // event that actually started as a communication failure.
  if (reason != FaultReason::kNone && fault_ == FaultReason::kNone) {
    fault_ = reason;
  }
}

void SafetyStateMachine::forceFault(FaultReason reason) noexcept {
  enterSafeTorqueOff(reason);
}

// @satisfies REQ-SAF-023
// @satisfies REQ-SAF-024
// @satisfies REQ-SAF-025
bool SafetyStateMachine::unconditionalDemandActive(
    const SafetyInputs& inputs) const noexcept {
  return inputs.emergency_stop_asserted || !inputs.communication_ok ||
         !inputs.heartbeat_ok;
}

FaultReason SafetyStateMachine::unconditionalDemandReason(
    const SafetyInputs& inputs) const noexcept {
  // Ordered by how directly each represents a person in danger. An E-stop is
  // someone's hand on a button; the others are inferred conditions. When more
  // than one is true at once, the one reported should be the one an operator
  // would recognise.
  if (inputs.emergency_stop_asserted) {
    return FaultReason::kEmergencyStop;
  }
  if (!inputs.communication_ok) {
    return FaultReason::kCommunicationFault;
  }
  if (!inputs.heartbeat_ok) {
    return FaultReason::kWatchdogTimeout;
  }
  return FaultReason::kNone;
}

SafetyOutputs SafetyStateMachine::outputsFor(SafetyState state) const noexcept {
  SafetyOutputs outputs;
  outputs.state = state;
  outputs.fault = fault_;
  outputs.fault_latched = fault_ != FaultReason::kNone;

  switch (state) {
    case SafetyState::kSelfTest:
    case SafetyState::kSafeTorqueOff:
      outputs.torque_permitted = false;
      outputs.motion_permitted = false;
      outputs.speed_limit = 0.0;
      outputs.brake_engaged = true;
      break;

    case SafetyState::kStopping:
      // @satisfies REQ-SAF-030
      // SS1: the drive keeps torque so it can decelerate under control.
      // Removing torque here would let the load coast, which for a vertical
      // axis means it falls -- the opposite of safe.
      outputs.torque_permitted = true;
      outputs.motion_permitted = false;
      outputs.speed_limit = 0.0;
      outputs.brake_engaged = false;
      break;

    case SafetyState::kOperatingStop:
      // SOS: powered, actively holding position. No commanded motion.
      outputs.torque_permitted = true;
      outputs.motion_permitted = false;
      outputs.speed_limit = 0.0;
      outputs.brake_engaged = false;
      break;

    case SafetyState::kLimitedSpeed:
      outputs.torque_permitted = true;
      outputs.motion_permitted = true;
      outputs.speed_limit = limits_.limited_speed;
      outputs.brake_engaged = false;
      break;

    case SafetyState::kOperational:
      outputs.torque_permitted = true;
      outputs.motion_permitted = true;
      outputs.speed_limit = std::numeric_limits<double>::infinity();
      outputs.brake_engaged = false;
      break;
  }
  return outputs;
}

SafetyOutputs SafetyStateMachine::evaluate(const SafetyInputs& inputs) noexcept {
  // --- 1. Unconditional demands -------------------------------------------
  // Checked before anything else, from every state including kSelfTest. These
  // are not requests to be arbitrated against other requests; they are
  // conditions under which no amount of host insistence may keep torque on.
  if (unconditionalDemandActive(inputs)) {
    enterSafeTorqueOff(unconditionalDemandReason(inputs));
    return outputsFor(state_);
  }

  // @satisfies REQ-SAF-034
  // @satisfies REQ-SAF-035
  // --- 2. Fault latch and acknowledgement ----------------------------------
  if (fault_ != FaultReason::kNone) {
    // Reaching here means every unconditional demand has cleared, so the cause
    // is genuinely gone rather than merely un-asserted for one cycle.
    //
    // Acknowledgement clears the fault but does NOT restart motion: the
    // machine stays in STO and a separate enable request is required to leave
    // it. Collapsing those two steps into one is how an acknowledgement button
    // becomes a start button, which is precisely the confusion that gets
    // people hurt when someone presses it to silence an alarm.
    if (inputs.acknowledge) {
      fault_ = FaultReason::kNone;
    }
    state_ = SafetyState::kSafeTorqueOff;
    return outputsFor(state_);
  }

  // @satisfies REQ-SAF-033
  // --- 3. Per-state monitors -----------------------------------------------
  // Before requests, so that a host repeatedly requesting the current function
  // cannot ride through a limit violation.
  switch (state_) {
    case SafetyState::kSelfTest:
      // @satisfies REQ-SAF-021
      // @satisfies REQ-SAF-022
      if (!inputs.self_test_passed) {
        // Not yet complete is indistinguishable from failed, and both mean the
        // same thing: stay where torque is off. A self test that never
        // completes therefore never enables the drive.
        return outputsFor(state_);
      }
      // Passing the self test lands in STO, and returns immediately so that
      // requests present in the same cycle are not acted on. Without the early
      // return, a host already asking for unrestricted motion would go from
      // power-on to full speed in a single evaluation, which makes "never
      // straight into motion" a comment rather than a property.
      state_ = SafetyState::kSafeTorqueOff;
      return outputsFor(state_);

    case SafetyState::kLimitedSpeed:
      // @satisfies REQ-SAF-026
      if (inputs.speed_magnitude > limits_.limited_speed) {
        enterSafeTorqueOff(FaultReason::kSpeedLimitExceeded);
        return outputsFor(state_);
      }
      break;

    case SafetyState::kOperatingStop:
      // @satisfies REQ-SAF-027
      // @satisfies REQ-SAF-028
      if (std::fabs(inputs.position - standstill_reference_) >
          limits_.standstill_window) {
        enterSafeTorqueOff(FaultReason::kStandstillDeviation);
        return outputsFor(state_);
      }
      if (inputs.speed_magnitude > limits_.standstill_speed) {
        enterSafeTorqueOff(FaultReason::kStandstillDeviation);
        return outputsFor(state_);
      }
      break;

    case SafetyState::kStopping:
      // @satisfies REQ-SAF-029
      // @satisfies REQ-SAF-031
      if (inputs.speed_magnitude <= limits_.standstill_speed) {
        // Stopped in time. STO with no fault -- this is a successful SS1, not
        // an incident.
        enterSafeTorqueOff(FaultReason::kNone);
        return outputsFor(state_);
      }
      if (inputs.timestamp_ns - stop_started_ns_ > limits_.stop_time_limit_ns) {
        // The drive did not reach standstill inside the monitored window.
        // Torque comes off regardless -- SS1-t always ends in STO -- but this
        // ending is a fault, because the deceleration was not what was
        // promised.
        enterSafeTorqueOff(FaultReason::kStopTimeExceeded);
        return outputsFor(state_);
      }
      // Still decelerating and still inside the window. A request cannot
      // interrupt an SS1 in progress; the only exits are standstill, timeout,
      // or an unconditional demand, all handled above.
      return outputsFor(state_);

    case SafetyState::kSafeTorqueOff:
    case SafetyState::kOperational:
      break;
  }

  // SLP is a monitor rather than a state: it applies in every state where
  // motion is permitted, alongside whatever else is active.
  // @satisfies REQ-SAF-032
  if (limits_.position_monitoring_enabled &&
      (state_ == SafetyState::kOperational || state_ == SafetyState::kLimitedSpeed)) {
    if (inputs.position < limits_.position_min ||
        inputs.position > limits_.position_max) {
      enterSafeTorqueOff(FaultReason::kPositionLimitExceeded);
      return outputsFor(state_);
    }
  }

  // --- 4. Requested transitions --------------------------------------------
  switch (inputs.request) {
    case SafetyFunction::kSafeTorqueOff:
      // Always honoured, from anywhere, without a fault. A deliberate STO is a
      // normal operation, not an incident.
      enterSafeTorqueOff(FaultReason::kNone);
      break;

    case SafetyFunction::kSafeStop1:
      if (state_ == SafetyState::kOperational || state_ == SafetyState::kLimitedSpeed ||
          state_ == SafetyState::kOperatingStop) {
        state_ = SafetyState::kStopping;
        stop_started_ns_ = inputs.timestamp_ns;
      } else if (state_ == SafetyState::kSafeTorqueOff) {
        // Already stopped with torque off. SS1 from here is satisfied by
        // definition rather than being an error.
        state_ = SafetyState::kSafeTorqueOff;
      }
      break;

    case SafetyFunction::kSafeOperatingStop:
      if (state_ == SafetyState::kOperational || state_ == SafetyState::kLimitedSpeed) {
        if (inputs.speed_magnitude > limits_.standstill_speed) {
          // @satisfies REQ-SAF-039
          // SOS means "hold where you are", which is only meaningful from a
          // standstill. Entering it while still moving would immediately
          // register as deviation from a reference captured mid-motion, so the
          // request is refused explicitly rather than accepted and then
          // faulted a cycle later for a reason that would look unrelated.
          enterSafeTorqueOff(FaultReason::kInvalidRequest);
          break;
        }
        state_ = SafetyState::kOperatingStop;
        standstill_reference_ = inputs.position;
      } else if (state_ == SafetyState::kSafeTorqueOff) {
        // Would require re-energising to hold position. Refused: enable first.
        enterSafeTorqueOff(FaultReason::kInvalidRequest);
      }
      break;

    case SafetyFunction::kSafelyLimitedSpeed:
      if (state_ == SafetyState::kSafeTorqueOff || state_ == SafetyState::kOperational ||
          state_ == SafetyState::kOperatingStop) {
        // Entering SLS while already over the limit must fault rather than
        // silently permit one cycle at speed.
        if (inputs.speed_magnitude > limits_.limited_speed) {
          enterSafeTorqueOff(FaultReason::kSpeedLimitExceeded);
          break;
        }
        state_ = SafetyState::kLimitedSpeed;
      }
      break;

    case SafetyFunction::kNone:
      // A request for unrestricted motion. Granted only from a state that is
      // already energised or safely stopped -- never straight out of SS1,
      // which is handled above and cannot reach here.
      if (state_ == SafetyState::kSafeTorqueOff || state_ == SafetyState::kLimitedSpeed ||
          state_ == SafetyState::kOperatingStop) {
        state_ = SafetyState::kOperational;
      }
      break;
  }

  return outputsFor(state_);
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char* toString(SafetyState state) noexcept {
  switch (state) {
    case SafetyState::kSelfTest:
      return "SelfTest";
    case SafetyState::kSafeTorqueOff:
      return "STO";
    case SafetyState::kStopping:
      return "SS1-Stopping";
    case SafetyState::kOperatingStop:
      return "SOS";
    case SafetyState::kLimitedSpeed:
      return "SLS";
    case SafetyState::kOperational:
      return "Operational";
  }
  return "?";
}

const char* toString(FaultReason reason) noexcept {
  switch (reason) {
    case FaultReason::kNone:
      return "none";
    case FaultReason::kSelfTestFailed:
      return "self-test failed";
    case FaultReason::kEmergencyStop:
      return "emergency stop";
    case FaultReason::kSpeedLimitExceeded:
      return "speed limit exceeded";
    case FaultReason::kPositionLimitExceeded:
      return "position limit exceeded";
    case FaultReason::kStandstillDeviation:
      return "standstill deviation";
    case FaultReason::kStopTimeExceeded:
      return "stop time exceeded";
    case FaultReason::kCommunicationFault:
      return "communication fault";
    case FaultReason::kWatchdogTimeout:
      return "watchdog timeout";
    case FaultReason::kChannelDiscrepancy:
      return "channel discrepancy";
    case FaultReason::kInvalidRequest:
      return "invalid request";
  }
  return "?";
}

const char* toString(SafetyFunction function) noexcept {
  switch (function) {
    case SafetyFunction::kNone:
      return "none";
    case SafetyFunction::kSafeTorqueOff:
      return "STO";
    case SafetyFunction::kSafeStop1:
      return "SS1";
    case SafetyFunction::kSafeOperatingStop:
      return "SOS";
    case SafetyFunction::kSafelyLimitedSpeed:
      return "SLS";
  }
  return "?";
}

}  // namespace safeedge::safety

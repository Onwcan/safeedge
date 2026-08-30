// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <limits>

namespace safeedge::safety {

/// Safety functions from IEC 61800-5-2, as requested by the host.
///
/// Implemented here: STO, SS1 (time-monitored variant, SS1-t), SOS, SLS, SLP.
///
/// Deliberately not implemented: SS2 (controlled stop to SOS), SBC (safe brake
/// control), SDI (safe direction), SLI (safe limited increment), SMS (safe
/// maximum speed), SSM (safe speed monitor), SAR/SLA. Each needs either drive
/// hardware this does not model or a second monitored quantity. Listing them
/// is not padding: a reviewer needs to know the boundary of what is claimed,
/// and "the rest of 61800-5-2" is a much bigger set than the part built here.
///
/// Requests are **level-triggered, not edge-triggered**: a function stays
/// active only while it is still being requested, and dropping back to kNone
/// releases it.
///
/// The opposite convention is equally plausible, and the difference matters:
/// with edge triggering, a host that falls silent leaves the machine
/// restricted, which is safe. With level triggering it releases the
/// restriction, which is not -- so protection against a silent host comes from
/// the watchdog and the communication monitor, not from the request semantics.
/// That is a load-bearing distinction, and it is why `communication_ok` and
/// `heartbeat_ok` are unconditional demands rather than advisory inputs.
enum class SafetyFunction : std::uint8_t {
  /// No restriction requested. Not the same as "safe" -- it means the host is
  /// asking for unrestricted motion.
  kNone,
  kSafeTorqueOff,
  kSafeStop1,
  kSafeOperatingStop,
  kSafelyLimitedSpeed,
};

/// The observable state of the supervisor.
///
/// Ordered so that lower values are more restrictive, which makes "take the
/// safer of two states" a min() and removes a class of ordering mistakes from
/// the dual-channel comparison.
enum class SafetyState : std::uint8_t {
  /// Torque removed. The safe state, and where the machine starts.
  kSafeTorqueOff = 0,
  /// Power-on self test. Torque is not permitted here either.
  kSelfTest = 1,
  /// SS1 in progress: controlled deceleration under a time monitor, ending in
  /// STO whether or not the drive actually stops.
  kStopping = 2,
  /// SOS: holding position with power applied.
  kOperatingStop = 3,
  /// SLS: motion permitted below a speed limit.
  kLimitedSpeed = 4,
  /// Unrestricted motion permitted.
  kOperational = 5,
};

/// Why the supervisor latched. Reported alongside the state so a diagnostic
/// system can distinguish a deliberate stop from a fault reaction -- they look
/// identical from the outside, and conflating them is how an intermittent
/// fault gets acknowledged away as a normal stop.
enum class FaultReason : std::uint8_t {
  kNone,
  kSelfTestFailed,
  kEmergencyStop,
  kSpeedLimitExceeded,
  kPositionLimitExceeded,
  kStandstillDeviation,
  kStopTimeExceeded,
  kCommunicationFault,
  kWatchdogTimeout,
  kChannelDiscrepancy,
  kInvalidRequest,
};

/// Configured limits. Units are the caller's, but must be consistent: the
/// supervisor compares magnitudes and never converts. Mixing rad/s into a
/// machine configured in m/s produces a limit that is numerically valid and
/// physically meaningless, which no amount of range checking here would catch.
struct SafetyLimits {
  /// SLS: speed must stay at or below this while kLimitedSpeed is active.
  double limited_speed{0.0};
  /// SLP: permitted position band. Monitored whenever motion is permitted.
  double position_min{-std::numeric_limits<double>::infinity()};
  double position_max{std::numeric_limits<double>::infinity()};
  bool position_monitoring_enabled{false};
  /// SOS: how far the axis may drift from where it stopped.
  double standstill_window{0.0};
  /// At or below this, the axis counts as stopped. Used to end SS1 and to
  /// validate entry into SOS.
  double standstill_speed{0.0};
  /// SS1-t: the drive must reach standstill within this long, or the stop is
  /// a fault rather than a completion.
  std::int64_t stop_time_limit_ns{0};
};

/// Everything the supervisor looks at in a cycle.
///
/// Polarity is spelled out in every field name. A supervisor whose inputs mix
/// "ok" and "fault" conventions acquires an inverted condition sooner or later,
/// and the failure mode of an inverted safety input is that the machine runs
/// when it should not.
// @satisfies REQ-SAF-037
struct SafetyInputs {
  /// Magnitude of measured speed. Sign is not used: a limit is a limit in
  /// either direction, and taking the magnitude at the boundary means a caller
  /// cannot accidentally pass a negative speed that compares below a limit.
  double speed_magnitude{0.0};
  double position{0.0};

  /// True means the emergency stop is *demanding a stop*.
  bool emergency_stop_asserted{true};
  /// True means the safety communication channel is healthy.
  bool communication_ok{false};
  /// True means the cyclic executor is meeting its deadlines.
  bool heartbeat_ok{false};
  /// True means power-on diagnostics passed.
  bool self_test_passed{false};

  SafetyFunction request{SafetyFunction::kSafeTorqueOff};
  /// Operator acknowledgement. Clears a latched fault; does not restart
  /// motion.
  bool acknowledge{false};

  std::int64_t timestamp_ns{0};
};

/// What the supervisor permits. Every field defaults to the restrictive value,
/// so a partially-populated or default-constructed instance denies motion
/// rather than allowing it.
struct SafetyOutputs {
  /// False means STO is asserted: remove torque-producing energy.
  bool torque_permitted{false};
  /// False means hold: torque may be applied to maintain position, but no
  /// commanded motion may be executed.
  bool motion_permitted{false};
  /// Effective speed ceiling. Infinity when unrestricted.
  double speed_limit{0.0};
  /// True when a mechanical brake should be engaged.
  bool brake_engaged{true};

  SafetyState state{SafetyState::kSafeTorqueOff};
  FaultReason fault{FaultReason::kNone};
  /// True while a fault is latched and awaiting acknowledgement.
  bool fault_latched{false};
};

/// The IEC 61800-5-2 supervisor.
///
/// Structure
/// ---------
/// `evaluate()` is the only mutator, and the machine holds no state beyond what
/// is declared below. Given the same starting state and the same inputs, it
/// produces the same result every time -- which is what makes the exhaustive
/// transition table in the tests meaningful rather than indicative.
///
/// Evaluation order inside a cycle is fixed and matters:
///   1. Unconditional demands (E-stop, communication, watchdog). These override
///      every state, including a request for unrestricted motion.
///   2. Fault latch handling and acknowledgement.
///   3. Per-state monitors (SLS speed, SOS deviation, SS1 time, SLP band).
///   4. Requested transitions.
/// A monitor must be able to fault a state before a request can leave it,
/// otherwise a host that keeps requesting the current function could ride
/// through a limit violation indefinitely.
///
/// Allocation-free, exception-free and branch-bounded, so it runs inside the
/// cyclic executor's NoAllocScope.
// @satisfies REQ-SAF-020
// @satisfies REQ-SAF-038
class SafetyStateMachine {
 public:
  SafetyStateMachine() = default;
  explicit SafetyStateMachine(SafetyLimits limits) noexcept : limits_(limits) {}

  /// Advances one cycle and returns what is now permitted.
  [[nodiscard]] SafetyOutputs evaluate(const SafetyInputs& inputs) noexcept;

  [[nodiscard]] SafetyState state() const noexcept { return state_; }
  [[nodiscard]] FaultReason fault() const noexcept { return fault_; }
  [[nodiscard]] bool faultLatched() const noexcept {
    return fault_ != FaultReason::kNone;
  }

  [[nodiscard]] const SafetyLimits& limits() const noexcept { return limits_; }
  void setLimits(SafetyLimits limits) noexcept { limits_ = limits; }

  /// Restores the power-on condition. For tests and for a deliberate restart;
  /// not a recovery path.
  void reset() noexcept;

  /// Forces a latched safe state from outside.
  ///
  /// Exists for the dual-channel supervisor: a channel cannot detect that its
  /// peer disagrees with it, because by construction it has no visibility of
  /// the peer. Cross-comparison happens one level up, and this is how that
  /// level drives both channels down.
  ///
  /// Not a general-purpose back door -- it is the only way to introduce a
  /// fault the channel could not have observed itself, and every other fault
  /// arises from the inputs.
  void forceFault(FaultReason reason) noexcept;

 private:
  [[nodiscard]] static bool unconditionalDemandActive(
      const SafetyInputs& inputs) noexcept;
  [[nodiscard]] static FaultReason unconditionalDemandReason(
      const SafetyInputs& inputs) noexcept;
  [[nodiscard]] SafetyOutputs outputsFor(SafetyState state) const noexcept;

  void enterSafeTorqueOff(FaultReason reason) noexcept;

  SafetyLimits limits_{};
  SafetyState state_{SafetyState::kSelfTest};
  FaultReason fault_{FaultReason::kNone};

  /// Captured on entry to SS1, to run the stop-time monitor against.
  std::int64_t stop_started_ns_{0};
  /// Captured on entry to SOS, as the position the axis must hold.
  double standstill_reference_{0.0};
};

/// Human-readable names. Present so that a test failure or a log line names the
/// state rather than printing an integer, which matters more than usual here:
/// these are the values someone will be reading at 3am.
[[nodiscard]] const char* toString(SafetyState state) noexcept;
[[nodiscard]] const char* toString(FaultReason reason) noexcept;
[[nodiscard]] const char* toString(SafetyFunction function) noexcept;

}  // namespace safeedge::safety

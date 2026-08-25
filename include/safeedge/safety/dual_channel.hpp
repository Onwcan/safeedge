// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "safeedge/safety/safety_state_machine.hpp"

namespace safeedge::safety {

/// 1oo2D: one-out-of-two with diagnostics.
///
/// What the name means
/// -------------------
/// **1oo2** — either channel alone is sufficient to *demand* the safe reaction.
/// Torque is permitted only if both channels permit it, so a single channel
/// failing towards "stop" stops the machine, and a single channel failing
/// towards "run" is overruled by its peer. The safe direction wins by
/// construction rather than by arbitration.
///
/// **D** — the diagnostics: the two channels are continuously cross-compared,
/// so a channel that has failed in a way neither channel can see on its own
/// still becomes visible as a disagreement. Without the D, a channel stuck
/// permitting motion would be silently carried by its peer until the day the
/// peer also needed to permit motion, which is precisely when nobody is
/// watching.
///
/// Why disagreement is tolerated briefly
/// -------------------------------------
/// The two channels read independent sensors. Independent sensors have
/// independent noise and independent sampling instants, so at a threshold
/// boundary they will disagree for a cycle or two entirely legitimately -- one
/// reads 9.99 and the other 10.01 against a limit of 10. Faulting on the first
/// disagreement would make the machine unusable; never faulting would make the
/// diagnostics decorative. The tolerance window is where that judgement lives,
/// and it is a configured parameter rather than a constant because the right
/// value depends on the sensors.
///
/// The limitation that matters most
/// --------------------------------
/// **Both channels here run the same code.** Real 1oo2D uses diverse
/// implementations -- different algorithms, often different processors,
/// sometimes different teams -- because two identical implementations share
/// identical systematic faults. A logic error in `SafetyStateMachine` produces
/// the same wrong answer in both channels, they agree perfectly, and the
/// cross-comparison reports everything is fine.
///
/// So what this defends against is **random hardware faults and divergent
/// sensor input**, which is a real and useful class. It does *not* defend
/// against a software defect, and no amount of running the same function twice
/// ever will. Stating that plainly is more useful than implying a redundancy
/// that is not there.
class DualChannelSupervisor {
 public:
  struct Config {
    SafetyLimits limits{};
    /// How long the channels may disagree before it is treated as a fault.
    /// Zero means "fault on the first disagreeing cycle", which is only
    /// appropriate when both channels are driven from the same sampled input.
    std::int64_t discrepancy_tolerance_ns{0};
  };

  struct Diagnostics {
    /// Cycles in which the two channels reported different states.
    std::uint64_t disagreeing_cycles{0};
    /// Times a disagreement outlasted the tolerance and became a fault.
    std::uint64_t discrepancy_faults{0};
    /// Longest disagreement observed, whether or not it faulted. Useful for
    /// choosing the tolerance: if this sits close to the configured window in
    /// normal operation, the window is too tight.
    std::int64_t longest_disagreement_ns{0};
  };

  explicit DualChannelSupervisor(Config config) noexcept
      : config_(config), channel_a_(config.limits), channel_b_(config.limits) {}

  /// Evaluates both channels against their own independently-sensed inputs and
  /// returns the combined decision.
  ///
  /// The two input structs are deliberately separate rather than one shared
  /// struct: passing the same inputs to both channels would make the
  /// cross-comparison test nothing but determinism, which is already covered
  /// elsewhere. Independent inputs are the whole point.
  [[nodiscard]] SafetyOutputs evaluate(const SafetyInputs& channel_a_inputs,
                                       const SafetyInputs& channel_b_inputs,
                                       std::int64_t now_ns) noexcept;

  [[nodiscard]] bool disagreeing() const noexcept { return disagreement_since_ns_ >= 0; }
  [[nodiscard]] bool discrepancyLatched() const noexcept { return discrepancy_latched_; }
  [[nodiscard]] const Diagnostics& diagnostics() const noexcept { return diagnostics_; }

  [[nodiscard]] const SafetyStateMachine& channelA() const noexcept { return channel_a_; }
  [[nodiscard]] const SafetyStateMachine& channelB() const noexcept { return channel_b_; }

  void reset() noexcept;

  /// Combines two channel results, taking the safe direction on every field.
  /// Exposed as a free-standing operation so it can be tested directly rather
  /// than only through a full evaluation.
  [[nodiscard]] static SafetyOutputs combine(const SafetyOutputs& a,
                                             const SafetyOutputs& b) noexcept;

 private:
  Config config_;
  SafetyStateMachine channel_a_;
  SafetyStateMachine channel_b_;

  /// Timestamp the current disagreement began, or -1 when the channels agree.
  std::int64_t disagreement_since_ns_{-1};
  bool discrepancy_latched_{false};
  Diagnostics diagnostics_{};
};

}  // namespace safeedge::safety

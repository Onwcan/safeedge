// SPDX-License-Identifier: Apache-2.0
#include "safeedge/safety/dual_channel.hpp"

#include <algorithm>

namespace safeedge::safety {

SafetyOutputs DualChannelSupervisor::combine(const SafetyOutputs& a,
                                             const SafetyOutputs& b) noexcept {
  SafetyOutputs combined;

  // Every field takes the safe direction. This is the 1oo2 property: one
  // channel is sufficient to demand the safe reaction, and no channel can
  // grant a permission its peer withholds.
  combined.torque_permitted = a.torque_permitted && b.torque_permitted;
  combined.motion_permitted = a.motion_permitted && b.motion_permitted;
  combined.speed_limit = std::min(a.speed_limit, b.speed_limit);
  combined.brake_engaged = a.brake_engaged || b.brake_engaged;

  // SafetyState is ordered from most to least restrictive, so the safer of the
  // two is simply the smaller. That ordering is asserted in the tests
  // precisely because this line silently depends on it.
  combined.state = std::min(a.state, b.state);

  combined.fault_latched = a.fault_latched || b.fault_latched;
  // Report channel A's fault when it has one, otherwise B's. Arbitrary between
  // two simultaneous faults, but deterministic -- and a diagnostic that varies
  // run to run is worse than one that is consistently one-sided.
  combined.fault = a.fault != FaultReason::kNone ? a.fault : b.fault;
  return combined;
}

void DualChannelSupervisor::reset() noexcept {
  channel_a_.reset();
  channel_b_.reset();
  disagreement_since_ns_ = -1;
  discrepancy_latched_ = false;
  diagnostics_ = Diagnostics{};
}

SafetyOutputs DualChannelSupervisor::evaluate(const SafetyInputs& channel_a_inputs,
                                              const SafetyInputs& channel_b_inputs,
                                              std::int64_t now_ns) noexcept {
  const SafetyOutputs a = channel_a_.evaluate(channel_a_inputs);
  const SafetyOutputs b = channel_b_.evaluate(channel_b_inputs);

  // --- cross-comparison ----------------------------------------------------
  if (a.state != b.state) {
    ++diagnostics_.disagreeing_cycles;
    if (disagreement_since_ns_ < 0) {
      disagreement_since_ns_ = now_ns;
    }
    const std::int64_t duration = now_ns - disagreement_since_ns_;
    diagnostics_.longest_disagreement_ns =
        std::max(diagnostics_.longest_disagreement_ns, duration);

    if (duration > config_.discrepancy_tolerance_ns && !discrepancy_latched_) {
      // The channels have disagreed for longer than independent sensor noise
      // can explain. Neither channel can detect this on its own -- by
      // construction each is blind to the other -- so it is forced on both
      // from here.
      discrepancy_latched_ = true;
      ++diagnostics_.discrepancy_faults;
      channel_a_.forceFault(FaultReason::kChannelDiscrepancy);
      channel_b_.forceFault(FaultReason::kChannelDiscrepancy);
    }
  } else {
    // Agreement clears the timer but never the latch. A discrepancy that
    // resolves itself is exactly the shape of an intermittent fault, and
    // letting it clear silently would mean the machine kept running on a
    // channel pair that had already been caught disagreeing once.
    disagreement_since_ns_ = -1;
  }

  if (discrepancy_latched_) {
    // Clearing requires both channels to have been acknowledged, which they
    // handle themselves through their own inputs. Only once neither reports a
    // fault does the supervisor release its own latch.
    if (!channel_a_.faultLatched() && !channel_b_.faultLatched()) {
      discrepancy_latched_ = false;
    } else {
      SafetyOutputs safe;  // defaults are fail-safe: nothing permitted
      safe.state = SafetyState::kSafeTorqueOff;
      safe.fault = FaultReason::kChannelDiscrepancy;
      safe.fault_latched = true;
      return safe;
    }
  }

  return combine(a, b);
}

}  // namespace safeedge::safety

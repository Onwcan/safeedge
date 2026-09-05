// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "safeedge/edge/runtime_snapshot.hpp"

struct UA_Server;

namespace safeedge::opcua {

/// Why an event exists here at all, when the same facts are already variables.
///
/// A client subscribed to a variable does not receive changes; it receives
/// *samples*. The server reads the node every `samplingInterval` and reports the
/// value if it differs from the last one it read. A state that begins and ends
/// between two samples never existed as far as that client is concerned -- not
/// delayed, not merged, absent. For a diagnostic gauge that is a fair trade. For
/// "the machine tripped and recovered", it is the difference between a log and
/// a fiction.
///
/// An event is queued when it is triggered, so a client that asked for a queue
/// deep enough receives every one. That is a correctness difference and not a
/// latency optimisation, which is the reverse of how the choice is usually
/// described.
///
/// It is not magic, and the limits are worth stating precisely:
///
///   * An event queue can overflow too. It is lossless up to the queue size the
///     client requested; a sampled variable is lossy by construction whatever
///     queue it asks for.
///   * This server reads its own snapshot on a poll. A transition that begins
///     and ends between two of those reads is invisible before OPC UA is
///     involved at all -- the chain is only as event-driven as its most sampled
///     link.
///   * That last case is detectable, because the snapshot carries a transition
///     counter. When it advances by more than one between reads, the server
///     knows how many transitions it coalesced even though it cannot recover
///     them, and says so in MissedTransitions rather than presenting the newest
///     state as though it were the only one.
struct SafetyTransitionEvent {
  /// Monotonic instant of the transition, nanoseconds, on the runtime's clock.
  std::int64_t transition_monotonic_ns{0};
  /// The runtime's transition counter after this transition. Matches the
  /// snapshot's own width, so nothing is narrowed on the way out.
  std::uint64_t safety_sequence{0};
  /// Transitions the server could not observe individually because they
  /// happened between two reads of the snapshot. Zero means this event is the
  /// complete story since the previous one.
  std::uint64_t missed_transitions{0};
  std::uint32_t safety_state{0};
  bool torque_permitted{false};
};

/// Creates the SafetyTransitionEventType and its fields.
///
/// Called after buildAddressSpace, because the fields live in the same
/// namespace and the index has to exist first. Returns false if any node could
/// not be added -- an event type missing a field would produce events whose
/// contents silently differ from the ones documented.
bool buildEventType(UA_Server* server);

/// Triggers one event from the Server object.
///
/// Returns false if the event could not be created or fired. Callers log it and
/// carry on: a missed event is bad, and stopping the server because of one is
/// worse.
bool fireSafetyTransition(UA_Server* server, const SafetyTransitionEvent& event);

/// Builds the event payload from two consecutive snapshots.
///
/// `previous_sequence` is the counter from the last snapshot this server acted
/// on, which is what makes `missed_transitions` computable.
SafetyTransitionEvent describeTransition(const edge::RuntimeSnapshot& snapshot,
                                         std::uint64_t previous_sequence) noexcept;

/// Browse name of the event type, for tests and documentation.
inline constexpr const char* kSafetyEventTypeName = "SafetyTransitionEventType";

}  // namespace safeedge::opcua

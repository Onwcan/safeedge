// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "safeedge/edge/runtime_snapshot.hpp"

// Forward-declared so this header does not drag open62541 into every consumer.
// The C API is large, and a component that only needs to know "there is a
// server" should not have to compile a protocol stack to say so.
struct UA_Server;

namespace safeedge::opcua {

/// Numeric identifiers for the nodes this server exposes.
///
/// **These are a contract in exactly the way protobuf field numbers are.** A
/// client stores a NodeId and uses it forever; renaming the browse name is
/// cosmetic and safe, renumbering is a silent break that leaves the client
/// reading a different variable or nothing at all. The rules are the same ones
/// robot-contracts documents for field numbers:
///
///   * never change a number
///   * never reuse one, even after removing the node
///   * add new nodes at new numbers
///
/// Numeric rather than string identifiers because they are what OPC UA clients
/// cache and what appear in stored subscriptions; string NodeIds are convenient
/// to read and encourage renaming, which is the thing that must not happen.
enum class NodeNumber : std::uint32_t {
  kRuntimeObject = 1000,

  kSafetyState = 1001,
  kTorquePermitted = 1002,
  kFaultLatched = 1003,
  kFaultReason = 1004,
  kEstopAsserted = 1005,
  kSafetySequence = 1006,
  kSafetyStateAgeSeconds = 1007,

  /// The monotonic instant of the last safety transition, in nanoseconds.
  ///
  /// Exposed here and deliberately NOT as a Prometheus gauge. That exposition
  /// format renders a double to six significant digits, and a CLOCK_MONOTONIC
  /// nanosecond value near 1e18 does not survive it. OPC UA has a signed 64-bit
  /// integer type, so the number arrives intact -- which is what lets a client
  /// subscribe to this node and compute its own notification latency.
  kSafetyTransitionMonotonicNs = 1008,

  kCyclesExecuted = 1010,
  kOverruns = 1011,
  kJitterP99Ns = 1012,
  kJitterMaxNs = 1013,
  kRealtimeSchedulingGranted = 1014,
  kUptimeSeconds = 1015,

  // --- Event type and its fields ------------------------------------------
  // A separate block, one authority. Two enums numbering nodes in the same
  // namespace is how a collision gets introduced by someone who only read one
  // of them.
  kSafetyTransitionEventType = 1100,
  kEventTransitionMonotonicNs = 1101,
  kEventSafetySequence = 1102,
  kEventTorquePermitted = 1103,
  kEventSafetyState = 1104,
  kEventMissedTransitions = 1105,

  // Next free: 1009 in the safety block, 1016 in the variables block, 1106 in
  // the event block. Nothing above is ever reused.
};

/// Creates the SafeEdgeRuntime object and its variables.
///
/// Returns false if any node could not be added, which is treated as fatal by
/// the daemon: a half-built address space would let a client browse to a
/// variable that silently never updates.
bool buildAddressSpace(UA_Server* server);

/// Copies a snapshot into the address space.
///
/// `fresh` is false when the snapshot could not be read. Every variable is then
/// marked `UA_STATUSCODE_BADNODATA` rather than left holding its previous value.
///
/// That distinction is the whole reason this function takes the flag. OPC UA
/// gives every value a StatusCode precisely so a server can say "I do not know"
/// instead of guessing, and a server that serves the last good value with a Good
/// status is lying in the one direction that matters: a client cannot tell a
/// machine that is safe from a runtime that stopped answering. It is the same
/// argument as UNSPECIFIED-at-zero in the protobuf contracts and the heartbeat
/// on the shared-memory link, in the vocabulary this protocol already has.
void publishSnapshot(UA_Server* server, const edge::RuntimeSnapshot& snapshot,
                     bool fresh);

/// Browse name of the runtime object, for tests and documentation.
inline constexpr const char* kRuntimeObjectName = "SafeEdgeRuntime";

/// Namespace URI this server registers its nodes under.
///
/// This, not a namespace index, is what a client must key on. The index is
/// assigned by the server at run time and moves if the namespace array changes;
/// the URI does not. Resolve it once per session and use the result.
inline constexpr const char* kNamespaceUri = "urn:safeedge:runtime";

/// The namespace index the running server assigned to kNamespaceUri, or 0 if
/// the address space has not been built. Exposed for tests and diagnostics.
std::uint16_t namespaceIndex() noexcept;

}  // namespace safeedge::opcua

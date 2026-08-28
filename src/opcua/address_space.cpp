// SPDX-License-Identifier: Apache-2.0
#include "safeedge/opcua/address_space.hpp"

#include <open62541/server.h>

#include <array>

namespace safeedge::opcua {
namespace {

/// The namespace index this server assigned to kNamespaceUri.
///
/// **Assigned at run time, and deliberately not a constant.** A namespace index
/// is a per-server, per-session lookup into that server's namespace array. It
/// is not stable: index 0 is always the OPC UA base namespace and index 1 is
/// conventionally the server's own application URI, so a custom namespace
/// usually lands at 2 -- but "usually" is the whole problem. Add another
/// namespace, or connect to a different server, and it moves.
///
/// The first version of this file hardcoded 1 and failed to build its address
/// space at all, which is the good outcome; the bad one is hardcoding the index
/// that happens to be right today and having a client read the wrong variable
/// after somebody adds a namespace.
///
/// **What is stable is the URI.** A client resolves `urn:safeedge:runtime` to an
/// index at connect time, then uses that index for the session. That is the
/// protocol's own answer and it is the same shape as every other contract in
/// this portfolio: the name is the promise, the number is an implementation
/// detail that must be looked up.
///
/// One server per process here, so a file-scope value is adequate; a process
/// hosting several would need this per-server.
UA_UInt16 g_namespace_index = 0;

UA_NodeId nodeIdOf(NodeNumber number) {
  return UA_NODEID_NUMERIC(g_namespace_index, static_cast<UA_UInt32>(number));
}

/// One variable in the address space, described once so adding and updating
/// cannot disagree about type.
struct VariableSpec {
  NodeNumber number;
  const char* name;
  const UA_DataType* type;
};

const std::array<VariableSpec, 13>& specs() {
  static const std::array<VariableSpec, 13> kSpecs{{
      {NodeNumber::kSafetyState, "SafetyState", &UA_TYPES[UA_TYPES_UINT32]},
      {NodeNumber::kTorquePermitted, "TorquePermitted", &UA_TYPES[UA_TYPES_BOOLEAN]},
      {NodeNumber::kFaultLatched, "FaultLatched", &UA_TYPES[UA_TYPES_BOOLEAN]},
      {NodeNumber::kFaultReason, "FaultReason", &UA_TYPES[UA_TYPES_UINT32]},
      {NodeNumber::kEstopAsserted, "EmergencyStopAsserted", &UA_TYPES[UA_TYPES_BOOLEAN]},
      {NodeNumber::kSafetySequence, "SafetySequence", &UA_TYPES[UA_TYPES_UINT64]},
      {NodeNumber::kSafetyStateAgeSeconds, "SafetyStateAgeSeconds",
       &UA_TYPES[UA_TYPES_DOUBLE]},
      {NodeNumber::kCyclesExecuted, "CyclesExecuted", &UA_TYPES[UA_TYPES_UINT64]},
      {NodeNumber::kOverruns, "Overruns", &UA_TYPES[UA_TYPES_UINT64]},
      {NodeNumber::kJitterP99Ns, "WakeupJitterP99Nanoseconds", &UA_TYPES[UA_TYPES_INT64]},
      {NodeNumber::kJitterMaxNs, "WakeupJitterMaxNanoseconds", &UA_TYPES[UA_TYPES_INT64]},
      {NodeNumber::kRealtimeSchedulingGranted, "RealtimeSchedulingGranted",
       &UA_TYPES[UA_TYPES_BOOLEAN]},
      {NodeNumber::kUptimeSeconds, "UptimeSeconds", &UA_TYPES[UA_TYPES_DOUBLE]},
  }};
  return kSpecs;
}

/// Writes one variable, or marks it as having no data.
template <typename T>
void writeValue(UA_Server* server, NodeNumber number, const UA_DataType* type, T value,
                bool fresh) {
  UA_NodeId node = nodeIdOf(number);

  // The value is always written, even when there is nothing to report.
  //
  // The first version set hasValue = false and relied on the status alone. That
  // does not do what it looks like: open62541 leaves the node's previous value
  // in place, so a client reading the value attribute still gets the last good
  // one -- a dead runtime's "torque permitted", indefinitely. The status said
  // BadNoData the whole time, and a client that checked it would have been fine,
  // which is precisely the kind of safety that depends on everyone being
  // careful.
  //
  // So both channels carry the same message. The status says "I do not know",
  // and the value carries the caller's not-fresh snapshot -- a default-
  // constructed one, whose booleans are false and whose counters are zero. A
  // client that ignores the status still reads "torque not permitted", which is
  // the direction it has to fail in.
  UA_DataValue data;
  UA_DataValue_init(&data);
  UA_Variant_setScalar(&data.value, &value, type);
  data.hasValue = true;
  data.status = fresh ? UA_STATUSCODE_GOOD : UA_STATUSCODE_BADNODATA;
  data.hasStatus = true;
  data.hasSourceTimestamp = true;
  data.sourceTimestamp = UA_DateTime_now();

  (void)UA_Server_writeDataValue(server, node, data);
  // `data.value` borrows `value`, which lives on this stack frame, so it must
  // not be cleared through UA_DataValue_clear -- that would try to free it.
}

bool addVariable(UA_Server* server, const VariableSpec& spec) {
  UA_VariableAttributes attributes = UA_VariableAttributes_default;
  attributes.displayName =
      UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(spec.name));
  attributes.dataType = spec.type->typeId;
  attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
  attributes.valueRank = UA_VALUERANK_SCALAR;

  const UA_StatusCode status = UA_Server_addVariableNode(
      server, nodeIdOf(spec.number), nodeIdOf(NodeNumber::kRuntimeObject),
      UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
      UA_QUALIFIEDNAME(g_namespace_index, const_cast<char*>(spec.name)),
      UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attributes, nullptr, nullptr);
  return status == UA_STATUSCODE_GOOD;
}

}  // namespace

std::uint16_t namespaceIndex() noexcept { return g_namespace_index; }

bool buildAddressSpace(UA_Server* server) {
  if (server == nullptr) {
    return false;
  }

  g_namespace_index = UA_Server_addNamespace(server, kNamespaceUri);
  if (g_namespace_index == 0) {
    // 0 is the OPC UA base namespace and can never be ours, so this is the
    // server refusing rather than a plausible answer.
    return false;
  }

  UA_ObjectAttributes object_attributes = UA_ObjectAttributes_default;
  object_attributes.displayName =
      UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(kRuntimeObjectName));

  UA_StatusCode status = UA_Server_addObjectNode(
      server, nodeIdOf(NodeNumber::kRuntimeObject),
      UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
      UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
      UA_QUALIFIEDNAME(g_namespace_index, const_cast<char*>(kRuntimeObjectName)),
      UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), object_attributes, nullptr, nullptr);
  if (status != UA_STATUSCODE_GOOD) {
    return false;
  }

  for (const VariableSpec& spec : specs()) {
    if (!addVariable(server, spec)) {
      // A half-built address space is worse than none: a client browses
      // successfully to a variable that then never updates.
      return false;
    }
  }
  return true;
}

void publishSnapshot(UA_Server* server, const edge::RuntimeSnapshot& snapshot,
                     bool fresh) {
  if (server == nullptr) {
    return;
  }

  writeValue<UA_UInt32>(server, NodeNumber::kSafetyState, &UA_TYPES[UA_TYPES_UINT32],
                        snapshot.safety_state, fresh);
  writeValue<UA_Boolean>(server, NodeNumber::kTorquePermitted,
                         &UA_TYPES[UA_TYPES_BOOLEAN], snapshot.torque_permitted != 0,
                         fresh);
  writeValue<UA_Boolean>(server, NodeNumber::kFaultLatched, &UA_TYPES[UA_TYPES_BOOLEAN],
                         snapshot.fault_latched != 0, fresh);
  writeValue<UA_UInt32>(server, NodeNumber::kFaultReason, &UA_TYPES[UA_TYPES_UINT32],
                        snapshot.fault_reason, fresh);
  writeValue<UA_Boolean>(server, NodeNumber::kEstopAsserted, &UA_TYPES[UA_TYPES_BOOLEAN],
                         snapshot.estop_asserted != 0, fresh);
  writeValue<UA_UInt64>(server, NodeNumber::kSafetySequence, &UA_TYPES[UA_TYPES_UINT64],
                        snapshot.safety_sequence, fresh);
  writeValue<UA_Double>(server, NodeNumber::kSafetyStateAgeSeconds,
                        &UA_TYPES[UA_TYPES_DOUBLE],
                        static_cast<double>(snapshot.safety_state_age_ns) / 1e9, fresh);

  writeValue<UA_UInt64>(server, NodeNumber::kCyclesExecuted, &UA_TYPES[UA_TYPES_UINT64],
                        snapshot.cycles_executed, fresh);
  writeValue<UA_UInt64>(server, NodeNumber::kOverruns, &UA_TYPES[UA_TYPES_UINT64],
                        snapshot.overruns, fresh);

  // Nanoseconds as Int64, not Double. OPC UA has a 64-bit integer type and
  // using it costs nothing; going through a double would repeat the mistake the
  // Prometheus exposition forced -- six significant digits and a timestamp that
  // is no longer a timestamp.
  writeValue<UA_Int64>(server, NodeNumber::kJitterP99Ns, &UA_TYPES[UA_TYPES_INT64],
                       snapshot.jitter_p99_ns, fresh);
  writeValue<UA_Int64>(server, NodeNumber::kJitterMaxNs, &UA_TYPES[UA_TYPES_INT64],
                       snapshot.jitter_max_ns, fresh);

  writeValue<UA_Boolean>(server, NodeNumber::kRealtimeSchedulingGranted,
                         &UA_TYPES[UA_TYPES_BOOLEAN],
                         snapshot.realtime_scheduling_granted != 0, fresh);
  writeValue<UA_Double>(server, NodeNumber::kUptimeSeconds, &UA_TYPES[UA_TYPES_DOUBLE],
                        static_cast<double>(snapshot.uptime_ns) / 1e9, fresh);
}

}  // namespace safeedge::opcua

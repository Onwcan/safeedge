// SPDX-License-Identifier: Apache-2.0
#include "safeedge/opcua/events.hpp"

#include <open62541/server.h>

#include <array>
#include <cstdint>
#include <string>

#include "safeedge/opcua/address_space.hpp"

namespace safeedge::opcua {
namespace {

UA_NodeId nodeIdFor(NodeNumber number) {
  return UA_NODEID_NUMERIC(namespaceIndex(), static_cast<UA_UInt32>(number));
}

/// One field of the event type.
struct FieldSpec {
  NodeNumber number;
  const char* name;
  const UA_DataType* type;
};

/// Adds one property to the event type, with the Mandatory modelling rule.
///
/// The modelling rule is the part that is easy to leave out and hard to notice
/// missing: without it the field exists on the *type* and is not instantiated on
/// the events, so a client browsing the type sees the field it expects and every
/// event it receives has nothing in it.
bool addField(UA_Server* server, const FieldSpec& spec) {
  UA_VariableAttributes attributes = UA_VariableAttributes_default;
  attributes.displayName =
      UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(spec.name));
  attributes.dataType = spec.type->typeId;
  attributes.valueRank = UA_VALUERANK_SCALAR;

  const UA_NodeId field = nodeIdFor(spec.number);
  if (UA_Server_addVariableNode(
          server, field, nodeIdFor(NodeNumber::kSafetyTransitionEventType),
          UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY),
          UA_QUALIFIEDNAME(namespaceIndex(), const_cast<char*>(spec.name)),
          UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), attributes, nullptr,
          nullptr) != UA_STATUSCODE_GOOD) {
    return false;
  }
  return UA_Server_addReference(
             server, field, UA_NODEID_NUMERIC(0, UA_NS0ID_HASMODELLINGRULE),
             UA_EXPANDEDNODEID_NUMERIC(0, UA_NS0ID_MODELLINGRULE_MANDATORY),
             true) == UA_STATUSCODE_GOOD;
}

}  // namespace

bool buildEventType(UA_Server* server) {
  if (server == nullptr || namespaceIndex() == 0) {
    // Namespace index zero means buildAddressSpace has not run. The fields
    // would land in namespace 0, which belongs to the OPC Foundation.
    return false;
  }

  UA_ObjectTypeAttributes type_attributes = UA_ObjectTypeAttributes_default;
  type_attributes.displayName = UA_LOCALIZEDTEXT(const_cast<char*>("en-US"),
                                                 const_cast<char*>(kSafetyEventTypeName));
  if (UA_Server_addObjectTypeNode(
          server, nodeIdFor(NodeNumber::kSafetyTransitionEventType),
          UA_NODEID_NUMERIC(0, UA_NS0ID_BASEEVENTTYPE),
          UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
          UA_QUALIFIEDNAME(namespaceIndex(), const_cast<char*>(kSafetyEventTypeName)),
          type_attributes, nullptr, nullptr) != UA_STATUSCODE_GOOD) {
    return false;
  }

  const std::array<FieldSpec, 5> fields{{
      {NodeNumber::kEventTransitionMonotonicNs, "TransitionMonotonicNanoseconds",
       &UA_TYPES[UA_TYPES_INT64]},
      {NodeNumber::kEventSafetySequence, "SafetySequence", &UA_TYPES[UA_TYPES_UINT64]},
      {NodeNumber::kEventMissedTransitions, "MissedTransitions",
       &UA_TYPES[UA_TYPES_UINT64]},
      {NodeNumber::kEventSafetyState, "SafetyState", &UA_TYPES[UA_TYPES_UINT32]},
      {NodeNumber::kEventTorquePermitted, "TorquePermitted", &UA_TYPES[UA_TYPES_BOOLEAN]},
  }};
  for (const FieldSpec& spec : fields) {
    if (!addField(server, spec)) {
      return false;
    }
  }
  return true;
}

SafetyTransitionEvent describeTransition(const edge::RuntimeSnapshot& snapshot,
                                         std::uint64_t previous_sequence) noexcept {
  SafetyTransitionEvent event;
  event.transition_monotonic_ns =
      static_cast<std::int64_t>(snapshot.safety_transition_monotonic_ns);
  event.safety_sequence = snapshot.safety_sequence;
  event.safety_state = snapshot.safety_state;
  event.torque_permitted = snapshot.torque_permitted != 0;
  // Unsigned subtraction, so a counter that wraps still yields the right gap.
  // One transition since the last observation is the normal case and means
  // nothing was missed.
  const std::uint64_t advanced = snapshot.safety_sequence - previous_sequence;
  event.missed_transitions = advanced > 1 ? advanced - 1 : 0;
  return event;
}

bool fireSafetyTransition(UA_Server* server, const SafetyTransitionEvent& event) {
  if (server == nullptr || namespaceIndex() == 0) {
    return false;
  }

  UA_NodeId instance = UA_NODEID_NULL;
  if (UA_Server_createEvent(server, nodeIdFor(NodeNumber::kSafetyTransitionEventType),
                            &instance) != UA_STATUSCODE_GOOD) {
    return false;
  }

  // The namespace is part of the field name, and the two kinds of field on this
  // event live in different ones: the fields declared above are in this
  // server's namespace, while Severity, Message and the rest are inherited from
  // BaseEventType and belong to namespace 0. Writing an inherited field under
  // this server's index names a property that does not exist, and the write
  // fails -- which is how this was found.
  const auto writeField = [&](UA_UInt16 name_space, const char* name, const void* value,
                              const UA_DataType* type) {
    return UA_Server_writeObjectProperty_scalar(
               server, instance, UA_QUALIFIEDNAME(name_space, const_cast<char*>(name)),
               value, type) == UA_STATUSCODE_GOOD;
  };
  const UA_UInt16 own = namespaceIndex();

  const UA_Boolean permitted = event.torque_permitted ? UA_TRUE : UA_FALSE;
  bool ok =
      writeField(own, "TransitionMonotonicNanoseconds", &event.transition_monotonic_ns,
                 &UA_TYPES[UA_TYPES_INT64]) &&
      writeField(own, "SafetySequence", &event.safety_sequence,
                 &UA_TYPES[UA_TYPES_UINT64]) &&
      writeField(own, "MissedTransitions", &event.missed_transitions,
                 &UA_TYPES[UA_TYPES_UINT64]) &&
      writeField(own, "SafetyState", &event.safety_state, &UA_TYPES[UA_TYPES_UINT32]) &&
      writeField(own, "TorquePermitted", &permitted, &UA_TYPES[UA_TYPES_BOOLEAN]);

  // Severity is a standard BaseEventType field, and it is the one a generic
  // client filters on without knowing anything about this server. Losing torque
  // permission is the condition an operator is woken for; regaining it is not.
  // 500 and 100 are the middle and low bands of the 1..1000 range OPC UA
  // defines, chosen so an unmodified client ranks these sensibly against events
  // from equipment nobody here wrote.
  const UA_UInt16 severity = event.torque_permitted ? 100 : 500;
  const std::string message =
      std::string("safety transition: torque ") +
      (event.torque_permitted ? "permitted" : "not permitted") + ", sequence " +
      std::to_string(event.safety_sequence) +
      (event.missed_transitions > 0
           ? ", " + std::to_string(event.missed_transitions) + " transition(s) coalesced"
           : "");
  UA_LocalizedText text =
      UA_LOCALIZEDTEXT(const_cast<char*>("en-US"), const_cast<char*>(message.c_str()));
  ok = writeField(0, "Severity", &severity, &UA_TYPES[UA_TYPES_UINT16]) && ok;
  ok = writeField(0, "Message", &text, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]) && ok;

  // Fired from the Server object, which carries the EventNotifier attribute by
  // default and is where a client that knows nothing about this address space
  // will subscribe. The event node is deleted afterwards -- keeping it would
  // grow the address space by one node per transition, forever.
  const UA_StatusCode triggered =
      UA_Server_triggerEvent(server, instance, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER),
                             nullptr, /*deleteEventNode=*/UA_TRUE);
  return ok && triggered == UA_STATUSCODE_GOOD;
}

}  // namespace safeedge::opcua

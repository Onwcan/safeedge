// SPDX-License-Identifier: Apache-2.0
//
// The address space, exercised through the server's own read path.
//
// No sockets and no client: UA_Server_readValue goes straight at the node store,
// which is where the mapping this component owns actually lives. A test that
// connected over TCP would mostly be testing open62541's networking, and would
// bring a port number and a race into CI for nothing.

#include <gtest/gtest.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include "safeedge/edge/runtime_snapshot.hpp"
#include "safeedge/opcua/address_space.hpp"

namespace safeedge::opcua {
namespace {

/// Owns a configured server for one test.
class ServerFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    server_ = UA_Server_new();
    ASSERT_NE(server_, nullptr);
    ASSERT_EQ(UA_ServerConfig_setMinimal(UA_Server_getConfig(server_), 0, nullptr),
              UA_STATUSCODE_GOOD);
    ASSERT_TRUE(buildAddressSpace(server_));
  }

  void TearDown() override {
    if (server_ != nullptr) {
      UA_Server_delete(server_);
    }
  }

  UA_NodeId node(NodeNumber number) const {
    return UA_NODEID_NUMERIC(namespaceIndex(), static_cast<UA_UInt32>(number));
  }

  /// Reads one variable as a DataValue, so the status is visible as well as the
  /// value. Reading only the value would hide the entire point of this test.
  UA_DataValue read(NodeNumber number) const {
    UA_ReadValueId item;
    UA_ReadValueId_init(&item);
    item.nodeId = node(number);
    item.attributeId = UA_ATTRIBUTEID_VALUE;
    return UA_Server_read(server_, &item, UA_TIMESTAMPSTORETURN_NEITHER);
  }

  UA_Server* server_ = nullptr;
};

TEST_F(ServerFixture, TheNamespaceIsResolvedNotAssumed) {
  // The index is whatever this server assigned. It must not be 0 -- that is the
  // OPC UA base namespace -- and the test deliberately does not assert a
  // specific number, because asserting one would enshrine exactly the
  // assumption that made the first version of this file fail.
  EXPECT_NE(namespaceIndex(), 0);
}

TEST_F(ServerFixture, ASnapshotIsReadableThroughTheAddressSpace) {
  edge::RuntimeSnapshot snapshot;
  snapshot.safety_state = 4;
  snapshot.torque_permitted = 1;
  snapshot.fault_latched = 0;
  snapshot.estop_asserted = 0;
  snapshot.safety_sequence = 7;
  snapshot.cycles_executed = 123456;
  snapshot.overruns = 2;
  snapshot.jitter_p99_ns = 41000;
  snapshot.realtime_scheduling_granted = 1;

  publishSnapshot(server_, snapshot, /*fresh=*/true);

  {
    UA_DataValue value = read(NodeNumber::kSafetyState);
    ASSERT_TRUE(value.hasValue);
    EXPECT_EQ(value.status, UA_STATUSCODE_GOOD);
    ASSERT_TRUE(UA_Variant_hasScalarType(&value.value, &UA_TYPES[UA_TYPES_UINT32]));
    EXPECT_EQ(*static_cast<UA_UInt32*>(value.value.data), 4u);
    UA_DataValue_clear(&value);
  }
  {
    UA_DataValue value = read(NodeNumber::kTorquePermitted);
    ASSERT_TRUE(value.hasValue);
    ASSERT_TRUE(UA_Variant_hasScalarType(&value.value, &UA_TYPES[UA_TYPES_BOOLEAN]));
    EXPECT_TRUE(*static_cast<UA_Boolean*>(value.value.data));
    UA_DataValue_clear(&value);
  }
  {
    // Nanoseconds as a 64-bit integer, not a double. The Prometheus exposition
    // forced a double and lost a timestamp to six significant digits; OPC UA has
    // Int64 and there is no reason to repeat that.
    UA_DataValue value = read(NodeNumber::kJitterP99Ns);
    ASSERT_TRUE(value.hasValue);
    ASSERT_TRUE(UA_Variant_hasScalarType(&value.value, &UA_TYPES[UA_TYPES_INT64]));
    EXPECT_EQ(*static_cast<UA_Int64*>(value.value.data), 41000);
    UA_DataValue_clear(&value);
  }
  {
    UA_DataValue value = read(NodeNumber::kCyclesExecuted);
    ASSERT_TRUE(value.hasValue);
    ASSERT_TRUE(UA_Variant_hasScalarType(&value.value, &UA_TYPES[UA_TYPES_UINT64]));
    EXPECT_EQ(*static_cast<UA_UInt64*>(value.value.data), 123456u);
    UA_DataValue_clear(&value);
  }
}

// The reason publishSnapshot takes a `fresh` flag at all.
TEST_F(ServerFixture, AnUnreadableSnapshotIsBadNoDataNotTheLastGoodValue) {
  edge::RuntimeSnapshot good;
  good.torque_permitted = 1;
  good.safety_state = 5;
  publishSnapshot(server_, good, /*fresh=*/true);

  {
    UA_DataValue value = read(NodeNumber::kTorquePermitted);
    ASSERT_TRUE(value.hasValue);
    EXPECT_TRUE(*static_cast<UA_Boolean*>(value.value.data)) << "precondition";
    UA_DataValue_clear(&value);
  }

  // The runtime stops answering. The server must say it does not know, rather
  // than keep serving the last value it happened to have -- a client cannot
  // distinguish a safe machine from a dead runtime otherwise, and it would
  // choose wrongly in the dangerous direction.
  publishSnapshot(server_, edge::RuntimeSnapshot{}, /*fresh=*/false);

  // Both channels must carry the message, and this test exists because relying
  // on one of them did not work. Setting hasValue = false does NOT clear the
  // node's value in open62541 -- the previous one stays, so a client reading
  // only the value attribute would still have seen "torque permitted" from a
  // runtime that had stopped answering.
  {
    UA_DataValue value = read(NodeNumber::kTorquePermitted);
    EXPECT_EQ(value.status, UA_STATUSCODE_BADNODATA) << "the status must say so";
    ASSERT_TRUE(value.hasValue);
    EXPECT_FALSE(*static_cast<UA_Boolean*>(value.value.data))
        << "and the value must fail safe for a client that ignores the status";
    UA_DataValue_clear(&value);
  }
  {
    UA_DataValue value = read(NodeNumber::kSafetyState);
    EXPECT_EQ(value.status, UA_STATUSCODE_BADNODATA);
    ASSERT_TRUE(value.hasValue);
    // 0 is kSafeTorqueOff in the supervisor's enumeration: the restrictive end,
    // not the permissive one. A zeroed snapshot degrades toward stopped.
    EXPECT_EQ(*static_cast<UA_UInt32*>(value.value.data), 0u);
    UA_DataValue_clear(&value);
  }
}

TEST_F(ServerFixture, EveryDeclaredNodeExists) {
  // A half-built address space lets a client browse to a variable that then
  // never updates, so buildAddressSpace refuses to half-succeed. This checks the
  // full set is actually there rather than trusting that it returned true.
  const NodeNumber all[] = {
      NodeNumber::kSafetyState,
      NodeNumber::kTorquePermitted,
      NodeNumber::kFaultLatched,
      NodeNumber::kFaultReason,
      NodeNumber::kEstopAsserted,
      NodeNumber::kSafetySequence,
      NodeNumber::kSafetyStateAgeSeconds,
      NodeNumber::kCyclesExecuted,
      NodeNumber::kOverruns,
      NodeNumber::kJitterP99Ns,
      NodeNumber::kJitterMaxNs,
      NodeNumber::kRealtimeSchedulingGranted,
      NodeNumber::kUptimeSeconds,
  };

  publishSnapshot(server_, edge::RuntimeSnapshot{}, /*fresh=*/true);
  for (const NodeNumber number : all) {
    UA_DataValue value = read(number);
    EXPECT_NE(value.status, UA_STATUSCODE_BADNODEIDUNKNOWN)
        << "node " << static_cast<std::uint32_t>(number) << " is missing";
    UA_DataValue_clear(&value);
  }
}

}  // namespace
}  // namespace safeedge::opcua

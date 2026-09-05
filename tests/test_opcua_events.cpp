// SPDX-License-Identifier: Apache-2.0
//
// The event type, and the arithmetic that decides what an event claims.
//
// A client subscribed to a variable receives samples, not changes: a state that
// begins and ends between two samples is absent rather than late. Events are
// queued when triggered, so they carry transitions a sampled variable cannot.
// These tests cover the part that can be checked in one process; the part that
// needs two is scripts/demonstrate-events-vs-sampling.sh.

#include <gtest/gtest.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include <cstdint>
#include <limits>

#include "safeedge/edge/runtime_snapshot.hpp"
#include "safeedge/opcua/address_space.hpp"
#include "safeedge/opcua/events.hpp"

namespace safeedge::opcua {
namespace {

edge::RuntimeSnapshot snapshotAt(std::uint64_t sequence, bool permitted) {
  edge::RuntimeSnapshot snapshot;
  snapshot.safety_sequence = sequence;
  snapshot.torque_permitted = permitted ? 1u : 0u;
  snapshot.safety_transition_monotonic_ns = 1'234'567'890'123ULL;
  snapshot.safety_state = permitted ? 2u : 4u;
  return snapshot;
}

class EventFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    server_ = UA_Server_new();
    ASSERT_NE(server_, nullptr);
    UA_ServerConfig_setMinimal(UA_Server_getConfig(server_), 0, nullptr);
    ASSERT_TRUE(buildAddressSpace(server_));
  }
  void TearDown() override {
    if (server_ != nullptr) {
      UA_Server_delete(server_);
    }
  }
  UA_Server* server_ = nullptr;
};

TEST_F(EventFixture, TheEventTypeBuildsOnTopOfTheAddressSpace) {
  EXPECT_TRUE(buildEventType(server_));

  // The type must be reachable at the number the contract names, in the
  // server's own namespace rather than namespace 0.
  ASSERT_NE(namespaceIndex(), 0);
  const UA_NodeId type = UA_NODEID_NUMERIC(
      namespaceIndex(), static_cast<UA_UInt32>(NodeNumber::kSafetyTransitionEventType));
  UA_NodeClass node_class = UA_NODECLASS_UNSPECIFIED;
  ASSERT_EQ(UA_Server_readNodeClass(server_, type, &node_class), UA_STATUSCODE_GOOD);
  EXPECT_EQ(node_class, UA_NODECLASS_OBJECTTYPE);
}

TEST_F(EventFixture, EveryDocumentedFieldExists) {
  ASSERT_TRUE(buildEventType(server_));
  for (const NodeNumber number :
       {NodeNumber::kEventTransitionMonotonicNs, NodeNumber::kEventSafetySequence,
        NodeNumber::kEventMissedTransitions, NodeNumber::kEventSafetyState,
        NodeNumber::kEventTorquePermitted}) {
    const UA_NodeId field =
        UA_NODEID_NUMERIC(namespaceIndex(), static_cast<UA_UInt32>(number));
    UA_NodeClass node_class = UA_NODECLASS_UNSPECIFIED;
    EXPECT_EQ(UA_Server_readNodeClass(server_, field, &node_class), UA_STATUSCODE_GOOD)
        << "field " << static_cast<std::uint32_t>(number) << " is missing";
    EXPECT_EQ(node_class, UA_NODECLASS_VARIABLE);
  }
}

TEST_F(EventFixture, FiringAnEventSucceedsOnceTheTypeExists) {
  ASSERT_TRUE(buildEventType(server_));
  const SafetyTransitionEvent event = describeTransition(snapshotAt(7, false), 6);

  // This covers the inherited fields as much as the declared ones. Severity and
  // Message come from BaseEventType and live in namespace 0; writing them under
  // this server's namespace index names properties that do not exist, and the
  // event goes out without the two fields a generic client actually reads.
  EXPECT_TRUE(fireSafetyTransition(server_, event));
}

TEST_F(EventFixture, FiringWithoutTheTypeFailsRatherThanFiringSomethingElse) {
  // buildEventType was not called. An event of an unknown type would either be
  // rejected downstream or arrive as a bare BaseEvent carrying none of the
  // fields a client filtered for.
  const SafetyTransitionEvent event = describeTransition(snapshotAt(2, true), 1);
  EXPECT_FALSE(fireSafetyTransition(server_, event));
}

// ---------------------------------------------------------------------------
// What an event claims about what it missed
// ---------------------------------------------------------------------------

// The server reads its snapshot on a poll, so it can coalesce transitions
// before OPC UA is involved at all. It cannot recover them -- but the counter
// tells it how many there were, and reporting that is the difference between a
// client knowing it has the whole story and assuming it.
TEST(SafetyTransitionEventPayload, OneStepMeansNothingWasMissed) {
  const SafetyTransitionEvent event = describeTransition(snapshotAt(5, false), 4);
  EXPECT_EQ(event.safety_sequence, 5u);
  EXPECT_EQ(event.missed_transitions, 0u);
  EXPECT_FALSE(event.torque_permitted);
  EXPECT_EQ(event.transition_monotonic_ns, 1'234'567'890'123);
}

TEST(SafetyTransitionEventPayload, ASequenceJumpReportsWhatWasCoalesced) {
  // The runtime tripped, recovered and tripped again between two reads. The
  // newest state is correct and is not the whole story, and an event that
  // presented it as though it were would be the more dangerous of the two.
  const SafetyTransitionEvent event = describeTransition(snapshotAt(9, false), 5);
  EXPECT_EQ(event.missed_transitions, 3u);
}

TEST(SafetyTransitionEventPayload, AWrappedCounterStillYieldsTheRightGap) {
  // safety_sequence is 64-bit and increments on every transition, so it will
  // not wrap in any deployment. Unsigned subtraction gets the gap right across
  // the boundary for free, whereas a signed difference would report the whole
  // range as missed transitions -- worth pinning, because the arithmetic is the
  // same arithmetic whatever the width, and the next counter may be narrower.
  // From the last representable value the counter goes to 0 and then to 1, so
  // arriving at 1 means exactly one transition was coalesced: the one at 0.
  const std::uint64_t before = std::numeric_limits<std::uint64_t>::max();
  const SafetyTransitionEvent event = describeTransition(snapshotAt(1, true), before);
  EXPECT_EQ(event.missed_transitions, 1u);
}

TEST(SafetyTransitionEventPayload, TorquePermissionSurvivesAsABoolean) {
  EXPECT_TRUE(describeTransition(snapshotAt(2, true), 1).torque_permitted);
  EXPECT_FALSE(describeTransition(snapshotAt(2, false), 1).torque_permitted);
}

}  // namespace
}  // namespace safeedge::opcua

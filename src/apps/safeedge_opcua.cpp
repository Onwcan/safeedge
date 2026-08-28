// SPDX-License-Identifier: Apache-2.0
//
// safeedge-opcua -- an OPC UA view of the runtime, in its own process.
//
// Reads the RuntimeSnapshot the failsafe runtime publishes into shared memory
// and serves it as an OPC UA address space. It does not participate in the
// control loop, does not decide anything, and cannot influence the machine: it
// is a window, and it is deliberately on the other side of a process boundary
// from the thing it looks at.
//
// **Why a separate process rather than another thread in safeedged.**
//
// A protocol stack is a large body of code with a large attack surface. OPC UA
// brings a session layer, a binary and a JSON encoder, a certificate handler and
// a subscription engine. Any of that failing -- a parse bug, an unbounded
// allocation, a certificate library aborting -- must not be able to take down a
// loop that is holding a machine in a safe state.
//
// It also keeps safeedged's image what it is. That binary is 1.51 MB on
// `scratch` with no userland at all; linking open62541 into it would end that,
// and the minimal image is a real property rather than a boast.
//
// The cost is honest: the OPC UA view can be stale or absent while the runtime
// is perfectly healthy. That is why every variable carries a StatusCode and why
// a snapshot that cannot be read is published as BadNoData rather than as the
// last good value.

#include <open62541/plugin/log_stdout.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "safeedge/edge/runtime_snapshot.hpp"
#include "safeedge/ipc/seqlock_slot.hpp"
#include "safeedge/ipc/shared_memory.hpp"
#include "safeedge/opcua/address_space.hpp"

namespace {

using namespace safeedge;

/// UA_Server_run() takes a `volatile UA_Boolean*` and runs until it goes false,
/// so the flag is that type and that sense directly. Keeping a separate
/// sig_atomic_t and translating between them would mean two things to keep in
/// step for no benefit.
volatile UA_Boolean g_server_running = true;

extern "C" void onSignal(int) { g_server_running = false; }

void logEvent(const char* level, const char* message, const char* detail = nullptr) {
  if (detail != nullptr) {
    std::printf(R"({"level":"%s","msg":"%s","detail":"%s"})"
                "\n",
                level, message, detail);
  } else {
    std::printf(R"({"level":"%s","msg":"%s"})"
                "\n",
                level, message);
  }
  std::fflush(stdout);
}

long environmentLong(const char* name, long fallback) {
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(raw, &end, 10);
  return (end != nullptr && *end == '\0') ? parsed : fallback;
}

std::string environmentString(const char* name, const char* fallback) {
  const char* raw = std::getenv(name);
  return (raw != nullptr && *raw != '\0') ? std::string(raw) : std::string(fallback);
}

}  // namespace

int main() {
  std::signal(SIGTERM, onSignal);
  std::signal(SIGINT, onSignal);

  const std::string region_name =
      environmentString("SAFEEDGE_SNAPSHOT_SHM", "/safeedge-runtime");
  const auto port =
      static_cast<std::uint16_t>(environmentLong("SAFEEDGE_OPCUA_PORT", 4840));
  const auto publish_ms = environmentLong("SAFEEDGE_OPCUA_PUBLISH_MS", 100);

  using SnapshotSlot = ipc::SeqlockSlot<edge::RuntimeSnapshot>;

  // Opened, not created. This process is a reader; if it created the region it
  // would hand a starting runtime a slot full of zeros that looks like data.
  auto region =
      ipc::SharedMemoryRegion::openExisting(region_name.c_str(), sizeof(SnapshotSlot));
  if (!region.valid()) {
    logEvent("fatal",
             "snapshot region not available; is safeedged running with "
             "SAFEEDGE_SNAPSHOT_SHM set?",
             region_name.c_str());
    return 1;
  }
  auto* slot = static_cast<SnapshotSlot*>(region.data());
  logEvent("info", "reading snapshot from shared memory", region_name.c_str());

  UA_Server* server = UA_Server_new();
  if (server == nullptr) {
    logEvent("fatal", "could not create the OPC UA server");
    return 2;
  }
  UA_ServerConfig* config = UA_Server_getConfig(server);
  if (UA_ServerConfig_setMinimal(config, port, nullptr) != UA_STATUSCODE_GOOD) {
    logEvent("fatal", "could not configure the OPC UA server");
    UA_Server_delete(server);
    return 2;
  }

  // Said out loud rather than left for someone to discover. The default
  // configuration accepts anonymous connections with no encryption, which is
  // appropriate for a read-only view on a trusted cell network and is not
  // appropriate anywhere else. Making it safe to expose means certificates, an
  // AccessControl plugin and a security policy above None -- a separate piece of
  // work, not a flag.
  logEvent("warn", "anonymous access, no encryption: trusted cell network only");

  if (!opcua::buildAddressSpace(server)) {
    logEvent("fatal", "could not build the address space");
    UA_Server_delete(server);
    return 3;
  }
  logEvent("info", "address space ready");

  // Run the server on its own thread so the publishing loop is not at the mercy
  // of the protocol stack's iteration timing, and vice versa.
  std::atomic<bool> running{true};
  std::thread publisher([&] {
    while (running.load(std::memory_order_relaxed)) {
      edge::RuntimeSnapshot snapshot;
      const bool fresh = slot->tryLoad(snapshot);
      opcua::publishSnapshot(server, snapshot, fresh);
      std::this_thread::sleep_for(std::chrono::milliseconds(publish_ms));
    }
  });

  logEvent("info", "listening", ("opc.tcp://0.0.0.0:" + std::to_string(port)).c_str());

  // Blocks until a signal clears the flag.
  const UA_StatusCode status = UA_Server_run(server, &g_server_running);
  if (status != UA_STATUSCODE_GOOD) {
    logEvent("error", "server loop exited with a fault", UA_StatusCode_name(status));
  }

  // Stop the publisher and wait for it BEFORE deleting the server. The other
  // order is a use-after-free: the publishing thread is still inside
  // UA_Server_writeDataValue on a server that has just been freed, and it would
  // usually survive long enough to look like it worked.
  running.store(false);
  publisher.join();
  UA_Server_delete(server);

  logEvent("info", "stopped");
  return 0;
}

// SPDX-License-Identifier: Apache-2.0
//
// safeedge-opcua-probe -- how long does an OPC UA client take to learn?
//
// Subscribes to SafetyTransitionMonotonicNanoseconds and, on every notification,
// subtracts the value from its own clock. The value is the instant the runtime
// entered its current safety state, so the difference is the whole path:
// decision, snapshot publish, shared memory, the OPC UA server's publishing
// loop, the subscription's sampling interval, and the network.
//
// This is the same quantity `pickcell` measures over an HTTP poll and over a
// shared-memory seqlock, so the three are directly comparable.
//
// What the measurement turned out to say is not what it was built expecting.
// A subscription pushes, so it removes the *client's* poll -- but three other
// things bound the result, and each had to be found the hard way:
//
//   * the subscription's first notification reports state, not an event
//   * UA_Client_run_iterate's timeout IS a client poll, by another name
//   * the intervals the server GRANTS are not the ones the client requested,
//     and with open62541's stock limits they differ by a factor of ten
//
// So this probe reports the revised intervals alongside the latency. A number
// measured without them is a number about somebody's wishes.
//
// It is also a working demonstration of the client side of the contract: the
// namespace is resolved from its URI rather than assumed, which is the thing
// the server's own first version got wrong.

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>
#include <open62541/plugin/create_certificate.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/plugin/pki_default.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "safeedge/opcua/address_space.hpp"
#include "safeedge/opcua/security.hpp"

namespace {

/// This probe's own ApplicationUri, matching the certificate it generates.
constexpr const char* kProbeApplicationUri = "urn:safeedge:opcua:probe";

/// A UA_String over a literal without open62541's macro, which expands to
/// C-style casts this project rejects. Same reasoning as in security.cpp.
UA_String literalString(const char* text) noexcept {
  UA_String value;
  value.length = std::strlen(text);
  value.data = reinterpret_cast<UA_Byte*>(const_cast<char*>(text));
  return value;
}

std::uint64_t monotonicNanos() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

/// Latencies observed, in nanoseconds. Written from the notification callback,
/// which open62541 runs on the thread calling UA_Client_run_iterate, so no
/// synchronisation is needed as long as nothing else touches it.
std::vector<std::uint64_t> g_latencies;
std::uint64_t g_last_transition = 0;
bool g_saw_bad_status = false;

/// Whether the initial notification has been seen and discarded.
///
/// A subscription's first notification reports the *current value*, not a
/// change. Its timestamp is whenever the runtime last transitioned, which may
/// have been long before this client connected -- the first run of this probe
/// reported 7107 ms, and that was the age of the state rather than a latency.
/// Only notifications after the first describe an event this client witnessed.
bool g_had_initial = false;

void onTransitionChanged(UA_Client* /*client*/, UA_UInt32 /*subscription_id*/,
                         void* /*subscription_context*/, UA_UInt32 /*monitored_id*/,
                         void* /*monitored_context*/, UA_DataValue* value) {
  if (value == nullptr) {
    return;
  }
  // A BadNoData notification means the server lost the runtime. Recorded, not
  // measured -- there is no decision instant to subtract from.
  if (value->hasStatus && value->status != UA_STATUSCODE_GOOD) {
    g_saw_bad_status = true;
    return;
  }
  if (!value->hasValue ||
      !UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_INT64])) {
    return;
  }

  const auto transition =
      static_cast<std::uint64_t>(*static_cast<UA_Int64*>(value->value.data));
  if (transition == 0 || transition == g_last_transition) {
    return;  // a repeat
  }
  g_last_transition = transition;

  if (!g_had_initial) {
    // The subscription's opening report of the current value. Not an event, and
    // subtracting from it measures how long ago the runtime last changed state
    // rather than how long this client took to hear about anything.
    g_had_initial = true;
    std::printf("  (initial value discarded: it reports state, not a change)\n");
    std::fflush(stdout);
    return;
  }

  const std::uint64_t now = monotonicNanos();
  if (now > transition) {
    g_latencies.push_back(now - transition);
    std::printf("  notification %2zu: %8.3f ms after the runtime decided\n",
                g_latencies.size(), static_cast<double>(now - transition) / 1e6);
    std::fflush(stdout);
  }
}

/// Reads a whole file into a UA_ByteString, or returns an empty one.
UA_ByteString readFile(const char* path) {
  UA_ByteString out = UA_BYTESTRING_NULL;
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return out;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0 || UA_ByteString_allocBuffer(&out, static_cast<std::size_t>(size)) !=
                       UA_STATUSCODE_GOOD) {
    std::fclose(file);
    return out;
  }
  if (std::fread(out.data, 1, out.length, file) != out.length) {
    UA_ByteString_clear(&out);
  }
  std::fclose(file);
  return out;
}

bool writeFile(const std::string& path, const UA_ByteString& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const std::size_t written = std::fwrite(bytes.data, 1, bytes.length, file);
  std::fclose(file);
  return written == bytes.length;
}

/// Generates this probe's client identity.
UA_StatusCode createIdentity(UA_ByteString* certificate, UA_ByteString* key) {
  UA_String subject[3] = {literalString("C=DE"), literalString("O=safeedge"),
                          literalString("CN=safeedge-opcua-probe")};
  const std::string alt_uri = std::string("URI:") + kProbeApplicationUri;
  UA_String subject_alt_name[2] = {literalString("DNS:localhost"),
                                   literalString(alt_uri.c_str())};
  return UA_CreateCertificate(UA_Log_Stdout, subject, 3, subject_alt_name, 2,
                              UA_CERTIFICATEFORMAT_DER, nullptr, key, certificate);
}

}  // namespace

int main(int argc, char** argv) {
  std::string endpoint = "opc.tcp://127.0.0.1:4840";
  double sampling_ms = 50.0;
  int seconds = 30;
  std::string cert_path;
  std::string key_path;
  std::string write_identity;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--endpoint") == 0) {
      endpoint = argv[i + 1];
    } else if (std::strcmp(argv[i], "--sampling-ms") == 0) {
      sampling_ms = std::atof(argv[i + 1]);
    } else if (std::strcmp(argv[i], "--seconds") == 0) {
      seconds = std::atoi(argv[i + 1]);
    } else if (std::strcmp(argv[i], "--cert") == 0) {
      cert_path = argv[i + 1];
    } else if (std::strcmp(argv[i], "--key") == 0) {
      key_path = argv[i + 1];
    } else if (std::strcmp(argv[i], "--write-identity") == 0) {
      write_identity = argv[i + 1];
    }
  }

#ifdef UA_ENABLE_ENCRYPTION
  // Generate an identity, write it out, and stop. A trust list can only contain
  // a certificate that exists before the server starts, and this probe used to
  // create a fresh one on every run -- which made it precisely the thing a trust
  // list can never contain.
  if (!write_identity.empty()) {
    UA_ByteString certificate = UA_BYTESTRING_NULL;
    UA_ByteString key = UA_BYTESTRING_NULL;
    if (createIdentity(&certificate, &key) != UA_STATUSCODE_GOOD) {
      std::printf("could not create a client certificate\n");
      return 1;
    }
    const bool written = writeFile(write_identity + "/client.der", certificate) &&
                         writeFile(write_identity + "/client-key.der", key);
    UA_ByteString_clear(&certificate);
    UA_ByteString_clear(&key);
    if (!written) {
      std::printf("could not write the identity to %s\n", write_identity.c_str());
      return 1;
    }
    std::printf("wrote %s/client.der and %s/client-key.der\n", write_identity.c_str(),
                write_identity.c_str());
    return 0;
  }
#endif

  UA_Client* client = UA_Client_new();
  UA_ClientConfig* client_config = UA_Client_getConfig(client);

#ifdef UA_ENABLE_ENCRYPTION
  // The server drops SecurityPolicy#None by default, so an unencrypted client
  // simply cannot connect -- which is the security working, and which broke this
  // probe the moment encryption was turned on. A client needs its own identity.
  UA_ByteString client_cert = UA_BYTESTRING_NULL;
  UA_ByteString client_key = UA_BYTESTRING_NULL;
  if (!cert_path.empty() && !key_path.empty()) {
    // A provisioned identity, so that a server trust list can name it.
    client_cert = readFile(cert_path.c_str());
    client_key = readFile(key_path.c_str());
    if (client_cert.length == 0 || client_key.length == 0) {
      std::printf("could not read the identity from %s / %s\n", cert_path.c_str(),
                  key_path.c_str());
      UA_ByteString_clear(&client_cert);
      UA_ByteString_clear(&client_key);
      UA_Client_delete(client);
      return 1;
    }
    std::printf("using the provisioned identity in %s\n", cert_path.c_str());
  } else if (createIdentity(&client_cert, &client_key) != UA_STATUSCODE_GOOD) {
    std::printf("could not create a client certificate\n");
    UA_Client_delete(client);
    return 1;
  }
  UA_ClientConfig_setDefaultEncryption(client_config, client_cert, client_key, nullptr, 0,
                                       nullptr, 0);
  UA_String_clear(&client_config->clientDescription.applicationUri);
  client_config->clientDescription.applicationUri = UA_STRING_ALLOC(kProbeApplicationUri);
  UA_ByteString_clear(&client_cert);
  UA_ByteString_clear(&client_key);

  // Accepts whatever certificate the server presents.
  //
  // This is the thing a real client must not do, and saying so is the point.
  // A production client validates against a trust list, or pins the server's
  // certificate, so that an attacker who can answer on this address cannot
  // simply present their own certificate and be believed. Encryption without
  // verification stops passive eavesdropping and does nothing about an active
  // impersonator.
  //
  // A diagnostic probe pointed at localhost is the one case where accepting any
  // certificate is defensible, and it is defensible only because it is stated.
  UA_CertificateVerification_AcceptAll(&client_config->certificateVerification);
  std::printf(
      "NOTE: this probe accepts any server certificate. A real client validates\n"
      "      against a trust list -- encryption without verification stops\n"
      "      eavesdropping and not impersonation.\n");
#else
  UA_ClientConfig_setDefault(client_config);
#endif

  if (UA_Client_connect(client, endpoint.c_str()) != UA_STATUSCODE_GOOD) {
    std::printf("could not connect to %s\n", endpoint.c_str());
    UA_Client_delete(client);
    return 1;
  }
  std::printf("connected to %s\n", endpoint.c_str());

  // Resolve the namespace by URI. This is the client half of the contract the
  // server's address space documents: the index is per-session and moves, the
  // URI does not. Hardcoding an index here would work today and read the wrong
  // variable the day somebody adds a namespace.
  UA_UInt16 namespace_index = 0;
  UA_String namespace_uri = UA_STRING(const_cast<char*>(safeedge::opcua::kNamespaceUri));
  if (UA_Client_NamespaceGetIndex(client, &namespace_uri, &namespace_index) !=
      UA_STATUSCODE_GOOD) {
    std::printf("server does not publish %s\n", safeedge::opcua::kNamespaceUri);
    UA_Client_disconnect(client);
    UA_Client_delete(client);
    return 2;
  }
  std::printf("namespace %s resolved to index %u\n", safeedge::opcua::kNamespaceUri,
              static_cast<unsigned>(namespace_index));

  UA_CreateSubscriptionRequest subscription_request =
      UA_CreateSubscriptionRequest_default();
  subscription_request.requestedPublishingInterval = sampling_ms;
  const UA_CreateSubscriptionResponse subscription = UA_Client_Subscriptions_create(
      client, subscription_request, nullptr, nullptr, nullptr);
  if (subscription.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
    std::printf("could not create a subscription\n");
    UA_Client_disconnect(client);
    UA_Client_delete(client);
    return 3;
  }

  // Requesting is not getting. The server is free to revise a subscription's
  // publishing interval to whatever its own limits allow, and it does -- ask for
  // 10 ms and this server answers with its floor. A client that reports the
  // interval it *asked for* is reporting a wish; the revised value is the one
  // that bounds anything.
  //
  // Same discipline as reading a scheduling policy back after requesting it:
  // the request succeeded, and that says nothing about what you were given.
  const double revised_publishing = subscription.revisedPublishingInterval;
  if (revised_publishing != sampling_ms) {
    std::printf("publishing interval: asked %.0f ms, server revised to %.0f ms\n",
                sampling_ms, revised_publishing);
  }

  const UA_NodeId transition_node = UA_NODEID_NUMERIC(
      namespace_index,
      static_cast<UA_UInt32>(safeedge::opcua::NodeNumber::kSafetyTransitionMonotonicNs));

  UA_MonitoredItemCreateRequest item =
      UA_MonitoredItemCreateRequest_default(transition_node);
  item.requestedParameters.samplingInterval = sampling_ms;
  const UA_MonitoredItemCreateResult monitored =
      UA_Client_MonitoredItems_createDataChange(client, subscription.subscriptionId,
                                                UA_TIMESTAMPSTORETURN_BOTH, item, nullptr,
                                                onTransitionChanged, nullptr);
  if (monitored.statusCode != UA_STATUSCODE_GOOD) {
    std::printf("could not monitor the transition node\n");
    UA_Client_disconnect(client);
    UA_Client_delete(client);
    return 4;
  }

  const double revised_sampling = monitored.revisedSamplingInterval;
  if (revised_sampling != sampling_ms) {
    std::printf("sampling interval:   asked %.0f ms, server revised to %.0f ms\n",
                sampling_ms, revised_sampling);
  }

  std::printf(
      "subscribed: publishing %.0f ms, sampling %.0f ms -- as revised by the\n"
      "server, not as requested. Trip the runtime to produce notifications:\n"
      "  touch /tmp/safeedge-estop   then   touch /tmp/safeedge-ack\n\n",
      revised_publishing, revised_sampling);

  // The timeout here is the client's own poll, and getting it wrong invalidates
  // the whole measurement.
  //
  // UA_Client_run_iterate blocks up to `timeout` milliseconds waiting for
  // network input. A pushed notification sits in the socket until the client
  // next runs its event loop, so this interval bounds the reaction exactly the
  // way an HTTP poll interval does. The first version of this probe used 100 ms
  // and reported a median of 88 ms no matter what the server-side intervals
  // were set to -- the number being measured was this loop, not the server.
  //
  // 1 ms costs CPU and is right for a measurement tool. A real client would
  // choose a larger value and accept the latency, or drive the event loop from
  // its own reactor -- and would then have to count that choice against its
  // reaction budget, which is the entire point being made.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    UA_Client_run_iterate(client, 1);
  }

  std::printf("\n");
  if (g_latencies.empty()) {
    std::printf("no transitions observed in %d s -- nothing tripped the runtime\n",
                seconds);
  } else {
    std::sort(g_latencies.begin(), g_latencies.end());
    const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    std::printf("n=%zu   min %.1f ms   median %.1f ms   max %.1f ms\n",
                g_latencies.size(), ms(g_latencies.front()),
                ms(g_latencies[g_latencies.size() / 2]), ms(g_latencies.back()));
    std::printf(
        "\nThe binding constraint is the REVISED publishing interval above, not\n"
        "the one this client requested. Measured on this server:\n"
        "\n"
        "  asked 10/10, revised to 100/50 (stock limits)  ->  median 74.9 ms\n"
        "  asked 10/10, granted 10/10 (limits lowered)    ->  median 12.1 ms\n"
        "\n"
        "Same client, same request, six times the difference -- decided entirely\n"
        "by the server's publishingIntervalLimits. A client cannot subscribe\n"
        "faster than the server permits, and one that reports the interval it\n"
        "asked for rather than the one it was given is reporting a wish.\n"
        "\n"
        "So a subscription is not inherently faster than a poll. What it removes\n"
        "is the *client's* poll; the server-side intervals remain, and they are\n"
        "somebody's configuration rather than a property of the protocol.\n");
  }
  if (g_saw_bad_status) {
    std::printf(
        "\nAt least one notification carried a bad status: the server could not\n"
        "read the runtime snapshot at that moment.\n");
  }

  UA_Client_disconnect(client);
  UA_Client_delete(client);
  return 0;
}

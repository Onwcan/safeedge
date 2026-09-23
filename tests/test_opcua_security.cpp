// SPDX-License-Identifier: Apache-2.0
//
// What posture the server actually ends up in.
//
// Every assertion here is about a decision that is invisible from a successful
// startup: a server with a certificate and a server without one both log
// "listening", and only one of them is safe to put on a network.

#include <gtest/gtest.h>

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/plugin/create_certificate.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "safeedge/opcua/security.hpp"

namespace safeedge::opcua {
namespace {

/// Counts endpoints offering SecurityPolicy#None.
std::size_t unencryptedEndpoints(const UA_ServerConfig* config) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < config->endpointsSize; ++i) {
    if (config->endpoints[i].securityMode == UA_MESSAGESECURITYMODE_NONE) {
      ++count;
    }
  }
  return count;
}

/// The common shape of a call, so a test names only what it is varying.
SecurityOptions options(const std::string& cert, const std::string& key,
                        bool allow_unencrypted) {
  SecurityOptions out;
  out.port = 0;
  out.certificate_path = cert;
  out.key_path = key;
  out.allow_unencrypted = allow_unencrypted;
  return out;
}

class SecurityFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    server_ = UA_Server_new();
    ASSERT_NE(server_, nullptr);
  }
  void TearDown() override {
    if (server_ != nullptr) {
      UA_Server_delete(server_);
    }
  }
  UA_ServerConfig* config() { return UA_Server_getConfig(server_); }

  UA_Server* server_ = nullptr;
};

TEST_F(SecurityFixture, EncryptionIsCompiledIn) {
  // If this fails the rest of the file is testing a build nobody should ship.
  EXPECT_TRUE(encryptionAvailable());
}

TEST_F(SecurityFixture, WithNoCertificateProvidedOneIsGenerated) {
  const SecurityPosture posture =
      configureSecurity(config(), options("", "", /*allow_unencrypted=*/false));
  EXPECT_EQ(posture.origin, CertificateOrigin::kGeneratedSelfSigned) << posture.detail;
  EXPECT_GT(posture.policy_count, 1u) << "encrypted policies should be offered";
  EXPECT_FALSE(posture.allows_unencrypted);

  // The detail must say the identity is throwaway. An operator reading the log
  // needs to know that clients will have to re-trust this server after every
  // restart -- that friction is what teaches people to disable verification.
  EXPECT_NE(posture.detail.find("re-trust"), std::string::npos) << posture.detail;
}

// The property that matters most: "encryption enabled" must not mean "encryption
// available alongside plaintext".
TEST_F(SecurityFixture, UnencryptedEndpointsAreRemovedUnlessAskedFor) {
  const SecurityPosture posture =
      configureSecurity(config(), options("", "", /*allow_unencrypted=*/false));
  ASSERT_GT(posture.policy_count, 0u) << posture.detail;

  EXPECT_EQ(unencryptedEndpoints(config()), 0u)
      << "the server still advertises a plaintext endpoint, and a client will take it";
  EXPECT_GT(config()->endpointsSize, 0u) << "something must still be reachable";
}

TEST_F(SecurityFixture, UnencryptedIsAvailableWhenExplicitlyPermitted) {
  const SecurityPosture posture =
      configureSecurity(config(), options("", "", /*allow_unencrypted=*/true));
  ASSERT_GT(posture.policy_count, 0u) << posture.detail;
  EXPECT_TRUE(posture.allows_unencrypted);
  EXPECT_GT(unencryptedEndpoints(config()), 0u)
      << "opting in to insecure operation should actually offer it";
}

TEST_F(SecurityFixture, TheServerClaimsTheIdentityItsCertificateAsserts) {
  const SecurityPosture posture =
      configureSecurity(config(), options("", "", /*allow_unencrypted=*/false));
  ASSERT_GT(posture.policy_count, 0u) << posture.detail;

  // The certificate's subjectAltName carries this URI, and OPC UA requires the
  // server's ApplicationUri to match it. When they disagreed, the server started
  // normally, logged "listening", and then the event loop exited with
  // BadCertificateUriInvalid -- a failure with no symptom until it was fatal.
  const std::string uri(
      reinterpret_cast<const char*>(config()->applicationDescription.applicationUri.data),
      config()->applicationDescription.applicationUri.length);
  EXPECT_EQ(uri, kApplicationUri);

  for (std::size_t i = 0; i < config()->endpointsSize; ++i) {
    const UA_String& endpoint_uri = config()->endpoints[i].server.applicationUri;
    const std::string value(reinterpret_cast<const char*>(endpoint_uri.data),
                            endpoint_uri.length);
    EXPECT_EQ(value, kApplicationUri)
        << "endpoint " << i << " advertises a different URI";
  }
}

TEST_F(SecurityFixture, AMissingCertificateFileFallsBackToGenerationNotToPlaintext) {
  // Paths that do not exist. The server must not read that as permission to run
  // unencrypted; it generates instead, and only an explicit opt-in produces
  // plaintext.
  const SecurityPosture posture =
      configureSecurity(config(), options("/nonexistent/cert.der", "/nonexistent/key.der",
                                          /*allow_unencrypted=*/false));
  EXPECT_EQ(posture.origin, CertificateOrigin::kGeneratedSelfSigned) << posture.detail;
  EXPECT_FALSE(posture.allows_unencrypted);
  EXPECT_EQ(unencryptedEndpoints(config()), 0u);
}

// ---------------------------------------------------------------------------
// Client authentication
// ---------------------------------------------------------------------------

/// A directory holding `count` files that look like DER certificates.
///
/// The bytes are not valid certificates, and that is deliberate: these tests are
/// about which configurations are accepted at all, and a refusal that happens
/// before anything is parsed must not depend on the contents parsing. The live
/// end-to-end check with real certificates is scripts/demonstrate-client-auth.sh.
class TrustListDirectory {
 public:
  explicit TrustListDirectory(std::size_t count) {
    path_ = std::filesystem::temp_directory_path() /
            ("safeedge-trust-" + std::to_string(::getpid()) + "-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(path_);
    for (std::size_t i = 0; i < count; ++i) {
      const std::filesystem::path file = path_ / ("anchor" + std::to_string(i) + ".der");
      std::FILE* out = std::fopen(file.string().c_str(), "wb");
      if (out != nullptr) {
        std::fputs("not-a-certificate", out);
        std::fclose(out);
      }
    }
  }
  ~TrustListDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TrustListDirectory(const TrustListDirectory&) = delete;
  TrustListDirectory& operator=(const TrustListDirectory&) = delete;

  [[nodiscard]] std::string string() const { return path_.string(); }

  bool addCertificate(const UA_ByteString& certificate) const {
    const auto file = path_ / "trusted.der";
    std::FILE* out = std::fopen(file.string().c_str(), "wb");
    if (out == nullptr) {
      return false;
    }
    const std::size_t written = std::fwrite(certificate.data, 1, certificate.length, out);
    std::fclose(out);
    return written == certificate.length;
  }

 private:
  std::filesystem::path path_;
};

// The bypass. A plaintext endpoint needs no certificate, so offering one beside
// a trust list means every client the list was meant to exclude connects to the
// other endpoint instead -- and the server reports itself as authenticating.
TEST_F(SecurityFixture, ATrustListCannotBeCombinedWithAPlaintextEndpoint) {
  const TrustListDirectory trust(1);
  SecurityOptions opts = options("", "", /*allow_unencrypted=*/true);
  opts.trust_list_directory = trust.string();
  opts.authentication = ClientAuthentication::kTrustList;

  const SecurityPosture posture = configureSecurity(config(), opts);
  EXPECT_EQ(posture.policy_count, 0u) << "the configuration should be refused";
  EXPECT_NE(posture.detail.find("way around the trust list"), std::string::npos)
      << posture.detail;
}

// Checking certificates against nothing is a configuration mistake, and both
// readings of it -- trust everyone, trust no one -- are worse than saying so.
TEST_F(SecurityFixture, AnEmptyTrustListIsRefusedRatherThanInterpreted) {
  const TrustListDirectory empty(0);
  SecurityOptions opts = options("", "", /*allow_unencrypted=*/false);
  opts.trust_list_directory = empty.string();
  opts.authentication = ClientAuthentication::kTrustList;

  const SecurityPosture posture = configureSecurity(config(), opts);
  EXPECT_EQ(posture.policy_count, 0u);
  EXPECT_EQ(posture.trusted_certificate_count, 0u);
  EXPECT_NE(posture.detail.find("not a policy"), std::string::npos) << posture.detail;
}

TEST_F(SecurityFixture, AMissingTrustListDirectoryIsRefusedToo) {
  SecurityOptions opts = options("", "", /*allow_unencrypted=*/false);
  opts.trust_list_directory = "/nonexistent/trust";
  opts.authentication = ClientAuthentication::kTrustList;

  const SecurityPosture posture = configureSecurity(config(), opts);
  EXPECT_EQ(posture.policy_count, 0u);
  EXPECT_EQ(posture.trusted_certificate_count, 0u);
}

TEST_F(SecurityFixture, TheDefaultPostureAcceptsAnyClientAndSaysSo) {
  const SecurityPosture posture =
      configureSecurity(config(), options("", "", /*allow_unencrypted=*/false));
  ASSERT_GT(posture.policy_count, 0u) << posture.detail;
  EXPECT_EQ(posture.authentication, ClientAuthentication::kAcceptAnyCertificate);
  EXPECT_EQ(posture.trusted_certificate_count, 0u);
  // An operator reading the log has to be able to tell an encrypted server that
  // restricts who may connect from one that does not.
  EXPECT_NE(posture.detail.find("any client certificate accepted"), std::string::npos)
      << posture.detail;
}

TEST(ProbeServerVerification, TrustMustBeExplicitAndNonempty) {
  UA_Client* client = UA_Client_new();
  ASSERT_NE(client, nullptr);
  ServerVerificationOptions opts;
  EXPECT_FALSE(configureServerVerification(UA_Client_getConfig(client), opts).configured);
  const TrustListDirectory empty(0);
  opts.trust_list_directory = empty.string();
  EXPECT_FALSE(configureServerVerification(UA_Client_getConfig(client), opts).configured);
  opts.trust_list_directory = "/nonexistent/server-trust";
  EXPECT_FALSE(configureServerVerification(UA_Client_getConfig(client), opts).configured);
  const TrustListDirectory malformed(1);
  opts.trust_list_directory = malformed.string();
  EXPECT_FALSE(configureServerVerification(UA_Client_getConfig(client), opts).configured);
  opts.insecure_accept_any_certificate = true;
  EXPECT_FALSE(configureServerVerification(UA_Client_getConfig(client), opts).configured)
      << "an insecure bypass must not silently override an operator's trust list";
  UA_Client_delete(client);
}

// Both event loops run on the test thread. Binding loopback port zero gives each
// test a private ephemeral port, so parallel CTest runs cannot steal a fixed one.
class ProbeConnectionFixture : public SecurityFixture {
 protected:
  void SetUp() override {
    SecurityFixture::SetUp();
    ASSERT_NE(server_, nullptr);
    const auto posture = configureSecurity(config(), options("", "", true));
    ASSERT_GT(posture.policy_count, 0u) << posture.detail;
    ASSERT_GT(config()->endpointsSize, 0u);
    bool saved_server_certificate = false;
    for (std::size_t i = 0; i < config()->securityPoliciesSize; ++i) {
      const auto& certificate = config()->securityPolicies[i].localCertificate;
      if (certificate.length > 0) {
        saved_server_certificate = server_trust_.addCertificate(certificate);
        break;
      }
    }
    ASSERT_TRUE(saved_server_certificate);

    UA_Array_delete(config()->serverUrls, config()->serverUrlsSize,
                    &UA_TYPES[UA_TYPES_STRING]);
    config()->serverUrls = UA_String_new();
    ASSERT_NE(config()->serverUrls, nullptr);
    config()->serverUrlsSize = 1;
    config()->serverUrls[0] = UA_STRING_ALLOC("opc.tcp://127.0.0.1:0");
    ASSERT_EQ(UA_Server_run_startup(server_), UA_STATUSCODE_GOOD);
    started_ = true;
    for (std::size_t i = 0; i < config()->applicationDescription.discoveryUrlsSize; ++i) {
      const UA_String& url = config()->applicationDescription.discoveryUrls[i];
      const std::string value(reinterpret_cast<const char*>(url.data), url.length);
      if (value.starts_with("opc.tcp://127.0.0.1:") && !value.ends_with(":0")) {
        endpoint_ = value;
        break;
      }
    }
    ASSERT_FALSE(endpoint_.empty());

    const auto text = [](const char* value) {
      return UA_String{std::strlen(value),
                       reinterpret_cast<UA_Byte*>(const_cast<char*>(value))};
    };
    UA_String subject[] = {text("CN=safeedge-probe-test")};
    UA_String alt_names[] = {text("URI:urn:safeedge:probe:test"), text("DNS:localhost")};
    UA_ByteString certificate = UA_BYTESTRING_NULL;
    UA_ByteString key = UA_BYTESTRING_NULL;
    const UA_StatusCode generated =
        UA_CreateCertificate(UA_Log_Stdout, subject, 1, alt_names, 2,
                             UA_CERTIFICATEFORMAT_DER, nullptr, &key, &certificate);
    if (generated == UA_STATUSCODE_GOOD) {
      // Trusting the client certificate instead gives a valid, unrelated anchor.
      EXPECT_TRUE(unrelated_trust_.addCertificate(certificate));
      client_ = UA_Client_new();
      if (client_ != nullptr) {
        configured_ = UA_ClientConfig_setDefaultEncryption(
            UA_Client_getConfig(client_), certificate, key, nullptr, 0, nullptr, 0);
      }
    }
    UA_ByteString_clear(&certificate);
    UA_ByteString_clear(&key);
    ASSERT_EQ(generated, UA_STATUSCODE_GOOD);
    ASSERT_NE(client_, nullptr);
    ASSERT_EQ(configured_, UA_STATUSCODE_GOOD);
    UA_ClientConfig* client_config = UA_Client_getConfig(client_);
    UA_String_clear(&client_config->clientDescription.applicationUri);
    client_config->clientDescription.applicationUri =
        UA_STRING_ALLOC("urn:safeedge:probe:test");
    client_config->timeout = 2000;
  }

  void TearDown() override {
    if (client_ != nullptr) {
      UA_Client_disconnectAsync(client_);
      for (int i = 0; started_ && i < 10; ++i) {
        UA_Server_run_iterate(server_, false);
        UA_Client_run_iterate(client_, 1);
      }
      UA_Client_delete(client_);
    }
    if (started_) {
      UA_Server_run_shutdown(server_);
    }
    SecurityFixture::TearDown();
  }

  UA_StatusCode connect() {
    UA_StatusCode result = UA_Client_connectAsync(client_, endpoint_.c_str());
    if (result != UA_STATUSCODE_GOOD) {
      return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      UA_Server_run_iterate(server_, false);
      UA_Client_run_iterate(client_, 1);
      UA_SessionState session = UA_SESSIONSTATE_CLOSED;
      UA_Client_getState(client_, nullptr, &session, &result);
      if (result != UA_STATUSCODE_GOOD || session == UA_SESSIONSTATE_ACTIVATED) {
        return result;
      }
    }
    return UA_STATUSCODE_BADTIMEOUT;
  }

  TrustListDirectory server_trust_{0};
  TrustListDirectory unrelated_trust_{0};
  UA_Client* client_{nullptr};

 private:
  bool started_{false};
  UA_StatusCode configured_{UA_STATUSCODE_BADINTERNALERROR};
  std::string endpoint_;
};

TEST_F(ProbeConnectionFixture, TrustedServerConnectsWithEncryption) {
  ServerVerificationOptions opts;
  opts.trust_list_directory = server_trust_.string();
  const auto posture = configureServerVerification(UA_Client_getConfig(client_), opts);
  ASSERT_TRUE(posture.configured) << posture.detail;
  ASSERT_TRUE(posture.verifies_server_certificate);
  EXPECT_EQ(posture.trusted_certificate_count, 1u);
  ASSERT_EQ(connect(), UA_STATUSCODE_GOOD);
  UA_MessageSecurityMode mode = UA_MESSAGESECURITYMODE_INVALID;
  ASSERT_EQ(UA_Client_getConnectionAttribute_scalar(
                client_, UA_QUALIFIEDNAME(0, const_cast<char*>("securityMode")),
                &UA_TYPES[UA_TYPES_MESSAGESECURITYMODE], &mode),
            UA_STATUSCODE_GOOD);
  EXPECT_EQ(mode, UA_MESSAGESECURITYMODE_SIGNANDENCRYPT);
}

TEST_F(ProbeConnectionFixture, UntrustedServerIsRejectedEvenWhenItOffersPlaintext) {
  ServerVerificationOptions opts;
  opts.trust_list_directory = unrelated_trust_.string();
  const auto posture = configureServerVerification(UA_Client_getConfig(client_), opts);
  ASSERT_TRUE(posture.configured) << posture.detail;
  ASSERT_TRUE(posture.verifies_server_certificate);
  EXPECT_EQ(connect(), UA_STATUSCODE_BADCERTIFICATEUNTRUSTED);
}

TEST_F(ProbeConnectionFixture, ExplicitInsecureBypassConnectsAndReportsNoVerification) {
  ServerVerificationOptions opts;
  opts.insecure_accept_any_certificate = true;
  const auto posture = configureServerVerification(UA_Client_getConfig(client_), opts);
  ASSERT_TRUE(posture.configured) << posture.detail;
  EXPECT_FALSE(posture.verifies_server_certificate);
  EXPECT_NE(posture.detail.find("DISABLED"), std::string::npos);
  EXPECT_EQ(connect(), UA_STATUSCODE_GOOD);
}

}  // namespace
}  // namespace safeedge::opcua

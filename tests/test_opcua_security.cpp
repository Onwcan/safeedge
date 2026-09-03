// SPDX-License-Identifier: Apache-2.0
//
// What posture the server actually ends up in.
//
// Every assertion here is about a decision that is invisible from a successful
// startup: a server with a certificate and a server without one both log
// "listening", and only one of them is safe to put on a network.

#include <gtest/gtest.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
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

}  // namespace
}  // namespace safeedge::opcua

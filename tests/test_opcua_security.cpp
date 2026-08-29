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
  const SecurityPosture posture = configureSecurity(config(), 0, "", "",
                                                    /*allow_unencrypted=*/false,
                                                    /*allow_anonymous=*/true);
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
  const SecurityPosture posture = configureSecurity(config(), 0, "", "",
                                                    /*allow_unencrypted=*/false,
                                                    /*allow_anonymous=*/true);
  ASSERT_GT(posture.policy_count, 0u) << posture.detail;

  EXPECT_EQ(unencryptedEndpoints(config()), 0u)
      << "the server still advertises a plaintext endpoint, and a client will take it";
  EXPECT_GT(config()->endpointsSize, 0u) << "something must still be reachable";
}

TEST_F(SecurityFixture, UnencryptedIsAvailableWhenExplicitlyPermitted) {
  const SecurityPosture posture = configureSecurity(config(), 0, "", "",
                                                    /*allow_unencrypted=*/true,
                                                    /*allow_anonymous=*/true);
  ASSERT_GT(posture.policy_count, 0u) << posture.detail;
  EXPECT_TRUE(posture.allows_unencrypted);
  EXPECT_GT(unencryptedEndpoints(config()), 0u)
      << "opting in to insecure operation should actually offer it";
}

TEST_F(SecurityFixture, TheServerClaimsTheIdentityItsCertificateAsserts) {
  const SecurityPosture posture = configureSecurity(config(), 0, "", "",
                                                    /*allow_unencrypted=*/false,
                                                    /*allow_anonymous=*/true);
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
      configureSecurity(config(), 0, "/nonexistent/cert.der", "/nonexistent/key.der",
                        /*allow_unencrypted=*/false, /*allow_anonymous=*/true);
  EXPECT_EQ(posture.origin, CertificateOrigin::kGeneratedSelfSigned) << posture.detail;
  EXPECT_FALSE(posture.allows_unencrypted);
  EXPECT_EQ(unencryptedEndpoints(config()), 0u);
}

}  // namespace
}  // namespace safeedge::opcua

// SPDX-License-Identifier: Apache-2.0
#include "safeedge/opcua/security.hpp"

#include <open62541/plugin/log_stdout.h>
#include <open62541/server_config_default.h>

#ifdef UA_ENABLE_ENCRYPTION
#include <open62541/plugin/create_certificate.h>
#include <open62541/plugin/pki_default.h>
#endif

#include <cstdio>
#include <cstring>
#include <vector>

namespace safeedge::opcua {
namespace {

/// A UA_String over a literal, without open62541's macro.
///
/// UA_STRING_STATIC expands to C-style casts, and this project compiles with
/// -Wold-style-cast -Werror. The macro is fine; it simply cannot be expanded
/// inside code held to that bar, so the two casts are written explicitly. The
/// const_cast is unavoidable: UA_String::data is non-const even for strings the
/// library only ever reads.
UA_String literalString(const char* text) noexcept {
  UA_String value;
  value.length = std::strlen(text);
  value.data = reinterpret_cast<UA_Byte*>(const_cast<char*>(text));
  return value;
}

/// Reads a whole file into a UA_ByteString, or returns an empty one.
UA_ByteString readFile(const std::string& path) {
  UA_ByteString out = UA_BYTESTRING_NULL;
  if (path.empty()) {
    return out;
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return out;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(file);
    return out;
  }
  if (UA_ByteString_allocBuffer(&out, static_cast<std::size_t>(size)) !=
      UA_STATUSCODE_GOOD) {
    std::fclose(file);
    return out;
  }
  const std::size_t read = std::fread(out.data, 1, out.length, file);
  std::fclose(file);
  if (read != out.length) {
    UA_ByteString_clear(&out);
  }
  return out;
}

/// Removes SecurityPolicy#None and every endpoint that uses it.
///
/// open62541 always adds None alongside the encrypted policies. Leaving it in
/// place means the server advertises an unencrypted endpoint and a client will
/// happily take it -- so a server "with encryption enabled" can spend its whole
/// life serving plaintext to clients that never asked for anything better.
/// Offering it has to be a decision, not a default.
std::size_t dropUnencryptedEndpoints(UA_ServerConfig* config) {
  std::size_t removed = 0;
  std::size_t kept = 0;
  for (std::size_t i = 0; i < config->endpointsSize; ++i) {
    const UA_MessageSecurityMode mode = config->endpoints[i].securityMode;
    if (mode == UA_MESSAGESECURITYMODE_NONE) {
      UA_EndpointDescription_clear(&config->endpoints[i]);
      ++removed;
      continue;
    }
    if (kept != i) {
      config->endpoints[kept] = config->endpoints[i];
    }
    ++kept;
  }
  config->endpointsSize = kept;
  return removed;
}

}  // namespace

bool encryptionAvailable() noexcept {
#ifdef UA_ENABLE_ENCRYPTION
  return true;
#else
  return false;
#endif
}

SecurityPosture configureSecurity(UA_ServerConfig* config, std::uint16_t port,
                                  const std::string& cert_path,
                                  const std::string& key_path, bool allow_unencrypted,
                                  bool allow_anonymous) {
  SecurityPosture posture;
  posture.allows_anonymous = allow_anonymous;

  if (config == nullptr) {
    posture.detail = "no server config";
    return posture;
  }

#ifdef UA_ENABLE_ENCRYPTION
  UA_ByteString certificate = readFile(cert_path);
  UA_ByteString private_key = readFile(key_path);

  if (certificate.length > 0 && private_key.length > 0) {
    posture.origin = CertificateOrigin::kLoadedFromFiles;
    posture.detail = "certificate loaded from " + cert_path;
  } else {
    UA_ByteString_clear(&certificate);
    UA_ByteString_clear(&private_key);

    UA_String subject[3] = {literalString("C=DE"), literalString("O=safeedge"),
                            literalString("CN=safeedge-opcua")};
    const std::string alt_uri = std::string("URI:") + kApplicationUri;
    UA_String subject_alt_name[2] = {literalString("DNS:localhost"),
                                     literalString(alt_uri.c_str())};
    const UA_StatusCode created = UA_CreateCertificate(
        UA_Log_Stdout, subject, 3, subject_alt_name, 2, UA_CERTIFICATEFORMAT_DER, nullptr,
        &private_key, &certificate);
    if (created != UA_STATUSCODE_GOOD) {
      UA_ByteString_clear(&certificate);
      UA_ByteString_clear(&private_key);
      posture.detail = "could not generate a certificate";
      if (!allow_unencrypted) {
        // No silent downgrade. Everything would appear to work and the transport
        // would be readable by anyone on the network, which is the one outcome
        // worse than refusing to start.
        return posture;
      }
      if (UA_ServerConfig_setMinimal(config, port, nullptr) == UA_STATUSCODE_GOOD) {
        posture.policy_count = config->securityPoliciesSize;
        posture.allows_unencrypted = true;
      }
      return posture;
    }
    posture.origin = CertificateOrigin::kGeneratedSelfSigned;
    posture.detail =
        "self-signed certificate generated at startup; clients must re-trust it "
        "after every restart";
  }

  const UA_StatusCode configured = UA_ServerConfig_setDefaultWithSecurityPolicies(
      config, port, &certificate, &private_key, nullptr, 0, nullptr, 0, nullptr, 0);
  UA_ByteString_clear(&certificate);
  UA_ByteString_clear(&private_key);

  if (configured != UA_STATUSCODE_GOOD) {
    posture.origin = CertificateOrigin::kNone;
    posture.detail = "could not apply security policies";
    return posture;
  }

  // The server must claim the identity its certificate asserts. Setting this
  // after setDefaultWithSecurityPolicies, because that call populates the
  // application description with open62541's default URI and would overwrite it.
  UA_String_clear(&config->applicationDescription.applicationUri);
  config->applicationDescription.applicationUri = UA_STRING_ALLOC(kApplicationUri);
  for (std::size_t i = 0; i < config->endpointsSize; ++i) {
    UA_String_clear(&config->endpoints[i].server.applicationUri);
    config->endpoints[i].server.applicationUri = UA_STRING_ALLOC(kApplicationUri);
  }

  // The server accepts any client certificate, and that is a decision worth
  // stating rather than a default worth hiding.
  //
  // What this server is protecting is the *channel*: with an encrypted policy,
  // traffic cannot be read or altered by anyone else on the network. Client
  // certificates would be about *authentication* -- deciding who is allowed to
  // ask -- and this server already permits anonymous login, so validating the
  // certificate authenticates nobody. Rejecting a self-signed client cert while
  // waving through an anonymous session is an obstacle, not a control.
  //
  // Real client authentication means two things together, and neither is here:
  // a trust list the operator populates, and anonymous access turned off. Doing
  // one without the other produces a server that looks authenticated and is not.
  UA_CertificateVerification_AcceptAll(&config->secureChannelPKI);
  UA_CertificateVerification_AcceptAll(&config->sessionPKI);
  posture.detail +=
      "; any client certificate accepted (anonymous is permitted, so "
      "validating one would authenticate nobody)";

  if (!allow_unencrypted) {
    const std::size_t removed = dropUnencryptedEndpoints(config);
    posture.detail += "; dropped " + std::to_string(removed) + " unencrypted endpoint(s)";
  }
  posture.policy_count = config->securityPoliciesSize;
  posture.allows_unencrypted = allow_unencrypted;

  // Anonymous access is separate from encryption and is worth keeping separate.
  // Encryption says nobody can read or forge the traffic; it says nothing about
  // who is allowed to ask. For a read-only diagnostic view of a safety runtime
  // on a cell network, anonymous is defensible -- there is nothing to write and
  // nothing secret. It stops being defensible the moment anything here is
  // writable, which is why the address space has no writable node.
  if (!allow_anonymous && config->accessControl.clear != nullptr) {
    // open62541's default AccessControl is built with anonymous enabled. Turning
    // it off properly means supplying credentials, which is a deployment
    // decision this component does not get to invent -- so it refuses rather
    // than pretending.
    posture.detail += "; anonymous cannot be disabled without configured credentials";
    posture.allows_anonymous = true;
  }
  return posture;
#else
  (void)cert_path;
  (void)key_path;
  (void)allow_anonymous;
  if (!allow_unencrypted) {
    posture.detail = "built without a crypto backend and unencrypted use not permitted";
    return posture;
  }
  if (UA_ServerConfig_setMinimal(config, port, nullptr) == UA_STATUSCODE_GOOD) {
    posture.policy_count = config->securityPoliciesSize;
    posture.allows_unencrypted = true;
    posture.detail = "built without a crypto backend";
  }
  return posture;
#endif
}

}  // namespace safeedge::opcua

// SPDX-License-Identifier: Apache-2.0
#include "safeedge/opcua/security.hpp"

#include <open62541/client.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/server_config_default.h>

#ifdef UA_ENABLE_ENCRYPTION
#include <open62541/plugin/create_certificate.h>
#include <open62541/plugin/pki_default.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
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

/// Every `.der` file in `directory`, read into byte strings.
///
/// Read in sorted order so the trust list is the same on every start, and so a
/// log line naming its size means the same thing twice. Sub-directories are not
/// followed: a trust list is a flat set of anchors, and recursing into whatever
/// happens to be underneath would make its contents depend on tidiness.
std::vector<UA_ByteString> readTrustList(const std::string& directory) {
  std::vector<UA_ByteString> loaded;
  if (directory.empty()) {
    return loaded;
  }
  std::error_code ec;
  std::vector<std::filesystem::path> candidates;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".der") {
      candidates.push_back(entry.path());
    }
  }
  if (ec) {
    return loaded;
  }
  std::sort(candidates.begin(), candidates.end());
  for (const auto& path : candidates) {
    UA_ByteString bytes = readFile(path.string());
    if (bytes.length > 0) {
      loaded.push_back(bytes);
    } else {
      // A file that is present but unreadable is not the same as one that is
      // absent, and silently skipping it would shrink the trust list without
      // saying so. It is dropped, and the count in the posture is what the
      // operator compares against what they put there.
      UA_ByteString_clear(&bytes);
    }
  }
  return loaded;
}

void clearTrustList(std::vector<UA_ByteString>& list) {
  for (UA_ByteString& entry : list) {
    UA_ByteString_clear(&entry);
  }
  list.clear();
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

ServerVerificationPosture configureServerVerification(
    UA_ClientConfig* config, const ServerVerificationOptions& options) {
  ServerVerificationPosture posture;
  if (config == nullptr) {
    posture.detail = "no client config";
    return posture;
  }
  if (options.insecure_accept_any_certificate && !options.trust_list_directory.empty()) {
    posture.detail =
        "--trust-list and --insecure-accept-any-server-cert are mutually exclusive";
    return posture;
  }
#ifdef UA_ENABLE_ENCRYPTION
  // Checking certificates is ineffective if endpoint selection can choose None.
  config->securityMode = UA_MESSAGESECURITYMODE_SIGNANDENCRYPT;
  if (options.insecure_accept_any_certificate) {
    UA_CertificateVerification_AcceptAll(&config->certificateVerification);
    posture.configured = true;
    posture.detail = "DISABLED (--insecure-accept-any-server-cert)";
    return posture;
  }
  if (options.trust_list_directory.empty()) {
    posture.detail =
        "server verification requires --trust-list DIR; "
        "use --insecure-accept-any-server-cert only for an intentional insecure demo";
    return posture;
  }
  std::vector<UA_ByteString> trust_list = readTrustList(options.trust_list_directory);
  posture.trusted_certificate_count = trust_list.size();
  if (trust_list.empty()) {
    posture.detail =
        "no server certificates loaded from '" + options.trust_list_directory + "'";
    return posture;
  }
  const UA_StatusCode result = UA_CertificateVerification_Trustlist(
      &config->certificateVerification, trust_list.data(), trust_list.size(), nullptr, 0,
      nullptr, 0);
  clearTrustList(trust_list);
  if (result != UA_STATUSCODE_GOOD) {
    posture.detail = "could not configure server trust list: " +
                     std::string(UA_StatusCode_name(result));
    return posture;
  }
  posture.configured = true;
  posture.verifies_server_certificate = true;
  posture.detail = "ENABLED (" + std::to_string(posture.trusted_certificate_count) +
                   " trusted certificate(s) from " + options.trust_list_directory + ")";
#else
  posture.detail = "server verification requires a build with encryption enabled";
#endif
  return posture;
}

SecurityPosture configureSecurity(UA_ServerConfig* config,
                                  const SecurityOptions& options) {
  SecurityPosture posture;
  posture.authentication = options.authentication;

  if (config == nullptr) {
    posture.detail = "no server config";
    return posture;
  }

#ifdef UA_ENABLE_ENCRYPTION
  const bool wants_trust_list =
      options.authentication == ClientAuthentication::kTrustList;

  // Refused before anything is built, because both of these produce a server
  // that reports itself as authenticating clients and does not.
  if (wants_trust_list && options.allow_unencrypted) {
    posture.detail =
        "a trust list cannot be combined with SecurityPolicy#None: the "
        "unencrypted endpoint needs no certificate, so it is a way around the "
        "trust list rather than a fallback beside it";
    return posture;
  }

  std::vector<UA_ByteString> trust_list;
  if (wants_trust_list) {
    trust_list = readTrustList(options.trust_list_directory);
    posture.trusted_certificate_count = trust_list.size();
    if (trust_list.empty()) {
      posture.detail =
          "client authentication was requested but no certificates "
          "were loaded from '" +
          options.trust_list_directory + "'; checking against nothing is not a policy";
      return posture;
    }
  }

  UA_ByteString certificate = readFile(options.certificate_path);
  UA_ByteString private_key = readFile(options.key_path);

  if (certificate.length > 0 && private_key.length > 0) {
    posture.origin = CertificateOrigin::kLoadedFromFiles;
    posture.detail = "certificate loaded from " + options.certificate_path;
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
      clearTrustList(trust_list);
      posture.detail = "could not generate a certificate";
      if (!options.allow_unencrypted) {
        // No silent downgrade. Everything would appear to work and the transport
        // would be readable by anyone on the network, which is the one outcome
        // worse than refusing to start.
        return posture;
      }
      if (UA_ServerConfig_setMinimal(config, options.port, nullptr) ==
          UA_STATUSCODE_GOOD) {
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

  // Passing the trust list here rather than overriding afterwards: this call
  // installs it into both secureChannelPKI and sessionPKI, and getting only one
  // of them is a distinction nothing later would reveal.
  const UA_StatusCode configured = UA_ServerConfig_setDefaultWithSecurityPolicies(
      config, options.port, &certificate, &private_key,
      trust_list.empty() ? nullptr : trust_list.data(), trust_list.size(), nullptr, 0,
      nullptr, 0);
  UA_ByteString_clear(&certificate);
  UA_ByteString_clear(&private_key);
  clearTrustList(trust_list);

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

  if (wants_trust_list) {
    posture.detail += "; " + std::to_string(posture.trusted_certificate_count) +
                      " trusted client certificate(s) loaded from " +
                      options.trust_list_directory;
  } else {
    // Accepting any client certificate is a decision worth stating rather than
    // a default worth hiding. What the server still protects is the *channel*:
    // with an encrypted policy, traffic cannot be read or altered by anyone
    // else on the network. What it does not do is decide who is allowed to ask
    // -- anyone who can reach the port can connect.
    UA_CertificateVerification_AcceptAll(&config->secureChannelPKI);
    UA_CertificateVerification_AcceptAll(&config->sessionPKI);
    posture.detail += "; any client certificate accepted";
  }

  if (!options.allow_unencrypted) {
    const std::size_t removed = dropUnencryptedEndpoints(config);
    posture.detail += "; dropped " + std::to_string(removed) + " unencrypted endpoint(s)";
  }
  posture.policy_count = config->securityPoliciesSize;
  posture.allows_unencrypted = options.allow_unencrypted;

  // Anonymous user tokens are a separate question from who may open a channel,
  // and the trust list above does not answer it. For a read-only diagnostic
  // view there is nothing to write and nothing secret, so anonymous is
  // defensible; turning it off properly means supplying credentials, which is a
  // deployment decision this component does not get to invent.
  posture.allows_anonymous = true;
  return posture;
#else
  if (options.authentication == ClientAuthentication::kTrustList) {
    posture.detail =
        "client authentication needs a crypto backend, and this "
        "build has none";
    return posture;
  }
  if (!options.allow_unencrypted) {
    posture.detail = "built without a crypto backend and unencrypted use not permitted";
    return posture;
  }
  if (UA_ServerConfig_setMinimal(config, options.port, nullptr) == UA_STATUSCODE_GOOD) {
    posture.policy_count = config->securityPoliciesSize;
    posture.allows_unencrypted = true;
    posture.detail = "built without a crypto backend";
  }
  return posture;
#endif
}

}  // namespace safeedge::opcua

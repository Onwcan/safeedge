// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

struct UA_Server;
struct UA_ServerConfig;

namespace safeedge::opcua {

/// How the server's identity was obtained.
enum class CertificateOrigin : std::uint8_t {
  /// Loaded from files the operator provided. The only option that gives a
  /// stable identity across restarts.
  kLoadedFromFiles,
  /// Generated at startup because none was provided.
  kGeneratedSelfSigned,
  /// Encryption is not compiled in, or no certificate could be obtained.
  kNone,
};

/// What the server ended up running as, so the daemon can report it rather than
/// leaving the operator to infer it from a successful startup.
struct SecurityPosture {
  CertificateOrigin origin{CertificateOrigin::kNone};

  /// Number of security policies the server offers.
  std::size_t policy_count{0};

  /// True when SecurityPolicy#None is among them, so an unencrypted session is
  /// possible.
  bool allows_unencrypted{false};

  /// True when a client may connect without presenting any user identity.
  bool allows_anonymous{true};

  /// Human-readable explanation for the startup log.
  std::string detail;
};

/// Configures `config` for `port`, with encryption if it is available.
///
/// The order of preference is deliberate:
///
///   1. A certificate and key from `cert_path` / `key_path` if both are set.
///      This is the only arrangement that gives the server a stable identity, so
///      clients that trusted it yesterday still trust it today.
///
///   2. A self-signed certificate generated now. Encryption works immediately
///      with nothing to provision, at the cost of a new identity on every
///      restart -- every client must re-trust it, which is exactly the friction
///      that teaches people to disable certificate checking. Fine for a
///      demonstration, not fine for a deployment.
///
///   3. Unencrypted, only if `allow_unencrypted` is true.
///
/// If encryption is compiled in and `allow_unencrypted` is false, a server that
/// cannot obtain a certificate **fails** rather than quietly falling back. A
/// silent downgrade to plaintext is the worst outcome available: everything
/// works, nothing complains, and the transport is readable by anyone on the
/// network.
SecurityPosture configureSecurity(UA_ServerConfig* config, std::uint16_t port,
                                  const std::string& cert_path,
                                  const std::string& key_path, bool allow_unencrypted,
                                  bool allow_anonymous);

/// Whether this build has a crypto backend at all.
bool encryptionAvailable() noexcept;

/// The server's ApplicationUri.
///
/// The same string must appear in two places: the `URI:` subjectAltName of the
/// certificate, and `applicationDescription.applicationUri` on the server. OPC
/// UA requires them to match, because a certificate is a claim to an identity
/// and a server presenting one has to actually claim that identity.
///
/// The consequence of getting it wrong is not subtle; the symptom is. The
/// server starts, builds its address space, logs "listening", and then the
/// event loop exits with `BadCertificateUriInvalid` -- open62541 defaults the
/// ApplicationUri to `urn:open62541.server.application` while the generated
/// certificate says something else. One constant, used by both, so they cannot
/// drift apart.
inline constexpr const char* kApplicationUri = "urn:safeedge:opcua";

}  // namespace safeedge::opcua

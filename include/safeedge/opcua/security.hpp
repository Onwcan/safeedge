// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
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

/// How the server decides which clients may open a channel.
enum class ClientAuthentication : std::uint8_t {
  /// Any client certificate is accepted without being checked. The channel is
  /// still encrypted, so traffic cannot be read or forged in transit -- but
  /// anyone who can reach the port can connect.
  kAcceptAnyCertificate,
  /// Only clients whose certificate is in the trust list may open a channel.
  /// Everyone else is refused with BadCertificateUntrusted before a session
  /// exists.
  kTrustList,
};

/// Everything the server needs to decide what it will and will not accept.
///
/// A struct rather than six positional arguments, two of which were adjacent
/// booleans. `configureSecurity(config, port, "", "", false, true)` is a call
/// nobody can read, and swapping those last two silently turns encryption
/// enforcement into an anonymous-access setting.
struct SecurityOptions {
  std::uint16_t port{4840};

  /// DER certificate and private key giving the server a stable identity.
  /// Both must be set, or neither.
  std::string certificate_path;
  std::string key_path;

  /// Directory of DER-encoded certificates to trust, read when `authentication`
  /// is kTrustList. Every regular file ending in `.der` is loaded.
  std::string trust_list_directory;

  /// Offer SecurityPolicy#None alongside the encrypted policies.
  bool allow_unencrypted{false};

  ClientAuthentication authentication{ClientAuthentication::kAcceptAnyCertificate};
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

  /// What the server actually ended up doing about client certificates, which
  /// is not always what was asked for -- a rejected configuration reports the
  /// posture it refused to take.
  ClientAuthentication authentication{ClientAuthentication::kAcceptAnyCertificate};

  /// Certificates loaded from the trust list. Zero with kTrustList is a
  /// configuration error, not an empty allow-list.
  std::size_t trusted_certificate_count{0};

  /// Human-readable explanation for the startup log.
  std::string detail;
};

/// Configures `config` from `options`, with encryption if it is available.
///
/// The order of preference for the server's own identity is deliberate:
///
///   1. A certificate and key from the configured paths if both are set. This
///      is the only arrangement that gives the server a stable identity, so
///      clients that trusted it yesterday still trust it today.
///
///   2. A self-signed certificate generated now. Encryption works immediately
///      with nothing to provision, at the cost of a new identity on every
///      restart -- every client must re-trust it, which is exactly the friction
///      that teaches people to disable certificate checking. Fine for a
///      demonstration, not fine for a deployment.
///
///   3. Unencrypted, only if `allow_unencrypted` is set.
///
/// If encryption is compiled in and unencrypted operation is not permitted, a
/// server that cannot obtain a certificate **fails** rather than quietly
/// falling back. A silent downgrade to plaintext is the worst outcome
/// available: everything works, nothing complains, and the transport is
/// readable by anyone on the network.
///
/// Two configurations are refused rather than approximated, both for the same
/// reason -- they would produce a server that looks authenticated and is not:
///
///   * **kTrustList together with `allow_unencrypted`.** A SecurityPolicy#None
///     endpoint needs no certificate at all, so it is an unauthenticated way
///     around the trust list. Every client the trust list was meant to exclude
///     simply connects to the other endpoint instead.
///
///   * **kTrustList with an empty trust list.** Asking to check certificates
///     against nothing is a configuration mistake, and the two things it could
///     be taken to mean -- trust everyone, trust no one -- are the most
///     permissive and least useful readings available. It is refused so that
///     the operator finds out at startup rather than from a client that cannot
///     connect, or worse, from one that can.
///
/// Note what the trust list does and does not do. It authenticates the client
/// *application*, at the SecureChannel, before any session exists -- which is
/// real access control and is what an OPC UA deployment normally relies on. It
/// says nothing about *which user* is behind that application; anonymous user
/// tokens remain acceptable, and for a read-only diagnostic view of a safety
/// runtime that is defensible. It stops being defensible the moment anything
/// here is writable, which is why the address space has no writable node.
SecurityPosture configureSecurity(UA_ServerConfig* config,
                                  const SecurityOptions& options);

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

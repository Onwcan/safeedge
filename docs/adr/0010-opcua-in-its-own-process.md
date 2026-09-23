# ADR-0010: OPC UA lives in its own process

- **Status**: Accepted
- **Date**: 2026-08-28
- **Deciders**: Onur Can Urhan

## Context

OPC UA is the protocol an industrial cell is expected to speak outward. A
control system that cannot be read by a SCADA or MES stack is a control system
somebody has to write an adapter for.

The runtime already publishes a `RuntimeSnapshot` — safety state, jitter
percentiles, cycle counters, whether real-time scheduling was granted. The
question is only where the server that exposes it should run.

## Decision

A separate component, `safeedge::opcua`, and a separate binary,
`safeedge-opcua`. It reads the snapshot from shared memory and serves it. The
failsafe runtime links no part of it.

`SAFEEDGE_BUILD_OPCUA` is **off by default**, so the ordinary build does not
compile a protocol stack.

## Why not another thread in safeedged

**A protocol stack is a large body of code with a large attack surface.** OPC UA
brings a session layer, binary and JSON encoders, a certificate handler and a
subscription engine. Any of it failing — a parse bug on a malformed request, an
unbounded allocation, a certificate library calling `abort()` — must not be able
to take down a loop that is holding a machine in a safe state.

Putting it in another process makes that structural rather than aspirational.
The OPC UA server can crash, leak, be restarted or be killed by the OOM killer,
and the control loop does not notice. There is no shared heap, no shared
allocator, no signal disposition in common.

**It also preserves what safeedged's image is.** That binary is 1.51 MB on
`scratch`, with no shell and no userland at all — a property the container job
in CI verifies by unpacking the layers. Linking open62541 into it would end
that, and the minimal image is a real security property rather than a boast.

The snapshot travels through the `ipc` component, which is the first time that
component carries a real payload across a real process boundary rather than a
test's. The same seqlock serves both the in-process HTTP handlers and the
out-of-process reader, so there is one publish rather than two — a second
publish path would be a second thing to forget to update, and the two would
disagree the first time somebody added a field.

## What it costs, honestly

The OPC UA view can be stale, or absent, while the runtime is perfectly healthy.
Two processes means two lifetimes.

That is why `publishSnapshot` takes a `fresh` flag, and why a snapshot that
cannot be read is published as `BadNoData`.

## Two things that had to be got right

### The namespace index is not a constant

The first implementation hardcoded namespace index 1 and failed to build its
address space at all.

A namespace index is a per-server lookup into that server's namespace array.
Index 0 is always the OPC UA base namespace and index 1 is conventionally the
server's own application URI, so a custom namespace usually lands at 2 — and
"usually" is the whole problem. Add another namespace and it moves.

**The URI is the contract; the index is a lookup.** A client resolves
`urn:safeedge:runtime` at connect time and uses the result for that session.
That is the protocol's own answer, and it is the same shape as every other
contract in this portfolio: the name is the promise, the number is an
implementation detail. `ServerFixture.TheNamespaceIsResolvedNotAssumed`
deliberately asserts only that the index is non-zero — asserting a specific
value would enshrine exactly the assumption that failed.

NodeIds within the namespace are the opposite: numeric, fixed, and never reused,
for exactly the reasons `robot-contracts` gives about protobuf field numbers. A
client caches a NodeId. Renaming a browse name is cosmetic; renumbering is a
silent break.

### A bad status does not clear a value

The first implementation wrote `hasValue = false` alongside
`UA_STATUSCODE_BADNODATA`, on the assumption that this would leave a client with
nothing to read.

It does not. open62541 keeps the node's previous value, so a client reading the
value attribute still gets the last good one — a dead runtime's "torque
permitted", indefinitely. The status said `BadNoData` the whole time, and a
client that checked it would have been fine.

Which is precisely the kind of safety that depends on everyone being careful.

Both channels now carry the same message: the status says "I do not know", and
the value is the not-fresh snapshot, whose booleans are false and whose counters
are zero. A client that ignores the status still reads *torque not permitted*,
which is the direction it has to fail in.

This is the third time the same rule has appeared in this portfolio, in three
vocabularies: `UNSPECIFIED` at zero in the protobuf contracts, the writer
heartbeat on the shared-memory safety link, and now a StatusCode paired with a
fail-safe value. **The absence of information must never be representable as
permission.**

## Subscriptions, and who actually decides the latency

`safeedge-opcua-probe` subscribes to `SafetyTransitionMonotonicNanoseconds` and
subtracts it from its own clock, measuring the same quantity `pickcell` measures
over HTTP and shared memory. Three findings, each one a correction of something
that looked right.

**The first notification is not an event.** A subscription opens by reporting
the current value, whose timestamp is whenever the runtime last changed state —
possibly long before the client connected. The first run reported 7107 ms, which
was the age of the state and not a latency. The probe discards it.

**`UA_Client_run_iterate(client, timeout)` is the client's poll.** A pushed
notification sits in the socket until the client runs its event loop. The first
version passed 100 ms and reported a median near 88 ms regardless of every
server-side interval — it was measuring its own loop. A probe built to show that
subscriptions remove the client's poll was itself polling at 100 ms.

**Requesting an interval is not getting one.** This is the finding that matters.

open62541's stock configuration floors the publishing interval at 100 ms and
sampling at 50 ms. A client asking for 10 ms is given those instead;
`revisedPublishingInterval` says so, and a client that never reads it reports the
interval it wished for.

| request | granted | median |
|---|---|---:|
| 10 ms / 10 ms | 100 ms / 50 ms (stock limits) | **74.9 ms** |
| 10 ms / 10 ms | 10 ms / 10 ms (limits lowered) | **12.1 ms** |

Same client, same request, six times the difference — decided entirely by
`publishingIntervalLimits` on the server. So this server lowers both floors to
10 ms, and the probe prints any revision it receives.

That is the third protocol in this portfolio to need the same discipline, after
`pthread_setschedparam` returning success while granting nothing and `SCHED_FIFO`
being refused by an unprivileged container: **read back what you were granted,
not what you asked for.**

Lowering the floors is a real trade rather than free speed. A shorter publishing
interval is more wakeups and more packets per subscriber per second, and a server
facing many clients should not hand that out. For a read-only view of one safety
runtime on a cell network it is affordable — and the point is that it should be a
number somebody chose, which at 100 ms by default it was not.

With the limits granted, the sweep in `evidence/opcua-subscription-latency.txt`
behaves the way the mechanism predicts:

| server snapshot read | sampling | median |
|---:|---:|---:|
| 10 ms | 10 ms | 10.1 ms |
| 10 ms | 50 ms | 30.9 ms |
| 10 ms | 200 ms | 180.4 ms |
| 100 ms | 10 ms | 25.5 ms |

**A subscription is not inherently faster than a poll.** It removes the
*client's* poll. The server-side intervals remain, and they are somebody's
configuration rather than a property of the protocol.

## Security

The first version of this server ran anonymous and unencrypted and said so in a
startup warning. A warning is not a control, so the server now either has a
certificate or does not start.

**Encryption is on by default and plaintext is opt-in.** `SecurityPolicy#None`
is removed from the endpoint list unless `SAFEEDGE_OPCUA_INSECURE=1`. This
matters more than it sounds: open62541 adds `None` *alongside* the encrypted
policies, so a server configured "with encryption" happily serves plaintext to
any client that asks for it — and clients ask for it, because it works. Offering
it has to be a decision.

**A certificate is loaded if provided, generated if not.** Files named by
`SAFEEDGE_OPCUA_CERT` / `SAFEEDGE_OPCUA_KEY` give the server a stable identity;
without them it generates a self-signed certificate at startup. Generation is
logged as a *warning*, not an info line, because it means every client must
re-trust the server after each restart — and that friction is exactly what
teaches operators to disable certificate checking altogether.

There is no silent fallback to plaintext. If encryption is compiled in and a
certificate cannot be obtained, the server fails. A downgrade that leaves
everything working and the transport readable is the worst outcome available.

### mbedTLS, and two settings that are not optional

The backend is mbedTLS via FetchContent rather than OpenSSL, because it builds
from source with no system packages — which is what lets the component be built
on a machine with no install rights.

Two settings had to be forced, and neither error explains itself:

- `DISABLE_PACKAGE_CONFIG_AND_INSTALL` defaults to ON for a subproject, leaving
  mbedTLS's targets out of any export set. open62541's `install(EXPORT)` then
  fails with *"requires target mbedtls that is not in any export set"*, which
  does not obviously mean "switch mbedTLS's install rules back on".
- `MBEDTLS_FATAL_WARNINGS` is mbedTLS's own `-Werror`. GCC 15 added
  `-Wunterminated-string-initialization`, which fires inside mbedTLS 3.6.2, so
  the dependency will not compile on a new enough compiler.

The same class of problem in this project's direction: `UA_STRING_STATIC`
expands to C-style casts, which `-Wold-style-cast -Werror` rejects. The macro is
fine; it simply cannot be expanded in code held to that bar, so the two casts are
written out. Third-party headers are included as `SYSTEM` for the same reason —
holding somebody else's header to your warning bar has exactly one outcome, and
it is that the warning gets turned off globally.

### The failure with no symptom until it is fatal

The certificate's `subjectAltName` carried `URI:urn:safeedge:opcua`, while
open62541 defaults `applicationDescription.applicationUri` to
`urn:open62541.server.application`. OPC UA requires them to match: a certificate
is a claim to an identity, and a server presenting one has to actually claim
that identity.

What that mismatch looks like is the instructive part. The server started, built
its address space, logged `listening` — and then the event loop exited with
`BadCertificateUriInvalid`. Everything up to the fatal moment looked correct.
There is now one constant, `kApplicationUri`, used to build the certificate and
to set the application description, so the two cannot drift; a test asserts every
endpoint advertises it.

### Client authentication, and a claim this ADR previously got wrong

An earlier version of this document said client certificates were pointless here
because the server permits anonymous login, so validating one "authenticates
nobody". That is wrong, and the mistake is worth keeping visible because it is
easy to make: it conflates two different things OPC UA authenticates.

A trust list is checked at the **SecureChannel**, against the client's
*application instance certificate*, before any session exists. A client whose
certificate is not in it is refused with `BadCertificateUntrusted` and never gets
as far as offering a user token. Anonymous user tokens are about *which user* is
behind an application that has already been admitted. So the trust list does
authenticate — the application — and it does so whether or not anonymous user
tokens are accepted.

What actually makes a trust list meaningless is a **`SecurityPolicy#None`
endpoint next to it**. That endpoint needs no certificate at all, so every client
the trust list was meant to exclude simply connects to the other one, while the
server goes on reporting that it authenticates clients. That is the pairing that
produces "looks authenticated and is not", and it is now refused at startup
rather than documented as a caveat:

- `kTrustList` together with `allow_unencrypted` — refused, because the
  plaintext endpoint is a way around the list rather than a fallback beside it.
- `kTrustList` with an empty or unreadable trust list — refused, because
  checking against nothing has two possible readings, trust everyone and trust
  no one, and both are worse than saying so at startup.

Setting `SAFEEDGE_OPCUA_TRUSTLIST` is what asks for it; there is no separate
switch, because a switch without a directory and a directory without a switch
are both ways of ending up with neither. The startup log states the posture
either way, and the accept-any default is logged as a **warning** rather than an
info line — an operator who assumed otherwise should find out from the log
rather than from an intrusion.

`scripts/demonstrate-client-auth.sh` runs the real server against two real
clients, one in the trust list and one not, and checks the specific status code
rather than the word "certificate" — which also appears in the startup line and
would have made the check unfailable. It carries a negative control: the same
rejection must *not* appear when no trust list is configured.

### Server authentication in the probe

The probe requires `--trust-list DIR`, containing DER-encoded trusted server
certificates, and refuses missing or unusable trust material. Its channel must
use SignAndEncrypt. A successful connection reports its negotiated
SecurityPolicy, security mode and whether server certificate verification was
enabled. Trust should be provisioned before connecting, using a stable server
certificate supplied through `SAFEEDGE_OPCUA_CERT` / `SAFEEDGE_OPCUA_KEY`.

`--insecure-accept-any-server-cert` preserves the demonstration behavior as an
explicit opt-in. Only this path warns that any server certificate is accepted;
it still requires encryption. The local demonstration scripts select this path
because they generate temporary server identities. Connection tests exercise
both a trusted server and a server absent from the client's trust list.

### What this is still not

**User authentication.** Anonymous user tokens remain acceptable. For a
read-only diagnostic view there is nothing to write and nothing secret, and
turning anonymous off properly means supplying credentials, which is a
deployment decision this component does not get to invent. It stops being
defensible the moment anything here is writable, which is why the address space
has no writable node.

Encryption costs about 4 ms on the subscription path here — a median of 14.0 ms
against 10.1 ms unencrypted, on the same intervals. Small, real, and worth
knowing rather than assuming either way.

## Events, and what a subscription on a variable actually delivers

A client subscribed to a variable does not receive changes. It receives
**samples**: the server reads the node every `samplingInterval` and reports the
value when it differs from the one it read last time. A state that begins and
ends between two reads is not delayed and not merged — it is absent, and the
client has no way to learn it existed.

For a diagnostic gauge that is a fair trade. For "the machine tripped and
recovered" it is the difference between a log and a fiction, so the server also
raises a `SafetyTransitionEventType` event on every transition. Events are
queued when triggered rather than read on a timer.

Measured with both subscriptions in one client session against one server, so
the two see identical transitions —
`scripts/demonstrate-events-vs-sampling.sh`, twelve transitions in states
lasting 100 ms, client sampling at 500 ms:

| | delivered |
|---|---|
| events | **12 of 12** |
| data-change notifications | **4** |
| coalesced by the server | 0 |

The usual framing has this backwards. Events are described as a latency
optimisation; both subscriptions here share one publishing interval, so the
events arrive no sooner than the samples do. What differs is how many arrive at
all. **The difference is correctness, and the latency is the same.**

### Three limits, stated rather than implied

**An event queue overflows too.** It is lossless up to the depth the client
requested — this probe asks for 64 and discards the *newest*, because for a
safety log the first thing that went wrong is worth more than the most recent.
Lossless-up-to-a-depth is a weaker promise than lossless and a stronger one than
a sampler can make at any depth.

**The chain is only as event-driven as its most sampled link.** This server
reads its own snapshot on a poll, so a transition shorter than
`SAFEEDGE_OPCUA_PUBLISH_MS` never reaches OPC UA at all. That is not fixed by
using events; it is *upstream* of them.

**So that case is reported rather than hidden.** The snapshot carries a
transition counter. When it advances by more than one between reads, the server
knows how many transitions it coalesced even though it cannot recover them, and
puts the number in `MissedTransitions` instead of presenting the newest state as
though it were the only one. A client can then tell "I have the whole story"
from "I have the current state". It is the same argument as `BadNoData` on a
stale variable and `UNSPECIFIED`-at-zero in the protobuf contracts: the absence
of information must be representable, or it gets read as permission.

### Alarms & Conditions is not enabled

open62541 ships `UA_ENABLE_SUBSCRIPTIONS_ALARMS_CONDITIONS` as an experimental
option, and it would map neatly onto what this runtime already does — a
`ConditionType` with acknowledgement is very close to the e-stop latch that
requires an explicit acknowledge before torque returns. It is off. Plain events
derived from `BaseEventType` are what a generic client subscribes to without
knowing anything about this address space, and they are enough for the claim
being made here. Turning on an experimental branch of a protocol stack to model
a state machine that already works is a decision that needs a reason beyond
neatness.

### A namespace mistake worth keeping

The fields declared on this event type live in the server's own namespace. The
inherited ones — `Severity`, `Message`, and the rest of `BaseEventType` — live
in namespace 0. Writing an inherited field under the server's own index names a
property that does not exist, and the write fails: the event goes out without
the two fields a generic client actually reads, while every field this server
defined is present and correct. It is the same class of error as assuming a
namespace index instead of resolving it, and it fails in the same quiet way.

## Consequences

- `safeedged` is unchanged in size, dependencies and image contents.
- A fault anywhere in the OPC UA stack cannot reach the control loop.
- Building the component costs a couple of minutes of open62541, which is why it
  is off by default and has its own CI job.
- Two processes must both be deployed and both be running for the OPC UA view to
  exist. The compose stack starts both.
- **The channel is encrypted and the door can be locked; the users are not
  distinguished.** Encrypted policies are the default and `SecurityPolicy#None`
  has to be asked for. A trust list restricts which client applications may
  connect. User identity is still anonymous, which is appropriate for a
  read-only view and appropriate nowhere else; the daemon logs the posture it is
  in rather than leaving it to be inferred from a clean start.
- The server is read-only. Nothing in the address space is writable, and the
  runtime takes no input from it. A write path into a safety runtime is a much
  larger decision than an address space.

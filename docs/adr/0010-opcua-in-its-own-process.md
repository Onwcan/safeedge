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

### What this is not

**Client certificates are accepted without validation, deliberately.** What this
server protects is the *channel* — traffic cannot be read or altered by others on
the network. Client certificates would be about *authentication*, and the server
permits anonymous login, so validating one authenticates nobody. Rejecting a
self-signed client certificate while waving through an anonymous session is an
obstacle, not a control.

Real client authentication needs two things together: a trust list the operator
populates, and anonymous access turned off. Doing one without the other produces
a server that looks authenticated and is not. Neither is implemented, and the
startup log says which posture the server is actually in rather than leaving it
to be inferred from a clean start.

Encryption costs about 4 ms on the subscription path here — a median of 14.0 ms
against 10.1 ms unencrypted, on the same intervals. Small, real, and worth
knowing rather than assuming either way.

## Consequences

- `safeedged` is unchanged in size, dependencies and image contents.
- A fault anywhere in the OPC UA stack cannot reach the control loop.
- Building the component costs a couple of minutes of open62541, which is why it
  is off by default and has its own CI job.
- Two processes must both be deployed and both be running for the OPC UA view to
  exist. The compose stack starts both.
- **Security is not addressed.** The server accepts anonymous connections with
  no encryption, which is appropriate for a read-only view on a trusted cell
  network and appropriate nowhere else. The daemon logs that at startup rather
  than leaving it to be discovered. Making it safe to expose means certificates,
  an AccessControl plugin and a security policy above `None` — a separate piece
  of work, not a flag.
- The server is read-only. Nothing in the address space is writable, and the
  runtime takes no input from it. A write path into a safety runtime is a much
  larger decision than an address space.

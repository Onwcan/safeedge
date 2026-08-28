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

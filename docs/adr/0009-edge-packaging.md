# ADR-0009: Packaging the runtime as an industrial-edge application

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

An industrial edge device runs several applications on modest hardware,
delivered as container images and orchestrated by something the application
author does not control. The runtime has to survive that environment without
giving up the properties the previous eight ADRs were spent establishing.

## Decision 1: `scratch` base, statically linked

The runtime image contains **exactly one file**: the binary. No shell, no
package manager, no libc, no `/etc`. Measured size: **1.51 MB**.

An image with no userland cannot have a userland vulnerability. That is a
stronger property than keeping one patched, and it removes an entire recurring
operational burden from a component that will sit on a machine for years.

**The cost is real and worth stating.** There is nothing to `docker exec` into.
No shell, no `ps`, no `strace`. A team expecting to shell into a misbehaving
container will find this painful. It is the right trade here because the
diagnostics were designed for it — structured JSON logs and a metrics endpoint
carrying everything the runtime knows about itself — but a component without
that groundwork should not copy the base image and hope.

CI enforces the claim rather than repeating it: it unpacks the image layers and
fails if they contain anything but `safeedged`, and enforces an 8 MB budget so
that a debug tool added "just for now" fails the build instead of quietly
staying.

That check took two attempts. The first used `docker export`, which exports a
*container* and therefore includes files the runtime injects — `.dockerenv`,
`/etc/hosts`, `/etc/resolv.conf`, `/dev/console`. It would have failed on a
perfectly correct image. `docker save` and unpacking the layers is the right
tool: it inspects the image, which is the thing being claimed about.

## Decision 2: hardening, and the conflict it creates

The container runs as UID 65532, read-only root filesystem,
`no-new-privileges`, all capabilities dropped.

**Except one, and this is the interesting part.**

`SCHED_FIFO` requires `CAP_SYS_NICE`. A container that drops every capability
therefore **cannot do real-time scheduling** — it starts, serves metrics, passes
its healthcheck, and silently misses deadlines. Nothing looks broken.

Demonstrated with the same image:

| Run | `safeedge_realtime_scheduling_granted` |
|---|---|
| `--cap-drop=ALL` | **0** |
| `--cap-drop=ALL --cap-add=SYS_NICE --ulimit rtprio=99` | **1** |

Security hardening and real-time behaviour are in direct tension, and the
default advice — drop everything — silently costs the runtime the property it
exists for. The compose file adds `SYS_NICE` back deliberately, with the reason
written next to it, so that a future reader removing it understands what they
are removing.

The mitigation for getting it wrong is not a comment, though. It is
Decision 4.

## Decision 3: liveness and readiness are different questions

`/healthz` asks whether the process is functioning. `/readyz` asks whether it is
fit to be used. A latched safety fault makes it **not ready** but still
**healthy**.

That distinction is load-bearing. If liveness reported the safety state, an
orchestrator would restart the container every time the machine correctly
stopped — throwing away the fault information an engineer needs, and restarting
a machine whose safety supervisor had just decided it should not be running. The
correct behaviour is to withdraw traffic and leave it alone, which is exactly
what a failing readiness probe does.

## Decision 4: export the metric that says the metrics cannot be trusted

`safeedge_realtime_scheduling_granted` is the first metric the endpoint emits
and the first, largest panel on the Grafana dashboard.

Every latency figure alongside it is meaningless when it reads 0 — they describe
the host's load rather than the runtime. A dashboard showing microsecond jitter
without showing this would be *actively misleading*, and confidently so.

This is the same principle as the benchmark reporting its own measurement
conditions and the seqlock exporting whether the scrape got a clean read. A
number without the conditions it was gathered under is not evidence.

## Decision 5: an HTTP server rather than a dependency

Roughly two hundred lines serving three routes and answering only GET.

Taking a library would add something to scan, patch and license; grow an image
whose whole argument is that it contains nothing; and widen the attack surface
of a process that also runs a safety supervisor. The limits are documented
rather than hidden: no keep-alive, no chunked encoding, no TLS, one connection
at a time.

Reads are bounded in **size and time**, because a client that connects and sends
nothing must not be able to stall a process hosting a safety supervisor. The
accept loop polls rather than blocking, so `stop()` is observed within 200 ms
instead of waiting for the next connection.

## Decision 6: written to be PID 1

Two things follow from being PID 1 that most daemons ignore.

**No default signal dispositions.** SIGTERM does not terminate PID 1 unless a
handler is installed. Without one, every `docker stop` is a ten-second wait
followed by SIGKILL — and SIGKILL means the runtime never reaches a safe state.
Handlers are installed before anything else starts, so a SIGTERM arriving during
startup is still honoured. Measured: `docker stop` completes in **829 ms**
against a 10 s grace period, and CI fails if it exceeds 5 s.

**Orphan reaping.** This process forks nothing, so there is nothing to reap.
Stated explicitly because the reflex is to add an init shim, and adding one that
is not needed is its own kind of cargo cult.

**The healthcheck probes itself**, because a `scratch` image has no curl to do
it. `safeedged --healthcheck` connects to its own port and exits 0 or 1. The
alternative was to abandon the minimal base and pull in a userland to run one
HTTP GET.

## Decision 7: quantiles as a summary, not a histogram

The quantiles come from a fixed-bucket histogram inside the real-time loop.
Exporting bucket counts and letting Prometheus re-derive them would add error on
top of the bucketing error already present.

A summary also makes the limitation visible: summary quantiles cannot be
averaged across instances, and a dashboard that tries will be wrong. That is
true of this data however it is exported, so the format may as well say so.

## Consequences

**Positive**

- 1.51 MB image, verified to contain nothing but the binary.
- The real-time/capability conflict is visible in a metric rather than
  discovered from a machine that misses deadlines for no apparent reason.
- Diagnostics survive the absence of a shell, because they were designed to.
- Shutdown is prompt enough that the runtime always reaches a safe state.

**Negative**

- **No in-container debugging at all.** The honest cost of the base image.
- The metrics server is minimal and would need replacing if the endpoint ever
  had to do more than answer three GETs.
- Static linking means a libc security fix requires rebuilding rather than
  patching a shared library. For an image rebuilt from source on every change
  that is a fair trade; for one shipped and forgotten it would not be.
- The compose file's `SYS_NICE` grant widens the container's privileges. That is
  a deliberate, documented decision, not an oversight — but it is a real
  reduction in isolation and belongs on a reviewer's list.

**Not done**

Signed images and provenance attestation. An SBOM is generated in CI, which is
half the supply-chain story; signing and verifying it at deploy time is the
other half and is the obvious next step.

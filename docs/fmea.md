# FMEA — failure modes and effects analysis

What can go wrong in this runtime, what happens when it does, how it is
detected, and which requirement covers it. Every `REQ-` reference here is
validated by `scripts/traceability.py`, so a requirement renamed or deleted
breaks the build rather than leaving a dangling row.

## On the absence of RPN numbers

A conventional FMEA scores Severity, Occurrence and Detection on 1–10 scales and
multiplies them into a Risk Priority Number. There are none here, deliberately.

Occurrence scores require field data — failure rates for the specific hardware,
in the specific installation, over a meaningful period. This component has none.
Inventing the numbers would produce an RPN that looks like measurement and is
actually opinion dressed as arithmetic, and RPN is exactly the artefact people
stop interrogating once it has a number in it.

Severity is stated qualitatively because it genuinely is known: a failure either
can or cannot leave the machine moving when it should not. Detection is stated
as the mechanism, which is more useful to a reviewer than a score, because the
question they actually need answered is *how* it is caught.

**Severity key** — **Critical**: the machine may move when it should not.
**High**: the machine stops when it should not, or a fault goes unreported.
**Moderate**: degraded diagnostics or availability only.

---

## Communication

| # | Item | Failure mode | Effect | Detection | Mitigation | Severity | Requirements |
|---|---|---|---|---|---|---|---|
| C-01 | Transport | Bits corrupted in transit | Consumer acts on wrong setpoint | CRC-32/AUTOSAR over payload, status, sequence and untransmitted address | Telegram rejected, safe state latched | Critical | REQ-SAF-001, REQ-SAF-002 |
| C-02 | Transport | Telegram delivered twice | Consumer acts on a stale setpoint. CRC is valid — the data really was sent, just not now | Consecutive number equal to the last accepted | Rejected, latched | Critical | REQ-SAF-003 |
| C-03 | Transport | Telegrams reordered | Consumer acts on an older command after a newer one | Consecutive number older than last accepted | Rejected, latched | Critical | REQ-SAF-004 |
| C-04 | Transport | Telegram lost | Consumer continues on stale data | Gap in consecutive number | Rejected, latched, loss count recorded | Critical | REQ-SAF-005 |
| C-05 | Producer | Stops transmitting entirely | Consumer continues on stale data indefinitely. No gap is ever observed because nothing arrives | Watchdog on time since last valid telegram | Safe state latched | Critical | REQ-SAF-006 |
| C-06 | Transport | Telegram delayed past its useful life | Consumer acts on a command describing a world that has moved on. CRC and sequence are both valid | Watchdog checked on receipt as well as on poll | Rejected, latched | Critical | REQ-SAF-007 |
| C-07 | Network | Foreign traffic lands in the safety buffer | Consumer acts on data from an unrelated device | CRC seeded with untransmitted source address and parameter signature | Rejected, latched | Critical | REQ-SAF-008 |
| C-08 | Commissioning | Telegram delivered to the wrong consumer | A machine obeys another machine | Destination address in the CRC seed | Rejected, latched | Critical | REQ-SAF-009 |
| C-09 | Commissioning | The two ends configured with different safety parameters | Both ends agree on bytes, disagree on meaning | Parameter signature in the CRC seed | Every telegram rejected | Critical | REQ-SAF-008 |
| C-10 | Transport | Truncated or oversized frame delivered | Partial setpoint acted on | Length checked against mandatory fields, maximum, and the caller's buffer | Rejected as malformed; nothing written to the caller | Critical | REQ-SAF-010, REQ-SAF-011 |
| C-11 | Transceiver | Dead link presenting as all-zero data | An all-zeroes frame is mistaken for a valid one | Consecutive number zero is never emitted; CRC also fails | Rejected | Critical | REQ-SAF-016 |
| C-12 | Link | Intermittent fault — mostly good telegrams | Consumer rides through it, running on data that is only sometimes trustworthy | Any single fault latches | Recovery requires explicit acknowledgement | Critical | REQ-SAF-012, REQ-SAF-013 |
| C-13 | Counter | Consecutive number wraps after ~49 days at 1 kHz | Machine faults for no reason an operator can correlate | — | Expected value derived with the producer's zero-skipping rule | High | REQ-SAF-015 |
| C-14 | Consumer | Operator takes a long time to acknowledge | Immediate re-timeout on acknowledgement | — | Watchdog window restarts from the acknowledgement | High | REQ-SAF-014 |
| C-15 | Decoder | Malformed input causes out-of-bounds access | Undefined behaviour in the safety layer — a detectable fault becomes an undetectable one | ASan/UBSan in CI; 50 000-iteration mutation test; libFuzzer target | Bounds checked before any field is read | Critical | REQ-SAF-017 |

---

## Supervisor

| # | Item | Failure mode | Effect | Detection | Mitigation | Severity | Requirements |
|---|---|---|---|---|---|---|---|
| S-01 | Power-on | Supervisor starts in a state permitting motion | Machine moves before diagnostics complete | — | Constructed in self test; torque not permitted | Critical | REQ-SAF-020, REQ-SAF-021 |
| S-02 | Power-on | Host already requesting motion when diagnostics pass | Power-on to full speed in one evaluation | — | Self-test completion returns immediately in STO; requests take effect the following cycle | Critical | REQ-SAF-022 |
| S-03 | E-stop circuit | Button pressed | Motion must stop from any state | Input polled every cycle before any request is considered | Torque removed, fault latched | Critical | REQ-SAF-023 |
| S-04 | Safety channel | Communication lost | Commands can no longer be trusted | Unconditional demand, checked before requests | Torque removed | Critical | REQ-SAF-024 |
| S-05 | Executor | Deadlines missed | Supervisor is no longer evaluating in time | Unconditional demand on heartbeat | Torque removed | Critical | REQ-SAF-025 |
| S-06 | Host | Keeps requesting the function it is violating | Limit violation ridden through indefinitely | Monitors evaluated before requests within each cycle | Violation faults in the same cycle | Critical | REQ-SAF-026, REQ-SAF-033 |
| S-07 | Drive | Exceeds the limited speed | Personnel exposed to unsafe motion | SLS monitor, every cycle | Torque removed | Critical | REQ-SAF-026 |
| S-08 | Drive | Drifts out of a commanded standstill | Unexpected motion during an operator intervention | SOS position and speed monitors | Torque removed | Critical | REQ-SAF-027, REQ-SAF-028 |
| S-09 | Drive | Fails to decelerate within the stop time | Machine still moving after a commanded stop | SS1 time monitor | Torque removed, fault reported | Critical | REQ-SAF-029 |
| S-10 | Supervisor | Torque removed during a controlled stop | Load coasts. On a vertical axis it falls | — | SS1 maintains torque while decelerating | Critical | REQ-SAF-030 |
| S-11 | Host | Cancels a stop already in progress | Machine resumes mid-stop | — | No request can leave the stopping state | Critical | REQ-SAF-031 |
| S-12 | Drive | Leaves the permitted position band | Collision with fixed structure or personnel | SLP monitor, wherever motion is permitted | Torque removed | Critical | REQ-SAF-032 |
| S-13 | Operator | Presses acknowledge to silence an alarm | Machine restarts unexpectedly under someone's hand | — | Acknowledgement clears the latch but leaves STO; a separate enable is required | Critical | REQ-SAF-035 |
| S-14 | Operator | Acknowledges while the cause is still present | Fault cleared with the hazard still live | Unconditional demands re-checked before acknowledgement is honoured | Acknowledgement has no effect | Critical | REQ-SAF-034 |
| S-15 | Diagnostics | Follow-on violations overwrite the original cause | Technician sent to the wrong subsystem; real cause never found | — | First cause retained | High | REQ-SAF-036 |
| S-16 | Integration | Caller leaves an input field unpopulated | Machine runs on a default that permits motion | — | Every default is the restrictive value | Critical | REQ-SAF-037 |
| S-17 | Host | Requests a standstill hold while still moving | Reference captured mid-motion; faults a cycle later for an unrelated-looking reason | Speed checked at the point of request | Request refused and reported as invalid | High | REQ-SAF-039 |
| S-18 | Supervisor | Hidden state makes behaviour depend on history | Transition table stops describing the real machine; verification becomes meaningless | Determinism test drives two instances identically | No state beyond what is declared | High | REQ-SAF-038 |

---

## Dual channel

| # | Item | Failure mode | Effect | Detection | Mitigation | Severity | Requirements |
|---|---|---|---|---|---|---|---|
| D-01 | One channel | Fails towards permitting motion | Machine runs when it should not | Peer withholds permission | Combined output takes the safe direction on every field | Critical | REQ-SAF-040 |
| D-02 | One channel | Fails towards demanding a stop | Machine stops unnecessarily | — | Accepted: 1oo2 favours availability loss over hazard | High | REQ-SAF-040 |
| D-03 | One channel | Fails in a way it cannot itself detect | Silently carried by its peer until the peer also needs to act | Cross-comparison of channel states every cycle | Discrepancy fault | Critical | REQ-SAF-041 |
| D-04 | Sensors | Independent noise at a threshold boundary | Nuisance trips make the machine unusable | Disagreement duration measured against a tolerance | Fault only once disagreement outlasts the window | Moderate | REQ-SAF-042 |
| D-05 | Supervisor | Discrepancy masked at the output only | Channels keep believing they are running | — | Fault forced onto both channels | Critical | REQ-SAF-043 |
| D-06 | Supervisor | Discrepancy clears when the channels re-agree | Intermittent channel fault ridden through | — | Latch clears only once neither channel reports a fault | Critical | REQ-SAF-044 |
| D-07 | **Both channels** | **Shared systematic software defect** | **Both channels compute the same wrong answer, agree perfectly, and the cross-comparison reports everything is fine** | **None. This structure cannot detect it** | **None in this implementation.** Real 1oo2D uses diverse implementations; both channels here run identical code | **Critical** | — |

Row D-07 is the honest limit of this design and is listed without a covering
requirement on purpose, because there is no requirement it could be traced to.
Running the same function twice never detects a fault in that function. See
[ADR-0006](adr/0006-safety-supervisor-and-dual-channel.md).

---

## Real-time execution

| # | Item | Failure mode | Effect | Detection | Mitigation | Severity | Requirements |
|---|---|---|---|---|---|---|---|
| R-01 | Control loop | Allocation on the deadline path | Unbounded pause inside a 1 ms cycle; appears in the field, not in testing | Global `operator new` replacement aborts inside a guarded region | Guard linked in tests and CI | Critical | REQ-RT-001 |
| R-02 | Scheduling | Relative sleep instead of an absolute deadline | Cycle rate drifts permanently; every individual cycle looks fine | Drift test over 300 cycles | Deadlines derived from the previous deadline | High | REQ-RT-002 |
| R-03 | Control loop | Cycle overruns its period | Stale setpoints, or a burst of catch-up commands | Overrun and skipped-deadline counters | Skip to the next future deadline rather than emitting a burst | Critical | REQ-RT-003 |
| R-04 | Handoff | Lock or allocation between RT and non-RT threads | Priority inversion; deadline missed | TSan; allocation guard | Wait-free SPSC ring | Critical | REQ-RT-004 |
| R-05 | Deployment | Requested real-time scheduling silently not granted | Every latency figure is fiction; failure appears in the field | Policy, affinity and memory locking read back from the kernel | Discrepancy reported, not assumed away | High | REQ-RT-005 |
| R-06 | Measurement | Benchmarks quoted from a virtualised host | Tail figures describe the hypervisor, not the code | Benchmark detects virtualisation and pinning failure | Conditions printed above the numbers | Moderate | — |

---

## Inter-process transport

| # | Item | Failure mode | Effect | Detection | Mitigation | Severity | Requirements |
|---|---|---|---|---|---|---|---|
| T-01 | Startup | A crashed instance left its region name behind | New instance attaches to the previous instance's half-written state | Exclusive creation fails with EEXIST | Adoption is refused; reclaiming a stale name is an explicit act | Critical | REQ-IPC-001 |
| T-02 | Configuration | Peers disagree about region size | mmap maps past the end and the excess raises SIGBUS on first touch, far from the cause | Size read back before mapping | Open fails with a size error instead | High | REQ-IPC-002 |
| T-03 | Integration | Malformed region name | Confusing EINVAL from inside libc | Name validated against the POSIX rule first | Rejected with an error naming the actual rule | Moderate | REQ-IPC-003 |
| T-04 | Shutdown | Name removed while a peer still holds a mapping | Peer loses its data mid-operation | — | Mapping and name have independent lifetimes | High | REQ-IPC-004 |
| T-05 | Lifetime | Region moved between owners | Double munmap, or a leaked mapping | — | Move leaves the source owning nothing | High | REQ-IPC-005 |
| T-06 | Transport | Reader samples while the writer is publishing | Consumer acts on half of one value and half of the next | Sequence counter sampled before and after | Read retried; torn values never accepted | Critical | REQ-IPC-006 |
| T-07 | Peer | Writer process dies mid-update | Counter left odd forever; an unbounded reader spins inside its own control cycle | Retry budget | Read fails and is recoverable rather than hanging | Critical | REQ-IPC-007 |
| T-08 | Payload | A type whose all-zero state is not a valid value | Reader observing the region before the first write accepts a value the type can never produce | Caught by the torn-value test during development | Documented constraint: the zero value must be valid | High | REQ-IPC-006 |
| T-09 | **Peer** | **A buggy or hostile peer writes anywhere in the region** | **Arbitrary corruption of runtime state; shared memory provides no isolation whatsoever** | **None at this layer** | **None. Where the peer is not trusted, the black-channel CRC and sequence number apply exactly as they do over a network** | **Critical** | REQ-SAF-001, REQ-SAF-008 |

---

## Edge deployment

| # | Item | Failure mode | Effect | Detection | Mitigation | Severity | Requirements |
|---|---|---|---|---|---|---|---|
| E-01 | Container | SIGTERM ignored because the process is PID 1 and has no default disposition | Every stop takes the full grace period and ends in SIGKILL; the runtime never reaches a safe state | CI measures shutdown time | Handlers installed before anything else starts | Critical | REQ-EDGE-001 |
| E-02 | Orchestrator | Liveness probe treats a latched safety fault as unhealthy | Container restarted, fault information discarded, machine restarted without anyone diagnosing why it stopped | — | Liveness reflects the process; readiness reflects fitness. A latched fault withdraws traffic without a restart | Critical | REQ-EDGE-002 |
| E-03 | Operations | Dashboard shows microsecond jitter on a host that never granted real-time scheduling | Latency figures describe the host's load and are read as a property of the runtime | Scheduling status exported and shown first on the dashboard | Every latency figure is qualified by one gauge | High | REQ-EDGE-003 |
| E-04 | Metrics | Exporter cannot read a complete snapshot | Stale values served as current | Freshness flag exported | Reported rather than hidden | Moderate | REQ-EDGE-004 |
| E-05 | Image | Shell or package manager present in the runtime image | Userland vulnerabilities apply to a safety-adjacent runtime; an attacker who lands has tools | CI unpacks the layers and asserts exactly one file | `scratch` base, statically linked binary | High | REQ-EDGE-005 |
| E-06 | Deployment | Container runs as root with full capabilities | A compromise or bug reaches the host | CI runs it hardened and asserts the consequences | Non-root, read-only rootfs, all capabilities dropped bar one | High | REQ-EDGE-006 |
| E-07 | **Deployment** | **Full hardening removes CAP_SYS_NICE, so real-time scheduling is silently unavailable** | **The runtime starts, serves metrics and misses deadlines. Nothing looks broken** | **`safeedge_realtime_scheduling_granted` reads 0** | **The compose file adds SYS_NICE back deliberately and says why. The metric makes the alternative visible rather than silent** | **Critical** | REQ-EDGE-003, REQ-EDGE-006 |
| E-08 | Endpoint | A client connects and sends nothing | Server thread blocked; metrics and health unavailable for everyone | — | Bounded read size and timeout per connection | Moderate | REQ-EDGE-007 |
| E-09 | Device | Container consumes all memory or CPU on a shared edge device | Neighbouring applications starved | — | Explicit CPU and memory limits, bounded log rotation | High | — |

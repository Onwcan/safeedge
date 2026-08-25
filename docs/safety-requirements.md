# Safety requirements

The authoritative list. Every requirement here must be linked to implementing
code (`@satisfies`) and to at least one verifying test (`@verifies`), and
`scripts/traceability.py` fails the build if either link is missing.

**This is not a safety case.** These are the requirements this portfolio
component was built against, written in the style a real one would use. A
certifiable system needs a hazard analysis, a qualified implementation, an
assessed development process, and a notified body. What this demonstrates is
that requirements, design and verification can be kept mechanically connected.

Format is fixed because the tooling parses it: a level-3 heading of the form
`### REQ-<GROUP>-<NNN> — <title>`, followed by a `**Statement:**` line.

---

## Communication — the black channel

### REQ-SAF-001 — Corruption is detected

**Statement:** The receiver shall reject any telegram whose contents have been
altered in transit, and shall not deliver its payload.

### REQ-SAF-002 — Checksum strength is adequate for the telegram length

**Statement:** The protecting checksum shall guarantee detection of at least
five bit errors across the maximum telegram length.

### REQ-SAF-003 — Repetition is detected

**Statement:** The receiver shall reject a telegram bearing a consecutive number
it has already accepted.

### REQ-SAF-004 — Reordering is detected

**Statement:** The receiver shall reject a telegram bearing a consecutive number
older than the last accepted.

### REQ-SAF-005 — Loss is detected

**Statement:** The receiver shall reject a telegram whose consecutive number
skips one or more values, and shall record how many were missed.

### REQ-SAF-006 — Silence is detected

**Statement:** The receiver shall enter the safe state when no valid telegram
has been received within the configured watchdog period.

### REQ-SAF-007 — Late delivery is detected

**Statement:** The receiver shall reject a telegram that is otherwise valid but
arrives after the watchdog period has elapsed.

### REQ-SAF-008 — Foreign traffic is rejected

**Statement:** The receiver shall reject any telegram not produced by a sender
sharing its configured source address and parameter signature.

### REQ-SAF-009 — Misaddressed traffic is rejected

**Statement:** The receiver shall reject any telegram addressed to a different
destination.

### REQ-SAF-010 — Malformed frames are rejected

**Statement:** The receiver shall reject any frame shorter than the mandatory
fields, longer than the maximum telegram, or larger than the caller's payload
buffer.

### REQ-SAF-011 — No payload is delivered on rejection

**Statement:** When a telegram is rejected for any reason, the receiver shall
report a payload length of zero and shall not write to the caller's buffer.

### REQ-SAF-012 — Communication faults latch

**Statement:** Once the receiver has detected any transmission fault, it shall
remain in the safe state and reject subsequent telegrams regardless of their
validity, until explicitly acknowledged.

### REQ-SAF-013 — Recovery requires acknowledgement

**Statement:** The receiver shall leave the safe state only on an explicit
acknowledgement, and shall then resynchronise to the producer's sequence.

### REQ-SAF-014 — Acknowledgement restarts the watchdog

**Statement:** Acknowledgement shall restart the watchdog window, so that the
time taken to acknowledge does not itself cause an immediate timeout.

### REQ-SAF-015 — Sequence wraparound is handled

**Statement:** The receiver shall accept a correctly sequenced telegram stream
across the wraparound of the consecutive number counter.

### REQ-SAF-016 — Zero is never a valid consecutive number

**Statement:** The sender shall never emit a telegram bearing consecutive number
zero, so that an all-zeroes frame cannot be a well-formed telegram.

### REQ-SAF-017 — The decoder is robust to arbitrary input

**Statement:** No input, of any length or content, shall cause the receiver to
read outside its buffers, report a payload length inconsistent with the frame,
or fail to latch after a rejection.

---

## Supervisor — IEC 61800-5-2 safety functions

### REQ-SAF-020 — The machine powers on in a safe state

**Statement:** On construction the supervisor shall be in a state in which
torque is not permitted.

### REQ-SAF-021 — Diagnostics gate enabling

**Statement:** The supervisor shall not permit torque until power-on
diagnostics have reported success.

### REQ-SAF-022 — Passing diagnostics does not start motion

**Statement:** Completion of the self test shall place the supervisor in Safe
Torque Off, irrespective of any request present in the same cycle.

### REQ-SAF-023 — Emergency stop overrides everything

**Statement:** While the emergency stop input is asserted, the supervisor shall
remove torque permission from any state, regardless of the requested function.

### REQ-SAF-024 — Communication loss removes torque

**Statement:** While the safety communication channel is not healthy, the
supervisor shall remove torque permission from any state.

### REQ-SAF-025 — Watchdog loss removes torque

**Statement:** While the cyclic executor is not meeting its deadlines, the
supervisor shall remove torque permission from any state.

### REQ-SAF-026 — Safely Limited Speed is enforced

**Statement:** While SLS is active, the supervisor shall remove torque
permission if the measured speed exceeds the configured limit.

### REQ-SAF-027 — Safe Operating Stop detects position drift

**Statement:** While SOS is active, the supervisor shall remove torque
permission if the position departs from the captured reference by more than the
configured window, in either direction.

### REQ-SAF-028 — Safe Operating Stop detects motion

**Statement:** While SOS is active, the supervisor shall remove torque
permission if the measured speed exceeds the standstill threshold.

### REQ-SAF-029 — Safe Stop 1 is time-monitored

**Statement:** If the drive has not reached standstill within the configured
stop time, the supervisor shall remove torque permission and report a fault.

### REQ-SAF-030 — Safe Stop 1 maintains torque while decelerating

**Statement:** While a controlled stop is in progress, the supervisor shall
continue to permit torque so that the drive can decelerate under control.

### REQ-SAF-031 — Safe Stop 1 cannot be interrupted

**Statement:** Once a controlled stop has begun, no requested function shall
return the supervisor to a state permitting motion.

### REQ-SAF-032 — Safely Limited Position is enforced

**Statement:** Where position monitoring is enabled, the supervisor shall remove
torque permission if the position leaves the configured band while motion is
permitted.

### REQ-SAF-033 — Monitors are evaluated before requests

**Statement:** A limit violation shall take effect in the same cycle in which it
occurs, irrespective of the function requested in that cycle.

### REQ-SAF-034 — Supervisor faults latch

**Statement:** Once a fault has been detected, the supervisor shall remain in
Safe Torque Off until acknowledged, and acknowledgement shall have no effect
while the cause is still present.

### REQ-SAF-035 — Acknowledgement does not restart motion

**Statement:** Acknowledging a fault shall clear the latch but leave the
supervisor in Safe Torque Off; a subsequent, separate request is required before
motion is permitted.

### REQ-SAF-036 — The first fault cause is retained

**Statement:** Where several faults occur in succession, the supervisor shall
report the first, not the most recent.

### REQ-SAF-037 — Defaults are fail-safe

**Statement:** A default-constructed input set shall describe a machine
demanding a stop, and a default-constructed output set shall permit nothing.

### REQ-SAF-038 — Evaluation is deterministic

**Statement:** Two supervisors given identical configuration and identical input
sequences shall produce identical outputs at every cycle.

### REQ-SAF-039 — Safe Operating Stop is refused while moving

**Statement:** A request for SOS made while the measured speed exceeds the
standstill threshold shall be refused and reported as an invalid request.

---

## Dual channel — 1oo2D

### REQ-SAF-040 — Either channel can demand the safe reaction

**Statement:** The combined output shall permit torque, motion, a speed limit or
a released brake only where both channels permit it, irrespective of the order
in which the channels are combined.

### REQ-SAF-041 — Channel disagreement is detected

**Statement:** The supervisor shall detect any cycle in which the two channels
report different states.

### REQ-SAF-042 — Brief disagreement is tolerated

**Statement:** A disagreement shall become a fault only once it has persisted
longer than the configured tolerance.

### REQ-SAF-043 — A discrepancy fault is forced onto both channels

**Statement:** On a discrepancy fault, both channels shall be driven into a
latched safe state.

### REQ-SAF-044 — Discrepancy recovery requires both channels

**Statement:** The discrepancy latch shall clear only once neither channel
reports a fault; agreement alone shall not clear it.

---

## Real-time execution

### REQ-RT-001 — The real-time path does not allocate

**Statement:** No component invoked from within the cyclic executor's guarded
region shall invoke the allocator.

### REQ-RT-002 — Cyclic execution is drift-free

**Statement:** Cycle deadlines shall be derived from the previous deadline
rather than from the current time, so that wakeup delay does not accumulate.

### REQ-RT-003 — Deadline overruns are detected and counted

**Statement:** The executor shall count cycles that complete after their
successor was due, and shall count deadlines skipped as a result.

### REQ-RT-004 — Handoff to non-real-time threads is wait-free

**Statement:** Passing data from the real-time thread to a thread without a
deadline shall complete in a bounded number of instructions, without locks,
allocation, syscalls or unbounded retry.

### REQ-RT-005 — Real-time scheduling failures are reported

**Statement:** Where requested scheduling policy, CPU affinity or memory locking
does not take effect, the executor shall report the discrepancy rather than
proceed as though it had succeeded.

---

## Inter-process transport

### REQ-IPC-001 — Region creation never adopts an existing region

**Statement:** Creating a shared-memory region shall fail if the name already
exists, rather than attaching to whatever is there.

### REQ-IPC-002 — A region smaller than requested is rejected

**Statement:** Opening an existing region shall fail if it is smaller than the
requested size, rather than mapping beyond its end.

### REQ-IPC-003 — Malformed region names are rejected

**Statement:** A name that does not begin with a single leading slash, or that
contains a further slash, shall be rejected before any system call is made.

### REQ-IPC-004 — Mapping lifetime is independent of name lifetime

**Statement:** Removing a region's name shall not invalidate any existing
mapping, and destroying a mapping shall not remove the name.

### REQ-IPC-005 — Region ownership transfers exactly once

**Statement:** Moving a region shall transfer the mapping to the destination and
leave the source owning nothing, so that the mapping is released exactly once.

### REQ-IPC-006 — Readers never observe a partially written value

**Statement:** A value accepted by a seqlock reader shall be one complete
published value, never a mixture of two.

### REQ-IPC-007 — Reader retries are bounded

**Statement:** A seqlock read shall fail after a bounded number of attempts
rather than retry indefinitely, so that a writer which died mid-update cannot
stall a reader.

### REQ-IPC-008 — Publishing is wait-free and allocation-free

**Statement:** Publishing a value shall complete in a bounded number of
instructions, without locks, allocation or waiting for any reader.

### REQ-IPC-009 — Observed values never regress

**Statement:** Successive values accepted by a reader shall never move backwards
through the publication sequence.

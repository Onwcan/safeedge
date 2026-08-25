# ADR-0006: The IEC 61800-5-2 supervisor and the 1oo2D structure

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

The black channel of ADR-0005 delivers commands the runtime can trust. Deciding
what the machine is *allowed to do* with them is a separate problem, and the one
where getting it subtly wrong is most dangerous: a communication fault announces
itself, whereas a supervisor that permits motion one cycle too early does not.

IEC 61800-5-2 defines the safety functions for adjustable-speed drives.
Implemented here: **STO**, **SS1-t**, **SOS**, **SLS**, **SLP**. Deliberately
not implemented: SS2, SBC, SDI, SLI, SMS, SSM, SAR, SLA — each needs drive
hardware this does not model or a second monitored quantity. Naming what is
absent is not padding; a reviewer needs the boundary of what is claimed.

## Decision 1: one evaluation function with a fixed internal order

`evaluate()` is the only mutator, and the order inside it is fixed:

1. **Unconditional demands** — E-stop, communication loss, watchdog loss.
2. **Fault latch and acknowledgement.**
3. **Per-state monitors** — SLS speed, SOS deviation, SS1 time, SLP band.
4. **Requested transitions.**

The order is load-bearing at two points.

**Demands come first** so that no request, from any host, in any state, can keep
torque on when one of these is active. They are not inputs to be arbitrated;
they are conditions under which permission is not available to grant.

**Monitors run before requests** so that a host repeatedly requesting the
function it is currently violating cannot ride through the violation. Had the
order been reversed, a controller stuck re-asserting SLS while the axis
accelerated would keep re-entering SLS every cycle and never trip its own limit.
`MonitorsRunBeforeRequestsSoAViolationCannotBeRiddenThrough` pins this.

The machine holds no state beyond what is declared: the current state, the
latched fault, the SS1 entry timestamp, and the SOS reference position. That is
what makes the transition table in the tests meaningful — with hidden state, a
table proves nothing about the next run.

## Decision 2: requests are level-triggered

A safety function stays active only while it is still being requested. Dropping
to `kNone` means "no restriction requested" and releases it.

The opposite convention is equally defensible and the difference is sharp. Under
**edge triggering**, a host that falls silent leaves the machine restricted —
safe. Under **level triggering**, silence releases the restriction — not safe.

Level triggering was chosen because it matches how safety fieldbuses actually
work, and because it makes the request stream stateless and therefore
verifiable. But it moves the entire burden of detecting a silent host onto the
watchdog and the communication monitor. That is why `communication_ok` and
`heartbeat_ok` are unconditional demands rather than advisory inputs, and it is
the single most important consequence of this decision.

## Decision 3: acknowledgement clears a fault; it does not restart motion

After a fault, `acknowledge` clears the latch and the machine stays in STO. A
separate enable request is needed to move again.

Collapsing those two steps is how an acknowledgement button becomes a start
button. Someone presses it to silence an alarm and the axis moves. Keeping them
distinct costs one extra cycle and one extra operator action.

Acknowledgement is also refused while the cause is still present — the fault
clears only once every unconditional demand has gone, so releasing the E-stop is
a precondition rather than a formality.

## Decision 4: the first cause is retained, not the latest

Once torque is removed the axis decelerates, which reliably produces follow-on
violations — standstill deviation, speed limits crossed on the way down. If each
overwrote the last, the reported fault would be whatever happened most recently
rather than what actually went wrong, and a technician would be sent to the
wrong subsystem. `TheFirstCauseIsRetainedNotTheLatest` pins it.

## Decision 5: SS1 keeps torque on

During a controlled stop the drive needs torque in order to decelerate. Removing
it would let the load coast, which on a vertical axis means it falls — the
opposite of safe. SS1-t therefore holds torque, denies commanded motion, and
monitors the time to standstill. It always ends in STO; the only question is
whether that ending carries a fault.

## Decision 6: `SafetyState` is ordered from most to least restrictive

`kSafeTorqueOff = 0` through `kOperational = 5`. This makes "take the safer of
two states" a `std::min`, which is what the dual-channel combination does. The
ordering is asserted in its own test precisely because one line of production
code silently depends on it, and a reordering would be a silent safety
regression rather than a compile error.

## Decision 7: 1oo2D, and what it does not buy

**1oo2** — either channel alone is sufficient to demand the safe reaction.
Torque is permitted only if both channels permit it; every field of the combined
output takes the safe direction. A channel failing towards "stop" stops the
machine; a channel failing towards "run" is overruled by its peer.

**D** — the channels are cross-compared every cycle, so a channel that has
failed in a way it cannot itself detect becomes visible as a disagreement.
Without the diagnostics, a channel stuck permitting motion would be silently
carried by its peer until the day the peer also permitted motion, which is
exactly when nobody is watching.

**Disagreement is tolerated briefly.** Independent sensors have independent noise
and independent sampling instants, so at a threshold boundary the channels will
disagree for a cycle or two legitimately — 9.99 against 10.01 with a limit of 10.
Faulting on the first disagreement makes the machine unusable; never faulting
makes the diagnostics decorative. The tolerance window is where that judgement
lives, and it is configured rather than constant because the right value is a
property of the sensors. `Diagnostics::longest_disagreement_ns` exists so that a
window set too tight is visible before it becomes a nuisance trip.

Neither channel can detect a discrepancy on its own — by construction each is
blind to the other — so the supervisor forces the fault onto both via
`forceFault()`. Masking only the combined output would leave both channels
believing they were still running.

### The limitation that matters most

**Both channels run the same code.** Real 1oo2D uses diverse implementations —
different algorithms, often different processors, sometimes different teams —
because two identical implementations share identical systematic faults. A logic
error in `SafetyStateMachine` produces the same wrong answer in both channels,
they agree perfectly, and the cross-comparison reports everything is fine.

So this structure defends against **random hardware faults and divergent sensor
input**, which is a real and useful class. It does **not** defend against a
software defect, and running the same function twice never will. Saying so
plainly is more useful than implying a redundancy that is not there.

## Consequences

**Positive**

- Every state/request pair has a defined outcome, verified against a table
  written by hand from the specification rather than generated from the code —
  a generated table would prove only self-consistency.
- Every default is fail-safe: a default-constructed `SafetyInputs` describes a
  machine with the E-stop pressed and no communication, and a default
  `SafetyOutputs` permits nothing. A caller who forgets a field gets a stop.
- Allocation-free and deterministic, so it runs inside the executor's
  `NoAllocScope` — verified, not assumed.

**Negative**

- **Not certified, and this is not a safety case.** A real installation needs a
  qualified implementation, an assessed development process, and a notified
  body. What this demonstrates is that the functions, their interactions and
  their failure modes are understood.
- No diverse channel implementation, as above.
- SS1 is time-monitored only (SS1-t). Ramp- and deceleration-monitored variants
  need a modelled deceleration profile to compare against.
- The discrepancy tolerance is a single global window. A real system would want
  per-signal tolerances, since position and speed have very different noise
  characteristics.

**Verification**

55 tests across the supervisor and the dual-channel structure: the full
state/request cross product, every unconditional demand from every state,
latching and acknowledgement, each monitor at and beyond its limit, SS1
completion and timeout, the 1oo2 combination in both channel orders, discrepancy
detection with and without the tolerance, and a realistic sensor-noise scenario
that must *not* fault.

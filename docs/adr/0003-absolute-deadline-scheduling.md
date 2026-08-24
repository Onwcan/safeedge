# ADR-0003: Absolute deadlines, and what to do when one is missed

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

The executor must invoke a callback at a fixed rate — 250 Hz or 1 kHz — for as
long as the machine is running. Two questions have to be answered before any of
it works: how the loop waits, and what it does when a cycle finishes after its
successor was already due.

Both have an obvious answer that is wrong.

## Decision 1: absolute deadlines, not relative sleeps

### The obvious approach

```cpp
for (;;) {
    doWork();
    std::this_thread::sleep_for(period);   // wrong
}
```

Every iteration restarts its countdown from whenever the thread happened to
wake up, so the actual interval is `period + wakeup_delay + work_duration`.
Each of those delays is added to the schedule **permanently**.

The numbers are not marginal. On the host this was developed on, measured
wakeup delay at 1 kHz has a median of **78 µs**. A relative-sleep loop would
therefore run at roughly 928 Hz rather than 1000 Hz, and after one minute would
be about 4.7 seconds behind where it should be. Nothing in the loop reports an
error while this happens; every cycle looks fine locally. The failure is only
visible as a machine that gradually falls out of sync with everything it
coordinates with.

### What is done instead

Each deadline is computed by adding the period to the **previous deadline**,
and the wait is `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`.

```cpp
deadline += period;
sleepUntil(deadline);
```

A late wakeup is then absorbed by the next interval instead of accumulating: it
appears as jitter in the histogram, which is a measurement, rather than as
drift, which is a defect. The deadline sequence is exact by construction because
nothing downstream ever reads the clock to decide when the next cycle is due.

`CLOCK_MONOTONIC` rather than `CLOCK_REALTIME`: the latter jumps when NTP steps
the clock or an operator changes the timezone, and a control loop that skips
forward an hour because a time sync landed is a safety event.

`EINTR` is retried rather than treated as a wakeup. A signal arriving mid-wait
would otherwise make the cycle run *early*, which reads as negative jitter and
puts a value in the histogram that is not a latency at all.

`CyclicExecutorTest.DeadlinesDoNotDriftAcrossManyCycles` pins this.

## Decision 2: skip missed deadlines by default

When a callback finishes after the next deadline has already passed, there are
two defensible policies.

### `kRunLate` — never drop a cycle

Run every iteration regardless of lateness. Nothing is lost, but the loop drifts
behind real time for as long as the overload lasts.

Correct when the callback is an integrator or a counter whose every step
matters, and where being late is preferable to being incomplete.

### `kSkipMissed` — jump to the next future deadline (default)

Advance past every missed deadline, counting them, and resume on the next one
that is still ahead.

This is the default because of what the alternative does to a machine. Catching
up means running four cycles back to back with no wait between them, which
emits four setpoints computed from stale sensor data as fast as the bus will
carry them. For a robot, a burst of stale setpoints is worse than a clean gap:
the gap is visible to the safety supervisor and can be responded to, while the
burst looks like a legitimate — and violently discontinuous — command sequence.

Skipped deadlines are counted in `ExecutorStats::skipped_cycles`. They are a
reportable event, never a silent recovery.

## Consequences

**Positive**

- Long-run cadence is exact regardless of platform jitter.
- Overload degrades into a countable, observable gap rather than into drift or
  a setpoint burst.
- `cycle_total` (deadline to callback return) is measured directly rather than
  inferred by adding jitter and execution time, so a correlation between those
  two cannot hide inside the sum.

**Negative**

- `kSkipMissed` means `cycles_executed` is not `elapsed / period`. Any consumer
  of the statistics must account for `skipped_cycles`, and that is an easy thing
  to forget. Mitigated by naming the field for what it is rather than folding it
  into a single "cycles" number.
- Absolute-deadline sleeps are Linux-specific in this form. Acceptable — the
  target is industrial edge Linux — and confined to one function.

**Not addressed here**

What the *system* should do when deadlines are being missed persistently. This
ADR covers only how the executor accounts for it. The policy question — how many
missed cycles in what window should trigger a transition to a safe state —
belongs to the safety supervisor in WP-09, and needs a requirement behind it
rather than a default.

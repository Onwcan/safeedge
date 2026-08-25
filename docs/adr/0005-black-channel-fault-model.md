# ADR-0005: A black-channel safety layer, and the fault model it defends

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

The runtime receives motion commands over a transport it does not control:
Ethernet, a backplane, shared memory, eventually a network with switches and
other traffic on it. Acting on a command that is corrupted, stale, duplicated,
misdelivered or forged is a safety event, not a data-integrity inconvenience.

The obvious response — certify the transport — is the wrong one. It makes every
switch, cable and driver in the path a safety component, which is
extraordinarily expensive and rules out standard networking hardware entirely.

## The black channel

The alternative, and what PROFIsafe, openSAFETY and FSoE all do, is to assume
the transport is **entirely untrustworthy and give it no safety responsibility
at all**. Every defence lives in a thin layer at each endpoint. The transport
becomes a "black channel": opaque, unqualified, and free to be ordinary
commercial equipment, because nothing about the safety argument depends on it
behaving.

That is the architecture implemented here.

**This is PROFIsafe-inspired, not PROFIsafe.** It borrows the mechanisms —
consecutive number, CRC seeded with untransmitted parameters, watchdog, latched
safe state — because they are the right mechanisms and are publicly documented
in IEC 61784-3. It is not conformant, not certified, and not interoperable with
a real PROFIsafe device. Saying otherwise would be the single most dangerous
sentence in this repository.

## The fault model, and the defence for each

IEC 61784-3 enumerates the ways a message can go wrong. Each row below is
implemented and has a test that injects that specific fault:

| Fault | What it looks like | Defence |
|---|---|---|
| **Corruption** | Bits flipped in transit | CRC-32/AUTOSAR, HD=6 |
| **Unintended repetition** | A valid telegram delivered twice | Consecutive number |
| **Incorrect sequence** | Telegrams arriving out of order | Consecutive number |
| **Loss** | A telegram never arrives | Sequence gap, plus watchdog if all traffic stops |
| **Unacceptable delay** | Arrives intact and in order, but too late | Watchdog |
| **Insertion** | A foreign message enters the stream | Source address folded into the CRC, never transmitted |
| **Masquerade** | Non-safety traffic that looks like a telegram | Same |
| **Addressing error** | A real telegram delivered to the wrong consumer | Destination address folded into the CRC |

Three of these are worth dwelling on, because they are the ones a CRC alone
gives no protection against and are therefore the ones a home-grown protocol
usually misses.

**Repetition.** A replayed telegram is byte-for-byte valid. The checksum is
correct because the data is genuinely what the producer sent — just not when it
sent it. A stale setpoint acted on confidently is exactly as dangerous as a
corrupted one, and only the consecutive number sees it.

**Unacceptable delay.** Authentic, correctly ordered, intact — and describing a
world that has moved on. Neither the CRC nor the sequence number can see this;
only a clock can.

**Masquerade and misaddressing.** Both look like perfectly good frames. The
defence is that the CRC is seeded with the source address, destination address
and a parameter signature that are **never put on the wire**. Anything that does
not already share those values cannot produce a matching checksum. That single
decision turns an integrity check into an authenticity check and covers three
fault classes at once.

That mechanism is emphatically **not cryptography**. The address parameters are
commissioning data, not secrets. It defends against faults and mistakes, not
against an adversary who can read the configuration. A safety layer described
as providing security tends to get deployed as though it does.

## Why CRC-32/AUTOSAR rather than the familiar CRC-32

The telegram is tens of bytes. What matters at that length is Hamming distance:
how many bit errors can occur before corruption produces a valid checksum.

The classic polynomial (0x04C11DB7, as used by Ethernet and zip) drops to
**HD=4 above 91 bits** — four flips can slip through. 0xF4ACFB13 was selected by
Koopman for short messages and holds **HD=6 up to 2048 bits**, so five errors
are still guaranteed detected across any telegram this will ever carry.

For a checksum whose failure mode is "the machine acts on corrupted motion
data", two extra guaranteed bits is not marginal. It is also what AUTOSAR
standardised for this class of use, so the choice is defensible without
re-deriving the argument in review.

The tests verify this rather than assert it: exhaustive detection of all single-
and double-bit errors, heavy sampling at three, four and five bits, and every
burst up to 32 bits. The standard check vector (`0x1697D06A` for "123456789")
is asserted so the implementation is identifiably that algorithm and not
something resembling it — a device at the other end will use the specified one
and nothing else.

## Latching, and why recovery is deliberately inconvenient

Any detected fault latches a safe state. Subsequent telegrams are refused —
`receive()` hands over no payload at all — until an explicit acknowledgement.

The alternative, recovering on the next good telegram, fails in exactly the case
that matters. Real faults are usually intermittent: a loose connector, a failing
transceiver, a switch dropping frames under load. Those produce good telegrams
most of the time. A self-recovering consumer rides through them indefinitely,
and the machine keeps running on data that is only sometimes trustworthy, with
nothing in any log to say so.

`BlackChannel.StaysInSafeStateEvenWhenGoodTelegramsResume` pins this: after one
corrupt frame, seventeen consecutive valid ones are refused.

The acknowledgement restarts the watchdog window rather than resuming it,
because otherwise the consumer would immediately time out again on the strength
of how long the operator took to walk to the panel.

## A bug this design produced, and the fix

The producer skips consecutive number 0, so that an all-zeroes frame — a dead
transport, an uninitialised buffer, a disconnected transceiver — can never be
mistaken for a legitimate telegram.

The first implementation of the consumer required `received == last + 1`. Those
two rules are incompatible exactly once per wrap: the producer emits
`0xFFFFFFFF` then `1`, the consumer computes an expected value of `0`, sees a
gap of two, and latches.

At 1 kHz the counter wraps about every 49 days. A machine would therefore drop
to a safe state roughly every seven weeks of uptime, with no correlating event
anyone at the plant could reproduce — the worst possible shape for a defect.

The consumer now derives the expected value with the same zero-skipping rule the
producer uses. `BlackChannel.SequenceSurvivesWraparound` starts the sequence
three below the boundary and drives it across, and would have caught this before
it ever reached hardware. It did, in fact, catch it: the test was written first
and failed.

## Consequences

**Positive**

- Standard, uncertified transport hardware can carry safety traffic.
- The fault model is enumerated in a table in the tests, so adding a fault type
  without a defence fails the build rather than quietly widening the gap.
- The whole layer is allocation-free and deterministic, so it can run inside the
  cyclic executor's `NoAllocScope` — verified, not assumed.

**Negative**

- **Not certified, and nothing here should be read as a safety case.** A real
  installation needs a qualified implementation, an assessed development
  process, and a notified body. What this demonstrates is that the mechanisms
  and their failure modes are understood.
- The consecutive number is unidirectional. Real PROFIsafe uses a virtual
  counter driven by the host and echoed by the device, which additionally
  detects a stalled producer that is still emitting well-formed frames. That is
  a genuine gap in coverage, not a simplification with equivalent effect.
- Latching means a transient fault stops the machine and requires human
  intervention. That is the intended trade, but it makes the watchdog timeout a
  parameter with real operational cost if set too tight.

**Verification**

45 tests across the CRC and the channel, including every fault in the model, a
50 000-iteration mutation test asserting the decoder cannot be made to misbehave
by any input, and a libFuzzer target with a seed corpus for deeper coverage
under Clang. The seed corpus is generated by an independent Python
implementation of the CRC, which agrees with the C++ one on the standard check
vector — two implementations reaching the same answer is worth more than one
implementation agreeing with itself.

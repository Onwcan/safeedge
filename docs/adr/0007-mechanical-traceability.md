# ADR-0007: Requirements traceability is generated, not maintained

- **Status**: Accepted
- **Date**: 2026-08-25
- **Deciders**: Onur Can Urhan

## Context

Safety-related development is expected to demonstrate that every requirement is
implemented and verified, and to show *where*. IEC 61508 and its sector
derivatives all want the same artefact: a matrix linking requirement → design →
verification.

The default way this is produced is a spreadsheet, filled in by hand, updated at
milestones. That artefact is accurate on the day it is written and decays from
then on. Tests get renamed, requirements get reworded, code moves between files,
and nothing in the build notices. By the time anyone audits it, the matrix
describes a system that no longer exists — while looking exactly as authoritative
as it did on day one.

That decay is not a discipline problem to be solved with more diligence. It is a
structural consequence of keeping the evidence somewhere the build cannot see.

## Decision

Requirements live in `docs/safety-requirements.md`. Links live in the source as
annotations:

```cpp
// @satisfies REQ-SAF-023      in include/ or src/
// @verifies  REQ-SAF-023      in tests/ or fuzz/
```

`scripts/traceability.py` parses both, generates `docs/traceability-matrix.md`,
and **fails the build** if:

1. a requirement has no `@satisfies` link;
2. a requirement has no `@verifies` link;
3. an annotation names a requirement that does not exist;
4. an FMEA row references a requirement that does not exist;
5. the committed matrix is out of date with respect to the source.

Deleting a test breaks the build. Renaming a requirement breaks the build.
Adding a requirement without implementing it breaks the build.

### Check 3 is the one that matters

Without it, the tool is capable of lying. An annotation reading
`@verifies REQ-SAF-O23` — letter O instead of zero — attaches to nothing. The
requirement it was meant to cover shows as unverified, which *would* be caught;
but a subtler variant, where the typo happens to match a different real
requirement, produces a matrix showing coverage that does not exist while every
gate passes.

A traceability tool that cannot detect its own broken links is worse than having
no tool, because it manufactures confident evidence for a claim nobody has
checked. The unknown-id check is what makes the other four trustworthy.

The negative case is exercised rather than assumed: introducing
`REQ-SAF-O43` produces two failures — the unresolved reference at its exact
line, and the requirement it abandoned.

### The matrix is committed, not just generated

The generated matrix is checked in, and CI verifies it is current. Two reasons.

It is the artefact a reviewer actually opens — a matrix that exists only inside
a CI run is invisible to anyone browsing the repository. And committing it makes
coverage changes visible in a diff: a pull request that removes the last test
covering a requirement shows that removal in the matrix, in review, next to the
change that caused it.

The staleness check is what makes committing it safe. Without it, a committed
generated file is just another artefact that decays.

## Consequences

**Positive**

- The matrix cannot silently become wrong; the failure mode is a red build
  rather than a misleading document.
- Coverage is visible in review, in the diff, at the moment it changes.
- Writing the requirements first surfaced two gaps in what had been built —
  neither was a defect, but neither had a test asserting it directly either.
- The FMEA is held to the same standard, so a renamed requirement cannot leave a
  row citing a hazard as covered when it no longer is.

**Negative**

- **47 requirements is a small number**, and they were written after most of the
  code. That inverts the intended order: requirements are supposed to constrain
  the design, not describe it afterwards. What this demonstrates is the
  mechanism, not a requirements-first process — and the two are different
  claims.
- Annotations are comments, so they carry no compiler checking of their own.
  A `@satisfies` can sit next to code that does not in fact satisfy anything, and
  no tool here would notice. The link is a claim by the author; only review
  checks whether it is true.
- Coverage is binary — linked or not linked. It says nothing about whether a
  test is *adequate*. One weak assertion satisfies the gate exactly as well as
  an exhaustive suite.
- Requirements are written in prose. Machine-checkable requirements would need a
  formal specification language, which is an enormous step up in cost and is the
  right call only at a much higher integrity level.

**Not claimed**

This is not a safety case and not a certification artefact. A certifiable system
needs hazard analysis driving the requirements, a qualified toolchain, an
assessed development process, independent verification, and a notified body.
What is demonstrated here is that requirements, design and verification can be
kept mechanically connected so the connection cannot rot unnoticed — which is
the part most often done badly, and the part a spreadsheet does worst.

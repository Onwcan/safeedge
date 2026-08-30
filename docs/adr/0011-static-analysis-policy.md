# ADR-0011: Keep the static-analysis gate strict and actionable

**Status:** Accepted

## Context

The first CI workflow failed in its formatting step. The attempted correction
then introduced invalid workflow YAML, so GitHub rejected every later run before
creating a job. As a result, the repository's original `.clang-tidy` profile had
never completed against the code it was supposed to gate.

Running that profile with the pinned Clang 18 toolchain produced 228 errors. Most
were not undiscovered defects: 73 were `misc-include-cleaner`, and 102 were C++
Core Guidelines checks rejecting the deliberate POSIX boundary used for sockets,
signals, fixed buffers, shared memory, and allocation interposition. Treating all
of those as release-blocking while the code had never passed them made the gate
aspirational rather than real.

Disabling clang-tidy or making its output advisory would give a green build by
removing the check. Mass-refactoring low-level safety code to satisfy style rules
in the same change would create a much larger behavioural review surface than
the CI repair itself.

## Decision

Clang-tidy remains a required gate, pinned to Clang 18, and every enabled finding
remains an error. CI obtains its input files from CMake's
`compile_commands.json`, restricted to first-party files under `src/`. This is
important for optional components: a source file has no valid compilation
command when the target that owns it is disabled.

The correctness-oriented families remain enabled:

- `bugprone-*` and `clang-analyzer-*`;
- `cert-*` and `concurrency-*`;
- `performance-*` and `portability-*`;
- the applicable `modernize-*`, `readability-*`, and naming checks.

The following checks are excluded deliberately:

- Raw-pointer, C-array, vararg, `malloc`, and ownership checks from
  `cppcoreguidelines-*`. These constructs are confined to reviewed boundaries
  where POSIX requires them, or to the allocation guard that must interpose the
  allocation functions themselves.
- `cppcoreguidelines-avoid-non-const-global-variables`, because a
  `volatile sig_atomic_t` flag is the standard communication mechanism available
  to this process's signal handler.
- `misc-include-cleaner`. Fixing the existing transitive-include debt is useful,
  but it is a mechanical change across most translation units and belongs in a
  separate review.
- The duplicate C-array modernization check and the named-parameter check. Fixed
  byte buffers and unnamed C callback parameters are intentional at the system
  boundary.
- Cognitive-complexity. The safety state machine's explicit evaluation order is
  audited and exhaustively tested; splitting it merely to lower a score would
  scatter that order across helper functions.
- Magic-number, identifier-length, trailing-return-type, and non-private-data
  rules already excluded by the original profile. Those are project style
  choices, not correctness findings.

No blanket exclusion is made for CERT or concurrency findings. Signal handler
installation is checked, error formatting no longer relies on `strerror`'s
shared storage where concurrent access is possible, and the single `getenv`
suppression is attached to a helper whose callers snapshot all environment
configuration before any worker thread starts.

The one `readability-make-member-function-const` suppression preserves the
installed `CyclicExecutor` ABI: adding `const` to that public member changes its
mangled C++ symbol and would break already-built downstream programs.

The newly reachable bug-prone findings were reviewed rather than suppressed:
percentile rank selection now uses a ceiling as its documented contract states,
the allocation diagnostic is explicitly terminated after `memcpy`, and the
remaining branch and integer-width findings were corrected without changing the
runtime's decisions.

## Consequences

- A green static-analysis job now means every enabled rule passed; there is no
  warning baseline and no advisory-only output.
- The gate checks only sources CMake can actually compile in that configuration.
  The OPC UA job separately compiles its optional sources with the project's
  compiler warnings as errors and runs their tests.
- Adding a new exclusion requires updating this decision. Existing include
  hygiene and POSIX-boundary style debt cannot silently expand the exclusion
  list.
- Include cleanup remains useful follow-up work, but it can be reviewed as a
  mechanical change rather than mixed into a workflow repair.

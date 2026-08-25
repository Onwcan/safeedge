#!/usr/bin/env python3
"""Builds and checks the requirements traceability matrix.

Every requirement in docs/safety-requirements.md must be linked to implementing
code and to at least one verifying test. The links are source annotations:

    // @satisfies REQ-SAF-023      in include/ or src/
    // @verifies  REQ-SAF-023      in tests/ or fuzz/

The point of generating this rather than maintaining it by hand is that a
hand-maintained matrix is accurate on the day it is written and wrong forever
afterwards. Deleting a test here breaks the build; renaming a requirement breaks
the build; inventing a requirement id in an annotation breaks the build.

That last check is the one that matters most. Without it, a typo such as
`REQ-SAF-O23` (letter O for zero) silently verifies nothing at all, the gate
still passes, and the matrix reports coverage that does not exist. A traceability
tool that cannot detect its own broken links is worse than no tool, because it
produces confident evidence for a claim nobody checked.

Usage:
    scripts/traceability.py            # regenerate docs/traceability-matrix.md
    scripts/traceability.py --check    # verify links and that the matrix is current
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
REQUIREMENTS_FILE = REPO_ROOT / "docs" / "safety-requirements.md"
MATRIX_FILE = REPO_ROOT / "docs" / "traceability-matrix.md"
FMEA_FILE = REPO_ROOT / "docs" / "fmea.md"

IMPLEMENTATION_DIRS = ("include", "src")
VERIFICATION_DIRS = ("tests", "fuzz")
SOURCE_SUFFIXES = (".hpp", ".cpp", ".h", ".cc")

REQUIREMENT_HEADING = re.compile(r"^###\s+(REQ-[A-Z]+-\d+)\s+[-—]+\s+(.+?)\s*$")
STATEMENT_LINE = re.compile(r"^\*\*Statement:\*\*\s*(.*)$")
ANNOTATION = re.compile(r"@(satisfies|verifies)\s+(REQ-[A-Za-z0-9-]+)")
# Matches the enclosing gtest TEST/TEST_F so a verification link can name the
# test rather than a bare line number.
TEST_DECLARATION = re.compile(r"^\s*TEST(?:_F)?\(\s*(\w+)\s*,\s*(\w+)\s*\)")


@dataclass
class Requirement:
    ident: str
    title: str
    statement: str
    satisfied_by: list[str] = field(default_factory=list)
    verified_by: list[str] = field(default_factory=list)


@dataclass
class Annotation:
    kind: str
    ident: str
    location: str
    label: str


def parse_requirements(path: Path) -> list[Requirement]:
    if not path.is_file():
        sys.exit(f"error: requirements file not found: {path}")

    requirements: list[Requirement] = []
    current: Requirement | None = None
    seen: set[str] = set()

    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        heading = REQUIREMENT_HEADING.match(raw)
        if heading:
            ident, title = heading.group(1), heading.group(2)
            if ident in seen:
                sys.exit(f"error: {path.name}:{number}: duplicate requirement {ident}")
            seen.add(ident)
            current = Requirement(ident=ident, title=title, statement="")
            requirements.append(current)
            continue

        statement = STATEMENT_LINE.match(raw)
        if statement and current is not None and not current.statement:
            current.statement = statement.group(1).strip()

    for requirement in requirements:
        if not requirement.statement:
            sys.exit(f"error: {requirement.ident} has no **Statement:** line")

    return requirements


def source_files(directories: tuple[str, ...]) -> list[Path]:
    found: list[Path] = []
    for directory in directories:
        base = REPO_ROOT / directory
        if not base.is_dir():
            continue
        for suffix in SOURCE_SUFFIXES:
            found.extend(sorted(base.rglob(f"*{suffix}")))
    return found


def enclosing_test(lines: list[str], index: int) -> str | None:
    """Nearest TEST/TEST_F at or above `index`, so a link names the test."""
    for candidate in range(index, -1, -1):
        match = TEST_DECLARATION.match(lines[candidate])
        if match:
            return f"{match.group(1)}.{match.group(2)}"
    return None


def collect_annotations(directories: tuple[str, ...], kind: str) -> list[Annotation]:
    annotations: list[Annotation] = []
    for path in source_files(directories):
        lines = path.read_text(encoding="utf-8").splitlines()
        relative = path.relative_to(REPO_ROOT).as_posix()
        for index, line in enumerate(lines):
            for found_kind, ident in ANNOTATION.findall(line):
                if found_kind != kind:
                    continue
                label = enclosing_test(lines, index) if kind == "verifies" else None
                annotations.append(
                    Annotation(
                        kind=found_kind,
                        ident=ident,
                        location=f"{relative}:{index + 1}",
                        label=label or relative,
                    )
                )
    return annotations


def check_fmea_references(requirements: list[Requirement]) -> list[str]:
    """Every REQ- reference in the FMEA must resolve.

    An FMEA row citing a requirement that has been renamed or deleted is worse
    than a row citing none: it reads as covered. Checking these costs one regex
    and removes a whole category of quiet documentation rot.
    """
    if not FMEA_FILE.is_file():
        return [f"{FMEA_FILE.name} is missing"]

    known = {requirement.ident for requirement in requirements}
    problems: list[str] = []
    reference = re.compile(r"REQ-[A-Za-z0-9-]+")
    for number, line in enumerate(FMEA_FILE.read_text(encoding="utf-8").splitlines(), 1):
        for ident in reference.findall(line):
            if ident not in known:
                problems.append(
                    f"docs/fmea.md:{number}: references unknown requirement {ident}"
                )
    return problems


def build_matrix(requirements: list[Requirement]) -> tuple[list[str], list[str]]:
    """Returns (matrix lines, problems)."""
    by_ident = {requirement.ident: requirement for requirement in requirements}
    problems: list[str] = []

    for kind, directories in (("satisfies", IMPLEMENTATION_DIRS),
                              ("verifies", VERIFICATION_DIRS)):
        for annotation in collect_annotations(directories, kind):
            requirement = by_ident.get(annotation.ident)
            if requirement is None:
                # The check that stops the tool from lying to itself.
                problems.append(
                    f"{annotation.location}: @{kind} references unknown requirement "
                    f"{annotation.ident}"
                )
                continue
            target = (requirement.satisfied_by if kind == "satisfies"
                      else requirement.verified_by)
            if annotation.label not in target:
                target.append(annotation.label)

    for requirement in requirements:
        if not requirement.satisfied_by:
            problems.append(f"{requirement.ident}: no implementation is annotated "
                            f"@satisfies {requirement.ident}")
        if not requirement.verified_by:
            problems.append(f"{requirement.ident}: no test is annotated "
                            f"@verifies {requirement.ident}")

    verified = sum(1 for r in requirements if r.verified_by)
    implemented = sum(1 for r in requirements if r.satisfied_by)

    lines = [
        "# Requirements traceability matrix",
        "",
        "**Generated by `scripts/traceability.py` — do not edit by hand.**",
        "",
        "A hand-maintained matrix is accurate on the day it is written and wrong",
        "ever after. This one is derived from `@satisfies` and `@verifies`",
        "annotations in the source, and CI fails if any requirement has no",
        "implementation link, no verification link, or if an annotation names a",
        "requirement that does not exist.",
        "",
        f"- Requirements: **{len(requirements)}**",
        f"- With an implementation link: **{implemented}/{len(requirements)}**",
        f"- With a verification link: **{verified}/{len(requirements)}**",
        "",
        "This is not a safety case. See `docs/safety-requirements.md`.",
        "",
        "| Requirement | Title | Implemented by | Verified by |",
        "|---|---|---|---|",
    ]

    for requirement in requirements:
        implementations = "<br>".join(f"`{item}`" for item in requirement.satisfied_by) or "—"
        verifications = "<br>".join(f"`{item}`" for item in requirement.verified_by) or "—"
        lines.append(
            f"| **{requirement.ident}** | {requirement.title} | "
            f"{implementations} | {verifications} |"
        )

    lines.append("")
    return lines, problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="verify links and that the committed matrix is current")
    arguments = parser.parse_args()

    requirements = parse_requirements(REQUIREMENTS_FILE)
    lines, problems = build_matrix(requirements)
    problems.extend(check_fmea_references(requirements))
    rendered = "\n".join(lines) + "\n"

    if problems:
        print(f"traceability: {len(problems)} problem(s)\n", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        print("", file=sys.stderr)
        return 1

    if arguments.check:
        if not MATRIX_FILE.is_file():
            print(f"error: {MATRIX_FILE.name} is missing; run scripts/traceability.py",
                  file=sys.stderr)
            return 1
        if MATRIX_FILE.read_text(encoding="utf-8") != rendered:
            print(f"error: {MATRIX_FILE.name} is out of date; "
                  "run scripts/traceability.py and commit the result", file=sys.stderr)
            return 1
        print(f"traceability OK: {len(requirements)} requirements, "
              "all implemented and verified, matrix current")
        return 0

    MATRIX_FILE.write_text(rendered, encoding="utf-8")
    print(f"wrote {MATRIX_FILE.relative_to(REPO_ROOT).as_posix()} "
          f"({len(requirements)} requirements)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

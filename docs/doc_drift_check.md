# `tools/doc_drift_check.sh`

Mechanical detector for the doc/code drift classes that recurred across
multiple `/pax --loop --auto review eph-net-dpdk` sessions (rounds 3
and 4 in particular). Each drift instance was caught only by human or
subagent eyeball pass — by the time a loop reached batch 3-4 of a 2-hour
review window, prior batches had already been spent on actual code bugs.
This script catches the cheap mechanical subset before the next loop
runs.

## Quick start

```bash
$ bash tools/doc_drift_check.sh

doc_drift_check.sh — scanning /home/me/eph

== Drift class B2: README test counts ==
  clean — all module READMEs match their test/ tree

== Drift class B1: doc symbol references vs codebase ==
[WARN] eph-itch/docs/ONBOARDING.md:165 — `msg::MyType` referenced in doc but not found in codebase
...

== 14 warnings, 0 errors ==
```

Exit code is what CI / pre-commit hooks key on:

| Exit | Meaning | Action |
|------|---------|--------|
| 0 | Clean | nothing to do |
| 1 | Warnings only | human review — could be false positive |
| 2 | Mechanical drift errors | fix the drift, then re-run |

## What it catches

### B1 — doc symbol refs that don't exist in the codebase

Scans every `.md` outside excluded dirs for inline-code patterns that
look like fully-qualified C++ identifiers:

```
`eph::utils::ScopeGuard`
`detail::transport_logger`
`bench::OneWayScenario`
```

For each unique symbol, greps `*.hpp` / `*.cpp` under the eph-* /
examples / benchmarks / tests roots. **Zero matches → warning.**

Deliberately limited to **qualified** identifiers (`A::B`, `ns::A::B`,
…) because unqualified `Foo` / `bar()` produces too many false positives
from English words wrapped in backticks ("the `target` of the migration
is …").

Known false-positive shapes (acceptable noise):

- ONBOARDING.md template placeholders — `msg::MyType`, `detail::xxx_logger`
- Future-pointing references in design docs that link to symbols not
  yet implemented (these should ideally be moved to `.artifacts/` so
  they're excluded automatically; keep them in active docs only as a
  conscious choice)
- Forward-references to symbols in legacy `benchmarks/latency/docs/` —
  the bench was rewritten in 2026-04-09 and several historical
  references in those docs are now stale (separate cleanup)

### B2 — module README test-count claims that don't match the tree

Scans `eph-*/README.md` for phrases like:

```
22 GoogleTest files under tests/
13 test files exercise the codec
```

(specifically the `<N> (GoogleTest|test) files` shape — the "files"
suffix is required to avoid matching `434 tests across 6 binaries`,
which counts TEST() invocations not files).

For each match, runs `find tests/ -name 'test_*.cpp' | wc -l` and
compares. **Mismatch → error, exit 2.**

Zero false positives by construction. This drift class triggered at
least three times in r4 alone (eph-utils 20→22, then again 22→23 after
the scope_guard reshape).

## What it deliberately doesn't catch

Listed for completeness — if you find drift in one of these classes,
fix it manually and consider whether the cost/benefit of adding a
detector here is worth it:

| Class | Why skipped | Workaround |
|-------|-------------|-----------|
| Function signature drift inside ```cpp blocks | Needs a real C++ parser to verify arity / param types | Spot check during PR review; r4 caught Eal::init this way |
| Public-API "factories are X, Y, Z" lists | Same parser problem; also list-style varies wildly | Same as above |
| Prose semantic drift ("the bench writes no files") | Pure NLP; no mechanical check possible | Loop reviews — `/pax --loop --auto review` finds these |
| CHANGELOG `[Unreleased]` staleness | Legitimate to mention removed symbols here | Code-review checklist when merging significant fixes |
| Pure docstring (header inline-doc) drift | Header-internal; outside the doc/markdown scope | Compiler `-Wdoxygen-style` (project doesn't enable this) |

## Excludes

The script excludes the following dirs from doc scanning:

- `.git/`
- `build/` (and `build_*/`)
- `.artifacts/` — historical decision records, pre-merge plans, retros
- `.planning/`, `.discuss/` — same rationale
- `.claude/`, `.xmake/` — tool state
- `*CHANGELOG*.md` (any case) — legitimately mentions removed symbols

If a doc you wrote is being scanned and you don't think it should be,
either move it under `.artifacts/` or extend the `EXCLUDE_DIRS` array
in the script (and document why).

## CI integration

Not auto-wired to CI today (the project doesn't have a CI YAML). When
CI is added, the simplest gate is:

```yaml
- name: Doc/code drift check
  run: bash tools/doc_drift_check.sh
  # Treat exit 2 (mechanical drift) as a failure; exit 1 (warnings) as
  # informational — log but don't block.
```

If desired, exit-1 could be promoted to failure once the legacy bench
docs are cleaned up and the warning baseline is zero.

## Maintenance

If a new module is added (e.g. `eph-foobar/`), the script automatically
picks up `eph-foobar/README.md` and `eph-foobar/tests/` because the
glob is `eph-*/`. No script change needed.

If a new drift class is identified that's worth catching mechanically,
add a new `check_<name>()` function to the script following the B1/B2
pattern. Each check should:

- Print its own header (`== Drift class XX: ... ==`)
- Increment `WARN_COUNT` (warnings) or `ERR_COUNT` (errors) on each hit
- Print a structured `[ERROR] file:line — detail` or `[WARN] ...` line
- Optionally print `clean — ...` in green when it found nothing

## Related

- `.artifacts/pax-review-dpdk-r3-20260502.md` — drift surfaced in r3
- `.artifacts/pax-review-dpdk-r4-20260502.md` — drift surfaced in r4
  (incl. trend analysis showing this is a recurring class)

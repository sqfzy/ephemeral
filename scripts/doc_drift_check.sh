#!/usr/bin/env bash
#
# doc_drift_check.sh — mechanical doc/code drift detector
#
# Surfaces drift classes that recurred across `/pax --loop --auto review`
# rounds 3 and 4 (CLAUDE.md "bench writes no files", README test counts,
# stale Eal::init signature, summary.md "same surface as KernelTcpStream"
# shorthand). Each was caught by human / subagent eyeball pass; this
# script makes the cheap subset fail fast.
#
# What this DOES detect:
#   1. Inline-code symbol references in docs that don't exist in the
#      codebase  (drift-class B1)
#   2. Test-count claims in module READMEs that don't match the actual
#      test file count                                  (drift-class B2)
#
# What this does NOT detect (out of scope, false-positive prone):
#   - Function signature drift (parser-required to verify arity / types)
#   - Public-API list staleness (ditto; would need a real cppfront-style
#     parser; the cost/benefit doesn't justify it)
#   - Prose drift (semantic mismatch between description and behavior)
#
# Excludes:
#   .git/, build*/, .artifacts/ (historical), .planning/, .claude/,
#   any *CHANGELOG*.md (legitimately mentions removed symbols)
#
# Exit codes:
#   0  clean
#   1  warnings only (drift-class B1: doc references symbol the grep
#      heuristic couldn't find — could be a true positive or a parser-
#      shaped false positive)
#   2  errors        (drift-class B2: test count mismatch — mechanical,
#      no false positive)
#
# Usage:
#   bash scripts/doc_drift_check.sh [REPO_ROOT]
#
# Default REPO_ROOT = current directory. Robust against being invoked
# from any subdir as long as REPO_ROOT is passed or pwd is the root.
#
# Idempotent: read-only; running it twice produces identical output for
# the same tree state.

set -euo pipefail

# -----------------------------------------------------------------------
# Setup
# -----------------------------------------------------------------------

REPO_ROOT="${1:-$(pwd)}"
REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"   # canonicalise

if [[ ! -d "$REPO_ROOT/eph-utils" ]] || [[ ! -f "$REPO_ROOT/CLAUDE.md" ]]; then
    printf 'ERROR: %s does not look like an eph repo root (no eph-utils/ or CLAUDE.md)\n' \
        "$REPO_ROOT" >&2
    exit 2
fi

cd "$REPO_ROOT"

# Track findings; exit code derived at the end.
WARN_COUNT=0
ERR_COUNT=0

# Common find/grep excludes — keep in sync with the comment above.
# Patterns use `*/<dir>/*` so they match regardless of whether find was
# invoked with a relative or absolute starting path.
EXCLUDE_DIRS=(
    -not -path '*/.git/*'
    -not -path '*/build/*'
    -not -path '*/build_*/*'
    -not -path '*/.artifacts/*'
    -not -path '*/.planning/*'
    -not -path '*/.claude/*'
    -not -path '*/.discuss/*'
    -not -path '*/.xmake/*'
)

# ANSI helpers — only when stdout is a tty.
if [[ -t 1 ]]; then
    R='\033[0;31m'; Y='\033[0;33m'; G='\033[0;32m'; D='\033[0m'
else
    R=''; Y=''; G=''; D=''
fi

# -----------------------------------------------------------------------
# Drift class B2: module README test-count claims vs actual *.cpp count
# -----------------------------------------------------------------------
#
# Pattern: any "<N> (GoogleTest files|test files|tests)" claim in
# eph-*/README.md.  Compare N to actual `find tests/ -name 'test_*.cpp' | wc -l`.
# Handle "GoogleTest" capitalised either way.
#
# Heuristic: extract the FIRST integer that appears within ~5 chars of
# one of the trigger phrases. Avoid matching unrelated counts (e.g.
# "supports 16 streams" elsewhere in the README).

check_test_counts() {
    printf '%b== Drift class B2: README test counts ==%b\n' "$G" "$D"
    local found_any=0
    for readme in "$REPO_ROOT"/eph-*/README.md; do
        [[ -f "$readme" ]] || continue
        local module
        module="$(basename "$(dirname "$readme")")"
        local tests_dir="$REPO_ROOT/$module/tests"
        [[ -d "$tests_dir" ]] || continue

        # Count actual test files (test_*.cpp under tests/, recursive).
        local actual
        actual="$(find "$tests_dir" -name 'test_*.cpp' -type f 2>/dev/null | wc -l | tr -d ' ')"

        # Look for claims like "20 GoogleTest files" or "13 test files".
        # We deliberately require the "files" suffix to avoid matching
        # TEST()-count phrasing like "434 tests across 6 binaries"
        # (which is a count of test cases, not files, and follows a
        # different convention).
        local line_match
        line_match="$(grep -nE \
            '[0-9]+[[:space:]]+(GoogleTest|test)[[:space:]]+files' \
            "$readme" 2>/dev/null || true)"
        [[ -z "$line_match" ]] && continue

        # Each matching line: extract the integer that immediately
        # precedes the trigger and compare.
        while IFS= read -r line; do
            local lineno claim
            lineno="${line%%:*}"
            claim="$(echo "$line" | grep -oE '[0-9]+[[:space:]]+(GoogleTest|test)[[:space:]]+files' | head -1)"
            [[ -z "$claim" ]] && continue
            local n
            n="$(echo "$claim" | grep -oE '^[0-9]+')"
            if [[ "$n" != "$actual" ]]; then
                printf '%b[ERROR]%b %s:%s — claims "%s" but tests/ has %s test_*.cpp files\n' \
                    "$R" "$D" "$readme" "$lineno" "$claim" "$actual"
                ERR_COUNT=$((ERR_COUNT + 1))
                found_any=1
            fi
        done <<< "$line_match"
    done
    if [[ "$found_any" -eq 0 ]]; then
        printf '%b  clean — all module READMEs match their test/ tree%b\n' "$G" "$D"
    fi
}

# -----------------------------------------------------------------------
# Drift class B1: inline-code symbol references in docs that don't exist
# -----------------------------------------------------------------------
#
# Heuristic:
#   - Scan *.md files (excluding CHANGELOG / .artifacts / etc.)
#   - Extract patterns matching `eph::ns::Symbol` (qualified C++ identifiers
#     with at least one `::`), since those have low false-positive rate
#   - For each unique symbol, grep the codebase (include/ + src/) for it
#     If zero matches: flag as warning (might be a parser FP)
#
# We deliberately DO NOT match unqualified `Foo` / `bar()` because the
# false-positive rate is too high — too many docs use English words that
# happen to look like identifiers when wrapped in backticks.

check_symbol_refs() {
    printf '%b== Drift class B1: doc symbol references vs codebase ==%b\n' "$G" "$D"

    # Files to scan: all .md outside excluded dirs, minus CHANGELOG variants.
    local md_files
    md_files="$(find "$REPO_ROOT" -name '*.md' -type f \
        "${EXCLUDE_DIRS[@]}" \
        -not -iname '*CHANGELOG*' \
        2>/dev/null | sort)"

    [[ -z "$md_files" ]] && {
        printf '%b  no .md files to scan%b\n' "$G" "$D"
        return
    }

    # Build a single grep over all qualified-identifier patterns inside
    # backticks. Pattern: `<ident>::<ident>(::<ident>)*` with optional
    # trailing `()`/`<>` for callability/template hints.
    # Regex: `[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)+`
    local refs
    refs="$(grep -hoE '`[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)+(\(\))?(\<[^`<>]+\>)?`' \
            $md_files 2>/dev/null \
        | sort -u \
        | sed 's/^`//; s/`$//; s/()$//; s/<.*>$//')"
    [[ -z "$refs" ]] && {
        printf '%b  no qualified-identifier references found in docs%b\n' "$G" "$D"
        return
    }

    # For each unique symbol, look up in the codebase. We grep across all
    # *.hpp / *.cpp under the eph-* / examples/ / benchmarks/ trees. The
    # symbol could be a class, function, namespace, or constant.
    local found_any=0
    while IFS= read -r sym; do
        [[ -z "$sym" ]] && continue
        # Look for the LAST component as the actual identifier, since
        # `eph::utils::Foo` may be `using` aliased internally — we only
        # want to know "does this symbol-name exist anywhere in the
        # codebase?"
        local last_part
        last_part="${sym##*::}"
        # Search in code roots only.
        local hit
        hit="$(grep -rlE \
            "(\b|^)${last_part}\b" \
            --include='*.hpp' --include='*.cpp' \
            "$REPO_ROOT/eph-utils/" \
            "$REPO_ROOT/eph-core/" \
            "$REPO_ROOT/eph-codec/" \
            "$REPO_ROOT/eph-net/" \
            "$REPO_ROOT/eph-net-kernel/" \
            "$REPO_ROOT/eph-net-dpdk/" \
            "$REPO_ROOT/eph-fix/" \
            "$REPO_ROOT/eph-itch/" \
            "$REPO_ROOT/eph-json/" \
            "$REPO_ROOT/eph-book/" \
            "$REPO_ROOT/eph-containers/" \
            "$REPO_ROOT/examples/" \
            "$REPO_ROOT/benchmarks/" \
            "$REPO_ROOT/tests/" \
            2>/dev/null | head -1 || true)"
        if [[ -z "$hit" ]]; then
            # Find the first doc file that mentions this symbol so the
            # warning is actionable.
            local doc_hit
            doc_hit="$(grep -lE "\`${sym}(\(\))?(<[^\`<>]+>)?\`" $md_files 2>/dev/null | head -1 || true)"
            local lineno
            lineno="$(grep -nE "\`${sym}(\(\))?(<[^\`<>]+>)?\`" "$doc_hit" 2>/dev/null | head -1 | cut -d: -f1 || true)"
            printf '%b[WARN]%b %s:%s — \`%s\` referenced in doc but not found in codebase\n' \
                "$Y" "$D" "$doc_hit" "${lineno:-?}" "$sym"
            WARN_COUNT=$((WARN_COUNT + 1))
            found_any=1
        fi
    done <<< "$refs"

    if [[ "$found_any" -eq 0 ]]; then
        printf '%b  clean — every qualified symbol referenced in docs exists in code%b\n' "$G" "$D"
    fi
}

# -----------------------------------------------------------------------
# Run the checks
# -----------------------------------------------------------------------

printf '\n%bdoc_drift_check.sh%b — scanning %s\n\n' "$G" "$D" "$REPO_ROOT"
check_test_counts
printf '\n'
check_symbol_refs
printf '\n'

# -----------------------------------------------------------------------
# Summary + exit code
# -----------------------------------------------------------------------

if [[ "$ERR_COUNT" -eq 0 && "$WARN_COUNT" -eq 0 ]]; then
    printf '%b== ALL CHECKS PASSED ==%b\n' "$G" "$D"
    exit 0
elif [[ "$ERR_COUNT" -eq 0 ]]; then
    printf '%b== %s warnings, 0 errors ==%b\n' "$Y" "$WARN_COUNT" "$D"
    exit 1
else
    printf '%b== %s errors, %s warnings ==%b\n' "$R" "$ERR_COUNT" "$WARN_COUNT" "$D"
    exit 2
fi

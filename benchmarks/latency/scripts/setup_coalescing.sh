#!/usr/bin/env bash
# benchmarks/latency/scripts/setup_coalescing.sh
#   — tune NIC RX coalescing for reproducible bench latency
#
# ┌── Script roles in this repo ────────────────────────────────────────────┐
# │ eph-net-dpdk/scripts/dpdk-setup.sh     one-shot host env: vfio + hugepg │
# │ eph-net-dpdk/scripts/dpdk-teardown.sh  undo dpdk-setup, restore kernel  │
# │ benchmarks/latency/scripts/            this script — RX coalescing tune │
# │   setup_coalescing.sh                                                   │
# │ benchmarks/latency/lat                 per-run bench wrapper            │
# └─────────────────────────────────────────────────────────────────────────┘
#
# Why: AWS ena (and some other NIC drivers) ship with Adaptive RX coalescing
# enabled by default. Under sustained UDP load the adaptive algorithm settles
# at a workpoint that adds a clear ~270 us "NAPI shoulder" to kernel
# `recvmsg` latency (P90/P99/P99.9 all clamp to ~270-280 us). This makes
# kernel-vs-DPDK bench comparisons report a fake "kernel is slow" gap that
# disappears the moment you force coalescing off:
#
#     ethtool -C <nic> adaptive-rx off rx-usecs 0
#
# We discovered this the hard way — see eph7's docs/latency-benchmark-fairness
# for the full story. This script is the canonical way to apply that fix to
# the NICs the bench actually uses.
#
# Default behaviour: read NIC_A and NIC_B from benchmarks/latency/bench.conf
# (the convention used by `lat`) and tune both. NIC_B is assumed to be in the
# `bench_ns` netns when in kernel mode (the same netns name `lat` uses); the
# script auto-detects whether NIC_B is in default ns or in bench_ns and
# applies the setting accordingly. If bench.conf doesn't exist or doesn't
# define both NICs, you must pass them explicitly.
#
# Settings persist across kernel-driver lifecycles **but are lost when a NIC
# is bound to vfio-pci and then re-bound to the kernel driver** (vfio rebind
# re-initialises the NIC). If your bench harness does any vfio bind/unbind,
# re-run this script (or have the harness call it) after the bind cycle.

set -uo pipefail

# ──────────────────────────────────────────
# Color & logging
# ──────────────────────────────────────────
if [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]]; then
    GREEN=$'\033[0;32m'; RED=$'\033[0;31m'; YELLOW=$'\033[1;33m'
    BLUE=$'\033[0;34m'; CYAN=$'\033[0;36m'; BOLD=$'\033[1m'; NC=$'\033[0m'
else
    GREEN=''; RED=''; YELLOW=''; BLUE=''; CYAN=''; BOLD=''; NC=''
fi
log_ok()    { printf '%s✓%s  %s\n' "$GREEN" "$NC" "$*"; }
log_err()   { printf '%s✗%s  %s%s%s\n' "$RED" "$NC" "$BOLD" "$*" "$NC" >&2; }
log_warn()  { printf '%s⚠%s  %s\n' "$YELLOW" "$NC" "$*" >&2; }
log_info()  { printf '%sℹ%s  %s\n' "$CYAN" "$NC" "$*"; }
log_step()  { printf '\n%s▶%s  %s%s%s\n' "$BLUE" "$NC" "$BOLD" "$*" "$NC"; }

die() {
    log_err "$1"
    [[ -n "${2:-}" ]] && printf '\n%s\n' "$2" >&2
    [[ -n "${SCRIPT_ARGS_DISPLAY:-}" ]] && printf '\n  retry: sudo %s %s\n' "$0" "$SCRIPT_ARGS_DISPLAY" >&2
    exit 1
}

# ──────────────────────────────────────────
# Locate repo root + bench.conf
# ──────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if PROJECT_DIR=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null); then
    :
else
    # Fallback: walk up looking for the eph-net-dpdk/ marker. Phase 7
    # renamed the legacy eph-dpdk/ directory to eph-net-dpdk/, so the
    # old marker stopped existing — left unchanged this loop bottomed
    # out at "/" and silently aliased PROJECT_DIR to "$SCRIPT_DIR/.."
    # (= benchmarks/latency), making BENCH_CONFIG resolve to
    # `benchmarks/latency/benchmarks/latency/bench.conf` which never
    # exists. Use the current marker.
    PROJECT_DIR="$SCRIPT_DIR"
    while [[ "$PROJECT_DIR" != "/" && ! -d "$PROJECT_DIR/eph-net-dpdk" ]]; do
        PROJECT_DIR="$(dirname "$PROJECT_DIR")"
    done
    [[ "$PROJECT_DIR" == "/" ]] && PROJECT_DIR="$SCRIPT_DIR/.."
fi
BENCH_CONFIG="${BENCH_CONFIG:-$PROJECT_DIR/benchmarks/latency/bench.conf}"

# Convention used by benchmarks/latency/lat — keep in sync.
BENCH_NS_NAME="${BENCH_NS:-bench_ns}"

# Tunable parameters (override via env)
TARGET_RX_USECS="${TARGET_RX_USECS:-0}"
TARGET_ADAPTIVE="${TARGET_ADAPTIVE:-off}"

# ──────────────────────────────────────────
# Argument parsing
# ──────────────────────────────────────────
SCRIPT_ARGS_DISPLAY="$*"
MODE="apply"  # apply | check | restore
EXPLICIT_NIC=""
EXPLICIT_NETNS=""
DRY_RUN=false
VERBOSE=false

usage() {
    cat <<EOF
${BOLD}Usage${NC}: sudo ./benchmarks/latency/scripts/setup_coalescing.sh [options] [<nic> [<netns>]]

${BOLD}Tune NIC RX coalescing to a fixed (adaptive=off, rx-usecs=0) workpoint${NC}
so kernel-vs-DPDK latency benchmarks measure the stack itself, not the
driver's adaptive coalescing state. Required for any meaningful bench
on AWS ena, recommended on other NICs that expose the same knobs.

${BOLD}Default behaviour${NC} (no positional args):
  Reads ${BENCH_CONFIG#$PROJECT_DIR/} and tunes:
    - NIC_A         (server side, default ns)
    - NIC_B         (client side, auto-detected: default ns OR $BENCH_NS_NAME)

${BOLD}Options${NC}:
  --check          Show current coalescing state of all targets, do not modify
  --restore        Restore Adaptive RX = on, rx-usecs = 20 (driver default)
  --dry-run        Print what would happen without applying changes
  -v, --verbose    Verbose output (per-target reasoning)
  -h, --help       Show this help

${BOLD}Single-NIC mode${NC} (positional args override bench.conf):
  sudo ./benchmarks/latency/scripts/setup_coalescing.sh ens34                # default ns
  sudo ./benchmarks/latency/scripts/setup_coalescing.sh ens35 $BENCH_NS_NAME             # in $BENCH_NS_NAME
  sudo ./benchmarks/latency/scripts/setup_coalescing.sh ens5 my_custom_ns    # any netns

${BOLD}Environment variables${NC}:
  BENCH_CONFIG       Override bench.conf path (default: $BENCH_CONFIG)
  BENCH_NS           Override bench netns name (default: bench_ns)
  TARGET_RX_USECS    rx-usecs to apply (default: 0 = no coalescing window)
  TARGET_ADAPTIVE    adaptive-rx setting (default: off)

${BOLD}Examples${NC}:
  sudo ./benchmarks/latency/scripts/setup_coalescing.sh                # tune both NICs from bench.conf
  sudo ./benchmarks/latency/scripts/setup_coalescing.sh --check        # inspect current state
  sudo ./benchmarks/latency/scripts/setup_coalescing.sh --restore      # back to driver defaults
  sudo TARGET_RX_USECS=8 ./benchmarks/latency/scripts/setup_coalescing.sh  # use 8 us instead of 0
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check)         MODE="check"; shift ;;
        --restore)       MODE="restore"; shift ;;
        --dry-run)       DRY_RUN=true; shift ;;
        -v|--verbose)    VERBOSE=true; shift ;;
        -h|--help)       usage; exit 0 ;;
        -*)              die "unknown option: $1" "  see help: $0 --help" ;;
        *)
            if [[ -z "$EXPLICIT_NIC" ]]; then EXPLICIT_NIC="$1"
            elif [[ -z "$EXPLICIT_NETNS" ]]; then EXPLICIT_NETNS="$1"
            else die "too many positional args: $1" "  usage: $0 [<nic> [<netns>]]"
            fi
            shift ;;
    esac
done

# ──────────────────────────────────────────
# Pre-flight
# ──────────────────────────────────────────
# Every mode needs root: ethtool -c needs CAP_NET_ADMIN, nsenter into a netns
# needs CAP_SYS_ADMIN. Even --check would fail silently for netns NICs without
# root, so we require it across the board for honest output.
if [[ $EUID -ne 0 ]]; then
    die "needs root (ethtool and nsenter both need elevated privileges)" \
        "  rerun: sudo $0 ${SCRIPT_ARGS_DISPLAY}"
fi

if ! command -v ethtool &>/dev/null; then
    die "ethtool not installed" \
        "  Amazon Linux:  sudo dnf install -y ethtool\n  Ubuntu/Debian: sudo apt install -y ethtool\n  Arch:          sudo pacman -S ethtool"
fi

# ──────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────

# Run a command inside the named netns (empty netns = default ns).
# Uses nsenter rather than `ip netns exec` because some ethtool operations
# behave differently with the latter.
in_netns() {
    local netns="$1"; shift
    if [[ -z "$netns" ]]; then
        "$@"
    elif [[ -e "/var/run/netns/$netns" ]]; then
        nsenter --net="/var/run/netns/$netns" "$@"
    else
        return 1  # netns does not exist
    fi
}

nic_exists_in() {
    local nic="$1" netns="$2"
    in_netns "$netns" ip link show "$nic" &>/dev/null
}

# Locate which netns currently holds a NIC (if any). Echoes the netns name
# (or empty string for default ns) and returns 0 on success, 1 if not found
# anywhere visible.
locate_nic() {
    local nic="$1"
    if ip link show "$nic" &>/dev/null; then
        echo ""  # default ns
        return 0
    fi
    local ns
    for ns in $(ip netns list 2>/dev/null | awk '{print $1}'); do
        if [[ -e "/var/run/netns/$ns" ]] && nsenter --net="/var/run/netns/$ns" ip link show "$nic" &>/dev/null; then
            echo "$ns"
            return 0
        fi
    done
    return 1
}

# Read current coalescing: echoes "<adaptive_rx> <rx_usecs>" or returns
# non-zero if the driver does not support coalescing query.
get_coalescing() {
    local nic="$1" netns="$2"
    local out
    out=$(in_netns "$netns" ethtool -c "$nic" 2>/dev/null) || return 1
    local adaptive rx_usecs
    adaptive=$(echo "$out" | awk '/Adaptive RX:/ {print $3; exit}')
    rx_usecs=$(echo "$out" | awk '/^rx-usecs:/ {print $2; exit}')
    echo "${adaptive:-?} ${rx_usecs:-?}"
}

apply_coalescing() {
    local nic="$1" netns="$2" adaptive="$3" rx_usecs="$4"
    if [[ "$DRY_RUN" == true ]]; then
        log_info "[dry-run] would: ethtool -C $nic adaptive-rx $adaptive rx-usecs $rx_usecs${netns:+ (in $netns)}"
        return 0
    fi
    in_netns "$netns" ethtool -C "$nic" adaptive-rx "$adaptive" rx-usecs "$rx_usecs" 2>/dev/null
}

# Process a single (nic, netns) target.
process_target() {
    local nic="$1" netns="$2"
    local label="$nic${netns:+ (in $netns)}"

    if ! nic_exists_in "$nic" "$netns"; then
        # Try to find where it actually lives, to give a more useful warning.
        local actual_ns
        if actual_ns=$(locate_nic "$nic"); then
            log_warn "$label not found, but $nic exists in ${actual_ns:-default ns}"
            log_info "  retry: sudo $0 $nic ${actual_ns}"
        else
            log_warn "$label not found anywhere — skipping"
        fi
        return 0
    fi

    local cur adaptive_now rx_now
    cur=$(get_coalescing "$nic" "$netns") || {
        log_warn "$label: ethtool -c not supported (driver may lack coalescing knobs)"
        return 0
    }
    read -r adaptive_now rx_now <<<"$cur"

    case "$MODE" in
        check)
            log_info "$label: adaptive=$adaptive_now rx-usecs=$rx_now"
            return 0
            ;;
        restore)
            local target_adaptive="on" target_rx="20"  # ena driver default
            if [[ "$adaptive_now" == "$target_adaptive" && "$rx_now" == "$target_rx" ]]; then
                log_ok "$label already at driver default (adaptive=on rx-usecs=20)"
                return 0
            fi
            if apply_coalescing "$nic" "$netns" "$target_adaptive" "$target_rx"; then
                # Suppress the success line in dry-run; apply_coalescing already
                # printed the [dry-run] preview line.
                if [[ "$DRY_RUN" != true ]]; then
                    log_ok "$label → adaptive=on rx-usecs=20 (restored)"
                fi
            else
                log_err "$label: failed to restore"
                return 1
            fi
            ;;
        apply)
            local target_adaptive_lc
            target_adaptive_lc=$(echo "$TARGET_ADAPTIVE" | tr '[:upper:]' '[:lower:]')
            if [[ "$adaptive_now" == "$target_adaptive_lc" && "$rx_now" == "$TARGET_RX_USECS" ]]; then
                log_ok "$label already at adaptive=$target_adaptive_lc rx-usecs=$TARGET_RX_USECS"
                return 0
            fi
            $VERBOSE && log_info "$label was: adaptive=$adaptive_now rx-usecs=$rx_now"
            if apply_coalescing "$nic" "$netns" "$target_adaptive_lc" "$TARGET_RX_USECS"; then
                if [[ "$DRY_RUN" != true ]]; then
                    log_ok "$label → adaptive=$target_adaptive_lc rx-usecs=$TARGET_RX_USECS"
                fi
            else
                log_err "$label: failed to apply"
                log_info "  manual: sudo ${netns:+nsenter --net=/var/run/netns/$netns }ethtool -C $nic adaptive-rx $target_adaptive_lc rx-usecs $TARGET_RX_USECS"
                return 1
            fi
            ;;
    esac
}

# ──────────────────────────────────────────
# Build target list
# ──────────────────────────────────────────
declare -a TARGETS=()  # entries: "nic|netns"

if [[ -n "$EXPLICIT_NIC" ]]; then
    # Single-NIC mode (positional args). EXPLICIT_NETNS may be empty.
    TARGETS+=("$EXPLICIT_NIC|$EXPLICIT_NETNS")
elif [[ -f "$BENCH_CONFIG" ]]; then
    # Default mode: read NIC_A / NIC_B from bench.conf and locate each.
    # bench.conf is shell-syntax (NAME=VALUE one per line) — sourcing it is
    # acceptable here because it's part of the same repo and the user is
    # already running us as root.
    # shellcheck disable=SC1090
    source "$BENCH_CONFIG"
    if [[ -z "${NIC_A:-}" || -z "${NIC_B:-}" ]]; then
        die "bench.conf does not define NIC_A and NIC_B" \
            "  inspect: cat ${BENCH_CONFIG#$PROJECT_DIR/}\n  or pass NICs explicitly: sudo $0 <nic> [<netns>]"
    fi
    # NIC_A is the server side — always default ns by convention.
    TARGETS+=("$NIC_A|")
    # NIC_B may be in default ns (DPDK mode) or in bench_ns (kernel mode).
    nic_b_ns=$(locate_nic "$NIC_B" 2>/dev/null || true)
    if [[ -z "$nic_b_ns" ]] && ! ip link show "$NIC_B" &>/dev/null; then
        # Not visible at all — most likely bound to vfio-pci. Coalescing is
        # not applicable while bound to vfio. Warn and skip.
        log_warn "$NIC_B not found in any netns — possibly bound to vfio-pci, skipping"
        log_info "  if running a DPDK bench, this is expected; coalescing only matters in kernel mode"
        log_info "  to apply now, first re-bind: sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh"
    else
        TARGETS+=("$NIC_B|$nic_b_ns")
    fi
else
    die "no bench.conf at $BENCH_CONFIG and no NIC argument" \
        "  pass a NIC explicitly: sudo $0 <nic> [<netns>]\n  or create $BENCH_CONFIG (see benchmarks/latency/bench.conf in eph7)"
fi

# ──────────────────────────────────────────
# Main
# ──────────────────────────────────────────
DRYRUN_LABEL=""
[[ "$DRY_RUN" == true ]] && DRYRUN_LABEL=" (dry-run)"
printf '%ssetup_coalescing.sh%s — mode=%s%s\n' "$BOLD" "$NC" "$MODE" "$DRYRUN_LABEL"

EXIT_CODE=0
for entry in "${TARGETS[@]}"; do
    nic="${entry%%|*}"
    netns="${entry#*|}"
    process_target "$nic" "$netns" || EXIT_CODE=1
done

if [[ "$MODE" == "apply" && "$DRY_RUN" != true ]]; then
    echo
    log_info "💡 reminder: vfio-pci bind/unbind clears the kernel-side coalescing"
    log_info "   re-run this script after any DPDK bind cycle:  sudo $0 ${EXPLICIT_NIC:-}"
fi

exit "$EXIT_CODE"

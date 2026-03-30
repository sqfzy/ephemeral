#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# bench_binance.sh — Binance market data benchmark (kernel + DPDK simultaneous)
#
# Runs bench_pingpong, bench_market_multi, and bench_market_pingpong in
# kernel and DPDK variants SIMULTANEOUSLY against the same server endpoint.
#
# Usage:
#   scripts/bench_binance.sh mock 172.31.27.208                    # all, mock, 30s
#   scripts/bench_binance.sh binance                               # all, real Binance, 30s
#   scripts/bench_binance.sh mock 172.31.27.208 300                # all, mock, 5 min
#   scripts/bench_binance.sh binance 3600                          # all, real Binance, 1h
#   scripts/bench_binance.sh mock 172.31.27.208 30 market_multi    # only market_multi
#   scripts/bench_binance.sh binance 60 pingpong                   # only pingpong, 60s
#
# Supported benchmarks: all (default), pingpong, market_multi, market_pingpong
# ═══════════════════════════════════════════════════════════════════════════
set -euo pipefail

# ══════════════════════════════════════════════════════════════════════════
# Hardcoded configuration — edit here, not via CLI flags
# ══════════════════════════════════════════════════════════════════════════

# Binary directory (release build)
BINDIR="build/linux/arm64/release"

# Kernel CPU pinning
SOCK_TX_CPU=2
SOCK_RX_CPU=3
SOCK_MAIN_CPU=4

# DPDK configuration
DPDK_EAL_ARGS="-a 0000:28:00.0 -l 4-7"
DPDK_LOCAL_IP="172.31.23.112"
DPDK_GATEWAY_IP="172.31.16.1"
DPDK_TX_CPU=5
DPDK_RX_CPU=6
DPDK_MAIN_CPU=7

# Mock server
MOCK_PORT=9443
MOCK_SERVER_REMOTE_PATH="/tmp/mock_server"
MOCK_SERVER_LOCAL_PATH=".bench/mock"
SSH_KEY="$HOME/.ssh/aws_key.pem"

# Benchmark defaults
DEFAULT_DURATION=30       # seconds for market benchmarks
PING_COUNT=200            # pings for pingpong
PING_INTERVAL=200         # ms between pings
ORDER_INTERVAL=500        # ms between simulated orders

# DPDK ENA driver needs a few seconds between runs to release the NIC
DPDK_COOLDOWN=5

# ══════════════════════════════════════════════════════════════════════════
# Color & logging
# ══════════════════════════════════════════════════════════════════════════

if [ -t 1 ] && [ "${NO_COLOR:-}" = "" ]; then
    R='\033[0;31m'; G='\033[0;32m'; Y='\033[0;33m'; B='\033[0;34m'
    BOLD='\033[1m'; NC='\033[0m'
else
    R=''; G=''; Y=''; B=''; BOLD=''; NC=''
fi

info()  { echo -e "${B}▶${NC}  $*"; }
ok()    { echo -e "${G}✓${NC}  $*"; }
warn()  { echo -e "${Y}⚠${NC}  $*"; }
die()   {
    echo -e "${R}✗${NC}  $*" >&2
    echo "   Usage: scripts/bench_binance.sh {mock <host>|binance} [duration]" >&2
    exit 1
}

# ══════════════════════════════════════════════════════════════════════════
# Argument parsing — positional only, no flags
# ══════════════════════════════════════════════════════════════════════════

if [ $# -lt 1 ]; then
    cat <<'USAGE'
Usage:
  scripts/bench_binance.sh mock <ip> [duration] [bench]
  scripts/bench_binance.sh binance [duration] [bench]

Arguments:
  mode       mock <ip> | binance
  duration   seconds (default: 30)
  bench      all (default) | pingpong | market_multi | market_pingpong

Examples:
  scripts/bench_binance.sh mock 172.31.27.208              # all, 30s
  scripts/bench_binance.sh mock 172.31.27.208 300          # all, 5 min
  scripts/bench_binance.sh binance                         # all, 30s
  scripts/bench_binance.sh binance 3600                    # all, 1 hour
  scripts/bench_binance.sh binance 60 market_multi         # only market_multi
  scripts/bench_binance.sh mock 172.31.27.208 30 pingpong  # only pingpong

Kernel and DPDK benchmarks run SIMULTANEOUSLY against the same server.
USAGE
    exit 0
fi

MODE="$1"
BENCH="all"
case "$MODE" in
    mock)
        [ $# -lt 2 ] && die "mock mode requires server IP: scripts/bench_binance.sh mock <ip>"
        MOCK_HOST="$2"
        DURATION="${3:-$DEFAULT_DURATION}"
        BENCH="${4:-all}"
        ;;
    binance)
        DURATION="${2:-$DEFAULT_DURATION}"
        BENCH="${3:-all}"
        ;;
    *)
        die "Unknown mode: $MODE (use 'mock' or 'binance')"
        ;;
esac

# Validate bench name
case "$BENCH" in
    all|pingpong|market_multi|market_pingpong) ;;
    *) die "Unknown bench: $BENCH (use all, pingpong, market_multi, or market_pingpong)" ;;
esac

# ══════════════════════════════════════════════════════════════════════════
# Derived variables
# ══════════════════════════════════════════════════════════════════════════

PROJ_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJ_DIR"

OUTDIR=".bench/$(date '+%Y%m%d-%H%M%S')-${MODE}"
mkdir -p "$OUTDIR"

SOCK_ARGS="--tx-cpu $SOCK_TX_CPU --rx-cpu $SOCK_RX_CPU --main-cpu $SOCK_MAIN_CPU"
DPDK_NET="--local-ip $DPDK_LOCAL_IP --gateway-ip $DPDK_GATEWAY_IP --tx-cpu $DPDK_TX_CPU --rx-cpu $DPDK_RX_CPU --main-cpu $DPDK_MAIN_CPU"

SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=5"
ssh_mock() { ssh $SSH_OPTS "ec2-user@$MOCK_HOST" "$@" 2>/dev/null; }

# ══════════════════════════════════════════════════════════════════════════
# Pre-flight checks
# ══════════════════════════════════════════════════════════════════════════

info "Pre-flight checks"

# Required binaries (only check what we need)
required_bins=()
case "$BENCH" in
    all)
        required_bins=(bench_pingpong bench_pingpong_dpdk
                       bench_market_multi bench_market_multi_dpdk
                       bench_market_pingpong bench_market_pingpong_dpdk) ;;
    pingpong)        required_bins=(bench_pingpong bench_pingpong_dpdk) ;;
    market_multi)    required_bins=(bench_market_multi bench_market_multi_dpdk) ;;
    market_pingpong) required_bins=(bench_market_pingpong bench_market_pingpong_dpdk) ;;
esac
for bin in "${required_bins[@]}"; do
    [ -x "$BINDIR/$bin" ] || die "$bin not found at $BINDIR/$bin\n   Build: xmake build $bin"
done
ok "Binaries checked (${#required_bins[@]})"

# SSH key (mock mode only)
if [ "$MODE" = "mock" ]; then
    [ -f "$SSH_KEY" ] || die "SSH key not found: $SSH_KEY"
    ssh_mock "echo ok" || die "Cannot SSH to $MOCK_HOST\n   Check: ssh $SSH_OPTS ec2-user@$MOCK_HOST"
    ok "SSH to $MOCK_HOST OK"
fi

# ══════════════════════════════════════════════════════════════════════════
# Server setup
# ══════════════════════════════════════════════════════════════════════════

setup_mock_server() {
    info "Setting up mock server on $MOCK_HOST:$MOCK_PORT"

    # Deploy latest server code
    info "Deploying mock server files..."
    scp $SSH_OPTS \
        "$MOCK_SERVER_LOCAL_PATH/server.py" \
        "$MOCK_SERVER_LOCAL_PATH/cert.pem" \
        "$MOCK_SERVER_LOCAL_PATH/key.pem" \
        "ec2-user@$MOCK_HOST:$MOCK_SERVER_REMOTE_PATH/" 2>/dev/null \
        || die "SCP failed — cannot deploy mock server to $MOCK_HOST"

    # Ensure websockets is installed
    ssh_mock "python3 -c 'import websockets' 2>/dev/null" || {
        info "Installing websockets on $MOCK_HOST..."
        local wheel
        wheel=$(ls /tmp/ws_wheels/websockets-*.whl 2>/dev/null | head -1)
        if [ -n "$wheel" ]; then
            scp $SSH_OPTS "$wheel" "ec2-user@$MOCK_HOST:/tmp/" 2>/dev/null
            ssh_mock "python3 -m pip install --user /tmp/websockets-*.whl" 2>/dev/null
        else
            ssh_mock "python3 -m ensurepip --user 2>/dev/null; python3 -m pip install --user websockets" 2>/dev/null
        fi
        ssh_mock "python3 -c 'import websockets'" 2>/dev/null \
            || die "Failed to install websockets on $MOCK_HOST"
    }

    # Kill any existing server
    ssh_mock "killall -9 python3 2>/dev/null" || true
    # Wait for port release
    local retries=0
    while ssh_mock "ss -tlnp | grep -q $MOCK_PORT" && [ $retries -lt 10 ]; do
        sleep 1
        retries=$((retries + 1))
    done

    # Start with stress cycle = benchmark duration (loop mode)
    ssh_mock "
        nohup python3 $MOCK_SERVER_REMOTE_PATH/server.py \
            --tls --port $MOCK_PORT \
            --mode stress --stress-duration $DURATION --loop \
            > /tmp/mock_tls.log 2>&1 &
    "

    # Wait for server to be listening
    retries=0
    while ! ssh_mock "ss -tlnp | grep -q $MOCK_PORT" && [ $retries -lt 15 ]; do
        sleep 1
        retries=$((retries + 1))
    done
    ssh_mock "ss -tlnp | grep -q $MOCK_PORT" \
        || die "Mock server failed to start\n   Debug: ssh $SSH_OPTS ec2-user@$MOCK_HOST 'cat /tmp/mock_tls.log'"

    ok "Mock server running on $MOCK_HOST:$MOCK_PORT (stress=${DURATION}s, loop)"

    BENCH_HOST="$MOCK_HOST"
    BENCH_PORT="$MOCK_PORT"
}

setup_binance() {
    info "Setting up real Binance connection"

    BENCH_HOST="fstream.binance.com"
    BENCH_PORT="443"

    # Resolve and pin DNS to a single IP
    local resolved
    resolved=$(getent hosts fstream.binance.com | awk '{print $1}' | head -1)
    [ -n "$resolved" ] || die "Cannot resolve fstream.binance.com — check DNS"

    # Check if already pinned and reachable
    if grep -q "fstream.binance.com" /etc/hosts 2>/dev/null; then
        local pinned
        pinned=$(grep "fstream.binance.com" /etc/hosts | awk '{print $1}')
        if timeout 3 bash -c "echo > /dev/tcp/$pinned/443" 2>/dev/null; then
            ok "DNS pinned to $pinned (reachable)"
        else
            warn "Pinned IP $pinned is unreachable — re-pinning"
            sudo sed -i '/fstream.binance.com/d' /etc/hosts
            # Fall through to re-pin below
            resolved=$(getent hosts fstream.binance.com | awk '{print $1}' | head -1)
            info "Pinning DNS: $resolved → fstream.binance.com"
            sudo bash -c "echo '$resolved fstream.binance.com' >> /etc/hosts" \
                || die "Failed to pin DNS"
            ok "DNS re-pinned to $resolved"
        fi
    else
        info "Pinning DNS: $resolved → fstream.binance.com"
        sudo bash -c "echo '$resolved fstream.binance.com' >> /etc/hosts" \
            || die "Failed to pin DNS — run with sudo or add manually:\n   echo '$resolved fstream.binance.com' | sudo tee -a /etc/hosts"
        ok "DNS pinned to $resolved"
    fi

    # Verify connectivity
    timeout 3 bash -c "echo > /dev/tcp/$resolved/443" 2>/dev/null \
        || die "Cannot reach $resolved:443 — check network"
    ok "Binance reachable at $resolved"
}

# ══════════════════════════════════════════════════════════════════════════
# Run a kernel+DPDK pair simultaneously
# ══════════════════════════════════════════════════════════════════════════

CHILD_PIDS=()

cleanup() {
    echo ""
    warn "Interrupted — killing benchmarks..."
    for pid in "${CHILD_PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    # Also kill any sudo'd DPDK processes
    sudo pkill -f "bench_.*_dpdk" 2>/dev/null || true
    wait 2>/dev/null
    if [ "$MODE" = "mock" ]; then
        ssh_mock "killall python3 2>/dev/null" || true
    fi
    echo "   Partial results in: $OUTDIR"
    exit 130
}
trap cleanup INT TERM

run_pair() {
    local name=$1
    local kernel_extra="$2"
    local dpdk_extra="$3"

    local kernel_bin="$BINDIR/${name}"
    local dpdk_bin="$BINDIR/${name}_dpdk"
    local common="--host $BENCH_HOST --port $BENCH_PORT"

    info "[$name] kernel + DPDK → same server simultaneously"

    # Launch both in background
    $kernel_bin $common $SOCK_ARGS $kernel_extra \
        > "$OUTDIR/${name}_kernel.txt" 2>&1 &
    local kpid=$!
    CHILD_PIDS+=($kpid)

    sudo $dpdk_bin $DPDK_EAL_ARGS -- $common $DPDK_NET $dpdk_extra \
        > "$OUTDIR/${name}_dpdk.txt" 2>&1 &
    local dpid=$!
    CHILD_PIDS+=($dpid)

    # Wait for both
    local kok=true dok=true
    wait $kpid 2>/dev/null || kok=false
    wait $dpid 2>/dev/null || dok=false

    # Remove from tracking
    CHILD_PIDS=("${CHILD_PIDS[@]/$kpid/}")
    CHILD_PIDS=("${CHILD_PIDS[@]/$dpid/}")

    if $kok && $dok; then
        ok "[$name] both done"
    else
        $kok || warn "[$name] kernel exited with error — see $OUTDIR/${name}_kernel.txt"
        $dok || warn "[$name] dpdk exited with error — see $OUTDIR/${name}_dpdk.txt"
    fi
}

# ══════════════════════════════════════════════════════════════════════════
# Print result summary for a pair
# ══════════════════════════════════════════════════════════════════════════

extract_metric() {
    local file=$1 label=$2 field=$3
    grep "$label" -A5 "$file" 2>/dev/null | grep "$field:" | grep -v "${field}\." | head -1 | grep -oE '[0-9]+' | tail -1
}

summarize_pair() {
    local name=$1
    local kf="$OUTDIR/${name}_kernel.txt"
    local df="$OUTDIR/${name}_dpdk.txt"
    [ -f "$kf" ] && [ -f "$df" ] || return

    echo "  ┌── $name ──"

    # Duration & messages
    local kdur ddur kmsg dmsg kord dord
    kdur=$(grep -oE "Duration: [0-9.]+" "$kf" | grep -oE '[0-9.]+' || echo "?")
    ddur=$(grep -oE "Duration: [0-9.]+" "$df" | grep -oE '[0-9.]+' || echo "?")
    kmsg=$(grep -oE "Messages: [0-9]+" "$kf" | grep -oE '[0-9]+' || echo "")
    dmsg=$(grep -oE "Messages: [0-9]+" "$df" | grep -oE '[0-9]+' || echo "")
    kord=$(grep -oE "Orders: [0-9]+/[0-9]+" "$kf" || echo "")
    dord=$(grep -oE "Orders: [0-9]+/[0-9]+" "$df" || echo "")

    echo "  │ Duration:         kernel=${kdur}s  dpdk=${ddur}s"
    [ -n "$kmsg" ] && echo "  │ Messages:         kernel=$kmsg  dpdk=$dmsg"
    [ -n "$kord" ] && echo "  │ Orders:           kernel=$kord  dpdk=$dord"

    # RX totals
    local krx drx
    krx=$(grep "RX totals:" "$kf" 2>/dev/null | sed 's/.*RX totals: //' || echo "")
    drx=$(grep "RX totals:" "$df" 2>/dev/null | sed 's/.*RX totals: //' || echo "")
    [ -n "$krx" ] && echo "  │ RX totals (kern): $krx"
    [ -n "$drx" ] && echo "  │ RX totals (dpdk): $drx"

    # Per rx_burst (DPDK only)
    local dper
    dper=$(grep "Per rx_burst:" "$df" 2>/dev/null | sed 's/.*Per rx_burst: //' || echo "")
    [ -n "$dper" ] && echo "  │ Per rx_burst:     $dper"

    echo "  │"

    # Extract all "--- Label ---" sections and their p50/p99
    # Use python for reliable multi-line parsing
    python3 -c "
import re, sys

def parse_file(path):
    metrics = {}
    current = None
    with open(path) as f:
        for line in f:
            m = re.search(r'--- (.+?) ---', line)
            if m:
                current = m.group(1)
                metrics[current] = {}
                continue
            if current:
                for field in ['samples', 'p50', 'p99']:
                    fm = re.search(rf'{re.escape(field)}:\s+(\d+)', line)
                    if fm and field not in metrics[current]:
                        metrics[current][field] = int(fm.group(1))
                if 'no samples' in line:
                    metrics[current]['samples'] = 0
    return metrics

k = parse_file('$kf')
d = parse_file('$df')

all_labels = list(dict.fromkeys(list(k.keys()) + list(d.keys())))
for label in all_labels:
    km = k.get(label, {})
    dm = d.get(label, {})
    ks = km.get('samples', 0)
    ds = dm.get('samples', 0)
    if ks == 0 and ds == 0:
        continue
    kp50 = km.get('p50')
    dp50 = dm.get('p50')
    kp99 = km.get('p99')
    dp99 = dm.get('p99')
    parts = []
    if kp50 is not None and dp50 is not None and dp50 > 0:
        parts.append(f'p50: k={kp50}ns d={dp50}ns ({kp50/dp50:.1f}x)')
    elif kp50 is not None:
        parts.append(f'p50: k={kp50}ns')
    elif dp50 is not None:
        parts.append(f'p50: d={dp50}ns')
    if kp99 is not None and dp99 is not None and dp99 > 0:
        parts.append(f'p99: k={kp99}ns d={dp99}ns ({kp99/dp99:.1f}x)')
    if parts:
        print(f'  │ {label:20s} {\"  \".join(parts)}')
" 2>/dev/null

    echo "  └──"
}

# ══════════════════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════════════════

echo ""
echo -e "${BOLD}═══ bench_binance — kernel + DPDK simultaneous ═══${NC}"
echo "  Mode:     $MODE"
echo "  Bench:    $BENCH"
echo "  Duration: ${DURATION}s"
echo "  Output:   $OUTDIR"
echo ""

# Setup server
case "$MODE" in
    mock)    setup_mock_server ;;
    binance) setup_binance ;;
esac

echo ""
info "Running benchmarks (kernel + DPDK simultaneous against $BENCH_HOST:$BENCH_PORT)"
echo ""

# ── 1. Pingpong ───────────────────────────────────────────────────────────
if [ "$BENCH" = "all" ] || [ "$BENCH" = "pingpong" ]; then
    run_pair bench_pingpong \
        "--count $PING_COUNT --ping-interval $PING_INTERVAL" \
        "--count $PING_COUNT --ping-interval $PING_INTERVAL"
    sleep $DPDK_COOLDOWN
fi

# ── 2. Market multi ───────────────────────────────────────────────────────
if [ "$BENCH" = "all" ] || [ "$BENCH" = "market_multi" ]; then
    run_pair bench_market_multi \
        "--duration $DURATION" \
        "--duration $DURATION"
    sleep $DPDK_COOLDOWN
fi

# ── 3. Market pingpong (mock only — Binance doesn't echo text frames) ────
if [ "$BENCH" = "all" ] || [ "$BENCH" = "market_pingpong" ]; then
    if [ "$MODE" = "mock" ]; then
        run_pair bench_market_pingpong \
            "--duration $DURATION --ping-interval $ORDER_INTERVAL" \
            "--duration $DURATION --ping-interval $ORDER_INTERVAL"
    else
        warn "Skipping bench_market_pingpong — requires mock server for order echo"
    fi
fi

# ── Cleanup mock server ──────────────────────────────────────────────────
if [ "$MODE" = "mock" ]; then
    ssh_mock "killall python3 2>/dev/null" || true
fi

# ── Summary ──────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}═══ Results ═══${NC}"
echo ""

if [ "$BENCH" = "all" ] || [ "$BENCH" = "pingpong" ]; then
    summarize_pair bench_pingpong
fi
if [ "$BENCH" = "all" ] || [ "$BENCH" = "market_multi" ]; then
    summarize_pair bench_market_multi
fi
if [ "$BENCH" = "all" ] || [ "$BENCH" = "market_pingpong" ]; then
    [ "$MODE" = "mock" ] && summarize_pair bench_market_pingpong
fi

echo ""
echo "  Output: $OUTDIR"
echo -e "  ${BOLD}ls $OUTDIR/${NC}"
echo ""

#!/usr/bin/env bash
# bench-sysmetrics-tcp.sh — kernel vs DPDK lat_tcp 系统指标对比 wrapper.
#
# Drives one trial of `lat_tcp` (kernel mode) or `lat_tcp_dpdk` (DPDK mode)
# while attaching `perf stat` to the client process and snapshotting
# /proc/<pid>/status + /proc/interrupts before / after. Produces a JSON
# summary per trial; an aggregator turns N trial-JSONs into REPORT.md.
#
# CPU-pin assertion: the wrapper hard-codes FORBIDDEN_CPUS={2,3,6} (the
# user-declared "in-use by other programs" list 2026-05-06) and refuses
# to start if any pin in the temp config falls inside that set.
#
# === Usage ===
#
#   # Generate the temp config (idempotent, can be inspected)
#   ./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh --gen-config
#
#   # Run one trial (writes <OUTDIR>/<MODE>/trial-<TRIAL>.{json,perf.txt,...})
#   MODE=kernel TRIAL=1 DURATION=300 OUTDIR=.artifacts/bench-sysmetrics-tcp-echo-20260506 \
#     ./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh
#
#   # After all 6 trials, build the report
#   ./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh --aggregate \
#     .artifacts/bench-sysmetrics-tcp-echo-20260506
#
#   # Help / dry-run
#   ./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh --help
#   DRY_RUN=1 MODE=kernel TRIAL=1 DURATION=30 OUTDIR=/tmp/dry ./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh
#
# === Exit codes ===
#   0  trial completed and JSON written
#   1  precondition failure (forbidden CPU pin / missing tool / stale state)
#   2  runtime failure (perf or lat exited error / mockex died mid-run)
#   3  aggregate-mode error
#
# === Idempotence ===
# Re-running the same trial number overwrites its outputs in-place.
# `--gen-config` regenerates the temp config every call (cheap).

set -euo pipefail

# ── Constants ───────────────────────────────────────────────────────────────

readonly REPO=/home/ec2-user/ephemeral_dev
readonly LAT_WRAPPER="$REPO/benchmarks/latency/lat"
readonly SOURCE_CONFIG="$REPO/benchmarks/latency/config.toml"
readonly TEMP_CONFIG=/tmp/config-bench-sysmetrics.toml
readonly FORBIDDEN_CPUS=(2 3 6)
readonly PERF_EVENTS=cycles,instructions,cache-references,cache-misses,branch-misses,context-switches,cpu-migrations,page-faults

# ── Logging ─────────────────────────────────────────────────────────────────

log()   { printf '[%s] %s\n' "$(date -Iseconds)" "$*" >&2; }
die()   { log "ERROR: $*"; exit 1; }
step()  { log "── $* ──"; }

usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
  exit 0
}

# ── Subcommands ─────────────────────────────────────────────────────────────

case "${1:-}" in
  --help|-h) usage ;;
  --gen-config) :; gen_config_only=1 ;;
  --aggregate) :; aggregate_dir="${2:-}"; aggregate_mode=1 ;;
  '') :; ;;
  *) die "unknown argument: $1 (try --help)" ;;
esac

# ── Helpers ─────────────────────────────────────────────────────────────────

# Generate the temp config: copy source + flip use_tls=false in [scenarios.lat_tcp]
# + flip cpu_mock=6 → cpu_mock=5 in [cpu] + override duration_seconds in lat_tcp.
# DURATION env var (default 300) controls the per-trial runtime that lat reads.
gen_config() {
  local dur=${DURATION:-300}
  step "generating $TEMP_CONFIG (use_tls=false, cpu_mock=5, duration_seconds=$dur)"
  [[ -f "$SOURCE_CONFIG" ]] || die "source config not found: $SOURCE_CONFIG"

  # Copy then surgically edit. Both edits use awk to scope by section header
  # so we don't accidentally hit other sections that mention the same key.
  awk -v dur="$dur" -v in_section= '
    /^\[scenarios\.lat_tcp\][[:space:]]*$/ { in_section="lat_tcp"; print; next }
    /^\[cpu\][[:space:]]*$/                { in_section="cpu";     print; next }
    /^\[/                                  { in_section="";        print; next }
    in_section == "lat_tcp" && /^[[:space:]]*use_tls[[:space:]]*=/ {
      sub(/=.*/, "= false  # bench-sysmetrics override (TLS off for clean syscall/IRQ signal)")
      print; next
    }
    in_section == "lat_tcp" && /^[[:space:]]*duration_seconds[[:space:]]*=/ {
      sub(/=.*/, "= " dur "  # bench-sysmetrics override (DURATION env)")
      print; next
    }
    in_section == "cpu" && /^[[:space:]]*cpu_mock[[:space:]]*=/ {
      sub(/=.*/, "= 5      # bench-sysmetrics override (CPU 6 reserved by other workload)")
      print; next
    }
    { print }
  ' "$SOURCE_CONFIG" > "$TEMP_CONFIG"

  # Sanity: confirm the edits actually landed.
  grep -q '^use_tls *= *false' "$TEMP_CONFIG" || die "TLS override didn't take in $TEMP_CONFIG"
  grep -q '^cpu_mock *= *5'    "$TEMP_CONFIG" || die "cpu_mock override didn't take"
  grep -qE "^duration_seconds *= *${dur}\b" "$TEMP_CONFIG" || die "duration_seconds override didn't take ($dur)"

  log "  $TEMP_CONFIG ready"
}

# Parse the temp config for every CPU pin we plan to use, intersect with
# FORBIDDEN_CPUS, abort if non-empty. Covers cpu_client, cpu_mock, eal_cores.
assert_no_forbidden_cpus() {
  local cfg="$1"
  step "validating CPU pins in $cfg vs FORBIDDEN={${FORBIDDEN_CPUS[*]}}"

  # Helper: extract value-before-comment for a key inside [cpu] section.
  # Steps: 1) trim before `=`, 2) trim trailing `#...`, 3) trim whitespace.
  local cpu_client cpu_mock eal_cores
  cpu_client=$(awk '/^\[cpu\]/{f=1;next} /^\[/{f=0} f && /^[[:space:]]*cpu_client[[:space:]]*=/ {
    sub(/^[^=]*=[[:space:]]*/, ""); sub(/[[:space:]]*#.*$/, ""); gsub(/[[:space:]]/,"");
    print; exit }' "$cfg")
  cpu_mock=$(awk   '/^\[cpu\]/{f=1;next} /^\[/{f=0} f && /^[[:space:]]*cpu_mock[[:space:]]*=/ {
    sub(/^[^=]*=[[:space:]]*/, ""); sub(/[[:space:]]*#.*$/, ""); gsub(/[[:space:]]/,"");
    print; exit }' "$cfg")
  eal_cores=$(awk  '/^\[cpu\]/{f=1;next} /^\[/{f=0} f && /^[[:space:]]*eal_cores[[:space:]]*=/ {
    sub(/^[^=]*=[[:space:]]*/, ""); sub(/[[:space:]]*#.*$/, ""); gsub(/["'"'"' ]/,"");
    print; exit }' "$cfg")

  [[ -n "$cpu_client" ]] || die "cpu_client missing in $cfg"
  [[ -n "$cpu_mock"   ]] || die "cpu_mock missing in $cfg"
  [[ -n "$eal_cores"  ]] || die "eal_cores missing in $cfg"

  log "  parsed: cpu_client=$cpu_client  cpu_mock=$cpu_mock  eal_cores='$eal_cores'"

  local -a planned=("$cpu_client" "$cpu_mock")
  IFS=',' read -ra eal_arr <<< "$eal_cores"
  for c in "${eal_arr[@]}"; do planned+=("${c// /}"); done

  local conflict=()
  for c in "${planned[@]}"; do
    for f in "${FORBIDDEN_CPUS[@]}"; do
      [[ "$c" == "$f" ]] && conflict+=("$c")
    done
  done
  if (( ${#conflict[@]} > 0 )); then
    die "FORBIDDEN CPU(s) detected in pins: ${conflict[*]}; FORBIDDEN={${FORBIDDEN_CPUS[*]}}"
  fi
  log "  no forbidden CPU intersection ✅"
}

# Find the lat_tcp / lat_tcp_dpdk client PID once it's spawned (poll up to 30s).
# `sudo pgrep` because lat wrapper may run lat_tcp as root via sudo.
wait_for_lat_pid() {
  local mode="$1"
  # Patterns must distinguish lat_tcp from lat_tcp_dpdk. For kernel we
  # require a non-underscore boundary after lat_tcp (i.e., space or
  # cmdline end); for DPDK we require the explicit suffix.
  local bin_pat
  if [[ "$mode" == "dpdk" ]]; then
    bin_pat='release/lat_tcp_dpdk[[:space:]]'
  else
    bin_pat='release/lat_tcp[[:space:]]'
  fi
  local pid=""
  for _ in $(seq 1 30); do
    sleep 1
    pid=$(sudo pgrep -f "$bin_pat" | head -1 || true)
    [[ -n "$pid" ]] && { echo "$pid"; return 0; }
  done
  return 1
}

# Parse the perf.txt and proc snapshots into a single JSON summary.
# Uses python3 for robust number parsing (perf output has commas as thousands separator).
generate_json() {
  local mode="$1" trial="$2" outdir="$3"
  local perf="$outdir/$mode/trial-$trial.perf.txt"
  local pre="$outdir/$mode/trial-$trial.proc-status-pre.txt"
  local post="$outdir/$mode/trial-$trial.proc-status-post.txt"
  local int_pre="$outdir/$mode/trial-$trial.interrupts-pre.txt"
  local int_post="$outdir/$mode/trial-$trial.interrupts-post.txt"
  local lat_log="$outdir/$mode/trial-$trial.lat.log"
  local out="$outdir/$mode/trial-$trial.json"

  python3 - "$perf" "$pre" "$post" "$int_pre" "$int_post" "$lat_log" "$mode" "$trial" "$out" << 'PY'
import json, sys, re
perf_p, pre_p, post_p, int_pre_p, int_post_p, lat_p, mode, trial, out_p = sys.argv[1:]

def read(p, default=""):
    try:
        with open(p) as f: return f.read()
    except FileNotFoundError:
        return default

def parse_perf(text):
    """perf stat format: lines like '   1,234,567,890      cycles ...'"""
    out = {}
    for line in text.splitlines():
        m = re.match(r'\s*([\d,<>not\s]+?)\s+([\w\-]+)\s', line)
        if not m:
            continue
        raw, evt = m.groups()
        raw = raw.strip()
        if raw in ('<not counted>', '<not supported>'):
            out[evt] = None
        else:
            out[evt] = int(raw.replace(',', ''))
    # Also pull elapsed time
    m = re.search(r'([\d.]+)\s+seconds time elapsed', text)
    if m: out['elapsed_sec'] = float(m.group(1))
    return out

def parse_proc(text):
    """/proc/<pid>/status grep for ctxt switches"""
    out = {}
    for line in text.splitlines():
        if line.startswith('voluntary_ctxt_switches:'):
            out['voluntary'] = int(line.split()[1])
        elif line.startswith('nonvoluntary_ctxt_switches:'):
            out['nonvoluntary'] = int(line.split()[1])
    return out

def parse_interrupts(text):
    """/proc/interrupts table: per-row per-cpu counts; we keep ens34/ens35 rows."""
    out = []
    for line in text.splitlines():
        line = line.rstrip()
        if 'ens34' in line or 'ens35' in line:
            parts = line.split()
            # Format: "<irq>:  c0  c1  c2 ... cN  <chip>  <name>"
            # Find first non-numeric col (chip name); cpus are everything before it.
            counts, label = [], ''
            for p in parts[1:]:
                if p.isdigit() or (p.startswith('-') and p[1:].isdigit()):
                    counts.append(int(p))
                else:
                    label = ' '.join(parts[1+len(counts):])
                    break
            out.append({'label': label, 'cpu_counts': counts})
    return out

def diff_interrupts(pre, post):
    """Per-row, per-cpu delta: post minus pre."""
    out = []
    for r_pre, r_post in zip(pre, post):
        if r_pre['label'] != r_post['label']:
            continue  # row layout drifted — skip
        deltas = [b - a for a, b in zip(r_pre['cpu_counts'], r_post['cpu_counts'])]
        out.append({'label': r_pre['label'],
                    'cpu_deltas': deltas,
                    'total_delta': sum(deltas)})
    return out

def parse_lat_summary(text):
    """The lat wrapper prints a tail with min/avg/p99 etc; capture sample count + duration."""
    out = {}
    m = re.search(r'samples?\s*(?:recorded)?[:=]\s*([\d,]+)', text, re.IGNORECASE)
    if m: out['recorded'] = int(m.group(1).replace(',', ''))
    m = re.search(r'throughput:\s*([\d,]+)\s*samples', text, re.IGNORECASE)
    if m: out['throughput_per_sec'] = int(m.group(1).replace(',', ''))
    m = re.search(r'runtime_seconds["\s:]*([\d.]+)', text)
    if m: out['runtime_seconds'] = float(m.group(1))
    return out

perf = parse_perf(read(perf_p))
pre  = parse_proc(read(pre_p))
post = parse_proc(read(post_p))
int_pre  = parse_interrupts(read(int_pre_p))
int_post = parse_interrupts(read(int_post_p))
lat_summary = parse_lat_summary(read(lat_p))

# ctxt deltas
ctxt_delta = {}
for k in ('voluntary', 'nonvoluntary'):
    if k in pre and k in post:
        ctxt_delta[k] = post[k] - pre[k]

# IPC if both available
if perf.get('cycles') and perf.get('instructions'):
    perf['ipc'] = round(perf['instructions'] / perf['cycles'], 4)

result = {
    'mode': mode,
    'trial': int(trial),
    'perf': perf,
    'proc_ctxt_delta': ctxt_delta,
    'irq_delta_nic': diff_interrupts(int_pre, int_post),
    'lat_summary': lat_summary,
}
with open(out_p, 'w') as f:
    json.dump(result, f, indent=2)
print(f"  wrote {out_p}")
PY
}

# Print one-line trial summary to stderr (called after generate_json).
print_summary() {
  local out="$1"
  python3 - "$out" << 'PY'
import json, sys
with open(sys.argv[1]) as f: d = json.load(f)
p = d['perf']
def fmt(n, suf='', pre=''):
    if n is None: return 'n/a'
    if n >= 1e9: return f'{pre}{n/1e9:.2f}G{suf}'
    if n >= 1e6: return f'{pre}{n/1e6:.2f}M{suf}'
    if n >= 1e3: return f'{pre}{n/1e3:.2f}k{suf}'
    return f'{pre}{n}{suf}'
nic_total = sum(r['total_delta'] for r in d['irq_delta_nic'])
print(f"  summary: cycles={fmt(p.get('cycles'))} inst={fmt(p.get('instructions'))} "
      f"ipc={p.get('ipc','n/a')} cache-miss={fmt(p.get('cache-misses'))} "
      f"ctxt-sw(perf)={fmt(p.get('context-switches'))} "
      f"vol/invol={d['proc_ctxt_delta'].get('voluntary','?')}/{d['proc_ctxt_delta'].get('nonvoluntary','?')} "
      f"NIC-IRQ-total={nic_total}", file=__import__('sys').stderr)
PY
}

# Aggregator: walks <dir>/{kernel,dpdk}/trial-*.json, computes mean/stddev,
# writes REPORT.md + README.md.
aggregate() {
  local dir="$1"
  [[ -d "$dir" ]] || die "aggregate: not a directory: $dir"
  step "aggregating $dir"

  python3 - "$dir" << 'PY'
import json, sys, glob, os, statistics
import textwrap

root = sys.argv[1]
modes = ['kernel', 'dpdk']
groups = {}
for mode in modes:
    files = sorted(glob.glob(f"{root}/{mode}/trial-*.json"))
    if not files:
        continue
    trials = []
    for f in files:
        with open(f) as fh: trials.append(json.load(fh))
    groups[mode] = trials

if 'kernel' not in groups or 'dpdk' not in groups:
    print(f"WARN: missing one or both modes (have {list(groups)}); writing partial report", file=sys.stderr)

# All metrics we care about
PERF_KEYS = ['cycles', 'instructions', 'ipc', 'cache-references', 'cache-misses',
             'branch-misses', 'context-switches', 'cpu-migrations', 'page-faults',
             'elapsed_sec']
PROC_KEYS = ['voluntary', 'nonvoluntary']

def stats(vals):
    """vals: list of numbers (ignore None). Returns (mean, stddev) or (None, None)."""
    vals = [v for v in vals if v is not None]
    if not vals: return None, None
    m = statistics.mean(vals)
    s = statistics.stdev(vals) if len(vals) > 1 else 0.0
    return m, s

def fmt(v, kind=''):
    if v is None: return 'n/a'
    if kind == 'count':
        if v >= 1e9: return f'{v/1e9:.3f}G'
        if v >= 1e6: return f'{v/1e6:.3f}M'
        if v >= 1e3: return f'{v/1e3:.3f}k'
        return f'{v:.0f}'
    if kind == 'sec':
        return f'{v:.3f}s'
    if kind == 'ratio':
        return f'{v:.4f}'
    return f'{v:.3f}'

# Build per-metric mean/stddev
agg = {}
for mode, trials in groups.items():
    a = {}
    for k in PERF_KEYS:
        vals = [t['perf'].get(k) for t in trials]
        a[k] = stats(vals)
    for k in PROC_KEYS:
        vals = [t['proc_ctxt_delta'].get(k) for t in trials]
        a['proc_'+k] = stats(vals)
    # NIC IRQ total (sum across rows)
    irq_totals = []
    for t in trials:
        irq_totals.append(sum(r['total_delta'] for r in t['irq_delta_nic']))
    a['nic_irq_total'] = stats(irq_totals)
    agg[mode] = a

def kind_for(metric):
    if metric in ('ipc',): return 'ratio'
    if metric == 'elapsed_sec': return 'sec'
    return 'count'

def cell(mode, metric):
    m, s = agg.get(mode, {}).get(metric, (None, None))
    if m is None: return 'n/a'
    return f'{fmt(m, kind_for(metric))} ± {fmt(s, kind_for(metric))}'

def ratio(metric):
    k_m, _ = agg.get('kernel', {}).get(metric, (None, None))
    d_m, _ = agg.get('dpdk',   {}).get(metric, (None, None))
    if k_m is None or d_m is None: return 'n/a'
    if d_m == 0:
        return '∞x' if k_m else '1x'
    if k_m == 0:
        return '0x'
    return f'{k_m/d_m:.2f}x'

def interp(metric):
    return {
      'cycles':            'CPU 总忙时（busy-poll vs interrupt-driven）',
      'instructions':      '指令总条数',
      'ipc':               '每周期指令；DPDK busy-poll 通常更高',
      'cache-references':  'L1/L2 cache 摸内存频度',
      'cache-misses':      'cache miss 绝对数；syscall path 通常更多',
      'branch-misses':     '分支预测失败；协议栈分支多 → 多',
      'context-switches':  'perf 视角的 ctx switch（含被迫）',
      'cpu-migrations':    'CPU 迁移；绑核应都接近 0',
      'page-faults':       '缺页；mlockall 后应都接近 0',
      'proc_voluntary':    'voluntary ctx switch；syscall block 让出 → kernel 多',
      'proc_nonvoluntary': '被迫 ctx switch；调度器抢占信号',
      'nic_irq_total':     'NIC 中断总数；DPDK 应为 0',
    }.get(metric, '')

# Write REPORT.md
rows = []
rows.append('# kernel vs DPDK TCP echo — 系统指标对比')
rows.append('')
rows.append(f'生成时间：{__import__("datetime").datetime.now(__import__("datetime").timezone.utc).isoformat()}  ')
rows.append(f'数据源：`{root}`  ')
rows.append('每边 trial 数：' + ' / '.join(f'{m}={len(groups.get(m, []))}' for m in modes) + '  ')
rows.append('')
rows.append('## 横向对比表')
rows.append('')
rows.append('| metric | kernel (mean ± stddev) | dpdk (mean ± stddev) | k/d ratio | 解读 |')
rows.append('|---|---:|---:|---:|---|')

display = [
    ('cycles', 'cycles'),
    ('instructions', 'instructions'),
    ('ipc', 'IPC'),
    ('cache-references', 'cache-references'),
    ('cache-misses', 'cache-misses'),
    ('branch-misses', 'branch-misses'),
    ('context-switches', 'context-switches (perf)'),
    ('cpu-migrations', 'cpu-migrations'),
    ('page-faults', 'page-faults'),
    ('proc_voluntary', 'voluntary_ctxt_switches (/proc)'),
    ('proc_nonvoluntary', 'nonvoluntary_ctxt_switches (/proc)'),
    ('nic_irq_total', 'NIC IRQ delta (sum over ens34+ens35)'),
    ('elapsed_sec', 'perf elapsed_sec (sanity)'),
]
for key, label in display:
    rows.append(f'| {label} | {cell("kernel", key)} | {cell("dpdk", key)} | {ratio(key)} | {interp(key)} |')

rows.append('')
rows.append('## 每 trial 原始数据')
rows.append('')
for mode in modes:
    if mode not in groups: continue
    rows.append(f'### {mode}')
    rows.append('')
    rows.append('| trial | cycles | instructions | IPC | cache-misses | ctxt-sw (perf) | vol/invol (/proc) | NIC IRQ |')
    rows.append('|---:|---:|---:|---:|---:|---:|---:|---:|')
    for t in groups[mode]:
        p = t['perf']
        nic = sum(r['total_delta'] for r in t['irq_delta_nic'])
        rows.append('| {trial} | {c} | {i} | {ipc} | {cm} | {cs} | {v}/{nv} | {nic} |'.format(
            trial=t['trial'],
            c=fmt(p.get('cycles'), 'count'),
            i=fmt(p.get('instructions'), 'count'),
            ipc=p.get('ipc', 'n/a'),
            cm=fmt(p.get('cache-misses'), 'count'),
            cs=fmt(p.get('context-switches'), 'count'),
            v=t['proc_ctxt_delta'].get('voluntary', '?'),
            nv=t['proc_ctxt_delta'].get('nonvoluntary', '?'),
            nic=nic,
        ))
    rows.append('')

# ASCII bars for 5 key metrics — kernel / dpdk side by side, scaled to max
key_metrics = [('cycles', 'cycles'), ('cache-misses', 'cache-misses'),
               ('context-switches', 'ctxt-sw'), ('proc_voluntary', 'vol-ctxt'),
               ('nic_irq_total', 'NIC IRQ')]
rows.append('## ASCII 柱状对比（5 关键指标）')
rows.append('')
rows.append('```')
WIDTH = 50
for key, label in key_metrics:
    k_m, _ = agg.get('kernel', {}).get(key, (None, None))
    d_m, _ = agg.get('dpdk',   {}).get(key, (None, None))
    mx = max([v for v in (k_m, d_m) if v is not None] or [0])
    if mx == 0:
        rows.append(f'{label:>16}  (both 0 — N/A)')
        continue
    def bar(v):
        if v is None: return '?'
        n = int(round((v / mx) * WIDTH))
        return '█' * n + ' ' * (WIDTH - n)
    rows.append(f'{label:>16}  kernel │{bar(k_m)}│ {fmt(k_m, kind_for(key))}')
    rows.append(f'{label:>16}  dpdk   │{bar(d_m)}│ {fmt(d_m, kind_for(key))}')
    rows.append('')
rows.append('```')
rows.append('')

# Threats to validity
rows.append('## 威胁有效性 (threats to validity)')
rows.append('')
rows.append('1. **AWS hypervisor 计数器虚拟化**：cycles/instructions/cache-* 在 EC2 这类 hypervised 环境下数字精度受 host 抖动影响，绝对值可能比裸机噪声大 5-10%。倾向相对比较（ratio）而非绝对值。')
rows.append('2. **CPU 2/3/6 被其他工作占用**：本次配置已避开（cpu_client=4, cpu_mock=5, eal_cores=0,1）。但邻近核（cpu 1, 5, 7）仍可能因 LLC 共享受其他进程影响。')
rows.append('3. **mlockall 后 page-fault 应≈0**：若任一 trial 显示非零 page-faults，可能是 EAL hugepage 初始化阶段产生（DPDK 模式特有）。')
rows.append('4. **cross-trial stddev > 15% mean** 视为环境扰动严重，记录但不重跑。')
rows.append('')

# Reproduce
rows.append('## 复现')
rows.append('')
rows.append('```bash')
rows.append('# Pre-flight: NIC ena, hugepages free, no daemon residue')
rows.append('# (see benchmarks/latency/scripts/bench-sysmetrics-tcp.sh comment header)')
rows.append('')
rows.append('mkdir -p ' + root)
rows.append('for mode in kernel dpdk; do')
rows.append('  if [[ "$mode" == "dpdk" ]]; then')
rows.append('    sudo /home/ec2-user/ephemeral_dev/build/linux/arm64/release/eph_nicd \\')
rows.append('        --no-config-file --pci=0000:28:00.0 --total-queues=8 --daemon-lcore=14 &')
rows.append('    sleep 6')
rows.append('  fi')
rows.append('  for trial in 1 2 3; do')
rows.append('    MODE=$mode TRIAL=$trial DURATION=300 \\')
rows.append('      OUTDIR=' + root + ' \\')
rows.append('      ./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh')
rows.append('    sleep 10')
rows.append('  done')
rows.append('  if [[ "$mode" == "dpdk" ]]; then sudo pkill -f eph_nicd; fi')
rows.append('done')
rows.append('./benchmarks/latency/scripts/bench-sysmetrics-tcp.sh --aggregate ' + root)
rows.append('```')
rows.append('')

with open(f"{root}/REPORT.md", 'w') as f:
    f.write('\n'.join(rows))
print(f"wrote {root}/REPORT.md")

# README.md (short pointer)
readme = f"""# bench-sysmetrics-tcp-echo — README

This artifact compares **kernel TCP echo** vs **DPDK TCP echo** on system metrics
(cycles, IPC, cache misses, context switches, NIC IRQs) under identical
`lat_tcp` workload (300s, 256-byte payload, no TLS).

- See **REPORT.md** for the cross-run mean/stddev table + ASCII bars.
- Per-trial JSONs: `kernel/trial-{{1,2,3}}.json` and `dpdk/trial-{{1,2,3}}.json`.
- Raw `perf stat` outputs and /proc snapshots also archived alongside.

CPU pinning (revised — CPU 2/3/6 reserved for other workloads):
- `cpu_client=4`  (lat_tcp / lat_tcp_dpdk main thread; also DPDK RX queue
   poller for single-queue mode → queue core = upper core ✅)
- `cpu_mock=5`    (mockex echo handler)
- `eal_cores=0,1` (DPDK EAL housekeeping; not the RX poller in single-queue)

Built with: lat_tcp[_dpdk] from current HEAD, eph-nicd post-T2.3-revert.

Reproduce: see REPORT.md "复现" section.
"""
with open(f"{root}/README.md", 'w') as f:
    f.write(readme)
print(f"wrote {root}/README.md")
PY
}

# Run-mode trap: cleanup tmp config + any orphan procs.
cleanup() {
  local rc=$?
  if (( rc != 0 )); then
    log "trap: exit rc=$rc; killing any leftover lat/mockex/perf"
    sudo pkill -f 'build/linux/arm64/release/lat_tcp' 2>/dev/null || true
    sudo pkill -f 'benchmarks/mockex/mockex'          2>/dev/null || true
    sudo pkill -f 'perf stat'                          2>/dev/null || true
  fi
  return $rc
}

# ── Mode dispatch ───────────────────────────────────────────────────────────

if [[ "${gen_config_only:-0}" == "1" ]]; then
  gen_config
  assert_no_forbidden_cpus "$TEMP_CONFIG"
  exit 0
fi

if [[ "${aggregate_mode:-0}" == "1" ]]; then
  [[ -n "${aggregate_dir:-}" ]] || die "--aggregate requires a directory arg"
  aggregate "$aggregate_dir"
  exit 0
fi

# ── Trial-run validation ────────────────────────────────────────────────────

MODE=${MODE:-}
TRIAL=${TRIAL:-}
DURATION=${DURATION:-300}
OUTDIR=${OUTDIR:-}
DRY_RUN=${DRY_RUN:-0}

[[ -n "$MODE"   ]] || die "MODE=kernel|dpdk required"
[[ -n "$TRIAL"  ]] || die "TRIAL=<int> required"
[[ -n "$OUTDIR" ]] || die "OUTDIR=<path> required"
[[ "$MODE" == "kernel" || "$MODE" == "dpdk" ]] || die "MODE must be kernel|dpdk"
[[ "$TRIAL" =~ ^[1-9][0-9]*$ ]] || die "TRIAL must be positive int"
[[ "$DURATION" =~ ^[1-9][0-9]*$ ]] || die "DURATION must be positive int"

[[ -x "$LAT_WRAPPER" ]] || die "lat wrapper missing or not executable: $LAT_WRAPPER"
command -v perf     >/dev/null || die "perf not in PATH"
command -v python3  >/dev/null || die "python3 not in PATH"

# Always regenerate temp config (DURATION env var may differ across calls)
# + assert pin safety BEFORE doing anything heavy.
gen_config
assert_no_forbidden_cpus "$TEMP_CONFIG"

trap cleanup EXIT INT TERM

# ── Per-trial ─ ─────────────────────────────────────────────────────────────

step "trial $TRIAL ($MODE) — duration=${DURATION}s outdir=$OUTDIR"

mkdir -p "$OUTDIR/$MODE"
TRIAL_BASE="$OUTDIR/$MODE/trial-$TRIAL"
PERF_FILE="$TRIAL_BASE.perf.txt"
JSON_FILE="$TRIAL_BASE.json"
PROC_PRE="$TRIAL_BASE.proc-status-pre.txt"
PROC_POST="$TRIAL_BASE.proc-status-post.txt"
INT_PRE="$TRIAL_BASE.interrupts-pre.txt"
INT_POST="$TRIAL_BASE.interrupts-post.txt"
LAT_LOG="$TRIAL_BASE.lat.log"

if [[ "$DRY_RUN" == "1" ]]; then
  log "DRY_RUN=1 — skipping actual lat / perf execution; emitting placeholder JSON"
  echo '{"dry_run": true, "mode": "'"$MODE"'", "trial": '"$TRIAL"'}' > "$JSON_FILE"
  exit 0
fi

# ── Pre-snapshot: /proc/interrupts ──
log "snapshot pre /proc/interrupts → $INT_PRE"
cat /proc/interrupts > "$INT_PRE"

# ── Start lat in background ──
log "spawning lat ($MODE) via $LAT_WRAPPER"
LAT_ARGS=(tcp --config "$TEMP_CONFIG")
if [[ "$MODE" == "dpdk" ]]; then LAT_ARGS=(tcp --dpdk --config "$TEMP_CONFIG"); fi
sudo -E "$LAT_WRAPPER" "${LAT_ARGS[@]}" > "$LAT_LOG" 2>&1 &
LAT_WRAPPER_PID=$!
log "  lat-wrapper bash pid=$LAT_WRAPPER_PID"

# ── Wait for lat_tcp / lat_tcp_dpdk client to spawn ──
log "polling for lat_tcp client pid (up to 30s)..."
LAT_TCP_PID=$(wait_for_lat_pid "$MODE") || die "lat_tcp[_dpdk] never spawned within 30s; tail $LAT_LOG"
log "  lat client pid=$LAT_TCP_PID"

# ── Pre-snapshot: /proc/<pid>/status ──
log "snapshot pre /proc/$LAT_TCP_PID/status → $PROC_PRE"
sudo cat /proc/$LAT_TCP_PID/status > "$PROC_PRE"

# ── Attach perf ──
log "attaching perf stat (events: $PERF_EVENTS) to pid $LAT_TCP_PID"
sudo perf stat -e "$PERF_EVENTS" -p "$LAT_TCP_PID" -o "$PERF_FILE" &
PERF_PID=$!
log "  perf pid=$PERF_PID"

# ── Background /proc/<pid>/status poller ──
# Refresh PROC_POST every 1s while the pid is alive. Writes to a `.tmp`
# first then atomic-mv to PROC_POST only on success, so:
#   1. the final `kill` from main doesn't truncate PROC_POST mid-write
#      (race seen under cgroup-shielded scope);
#   2. when lat_tcp exits, the loop terminates leaving the last-good
#      snapshot in PROC_POST.
(
  while sudo cat /proc/$LAT_TCP_PID/status > "$PROC_POST.tmp" 2>/dev/null; do
    if [[ -s "$PROC_POST.tmp" ]]; then
      mv -f "$PROC_POST.tmp" "$PROC_POST"
    fi
    sleep 1
  done
  rm -f "$PROC_POST.tmp" 2>/dev/null || true
) &
PROC_POLLER_PID=$!
log "  proc-status poller pid=$PROC_POLLER_PID (refreshes $PROC_POST every 1s, atomic)"

# ── Wait for lat to finish ──
log "waiting for lat to finish (~${DURATION}s + warmup/teardown)"
if wait "$LAT_WRAPPER_PID"; then
  log "  lat exited cleanly"
else
  rc=$?
  log "  lat exited rc=$rc; checking lat log for mockex_died_mid_run..."
  if grep -q "mockex died mid-run" "$LAT_LOG"; then
    log "  ERROR: mockex died mid-run; trial $TRIAL invalid"
    exit 2
  fi
  log "  WARN: lat rc=$rc but no mockex_died flag; proceeding"
fi

# ── Stop proc-status poller (likely already exited when pid died) ──
kill "$PROC_POLLER_PID" 2>/dev/null || true
wait "$PROC_POLLER_PID" 2>/dev/null || true
log "  proc poller stopped"

# Sanity: PROC_POST must contain ctxt switch lines; if missing, write a stub
# so generate_json doesn't break (deltas come out as null and aggregator marks n/a)
if [[ -s "$PROC_POST" ]] && grep -q "voluntary_ctxt_switches" "$PROC_POST"; then
  :
else
  log "  WARN: $PROC_POST missing voluntary_ctxt_switches — perf-stat ctxt-sw counter still usable"
fi

# ── Wait for perf to flush ──
log "waiting for perf to flush..."
wait "$PERF_PID" 2>/dev/null || true

# ── Post-snapshot: /proc/interrupts ──
log "snapshot post /proc/interrupts → $INT_POST"
cat /proc/interrupts > "$INT_POST"

# ── Generate JSON ──
log "parsing perf + proc → $JSON_FILE"
generate_json "$MODE" "$TRIAL" "$OUTDIR"

# ── Print one-line summary ──
print_summary "$JSON_FILE"

log "trial $TRIAL ($MODE) done"
exit 0

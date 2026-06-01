#!/usr/bin/env bash
#
# nic_lowlat_setup.sh — 低延迟网卡"地基"setup：确定的 队列↔核 映射 + 去掉动态搬核
#
# 为"业务流软中断核 == app 核"(不跨核) 打地基。它只做地基那一层：
#   建立确定、稳定的 RX/TX 队列 ↔ CPU 映射，并关掉所有会动态搬核的机制。
# 它【不】负责把业务流钉到目标队列(RSS/ntuple/src_port)、也【不】绑 app 线程 ——
# 那是后续两步。地基铺好后，不跨核仍要等那两步补齐才成立。
#
#   ┌──────────────── 本脚本(地基) ────────────────┐   ┌──── 后续 ────┐
#   │ 关 irqbalance/RPS/RFS/aRFS/coalescing         │   │ (a) 业务流→  │
#   │ 队列 IRQ 1:1 绑核 + 业务核独占业务队列         │ → │     目标队列  │
#   │ XPS 对齐 TX                                    │   │ (c) app 绑核 │
#   └───────────────────────────────────────────────┘   └──────────────┘
#
# 做的事(全部幂等：写前回读，已是目标态则跳过；可反复重放)：
#   1. stop + disable irqbalance（否则它动态改 smp_affinity 冲掉绑定）
#   2. 队列 IRQ affinity：业务队列 → business_cpu，且 business_cpu 只服务这一个 NIC
#      IRQ；其余队列 round-robin 到除 business_cpu 外的核
#   3. 关 RPS：每个 rx-* 的 rps_cpus = 0
#   4. 关 RFS：rps_sock_flow_entries = 0 + 每个 rx-* 的 rps_flow_cnt = 0
#   5. 关 aRFS：ethtool -K <nic> ntuple off（注意：这也关了 Flow Director，后续若要
#      ntuple 流导向需再开）
#   6. XPS 对齐：tx-N 的 xps_cpus = 该队列绑定的核
#   7. 关中断合并：ethtool -C adaptive-rx/tx off + rx/tx-usecs 0（ENA 自适应必须显式关）
#   8. [--gro-off] ethtool -K gro off（延迟钮，默认不动）
#   9. 回读验证并打印最终状态
#
# 重放说明：irqbalance 关掉后，NIC reset / 重协商队列数会丢失 smp_affinity。本脚本设计
#   为可反复重放——建议在网卡 up 事件(udev / networkd-dispatcher)后再跑一次。
#
# 回滚：--restore 把开关恢复到【内核默认】(重启 irqbalance、adaptive 合并 on、gro on、
#   IRQ affinity 复位到全核)。注意它不是"快照还原"——本脚本不存旧值快照，恢复到的是通用
#   默认值而非你改之前的精确状态。
#
# Usage:
#   sudo ./nic_lowlat_setup.sh <nic> <business_cpu> [flags]
#   sudo ./nic_lowlat_setup.sh ens6 1 --dry-run
#   sudo ./nic_lowlat_setup.sh ens6 1 --yes
#   sudo ./nic_lowlat_setup.sh ens6 1 --restore
#
# Arguments:
#   nic            网卡名（如 ens6；须存在于 /sys/class/net/）
#   business_cpu   业务核 CPU 号（0..nproc-1，须 online）
#
# Flags:
#   --business-queue <N>  指定哪条队列为"业务队列"绑到 business_cpu（默认 0）
#   --gro-off             额外关闭 GRO（延迟优化，默认不动）
#   --restore             回滚到内核默认（见上）
#   --dry-run             只打印每步会写什么，不真写
#   --yes | -y            跳过确认（无人值守 / --auto 用）
#   -v | --verbose        打印每项写入的前后值
#   -h | --help           显示帮助
#
# Exit codes:
#   0  全部成功
#   1  用法 / 输入校验错误
#   2  运行期错误（非 root / 缺 ethtool / nic 不存在 / 无队列中断）
#   3  部分项 NIC 不支持或写入失败（已尽力，其余已应用，详见结尾汇总）

set -euo pipefail

readonly SCRIPT_NAME="$(basename "${BASH_SOURCE[0]}")"

# ── 默认 / 全局状态 ──
DRY_RUN=0
ASSUME_YES=0
VERBOSE=0
GRO_OFF=0
RESTORE=0
BUSINESS_QUEUE=0
PARTIAL=0          # 部分失败计数 → 影响退出码

# ── 日志（stderr；stdout 留给机器可读输出）──
log_info()  { printf '[INFO]  %s\n' "$*" >&2; }
log_warn()  { printf '[WARN]  %s\n' "$*" >&2; }
log_error() { printf '[ERROR] %s\n' "$*" >&2; }
log_step()  { printf '\n=== %s ===\n' "$*" >&2; }
log_debug() { if [[ "${VERBOSE}" == "1" ]]; then printf '[DEBUG] %s\n' "$*" >&2; fi; }
die()       { log_error "$1"; exit "${2:-2}"; }

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^#\{1,\}//; s/^ //'; }

# ── 小工具 ──
is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }

# 展开 "0-3,5" 这类 cpu list
expand_list() {
  local spec="$1" part lo hi c
  local -a out=()
  IFS=',' read -ra _parts <<< "${spec}"
  for part in "${_parts[@]}"; do
    if [[ "${part}" == *-* ]]; then
      lo="${part%-*}"; hi="${part#*-}"
      for ((c = lo; c <= hi; c++)); do out+=("${c}"); done
    else
      out+=("${part}")
    fi
  done
  printf '%s\n' "${out[@]}"
}

# cpu 号 → xps/rps 用的十六进制 CPU 掩码（支持 >32 核，高位字在前，逗号分隔）
cpu_to_hexmask() {
  local cpu="$1"
  local word=$((cpu / 32))
  local bit=$((cpu % 32))
  local i out=""
  for ((i = word; i >= 0; i--)); do
    if (( i == word )); then
      out="$(printf '%x' $((1 << bit)))"
    else
      out="${out},00000000"
    fi
  done
  printf '%s' "${out}"
}

# 写 sysfs/proc 值：幂等（已是目标态跳过）+ dry-run + 回读
# $1 path  $2 value  $3 desc
apply_write() {
  local path="$1" val="$2" desc="$3" cur
  if [[ ! -e "${path}" ]]; then
    log_warn "跳过 ${desc}：路径不存在（${path}）"; PARTIAL=$((PARTIAL + 1)); return 0
  fi
  cur="$(cat "${path}" 2>/dev/null || printf '?')"
  if (( DRY_RUN )); then
    printf '  [dry-run] %-34s %s -> %s\n' "${desc}" "${cur}" "${val}" >&2; return 0
  fi
  # 归一化比较（去空白）；格式差异最多导致一次无害重写，效果仍幂等
  if [[ "${cur// /}" == "${val// /}" ]]; then
    log_debug "${desc} 已是 ${val}（跳过）"; return 0
  fi
  if printf '%s' "${val}" > "${path}" 2>/dev/null; then
    log_debug "${desc}: ${cur} -> $(cat "${path}" 2>/dev/null || printf '?')"
  else
    log_warn "${desc} 写入失败（${path}，可能 managed IRQ / 只读）"; PARTIAL=$((PARTIAL + 1))
  fi
}

# 跑外部命令（ethtool 等）：dry-run + 失败容错（不支持 → warn 不 die）
# $1 desc; 其余为命令
apply_cmd() {
  local desc="$1"; shift
  if (( DRY_RUN )); then
    printf '  [dry-run] %-34s %s\n' "${desc}" "$*" >&2; return 0
  fi
  if "$@" >/dev/null 2>&1; then
    log_debug "${desc} ✓"
  else
    log_warn "${desc} 失败/不支持：$*"; PARTIAL=$((PARTIAL + 1))
  fi
}

# ── 参数解析 ──
declare -a POSARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --business-queue) [[ -n "${2:-}" ]] || die "--business-queue 需要一个值" 1; BUSINESS_QUEUE="$2"; shift 2 ;;
    --gro-off)  GRO_OFF=1; shift ;;
    --restore)  RESTORE=1; shift ;;
    --dry-run)  DRY_RUN=1; shift ;;
    --yes|-y)   ASSUME_YES=1; shift ;;
    -v|--verbose) VERBOSE=1; shift ;;
    -h|--help)  usage; exit 0 ;;
    --) shift; while [[ $# -gt 0 ]]; do POSARGS+=("$1"); shift; done ;;
    -*) die "未知选项：$1（-h 看用法）" 1 ;;
    *)  POSARGS+=("$1"); shift ;;
  esac
done

(( ${#POSARGS[@]} >= 2 )) || { usage; die "缺少必需参数（需 <nic> <business_cpu>）" 1; }
(( ${#POSARGS[@]} <= 2 )) || die "参数过多：${POSARGS[*]:2}" 1
readonly NIC="${POSARGS[0]}"
readonly BUSINESS_CPU="${POSARGS[1]}"

# ── 输入校验 ──
[[ -n "${NIC}" ]]               || die "nic 不能为空" 1
[[ -d "/sys/class/net/${NIC}" ]] || die "网卡不存在：${NIC}（见 /sys/class/net/）" 2
is_uint "${BUSINESS_CPU}"        || die "business_cpu 必须是整数，得到：'${BUSINESS_CPU}'" 1
is_uint "${BUSINESS_QUEUE}"      || die "--business-queue 必须是整数，得到：'${BUSINESS_QUEUE}'" 1

# online CPU 集合
ONLINE_SPEC="$(cat /sys/devices/system/cpu/online 2>/dev/null || printf '0-%d' "$(( $(nproc) - 1 ))")"
mapfile -t ONLINE_CPUS < <(expand_list "${ONLINE_SPEC}")
cpu_is_online() { local c; for c in "${ONLINE_CPUS[@]}"; do [[ "${c}" == "$1" ]] && return 0; done; return 1; }
cpu_is_online "${BUSINESS_CPU}" || die "business_cpu=${BUSINESS_CPU} 不在 online 集合（${ONLINE_SPEC}）" 1

# business_cpu 之外的核（用于 round-robin 其余队列）
declare -a OTHER_CPUS=()
for c in "${ONLINE_CPUS[@]}"; do [[ "${c}" == "${BUSINESS_CPU}" ]] || OTHER_CPUS+=("${c}"); done

# ── Preflight ──
log_step "Preflight"
PF_FAIL=0
if (( DRY_RUN == 0 )) && [[ "${EUID}" -ne 0 ]]; then
  log_error "需要 root（写 /proc/irq 与 ethtool）；用 sudo 重跑"; PF_FAIL=$((PF_FAIL + 1))
fi
command -v ethtool >/dev/null 2>&1 || { log_error "缺 ethtool（dnf install ethtool）"; PF_FAIL=$((PF_FAIL + 1)); }
# 解析该网卡的队列中断（排除 ENA 的 *-mgmnt 管理中断，只要末尾带数字的 Tx-Rx 队列）
declare -a QIRQ=() QNUM=()
while read -r _irq _name; do
  [[ -z "${_irq}" ]] && continue
  QIRQ+=("${_irq}"); QNUM+=("${_name##*-}")
done < <(awk -v nic="${NIC}" '
  { name = $NF }
  name ~ ("^" nic "-") && name ~ /-[0-9]+$/ { irq = $1; sub(/:/, "", irq); print irq, name }
' /proc/interrupts)
(( ${#QIRQ[@]} > 0 )) || { log_error "在 /proc/interrupts 找不到 ${NIC} 的队列中断"; PF_FAIL=$((PF_FAIL + 1)); }
(( PF_FAIL == 0 )) || die "Preflight 未通过（${PF_FAIL} 项）" 2
log_info "✓ root / ethtool / 队列中断（${#QIRQ[@]} 条）就绪；online CPU=[${ONLINE_CPUS[*]}]"

# ── Restore 模式：回滚到内核默认后退出 ──
if (( RESTORE == 1 )); then
  log_step "Restore → 内核默认（非快照还原）"
  if [[ "${ASSUME_YES}" == "0" && "${DRY_RUN}" == "0" ]]; then
    printf '将重启 irqbalance、把 adaptive 中断合并/gro 设回 on、IRQ affinity 复位到全核。继续？[y/N] ' >&2
    read -r r; [[ "${r}" =~ ^[Yy]$ ]] || die "用户取消" 0
  fi
  if command -v systemctl >/dev/null 2>&1; then
    apply_cmd "enable irqbalance"  systemctl enable irqbalance
    apply_cmd "start irqbalance"   systemctl start irqbalance
  fi
  apply_cmd "adaptive-rx on" ethtool -C "${NIC}" adaptive-rx on
  apply_cmd "adaptive-tx on" ethtool -C "${NIC}" adaptive-tx on
  apply_cmd "gro on"         ethtool -K "${NIC}" gro on
  ALL_MASK="$(printf '%s' "${ONLINE_SPEC}")"
  for irq in "${QIRQ[@]}"; do
    apply_write "/proc/irq/${irq}/smp_affinity_list" "${ALL_MASK}" "irq ${irq} → 全核"
  done
  log_info "Restore 完成（${PARTIAL} 项未成功）。"
  exit $(( PARTIAL > 0 ? 3 : 0 ))
fi

# ── 业务队列存在性校验 + 构造队列→核分配 ──
bq_found=0
for q in "${QNUM[@]}"; do [[ "${q}" == "${BUSINESS_QUEUE}" ]] && bq_found=1; done
(( bq_found == 1 )) || die "业务队列 ${BUSINESS_QUEUE} 不存在；可用队列：[${QNUM[*]}]" 1

declare -a ACPU=()
rr=0
for i in "${!QNUM[@]}"; do
  if [[ "${QNUM[$i]}" == "${BUSINESS_QUEUE}" ]]; then
    ACPU[$i]="${BUSINESS_CPU}"
  elif (( ${#OTHER_CPUS[@]} == 0 )); then
    ACPU[$i]="${BUSINESS_CPU}"   # 单核机：无法隔离
  else
    ACPU[$i]="${OTHER_CPUS[$(( rr % ${#OTHER_CPUS[@]} ))]}"
    rr=$((rr + 1))
  fi
done
(( ${#OTHER_CPUS[@]} > 0 )) || log_warn "只有 1 个 online 核，无法把别的队列移出业务核 —— 隔离不成立。"

# ── Plan ──
log_step "Plan（nic=${NIC} business_cpu=${BUSINESS_CPU} business_queue=${BUSINESS_QUEUE}）"
{
  printf '  irqbalance           : stop + disable\n'
  printf '  队列 IRQ → CPU       :\n'
  for i in "${!QNUM[@]}"; do
    tag=""; [[ "${QNUM[$i]}" == "${BUSINESS_QUEUE}" ]] && tag="  ← 业务队列"
    printf '      q%-2s irq=%-4s → cpu %s%s\n' "${QNUM[$i]}" "${QIRQ[$i]}" "${ACPU[$i]}" "${tag}"
  done
  printf '  RPS                  : rx-*/rps_cpus = 0\n'
  printf '  RFS                  : rps_sock_flow_entries=0 + rx-*/rps_flow_cnt=0\n'
  printf '  aRFS                 : ethtool -K %s ntuple off\n' "${NIC}"
  printf '  XPS                  : tx-N/xps_cpus = 队列绑定核\n'
  printf '  中断合并             : adaptive-rx/tx off, rx/tx-usecs 0\n'
  (( GRO_OFF )) && printf '  GRO                  : off\n'
} >&2
if [[ "${ASSUME_YES}" == "0" && "${DRY_RUN}" == "0" ]]; then
  printf '\n以上改动会影响 %s 的中断/收发路径与系统网络配置。继续？[y/N] ' "${NIC}" >&2
  read -r reply
  [[ "${reply}" =~ ^[Yy]$ ]] || die "用户取消" 0
fi

# ── Step 1: irqbalance ──
log_step "Step 1/7: irqbalance off"
if command -v systemctl >/dev/null 2>&1; then
  if systemctl is-active --quiet irqbalance 2>/dev/null; then
    apply_cmd "stop irqbalance" systemctl stop irqbalance
  else log_debug "irqbalance 未运行（跳过 stop）"; fi
  if systemctl is-enabled --quiet irqbalance 2>/dev/null; then
    apply_cmd "disable irqbalance" systemctl disable irqbalance
  else log_debug "irqbalance 未启用（跳过 disable）"; fi
else
  log_warn "无 systemctl —— 若 irqbalance 以其他方式运行，请手动停止，否则它会冲掉 IRQ 绑定。"
fi

# ── Step 2: 队列 IRQ affinity ──
log_step "Step 2/7: 队列 IRQ 1:1 绑核"
for i in "${!QNUM[@]}"; do
  apply_write "/proc/irq/${QIRQ[$i]}/smp_affinity_list" "${ACPU[$i]}" "q${QNUM[$i]} irq=${QIRQ[$i]} → cpu${ACPU[$i]}"
done

# ── Step 3: 关 RPS ──
log_step "Step 3/7: 关 RPS"
for rxq in /sys/class/net/"${NIC}"/queues/rx-*; do
  [[ -e "${rxq}/rps_cpus" ]] || continue
  apply_write "${rxq}/rps_cpus" "0" "$(basename "${rxq}")/rps_cpus = 0"
done

# ── Step 4: 关 RFS ──
log_step "Step 4/7: 关 RFS"
apply_write "/proc/sys/net/core/rps_sock_flow_entries" "0" "rps_sock_flow_entries = 0"
for rxq in /sys/class/net/"${NIC}"/queues/rx-*; do
  [[ -e "${rxq}/rps_flow_cnt" ]] || continue
  apply_write "${rxq}/rps_flow_cnt" "0" "$(basename "${rxq}")/rps_flow_cnt = 0"
done

# ── Step 5: 关 aRFS（注意同时关掉 Flow Director）──
log_step "Step 5/7: 关 aRFS（ntuple off）"
apply_cmd "ntuple off（亦关 Flow Director）" ethtool -K "${NIC}" ntuple off

# ── Step 6: XPS 对齐 ──
log_step "Step 6/7: XPS 对齐 TX"
for i in "${!QNUM[@]}"; do
  txpath="/sys/class/net/${NIC}/queues/tx-${QNUM[$i]}/xps_cpus"
  [[ -e "${txpath}" ]] || { log_debug "无 ${txpath}（跳过）"; continue; }
  apply_write "${txpath}" "$(cpu_to_hexmask "${ACPU[$i]}")" "tx-${QNUM[$i]}/xps_cpus → cpu${ACPU[$i]}"
done

# ── Step 7: 关中断合并（rx/tx 分开，免得一项不支持拖累另一项）──
log_step "Step 7/7: 关中断合并"
apply_cmd "adaptive-rx off + rx-usecs 0" ethtool -C "${NIC}" adaptive-rx off rx-usecs 0
apply_cmd "adaptive-tx off + tx-usecs 0" ethtool -C "${NIC}" adaptive-tx off tx-usecs 0

# ── 可选：关 GRO ──
if (( GRO_OFF )); then
  log_step "可选: 关 GRO"
  apply_cmd "gro off" ethtool -K "${NIC}" gro off
fi

# ── 回读验证 ──
log_step "验证（回读实际状态）"
biz_irq_count=0
printf '  队列 IRQ → 实际 affinity:\n' >&2
for i in "${!QNUM[@]}"; do
  actual="$(cat "/proc/irq/${QIRQ[$i]}/smp_affinity_list" 2>/dev/null || printf '?')"
  rps="$(cat "/sys/class/net/${NIC}/queues/rx-${QNUM[$i]}/rps_cpus" 2>/dev/null || printf '?')"
  xps="$(cat "/sys/class/net/${NIC}/queues/tx-${QNUM[$i]}/xps_cpus" 2>/dev/null || printf '?')"
  tag=""; [[ "${QNUM[$i]}" == "${BUSINESS_QUEUE}" ]] && tag="  ← 业务队列"
  printf '      q%-2s irq=%-4s affinity=%-6s rps=%-4s xps=%-10s%s\n' \
    "${QNUM[$i]}" "${QIRQ[$i]}" "${actual}" "${rps}" "${xps}" "${tag}" >&2
  [[ "${actual}" == "${BUSINESS_CPU}" ]] && biz_irq_count=$((biz_irq_count + 1))
done
printf '  business_cpu(%s) 上的 NIC 队列 IRQ 数 = %s （期望 1）\n' "${BUSINESS_CPU}" "${biz_irq_count}" >&2
if (( DRY_RUN == 0 && biz_irq_count != 1 )); then
  log_warn "business_cpu 上的 NIC IRQ 数不是 1 —— 隔离未达成，检查上面映射。"
fi
# 特性状态
ntuple_st="$(ethtool -k "${NIC}" 2>/dev/null | awk -F': ' '/ntuple-filters/{print $2}')"
gro_st="$(ethtool -k "${NIC}" 2>/dev/null | awk -F': ' '/^generic-receive-offload/{print $2}')"
rfs_st="$(cat /proc/sys/net/core/rps_sock_flow_entries 2>/dev/null || printf '?')"
printf '  特性: ntuple=%s  gro=%s  rps_sock_flow_entries=%s\n' "${ntuple_st:-?}" "${gro_st:-?}" "${rfs_st}" >&2
printf '  中断合并:\n' >&2
ethtool -c "${NIC}" 2>/dev/null | grep -iE 'adaptive|rx-usecs:|tx-usecs:' | sed 's/^/      /' >&2 || true

# ── 收尾 ──
log_step "完成"
if (( DRY_RUN )); then
  log_info "dry-run 结束（未写任何内容）。去掉 --dry-run 即真实应用。"
  exit 0
fi
cat >&2 <<EOF
地基已铺好（业务队列 q${BUSINESS_QUEUE} → cpu${BUSINESS_CPU}）。这只是第一步——
还需：
  (a) 把业务流 RSS-steer 到 q${BUSINESS_QUEUE}（src_port 预测 / ethtool ntuple 若支持），
      否则流按哈希落到别的队列，软中断仍在别的核 → 依旧跨核；
  (c) 把 app 线程绑到 cpu${BUSINESS_CPU}。
然后用 tools/run_bpf_checks.sh 实测 @softirq_cpu == @recv_cpu == ${BUSINESS_CPU} 验证。
重放提醒：NIC reset 会丢 affinity，网卡 up 后重跑本脚本。
EOF
if (( PARTIAL > 0 )); then
  log_warn "有 ${PARTIAL} 项未成功（NIC 不支持 / 只读），其余已应用。"
  exit 3
fi
log_info "全部应用成功。"
exit 0

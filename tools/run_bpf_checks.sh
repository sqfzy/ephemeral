#!/usr/bin/env bash
#
# run_bpf_checks.sh — bpftrace 套件 wrapper：并发跑 tools/bpftrace/ 下的 4 个观测脚本
#
# 一次性把"网卡→业务CPU 不跨核 / 干净网卡无杂流量 / 进程收发字节归因 /
# 业务核被抢占"四项检查并发挂上，运行固定时长后统一 SIGINT 收尾。bpftrace 在
# SIGINT 时会把所有 @map dump 到 stdout 再退出，本脚本把每个子脚本的 dump 收到
# 独立输出文件，并在终端打印一句话摘要。
#
# 为什么并发而不是合并成一个 bpftrace 程序：
#   bpftrace 的位置参数 $1/$2/$3 是程序级全局，4 个脚本对它们的语义不同
#   （cross_core 的 $1=ifindex vs sched_switch 的 $1=cpu 号），拼进一个进程会打架。
#   作为独立进程并发跑则各传各参、各自 map 空间，互不干扰（内核允许同一 hook 挂多个
#   BPF 程序）。代价：4 套热路径探针叠加，每包多 ~50-200ns —— 只看布尔判据时可接受，
#   但运行期间不要读延迟绝对值。
#
# Usage:
#   sudo ./run_bpf_checks.sh <ifindex> <sport> <comm> <business_cpu> [duration_seconds]
#
# Arguments:
#   ifindex        业务网卡 ifindex（正整数，如 ens6=3；见 `ip -o link`）
#   sport          业务/exchange 源端口（1-65535，如 443）
#   comm           业务线程 comm（非空，≤15 字节，超出会被内核 TASK_COMM_LEN-1 截断）
#   business_cpu   业务核 CPU 号（0..nproc-1）
#   duration_seconds  观测时长，可选，默认 30，范围 1-3600
#
# 各子脚本的参数映射：
#   cross_core_check.bt  <ifindex> <sport> <comm>
#   clean_nic.bt         <ifindex>
#   nic_check.bt         （无参）
#   sched_switch.bt      <business_cpu> <comm>
#
# Flags:
#   --dry-run   只打印将要执行的 4 条 bpftrace 命令与输出路径，不真正挂探针
#   --yes       跳过"将挂内核热路径探针"的确认提示（无人值守用）
#   -v          verbose：额外打印每个子进程 PID / 存活检查细节
#   -h|--help   显示本帮助
#
# Exit codes:
#   0  全部 4 个子脚本成功 dump
#   1  用法 / 输入校验错误
#   2  运行期错误（缺 bpftrace、非 root、子脚本缺失、全部子进程启动失败）
#   3  部分子进程异常退出（产生了部分输出，已在摘要标注）
#
# 幂等性：输出目录带时间戳（checks-YYYYMMDD-HHMMSS），重跑不覆盖；探针本身无状态，
#         反复挂载不累积副作用。Ctrl-C / 超时 / 异常都经 trap 清理所有后台 bpftrace。

set -euo pipefail

readonly SCRIPT_NAME="$(basename "${BASH_SOURCE[0]}")"
# 本 wrapper 所在目录（tools/），使脚本不依赖调用方 cwd
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
# .bt 子脚本与输出目录都在 tools/bpftrace/ 下
readonly BT_DIR="${SCRIPT_DIR}/bpftrace"

# ── 日志（全部走 stderr，stdout 留给潜在的机器可读输出）──
log_info()  { printf '[INFO]  %s\n' "$*" >&2; }
log_warn()  { printf '[WARN]  %s\n' "$*" >&2; }
log_error() { printf '[ERROR] %s\n' "$*" >&2; }
log_step()  { printf '\n=== %s ===\n' "$*" >&2; }
log_debug() { [[ "${VERBOSE}" == "1" ]] && printf '[DEBUG] %s\n' "$*" >&2 || true; }
die()       { log_error "$1"; exit "${2:-2}"; }

usage() {
  sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^#\?//; s/^ //'
}

# ── 默认值 / 全局状态 ──
DRY_RUN=0
ASSUME_YES=0
VERBOSE=0
DURATION=30
declare -a CHILD_PIDS=()   # 后台 bpftrace 的 PID，trap 清理用
CLEANED_UP=0

# ── trap：任何退出路径都收掉残留的 bpftrace，避免探针/进程泄漏 ──
cleanup() {
  # 幂等：trap 可能在 EXIT 与 INT 各触发一次
  [[ "${CLEANED_UP}" == "1" ]] && return 0
  CLEANED_UP=1
  local pid
  for pid in "${CHILD_PIDS[@]:-}"; do
    [[ -z "${pid}" ]] && continue
    if kill -0 "${pid}" 2>/dev/null; then
      log_debug "cleanup: SIGINT -> bpftrace pid=${pid}"
      kill -INT "${pid}" 2>/dev/null || true
    fi
  done
  # 给 bpftrace 一点时间 detach + dump，再硬杀仍存活的
  local waited=0
  while (( waited < 5 )); do
    local alive=0
    for pid in "${CHILD_PIDS[@]:-}"; do
      [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null && alive=1
    done
    (( alive == 0 )) && break
    sleep 1; waited=$((waited + 1))
  done
  for pid in "${CHILD_PIDS[@]:-}"; do
    [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null || continue
    log_warn "cleanup: bpftrace pid=${pid} 未在宽限期退出，SIGKILL"
    kill -KILL "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT
trap 'log_warn "收到中断，提前收尾..."; exit 130' INT TERM

# ── 参数解析 ──
declare -a POSARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --yes|-y)  ASSUME_YES=1; shift ;;
    -v|--verbose) VERBOSE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; while [[ $# -gt 0 ]]; do POSARGS+=("$1"); shift; done ;;
    -*) die "未知选项: $1（-h 看用法）" 1 ;;
    *)  POSARGS+=("$1"); shift ;;
  esac
done

# ── 输入校验 ──
(( ${#POSARGS[@]} >= 4 )) || { usage; die "缺少必需参数（需 ifindex sport comm business_cpu）" 1; }
(( ${#POSARGS[@]} <= 5 )) || die "参数过多（最多 5 个，多余的: ${POSARGS[*]:5}）" 1

IFINDEX="${POSARGS[0]}"
SPORT="${POSARGS[1]}"
COMM="${POSARGS[2]}"
BUSINESS_CPU="${POSARGS[3]}"
[[ -n "${POSARGS[4]:-}" ]] && DURATION="${POSARGS[4]}"

is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }

is_uint "${IFINDEX}"      || die "ifindex 必须是正整数，得到: '${IFINDEX}'" 1
(( IFINDEX >= 1 ))        || die "ifindex 必须 >= 1，得到: ${IFINDEX}" 1
is_uint "${SPORT}"        || die "sport 必须是整数，得到: '${SPORT}'" 1
(( SPORT >= 1 && SPORT <= 65535 )) || die "sport 须在 1-65535，得到: ${SPORT}" 1
[[ -n "${COMM}" ]]        || die "comm 不能为空" 1
is_uint "${BUSINESS_CPU}" || die "business_cpu 必须是整数，得到: '${BUSINESS_CPU}'" 1
is_uint "${DURATION}"     || die "duration_seconds 必须是整数，得到: '${DURATION}'" 1
(( DURATION >= 1 && DURATION <= 3600 )) || die "duration_seconds 须在 1-3600，得到: ${DURATION}" 1

# comm 截断软警告（不致命，bpftrace 会按前 15 字节匹配）
if (( ${#COMM} > 15 )); then
  log_warn "comm '${COMM}' 长 ${#COMM} 字节 > 15，内核会截断到 '${COMM:0:15}'；子脚本按截断后匹配。"
fi

# CPU 号范围软校验（拿不到 nproc 不阻断）
if NPROC="$(nproc 2>/dev/null)"; then
  (( BUSINESS_CPU < NPROC )) || log_warn "business_cpu=${BUSINESS_CPU} >= nproc=${NPROC}，sched_switch.bt 可能永不触发。"
fi

# ifindex 存在性软校验（NIC 可能在别的 netns，故仅警告）
if command -v ip >/dev/null 2>&1; then
  if ! ip -o link 2>/dev/null | awk -F': ' '{print $1}' | grep -qx "${IFINDEX}"; then
    log_warn "ifindex=${IFINDEX} 未在本 netns 的 \`ip -o link\` 中找到（若 NIC 在 bench_ns / 其他 netns 可忽略）。"
  fi
fi

# ── 前置检查 ──
command -v bpftrace >/dev/null 2>&1 || die "未找到 bpftrace；请先安装（probe 套件依赖它）。" 2

if (( DRY_RUN == 0 )) && [[ "${EUID}" -ne 0 ]]; then
  die "bpftrace 需要 root；请用 sudo 运行：sudo ${SCRIPT_NAME} ${POSARGS[*]}" 2
fi

# 子脚本清单：name|bt 文件|参数串（参数串经 word-split 传给 bpftrace）
readonly -a CHECKS=(
  "cross_core|cross_core_check.bt|${IFINDEX} ${SPORT} ${COMM}"
  "clean_nic|clean_nic.bt|${IFINDEX}"
  "nic_check|nic_check.bt|"
  "sched_switch|sched_switch.bt|${BUSINESS_CPU} ${COMM}"
)

# 确认所有 .bt 都在
for entry in "${CHECKS[@]}"; do
  IFS='|' read -r _name bt _args <<< "${entry}"
  [[ -f "${BT_DIR}/${bt}" ]] || die "找不到子脚本: ${BT_DIR}/${bt}" 2
done

# ── 输出目录（时间戳，幂等不覆盖）──
TS="$(date '+%Y%m%d-%H%M%S')"
OUTPUT_DIR="${OUTPUT_DIR:-${BT_DIR}/outputs/checks-${TS}}"

# ── Dry-run：打印将执行的命令，不挂探针 ──
if (( DRY_RUN == 1 )); then
  log_step "DRY-RUN：以下命令不会真正执行"
  printf '输出目录: %s\n' "${OUTPUT_DIR}" >&2
  printf '观测时长: %ss\n' "${DURATION}" >&2
  for entry in "${CHECKS[@]}"; do
    IFS='|' read -r name bt args <<< "${entry}"
    # shellcheck disable=SC2086  # args 故意 word-split 成 bpftrace 位置参数
    printf '  [%-12s] bpftrace %s/%s %s  > %s/%s.out 2> %s/%s.log\n' \
      "${name}" "${BT_DIR}" "${bt}" "${args}" "${OUTPUT_DIR}" "${name}" "${OUTPUT_DIR}" "${name}" >&2
  done
  log_info "dry-run 结束（未挂任何探针）。去掉 --dry-run 即真实运行。"
  exit 0
fi

# ── 确认环节：挂内核热路径探针对 busy-poll 系统有扰动 ──
if (( ASSUME_YES == 0 )); then
  cat >&2 <<EOF

即将并发挂载 4 套 bpftrace 探针（含收包热路径 tcp_v4_rcv / ip_rcv_core /
sock_recvmsg 等），持续 ${DURATION}s。这会给每个包叠加 ~50-200ns 探针开销——
运行期间请勿采信延迟绝对值。输出写入：
  ${OUTPUT_DIR}
EOF
  printf '继续？[y/N] ' >&2
  read -r reply
  [[ "${reply}" =~ ^[Yy]$ ]] || die "用户取消。" 0
fi

mkdir -p "${OUTPUT_DIR}"
log_info "输出目录: ${OUTPUT_DIR}"

# ── 启动 4 个 bpftrace（后台），各自 stdout→.out / stderr→.log ──
log_step "启动 ${#CHECKS[@]} 个 bpftrace（时长 ${DURATION}s）"
declare -a NAMES=()
for entry in "${CHECKS[@]}"; do
  IFS='|' read -r name bt args <<< "${entry}"
  # shellcheck disable=SC2086  # args 需 word-split 为多个 bpftrace 位置参数；上游已校验为数字/comm，无注入面
  bpftrace "${BT_DIR}/${bt}" ${args} \
      >"${OUTPUT_DIR}/${name}.out" 2>"${OUTPUT_DIR}/${name}.log" &
  pid=$!
  CHILD_PIDS+=("${pid}")
  NAMES+=("${name}")
  log_info "  [${name}] pid=${pid}"
done

# ── 存活检查：探针挂不上的子进程会立刻退出 ──
sleep 1
launch_fail=0
for i in "${!NAMES[@]}"; do
  pid="${CHILD_PIDS[$i]}"; name="${NAMES[$i]}"
  if ! kill -0 "${pid}" 2>/dev/null; then
    log_warn "  [${name}] 启动后即退出 —— 见 ${OUTPUT_DIR}/${name}.log（可能 probe attach 失败）"
    launch_fail=$((launch_fail + 1))
  else
    log_debug "  [${name}] pid=${pid} 存活"
  fi
done
if (( launch_fail == ${#NAMES[@]} )); then
  die "全部 ${launch_fail} 个子进程启动失败，无可观测对象。" 2
fi

# ── 观测窗口：带进度 ──
log_step "观测中（${DURATION}s）"
for ((s = 1; s <= DURATION; s++)); do
  printf '\r  采集 %d/%ds' "${s}" "${DURATION}" >&2
  sleep 1
done
printf '\n' >&2

# ── 统一 SIGINT 收尾 → bpftrace dump map 到各自 .out ──
log_step "SIGINT 收尾，等待 map dump"
for i in "${!NAMES[@]}"; do
  pid="${CHILD_PIDS[$i]}"
  kill -0 "${pid}" 2>/dev/null && kill -INT "${pid}" 2>/dev/null || true
done
# 等子进程把 map flush 完并退出（wait 对已死的 PID 立即返回）
exit_problem=0
for i in "${!NAMES[@]}"; do
  pid="${CHILD_PIDS[$i]}"; name="${NAMES[$i]}"
  if wait "${pid}" 2>/dev/null; then
    log_debug "  [${name}] 正常退出"
  else
    rc=$?
    # 130/SIGINT(2) 是我们主动发的，算正常收尾
    if (( rc == 130 || rc == 2 )); then
      log_debug "  [${name}] 经 SIGINT 退出"
    else
      log_warn "  [${name}] 异常退出码 ${rc}"
      exit_problem=$((exit_problem + 1))
    fi
  fi
done

# ── 终端摘要 ──
log_step "摘要"
for name in "${NAMES[@]}"; do
  out="${OUTPUT_DIR}/${name}.out"
  if [[ -s "${out}" ]]; then
    maps="$(grep -c '^@' "${out}" 2>/dev/null || echo 0)"
    printf '  [%-12s] %s（%s 行 map 数据）\n' "${name}" "${out}" "${maps}" >&2
  else
    printf '  [%-12s] 无输出 —— 见 %s.log\n' "${name}" "${OUTPUT_DIR}/${name}" >&2
  fi
done
log_info "完整 map dump 见各 .out 文件；判定逻辑见各 .bt 头部注释的「判定（harness）」段。"

if (( launch_fail > 0 || exit_problem > 0 )); then
  log_warn "有 $((launch_fail + exit_problem)) 个子脚本异常（见上）。"
  exit 3
fi
log_info "全部 ${#NAMES[@]} 个检查完成。"
exit 0

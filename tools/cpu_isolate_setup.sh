#!/usr/bin/env bash
#
# cpu_isolate_setup.sh — CPU 隔离配置 (业务核独占,无内核噪声)
#
# 与 nic_lowlat_setup.sh 配对使用:
#   nic_lowlat_setup.sh 把 NIC queue IRQ 钉到业务 CPU
#   cpu_isolate_setup.sh 把业务 CPU 从 OS 调度器拿走 (kernel 不调度其他任务上来)
#
# 做的事:
#   预启 (--grub, 写入 GRUB cmdline, 需重启生效):
#     1. isolcpus=<list>      内核调度器不分配普通任务到这些核
#     2. nohz_full=<list>     这些核停掉周期性 timer tick (减少抖动)
#     3. rcu_nocbs=<list>     RCU 回调 offload 到其他核
#
#   运行时 (--runtime, 立即生效, 不需重启):
#     4. cpufreq governor=performance (业务核 + 全局)
#     5. NMI watchdog off (全局, 否则每秒一次跨核打断)
#     6. THP defrag=never (避免后台 defrag 抢业务核)
#     7. timer_migration=0 (避免跨核迁移软定时器)
#     8. ksmd off (内存合并扫描)
#
#   --restore 模式: 回滚 GRUB cmdline + 恢复 governor / NMI / THP 默认值
#
# Usage:
#   sudo ./cpu_isolate_setup.sh <cpu_list> [flags]
#   sudo ./cpu_isolate_setup.sh 1,2,3 --dry-run
#   sudo ./cpu_isolate_setup.sh 1 --runtime-only       # 只跑无需重启的部分
#   sudo ./cpu_isolate_setup.sh 1 --grub-only --yes    # 只写 GRUB
#   sudo ./cpu_isolate_setup.sh --restore --yes
#
# Arguments:
#   cpu_list      逗号分隔 (1,2,3) 或区间 (1-3) 或混合. 业务 CPU, 必须 online.
#
# Flags:
#   --grub-only     只写 GRUB cmdline (需重启)
#   --runtime-only  只跑运行时 knob (不写 GRUB, 不需重启)
#   --restore       回滚 (GRUB + runtime 默认值)
#   --dry-run       打印改动不写
#   --yes | -y      跳过确认
#   -v | --verbose  打印前后值
#   -h | --help     显示帮助
#
# Exit codes:
#   0  全部成功
#   1  用法 / 输入错误
#   2  运行期错误 (非 root / GRUB 文件找不到 / CPU 离线)
#   3  部分项写入失败 (已尽力, 详见结尾汇总)

set -euo pipefail

readonly SCRIPT_NAME="$(basename "${BASH_SOURCE[0]}")"

# ── 状态 ──
CPU_LIST=""
DRY_RUN=0
ASSUME_YES=0
VERBOSE=0
GRUB_ONLY=0
RUNTIME_ONLY=0
RESTORE=0
PARTIAL=0

# ── 日志 ──
log_info()  { printf '[INFO]  %s\n' "$*" >&2; }
log_warn()  { printf '[WARN]  %s\n' "$*" >&2; }
log_error() { printf '[ERROR] %s\n' "$*" >&2; }
log_step()  { printf '\n=== %s ===\n' "$*" >&2; }
log_debug() { [[ "${VERBOSE}" == "1" ]] && printf '[DEBUG] %s\n' "$*" >&2 || true; }
die()       { log_error "$1"; exit "${2:-2}"; }

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^#\{1,\}//; s/^ //'; }

# ── 工具 ──
is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }

# 展开 "1-3,5" 这类 cpu list 成 "1 2 3 5"
expand_list() {
  local spec="$1" part lo hi c
  local -a out=()
  IFS=',' read -ra _parts <<< "${spec}"
  for part in "${_parts[@]}"; do
    if [[ "${part}" == *-* ]]; then
      lo="${part%-*}"; hi="${part#*-}"
      is_uint "${lo}" && is_uint "${hi}" || die "无效 cpu 区间: ${part}" 1
      for ((c = lo; c <= hi; c++)); do out+=("${c}"); done
    else
      is_uint "${part}" || die "无效 cpu 编号: ${part}" 1
      out+=("${part}")
    fi
  done
  printf '%s\n' "${out[@]}"
}

# 写文件 / sysfs (幂等: 已是目标态跳过)
write_safe() {
  local path="$1" want="$2" desc="${3:-$1}"
  [[ -e "${path}" ]] || { log_warn "skip ${desc}: ${path} 不存在"; PARTIAL=$((PARTIAL+1)); return 0; }
  local cur
  cur="$(cat "${path}" 2>/dev/null || echo '')"
  if [[ "${cur}" == "${want}" ]]; then
    log_debug "✓ ${desc} 已是 ${want}"
    return 0
  fi
  if [[ "${DRY_RUN}" == "1" ]]; then
    log_info "[DRY] ${desc}: ${cur} → ${want}"
    return 0
  fi
  if ! echo "${want}" > "${path}" 2>/dev/null; then
    log_warn "写 ${desc} 失败 (${path})"
    PARTIAL=$((PARTIAL+1))
    return 0
  fi
  log_info "${desc}: ${cur} → ${want}"
}

# ── 参数解析 ──
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)        DRY_RUN=1; shift ;;
    -y|--yes)         ASSUME_YES=1; shift ;;
    -v|--verbose)     VERBOSE=1; shift ;;
    --grub-only)      GRUB_ONLY=1; shift ;;
    --runtime-only)   RUNTIME_ONLY=1; shift ;;
    --restore)        RESTORE=1; shift ;;
    -h|--help)        usage; exit 0 ;;
    -*)               die "未知 flag: $1" 1 ;;
    *)
      if [[ -z "${CPU_LIST}" ]]; then CPU_LIST="$1"
      else die "意外参数: $1" 1
      fi
      shift ;;
  esac
done

# 校验
[[ "${GRUB_ONLY}" == "1" && "${RUNTIME_ONLY}" == "1" ]] && die "--grub-only 和 --runtime-only 互斥" 1

if [[ "${RESTORE}" != "1" && -z "${CPU_LIST}" ]]; then
  usage; exit 1
fi

[[ "${EUID}" == "0" ]] || die "需要 root (sudo)" 2

# ── 检测 OS / GRUB 路径 ──
GRUB_DEFAULT_FILE=""
GRUB_MKCONFIG=""
GRUB_CFG_FILE=""

detect_grub() {
  if [[ -f /etc/default/grub ]]; then
    GRUB_DEFAULT_FILE="/etc/default/grub"
  fi
  if command -v grub2-mkconfig &>/dev/null; then
    GRUB_MKCONFIG="grub2-mkconfig"
    [[ -f /boot/grub2/grub.cfg ]] && GRUB_CFG_FILE="/boot/grub2/grub.cfg"
  elif command -v grub-mkconfig &>/dev/null; then
    GRUB_MKCONFIG="grub-mkconfig"
    [[ -f /boot/grub/grub.cfg ]] && GRUB_CFG_FILE="/boot/grub/grub.cfg"
    [[ -f /boot/grub2/grub.cfg ]] && GRUB_CFG_FILE="/boot/grub2/grub.cfg"
  fi
  if command -v update-grub &>/dev/null; then
    GRUB_MKCONFIG="update-grub"   # ubuntu style
    GRUB_CFG_FILE=""              # update-grub 自动处理
  fi
}
detect_grub

# ── 展开 CPU list (restore 时不需要) ──
declare -a CPUS=()
if [[ -n "${CPU_LIST}" ]]; then
  while IFS= read -r c; do CPUS+=("${c}"); done < <(expand_list "${CPU_LIST}")
  # 校验每个 cpu online
  for c in "${CPUS[@]}"; do
    if [[ "${c}" == "0" ]]; then continue; fi  # CPU 0 always online
    onl="/sys/devices/system/cpu/cpu${c}/online"
    [[ -f "${onl}" ]] || die "CPU ${c} 不存在 (${onl})" 2
    [[ "$(cat "${onl}")" == "1" ]] || die "CPU ${c} 离线" 2
  done
fi

# ── GRUB 修改 ──
modify_grub() {
  log_step "GRUB cmdline 修改 (需重启生效)"
  [[ -n "${GRUB_DEFAULT_FILE}" ]] || die "找不到 /etc/default/grub (不支持的 distro?)" 2
  [[ -n "${GRUB_MKCONFIG}" ]] || die "找不到 grub-mkconfig / grub2-mkconfig / update-grub" 2

  local cpu_csv
  cpu_csv="$(IFS=','; echo "${CPUS[*]}")"

  log_info "目标参数: isolcpus=${cpu_csv} nohz_full=${cpu_csv} rcu_nocbs=${cpu_csv}"
  log_info "GRUB 文件: ${GRUB_DEFAULT_FILE}"
  log_info "重生成命令: ${GRUB_MKCONFIG} ${GRUB_CFG_FILE:+-o ${GRUB_CFG_FILE}}"

  if [[ "${ASSUME_YES}" != "1" && "${DRY_RUN}" != "1" ]]; then
    read -rp "确认修改 GRUB? [y/N] " resp
    [[ "${resp,,}" == "y" ]] || die "用户取消" 1
  fi

  # 备份
  local backup="${GRUB_DEFAULT_FILE}.bak.$(date -u +%Y%m%d%H%M%S)"
  if [[ "${DRY_RUN}" != "1" ]]; then
    cp -a "${GRUB_DEFAULT_FILE}" "${backup}"
    log_info "GRUB 备份: ${backup}"
  fi

  # 修改 GRUB_CMDLINE_LINUX (优先) 或 GRUB_CMDLINE_LINUX_DEFAULT
  local key
  if grep -q '^GRUB_CMDLINE_LINUX=' "${GRUB_DEFAULT_FILE}"; then
    key="GRUB_CMDLINE_LINUX"
  elif grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' "${GRUB_DEFAULT_FILE}"; then
    key="GRUB_CMDLINE_LINUX_DEFAULT"
  else
    log_warn "未找到 GRUB_CMDLINE_LINUX* 行, 加新行"
    if [[ "${DRY_RUN}" != "1" ]]; then
      echo "GRUB_CMDLINE_LINUX=\"isolcpus=${cpu_csv} nohz_full=${cpu_csv} rcu_nocbs=${cpu_csv}\"" >> "${GRUB_DEFAULT_FILE}"
    fi
    return 0
  fi

  # 删旧 isolcpus / nohz_full / rcu_nocbs, 加新
  local old_line new_line
  old_line=$(grep "^${key}=" "${GRUB_DEFAULT_FILE}" | head -1)
  # 提取引号内内容
  local val
  val=$(echo "${old_line}" | sed -E "s/^${key}=\"(.*)\"\$/\1/")
  # 去掉旧的 isolcpus= / nohz_full= / rcu_nocbs= 项
  val=$(echo "${val}" | sed -E 's/(^| )(isolcpus|nohz_full|rcu_nocbs)=[^ ]+//g' | sed -E 's/^ +//; s/ +$//; s/  +/ /g')
  # 加新
  if [[ -n "${val}" ]]; then
    val="${val} isolcpus=${cpu_csv} nohz_full=${cpu_csv} rcu_nocbs=${cpu_csv}"
  else
    val="isolcpus=${cpu_csv} nohz_full=${cpu_csv} rcu_nocbs=${cpu_csv}"
  fi
  new_line="${key}=\"${val}\""

  if [[ "${old_line}" == "${new_line}" ]]; then
    log_info "GRUB cmdline 已是目标态, 跳过"
  else
    log_info "old: ${old_line}"
    log_info "new: ${new_line}"
    if [[ "${DRY_RUN}" != "1" ]]; then
      sed -i "s|^${key}=.*|${new_line}|" "${GRUB_DEFAULT_FILE}"
    fi
  fi

  # 重生成 grub.cfg
  if [[ "${DRY_RUN}" != "1" ]]; then
    if [[ "${GRUB_MKCONFIG}" == "update-grub" ]]; then
      log_info "运行 update-grub..."
      update-grub
    else
      log_info "运行 ${GRUB_MKCONFIG} -o ${GRUB_CFG_FILE}..."
      ${GRUB_MKCONFIG} -o "${GRUB_CFG_FILE}"
    fi
  else
    log_info "[DRY] 跳过 grub.cfg 重生成"
  fi

  log_warn "需要重启生效: sudo reboot"
  log_info "重启后验证: cat /proc/cmdline | tr ' ' '\\n' | grep -E 'isol|nohz|rcu'"
}

# ── Runtime 配置 ──
apply_runtime() {
  log_step "Runtime 调优 (立即生效)"

  # 5. NMI watchdog 全局关
  write_safe "/proc/sys/kernel/nmi_watchdog" "0" "NMI watchdog off"

  # 6. THP defrag never (避免后台 defrag 跑业务核)
  write_safe "/sys/kernel/mm/transparent_hugepage/defrag" "never" "THP defrag=never"
  write_safe "/sys/kernel/mm/transparent_hugepage/enabled" "madvise" "THP enabled=madvise"

  # 7. timer migration off
  write_safe "/proc/sys/kernel/timer_migration" "0" "timer_migration off"

  # 8. KSM (内存合并扫描) off
  write_safe "/sys/kernel/mm/ksm/run" "0" "ksm off"

  # 4. cpufreq governor = performance, 业务核 + 系统其他核
  log_info "设置 cpufreq governor=performance (全核)"
  local n_online
  n_online=$(nproc)
  for ((i=0; i<n_online; i++)); do
    gov_path="/sys/devices/system/cpu/cpu${i}/cpufreq/scaling_governor"
    [[ -f "${gov_path}" ]] || continue
    write_safe "${gov_path}" "performance" "cpu${i} governor"
  done

  log_info "runtime 调优完成"
}

# ── Restore ──
restore_all() {
  log_step "回滚到内核默认"
  [[ -n "${GRUB_DEFAULT_FILE}" ]] || die "找不到 /etc/default/grub" 2

  if [[ "${ASSUME_YES}" != "1" && "${DRY_RUN}" != "1" ]]; then
    read -rp "确认回滚 GRUB cmdline (删 isolcpus/nohz_full/rcu_nocbs)? [y/N] " resp
    [[ "${resp,,}" == "y" ]] || die "用户取消" 1
  fi

  local backup="${GRUB_DEFAULT_FILE}.bak.$(date -u +%Y%m%d%H%M%S)"
  if [[ "${DRY_RUN}" != "1" ]]; then
    cp -a "${GRUB_DEFAULT_FILE}" "${backup}"
    log_info "GRUB 备份: ${backup}"
  fi

  for key in GRUB_CMDLINE_LINUX GRUB_CMDLINE_LINUX_DEFAULT; do
    if grep -q "^${key}=" "${GRUB_DEFAULT_FILE}"; then
      local old_line
      old_line=$(grep "^${key}=" "${GRUB_DEFAULT_FILE}" | head -1)
      local val
      val=$(echo "${old_line}" | sed -E "s/^${key}=\"(.*)\"\$/\1/")
      val=$(echo "${val}" | sed -E 's/(^| )(isolcpus|nohz_full|rcu_nocbs)=[^ ]+//g' | sed -E 's/^ +//; s/ +$//; s/  +/ /g')
      local new_line="${key}=\"${val}\""
      if [[ "${old_line}" != "${new_line}" ]]; then
        log_info "${key}: 删 isolcpus/nohz_full/rcu_nocbs"
        if [[ "${DRY_RUN}" != "1" ]]; then
          sed -i "s|^${key}=.*|${new_line}|" "${GRUB_DEFAULT_FILE}"
        fi
      fi
    fi
  done

  if [[ "${DRY_RUN}" != "1" ]]; then
    if [[ "${GRUB_MKCONFIG}" == "update-grub" ]]; then
      update-grub
    else
      ${GRUB_MKCONFIG} -o "${GRUB_CFG_FILE}"
    fi
  fi

  log_step "Runtime 默认值恢复"
  write_safe "/proc/sys/kernel/nmi_watchdog" "1" "NMI watchdog on (default)"
  write_safe "/sys/kernel/mm/transparent_hugepage/defrag" "madvise" "THP defrag=madvise"
  write_safe "/proc/sys/kernel/timer_migration" "1" "timer_migration on"
  local n_online; n_online=$(nproc)
  for ((i=0; i<n_online; i++)); do
    gov_path="/sys/devices/system/cpu/cpu${i}/cpufreq/scaling_governor"
    [[ -f "${gov_path}" ]] || continue
    write_safe "${gov_path}" "schedutil" "cpu${i} governor (default)"
  done

  log_warn "需要重启 GRUB cmdline 才完全生效"
}

# ── Main ──
log_info "${SCRIPT_NAME}: cpu_list=${CPU_LIST:-(restore)} dry_run=${DRY_RUN}"

if [[ "${RESTORE}" == "1" ]]; then
  restore_all
elif [[ "${GRUB_ONLY}" == "1" ]]; then
  modify_grub
elif [[ "${RUNTIME_ONLY}" == "1" ]]; then
  apply_runtime
else
  modify_grub
  apply_runtime
fi

log_step "汇总"
if [[ "${PARTIAL}" -gt 0 ]]; then
  log_warn "${PARTIAL} 项写入失败/跳过 (sysfs 不存在 / 不支持). 详见上方."
  exit 3
fi
log_info "全部完成. 当前 /proc/cmdline isolation 状态:"
cat /proc/cmdline | tr ' ' '\n' | grep -E 'isol|nohz|rcu_nocbs' | sed 's/^/  /' || echo "  (none — 需重启生效)"
log_info "推荐验证: cat /sys/devices/system/cpu/isolated"
echo "  $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '(empty 表示未生效)')"
exit 0

#!/usr/bin/env bash
# dpdk-teardown.sh — undo dpdk-setup.sh: rebind NIC to kernel, optionally free hugepages
#
# ┌── Script roles in this repo ────────────────────────────────────────────┐
# │ benchmarks/latency/scripts/dpdk-setup.sh     host env prep               │
# │ benchmarks/latency/scripts/dpdk-teardown.sh  this file — restore kernel  │
# │ benchmarks/latency/scripts/setup_coalescing.sh  NIC RX coalescing tune   │
# │ benchmarks/latency/lat                       per-run bench wrapper       │
# └─────────────────────────────────────────────────────────────────────────┘
#
# This script ONLY undoes dpdk-setup.sh:
#   - Rebind the vfio-pci NIC back to its kernel driver (typically ena/ixgbe)
#   - Wait for the kernel netdev to reappear and DHCP to assign an IP
#   - Optionally release hugepages (--release-hugepages)
#   - Delete .dpdk_state
#
# It does NOT:
#   - Unload the vfio-pci kernel module (intentional — reload has cost)
#   - Restore NIC coalescing settings (use
#     benchmarks/latency/scripts/setup_coalescing.sh --restore for that)
#   - Move the NIC into any netns (use `ip link set <nic> netns <ns>` or
#     run benchmarks/latency/lat which manages the bench_ns transitions)
#
# History: Phase 7 removed the eph-dpdk module; these scripts were restored
# from the pre-v3.3 baseline and re-rooted under benchmarks/latency/scripts/.
#
# 用法：sudo ./benchmarks/latency/scripts/dpdk-teardown.sh [选项]

set -euo pipefail

# ──────────────────────────────────────────
# 颜色 & 格式
# ──────────────────────────────────────────
if [[ -t 1 ]]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
    BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
else
    RED=''; YELLOW=''; GREEN=''; BLUE=''; CYAN=''; BOLD=''; RESET=''
fi

step()    { echo -e "\n${BLUE}▶${RESET}  ${BOLD}$*${RESET}"; }
ok()      { echo -e "${GREEN}✓${RESET}  $*"; }
warn()    { echo -e "${YELLOW}⚠${RESET}  $*"; }
info()    { echo -e "${CYAN}ℹ${RESET}  $*"; }
suggest() { echo -e "${CYAN}💡${RESET} $*"; }
error()   { echo -e "${RED}✗${RESET}  ${BOLD}$*${RESET}" >&2; }
verbose() { [[ "$VERBOSE" == true ]] && echo -e "  ${CYAN}💬${RESET} $*"; true; }
die() {
    error "$1"
    [[ -n "${2:-}" ]] && echo -e "\n${2}" >&2
    exit 1
}
separator() { echo -e "\n${BOLD}────────────────────────────────────────${RESET}"; }

# ──────────────────────────────────────────
# 默认配置
# ──────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if PROJECT_DIR=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null); then
    :
else
    PROJECT_DIR="$SCRIPT_DIR"
    while [[ "$PROJECT_DIR" != "/" ]]; do
        if [[ -f "$PROJECT_DIR/CLAUDE.md" && -f "$PROJECT_DIR/xmake.lua" ]]; then
            break
        fi
        PROJECT_DIR="$(dirname "$PROJECT_DIR")"
    done
    [[ "$PROJECT_DIR" == "/" ]] && { echo "cannot locate project root from $SCRIPT_DIR" >&2; exit 1; }
fi
STATE_FILE="$PROJECT_DIR/.dpdk_state"

# 默认值（可被 .dpdk_state 或环境变量覆盖）
DPDK_PCI="${DPDK_PCI:-}"
DPDK_IFACE="${DPDK_IFACE:-}"
DPDK_NIC_IP="${DPDK_NIC_IP:-}"
KERNEL_DRIVER="${KERNEL_DRIVER:-}"

# ──────────────────────────────────────────
# 参数解析
# ──────────────────────────────────────────
VERBOSE=false
RELEASE_HUGEPAGES=false
DRY_RUN=false
FORCE=false

usage() {
    cat <<EOF
${BOLD}用法${RESET}：sudo ./benchmarks/latency/scripts/dpdk-teardown.sh [选项]

${BOLD}恢复 DPDK 网卡到内核驱动，可选释放 hugepages。${RESET}

${BOLD}选项${RESET}：
  --release-hugepages  同时释放 hugepages 归还内存
  -f, --force          强制恢复（跳过运行进程检查）
  -v, --verbose        详细模式
  --dry-run            显示会执行什么，但不实际执行
  -h, --help           显示此帮助

${BOLD}环境变量${RESET}（覆盖 .dpdk_state 中的值）：
  DPDK_PCI          PCI 地址
  DPDK_IFACE        接口名
  KERNEL_DRIVER     恢复到的内核驱动
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose)           VERBOSE=true; shift ;;
        -f|--force)             FORCE=true; shift ;;
        --release-hugepages)    RELEASE_HUGEPAGES=true; shift ;;
        --dry-run)              DRY_RUN=true; shift ;;
        -h|--help)              usage; exit 0 ;;
        *)                      die "未知选项：$1" "  查看帮助：sudo $0 --help" ;;
    esac
done

# ──────────────────────────────────────────
# 加载保存的状态
# ──────────────────────────────────────────
load_state() {
    step "加载 DPDK 状态"
    verbose ".dpdk_state 文件记录了 dpdk-setup.sh 配置时的参数。"

    if [[ -f "$STATE_FILE" ]]; then
        local saved_pci saved_iface saved_driver saved_ip
        saved_pci="$DPDK_PCI"; saved_iface="$DPDK_IFACE"
        saved_driver="$KERNEL_DRIVER"; saved_ip="$DPDK_NIC_IP"

        # shellcheck disable=SC1090
        source "$STATE_FILE"

        [[ -n "$saved_pci" ]]     && DPDK_PCI="$saved_pci"
        [[ -n "$saved_iface" ]]   && DPDK_IFACE="$saved_iface"
        [[ -n "$saved_driver" ]]  && KERNEL_DRIVER="$saved_driver"
        [[ -n "$saved_ip" ]]      && DPDK_NIC_IP="$saved_ip"

        ok "从 ${STATE_FILE} 加载"
        info "  PCI=${DPDK_PCI}  接口=${DPDK_IFACE}  驱动=${KERNEL_DRIVER}"
        [[ -n "${SETUP_TIME:-}" ]] && info "  配置时间：${SETUP_TIME}"
    else
        warn "未找到 ${STATE_FILE}"
        verbose "没有 setup 留下的状态文件，将尝试自动检测。"
    fi

    if [[ -z "$DPDK_PCI" ]]; then
        step "自动检测 vfio-pci 设备"
        local vfio_line
        vfio_line=$(dpdk-devbind.py --status 2>/dev/null | grep 'drv=vfio-pci' | head -1 || true)
        if [[ -n "$vfio_line" ]]; then
            DPDK_PCI=$(echo "$vfio_line" | grep -oP '^\S+')
            ok "检测到 vfio-pci 设备：${DPDK_PCI}"
        else
            die "没有找到绑定到 vfio-pci 的设备，无需恢复" \
                "  查看状态：dpdk-devbind.py --status"
        fi
    fi

    if [[ -z "$KERNEL_DRIVER" ]]; then
        local modalias
        modalias=$(cat "/sys/bus/pci/devices/$DPDK_PCI/modalias" 2>/dev/null || echo "")
        if [[ -n "$modalias" ]]; then
            KERNEL_DRIVER=$(modprobe --resolve-alias "$modalias" 2>/dev/null | head -1 || echo "")
        fi
        KERNEL_DRIVER="${KERNEL_DRIVER:-ena}"
        info "推断内核驱动：${KERNEL_DRIVER}"
    fi

    DPDK_IFACE="${DPDK_IFACE:-}"
}

# ──────────────────────────────────────────
# 前置检查
# ──────────────────────────────────────────
pre_check() {
    step "前置检查"

    if [[ $EUID -ne 0 ]]; then
        die "需要 root 权限"
    fi
    ok "root 权限"

    if [[ ! -e "/sys/bus/pci/devices/$DPDK_PCI" ]]; then
        die "PCI 设备 $DPDK_PCI 不存在"
    fi
    ok "PCI 设备 $DPDK_PCI 存在"

    local current_driver
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "未绑定")
    if [[ "$current_driver" == "$KERNEL_DRIVER" ]]; then
        ok "网卡已在内核驱动 ${KERNEL_DRIVER} 上（无需恢复）"
        if [[ "$RELEASE_HUGEPAGES" != true ]]; then
            [[ -f "$STATE_FILE" ]] && rm -f "$STATE_FILE" && ok "已清理残留 ${STATE_FILE}"
            exit 0
        fi
    else
        info "当前驱动：${current_driver} → 目标：${KERNEL_DRIVER}"
    fi

    check_running_processes
}

# ──────────────────────────────────────────
# 检查运行中的 DPDK 进程
# ──────────────────────────────────────────
check_running_processes() {
    verbose "检查是否有 DPDK 进程正在使用此网卡。"

    local dpdk_pids=()
    if [[ -d /var/run/dpdk ]]; then
        while IFS= read -r pidfile; do
            local pid
            pid=$(basename "$(dirname "$pidfile")")
            if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
                dpdk_pids+=("$pid")
            fi
        done < <(find /var/run/dpdk -name 'config' 2>/dev/null)
    fi

    local vfio_pids
    vfio_pids=$(fuser /dev/vfio/* 2>/dev/null | tr -s ' ' '\n' | sort -u || true)
    for pid in $vfio_pids; do
        if [[ "$pid" =~ ^[0-9]+$ ]] && ! printf '%s\n' "${dpdk_pids[@]}" | grep -qx "$pid" 2>/dev/null; then
            dpdk_pids+=("$pid")
        fi
    done

    if [[ ${#dpdk_pids[@]} -gt 0 ]]; then
        warn "发现 ${#dpdk_pids[@]} 个 DPDK 进程可能正在使用此网卡："
        for pid in "${dpdk_pids[@]}"; do
            local cmdline
            cmdline=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || echo "<无法读取>")
            info "  PID ${pid}：${cmdline}"
        done

        if [[ "$FORCE" == true ]]; then
            warn "使用 --force，继续恢复（进程可能崩溃）"
        else
            die "有 DPDK 进程正在运行，解绑网卡可能导致崩溃" \
                "  先停止进程：kill ${dpdk_pids[*]}\n  或强制恢复：sudo $0 --force"
        fi
    else
        ok "没有 DPDK 进程在运行"
    fi
}

# ──────────────────────────────────────────
# 1. 恢复网卡到内核驱动
# ──────────────────────────────────────────
unbind_nic() {
    step "恢复网卡 ${DPDK_PCI} 到 ${KERNEL_DRIVER}"
    verbose "将网卡从 vfio-pci 解绑，重新交给内核驱动（${KERNEL_DRIVER}）管理。"

    local current_driver
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "none")

    if [[ "$current_driver" == "$KERNEL_DRIVER" ]]; then
        ok "已在 ${KERNEL_DRIVER} 上"
        return
    fi

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] dpdk-devbind.py --bind=${KERNEL_DRIVER} ${DPDK_PCI}"
        return
    fi

    dpdk-devbind.py --bind="$KERNEL_DRIVER" "$DPDK_PCI"

    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "none")
    if [[ "$current_driver" != "$KERNEL_DRIVER" ]]; then
        die "恢复失败：当前驱动仍为 ${current_driver}"
    fi

    ok "网卡已恢复到 ${KERNEL_DRIVER}"

    step "等待接口恢复"
    verbose "内核重新识别网卡需要几秒钟。"

    local retries=20
    local found_iface=""
    while [[ $retries -gt 0 ]]; do
        if ip link show "$DPDK_IFACE" &>/dev/null; then
            found_iface="$DPDK_IFACE"
            break
        fi
        local candidate
        candidate=$(ls "/sys/bus/pci/devices/$DPDK_PCI/net/" 2>/dev/null | head -1 || true)
        if [[ -n "$candidate" ]]; then
            found_iface="$candidate"
            break
        fi
        sleep 0.5
        retries=$((retries - 1))
    done

    if [[ -z "$found_iface" ]]; then
        warn "接口未出现（等待 10 秒后超时）"
        info "  手动检查：ls /sys/bus/pci/devices/${DPDK_PCI}/net/"
        return
    fi

    if [[ "$found_iface" != "$DPDK_IFACE" ]]; then
        warn "接口名变更：${DPDK_IFACE} → ${found_iface}"
        DPDK_IFACE="$found_iface"
    fi

    ip link set "$found_iface" up 2>/dev/null || true
    ok "接口 ${found_iface} 已启用"

    # Do NOT wait for DHCP here — the bench wrapper may immediately move
    # this NIC into a netns before the dhcp client can even start. Return
    # as soon as the netdev exists and is up; IP assignment is done by
    # the caller (lat script assigns static IPs inside bench_ns).
}

# ──────────────────────────────────────────
# 2. 释放 Hugepages（可选）
# ──────────────────────────────────────────
release_hugepages() {
    if [[ "$RELEASE_HUGEPAGES" != true ]]; then
        return
    fi

    step "释放 Hugepages"

    local current
    current=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)

    if [[ "$current" -eq 0 ]]; then
        ok "没有 hugepages 需要释放"
        return
    fi

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
        return
    fi

    echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

    local after
    after=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    if [[ "$after" -eq 0 ]]; then
        ok "已释放 ${current} 页 hugepages（$((current * 2))MB）"
    else
        warn "仍有 ${after} 页 hugepages"
    fi
}

# ──────────────────────────────────────────
# 3. 清理状态文件
# ──────────────────────────────────────────
cleanup_state() {
    if [[ "$DRY_RUN" == true ]]; then
        return
    fi

    if [[ -f "$STATE_FILE" ]]; then
        rm -f "$STATE_FILE"
        ok "已清理 ${STATE_FILE}"
    fi
}

# ──────────────────────────────────────────
# 结果报告
# ──────────────────────────────────────────
report_results() {
    separator
    echo -e "${BOLD}${GREEN}✅ DPDK 环境已恢复${RESET}"
    echo ""

    dpdk-devbind.py --status 2>&1 | grep -E 'kernel driver|drv=' | head -8 || true

    local hp_total
    hp_total=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)

    separator
    echo -e "  网卡    ：${DPDK_IFACE} → ${KERNEL_DRIVER}"
    echo -e "  Hugepages：${hp_total} 页"

    if [[ "$hp_total" -gt 0 && "$RELEASE_HUGEPAGES" != true ]]; then
        echo ""
        suggest "Hugepages 仍保留（下次 DPDK 启动更快）"
        info "  释放方式：sudo $0 --release-hugepages"
    fi
}

# ──────────────────────────────────────────
# 主流程
# ──────────────────────────────────────────
main() {
    separator
    echo -e "${BOLD}dpdk-teardown.sh — 恢复 DPDK 网卡到内核${RESET}"
    [[ "$VERBOSE" == true ]] && info "详细模式已开启"
    [[ "$DRY_RUN" == true ]] && info "模拟运行模式：不实际执行"

    load_state
    pre_check
    unbind_nic
    release_hugepages
    cleanup_state
    report_results
}

main "$@"

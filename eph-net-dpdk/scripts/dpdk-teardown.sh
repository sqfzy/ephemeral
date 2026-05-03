#!/usr/bin/env bash
# dpdk-teardown.sh — undo dpdk-setup.sh: rebind NIC to kernel, optionally free hugepages
#
# ┌── Script roles in this repo ────────────────────────────────────────────┐
# │ eph-net-dpdk/scripts/dpdk-setup.sh         one-shot host env: vfio + hugepg │
# │ eph-net-dpdk/scripts/dpdk-teardown.sh      this script — restore kernel NIC │
# │ benchmarks/latency/scripts/            NIC RX coalescing tuning         │
# │   setup_coalescing.sh                                                   │
# │ benchmarks/latency/lat                 per-run bench wrapper            │
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
#     benchmarks/latency/scripts/setup_coalescing.sh --restore for that —
#     they are independent concerns)
#   - Move the NIC into any netns (use `ip link set <nic> netns <ns>` or
#     run benchmarks/latency/lat which manages the bench_ns transitions)
#
# If the NIC is currently in a non-default netns (e.g. bench_ns from a
# previous `lat` run), it is already on the kernel driver and this script
# has nothing to undo — it will exit cleanly.
#
# 用法：sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh [选项]

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
# 跨主机 探测辅助
# ──────────────────────────────────────────

# 查找 dpdk-devbind.py — $PATH 优先，然后常见安装路径
DEVBIND=""
find_devbind() {
    if command -v dpdk-devbind.py &>/dev/null; then
        DEVBIND=$(command -v dpdk-devbind.py)
        return 0
    fi
    local c
    for c in \
        /usr/local/share/dpdk/usertools/dpdk-devbind.py \
        /usr/share/dpdk/usertools/dpdk-devbind.py \
        /usr/local/bin/dpdk-devbind.py; do
        if [[ -x "$c" ]]; then
            DEVBIND="$c"
            return 0
        fi
    done
    return 1
}

# 主动触发 DHCP / 网络管理器为接口重新获取 IP
request_dhcp() {
    local iface="$1"
    if command -v dhclient &>/dev/null; then
        verbose "使用 dhclient 异步获取 IP"
        dhclient -nw "$iface" 2>/dev/null || true
        return 0
    fi
    if command -v networkctl &>/dev/null && systemctl is-active --quiet systemd-networkd 2>/dev/null; then
        verbose "systemd-networkd 已管理，触发 reconfigure"
        networkctl reconfigure "$iface" 2>/dev/null || true
        return 0
    fi
    if command -v nmcli &>/dev/null && systemctl is-active --quiet NetworkManager 2>/dev/null; then
        verbose "NetworkManager 已管理，触发 connection up"
        nmcli device connect "$iface" 2>/dev/null || nmcli device reapply "$iface" 2>/dev/null || true
        return 0
    fi
    if command -v dhcpcd &>/dev/null; then
        verbose "使用 dhcpcd 获取 IP"
        dhcpcd "$iface" 2>/dev/null || true
        return 0
    fi
    verbose "未检测到已知网络管理器（dhclient/networkd/NetworkManager/dhcpcd）"
    return 1
}

# ──────────────────────────────────────────
# 默认配置
# ──────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Repo root, regardless of where this script lives. .dpdk_state lives at
# the top level of the checkout (so scripts/lat can read it).
if PROJECT_DIR=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null); then
    :
else
    PROJECT_DIR="$SCRIPT_DIR"
    while [[ "$PROJECT_DIR" != "/" && ! -d "$PROJECT_DIR/eph-net-dpdk" ]]; do
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
${BOLD}用法${RESET}：sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh [选项]

${BOLD}恢复 DPDK 网卡到内核驱动，可选释放 hugepages。${RESET}
自动读取 .dpdk_state 获取 setup 时的配置。

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

${BOLD}示例${RESET}：
  sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh                       # 恢复网卡
  sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh --release-hugepages   # 恢复 + 释放 hugepages
  sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh -f                    # 强制恢复（即使有进程在用）
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose)           VERBOSE=true; shift ;;
        -f|--force)             FORCE=true; shift ;;
        --release-hugepages)    RELEASE_HUGEPAGES=true; shift ;;
        --dry-run)              DRY_RUN=true; shift ;;
        -h|--help)              usage; exit 0 ;;
        *)                      die "未知选项：$1" "  查看帮助：sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh --help" ;;
    esac
done

# ──────────────────────────────────────────
# 加载保存的状态
# ──────────────────────────────────────────
load_state() {
    step "加载 DPDK 状态"
    verbose ".dpdk_state 文件记录了 dpdk-setup.sh 配置时的参数，确保恢复到正确状态。"

    if [[ -f "$STATE_FILE" ]]; then
        # 先 source，再让环境变量覆盖（环境变量优先级更高）
        local saved_pci saved_iface saved_driver saved_ip
        saved_pci="$DPDK_PCI"; saved_iface="$DPDK_IFACE"
        saved_driver="$KERNEL_DRIVER"; saved_ip="$DPDK_NIC_IP"

        # shellcheck disable=SC1090
        source "$STATE_FILE"

        # 环境变量覆盖 state file
        [[ -n "$saved_pci" ]]     && DPDK_PCI="$saved_pci"
        [[ -n "$saved_iface" ]]   && DPDK_IFACE="$saved_iface"
        [[ -n "$saved_driver" ]]  && KERNEL_DRIVER="$saved_driver"
        [[ -n "$saved_ip" ]]      && DPDK_NIC_IP="$saved_ip"

        ok "从 ${STATE_FILE} 加载"
        info "  PCI=${DPDK_PCI}  接口=${DPDK_IFACE}  驱动=${KERNEL_DRIVER}"
        [[ -n "${SETUP_TIME:-}" ]] && info "  配置时间：${SETUP_TIME}"
    else
        warn "未找到 ${STATE_FILE}"
        verbose "没有 setup 留下的状态文件，将尝试自动检测或使用默认值。"
    fi

    # 自动检测：如果仍没有 PCI 地址，扫描 vfio-pci 绑定的设备
    if [[ -z "$DPDK_PCI" ]]; then
        step "自动检测 vfio-pci 设备"
        local vfio_line
        vfio_line=$("$DEVBIND" --status 2>/dev/null | grep 'drv=vfio-pci' | head -1 || true)
        if [[ -n "$vfio_line" ]]; then
            DPDK_PCI=$(echo "$vfio_line" | grep -oP '^\S+')
            ok "检测到 vfio-pci 设备：${DPDK_PCI}"
        else
            die "没有找到绑定到 vfio-pci 的设备，无需恢复" \
                "  查看状态：${DEVBIND} --status"
        fi
    fi

    # 自动检测内核驱动（从设备的 modalias 推断）—— 拒绝猜测，错猜会把网卡绑到错的驱动
    if [[ -z "$KERNEL_DRIVER" ]]; then
        local modalias
        modalias=$(cat "/sys/bus/pci/devices/$DPDK_PCI/modalias" 2>/dev/null || echo "")
        if [[ -n "$modalias" ]]; then
            KERNEL_DRIVER=$(modprobe --resolve-alias "$modalias" 2>/dev/null | head -1 || echo "")
        fi
        if [[ -z "$KERNEL_DRIVER" ]]; then
            die "无法推断 ${DPDK_PCI} 的原内核驱动" \
                "  .dpdk_state 缺失且 modalias 解析失败；拒绝猜测（绑错驱动会损坏网卡状态）\n  手动指定：sudo KERNEL_DRIVER=<驱动名> ./eph-net-dpdk/scripts/dpdk-teardown.sh\n  常见值：ena (AWS), ixgbe/i40e/ice (Intel), mlx5_core (Mellanox), r8169 (Realtek)\n  查看 modalias：cat /sys/bus/pci/devices/${DPDK_PCI}/modalias"
        fi
        info "推断内核驱动：${KERNEL_DRIVER}"
    fi

    # 接口名：绑定前可能知道，绑定后由 unbind_nic() 通过 PCI 反查确认
    # 不设硬编码默认值——如果没有 state file 也没有环境变量，
    # unbind_nic() 会通过 /sys/bus/pci/devices/$DPDK_PCI/net/ 检测
    DPDK_IFACE="${DPDK_IFACE:-}"
}

# ──────────────────────────────────────────
# 前置检查
# ──────────────────────────────────────────
pre_check() {
    step "前置检查"

    if [[ $EUID -ne 0 ]]; then
        die "需要 root 权限" \
            "  运行方式：sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh"
    fi
    ok "root 权限"

    if [[ ! -e "/sys/bus/pci/devices/$DPDK_PCI" ]]; then
        die "PCI 设备 $DPDK_PCI 不存在" \
            "  查看可用设备：dpdk-devbind.py --status"
    fi
    ok "PCI 设备 $DPDK_PCI 存在"

    # 检查当前驱动
    local current_driver
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "未绑定")
    if [[ "$current_driver" == "$KERNEL_DRIVER" ]]; then
        ok "网卡已在内核驱动 ${KERNEL_DRIVER} 上（无需恢复）"
        if [[ "$RELEASE_HUGEPAGES" != true ]]; then
            # 清理残留 state file
            [[ -f "$STATE_FILE" ]] && rm -f "$STATE_FILE" && ok "已清理残留 ${STATE_FILE}"
            exit 0
        fi
    else
        info "当前驱动：${current_driver} → 目标：${KERNEL_DRIVER}"
    fi

    # 检查是否有 DPDK 进程还在使用这张网卡
    check_running_processes
}

# ──────────────────────────────────────────
# 检查运行中的 DPDK 进程
# ──────────────────────────────────────────
check_running_processes() {
    verbose "检查是否有 DPDK 进程正在使用此网卡。如果有，强制解绑可能导致进程崩溃。"

    # 方法 1：检查 /var/run/dpdk 下的运行时 mmap'd config 文件。
    #
    # DPDK runtime layout is /var/run/dpdk/<file_prefix>/config — the
    # parent dir is the EAL file_prefix (e.g. "eph_0000_28_00_0"), NOT
    # a PID. The previous code did `basename $(dirname $pidfile)` and
    # tried to regex-match it as a number; the match always failed and
    # method 1 silently produced no PIDs. fuser(1) is the right tool
    # here: every DPDK primary AND secondary mmaps `config`, so its
    # `m` flag picks them all up.
    local dpdk_pids=()
    if [[ -d /var/run/dpdk ]]; then
        while IFS= read -r cfg; do
            # fuser prints "<file>: <pid>m <pid>m" — strip suffix flags
            # and dedup. Run with sudo if not root (the dir is 0700 root).
            local fuser_out pid
            fuser_out=$(fuser "$cfg" 2>/dev/null || true)
            for pid in $fuser_out; do
                pid="${pid%[a-zA-Z]}"  # strip access-mode suffix (m / e / r ...)
                if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
                    # dedup: a single PID may map both primary's and any
                    # secondary's config in the same fuser line on multi-NIC
                    # daemons; keep the array unique.
                    if ! printf '%s\n' "${dpdk_pids[@]}" 2>/dev/null | grep -qx "$pid"; then
                        dpdk_pids+=("$pid")
                    fi
                fi
            done
        done < <(find /var/run/dpdk -name 'config' 2>/dev/null)
    fi

    # 方法 2：检查 /dev/vfio 的打开者
    #
    # 在无 vfio-pci（或模块未加载）的环境里 /dev/vfio 目录本身不存在，
    # fuser 会打印 "specified filename ... does not exist" 然后非零退出。
    # 之前用 `2>/dev/null || true` 把错误都吞掉，会让 /dev/vfio 缺失这种
    # 需要关注的状态静默通过 —— 本应是 "没人在跑 DPDK"，实际上是 "这台
    # 机器从未进过 vfio 态"。分开处理：仅在目录存在时才 fuser，并把
    # 异常日志保留到 verbose 通路。
    local vfio_pids=""
    if [[ -d /dev/vfio ]]; then
        vfio_pids=$(fuser /dev/vfio/* 2>/dev/null | tr -s ' ' '\n' | sort -u || true)
    else
        verbose "/dev/vfio 目录不存在，跳过 fuser 检查（vfio-pci 可能未加载）"
    fi
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
                "  先停止进程：kill ${dpdk_pids[*]}\n  或强制恢复：sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh --force"
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
    verbose "将网卡从 DPDK 的 vfio-pci 驱动解绑，重新交给内核驱动（${KERNEL_DRIVER}）管理。"

    local current_driver
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "none")

    if [[ "$current_driver" == "$KERNEL_DRIVER" ]]; then
        ok "已在 ${KERNEL_DRIVER} 上"
        return
    fi

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] ${DEVBIND} --bind=${KERNEL_DRIVER} ${DPDK_PCI}"
        return
    fi

    "$DEVBIND" --bind="$KERNEL_DRIVER" "$DPDK_PCI"

    # 验证
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "none")
    if [[ "$current_driver" != "$KERNEL_DRIVER" ]]; then
        die "恢复失败：当前驱动仍为 ${current_driver}" \
            "  手动尝试：${DEVBIND} --bind=${KERNEL_DRIVER} ${DPDK_PCI} --force"
    fi

    ok "网卡已恢复到 ${KERNEL_DRIVER}"

    # 等待接口出现并启用
    step "等待接口恢复"
    verbose "内核重新识别网卡需要几秒钟，然后 DHCP 会自动获取 IP。"

    local retries=20
    local found_iface=""
    while [[ $retries -gt 0 ]]; do
        if ip link show "$DPDK_IFACE" &>/dev/null; then
            found_iface="$DPDK_IFACE"
            break
        fi
        # 接口名可能变了，通过 PCI 地址反查
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
        info "  手动启用：ip link set <接口名> up"
        return
    fi

    if [[ "$found_iface" != "$DPDK_IFACE" ]]; then
        warn "接口名变更：${DPDK_IFACE} → ${found_iface}"
        DPDK_IFACE="$found_iface"
    fi

    ip link set "$found_iface" up 2>/dev/null || true
    ok "接口 ${found_iface} 已启用"

    # 主动触发网络管理器重新获取 IP（dhclient/systemd-networkd/NetworkManager/dhcpcd）
    request_dhcp "$found_iface" || \
        warn "未能主动触发 DHCP（未检测到已知管理器），等待系统自动恢复"

    # 等待接口获取 IP（最多 10 秒）
    step "等待接口获取 IP"
    verbose "取决于网络管理方式：dhclient / systemd-networkd / NetworkManager / 静态配置。"
    retries=20
    while [[ $retries -gt 0 ]]; do
        local current_ip
        current_ip=$(ip -4 addr show "$found_iface" 2>/dev/null | grep -oP 'inet \K[\d.]+' || true)
        if [[ -n "$current_ip" ]]; then
            ok "IP 已获取：${current_ip}"
            return
        fi
        sleep 0.5
        retries=$((retries - 1))
    done

    warn "10 秒内未检测到 IP"
    info "  静态 IP 系统：按本机 netplan/ifcfg/systemd-networkd 配置手动恢复"
    info "  手动触发 DHCP：dhclient ${found_iface}   或   dhcpcd ${found_iface}"
    info "  检查状态：ip addr show ${found_iface}"
}

# ──────────────────────────────────────────
# 2. 释放 Hugepages（可选）
# ──────────────────────────────────────────
release_hugepages() {
    if [[ "$RELEASE_HUGEPAGES" != true ]]; then
        return
    fi

    step "释放 Hugepages"
    verbose "将 hugepages 数量设为 0，归还内存给系统。"

    local current
    current=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)

    if [[ "$current" -eq 0 ]]; then
        ok "没有 hugepages 需要释放"
        return
    fi

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
        info "[模拟] 将释放 ${current} 页（$((current * 2))MB）"
        return
    fi

    echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

    local after
    after=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    if [[ "$after" -eq 0 ]]; then
        ok "已释放 ${current} 页 hugepages（$((current * 2))MB）"
    else
        warn "仍有 ${after} 页 hugepages（可能有进程在使用）"
        info "  检查占用：grep -i huge /proc/meminfo"
        info "  强制释放：先 kill DPDK 进程，再重新运行此脚本"
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

    "$DEVBIND" --status 2>&1 | grep -E 'kernel driver|drv=' | head -8

    local hp_total
    hp_total=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
    local nic_ip
    nic_ip=$(ip -4 addr show "$DPDK_IFACE" 2>/dev/null | grep -oP 'inet \K[\d.]+' || echo "未获取")

    separator
    echo -e "  网卡    ：${DPDK_IFACE} → ${KERNEL_DRIVER}（IP：${nic_ip}）"
    echo -e "  Hugepages：${hp_total} 页"

    if [[ "$hp_total" -gt 0 && "$RELEASE_HUGEPAGES" != true ]]; then
        echo ""
        suggest "Hugepages 仍保留（下次 DPDK 启动更快）"
        info "  释放方式：sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh --release-hugepages"
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

    # find dpdk-devbind.py first — load_state 依赖它做 vfio-pci 自动检测
    if ! find_devbind; then
        die "dpdk-devbind.py 未找到" \
            "  已搜索：\$PATH、/usr/local/share/dpdk/usertools、/usr/share/dpdk/usertools、/usr/local/bin\n  或手动加入 PATH：export PATH=\$PATH:/usr/local/share/dpdk/usertools"
    fi
    verbose "dpdk-devbind.py：${DEVBIND}"

    load_state
    pre_check
    unbind_nic
    release_hugepages
    cleanup_state
    report_results
}

main "$@"

#!/usr/bin/env bash
# dpdk-setup.sh — 配置 DPDK 独占网卡环境（hugepages + VFIO + NIC 绑定）
# 生成时间：2026-03-23
# 用法：sudo ./eph-dpdk/scripts/dpdk-setup.sh [选项]

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
# 默认配置（可通过环境变量覆盖）
# ──────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Repo root, regardless of where this script lives. The .dpdk_state file
# and the build/ tree both sit at the repo root, not under eph-dpdk/.
if PROJECT_DIR=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null); then
    :
else
    PROJECT_DIR="$SCRIPT_DIR"
    while [[ "$PROJECT_DIR" != "/" && ! -d "$PROJECT_DIR/eph-dpdk" ]]; do
        PROJECT_DIR="$(dirname "$PROJECT_DIR")"
    done
    [[ "$PROJECT_DIR" == "/" ]] && { echo "cannot locate project root from $SCRIPT_DIR" >&2; exit 1; }
fi

NR_HUGEPAGES="${NR_HUGEPAGES:-256}"

# 这些变量可以手动指定，也可以留空让脚本自动检测
DPDK_PCI="${DPDK_PCI:-}"
DPDK_IFACE="${DPDK_IFACE:-}"
KERNEL_DRIVER="${KERNEL_DRIVER:-}"

# ──────────────────────────────────────────
# 参数解析
# ──────────────────────────────────────────
VERBOSE=false
CHECK_ONLY=false
DRY_RUN=false
SKIP_CONFIRM=false

usage() {
    cat <<EOF
${BOLD}用法${RESET}：sudo ./eph-dpdk/scripts/dpdk-setup.sh [选项]

${BOLD}配置 DPDK 独占网卡环境：加载 VFIO、分配 hugepages、绑定网卡到 vfio-pci。${RESET}
自动检测 SSH 网卡并选择另一张网卡用于 DPDK，无需手动指定。

${BOLD}选项${RESET}：
  -v, --verbose     详细模式：解释每步操作的原因
  -y, --yes         跳过确认提示，直接执行
  --check-only      只检查当前状态，不做任何修改
  --dry-run         显示会执行什么，但不实际执行
  -h, --help        显示此帮助

${BOLD}环境变量${RESET}（覆盖自动检测）：
  DPDK_PCI          DPDK 网卡的 PCI 地址（自动检测）
  DPDK_IFACE        DPDK 网卡的内核接口名（自动检测）
  NR_HUGEPAGES      2MB hugepages 数量（默认：256，即 512MB）
  KERNEL_DRIVER     网卡的内核驱动名（自动检测）

${BOLD}示例${RESET}：
  sudo ./eph-dpdk/scripts/dpdk-setup.sh                     # 自动检测网卡并配置
  sudo ./eph-dpdk/scripts/dpdk-setup.sh --verbose            # 详细模式（推荐初次使用）
  sudo ./eph-dpdk/scripts/dpdk-setup.sh --check-only         # 只查看当前状态
  sudo ./eph-dpdk/scripts/dpdk-setup.sh -y                   # 跳过确认
  sudo DPDK_PCI=0000:29:00.0 ./eph-dpdk/scripts/dpdk-setup.sh  # 手动指定网卡

${BOLD}恢复${RESET}：
  sudo ./eph-dpdk/scripts/dpdk-teardown.sh
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose)   VERBOSE=true; shift ;;
        -y|--yes)       SKIP_CONFIRM=true; shift ;;
        --check-only)   CHECK_ONLY=true; shift ;;
        --dry-run)      DRY_RUN=true; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              die "未知选项：$1" "  查看帮助：sudo ./eph-dpdk/scripts/dpdk-setup.sh --help" ;;
    esac
done

# ──────────────────────────────────────────
# 网卡自动检测
# ──────────────────────────────────────────
detect_ssh_iface() {
    # 方法 1：通过 SSH_CONNECTION（sudo -E 保留时可用）
    if [[ -n "${SSH_CONNECTION:-}" ]]; then
        local ssh_client_ip="${SSH_CONNECTION%% *}"
        local iface
        iface=$(ip route get "$ssh_client_ip" 2>/dev/null | grep -oP 'dev \K\S+' || true)
        if [[ -n "$iface" && "$iface" != "lo" ]]; then
            echo "$iface"
            return
        fi
    fi

    # 方法 2：通过 ss 查 SSHD 连接的本地 IP → 反查接口
    local ssh_local_ip
    ssh_local_ip=$(ss -tnp 2>/dev/null | grep -i 'sshd\|ssh' | head -1 | awk '{print $4}' | rev | cut -d: -f2- | rev)
    if [[ -n "${ssh_local_ip:-}" && "$ssh_local_ip" != "*" ]]; then
        local iface
        iface=$(ip -o addr show 2>/dev/null | grep "inet ${ssh_local_ip}/" | awk '{print $2}' || true)
        if [[ -n "$iface" && "$iface" != "lo" ]]; then
            echo "$iface"
            return
        fi
    fi

    # 方法 3：通过 /proc/net/tcp 找 sshd 监听的连接（最可靠的 sudo 下方法）
    # 找到 sshd 进程打开的已建立 TCP 连接的本地 IP
    local sshd_pid
    sshd_pid=$(pgrep -o sshd 2>/dev/null || true)
    if [[ -n "$sshd_pid" ]]; then
        # 遍历 sshd 子进程（每个 SSH 连接是一个 sshd fork）
        local child_pids
        child_pids=$(pgrep -P "$sshd_pid" 2>/dev/null || true)
        for pid in $child_pids; do
            local local_ip
            local_ip=$(ss -tnp 2>/dev/null | grep "pid=${pid}," | head -1 | awk '{print $4}' | rev | cut -d: -f2- | rev || true)
            if [[ -n "$local_ip" && "$local_ip" != "*" && "$local_ip" != "0.0.0.0" && "$local_ip" != "::" ]]; then
                local iface
                iface=$(ip -o addr show 2>/dev/null | grep "inet ${local_ip}/" | awk '{print $2}' || true)
                if [[ -n "$iface" && "$iface" != "lo" ]]; then
                    echo "$iface"
                    return
                fi
            fi
        done
    fi

    # 方法 4：默认路由的接口（最后的 fallback）
    ip route show default 2>/dev/null | head -1 | awk '{print $5}' || true
}

detect_nic() {
    step "自动检测网卡"
    verbose "扫描系统网卡，找到 SSH 正在使用的网卡，然后选择另一张给 DPDK。"

    # 找到 SSH 用的网卡
    local ssh_iface
    ssh_iface=$(detect_ssh_iface)

    if [[ -z "$ssh_iface" ]]; then
        warn "无法检测 SSH 使用的网卡，将使用默认路由接口"
        ssh_iface=$(ip route show default 2>/dev/null | head -1 | awk '{print $5}')
    fi
    ok "SSH/管理网卡：${ssh_iface}"
    verbose "这张网卡承载 SSH 连接，绝对不能动，否则会断连。"

    # 列举所有物理网卡（排除 lo、docker、veth 等）
    local -a all_nics=()
    local -a dpdk_candidates=()

    while IFS= read -r iface; do
        [[ -z "$iface" ]] && continue
        # 跳过虚拟接口
        [[ "$iface" == lo ]] && continue
        [[ "$iface" == docker* ]] && continue
        [[ "$iface" == veth* ]] && continue
        [[ "$iface" == br-* ]] && continue

        # 需要有 PCI 设备（物理网卡）
        local pci_path="/sys/class/net/$iface/device"
        [[ ! -L "$pci_path" ]] && continue

        local pci_addr
        pci_addr=$(basename "$(readlink -f "$pci_path")")
        local driver
        driver=$(basename "$(readlink -f "$pci_path/driver")" 2>/dev/null || echo "none")
        local ip_addr
        ip_addr=$(ip -4 addr show "$iface" 2>/dev/null | grep -oP 'inet \K[\d.]+' || echo "无 IP")
        local mac
        mac=$(cat "/sys/class/net/$iface/address" 2>/dev/null || echo "unknown")

        all_nics+=("$iface")

        if [[ "$iface" != "$ssh_iface" ]]; then
            dpdk_candidates+=("$iface|$pci_addr|$driver|$ip_addr|$mac")
        fi

        info "  ${iface}  PCI=${pci_addr}  驱动=${driver}  IP=${ip_addr}  MAC=${mac}$(
            [[ "$iface" == "$ssh_iface" ]] && echo "  ← SSH" || echo "")"
    done < <(ls /sys/class/net/)

    # 检查是否已有网卡绑到 vfio-pci（可能是上次 setup 没 teardown）
    local vfio_bound
    vfio_bound=$(dpdk-devbind.py --status 2>/dev/null | grep 'drv=vfio-pci' | grep -oP '\S+(?= .*)' || true)
    if [[ -n "$vfio_bound" ]]; then
        warn "发现已绑定到 vfio-pci 的设备：${vfio_bound}"
        info "如果是上次未恢复，先运行：sudo ./eph-dpdk/scripts/dpdk-teardown.sh"
    fi

    if [[ ${#dpdk_candidates[@]} -eq 0 ]]; then
        die "没有找到可用于 DPDK 的网卡（只有 SSH 网卡 ${ssh_iface}）" \
            "  至少需要 2 张网卡：一张 SSH，一张 DPDK\n  当前网卡：${all_nics[*]}"
    fi

    # 如果用户没手动指定，自动选第一个候选
    if [[ -z "$DPDK_IFACE" ]]; then
        IFS='|' read -r DPDK_IFACE DPDK_PCI KERNEL_DRIVER _ _ <<< "${dpdk_candidates[0]}"
    fi

    # 如果指定了接口但没指定 PCI/驱动，从系统读取
    if [[ -n "$DPDK_IFACE" && -z "$DPDK_PCI" ]]; then
        DPDK_PCI=$(basename "$(readlink -f "/sys/class/net/$DPDK_IFACE/device")" 2>/dev/null || echo "")
        [[ -z "$DPDK_PCI" ]] && die "无法获取 ${DPDK_IFACE} 的 PCI 地址"
    fi
    if [[ -n "$DPDK_IFACE" && -z "$KERNEL_DRIVER" ]]; then
        KERNEL_DRIVER=$(basename "$(readlink -f "/sys/class/net/$DPDK_IFACE/device/driver")" 2>/dev/null || echo "")
        [[ -z "$KERNEL_DRIVER" ]] && die "无法获取 ${DPDK_IFACE} 的内核驱动名"
    fi

    # 保存 DPDK 网卡绑定前的 IP（供运行示例命令使用）
    DPDK_NIC_IP=$(ip -4 addr show "$DPDK_IFACE" 2>/dev/null | grep -oP 'inet \K[\d.]+' || echo "")

    separator
    echo -e "${BOLD}网卡选择${RESET}"
    echo -e "  SSH 网卡（保留）：${ssh_iface}"
    echo -e "  DPDK 网卡（绑定）：${DPDK_IFACE}  PCI=${DPDK_PCI}  驱动=${KERNEL_DRIVER}"
    [[ -n "$DPDK_NIC_IP" ]] && echo -e "  DPDK 网卡 IP  ：${DPDK_NIC_IP}（绑定后 DPDK 程序使用此 IP）"
    separator
}

# ──────────────────────────────────────────
# 前置检查
# ──────────────────────────────────────────
pre_check() {
    step "前置检查"

    if [[ "$(uname)" != "Linux" ]]; then
        die "DPDK 仅支持 Linux" \
            "  当前系统：$(uname -s)\n  在 macOS/Windows 上请使用 Linux VM 或容器"
    fi
    ok "Linux 系统"

    if [[ $EUID -ne 0 ]]; then
        die "需要 root 权限" \
            "  运行方式：sudo ./eph-dpdk/scripts/dpdk-setup.sh\n  保留环境变量：sudo -E ./eph-dpdk/scripts/dpdk-setup.sh"
    fi
    ok "root 权限"

    if ! modinfo vfio-pci &>/dev/null; then
        die "vfio-pci 内核模块不可用" \
            "  检查内核配置：grep VFIO /boot/config-\$(uname -r)\n  安装方式：sudo dnf install kernel-modules-extra"
    fi
    ok "vfio-pci 模块可用"

    if ! command -v dpdk-devbind.py &>/dev/null; then
        die "dpdk-devbind.py 未找到" \
            "  确认 DPDK 已安装：which dpdk-devbind.py\n  或指定完整路径：/usr/local/share/dpdk/usertools/dpdk-devbind.py"
    fi
    ok "dpdk-devbind.py 可用"

    # 检查是否有 DPDK 进程正在运行
    local dpdk_procs
    dpdk_procs=$(find /var/run/dpdk -name 'config' -newer /proc/1/cmdline 2>/dev/null | head -5 || true)
    if [[ -n "$dpdk_procs" ]]; then
        warn "检测到可能正在运行的 DPDK 进程"
        info "  查看：ls -la /var/run/dpdk/"
        info "  如果是残留文件，可以忽略（setup 会正常覆盖）"
    fi
}

# ──────────────────────────────────────────
# 确认操作
# ──────────────────────────────────────────
confirm_action() {
    if [[ "$SKIP_CONFIRM" == true || "$DRY_RUN" == true || "$CHECK_ONLY" == true ]]; then
        return
    fi

    echo ""
    echo -e "${BOLD}${YELLOW}即将执行以下操作：${RESET}"
    echo -e "  1. 加载 vfio-pci 模块 + 开启 noiommu"
    echo -e "  2. 分配 ${NR_HUGEPAGES} 页 hugepages（$((NR_HUGEPAGES * 2))MB）"
    echo -e "  3. 将 ${DPDK_IFACE} (${DPDK_PCI}) 从 ${KERNEL_DRIVER} 绑到 vfio-pci"
    echo -e "     ${RED}⚠ 网卡 ${DPDK_IFACE} 将从内核消失，无法用于普通网络${RESET}"
    echo ""
    read -rp "$(echo -e "${BOLD}确认执行？ [y/N]${RESET} ")" answer
    case "${answer,,}" in
        y|yes) ok "已确认" ;;
        *)
            info "已取消。"
            info "跳过确认：sudo ./eph-dpdk/scripts/dpdk-setup.sh -y"
            exit 0
            ;;
    esac
}

# ──────────────────────────────────────────
# 环境报告
# ──────────────────────────────────────────
report_env() {
    separator
    echo -e "${BOLD}📋 环境报告${RESET}"

    local current_driver
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "未绑定")

    local hp_total hp_free
    hp_total=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
    hp_free=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages 2>/dev/null || echo 0)

    local noiommu
    noiommu=$(cat /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || echo "模块未加载")

    echo -e "  操作系统      ：$(uname -s) $(uname -r) ($(uname -m))"
    echo -e "  DPDK 网卡     ：${DPDK_IFACE} (${DPDK_PCI})"
    echo -e "  当前驱动      ：${current_driver}"
    echo -e "  Hugepages     ：${hp_total} 已分配 / ${hp_free} 空闲（目标：${NR_HUGEPAGES}）"
    echo -e "  VFIO noiommu  ：${noiommu}"
    separator
}

# ──────────────────────────────────────────
# 1. 加载 VFIO 模块
# ──────────────────────────────────────────
setup_vfio() {
    step "加载 VFIO 模块"
    verbose "VFIO（Virtual Function I/O）允许用户态程序安全地直接访问 PCI 设备。"
    verbose "AWS Graviton 实例没有硬件 IOMMU，所以需要开启 noiommu 模式。"

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] modprobe vfio-pci"
        info "[模拟] echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode"
        return
    fi

    modprobe vfio-pci
    ok "vfio-pci 模块已加载"

    local noiommu
    noiommu=$(cat /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || echo "N")
    if [[ "$noiommu" != "Y" ]]; then
        echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
        ok "noiommu 模式已开启"
    else
        ok "noiommu 模式已开启（已是）"
    fi
}

# ──────────────────────────────────────────
# 2. 分配 Hugepages
# ──────────────────────────────────────────
setup_hugepages() {
    step "分配 Hugepages"
    verbose "Hugepages 是大页内存（每页 2MB），DPDK 用它做零拷贝包缓冲区。"
    verbose "目标分配 ${NR_HUGEPAGES} 页 = $((NR_HUGEPAGES * 2))MB。"

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] echo ${NR_HUGEPAGES} > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
        return
    fi

    # 确保 hugetlbfs 已挂载
    if ! mount | grep -q hugetlbfs; then
        mkdir -p /dev/hugepages
        mount -t hugetlbfs nodev /dev/hugepages
        ok "hugetlbfs 已挂载到 /dev/hugepages"
    fi

    echo "$NR_HUGEPAGES" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

    local actual free
    actual=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    free=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)

    if [[ "$actual" -lt "$NR_HUGEPAGES" ]]; then
        warn "请求 ${NR_HUGEPAGES} 页但只分配到 ${actual} 页（内存不足或碎片化）"
        info "尝试：NR_HUGEPAGES=${actual} sudo ./eph-dpdk/scripts/dpdk-setup.sh"
        info "或重启后重试（减少碎片）"
    fi

    ok "Hugepages：已分配=${actual} 空闲=${free}（$((actual * 2))MB）"
}

# ──────────────────────────────────────────
# 3. 绑定网卡到 vfio-pci
# ──────────────────────────────────────────
bind_nic() {
    step "绑定网卡 ${DPDK_IFACE} (${DPDK_PCI}) 到 vfio-pci"
    verbose "将网卡从内核驱动（${KERNEL_DRIVER}）解绑，交给 DPDK 的 VFIO 驱动独占使用。"
    verbose "绑定后内核将无法使用此网卡。"

    local current_driver
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "none")

    if [[ "$current_driver" == "vfio-pci" ]]; then
        ok "网卡已绑定到 vfio-pci（无需操作）"
        return
    fi

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] ip link set ${DPDK_IFACE} down"
        info "[模拟] dpdk-devbind.py --bind=vfio-pci ${DPDK_PCI}"
        return
    fi

    ip link set "$DPDK_IFACE" down 2>/dev/null || true
    dpdk-devbind.py --bind=vfio-pci "$DPDK_PCI"

    # 验证绑定成功
    current_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$DPDK_PCI/driver")" 2>/dev/null || echo "none")
    if [[ "$current_driver" != "vfio-pci" ]]; then
        die "绑定失败：当前驱动仍为 ${current_driver}" \
            "  手动尝试：\n    echo '${DPDK_PCI}' > /sys/bus/pci/devices/${DPDK_PCI}/driver/unbind\n    echo 'vfio-pci' > /sys/bus/pci/devices/${DPDK_PCI}/driver_override\n    echo '${DPDK_PCI}' > /sys/bus/pci/drivers_probe"
    fi

    ok "网卡已绑定到 vfio-pci"
}

# ──────────────────────────────────────────
# 4. 保存状态供 teardown 使用
# ──────────────────────────────────────────
save_state() {
    local state_file="$PROJECT_DIR/.dpdk_state"

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] 保存状态到 ${state_file}"
        return
    fi

    cat > "$state_file" <<EOF
# DPDK 环境状态 — 由 dpdk-setup.sh 生成，供 dpdk-teardown.sh 使用
# 请勿手动修改
DPDK_PCI="${DPDK_PCI}"
DPDK_IFACE="${DPDK_IFACE}"
DPDK_NIC_IP="${DPDK_NIC_IP}"
KERNEL_DRIVER="${KERNEL_DRIVER}"
SETUP_TIME="$(date '+%Y-%m-%d %H:%M:%S')"
EOF
    ok "状态已保存到 ${state_file}"
}

# ──────────────────────────────────────────
# 5. 快速验证 EAL 能否初始化
# ──────────────────────────────────────────
verify_eal() {
    step "验证 DPDK EAL"
    verbose "用 dpdk-quickstart 的 --help 快速测试 EAL 能否初始化（不建立连接）。"

    local arch
    arch=$(uname -m)
    local test_bin="$PROJECT_DIR/build/linux/${arch}/release/dpdk_quickstart"
    if [[ ! -x "$test_bin" ]]; then
        info "跳过 EAL 验证（未找到 ${test_bin}）"
        info "构建：xmake build dpdk_quickstart"
        return
    fi

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] ${test_bin} -a ${DPDK_PCI} -- --help"
        return
    fi

    # 用 --help 触发 EAL init 但不实际连接
    local output
    if output=$("$test_bin" -a "$DPDK_PCI" -- --help 2>&1); then
        ok "EAL 初始化成功"
    else
        if echo "$output" | grep -q "EAL: Detected"; then
            ok "EAL 初始化成功（程序以帮助信息退出）"
        else
            warn "EAL 初始化可能有问题："
            echo "$output" | head -5 | while IFS= read -r line; do
                info "  $line"
            done
        fi
    fi
}

# ──────────────────────────────────────────
# 结果报告
# ──────────────────────────────────────────
report_results() {
    separator
    echo -e "${BOLD}${GREEN}✅ DPDK 环境就绪${RESET}"
    echo ""

    dpdk-devbind.py --status 2>&1 | grep -E 'DPDK-compat|drv=' | head -6 || true

    local gw_ip
    gw_ip=$(ip route show default | head -1 | awk '{print $3}')
    local local_ip="${DPDK_NIC_IP:-<DPDK网卡IP>}"

    separator
    suggest "运行示例："
    echo ""
    local arch
    arch=$(uname -m)
    echo -e "  ${BOLD}sudo $PROJECT_DIR/build/linux/${arch}/release/ws_echo_client \\\\${RESET}"
    echo -e "      -a ${DPDK_PCI} \\\\"
    echo -e "      -- \\\\"
    echo -e "      --backend dpdk \\\\"
    echo -e "      --host echo.websocket.org \\\\"
    echo -e "      --local-ip ${local_ip} \\\\"
    echo -e "      --gateway-ip ${gw_ip} \\\\"
    echo -e "      --count 3 --msg 'hello dpdk'"
    echo ""
    suggest "恢复网卡："
    info "  sudo ./eph-dpdk/scripts/dpdk-teardown.sh"
}

# ──────────────────────────────────────────
# 主流程
# ──────────────────────────────────────────
main() {
    separator
    echo -e "${BOLD}dpdk-setup.sh — 配置 DPDK 独占网卡环境${RESET}"
    [[ "$VERBOSE" == true ]] && info "详细模式已开启"
    [[ "$DRY_RUN" == true ]] && info "模拟运行模式：不实际执行"

    pre_check
    detect_nic

    if [[ "$CHECK_ONLY" == true ]]; then
        report_env
        ok "环境检查完成。"
        exit 0
    fi

    report_env
    confirm_action

    setup_vfio
    setup_hugepages
    bind_nic
    save_state
    verify_eal
    report_results
}

main "$@"

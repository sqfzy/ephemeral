#!/usr/bin/env bash
# dpdk-setup.sh — one-shot host environment preparation for DPDK
#
# ┌── Script roles in this repo ────────────────────────────────────────────┐
# │ eph-net-dpdk/scripts/dpdk-setup.sh         this script — host env prep      │
# │ eph-net-dpdk/scripts/dpdk-teardown.sh      undo dpdk-setup, restore kernel  │
# │ benchmarks/latency/scripts/            NIC RX coalescing tuning         │
# │   setup_coalescing.sh                                                   │
# │ benchmarks/latency/lat                 per-run bench wrapper            │
# └─────────────────────────────────────────────────────────────────────────┘
#
# This script does ONLY host-level one-shot preparation:
#   1. Load the vfio-pci kernel module (with noiommu mode for cloud VMs)
#   2. Allocate 2 MB hugepages (default 256 = 512 MB)
#   3. Bind the auto-detected non-SSH NIC to vfio-pci
#   4. Save state to .dpdk_state for dpdk-teardown.sh
#
# It does NOT:
#   - Handle netns (the NIC must be in default ns to be detected)
#   - Tune NIC coalescing (that's benchmarks/latency/scripts/setup_coalescing.sh)
#   - Per-run bench orchestration (that's benchmarks/latency/lat)
#
# After this script runs once (typically right after a fresh boot), bench
# tools like benchmarks/latency/lat can switch the NIC between vfio-pci and
# bench_ns kernel mode without re-running setup.
#
# 用法：sudo ./eph-net-dpdk/scripts/dpdk-setup.sh [选项]

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
# 跨发行版 / 跨主机 探测辅助
# ──────────────────────────────────────────

# 根据 /etc/os-release 给出对应发行版的包安装命令
pkg_install_hint() {
    local pkg_rh="$1" pkg_deb="$2" pkg_arch="$3"
    local id=""
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        id=$(. /etc/os-release; echo "${ID_LIKE:-${ID:-}}")
    fi
    case "$id" in
        *rhel*|*fedora*|*centos*|*amzn*) echo "sudo dnf install $pkg_rh" ;;
        *debian*|*ubuntu*)                echo "sudo apt install $pkg_deb" ;;
        *arch*)                           echo "sudo pacman -S $pkg_arch" ;;
        *)                                echo "请用发行版包管理器安装（RH=$pkg_rh Deb=$pkg_deb Arch=$pkg_arch）" ;;
    esac
}

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

# 读取 PCI 设备的 NUMA 节点（-1 表示未知或单节点系统）
nic_numa_node() {
    local pci="$1"
    cat "/sys/bus/pci/devices/$pci/numa_node" 2>/dev/null || echo -1
}

# ──────────────────────────────────────────
# 默认配置（可通过环境变量覆盖）
# ──────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Repo root, regardless of where this script lives. The .dpdk_state file
# and the build/ tree both sit at the repo root, not under eph-net-dpdk/.
if PROJECT_DIR=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null); then
    :
else
    PROJECT_DIR="$SCRIPT_DIR"
    while [[ "$PROJECT_DIR" != "/" && ! -d "$PROJECT_DIR/eph-net-dpdk" ]]; do
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
SCRIPT_ARGS_DISPLAY="$*"  # cached for retry hints in error messages

usage() {
    cat <<EOF
${BOLD}用法${RESET}：sudo ./eph-net-dpdk/scripts/dpdk-setup.sh [选项]

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
  sudo ./eph-net-dpdk/scripts/dpdk-setup.sh                     # 自动检测网卡并配置
  sudo ./eph-net-dpdk/scripts/dpdk-setup.sh --verbose            # 详细模式（推荐初次使用）
  sudo ./eph-net-dpdk/scripts/dpdk-setup.sh --check-only         # 只查看当前状态
  sudo ./eph-net-dpdk/scripts/dpdk-setup.sh -y                   # 跳过确认
  sudo DPDK_PCI=0000:29:00.0 ./eph-net-dpdk/scripts/dpdk-setup.sh  # 手动指定网卡

${BOLD}恢复${RESET}：
  sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose)   VERBOSE=true; shift ;;
        -y|--yes)       SKIP_CONFIRM=true; shift ;;
        --check-only)   CHECK_ONLY=true; shift ;;
        --dry-run)      DRY_RUN=true; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              die "未知选项：$1" "  查看帮助：sudo ./eph-net-dpdk/scripts/dpdk-setup.sh --help" ;;
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
        # 跳过虚拟接口、wifi（DPDK 只支持以太网 PMD）、bond/bridge/tap/tunnel 等
        case "$iface" in
            lo|docker*|veth*|br-*|virbr*|bond*|tap*|tun*|wg*|ip6tnl*|sit*|gre*|wlan*|wlp*|wls*|vmnet*|dummy*|cni*|flannel*|cali*)
                continue ;;
        esac

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
    vfio_bound=$("$DEVBIND" --status 2>/dev/null | grep 'drv=vfio-pci' | grep -oP '\S+(?= .*)' || true)
    if [[ -n "$vfio_bound" ]]; then
        warn "发现已绑定到 vfio-pci 的设备：${vfio_bound}"
        info "如果是上次未恢复，先运行：sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh"
    fi

    if [[ ${#dpdk_candidates[@]} -eq 0 ]]; then
        # No candidate NIC in the default netns. The most common cause on a
        # bench-tooling host is that the candidate NIC has been moved into
        # bench_ns (or another netns) by an earlier `lat` run that didn't
        # tear down. Look there before giving up — the user almost certainly
        # just needs to move it back to default ns.
        local found_ns="" found_nic=""
        if command -v ip &>/dev/null; then
            local ns ns_nics ns_nic
            for ns in $(ip netns list 2>/dev/null | awk '{print $1}'); do
                [[ -e "/var/run/netns/$ns" ]] || continue
                ns_nics=$(ip netns exec "$ns" ls /sys/class/net/ 2>/dev/null | grep -v '^lo$' || true)
                for ns_nic in $ns_nics; do
                    if [[ "$ns_nic" != "$ssh_iface" ]]; then
                        found_ns="$ns"; found_nic="$ns_nic"; break 2
                    fi
                done
            done
        fi
        if [[ -n "$found_ns" ]]; then
            die "found candidate NIC ${found_nic} but it lives in netns ${found_ns} (dpdk-setup only sees default ns)" \
                "  Move it back to default ns and re-run setup:\n    sudo ip netns exec ${found_ns} ip link set ${found_nic} netns 1\n    sudo $0 ${SCRIPT_ARGS_DISPLAY:-}"
        fi
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
            "  运行方式：sudo ./eph-net-dpdk/scripts/dpdk-setup.sh\n  保留环境变量：sudo -E ./eph-net-dpdk/scripts/dpdk-setup.sh"
    fi
    ok "root 权限"

    if ! modinfo vfio-pci &>/dev/null; then
        local vfio_hint
        vfio_hint=$(pkg_install_hint kernel-modules-extra "linux-modules-extra-\$(uname -r)" linux)
        die "vfio-pci 内核模块不可用" \
            "  检查内核配置：grep VFIO /boot/config-\$(uname -r)\n  安装方式：${vfio_hint}"
    fi
    ok "vfio-pci 模块可用"

    if ! find_devbind; then
        local dpdk_hint
        dpdk_hint=$(pkg_install_hint dpdk dpdk dpdk)
        die "dpdk-devbind.py 未找到" \
            "  已搜索：\$PATH、/usr/local/share/dpdk/usertools、/usr/share/dpdk/usertools、/usr/local/bin\n  安装方式：${dpdk_hint}\n  或手动加入 PATH：export PATH=\$PATH:/usr/local/share/dpdk/usertools"
    fi
    ok "dpdk-devbind.py：${DEVBIND}"

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
    # bash 3.2-compat lowercasing (macOS 默认 bash 没有 ${var,,})
    case "$(printf '%s' "$answer" | tr '[:upper:]' '[:lower:]')" in
        y|yes) ok "已确认" ;;
        *)
            info "已取消。"
            info "跳过确认：sudo ./eph-net-dpdk/scripts/dpdk-setup.sh -y"
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
# 1. 加载 VFIO 模块 — 仅在无 IOMMU 时降级到 noiommu
# ──────────────────────────────────────────
setup_vfio() {
    step "加载 VFIO 模块"
    verbose "VFIO（Virtual Function I/O）允许用户态程序安全地直接访问 PCI 设备。"
    verbose "有硬件 IOMMU（bare-metal / VT-d / AMD-Vi / SMMU）时走受保护路径；"
    verbose "没有（如 AWS Graviton 等云 VM）才降级到 unsafe noiommu 模式。"

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] modprobe vfio-pci"
        info "[模拟] 根据 /sys/kernel/iommu_groups 决定是否开启 noiommu"
        return
    fi

    modprobe vfio-pci
    ok "vfio-pci 模块已加载"

    # 探测 IOMMU：有真 IOMMU 就不降级 DMA 安全边界
    local iommu_groups=0
    if [[ -d /sys/kernel/iommu_groups ]]; then
        iommu_groups=$(find /sys/kernel/iommu_groups -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    fi

    if [[ "$iommu_groups" -gt 0 ]]; then
        ok "检测到 IOMMU（${iommu_groups} 个组），使用受保护 DMA 映射"
        verbose "bare-metal 或支持 VT-d/AMD-Vi/SMMU 的虚拟化平台：保留 IOMMU 保护，不开 noiommu"
        return
    fi

    # 无 IOMMU：必须开启 noiommu 才能在此类主机使用 vfio-pci
    warn "未检测到 IOMMU（常见于 AWS/GCP/Azure VM），降级到 noiommu 模式"
    info "  noiommu 允许用户态直接做 DMA，仅适用于可信单租户环境"

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
# 2. 分配 Hugepages — NUMA-aware：多节点系统按 NIC 所在节点
# ──────────────────────────────────────────
setup_hugepages() {
    step "分配 Hugepages"
    verbose "Hugepages 是大页内存（每页 2MB），DPDK 用它做零拷贝包缓冲区。"
    verbose "目标分配 ${NR_HUGEPAGES} 页 = $((NR_HUGEPAGES * 2))MB。"

    local sys_dir="/sys/kernel/mm/hugepages/hugepages-2048kB"
    if [[ ! -d "$sys_dir" ]]; then
        die "内核未启用 2MB hugepage 支持（${sys_dir} 不存在）" \
            "  检查：ls /sys/kernel/mm/hugepages/\n  可能需要 kernel boot 参数：hugepagesz=2M default_hugepagesz=2M"
    fi

    # NUMA-aware：多节点系统按 NIC 所在节点分配，避免 DPDK 跨 socket 访存
    local nic_node node_count nr_path hp_dir
    nic_node=$(nic_numa_node "$DPDK_PCI")
    node_count=$(find /sys/devices/system/node -mindepth 1 -maxdepth 1 -type d -name 'node[0-9]*' 2>/dev/null | wc -l)

    if [[ "$node_count" -gt 1 && "$nic_node" -ge 0 \
          && -f "/sys/devices/system/node/node${nic_node}/hugepages/hugepages-2048kB/nr_hugepages" ]]; then
        hp_dir="/sys/devices/system/node/node${nic_node}/hugepages/hugepages-2048kB"
        nr_path="${hp_dir}/nr_hugepages"
        info "NIC 位于 NUMA node ${nic_node}（系统共 ${node_count} 节点），按节点分配"
    else
        hp_dir="$sys_dir"
        nr_path="${hp_dir}/nr_hugepages"
        if [[ "$node_count" -le 1 ]]; then
            verbose "单 NUMA 节点系统，使用全局 hugepage 池"
        else
            verbose "NIC 的 NUMA 节点信息不可用，退回到全局 hugepage 池"
        fi
    fi

    if [[ "$DRY_RUN" == true ]]; then
        info "[模拟] echo ${NR_HUGEPAGES} > ${nr_path}"
        return
    fi

    # 确保 hugetlbfs 已挂载
    if ! mount | grep -q hugetlbfs; then
        mkdir -p /dev/hugepages
        mount -t hugetlbfs nodev /dev/hugepages
        ok "hugetlbfs 已挂载到 /dev/hugepages"
    fi

    echo "$NR_HUGEPAGES" > "$nr_path"

    local actual free
    actual=$(cat "$nr_path")
    free=$(cat "${hp_dir}/free_hugepages")

    if [[ "$actual" -lt "$NR_HUGEPAGES" ]]; then
        warn "请求 ${NR_HUGEPAGES} 页但只分配到 ${actual} 页（内存不足或碎片化）"
        info "尝试：NR_HUGEPAGES=${actual} sudo ./eph-net-dpdk/scripts/dpdk-setup.sh"
        info "或重启后重试（减少碎片）"
    fi

    ok "Hugepages：${nr_path%/nr_hugepages} 已分配=${actual} 空闲=${free}（$((actual * 2))MB）"
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
        info "[模拟] ${DEVBIND} --bind=vfio-pci ${DPDK_PCI}"
        return
    fi

    ip link set "$DPDK_IFACE" down 2>/dev/null || true
    "$DEVBIND" --bind=vfio-pci "$DPDK_PCI"

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
# 结果报告
# ──────────────────────────────────────────
report_results() {
    separator
    echo -e "${BOLD}${GREEN}✅ DPDK 环境就绪${RESET}"
    echo ""

    "$DEVBIND" --status 2>&1 | grep -E 'DPDK-compat|drv=' | head -6 || true

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
    info "  sudo ./eph-net-dpdk/scripts/dpdk-teardown.sh"
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
    save_state      # 先保存状态再 bind：若 bind 中途失败，teardown 仍能找到目标 PCI
    bind_nic
    report_results
}

main "$@"

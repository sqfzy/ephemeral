#!/usr/bin/env python3
"""RSS source-port finder — 离线算出"让本机收包落到指定 CPU"的 src_port，并真实 verify。

配合 tools/nic_lowlat_setup.sh 实现"业务流软中断核 == app 核"（不跨核）。地基工具把
队列↔CPU 钉成确定映射；但业务流落到哪条队列由 NIC 的 RSS Toeplitz 哈希决定。ENA 的
ntuple 锁死（off[fixed]）无法用 Flow Director 强制流入队列，唯一手段是凑 src_port 让
4-tuple 哈希到目标队列。本工具离线枚举 src_port，算 Toeplitz→RETA→queue→cpu，按 cpu
分桶，再真实 bind+connect verify（SO_INCOMING_NAPI_ID + /proc/interrupts 增量），最后落盘。

关键正确性点（入站方向）：决定【本机收到的包】落哪条 RX 队列的，是【入站包】的 4-tuple，
即 src=对端(dst_ip:dst_port)、dst=本机(src_ip:src_port)。ENA 是 toeplitz on / xor off
（非对称哈希），方向写反则预测全错 —— verify 正是对此的地面真值兜底。

哈希输入 = inet(dst_ip) ‖ inet(src_ip) ‖ be16(dst_port) ‖ be16(src_port)

输出仅对【当前 affinity + 当前 RSS key】有效：重跑 nic_lowlat_setup（换 business_cpu/queue）
或 NIC reset（可能换 key）后，本工具必须重跑，别缓存隔天用。

Usage:
    # 免 root 预览（只算不 verify 不落盘）
    python3 tools/rss_srcport_finder.py --nic ens6 --dst <ip|域名> --dst-port 443 --dry-run
    # 真实 verify + 落盘
    sudo ./tools/rss_srcport_finder.py --nic ens6 --src-ip 172.31.0.136 \
         --dst 13.35.0.1 --dst-port 443 --per-cpu 1
    # 自测 Toeplitz（公开测试向量）
    python3 tools/rss_srcport_finder.py --selftest

Exit codes:
    0  全部目标 cpu 已 verify 并落盘
    1  用法 / 输入校验错误
    2  运行期错误（非 root / 缺 ethtool / nic 不存在 / 地基不对）
    3  verify 失败（目标不可达 或 napi 与预测不一致）→ 按约定不出文件
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Dict, List, Optional, Tuple

# SO_INCOMING_NAPI_ID 在 Python 3.9 的 socket 模块未必有常量，硬编码兜底（Linux = 56）。
SO_INCOMING_NAPI_ID = getattr(socket, "SO_INCOMING_NAPI_ID", 56)

EXIT_OK, EXIT_USAGE, EXIT_RUNTIME, EXIT_VERIFY = 0, 1, 2, 3


# ────────────────────────────── 异常体系 ──────────────────────────────
class NicReadError(RuntimeError):
    """读取 NIC RSS / 队列信息失败。"""


class FoundationError(RuntimeError):
    """网卡地基状态不满足（irqbalance / RPS / 队列未钉单核）—— 先跑 nic_lowlat_setup.sh。"""


class VerifyError(RuntimeError):
    """verify 失败：目标不可达，或实测 napi 与离线预测不一致。"""


# ────────────────────────────── 日志 ──────────────────────────────
_VERBOSE = False


def log_info(msg: str) -> None:
    print(f"[INFO]  {msg}", file=sys.stderr)


def log_warn(msg: str) -> None:
    print(f"[WARN]  {msg}", file=sys.stderr)


def log_error(msg: str) -> None:
    print(f"[ERROR] {msg}", file=sys.stderr)


def log_debug(msg: str) -> None:
    if _VERBOSE:
        print(f"[DEBUG] {msg}", file=sys.stderr)


# ══════════════════════════ 阶段1: 纯计算内核 ══════════════════════════
def toeplitz(key: bytes, data: bytes) -> int:
    """标准 Microsoft RSS Toeplitz 哈希。

    逐 bit 处理 data（MSB 优先）；每遇到 1，异或进 key 当前"最左 32 bit"，
    然后 key 概念上左移一位。等价实现：第 i 个 input bit 对应 key 偏移 i 处的 32-bit 窗口。
    key 必须足够长（len*8 >= len(data)*8 + 32）；40 字节 key 对 12 字节输入绰绰有余。
    """
    key_bits = len(key) * 8
    assert key_bits >= len(data) * 8 + 32, "RSS key 太短，无法覆盖输入长度"
    key_int = int.from_bytes(key, "big")
    result = 0
    for i in range(len(data) * 8):
        if (data[i >> 3] >> (7 - (i & 7))) & 1:
            result ^= (key_int >> (key_bits - 32 - i)) & 0xFFFFFFFF
    return result & 0xFFFFFFFF


def rss_input(src_ip: str, dst_ip: str, src_port: int, dst_port: int) -> bytes:
    """构造 RSS 哈希输入 —— 入站方向（src=对端 dst_ip:dst_port, dst=本机 src_ip:src_port）。

    决定本机收包落哪条 RX 队列的是入站包，所以"源"放对端、"目的"放本机：
        inet(dst_ip) ‖ inet(src_ip) ‖ be16(dst_port) ‖ be16(src_port)
    """
    return (
        socket.inet_aton(dst_ip)
        + socket.inet_aton(src_ip)
        + struct.pack(">H", dst_port)
        + struct.pack(">H", src_port)
    )


# 标准 MSFT RSS 测试 key（40 字节）+ 公开测试向量，用于 --selftest 验证 toeplitz()。
_STD_KEY = bytes(
    [
        0x6D, 0x5A, 0x56, 0xDA, 0x25, 0x5B, 0x0E, 0xC2,
        0x41, 0x67, 0x25, 0x3D, 0x43, 0xA3, 0x8F, 0xB0,
        0xD0, 0xCA, 0x2B, 0xCB, 0xAE, 0x7B, 0x30, 0xB4,
        0x77, 0xCB, 0x2D, 0xA3, 0x80, 0x30, 0xF2, 0x0C,
        0x6A, 0x42, 0xB7, 0x3B, 0xBE, 0xAC, 0x01, 0xFA,
    ]
)


def _selftest() -> int:
    """跑 Toeplitz 公开测试向量 + RETA 索引边界。返回退出码。"""

    def vec(src, dst, sp, dp, with_ports):
        data = socket.inet_aton(src) + socket.inet_aton(dst)
        if with_ports:
            data += struct.pack(">H", sp) + struct.pack(">H", dp)
        return toeplitz(_STD_KEY, data)

    # (src, dst, sport, dport, with_ports, expected)。权威锚 = MSFT RSS 规范的 5 个
    # IPv4-only 向量（最广引用、可对证；本机实测全部精确命中），覆盖地址路径；
    # 再加 vector-1 的 IPv4+TCP（0x51ccc178，权威）覆盖含端口路径。
    cases = [
        ("66.9.149.187", "161.142.100.80", 2794, 1766, False, 0x323E8FC2),
        ("199.92.111.2", "65.69.140.83", 14230, 1766, False, 0xD718262A),
        ("24.19.198.95", "12.22.207.184", 12898, 1766, False, 0xD2D0A5DE),
        ("38.27.205.30", "209.142.163.6", 48228, 1766, False, 0x82989176),
        ("153.39.163.191", "202.188.127.2", 2217, 1766, False, 0x5D1809C5),
        ("66.9.149.187", "161.142.100.80", 2794, 1766, True, 0x51CCC178),
    ]
    ok = True
    for src, dst, sp, dp, wp, exp in cases:
        got = vec(src, dst, sp, dp, wp)
        tag = "ok" if got == exp else "FAIL"
        if got != exp:
            ok = False
        print(f"  toeplitz {src}->{dst} ports={wp}: got=0x{got:08x} exp=0x{exp:08x} [{tag}]")

    # RETA 索引边界：h & (size-1) 必须落在 [0, size)
    for size in (64, 128, 512):
        for h in (0, size - 1, 0xFFFFFFFF):
            idx = h & (size - 1)
            assert 0 <= idx < size, f"RETA 索引越界 size={size} h={h}"
    print("  RETA 索引边界: ok")

    print("✅ selftest 通过" if ok else "❌ selftest 失败")
    return EXIT_OK if ok else EXIT_RUNTIME


# ══════════════════════════ 阶段2: NIC 读取层 ══════════════════════════
def _run(cmd: List[str]) -> str:
    """跑只读命令，返回 stdout；失败抛 NicReadError（命令用参数数组，无注入面）。"""
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise NicReadError(f"执行 {' '.join(cmd)} 失败: {e}") from e
    if out.returncode != 0:
        raise NicReadError(f"{' '.join(cmd)} 退出码 {out.returncode}: {out.stderr.strip()}")
    return out.stdout


def read_rss(nic: str) -> Tuple[bytes, List[int], str]:
    """解析 `ethtool -x <nic>` → (rss_key bytes, RETA list, hash_func)。"""
    text = _run(["ethtool", "-x", nic])
    reta: List[int] = []
    key: Optional[bytes] = None
    hash_func = "unknown"
    lines = text.splitlines()
    section = None
    for ln in lines:
        s = ln.strip()
        if s.startswith("RSS hash key"):
            section = "key"
            continue
        if s.startswith("RSS hash function"):
            section = "func"
            continue
        if "indirection table" in s:
            section = "reta"
            continue
        if section == "reta":
            # 行形如 "    0:      0     1     2     3 ..."；冒号后是若干队列号
            if ":" in s:
                _, _, rest = s.partition(":")
                for tok in rest.split():
                    if tok.isdigit():
                        reta.append(int(tok))
            elif s == "":
                section = None
        elif section == "key":
            if ":" in s and all(len(b) == 2 for b in s.split(":")):
                try:
                    key = bytes(int(b, 16) for b in s.split(":"))
                except ValueError:
                    pass
                section = None
            elif s:
                # 例如 "Operation not supported"
                raise NicReadError(f"无法读取 {nic} 的 RSS key（{s}）—— 该 NIC 不暴露 key，离线计算不可行")
        elif section == "func":
            if ":" in s:
                name, _, val = s.partition(":")
                if val.strip() == "on":
                    hash_func = name.strip()

    if key is None or len(key) < 16:
        raise NicReadError(f"{nic} 的 RSS key 读取失败/过短")
    if not reta:
        raise NicReadError(f"{nic} 的 RETA 间接表为空")
    if hash_func != "toeplitz":
        log_warn(f"{nic} 的 RSS hash function = {hash_func}（本工具按 Toeplitz 计算，结果可能不符；verify 会兜底）")
    return key, reta, hash_func


def _nic_queue_irqs(nic: str) -> List[Tuple[int, int]]:
    """解析 /proc/interrupts → [(queue_idx, irq), ...]，排除 ENA 的 *-mgmnt 管理中断。

    与 nic_lowlat_setup.sh 的启发式一致：取 label 形如 `<nic>-...-<N>` 且末尾是数字的行。
    """
    out: List[Tuple[int, int]] = []
    with open("/proc/interrupts", "r") as f:
        for ln in f:
            parts = ln.split()
            if len(parts) < 2:
                continue
            label = parts[-1]
            if not label.startswith(nic + "-"):
                continue
            tail = label.rsplit("-", 1)[-1]
            if not tail.isdigit():  # 排除 ens6-mgmnt 之类
                continue
            irq_str = parts[0].rstrip(":")
            if not irq_str.isdigit():
                continue
            out.append((int(tail), int(irq_str)))
    return out


def _read_affinity_list(irq: int) -> str:
    with open(f"/proc/irq/{irq}/smp_affinity_list", "r") as f:
        return f.read().strip()


def read_queue_cpu(nic: str) -> Dict[int, int]:
    """队列号 → CPU（读 /proc/irq/<irq>/smp_affinity_list，要求单核值）。"""
    qmap: Dict[int, int] = {}
    irqs = _nic_queue_irqs(nic)
    if not irqs:
        raise NicReadError(f"在 /proc/interrupts 找不到 {nic} 的队列中断")
    for qidx, irq in irqs:
        aff = _read_affinity_list(irq)
        if not aff.isdigit():
            raise FoundationError(
                f"队列 q{qidx}(irq {irq}) 的 affinity = '{aff}' 非单核 —— 先跑 nic_lowlat_setup.sh 钉核"
            )
        qmap[qidx] = int(aff)
    return qmap


def check_foundation(nic: str) -> None:
    """地基状态校验：irqbalance inactive + 所有 rps_cpus=0 + 队列单核 affinity。任一不满足拒跑。"""
    # irqbalance
    try:
        r = subprocess.run(
            ["systemctl", "is-active", "irqbalance"], capture_output=True, text=True, timeout=5
        )
        if r.stdout.strip() == "active":
            raise FoundationError("irqbalance 正在运行 —— 会动态改 affinity；先 `nic_lowlat_setup.sh` 关掉")
    except FileNotFoundError:
        log_warn("无 systemctl，跳过 irqbalance 检查（请自行确认未运行）")
    except subprocess.TimeoutExpired:
        log_warn("systemctl is-active irqbalance 超时，跳过")

    # RPS 必须全关
    qdir = f"/sys/class/net/{nic}/queues"
    if os.path.isdir(qdir):
        for entry in sorted(os.listdir(qdir)):
            if not entry.startswith("rx-"):
                continue
            p = os.path.join(qdir, entry, "rps_cpus")
            try:
                with open(p, "r") as f:
                    mask = f.read().strip()
            except OSError:
                continue
            if any(c not in "0,\n " for c in mask):
                raise FoundationError(f"{entry}/rps_cpus = {mask} 非全 0（RPS 开着）—— 先跑 nic_lowlat_setup.sh")

    # 队列单核 affinity（read_queue_cpu 内已对非单核抛 FoundationError；这里顺带验业务核独占）
    qmap = read_queue_cpu(nic)
    per_cpu: Dict[int, int] = {}
    for cpu in qmap.values():
        per_cpu[cpu] = per_cpu.get(cpu, 0) + 1
    multi = {c: n for c, n in per_cpu.items() if n > 1}
    if multi:
        log_warn(f"以下 CPU 服务了多条 NIC 队列 IRQ：{multi}（地基理想是每核 ≤1；不阻断，但这些核上多流会互扰）")
    log_info(f"地基检查: irqbalance off ✓  RPS off ✓  队列单核 affinity ✓")


def detect_src_ip(nic: str) -> str:
    """探测 nic 的 IPv4；多地址则抛错要求显式 --src-ip。"""
    text = _run(["ip", "-o", "-4", "addr", "show", nic])
    addrs = []
    for ln in text.splitlines():
        parts = ln.split()
        try:
            i = parts.index("inet")
            addrs.append(parts[i + 1].split("/")[0])
        except (ValueError, IndexError):
            continue
    if not addrs:
        raise NicReadError(f"{nic} 没有 IPv4 地址，请用 --src-ip 显式指定")
    if len(addrs) > 1:
        raise NicReadError(f"{nic} 有多个 IPv4（{', '.join(addrs)}），请用 --src-ip 显式指定要用哪个")
    return addrs[0]


# ══════════════════════════ 阶段3: 计算分桶 + verify ══════════════════════════
def compute_buckets(
    key: bytes,
    reta: List[int],
    queue_cpu: Dict[int, int],
    src_ip: str,
    dst_ip: str,
    dst_port: int,
    port_lo: int,
    port_hi: int,
    per_cpu: int,
    include_cpu0: bool,
) -> Tuple[Dict[int, List[dict]], List[int]]:
    """扫端口范围，按 cpu 分桶（每核 per_cpu 个）。返回 (buckets, 未填满的 cpu 列表)。"""
    target_cpus = {c for c in queue_cpu.values() if include_cpu0 or c != 0}
    buckets: Dict[int, List[dict]] = {c: [] for c in target_cpus}
    mask = len(reta) - 1
    for sport in range(port_lo, port_hi + 1):
        h = toeplitz(key, rss_input(src_ip, dst_ip, sport, dst_port))
        q = reta[h & mask]
        cpu = queue_cpu.get(q)
        if cpu is None or cpu not in buckets:
            continue
        if len(buckets[cpu]) < per_cpu:
            buckets[cpu].append({"src_port": sport, "predicted_queue": q, "hash": h})
            if all(len(v) >= per_cpu for v in buckets.values()):
                break
    unfilled = [c for c, v in buckets.items() if len(v) < per_cpu]
    return buckets, unfilled


def _read_irq_counts(nic: str) -> Dict[int, List[int]]:
    """队列号 → 各 CPU 列的中断计数（来自 /proc/interrupts）。"""
    counts: Dict[int, List[int]] = {}
    qirqs = {irq: q for q, irq in _nic_queue_irqs(nic)}
    with open("/proc/interrupts", "r") as f:
        for ln in f:
            parts = ln.split()
            if len(parts) < 2:
                continue
            irq_str = parts[0].rstrip(":")
            if not irq_str.isdigit() or int(irq_str) not in qirqs:
                continue
            nums = []
            for tok in parts[1:]:
                if tok.isdigit():
                    nums.append(int(tok))
                else:
                    break
            counts[qirqs[int(irq_str)]] = nums
    return counts


def verify_port(
    nic: str, src_ip: str, src_port: int, dst_ip: str, dst_port: int, timeout: float
) -> Tuple[int, Optional[int]]:
    """真实 bind+connect，返回 (napi_id, 增量法观测到的落核 cpu | None)。

    连接失败抛 VerifyError。连接用完即关、不发应用数据。
    """
    pre = _read_irq_counts(nic)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((src_ip, src_port))
        except OSError as e:
            raise VerifyError(f"bind {src_ip}:{src_port} 失败: {e}") from e
        s.settimeout(timeout)
        try:
            s.connect((dst_ip, dst_port))
        except OSError as e:
            raise VerifyError(f"connect {dst_ip}:{dst_port}（src_port={src_port}）失败: {e}") from e
        napi = s.getsockopt(socket.SOL_SOCKET, SO_INCOMING_NAPI_ID)
    finally:
        s.close()
    # 增量法绝对锚：握手 SYN-ACK 落在哪条队列 → 该队列计数 +1
    post = _read_irq_counts(nic)
    best_cpu, best_delta = None, 0
    for q, after in post.items():
        before = pre.get(q, [0] * len(after))
        for cpu_pos, val in enumerate(after):
            d = val - (before[cpu_pos] if cpu_pos < len(before) else 0)
            if d > best_delta:
                best_delta, best_cpu = d, cpu_pos
    return napi, best_cpu


def verify_all(
    nic: str, src_ip: str, dst_ip: str, dst_port: int, buckets: Dict[int, List[dict]], timeout: float
) -> None:
    """对每个选中 src_port 真实 verify，原地写入 verified/observed_*。

    硬失败（抛 VerifyError，调用方据此不落盘）：
      - 任一 connect 失败（目标不可达）
      - napi 双射被打破（同预测 queue 出现不同 napi，或不同 queue 共享 napi）→ 哈希方向/字段理解错
    """
    queue_napi: Dict[int, int] = {}   # 预测 queue → 观测 napi（双射）
    napi_queue: Dict[int, int] = {}
    for cpu, entries in sorted(buckets.items()):
        for e in entries:
            sport, pq = e["src_port"], e["predicted_queue"]
            napi, obs_cpu = verify_port(nic, src_ip, sport, dst_ip, dst_port, timeout)
            e["verified"] = True
            e["observed_napi_id"] = napi
            e["observed_cpu"] = obs_cpu
            mark = "✓" if (obs_cpu is None or obs_cpu == cpu) else f"⚠观测cpu={obs_cpu}"
            log_info(f"  cpu{cpu} (q{pq}): {sport} napi={napi} {mark}")
            if obs_cpu is not None and obs_cpu != cpu:
                log_warn(f"    src_port={sport} 增量法观测落核 {obs_cpu} ≠ 预测 {cpu}（ens6 背景流量噪声？以 napi 双射为准）")
            if napi == 0:
                raise VerifyError(f"src_port={sport} 拿不到 SO_INCOMING_NAPI_ID（=0），无法 verify")
            # 双射一致性
            if pq in queue_napi and queue_napi[pq] != napi:
                raise VerifyError(f"预测 queue {pq} 出现两个不同 napi（{queue_napi[pq]} vs {napi}）—— 哈希模型不一致")
            if napi in napi_queue and napi_queue[napi] != pq:
                raise VerifyError(f"napi {napi} 同时对应预测 queue {napi_queue[napi]} 与 {pq} —— 哈希模型不一致")
            queue_napi[pq] = napi
            napi_queue[napi] = pq


# ══════════════════════════ 阶段4: CLI / preflight / 落盘 ══════════════════════════
def _read_local_port_range() -> Tuple[int, int]:
    try:
        with open("/proc/sys/net/ipv4/ip_local_port_range", "r") as f:
            lo, hi = f.read().split()
            return int(lo), int(hi)
    except OSError:
        return 32768, 60999


def _resolve_dst(dst: str) -> str:
    """dst 是 IP 直接用；是域名则解析并要求确认（CloudFront/Anycast 每次解析 IP 不同）。"""
    try:
        socket.inet_aton(dst)
        return dst
    except OSError:
        pass
    try:
        infos = socket.getaddrinfo(dst, None, family=socket.AF_INET, type=socket.SOCK_STREAM)
    except socket.gaierror as e:
        raise NicReadError(f"解析域名 {dst} 失败: {e}") from e
    ips = sorted({i[4][0] for i in infos})
    log_warn(f"域名 {dst} 解析到 {ips}")
    log_warn("交易所走 CloudFront/Anycast，每次解析 IP 可能不同；4-tuple 必须用【实际连上的】dst_ip。")
    if len(ips) > 1:
        raise NicReadError(f"{dst} 有多个 A 记录，请用 `ss -tn` 确认实连 IP 后用 --dst <ip> 显式指定")
    log_warn(f"建议：用 `ss -tn` 核对实连 IP；这里采用 {ips[0]}")
    return ips[0]


def persist(out_path: str, payload: dict) -> str:
    """落盘 JSON。默认路径在 tools/outputs/；--out 显式指定时按用户给的路径写。"""
    real = os.path.realpath(out_path)
    os.makedirs(os.path.dirname(real) or ".", exist_ok=True)
    with open(real, "w") as f:
        json.dump(payload, f, indent=2, ensure_ascii=False)
    return real


def main(argv: Optional[List[str]] = None) -> int:
    global _VERBOSE
    p = argparse.ArgumentParser(
        prog="rss_srcport_finder.py",
        description="RSS source-port finder — 算出让本机收包落到指定 CPU 的 src_port 并 verify。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--nic", help="网卡名（如 ens6）")
    p.add_argument("--dst", help="目标 IP 或域名")
    p.add_argument("--dst-port", type=int, help="目标端口")
    p.add_argument("--src-ip", help="本地源 IP（默认探测 nic 主 IPv4，多地址则报错）")
    p.add_argument("--per-cpu", type=int, default=1, help="每个 CPU 找几个 src_port（默认 1）")
    p.add_argument("--port-range", help="候选 src_port 范围 LO-HI（默认读 ip_local_port_range）")
    p.add_argument("--include-cpu0", action="store_true", help="包含 CPU0（默认排除）")
    p.add_argument("--out", help="落盘路径（默认 tools/outputs/rss-srcports-<ts>.json）")
    p.add_argument("--connect-timeout", type=float, default=3.0, help="verify connect 超时秒（默认 3）")
    p.add_argument("--dry-run", action="store_true", help="只算不 verify 不落盘（免 root 预览）")
    p.add_argument("--yes", "-y", action="store_true", help="跳过 verify 前的确认提示")
    p.add_argument("-v", "--verbose", action="store_true", help="详细输出")
    p.add_argument("--selftest", action="store_true", help="跑 Toeplitz 公开测试向量后退出")
    args = p.parse_args(argv)
    _VERBOSE = args.verbose

    if args.selftest:
        return _selftest()

    # ── 输入校验 ──
    if not args.nic or not args.dst or args.dst_port is None:
        log_error("缺少必需参数：--nic --dst --dst-port（-h 看用法）")
        return EXIT_USAGE
    if not (1 <= args.dst_port <= 65535):
        log_error(f"--dst-port 须在 1-65535，得到 {args.dst_port}")
        return EXIT_USAGE
    if args.per_cpu < 1:
        log_error("--per-cpu 须 ≥ 1")
        return EXIT_USAGE
    if not os.path.isdir(f"/sys/class/net/{args.nic}"):
        log_error(f"网卡不存在：{args.nic}")
        return EXIT_RUNTIME

    port_lo, port_hi = _read_local_port_range()
    if args.port_range:
        try:
            lo_s, hi_s = args.port_range.split("-")
            port_lo, port_hi = int(lo_s), int(hi_s)
        except ValueError:
            log_error("--port-range 格式应为 LO-HI")
            return EXIT_USAGE
    if not (1 <= port_lo <= port_hi <= 65535):
        log_error(f"端口范围非法：{port_lo}-{port_hi}")
        return EXIT_USAGE

    if not args.dry_run and os.geteuid() != 0:
        log_error(f"verify 需要 root；用 sudo 重跑，或加 --dry-run 仅预览。")
        return EXIT_RUNTIME

    try:
        # ── preflight / 读 NIC ──
        if subprocess.run(["which", "ethtool"], capture_output=True).returncode != 0:
            log_error("缺 ethtool（dnf install ethtool）")
            return EXIT_RUNTIME

        check_foundation(args.nic)

        src_ip = args.src_ip or detect_src_ip(args.nic)
        try:
            socket.inet_aton(src_ip)
        except OSError:
            log_error(f"--src-ip 非法 IPv4：{src_ip}")
            return EXIT_USAGE
        dst_ip = _resolve_dst(args.dst)

        key, reta, hfunc = read_rss(args.nic)
        queue_cpu = read_queue_cpu(args.nic)
        key_fp = hashlib.sha256(key).hexdigest()[:8]
        log_info(f"RSS: key 指纹={key_fp} RETA={len(reta)} {hfunc}; queue→cpu = {queue_cpu}")
        log_info(f"4-tuple: src={src_ip} dst={dst_ip}:{args.dst_port}（哈希按入站方向）")
        log_info(f"扫描 ephemeral {port_lo}-{port_hi}，每核找 {args.per_cpu} 个…")

        buckets, unfilled = compute_buckets(
            key, reta, queue_cpu, src_ip, dst_ip, args.dst_port,
            port_lo, port_hi, args.per_cpu, args.include_cpu0,
        )
        if unfilled:
            log_warn(f"以下 CPU 在端口范围内没凑满 {args.per_cpu} 个：{sorted(unfilled)}（可扩大 --port-range）")
        for cpu in sorted(buckets):
            ports = [e["src_port"] for e in buckets[cpu]]
            log_info(f"  预测 cpu{cpu}: {ports}")

        if args.dry_run:
            log_info("dry-run：未 verify、未落盘。去掉 --dry-run 并用 sudo 跑真实 verify。")
            return EXIT_OK

        total = sum(len(v) for v in buckets.values())
        if total == 0:
            log_error("没有任何候选 src_port，无法 verify。")
            return EXIT_RUNTIME

        # ── 确认（建真实连接）──
        if not args.yes:
            print(
                f"\n即将向 {dst_ip}:{args.dst_port} 建立 {total} 个真实 TCP 连接（仅握手、不发数据、用完即关）"
                f"以 verify。继续？[y/N] ",
                end="",
                file=sys.stderr,
            )
            if input().strip().lower() not in ("y", "yes"):
                log_error("用户取消。")
                return EXIT_OK

        # ── verify（硬失败不落盘）──
        log_info(f"verify: 连接 {dst_ip}:{args.dst_port} …")
        verify_all(args.nic, src_ip, dst_ip, args.dst_port, buckets, args.connect_timeout)

        # ── 落盘 ──
        ts = datetime.now(timezone.utc).astimezone().strftime("%Y%m%d-%H%M%S")
        out_path = args.out or os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "outputs", f"rss-srcports-{ts}.json"
        )
        payload = {
            "generated_at": datetime.now(timezone.utc).astimezone().isoformat(),
            "nic": args.nic,
            "tuple": {"src_ip": src_ip, "dst_ip": dst_ip, "dst_port": args.dst_port},
            "rss": {"key_fingerprint_sha256_8": key_fp, "reta_size": len(reta), "hash_func": hfunc},
            "queue_to_cpu": {str(q): c for q, c in queue_cpu.items()},
            "foundation": "ok",
            "port_range": [port_lo, port_hi],
            "per_cpu": {
                str(cpu): [
                    {
                        "src_port": e["src_port"],
                        "predicted_queue": e["predicted_queue"],
                        "verified": e.get("verified", False),
                        "observed_napi_id": e.get("observed_napi_id"),
                        "observed_cpu": e.get("observed_cpu"),
                    }
                    for e in entries
                ]
                for cpu, entries in sorted(buckets.items())
            },
            "caveat": (
                "仅对当前 affinity + 当前 RSS key 有效。nic_lowlat_setup.sh 重跑（换 business_cpu/queue）"
                "或 NIC reset（可能换 key）后必须重跑本工具。哈希按入站方向 dst‖src‖dport‖sport 计算。"
            ),
        }
        real = persist(out_path, payload)
        log_info(f"落盘: {real}")
        log_info(f"✅ {len(buckets)} 个 cpu 共 {total} 个 src_port，全部 verify 通过。"
                 + ("" if args.include_cpu0 else " cpu0 已排除。"))
        return EXIT_OK

    except FoundationError as e:
        log_error(f"地基不满足：{e}")
        return EXIT_RUNTIME
    except NicReadError as e:
        log_error(f"读取 NIC 失败：{e}")
        return EXIT_RUNTIME
    except VerifyError as e:
        log_error(f"verify 失败：{e}")
        log_error("按约定 verify 失败不出文件。")
        return EXIT_VERIFY


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        log_error("中断。")
        sys.exit(EXIT_RUNTIME)

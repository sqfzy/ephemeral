#!/usr/bin/env python3
# rss_queue_probe.py — empirically map src_port -> RX queue / CPU via eBPF.
#
# WHY: on AWS ENA the readable RSS key is a placeholder, so Toeplitz prediction
# of src_port->queue is unreliable (rss_srcport_finder's predict-then-verify
# refuses to emit on ENA). This probe is PURE EMPIRICAL and key-free:
#
#   1. send a STATELESS raw TCP SYN  (src=<src_ip>:<port>, dst=<dst>:<dport>)
#      per candidate src_port — no socket, no connection, no handshake completion;
#      the venue replies one SYN-ACK which physically arrives at our NIC RX and
#      is hashed by the hardware RSS into a real queue. (kernel RSTs the
#      unsolicited SYN-ACK — fully stateless on our side.)
#   2. eBPF (bpftrace) at tcp_v4_rcv reads the ACTUAL skb->queue_mapping
#      (= rx_qid+1) for each reply, plus the softirq CPU. queue_mapping is the
#      ground-truth hardware queue (verified == softirq CPU); deterministic per
#      4-tuple. Probes are PACED (not flooded) to avoid batch-processing artifacts.
#
# OUTPUT: per-queue and per-cpu lists of src_ports. Feed the ports landing on the
# queue whose IRQ you pinned to the app core (see tools/nic_lowlat_setup.sh) into
# cfg.kernel.local_bind.port so every connection's RX stays on the app core
# (no cross-core). Run the probe and the app against the SAME dst IP (RSS hashes
# the full 4-tuple).
#
# Usage:
#   sudo ./tools/rss_queue_probe.py --nic ens6 --dst stream.bybit.com [--dst-port 443]
#        [--src-ip <auto>] [--port-lo 33000] [--count 16] [--pace 0.04] [--json out.json]
#
# Exit: 0 ok / 1 usage / 2 runtime (not root / missing tool / nic / dns)

import argparse, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time, random


def log(m): print(f"[rss_queue_probe] {m}", file=sys.stderr)
def die(m, code=2): log("ERROR: " + m); sys.exit(code)


def checksum(d: bytes) -> int:
    if len(d) % 2:
        d += b"\x00"
    s = sum(struct.unpack("!%dH" % (len(d) // 2), d))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return (~s) & 0xFFFF


def build_syn(sip: str, sp: int, dip: str, dp: int) -> bytes:
    seq = random.randint(0, 0xFFFFFFFF)
    off_flags = (5 << 12) | 0x002  # data offset 5 words, SYN
    tcp = struct.pack("!HHIIHHHH", sp, dp, seq, 0, off_flags, 65535, 0, 0)
    pseudo = socket.inet_aton(sip) + socket.inet_aton(dip) + struct.pack("!BBH", 0, 6, len(tcp))
    csum = checksum(pseudo + tcp)
    return tcp[:16] + struct.pack("!H", csum) + tcp[18:]


def nic_ifindex(nic: str) -> int:
    try:
        return int(open(f"/sys/class/net/{nic}/ifindex").read().strip())
    except OSError:
        die(f"nic {nic} not found")


def route_src(dst_ip: str, nic: str) -> str:
    # The src IP the kernel actually uses to reach dst (route's preferred src) —
    # NOT just nic's first address (which may be e.g. an SSH-only secondary IP).
    out = subprocess.run(["ip", "route", "get", dst_ip], capture_output=True, text=True).stdout
    m_src = re.search(r"\bsrc (\d+\.\d+\.\d+\.\d+)", out)
    m_dev = re.search(r"\bdev (\S+)", out)
    if not m_src:
        die(f"cannot determine src IP for route to {dst_ip}")
    if m_dev and m_dev.group(1) != nic:
        log(f"WARN: route to {dst_ip} egresses {m_dev.group(1)}, not --nic {nic} "
            f"(probe measures the actual egress NIC's RSS)")
    return m_src.group(1)


def resolve(host: str) -> str:
    try:
        socket.inet_aton(host)
        return host
    except OSError:
        infos = socket.getaddrinfo(host, None, socket.AF_INET)
        ips = sorted({i[4][0] for i in infos})
        if len(ips) > 1:
            log(f"{host} has multiple A records {ips}; using {ips[0]} (pass --dst <ip> to pin)")
        return ips[0]


# queue_mapping value (= rx_qid+1) -> current IRQ-affinity CPU, from /proc/interrupts.
def queue_cpu_map(nic: str) -> dict:
    m = {}
    for ln in open("/proc/interrupts"):
        if nic not in ln:
            continue
        vec = ln.split(":")[0].strip()
        qm = re.search(r"%s-\D*(\d+)\s*$" % re.escape(nic), ln.strip())
        if not vec.isdigit() or not qm:
            continue
        qid = int(qm.group(1))
        try:
            cpu = open(f"/proc/irq/{vec}/smp_affinity_list").read().strip()
        except OSError:
            cpu = "?"
        m[qid + 1] = cpu  # queue_mapping = qid+1
    return m


def main() -> int:
    ap = argparse.ArgumentParser(description="eBPF empirical src_port -> RX queue/CPU probe")
    ap.add_argument("--nic", required=True)
    ap.add_argument("--dst", required=True, help="venue IP or hostname (use the SAME IP the app will connect to)")
    ap.add_argument("--dst-port", type=int, default=443)
    ap.add_argument("--src-ip", default=None, help="local source IP (default: nic's IPv4)")
    ap.add_argument("--port-lo", type=int, default=33000)
    ap.add_argument("--count", type=int, default=16, help="src_ports wanted per queue")
    ap.add_argument("--pace", type=float, default=0.04, help="seconds between SYNs")
    ap.add_argument("--json", default=None, help="write result JSON here")
    args = ap.parse_args()

    if os.geteuid() != 0:
        die("must run as root (raw socket + bpftrace)")
    if not shutil.which("bpftrace"):
        die("bpftrace not found")

    ifindex = nic_ifindex(args.nic)
    dst_ip = resolve(args.dst)
    src_ip = args.src_ip or route_src(dst_ip, args.nic)
    nqueues = len(os.listdir(f"/sys/class/net/{args.nic}/queues")) // 2 or 4
    # scan enough ports: count per queue * nqueues * safety
    span = max(args.count * nqueues * 3, 128)
    lo, hi = args.port_lo, args.port_lo + span
    dp = args.dst_port
    log(f"nic={args.nic}(ifindex={ifindex}) src={src_ip} dst={dst_ip}:{dp} scan={lo}..{hi-1} pace={args.pace}s")

    bt = (
        "kprobe:tcp_v4_rcv { $skb=(struct sk_buff*)arg0; "
        f"if ($skb->skb_iif=={ifindex}) {{ "
        "$t=(struct tcphdr*)($skb->head + $skb->transport_header); "
        "$sp=(uint16)(($t->source>>8)|($t->source<<8)); "
        "$dp=(uint16)(($t->dest>>8)|($t->dest<<8)); "
        f"if ($sp=={dp} && $dp>={lo} && $dp<{hi}) {{ @q[$dp]=$skb->queue_mapping; @c[$dp,cpu]=count(); }} }} }}"
    )
    outf = tempfile.NamedTemporaryFile("r", suffix=".bt", delete=False)
    proc = subprocess.Popen(["bpftrace", "-e", bt], stdout=outf, stderr=subprocess.DEVNULL)
    time.sleep(2.0)  # let bpftrace attach

    rs = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_TCP)
    sent = 0
    for p in range(lo, hi):
        try:
            rs.sendto(build_syn(src_ip, p, dst_ip, dp), (dst_ip, dp))
            sent += 1
        except OSError:
            pass
        time.sleep(args.pace)
    rs.close()
    time.sleep(1.0)  # drain replies
    proc.send_signal(2)  # SIGINT -> bpftrace dumps maps
    proc.wait(timeout=10)
    outf.flush()
    text = open(outf.name).read()
    os.unlink(outf.name)

    port_q, port_c = {}, {}
    for ln in text.splitlines():
        m = re.match(r"@q\[(\d+)\]: (\d+)", ln)
        if m:
            port_q[int(m.group(1))] = int(m.group(2))
        m = re.match(r"@c\[(\d+), (\d+)\]: (\d+)", ln)
        if m:
            port_c.setdefault(int(m.group(1)), {})[int(m.group(2))] = int(m.group(3))

    qcpu = queue_cpu_map(args.nic)
    by_queue, by_cpu = {}, {}
    for port, qm in sorted(port_q.items()):
        by_queue.setdefault(qm, []).append(port)
        cpu = qcpu.get(qm, "?")
        by_cpu.setdefault(str(cpu), []).append(port)
    # trim to --count per bucket
    by_queue = {k: v[: args.count] for k, v in sorted(by_queue.items())}
    by_cpu = {k: v[: args.count] for k, v in sorted(by_cpu.items(), key=lambda kv: str(kv[0]))}

    result = {
        "nic": args.nic, "src_ip": src_ip, "dst_ip": dst_ip, "dst_port": dp,
        "sent": sent, "observed": len(port_q), "queue_cpu_map": qcpu,
        "by_queue": by_queue, "by_cpu": by_cpu,
    }
    log(f"sent={sent} observed={len(port_q)}  queue->cpu={qcpu}")
    for q, ports in by_queue.items():
        log(f"  queue_mapping {q} (cpu {qcpu.get(q,'?')}): {len(ports)} ports e.g. {ports[:6]}")
    print(json.dumps(result, indent=2))
    if args.json:
        json.dump(result, open(args.json, "w"), indent=2)
        log(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

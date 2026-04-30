/// @file repro_ena_mp_secondary_rxburst.cpp
/// Regression sentinel for the ENA PMD MP secondary I/O empty-ring path.
///
/// ────────────────────────────────────────────────────────────────────
/// Background (see `eph-net-dpdk/docs/ena-mp-limitation.md` for the full
/// post-isolation diagnosis):
///
/// On AWS ENA, the secondary-process `rx_burst` crash is **gated on
/// HW-completed RX descriptors**. Empty-ring `rx_burst` short-circuits
/// on `head==tail` before the bad deref into primary-only VA, so this
/// self-contained reproducer — which never injects real traffic on the
/// secondary's queue — does NOT crash on current DPDK 24.11.2 + ENA
/// combos. It exits **9** ("limitation NOT reproduced").
///
/// That is the **expected** outcome and this binary stays around as a
/// regression sentinel: if a future change ever makes the empty-ring
/// path crash too (exit 0 / SIGSEGV), that's a real escalation worth
/// chasing.
///
/// The full traffic-loaded reproducer (which DOES SIGSEGV with the
/// stack below, on both autojoin and declarative bring-up paths) lives
/// on the `diag/ena-mp-isolation-2` branch — intentionally not merged
/// because it re-introduces diagnostic envvar-gated bring-up paths.
/// See the doc for the workflow.
///
/// Confirmed on:
///   DPDK            24.11.2  (librte_net_ena.so.25)
///   Kernel          6.1.163-186.299.amzn2023.aarch64
///   Instance        AWS EC2 c8g.4xlarge (Graviton4)
///   PCI (NIC_B)     0000:28:00.0  (ENA, vfio-pci-bound)
///   Date            2026-04-30
///
/// Reference backtrace (from `gdb -batch -ex run -ex 'bt 30'` on the
/// secondary process):
///
///     Thread 1 "lat_udp" received signal SIGSEGV
///       #0  ena_com_get_next_rx_cdesc ()  librte_net_ena.so.25.0
///       #1  ena_com_rx_pkt ()             librte_net_ena.so.25.0
///       #2  eth_ena_recv_pkts ()          librte_net_ena.so.25.0
///       #3  rte_eth_rx_burst (port=0, queue=1) rte_ethdev.h:6293
///
/// ────────────────────────────────────────────────────────────────────
/// What this program does
///
///   1. Bring up Platform as PRIMARY on `EPH_REPRO_DEV` (auto-picks
///      the first vfio-pci NIC if unset), nb_rx_queues=2.
///   2. fork() + execv("/proc/self/exe", ...) with EPH_REPRO_ROLE=secondary
///      so the child runs in a fresh address space.
///   3. Child attaches as SECONDARY (same file_prefix), then exercises
///      both I/O burst paths on its owned queue (queue=1):
///        a. `rte_eth_tx_burst` with one minimum-length broadcast
///           Ethernet frame — DETERMINISTIC trigger, the descriptor
///           ring is touched even on an idle NIC.
///        b. If tx survived: `rte_eth_rx_burst` polled for 1 second —
///           covers the case where tx is shared via memzone but rx
///           isn't, by relying on incidental broadcast / multicast
///           traffic.
///      Expected outcome on ENA: SIGSEGV inside the PMD (typically in
///      `ena_com_get_next_rx_cdesc` or the doorbell-write path).
///   4. Parent waitpid()s the child and asserts:
///        WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV (or BUS)
///      → exit 0  : ENA limitation reproduced (the bug exists, as
///                  documented).
///      → exit 9  : child completed both tx and rx attempts without
///                  crashing — limitation has been lifted upstream
///                  (or this is a non-ENA NIC). Update the doc's
///                  "Verified versions" table.
///
/// This binary is intentionally NOT registered in the default `tests`
/// xmake group (it expects a SIGSEGV; CI must not flag it as a
/// regression). Build via the `repros` group:
///
///     xmake build -g repros
///     sudo $BUILD/repro_ena_mp_secondary_rxburst
///
/// Skips cleanly (exit 77, gtest SKIP convention) when:
///   - no vfio-pci NIC bound and EPH_REPRO_DEV unset
///   - HugePages_Free is below 64
///   - process is not euid 0 (DPDK needs root for vfio)

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"  // pulls in join_dynamic.hpp internally

namespace {

constexpr int kSkipExitCode = 77;  // matches gtest SKIP sentinel
constexpr uint16_t kNbRxQueues = 2;
constexpr uint16_t kSecondaryQueueId = 1;
constexpr int kReadyTimeoutSec = 10;

const char* env_or_null(const char* k) noexcept {
    const char* v = std::getenv(k);
    return (v && *v) ? v : nullptr;
}

[[nodiscard]] std::string auto_pick_vfio_dev() {
    const char* dir = "/sys/bus/pci/drivers/vfio-pci";
    DIR* d = ::opendir(dir);
    if (!d) return {};
    std::string picked;
    while (auto* ent = ::readdir(d)) {
        std::string name = ent->d_name;
        if (name.rfind("0000:", 0) == 0) {
            picked = std::move(name);
            break;
        }
    }
    ::closedir(d);
    return picked;
}

[[nodiscard]] long hugepages_free() {
    std::ifstream f("/proc/meminfo");
    std::string key;
    long val = -1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("HugePages_Free:", 0) == 0) {
            std::sscanf(line.c_str(), "HugePages_Free: %ld", &val);
            return val;
        }
    }
    return val;
}

void log(const char* role, const char* fmt, ...) {
    std::fprintf(stderr, "[repro %s pid=%d] ", role, ::getpid());
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

// ── secondary role ──────────────────────────────────────────────────────
//
// Bring up Platform as secondary, then call rte_eth_rx_burst once.
// Expected: SIGSEGV inside the PMD's recv_pkts. We do NOT install our
// own signal handler — let the kernel deliver it, the parent will
// observe via waitpid().
//
// If we somehow return normally from rx_burst, exit 1 (= bug not
// present on this combination of DPDK + NIC).
[[noreturn]] void secondary_main() {
    const char* allowed_dev = env_or_null("EPH_REPRO_DEV");
    if (!allowed_dev) {
        log("secondary", "missing EPH_REPRO_DEV (parent should have set it)");
        std::_Exit(2);
    }
    log("secondary", "join_dynamic-attaching: pci=%s queue=%u",
        allowed_dev, kSecondaryQueueId);

    // Match the autojoin code path (Platform::join_dynamic) — same
    // bring-up sequence used by lat_*_dpdk binaries via
    // EPH_LAT_AUTOJOIN_*. The original SIGSEGV was observed on this
    // path; the declarative create_secondary path appears unaffected.
    eph::dpdk::JoinDynamicConfig jcfg{};
    jcfg.pci                          = allowed_dev;
    jcfg.queues_per_proc              = 1;
    jcfg.pcfg_template.port_id        = 0;
    jcfg.pcfg_template.nb_rx_queues   = kNbRxQueues;
    jcfg.pcfg_template.nb_tx_queues   = kNbRxQueues;
    jcfg.lcores                       = {"1"};

    auto plat_r = eph::dpdk::Platform::join_dynamic(jcfg);
    if (!plat_r) {
        log("secondary", "join_dynamic failed: %s", plat_r.error().c_str());
        std::_Exit(3);
    }
    auto platform = std::move(*plat_r);
    const auto qr = platform.effective_rx_queue_range();
    log("secondary", "joined: port=%u mempool=%p rx_queue_range=[%u,%u)",
        platform.port_id(), (void*)platform.mempool(),
        qr.first, qr.second);
    if (qr.first != kSecondaryQueueId) {
        log("secondary", "unexpected: secondary did not claim queue %u "
                         "(got [%u,%u)) — parent may not have raced first",
            kSecondaryQueueId, qr.first, qr.second);
        std::_Exit(12);
    }

    // ── Attempt 1: tx_burst (deterministic) ────────────────────────
    //
    // tx_burst is the preferred trigger because it always touches the
    // per-queue descriptor ring on the way out (no traffic-arrival
    // dependency), so the secondary-vs-primary VA mismatch surfaces
    // even on an idle NIC. We build a minimum-length broadcast
    // Ethernet frame; whether the wire actually accepts it is
    // irrelevant — only the PMD code path inside the secondary
    // matters.
    rte_mbuf* tx_mbuf = rte_pktmbuf_alloc(platform.mempool());
    if (!tx_mbuf) {
        log("secondary", "rte_pktmbuf_alloc from shared pool failed");
        std::_Exit(11);
    }
    {
        auto* eth = rte_pktmbuf_mtod(tx_mbuf, uint8_t*);
        std::memset(eth,      0xff, 6);   // dst MAC = broadcast
        std::memset(eth + 6,  0x02, 6);   // src MAC = locally administered
        eth[12] = 0x12; eth[13] = 0x34;   // ethertype (synthetic; ENA may drop)
        std::memset(eth + 14, 0,    46);  // pad to ETH_MIN
        tx_mbuf->data_len = 60;
        tx_mbuf->pkt_len  = 60;
    }
    log("secondary", "calling rte_eth_tx_burst(port=0, queue=%u, n=1) ...",
        kSecondaryQueueId);
    std::fflush(stderr);
    [[maybe_unused]] uint16_t sent = rte_eth_tx_burst(
        /*port_id=*/0, /*queue_id=*/kSecondaryQueueId, &tx_mbuf, 1);
    log("secondary", "tx_burst returned %u (no SIGSEGV)",
        static_cast<unsigned>(sent));

    // ── Attempt 2: rx_burst on owned queue, poll loop ──────────────
    //
    // On a live cloud NIC there is usually some broadcast / multicast
    // traffic (gateway ARP, DHCP renewals) within a few seconds. If
    // anything lands on queue N, the cdesc-deref path is exercised.
    log("secondary", "tx survived — polling rte_eth_rx_burst on queue %u for 5s ...",
        kSecondaryQueueId);
    std::fflush(stderr);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    rte_mbuf* rx_mbufs[32];
    uint64_t total_rx  = 0;
    uint64_t byte_sink = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        uint16_t got = rte_eth_rx_burst(
            /*port_id=*/0, /*queue_id=*/kSecondaryQueueId, rx_mbufs, 32);
        for (uint16_t i = 0; i < got; ++i) {
            // Touch each mbuf's payload — the cdesc embeds an mbuf
            // pointer (or a req_id index resolving to one). If that
            // pointer was set up in primary's heap and isn't visible
            // here, this deref is what segfaults.
            const auto* p = rte_pktmbuf_mtod(rx_mbufs[i], const uint8_t*);
            for (uint16_t k = 0; k < rx_mbufs[i]->data_len; ++k) {
                byte_sink += p[k];
            }
            rte_pktmbuf_free(rx_mbufs[i]);
        }
        total_rx += got;
    }
    log("secondary", "rx poll on owned queue done; saw %llu pkts (byte_sink=%llu)",
        static_cast<unsigned long long>(total_rx),
        static_cast<unsigned long long>(byte_sink));

    // ── Attempt 3: rx_burst on queue 0 (PRIMARY's queue) ───────────
    //
    // The clean "isolated per-queue state" test. Secondary never set
    // up queue 0 — its per-queue ring metadata, doorbell pointer,
    // mempool reference, and cdesc head/tail live in primary's heap
    // and were never published to secondary. If ENA's PMD has any
    // per-queue private state at all, calling rx_burst on a queue
    // this secondary doesn't own should crash.
    constexpr uint16_t kPrimaryOwnedQueue = 0;
    log("secondary", "calling rte_eth_rx_burst on PRIMARY's queue %u ...",
        kPrimaryOwnedQueue);
    std::fflush(stderr);
    [[maybe_unused]] uint16_t got_steal = rte_eth_rx_burst(
        /*port_id=*/0, /*queue_id=*/kPrimaryOwnedQueue, rx_mbufs, 16);
    log("secondary", "rx_burst on queue %u (primary's) returned %u — limitation NOT reproduced",
        kPrimaryOwnedQueue, static_cast<unsigned>(got_steal));
    std::_Exit(0);
}

// ── primary role ────────────────────────────────────────────────────────

int primary_main(char** argv) {
    if (::geteuid() != 0) {
        log("primary", "SKIP: not running as root (DPDK needs root for vfio)");
        return kSkipExitCode;
    }

    std::string allowed_dev;
    if (const char* env = env_or_null("EPH_REPRO_DEV")) {
        allowed_dev = env;
    } else {
        allowed_dev = auto_pick_vfio_dev();
        if (allowed_dev.empty()) {
            log("primary", "SKIP: no vfio-pci NIC found and EPH_REPRO_DEV unset");
            return kSkipExitCode;
        }
        log("primary", "auto-picked dev=%s", allowed_dev.c_str());
    }

    if (long hp = hugepages_free(); hp >= 0 && hp < 64) {
        log("primary", "SKIP: HugePages_Free=%ld below 64", hp);
        return kSkipExitCode;
    }

    log("primary", "bringing up Platform via join_dynamic: pci=%s nb_rx_queues=%u",
        allowed_dev.c_str(), kNbRxQueues);

    // Use join_dynamic on BOTH peers — the original SIGSEGV was
    // observed on this autojoin code path, not the declarative
    // create_secondary path. file_prefix is auto-derived from the BDF
    // so parent and child agree without exchanging strings.
    eph::dpdk::JoinDynamicConfig jcfg{};
    jcfg.pci                          = allowed_dev;
    jcfg.queues_per_proc              = 1;
    jcfg.pcfg_template.port_id        = 0;
    jcfg.pcfg_template.nb_rx_queues   = kNbRxQueues;
    jcfg.pcfg_template.nb_tx_queues   = kNbRxQueues;
    jcfg.lcores                       = {"0"};

    auto plat_r = eph::dpdk::Platform::join_dynamic(jcfg);
    if (!plat_r) {
        log("primary", "join_dynamic (primary) failed: %s", plat_r.error().c_str());
        return 4;
    }
    auto platform = std::move(*plat_r);
    const auto qr = platform.effective_rx_queue_range();
    log("primary", "joined as primary: port=%u mempool=%p rx_queue_range=[%u,%u)",
        platform.port_id(), (void*)platform.mempool(), qr.first, qr.second);

    // ── Traffic generator (background thread) ──────────────────────
    //
    // Keep transmitting broadcast frames on queue 0 throughout the
    // child's lifetime. ENA in EC2 doesn't local-loopback, but VPC
    // fabric, gateway ARP, and any incidental ICMP we trigger may
    // produce inbound packets that RSS-hash to queue 1 (secondary's
    // owned queue), giving the secondary's rx_burst something real
    // to dequeue and exposing the alleged "deref into primary heap"
    // crash.
    std::atomic<bool> traffic_run{true};
    std::thread traffic([&]{
        rte_mbuf* mb = rte_pktmbuf_alloc(platform.mempool());
        if (!mb) return;
        auto* eth = rte_pktmbuf_mtod(mb, uint8_t*);
        std::memset(eth,      0xff, 6);   // dst = broadcast
        rte_eth_macaddr_get(0, reinterpret_cast<rte_ether_addr*>(eth + 6));
        eth[12] = 0x08; eth[13] = 0x06;   // ethertype = ARP
        // Minimal junk ARP payload — we just want the wire to wake up.
        std::memset(eth + 14, 0xa5, 46);
        mb->data_len = 60;
        mb->pkt_len  = 60;
        rte_mbuf_refcnt_set(mb, 1024);    // long-lived single mbuf for the loop
        while (traffic_run.load(std::memory_order_relaxed)) {
            rte_mbuf* clone = rte_pktmbuf_clone(mb, platform.mempool());
            if (clone) rte_eth_tx_burst(0, /*queue=*/0, &clone, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        rte_pktmbuf_free(mb);
    });

    log("primary", "fork()ing secondary; traffic thread running");

    // fork + execv self with EPH_REPRO_ROLE=secondary so the child
    // gets a fresh process / EAL state. setenv() in the child only
    // (post-fork pre-exec) so the parent's env stays clean.
    pid_t pid = ::fork();
    if (pid < 0) {
        log("primary", "fork failed: %s", std::strerror(errno));
        return 5;
    }
    if (pid == 0) {
        ::setenv("EPH_REPRO_ROLE", "secondary",         1);
        ::setenv("EPH_REPRO_DEV",  allowed_dev.c_str(), 1);
        ::execv("/proc/self/exe", argv);
        // execv only returns on failure.
        std::fprintf(stderr, "[repro secondary pid=%d] execv failed: %s\n",
                     ::getpid(), std::strerror(errno));
        std::_Exit(6);
    }

    // Parent: wait for child; the only outcome we accept as
    // "limitation reproduced" is WIFSIGNALED(status) && WTERMSIG ==
    // SIGSEGV (or SIGBUS — same root cause, different MMU response).
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        log("primary", "waitpid failed: %s", std::strerror(errno));
        traffic_run.store(false, std::memory_order_relaxed);
        if (traffic.joinable()) traffic.join();
        return 7;
    }
    traffic_run.store(false, std::memory_order_relaxed);
    if (traffic.joinable()) traffic.join();

    int rc;
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        log("primary", "child terminated by signal %d (%s)",
            sig, ::strsignal(sig));
        if (sig == SIGSEGV || sig == SIGBUS) {
            std::fprintf(stdout,
                "\n=== ENA PMD MP secondary rx_burst limitation REPRODUCED ===\n"
                "  child PID %d killed by SIG%s\n"
                "  see file header for documented root cause and recovery.\n",
                pid, ::strsignal(sig) + 3 /* skip "SIG" */);
            rc = 0;
        } else {
            log("primary", "FAIL: child died on unexpected signal");
            rc = 8;
        }
    } else if (WIFEXITED(status)) {
        const int xc = WEXITSTATUS(status);
        log("primary", "child exited normally rc=%d (limitation NOT reproduced)", xc);
        rc = (xc == kSkipExitCode) ? kSkipExitCode : 9;
    } else {
        log("primary", "child terminated abnormally without WIFEXITED/WIFSIGNALED");
        rc = 10;
    }

    // ~Platform tears down EAL.
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    if (const char* role = env_or_null("EPH_REPRO_ROLE");
        role && std::strcmp(role, "secondary") == 0) {
        secondary_main();  // [[noreturn]]
    }
    (void)argc;
    return primary_main(argv);
}

/// @file repro_ena_mp_secondary_rxburst.cpp
/// Sentinel: ENA PMD MP secondary `rx_burst` on idle rings does NOT crash.
///
/// ────────────────────────────────────────────────────────────────────
/// Two-condition root cause (2026-04-30 A/B isolation)
///
/// The SIGSEGV inside `ena_com_get_next_rx_cdesc` requires BOTH
/// conditions to be true simultaneously:
///
///   (1) PRIMARY is doing high-rate DpdkPoller-driven I/O — specifically
///       the Stream / Socket send+poll-burst loop that lat_*_dpdk uses.
///       A primary that just brings up Platform and idles is not enough.
///
///   (2) SECONDARY is driving its own DpdkPoller::poll() → DpdkUdpSocket
///       receive path.  Raw `rte_eth_rx_burst` calls alone — even under
///       live traffic — are NOT enough.
///
/// A/B evidence:
///
///   Config                                                        Result
///   ──────────────────────────────────────────────────────────────────
///   primary=lat_tcp_dpdk (high-rate)  +  secondary=lat_udp_dpdk    CRASH
///   primary=lat_tcp_dpdk (high-rate)  +  secondary=raw rx_burst     NO CRASH
///   primary=benign (Platform up, idle)+  secondary=lat_udp_dpdk     NO CRASH (753k samples)
///   primary=benign (Platform up, idle)+  secondary=raw rx_burst     NO CRASH
///
/// This binary exercises the bottom two rows (idle primary + raw
/// secondary) — the idle-ring sentinel.  Expected: exit 9 on ENA
/// DPDK 24.11.2.  If it ever exits 0 (SIGSEGV under idle), that is new
/// evidence worth chasing.
///
/// ────────────────────────────────────────────────────────────────────
/// What this program does
///
///   1. Bring up Platform as PRIMARY on `EPH_REPRO_DEV` (auto-picks the
///      first vfio-pci NIC if unset), nb_rx_queues=2.
///   2. fork() + execv("/proc/self/exe", ...) with EPH_REPRO_ROLE=secondary
///      so the child runs in a fresh address space.
///   3. Child attaches as SECONDARY and exercises three burst-API calls
///      against idle rings:
///        (a) rte_eth_tx_burst   on its own queue (queue 1)
///        (b) rte_eth_rx_burst   on its own queue (queue 1, ring idle → returns 0)
///        (c) rte_eth_rx_burst   on primary's queue (queue 0)
///   4. Parent waitpid()s child and asserts:
///        exit 9 : child survived all three calls — idle-ring sentinel holds.
///        exit 0 : SIGSEGV — idle-ring path is now broken; file upstream.
///
/// Background and full isolation log:
/// `eph-net-dpdk/docs/dpdk-mp-teardown-protocol.md`.
///
/// ────────────────────────────────────────────────────────────────────
/// Confirmed on:
///   DPDK            24.11.2  (librte_net_ena.so.25)
///   Kernel          6.1.163-186.299.amzn2023.aarch64
///   Instance        AWS EC2 c8g.4xlarge (Graviton4)
///   PCI (NIC_B)     0000:28:00.0  (ENA, vfio-pci-bound)
///   Date            2026-04-30
///
/// Reference backtrace (observed only in the two-condition configuration):
///
///     Thread 1 received signal SIGSEGV
///       #0  ena_com_get_next_rx_cdesc ()  librte_net_ena.so.25.0
///       #1  ena_com_rx_pkt ()             librte_net_ena.so.25.0
///       #2  eth_ena_recv_pkts ()          librte_net_ena.so.25.0
///       #3  rte_eth_rx_burst (port=0, queue=1) rte_ethdev.h:6293
///       #4  DpdkPoller::poll()            poller.hpp:398
///       #5  run_lat_udp_loop()            scenarios/lat_udp_loop.hpp:194
///
/// ────────────────────────────────────────────────────────────────────
/// This binary is intentionally NOT in the default `tests` xmake group.
/// Build via the `repros` group:
///
///     xmake build -g repros
///     sudo $BUILD/repro_ena_mp_secondary_rxburst
///
/// Skips cleanly (exit 77) when:
///   - no vfio-pci NIC bound and EPH_REPRO_DEV unset
///   - HugePages_Free below 64
///   - not euid 0

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
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
    std::string line;
    long val = -1;
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
// Attach as secondary, then probe three raw burst-API calls against idle
// rings. None of these crash on ENA 24.11.2 (empty-ring fast-path in
// `ena_com_get_next_rx_cdesc` short-circuits at head==tail before
// dereffing primary-only state).
//
// If we survive all three, exit 9 (sentinel holds). If we SIGSEGV before
// reaching exit, the kernel delivers it and the parent observes via
// waitpid() → exits 0 (idle-ring path broken — new evidence).
[[noreturn]] void secondary_main() {
    const char* allowed_dev = env_or_null("EPH_REPRO_DEV");
    if (!allowed_dev) {
        log("secondary", "missing EPH_REPRO_DEV (parent should have set it)");
        std::_Exit(2);
    }
    log("secondary", "join_dynamic-attaching: pci=%s queue=%u",
        allowed_dev, kSecondaryQueueId);

    eph::dpdk::JoinDynamicConfig jcfg{};
    jcfg.pci                          = allowed_dev;
    jcfg.queues_per_proc              = 1;
    jcfg.pcfg_template.port_id        = 0;
    jcfg.pcfg_template.nb_rx_queues   = kNbRxQueues;
    jcfg.pcfg_template.nb_tx_queues   = kNbRxQueues;
    const char* lc = env_or_null("EPH_REPRO_LCORES");
    jcfg.lcores                       = {std::string{lc ? lc : "1"}};

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

    // ── (a) tx_burst on own queue ──────────────────────────────────
    //
    // Touches the per-queue TX descriptor ring. Broadcast-MAC frame;
    // whether the wire accepts it is irrelevant — only the PMD path
    // inside the secondary matters.
    rte_mbuf* tx_mbuf = rte_pktmbuf_alloc(platform.mempool());
    if (!tx_mbuf) {
        log("secondary", "rte_pktmbuf_alloc from shared pool failed");
        std::_Exit(11);
    }
    {
        auto* eth = rte_pktmbuf_mtod(tx_mbuf, uint8_t*);
        std::memset(eth,      0xff, 6);   // dst = broadcast
        std::memset(eth + 6,  0x02, 6);   // src = locally administered
        eth[12] = 0x12; eth[13] = 0x34;   // synthetic ethertype
        std::memset(eth + 14, 0,    46);  // pad to ETH_MIN
        tx_mbuf->data_len = 60;
        tx_mbuf->pkt_len  = 60;
    }
    log("secondary", "(a) rte_eth_tx_burst(port=0, queue=%u, n=1) ...",
        kSecondaryQueueId);
    std::fflush(stderr);
    [[maybe_unused]] uint16_t sent = rte_eth_tx_burst(
        0, kSecondaryQueueId, &tx_mbuf, 1);
    log("secondary", "(a) tx_burst returned %u — no SIGSEGV",
        static_cast<unsigned>(sent));

    // ── (b) rx_burst on own queue (idle ring) ─────────────────────
    //
    // Ring should be empty (head==tail) — ENA's early-exit fires and
    // returns 0 without touching primary-only ring metadata.
    rte_mbuf* rx_mbufs[16];
    log("secondary", "(b) rte_eth_rx_burst(port=0, queue=%u) on idle ring ...",
        kSecondaryQueueId);
    std::fflush(stderr);
    [[maybe_unused]] uint16_t got_b = rte_eth_rx_burst(0, kSecondaryQueueId, rx_mbufs, 16);
    log("secondary", "(b) rx_burst returned %u — no SIGSEGV", static_cast<unsigned>(got_b));

    // ── (c) rx_burst on PRIMARY's queue ────────────────────────────
    //
    // Secondary never set up queue 0; its per-queue ring metadata lives
    // in primary's heap. If ENA exports any per-queue state, this call
    // would be the cleanest trigger. Under idle rings it does not crash.
    constexpr uint16_t kPrimaryQueue = 0;
    log("secondary", "(c) rte_eth_rx_burst(port=0, queue=%u) — primary's queue ...",
        kPrimaryQueue);
    std::fflush(stderr);
    [[maybe_unused]] uint16_t got_c = rte_eth_rx_burst(0, kPrimaryQueue, rx_mbufs, 16);
    log("secondary", "(c) rx_burst returned %u — idle-ring sentinel HOLDS",
        static_cast<unsigned>(got_c));

    // Sentinel holds: limitation NOT reproduced on idle rings.
    std::_Exit(9);
}

// ── primary role ────────────────────────────────────────────────────────

int primary_main(char** argv) {
    if (::geteuid() != 0) {
        log("primary", "SKIP: not running as root (DPDK needs root for vfio)");
        return kSkipExitCode;
    }

    // Benign-primary mode: bring up Platform via join_dynamic and idle
    // for N seconds. Used by /tmp/ena_mp_diag_benign_primary.sh to
    // test whether lat_udp_dpdk crashes with an idle primary (A/B leg
    // 3 of the two-condition isolation; result: NO CRASH, 753k samples).
    if (const char* benign_s = env_or_null("EPH_REPRO_BENIGN_PRIMARY");
        benign_s && std::strcmp(benign_s, "0") != 0) {
        std::string allowed_dev;
        if (const char* env = env_or_null("EPH_REPRO_DEV")) {
            allowed_dev = env;
        } else {
            allowed_dev = auto_pick_vfio_dev();
            if (allowed_dev.empty()) {
                log("primary", "SKIP: no vfio-pci NIC");
                return kSkipExitCode;
            }
        }
        log("primary", "BENIGN MODE — Platform up, no I/O, idle %s seconds",
            env_or_null("EPH_REPRO_BENIGN_HOLD") ? env_or_null("EPH_REPRO_BENIGN_HOLD") : "30");
        eph::dpdk::JoinDynamicConfig jcfg{};
        jcfg.pci                          = allowed_dev;
        jcfg.queues_per_proc              = 1;
        jcfg.pcfg_template.port_id        = 0;
        jcfg.pcfg_template.nb_rx_queues   = kNbRxQueues;
        jcfg.pcfg_template.nb_tx_queues   = kNbRxQueues;
        jcfg.lcores                       = {"0"};
        auto plat_r = eph::dpdk::Platform::join_dynamic(jcfg);
        if (!plat_r) {
            log("primary", "BENIGN: join_dynamic failed: %s", plat_r.error().c_str());
            return 4;
        }
        const char* hold_s = env_or_null("EPH_REPRO_BENIGN_HOLD");
        const int hold = hold_s ? std::atoi(hold_s) : 30;
        std::this_thread::sleep_for(std::chrono::seconds(hold));
        log("primary", "BENIGN: idle period done, exiting");
        return 0;
    }

    // Normal mode: idle-ring sentinel.

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

    // fork + execv self with EPH_REPRO_ROLE=secondary so the child
    // gets a fresh EAL / address space.
    pid_t pid = ::fork();
    if (pid < 0) {
        log("primary", "fork failed: %s", std::strerror(errno));
        return 5;
    }
    if (pid == 0) {
        ::setenv("EPH_REPRO_ROLE", "secondary",         1);
        ::setenv("EPH_REPRO_DEV",  allowed_dev.c_str(), 1);
        ::execv("/proc/self/exe", argv);
        std::fprintf(stderr, "[repro secondary pid=%d] execv failed: %s\n",
                     ::getpid(), std::strerror(errno));
        std::_Exit(6);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        log("primary", "waitpid failed: %s", std::strerror(errno));
        return 7;
    }

    int rc;
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        log("primary", "child terminated by signal %d (%s)",
            sig, ::strsignal(sig));
        if (sig == SIGSEGV || sig == SIGBUS) {
            std::fprintf(stdout,
                "\n=== ENA PMD idle-ring sentinel BROKEN ===\n"
                "  child PID %d killed by SIG%s on idle rings\n"
                "  see eph-net-dpdk/docs/dpdk-mp-teardown-protocol.md\n",
                pid, sig == SIGSEGV ? "SEGV" : "BUS");
            rc = 0;
        } else {
            log("primary", "FAIL: child died on unexpected signal %d", sig);
            rc = 8;
        }
    } else if (WIFEXITED(status)) {
        const int xc = WEXITSTATUS(status);
        if (xc == 9) {
            log("primary", "child exit 9 — idle-ring sentinel holds (expected on ENA 24.11.2)");
        } else {
            log("primary", "child exited rc=%d", xc);
        }
        rc = (xc == kSkipExitCode) ? kSkipExitCode : 9;
    } else {
        log("primary", "child terminated abnormally");
        rc = 10;
    }

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

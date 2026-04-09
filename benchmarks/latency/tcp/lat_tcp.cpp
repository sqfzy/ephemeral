/// @file tcp/lat_tcp.cpp
/// lat_tcp / lat_tcp_dpdk — single-binary TCP latency benchmark.
///
/// Layout:
///   main()  loads bench.conf, calibrates TSC, then forks. The child runs
///           the kernel-side TCP echo mock in the host network namespace.
///           The parent becomes the bench client: kernel build enters
///           bench_ns via setns(2); DPDK build stays in the host ns and
///           drives traffic through the user-space PMD on NIC-B.
///
/// TCP wire protocol (between mock and client):
///   Per connection: client sends a 4-byte little-endian uint32 = msg_size,
///   then loops sending exactly msg_size bytes and reading exactly msg_size
///   bytes back. The mock peeks the 4-byte header on accept and uses that
///   size for the entire echo lifetime of that connection. To sweep payload
///   sizes, the client closes and reconnects per sweep step — cheap on
///   localhost / direct-attached NICs and avoids any per-RTT framing
///   overhead inside the hot loop.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "../core/netns.hpp"
#include "../core/runner.hpp"
#include "../core/sample.hpp"
#include "../core/signal.hpp"
#include "../core/socket_bind.hpp"
#include "../core/tsc_protocol.hpp"

#if defined(EPH_USE_DPDK)
#include "../core/dpdk_env.hpp"
#include "eph/dpdk/tcp.hpp"
#endif

using namespace bench;

namespace {

// ── Defaults used when bench.conf has no TCP_PAYLOADS list ──────────────
constexpr std::array<size_t, 8> kDefaultTcpPayloads{
    64, 128, 256, 512, 1024, 1460, 4096, 16384
};

// ── shared I/O helpers (kernel + DPDK builds both need byte send/recv) ──
bool send_all_fd(int fd, const void* data, size_t len) noexcept {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, static_cast<const uint8_t*>(data) + sent,
                           len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_exact_fd(int fd, void* buf, size_t len) noexcept {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, static_cast<uint8_t*>(buf) + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false; // peer closed
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// mock_fn — kernel TCP echo server (always POSIX, runs in the child after
// fork in the host ns).
// ───────────────────────────────────────────────────────────────────────────
namespace bench::tcp::mock_fn {

int run(const BenchConfig& cfg) {
    // Pin the mock thread strictly. The mock and client run on different
    // cores so neither perturbs the other's TSC samples.
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_lat_tcp", policy); !p) {
        spdlog::error("mock_lat_tcp: pin failed: {}", p.error());
        return 1;
    }

    auto listen_fd = bench::tcp_bind_listen(cfg.server_ip, cfg.server_port);
    if (!listen_fd) {
        spdlog::error("mock_lat_tcp: {}", listen_fd.error());
        return 1;
    }
    spdlog::info("mock_lat_tcp: listening on {}:{} work_ns={}",
                 cfg.server_ip, cfg.server_port, cfg.server_work_ns);

    // Largest payload from any sweep — used to size the I/O buffer.
    size_t max_msg = 16384;
    for (size_t p : cfg.tcp_payloads) max_msg = std::max(max_msg, p);
    std::vector<uint8_t> buf(max_msg);

    while (g_running.load(std::memory_order_acquire)) {
        auto cfd = bench::accept_one(*listen_fd, g_running);
        if (!cfd) { spdlog::error("mock_lat_tcp: {}", cfd.error()); break; }
        if (*cfd < 0) break; // shutdown
        int client_fd = *cfd;

        // Read the 4-byte msg_size header that the client sends right after
        // connect. Future RTTs on this connection all use that size.
        uint32_t msg_size_le = 0;
        if (!recv_exact_fd(client_fd, &msg_size_le, 4)) {
            ::close(client_fd);
            continue;
        }
        size_t msg_size = static_cast<size_t>(msg_size_le);
        if (msg_size < tsc::kBinaryHeaderSize || msg_size > buf.size()) {
            spdlog::error("mock_lat_tcp: bad msg_size {} (max {})", msg_size, buf.size());
            ::close(client_fd);
            continue;
        }
        spdlog::info("mock_lat_tcp: client connected, msg_size={}", msg_size);

        // Echo loop: read msg_size bytes, stamp recv/send TSCs, send back.
        while (g_running.load(std::memory_order_acquire)) {
            if (!recv_exact_fd(client_fd, buf.data(), msg_size)) break;
            uint64_t recv_tsc = eph::utils::TSC::now();
            std::memcpy(buf.data() + 8, &recv_tsc, 8);
            eph::utils::spin_for_ns(cfg.server_work_ns);
            uint64_t send_tsc = eph::utils::TSC::now();
            std::memcpy(buf.data() + 16, &send_tsc, 8);
            if (!send_all_fd(client_fd, buf.data(), msg_size)) break;
        }
        ::close(client_fd);
        spdlog::info("mock_lat_tcp: client disconnected");
    }
    ::close(*listen_fd);
    return 0;
}

} // namespace bench::tcp::mock_fn

// ───────────────────────────────────────────────────────────────────────────
// client_fn — TCP RTT bench. Two scenario classes (POSIX + DPDK) live here
// because they share the same buffer / TSC stamping logic; only the wire
// transport differs.
// ───────────────────────────────────────────────────────────────────────────
namespace bench::tcp::client_fn {

#if !defined(EPH_USE_DPDK)
// ── kernel scenario ────────────────────────────────────────────────────
class TcpRttScenario {
public:
    TcpRttScenario(std::string ip, uint16_t port)
        : ip_(std::move(ip)), port_(port) {}

    // Each prepare() reopens the connection so the mock can re-read the
    // msg_size header. Cheap (~50 µs on direct-attached NICs).
    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        cleanup();
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        if (::inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
            ::close(fd); return false;
        }
        // Retry connect briefly — the forked mock may not be listening yet.
        bool connected = false;
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                connected = true; break;
            }
            ::usleep(20'000); // 20 ms × 50 = 1 s budget
        }
        if (!connected) { ::close(fd); return false; }
        fd_ = fd;

        // Tell the mock how big each subsequent message will be.
        uint32_t n = static_cast<uint32_t>(payload);
        if (!send_all_fd(fd_, &n, 4)) { cleanup(); return false; }

        msg_size_ = payload;
        send_buf_.assign(payload, 0xAB);
        recv_buf_.assign(payload, 0);
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!send_all_fd(fd_, send_buf_.data(), msg_size_)) return false;
        if (!recv_exact_fd(fd_, recv_buf_.data(), msg_size_)) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    ~TcpRttScenario() { cleanup(); }

private:
    std::string ip_;
    uint16_t    port_;
    int         fd_ = -1;
    size_t      msg_size_ = 0;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

int run(const BenchConfig& cfg, int /*argc*/, char** /*argv*/) {
    if (auto e = bench::enter_netns("bench_ns"); !e) {
        spdlog::error("lat_tcp: enter_netns: {}", e.error());
        return 1;
    }
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_tcp", policy); !p) {
        spdlog::error("lat_tcp: pin failed: {}", p.error());
        return 1;
    }

    spdlog::info("lat_tcp (kernel): peer {}:{}", cfg.server_ip, cfg.server_port);

    std::span<const size_t> payloads =
        cfg.tcp_payloads.empty()
            ? std::span<const size_t>{kDefaultTcpPayloads}
            : std::span<const size_t>{cfg.tcp_payloads};

    TcpRttScenario scenario{cfg.server_ip, cfg.server_port};
    BenchRunner runner{cfg, "tcp", "kernel"};
    runner.run_rtt_sweep(scenario, payloads);
    return 0;
}

#else // EPH_USE_DPDK

// ── DPDK scenario ──────────────────────────────────────────────────────
class TcpDpdkRttScenario {
public:
    TcpDpdkRttScenario(DpdkBenchEnv& env, uint16_t server_port)
        : env_(env), server_port_(server_port) {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        cleanup();

        // Rotating source port avoids TIME_WAIT after each reconnect.
        auto tcfg = env_.make_tcp_config(src_port_++, server_port_);
        session_.emplace(tcfg, env_.pool);
        if (auto r = session_->connect(std::chrono::seconds{3}); !r) {
            spdlog::error("lat_tcp(dpdk): connect: {}", r.error());
            session_.reset();
            return false;
        }

        // Send 4-byte length prefix to the mock.
        uint32_t n = static_cast<uint32_t>(payload);
        if (!send_dpdk(reinterpret_cast<const uint8_t*>(&n), 4)) {
            cleanup(); return false;
        }

        msg_size_ = payload;
        send_buf_.assign(payload, 0xAB);
        recv_buf_.assign(payload, 0);
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!send_dpdk(send_buf_.data(), msg_size_)) return false;

        // Read exactly msg_size_ bytes via poll_rx callback.
        size_t got = 0;
        uint64_t deadline = eph::utils::TSC::now() +
                            tsc::ns_to_cycles(100'000'000); // 100 ms
        while (got < msg_size_ && eph::utils::TSC::now() < deadline) {
            auto r = session_->poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    size_t take = std::min(static_cast<size_t>(len),
                                           msg_size_ - got);
                    std::memcpy(recv_buf_.data() + got, data, take);
                    got += take;
                });
            if (!r) return false;
        }
        if (got < msg_size_) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {
        if (session_) {
            (void)session_->close();
            session_.reset();
        }
    }

    ~TcpDpdkRttScenario() { cleanup(); }

private:
    bool send_dpdk(const uint8_t* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            size_t chunk = std::min(len - sent, size_t{1460});
            auto r = session_->send(data + sent,
                                    static_cast<uint16_t>(chunk));
            if (!r) return false;
            sent += *r;
        }
        return true;
    }

    DpdkBenchEnv& env_;
    uint16_t server_port_;
    uint16_t src_port_ = 55600;
    std::optional<eph::dpdk::TcpSession<>> session_;
    size_t msg_size_ = 0;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

int run(const BenchConfig& cfg, int argc, char** argv) {
    auto env = DpdkBenchEnv::create_full(argc, argv,
        cfg.server_ip, cfg.local_ip, cfg.gateway_ip, /*port_id=*/0);
    if (!env) { spdlog::error("lat_tcp(dpdk): {}", env.error()); return 1; }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_tcp", policy); !p) {
        spdlog::error("lat_tcp: pin failed: {}", p.error());
        return 1;
    }
    spdlog::info("lat_tcp (dpdk): peer {}:{}", cfg.server_ip, cfg.server_port);

    std::span<const size_t> payloads =
        cfg.tcp_payloads.empty()
            ? std::span<const size_t>{kDefaultTcpPayloads}
            : std::span<const size_t>{cfg.tcp_payloads};

    TcpDpdkRttScenario scenario{*env, cfg.server_port};
    BenchRunner runner{cfg, "tcp", "dpdk"};
    runner.run_rtt_sweep(scenario, payloads);
    return 0;
}

#endif // EPH_USE_DPDK

} // namespace bench::tcp::client_fn

// ───────────────────────────────────────────────────────────────────────────
// main — fork mock + run client + reap child.
// ───────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    install_signal_handlers();
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    auto cfg_r = load_bench_conf();
    if (!cfg_r) { spdlog::error("{}", cfg_r.error()); return 1; }
    const BenchConfig& cfg = *cfg_r;

    pid_t pid = ::fork();
    if (pid < 0) {
        spdlog::error("fork: {}", std::strerror(errno));
        return 1;
    }
    if (pid == 0) {
        // Child: kernel TCP mock in the host ns.
        return bench::tcp::mock_fn::run(cfg);
    }

    // Parent: bench client. On error or normal exit we tear the child down.
    int rc = bench::tcp::client_fn::run(cfg, argc, argv);

    ::kill(pid, SIGTERM);
    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    return rc;
}

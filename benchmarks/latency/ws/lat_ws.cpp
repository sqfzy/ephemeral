/// @file ws/lat_ws.cpp
/// lat_ws / lat_ws_dpdk — single-binary plain WebSocket latency benchmark.
///
/// Same fork+setns layout as lat_tcp.cpp / lat_udp.cpp. Frames are
/// self-describing so the mock handles variable client payload sizes
/// without needing per-payload reconnects.

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
#include <poll.h>
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
#include "../core/socket_io.hpp"
#include "../core/tsc_protocol.hpp"
#include "../core/ws_framing.hpp"
#include "../core/ws_handshake.hpp"

#if defined(EPH_USE_DPDK)
#include "../core/dpdk_env.hpp"
#include "eph/dpdk/tcp.hpp"
#endif

using namespace bench;

namespace {

constexpr std::array<size_t, 6> kDefaultWsPayloads{
    64, 128, 256, 512, 1024, 4096
};

// send_all_fd / recv_exact_fd live in core/socket_io.hpp now.

/// Read one server→client (unmasked) frame's payload into `out` (capacity
/// `cap`). Returns payload length on success, 0 on failure / control frame.
size_t recv_one_ws_frame(int fd, uint8_t* out, size_t cap) noexcept {
    uint8_t hdr[2];
    if (!recv_exact_fd(fd, hdr, 2)) return 0;
    uint8_t opcode = hdr[0] & 0x0F;
    if ((hdr[1] & 0x80) != 0) return 0;
    uint64_t plen = hdr[1] & 0x7F;
    if (plen == 126) {
        uint8_t ext[2];
        if (!recv_exact_fd(fd, ext, 2)) return 0;
        plen = (uint64_t(ext[0]) << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (!recv_exact_fd(fd, ext, 8)) return 0;
        plen = 0;
        for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
    }
    if (plen > cap) return 0;
    if (plen > 0 && !recv_exact_fd(fd, out, plen)) return 0;
    if (opcode != ws_framing::kOpText && opcode != ws_framing::kOpBinary) return 0;
    return static_cast<size_t>(plen);
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// mock_fn — POSIX WebSocket echo server.
// ───────────────────────────────────────────────────────────────────────────
namespace bench::ws::mock_fn {

int run(const BenchConfig& cfg) {
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_lat_ws", policy); !p) {
        spdlog::error("mock_lat_ws: pin failed: {}", p.error());
        return 1;
    }

    auto listen_fd = bench::tcp_bind_listen(cfg.server_ip, cfg.server_port);
    if (!listen_fd) { spdlog::error("mock_lat_ws: {}", listen_fd.error()); return 1; }
    spdlog::info("mock_lat_ws: listening on {}:{} work_ns={}",
                 cfg.server_ip, cfg.server_port, cfg.server_work_ns);

    constexpr size_t kRxBufCap = 65536;
    std::vector<uint8_t> rx(kRxBufCap);
    std::vector<uint8_t> tx_payload(1024);
    std::vector<uint8_t> tx_frame(1024 + 16);

    while (g_running.load(std::memory_order_acquire)) {
        auto cfd = bench::accept_one(*listen_fd, g_running);
        if (!cfd) { spdlog::error("mock_lat_ws: {}", cfd.error()); break; }
        if (*cfd < 0) break;
        int client_fd = *cfd;

        if (auto h = bench::ws_server_handshake(client_fd); !h) {
            spdlog::error("mock_lat_ws handshake: {}", h.error());
            ::close(client_fd);
            continue;
        }
        spdlog::info("mock_lat_ws: handshake complete");

        size_t buffered = 0;
        bool ok = true;
        while (ok && g_running.load(std::memory_order_acquire)) {
            // Refill until we likely have at least one frame header.
            while (buffered < 14) {
                pollfd p{}; p.fd = client_fd; p.events = POLLIN;
                int rv = ::poll(&p, 1, 100);
                if (rv < 0) { if (errno == EINTR) continue; ok = false; break; }
                if (rv == 0) {
                    if (!g_running.load(std::memory_order_acquire)) { ok = false; break; }
                    continue;
                }
                ssize_t n = ::recv(client_fd, rx.data() + buffered,
                                   rx.size() - buffered, 0);
                if (n <= 0) { ok = false; break; }
                buffered += static_cast<size_t>(n);
            }
            if (!ok) break;

            auto frame = ws_framing::parse_client_frame_inplace(rx.data(), buffered, kRxBufCap);
            while (!frame.has_value() && buffered < kRxBufCap) {
                pollfd p{}; p.fd = client_fd; p.events = POLLIN;
                int rv = ::poll(&p, 1, 100);
                if (rv <= 0) { if (rv < 0 && errno == EINTR) continue; ok = false; break; }
                ssize_t n = ::recv(client_fd, rx.data() + buffered,
                                   rx.size() - buffered, 0);
                if (n <= 0) { ok = false; break; }
                buffered += static_cast<size_t>(n);
                frame = ws_framing::parse_client_frame_inplace(rx.data(), buffered, kRxBufCap);
            }
            if (!ok || !frame.has_value()) { ok = false; break; }

            uint64_t recv_tsc = eph::utils::TSC::now();
            const uint8_t* payload = rx.data() + (frame->total_consumed - frame->payload_len);
            uint64_t client_send_tsc = tsc::parse_T_send(payload, frame->payload_len);

            eph::utils::spin_for_ns(cfg.server_work_ns);
            uint64_t send_tsc = eph::utils::TSC::now();

            int n = std::snprintf(reinterpret_cast<char*>(tx_payload.data()),
                tx_payload.size(),
                R"({"T":%llu,"T_recv":%llu,"T_send":%llu})",
                static_cast<unsigned long long>(send_tsc),
                static_cast<unsigned long long>(recv_tsc),
                static_cast<unsigned long long>(client_send_tsc));
            if (n <= 0) { ok = false; break; }

            tx_frame.resize(static_cast<size_t>(n) + 16);
            size_t flen = ws_framing::build_server_frame(tx_frame.data(),
                ws_framing::kOpText, tx_payload.data(), static_cast<size_t>(n));
            if (!send_all_fd(client_fd, tx_frame.data(), flen)) { ok = false; break; }

            size_t consumed = frame->total_consumed;
            std::memmove(rx.data(), rx.data() + consumed, buffered - consumed);
            buffered -= consumed;
        }
        ::close(client_fd);
        spdlog::info("mock_lat_ws: client disconnected");
    }
    ::close(*listen_fd);
    return 0;
}

} // namespace bench::ws::mock_fn

// ───────────────────────────────────────────────────────────────────────────
// client_fn — WebSocket RTT bench (kernel + DPDK).
// ───────────────────────────────────────────────────────────────────────────
namespace bench::ws::client_fn {

namespace {
// Build the JSON payload `{"T_send":<tsc>,"pad":"xxx..."}` filling exactly
// `target` bytes. Returns the actual length written (== target on success,
// 0 on failure).
size_t build_ws_payload(uint8_t* out, size_t target, uint64_t tsc) {
    char prefix[80];
    int n = std::snprintf(prefix, sizeof(prefix),
        R"({"T_send":%llu,"pad":")",
        static_cast<unsigned long long>(tsc));
    if (n <= 0) return 0;
    size_t prefix_len = static_cast<size_t>(n);
    constexpr size_t suffix_len = 2; // "}"
    if (prefix_len + suffix_len >= target) return 0;
    size_t pad_len = target - prefix_len - suffix_len;
    std::memcpy(out, prefix, prefix_len);
    std::memset(out + prefix_len, 'x', pad_len);
    out[prefix_len + pad_len + 0] = '"';
    out[prefix_len + pad_len + 1] = '}';
    return target;
}
} // namespace

#if !defined(EPH_USE_DPDK)
class WsRttScenario {
public:
    WsRttScenario(std::string ip, uint16_t port)
        : ip_(std::move(ip)), port_(port) {}

    bool open() {
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
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd); return false;
        }

        // Client-side WS upgrade. Sec-WebSocket-Key is fixed because the
        // mock just validates presence and computes the accept digest.
        char host[64];
        std::snprintf(host, sizeof(host), "%s:%u", ip_.c_str(), port_);
        std::string req =
            std::string("GET / HTTP/1.1\r\nHost: ") + host +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!send_all_fd(fd, req.data(), req.size())) { ::close(fd); return false; }

        char resp[4096];
        size_t got = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (got < sizeof(resp)) {
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (rem.count() <= 0) { ::close(fd); return false; }
            pollfd pfd{}; pfd.fd = fd; pfd.events = POLLIN;
            int rv = ::poll(&pfd, 1, static_cast<int>(rem.count()));
            if (rv <= 0) { ::close(fd); return false; }
            ssize_t n = ::recv(fd, resp + got, sizeof(resp) - got, 0);
            if (n <= 0) { ::close(fd); return false; }
            got += static_cast<size_t>(n);
            if (std::string_view{resp, got}.find("\r\n\r\n") != std::string_view::npos) break;
        }
        if (std::string_view{resp, got}.find("101") == std::string_view::npos) {
            ::close(fd); return false;
        }
        fd_ = fd;
        return true;
    }

    bool prepare(size_t payload) {
        if (payload < 64) return false;
        target_payload_ = payload;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        json_buf_.resize(target_payload_);
        if (!build_ws_payload(json_buf_.data(), target_payload_, out.client_send_tsc)) return false;

        frame_buf_.resize(target_payload_ + 16);
        size_t flen = ws_framing::build_masked_text_frame(
            frame_buf_.data(), json_buf_.data(), target_payload_, mask_seed_++);
        if (!send_all_fd(fd_, frame_buf_.data(), flen)) return false;

        recv_buf_.resize(2048);
        size_t plen = recv_one_ws_frame(fd_, recv_buf_.data(), recv_buf_.size());
        if (plen == 0) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        out.server_recv_tsc = tsc::parse_T_recv(recv_buf_.data(), plen);
        out.server_send_tsc = tsc::parse_T(recv_buf_.data(), plen);
        return true;
    }

    void cleanup() {}

    ~WsRttScenario() { if (fd_ >= 0) ::close(fd_); }

private:
    std::string ip_;
    uint16_t    port_;
    int         fd_ = -1;
    size_t      target_payload_ = 0;
    uint32_t    mask_seed_ = 0xDEADBEEF;
    std::vector<uint8_t> json_buf_;
    std::vector<uint8_t> frame_buf_;
    std::vector<uint8_t> recv_buf_;
};

int run(const BenchConfig& cfg, int /*argc*/, char** /*argv*/) {
    if (auto e = bench::enter_netns("bench_ns"); !e) {
        spdlog::error("lat_ws: enter_netns: {}", e.error());
        return 1;
    }
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_ws", policy); !p) {
        spdlog::error("lat_ws: pin failed: {}", p.error());
        return 1;
    }

    WsRttScenario scenario{cfg.server_ip, cfg.server_port};
    bool opened = false;
    for (int i = 0; i < 50; ++i) {
        if (scenario.open()) { opened = true; break; }
        ::usleep(20'000);
    }
    if (!opened) {
        spdlog::error("lat_ws: failed to connect / handshake after retries");
        return 1;
    }
    spdlog::info("lat_ws (kernel): connected to {}:{}", cfg.server_ip, cfg.server_port);

    std::span<const size_t> payloads =
        cfg.ws_payloads.empty()
            ? std::span<const size_t>{kDefaultWsPayloads}
            : std::span<const size_t>{cfg.ws_payloads};

    BenchRunner runner{cfg, "ws", "kernel"};
    runner.run_rtt_sweep(scenario, payloads);
    return 0;
}

#else // EPH_USE_DPDK

[[nodiscard]] inline std::expected<void, std::string>
dpdk_ws_handshake(eph::dpdk::TcpSession<>& session, std::string_view host) {
    std::string req =
        std::string("GET / HTTP/1.1\r\nHost: ") + std::string(host) +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    size_t sent = 0;
    while (sent < req.size()) {
        size_t chunk = std::min(req.size() - sent, size_t{1460});
        auto r = session.send(
            reinterpret_cast<const uint8_t*>(req.data()) + sent,
            static_cast<uint16_t>(chunk));
        if (!r) return std::unexpected("ws handshake send: " + r.error());
        sent += *r;
    }
    std::string resp;
    resp.reserve(512);
    uint64_t deadline = eph::utils::TSC::now() + tsc::ns_to_cycles(2'000'000'000ULL);
    while (eph::utils::TSC::now() < deadline) {
        auto r = session.poll_rx(
            [&](const uint8_t* data, uint16_t len) {
                resp.append(reinterpret_cast<const char*>(data), len);
            });
        if (!r) return std::unexpected("ws handshake recv: " + r.error());
        if (resp.find("\r\n\r\n") != std::string::npos) break;
    }
    if (resp.find("101") == std::string::npos) {
        return std::unexpected("ws handshake rejected (no 101)");
    }
    return {};
}

class WsDpdkRttScenario {
public:
    explicit WsDpdkRttScenario(eph::dpdk::TcpSession<>& session)
        : session_(session) {}

    bool prepare(size_t payload) {
        if (payload < 64) return false;
        target_payload_ = payload;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        json_buf_.resize(target_payload_);
        if (!build_ws_payload(json_buf_.data(), target_payload_, out.client_send_tsc)) return false;

        frame_buf_.resize(target_payload_ + 16);
        size_t flen = ws_framing::build_masked_text_frame(
            frame_buf_.data(), json_buf_.data(), target_payload_, mask_seed_++);

        size_t sent = 0;
        while (sent < flen) {
            size_t chunk = std::min(flen - sent, size_t{1460});
            auto r = session_.send(frame_buf_.data() + sent,
                                   static_cast<uint16_t>(chunk));
            if (!r) return false;
            sent += *r;
        }

        rx_accum_.clear();
        uint64_t deadline = eph::utils::TSC::now() +
                            tsc::ns_to_cycles(100'000'000ULL);
        while (eph::utils::TSC::now() < deadline) {
            auto r = session_.poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    rx_accum_.insert(rx_accum_.end(), data, data + len);
                });
            if (!r) return false;
            auto [hdr, plen] = ws_framing::parse_server_frame(
                rx_accum_.data(), rx_accum_.size());
            if (plen > 0) {
                out.client_recv_tsc = eph::utils::TSC::now();
                const uint8_t* payload = rx_accum_.data() + hdr;
                out.server_recv_tsc = tsc::parse_T_recv(payload, plen);
                out.server_send_tsc = tsc::parse_T(payload, plen);
                return true;
            }
        }
        return false;
    }

    void cleanup() {}

private:
    eph::dpdk::TcpSession<>& session_;
    size_t target_payload_ = 0;
    uint32_t mask_seed_ = 0xDEADBEEF;
    std::vector<uint8_t> json_buf_;
    std::vector<uint8_t> frame_buf_;
    std::vector<uint8_t> rx_accum_;
};

int run(const BenchConfig& cfg, int argc, char** argv) {
    auto env = DpdkBenchEnv::create_full(argc, argv,
        cfg.server_ip, cfg.local_ip, cfg.gateway_ip, /*port_id=*/0);
    if (!env) { spdlog::error("lat_ws(dpdk): {}", env.error()); return 1; }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_ws", policy); !p) {
        spdlog::error("lat_ws: pin failed: {}", p.error());
        return 1;
    }

    static uint16_t src_port = 55700;
    auto tcfg = env->make_tcp_config(src_port++, cfg.server_port);
    eph::dpdk::TcpSession<> session{tcfg, env->pool};
    if (auto r = session.connect(std::chrono::seconds{3}); !r) {
        spdlog::error("lat_ws(dpdk): connect: {}", r.error()); return 1;
    }
    std::string host = cfg.server_ip + ":" + std::to_string(cfg.server_port);
    if (auto h = dpdk_ws_handshake(session, host); !h) {
        spdlog::error("lat_ws(dpdk): handshake: {}", h.error()); return 1;
    }
    spdlog::info("lat_ws (dpdk): connected to {}:{}", cfg.server_ip, cfg.server_port);

    std::span<const size_t> payloads =
        cfg.ws_payloads.empty()
            ? std::span<const size_t>{kDefaultWsPayloads}
            : std::span<const size_t>{cfg.ws_payloads};

    WsDpdkRttScenario scenario{session};
    BenchRunner runner{cfg, "ws", "dpdk"};
    runner.run_rtt_sweep(scenario, payloads);

    (void)session.close();
    return 0;
}

#endif // EPH_USE_DPDK

} // namespace bench::ws::client_fn

// ───────────────────────────────────────────────────────────────────────────
// main
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
    if (pid < 0) { spdlog::error("fork: {}", std::strerror(errno)); return 1; }
    if (pid == 0) {
        return bench::ws::mock_fn::run(cfg);
    }

    int rc = bench::ws::client_fn::run(cfg, argc, argv);

    ::kill(pid, SIGTERM);
    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    return rc;
}

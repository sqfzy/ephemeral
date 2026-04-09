/// @file exchange/lat_ex_md_udp.cpp
/// lat_ex_md_udp / lat_ex_md_udp_dpdk — UDP market-data echo bench. Forks
/// the kernel UDP echo mock, then runs an RTT sweep with payloads filled
/// to look like a bookTicker feed (so a tcpdump trace is recognisable).
/// Wire format and timing are identical to lat_udp.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu_pin.hpp"
#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "../core/netns.hpp"
#include "../core/runner.hpp"
#include "../core/sample.hpp"
#include "../core/signal.hpp"
#include "../core/tsc_protocol.hpp"

#include "mock_md_udp.hpp"

#if defined(EPH_USE_DPDK)
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include "../core/dpdk_env.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/udp.hpp"
#endif

using namespace bench;

namespace {
// Market-data UDP payloads: 64=bookTicker, 256=depth update,
// 1024=depth snapshot, 1400=large snapshot near MTU.
constexpr std::array<size_t, 4> kDefaultMdPayloads{
    64, 256, 1024, 1400
};

// Fill `buf` from offset `tsc::kBinaryHeaderSize` onward with a fake
// bookTicker JSON template repeated to the target size. Pure aesthetics —
// does not affect timing.
void fill_md_payload(uint8_t* buf, size_t payload) {
    constexpr std::string_view tmpl =
        R"({"s":"BTCUSDT","b":"50000.00","a":"50001.00","T":0})";
    size_t cursor = tsc::kBinaryHeaderSize;
    while (cursor < payload) {
        size_t take = std::min(payload - cursor, tmpl.size());
        std::memcpy(buf + cursor, tmpl.data(), take);
        cursor += take;
    }
}
} // namespace

namespace bench::exchange::md_udp_client {

#if !defined(EPH_USE_DPDK)
class MdUdpScenario {
public:
    MdUdpScenario(std::string ip, uint16_t port)
        : ip_(std::move(ip)), port_(port) {}

    bool open() {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        timeval tv{}; tv.tv_usec = 100'000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        peer_.sin_family = AF_INET;
        peer_.sin_port = htons(port_);
        if (::inet_pton(AF_INET, ip_.c_str(), &peer_.sin_addr) != 1) {
            ::close(fd); return false;
        }
        fd_ = fd;
        return true;
    }

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0);
        recv_buf_.assign(payload, 0);
        fill_md_payload(send_buf_.data(), payload);
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        ssize_t s = ::sendto(fd_, send_buf_.data(), send_buf_.size(), 0,
                             reinterpret_cast<const sockaddr*>(&peer_),
                             sizeof(peer_));
        if (s < 0) return false;
        ssize_t n = ::recvfrom(fd_, recv_buf_.data(), recv_buf_.size(), 0,
                               nullptr, nullptr);
        if (n < static_cast<ssize_t>(tsc::kBinaryHeaderSize)) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {}

    ~MdUdpScenario() { if (fd_ >= 0) ::close(fd_); }

private:
    std::string ip_;
    uint16_t    port_;
    int         fd_ = -1;
    sockaddr_in peer_{};
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

int run(const BenchConfig& cfg, int /*argc*/, char** /*argv*/) {
    if (auto e = bench::enter_netns("bench_ns"); !e) {
        spdlog::error("lat_ex_md_udp: enter_netns: {}", e.error());
        return 1;
    }
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_ex_md", policy); !p) {
        spdlog::error("lat_ex_md_udp: pin failed: {}", p.error());
        return 1;
    }

    MdUdpScenario scenario{cfg.server_ip, cfg.server_port};
    bool opened = false;
    for (int i = 0; i < 50; ++i) {
        if (scenario.open()) { opened = true; break; }
        ::usleep(20'000);
    }
    if (!opened) {
        spdlog::error("lat_ex_md_udp: failed to open client socket");
        return 1;
    }
    spdlog::info("lat_ex_md_udp (kernel): peer {}:{}", cfg.server_ip, cfg.server_port);

    std::span<const size_t> payloads =
        cfg.md_udp_payloads.empty()
            ? std::span<const size_t>{kDefaultMdPayloads}
            : std::span<const size_t>{cfg.md_udp_payloads};

    BenchRunner runner{cfg, "exchange/md_udp", "kernel"};
    runner.run_rtt_sweep(scenario, payloads);
    return 0;
}

#else // EPH_USE_DPDK

class MdUdpDpdkScenario {
public:
    MdUdpDpdkScenario(eph::dpdk::UdpSender& sender,
                      uint16_t port_id, uint16_t rx_queue,
                      uint16_t expected_dst_port)
        : sender_(sender), port_id_(port_id), rx_queue_(rx_queue),
          expected_dst_port_(expected_dst_port),
          timeout_cycles_(tsc::ns_to_cycles(100'000'000)) {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0);
        recv_buf_.assign(payload, 0);
        payload_size_ = payload;
        fill_md_payload(send_buf_.data(), payload);
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!sender_.send(send_buf_.data(), static_cast<uint16_t>(payload_size_)))
            return false;

        uint64_t deadline = eph::utils::TSC::now() + timeout_cycles_;
        rte_mbuf* pkts[32];
        while (eph::utils::TSC::now() < deadline) {
            uint16_t nb_rx = rte_eth_rx_burst(port_id_, rx_queue_, pkts, 32);
            for (uint16_t i = 0; i < nb_rx; ++i) {
                auto parsed = eph::dpdk::net::parse_udp_packet(pkts[i]);
                if (parsed &&
                    parsed.dst_port() == expected_dst_port_ &&
                    parsed.payload_len >= tsc::kBinaryHeaderSize) {
                    size_t n = std::min(static_cast<size_t>(parsed.payload_len),
                                        recv_buf_.size());
                    std::memcpy(recv_buf_.data(), parsed.payload, n);
                    for (uint16_t j = 0; j < nb_rx; ++j) rte_pktmbuf_free(pkts[j]);
                    out.client_recv_tsc = eph::utils::TSC::now();
                    std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
                    std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
                    return true;
                }
                rte_pktmbuf_free(pkts[i]);
            }
        }
        return false;
    }

    void cleanup() {}

private:
    eph::dpdk::UdpSender& sender_;
    uint16_t port_id_;
    uint16_t rx_queue_;
    uint16_t expected_dst_port_;
    uint64_t timeout_cycles_;
    size_t payload_size_ = 0;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

int run(const BenchConfig& cfg, int argc, char** argv) {
    auto env = DpdkBenchEnv::create_full(argc, argv,
        cfg.server_ip, cfg.local_ip, cfg.gateway_ip, /*port_id=*/0);
    if (!env) { spdlog::error("lat_ex_md_udp(dpdk): {}", env.error()); return 1; }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_ex_md", policy); !p) {
        spdlog::error("lat_ex_md_udp: pin failed: {}", p.error());
        return 1;
    }

    constexpr uint16_t kLocalPort = 55550;
    auto sender = env->make_udp_sender(kLocalPort, cfg.server_port);
    if (!sender) { spdlog::error("UdpSender: {}", sender.error()); return 1; }
    spdlog::info("lat_ex_md_udp (dpdk): peer {}:{}", cfg.server_ip, cfg.server_port);

    std::span<const size_t> payloads =
        cfg.md_udp_payloads.empty()
            ? std::span<const size_t>{kDefaultMdPayloads}
            : std::span<const size_t>{cfg.md_udp_payloads};

    MdUdpDpdkScenario scenario{*sender, env->port_id, 0, kLocalPort};
    BenchRunner runner{cfg, "exchange/md_udp", "dpdk"};
    runner.run_rtt_sweep(scenario, payloads);
    return 0;
}
#endif // EPH_USE_DPDK

} // namespace bench::exchange::md_udp_client

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
        return bench::exchange::run_exchange_md_udp_mock(cfg);
    }

    int rc = bench::exchange::md_udp_client::run(cfg, argc, argv);

    ::kill(pid, SIGTERM);
    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    return rc;
}

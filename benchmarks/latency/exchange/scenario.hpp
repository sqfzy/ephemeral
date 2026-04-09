/// @file exchange/scenario.hpp
/// Three quantitative-trading scenarios:
///
///   - MarketRxScenario  (OneWayScenario): wait for the next pushed
///     bookTicker frame, parse the server T (TSC) field, and return one
///     OneWaySample {producer=T, consumer=now}.
///
///   - OrderRttScenario  (RttScenario): pipeline up to N orders inflight,
///     match each ExecutionReport against the originating client_send_tsc,
///     and return one RttSample per matched response. The "payload" arg
///     to prepare() is reinterpreted as the inflight target N.
///
///   - MdUdpScenario     (RttScenario): identical to UdpRttScenario at the
///     wire level (24-byte binary header + payload bytes), but the payload
///     bytes are filled with a market-data-like pattern (so a tcpdump
///     trace looks like a real feed). The bench label changes from "udp"
///     to "exchange/md_udp" so the report makes sense.
///
/// All three classes are header-only and stateless across rounds (apart
/// from buffer reuse). They depend only on `core/sample.hpp`,
/// `core/tsc_protocol.hpp`, and the per-scenario client helper headers.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../core/sample.hpp"
#include "../core/tsc_protocol.hpp"
#include "../core/ws_framing.hpp"
#include "../udp/client.hpp"
#include "../ws/client.hpp"
#include "eph/utils/time.hpp"

namespace bench::exchange {

// ── 1. MarketRxScenario ─────────────────────────────────────────────────

/// 1-leg pipeline scenario: receive the next pushed frame from the WS
/// connection, parse the server's "T" field, yield one sample.
class MarketRxScenario {
public:
    explicit MarketRxScenario(int fd) : fd_(fd), buf_(2048) {}

    bool prepare() { return true; }

    bool do_one_recv(OneWaySample& out) {
        size_t n = bench::ws::recv_one_frame(fd_, buf_.data(), buf_.size());
        if (n == 0) return false;
        uint64_t t = bench::tsc::parse_T(buf_.data(), n);
        if (t == 0) return false;
        out.producer_tsc = t;
        out.consumer_tsc = eph::utils::TSC::now();
        return true;
    }

    void cleanup() {}

private:
    int fd_;
    std::vector<uint8_t> buf_;
};

// ── 2. OrderRttScenario ─────────────────────────────────────────────────

/// N-inflight order pipeline. The "payload" parameter to prepare() is
/// reinterpreted as the inflight target N.
///
/// Strategy:
///   - prepare(N): set inflight_target_, reset slot table.
///   - do_one_rtt: while in_flight < N, send a new order; then poll until
///     a matching ExecutionReport arrives. Return one matched RttSample.
///   - Each order gets a monotonic id; send_tsc is stored in
///     slots_[id % kMaxInflight]. When the response arrives we look up
///     the slot via the echoed id and recover the original send_tsc.
///   - kMaxInflight = 128 covers all expected sweeps; modulo collisions
///     would require >128 inflight which is out of scope.
class OrderRttScenario {
public:
    static constexpr size_t kMaxInflight = 128;

    explicit OrderRttScenario(int fd) : fd_(fd) {}

    bool prepare(size_t inflight) {
        if (inflight == 0 || inflight > kMaxInflight) return false;
        inflight_target_ = inflight;
        slots_.fill(0);
        next_id_   = 1;
        in_flight_ = 0;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        // Top up the in-flight pipeline.
        while (in_flight_ < inflight_target_) {
            if (!send_one()) return false;
        }
        // Wait for ONE response, return it.
        return wait_for_one(out);
    }

    void cleanup() {}

private:
    bool send_one() {
        uint64_t id = next_id_++;
        uint64_t now_tsc = eph::utils::TSC::now();
        slots_[id % kMaxInflight] = now_tsc;

        // Order JSON: include id + T_send. The mock parses id and T_send,
        // echoes them in the ExecutionReport along with T_recv and T.
        char body[256];
        int n = std::snprintf(body, sizeof(body),
            R"({"id":%llu,"T_send":%llu,"sym":"BTCUSDT","px":"50000.00","qty":"0.001"})",
            static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(now_tsc));
        if (n <= 0) return false;

        std::vector<uint8_t> frame(static_cast<size_t>(n) + 16);
        size_t flen = bench::ws_framing::build_masked_text_frame(
            frame.data(), body, static_cast<size_t>(n), mask_seed_++);
        if (!bench::ws::detail::send_all(fd_, frame.data(), flen)) return false;
        ++in_flight_;
        return true;
    }

    bool wait_for_one(RttSample& out) {
        std::array<uint8_t, 2048> buf{};
        for (;;) {
            size_t n = bench::ws::recv_one_frame(fd_, buf.data(), buf.size());
            if (n == 0) return false;

            // Match by echoed id — bookTicker pushes on the same connection
            // are silently skipped (their id is 0).
            uint64_t id = bench::tsc::detail::parse_uint64_after(
                std::string_view{reinterpret_cast<const char*>(buf.data()), n},
                "\"id\":");
            if (id == 0) continue;

            uint64_t slot = slots_[id % kMaxInflight];
            if (slot == 0) continue; // duplicate / stale
            slots_[id % kMaxInflight] = 0;

            out.client_send_tsc = slot;
            out.server_recv_tsc = bench::tsc::parse_T_recv(buf.data(), n);
            out.server_send_tsc = bench::tsc::parse_T(buf.data(), n);
            out.client_recv_tsc = eph::utils::TSC::now();
            --in_flight_;
            return true;
        }
    }

    int fd_;
    size_t inflight_target_ = 1;
    size_t in_flight_ = 0;
    uint64_t next_id_ = 1;
    uint32_t mask_seed_ = 0xCAFEF00D;
    std::array<uint64_t, kMaxInflight> slots_{};
};

// ── 3. MdUdpScenario ────────────────────────────────────────────────────

/// UDP echo scenario with market-data-style payload. Wire format is the
/// same 24-byte TSC header used by udp/scenario.hpp; the only difference
/// is the *content* of the bytes after the header (so a tcpdump capture
/// looks like a real bookTicker feed). Latency-wise this is identical to
/// the raw udp scenario; the separate target exists so the report label
/// reflects the quant business intent.
class MdUdpScenario {
public:
    explicit MdUdpScenario(bench::udp::Endpoint& ep) : ep_(ep) {}

    bool prepare(size_t payload) {
        if (payload < bench::tsc::kBinaryHeaderSize) return false;
        send_buf_.resize(payload);
        recv_buf_.resize(payload);
        // Fill the post-header bytes with a fake bookTicker JSON repeated
        // out to the target size. Pure aesthetics — does not affect timing.
        constexpr std::string_view tmpl =
            R"({"s":"BTCUSDT","b":"50000.00","a":"50001.00","T":0})";
        size_t cursor = bench::tsc::kBinaryHeaderSize;
        while (cursor < payload) {
            size_t take = std::min(payload - cursor, tmpl.size());
            std::memcpy(send_buf_.data() + cursor, tmpl.data(), take);
            cursor += take;
        }
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!bench::udp::sendto_one(ep_, send_buf_.data(), send_buf_.size())) return false;
        ssize_t n = bench::udp::recvfrom_one(ep_, recv_buf_.data(), recv_buf_.size());
        if (n < static_cast<ssize_t>(bench::tsc::kBinaryHeaderSize)) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {}

private:
    bench::udp::Endpoint& ep_;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

} // namespace bench::exchange

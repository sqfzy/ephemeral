/// @file bench_soupbintcp_ouch.cpp
/// Microbenchmarks for `eph::itch::SoupBinTcpFramer` and the OUCH
/// outbound message views (`AcceptedView`, `ExecutedView`).
///
/// Coverage gap (batch 11): `bench_itch_parse.cpp` covers the ITCH
/// market-data parsers but not the OUCH order-entry side, and
/// SoupBinTcpFramer (the wrapper around OUCH for TCP transport) had
/// no bench either. This file pins:
/// - SoupBinTcpFramer encode/decode cost — every order out + every
///   accept/execute in goes through this framer over TCP.
/// - The two most-frequent inbound OUCH views: AcceptedView (66 B) and
///   ExecutedView (40 B). Each is a sequence of read_be32/64 + char
///   accessors; pinning the cost ensures any field-layout regression
///   surfaces.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/itch/ouch.hpp"
#include "eph/itch/soupbintcp.hpp"

using eph::itch::SoupBinTcpFramer;
using eph::itch::ouch::AcceptedView;
using eph::itch::ouch::ExecutedView;

namespace {

namespace ob = eph::itch::ouch;

/// Helper to build a wire-shape OUCH 'A' (Accepted) message of 66 bytes.
[[nodiscard]] std::array<uint8_t, AcceptedView::kSize> build_accepted() {
    std::array<uint8_t, AcceptedView::kSize> b{};
    b[0] = 'A';
    ob::write_be64(b.data() + 1, /*timestamp=*/0x0123456789ABCDEFULL);
    // 14-char token "ORD0000000001 "
    static constexpr char kTok[15] = "ORD0000000001 ";
    std::memcpy(b.data() + 9, kTok, 14);
    b[23] = 'B';                                   // side = Buy
    ob::write_be32(b.data() + 24, 100);    // shares = 100
    static constexpr char kSym[9] = "AAPL    ";
    std::memcpy(b.data() + 28, kSym, 8);
    ob::write_be32(b.data() + 36, 1500000); // price = $150.0000
    ob::write_be32(b.data() + 40, 0);       // tif = Day
    static constexpr char kFirm[5] = "EPH ";
    std::memcpy(b.data() + 44, kFirm, 4);
    b[48] = 'Y';                                   // display = displayed
    ob::write_be64(b.data() + 49, /*order_ref=*/12345);
    b[57] = 'O';                                   // capacity = agency
    b[58] = 'N';                                   // int_mkt_sweep
    b[59] = 'N';                                   // cross_type
    b[60] = 'L';                                   // order_state = live
    b[61] = '0';                                   // bbo_weight
    // bytes 62..65 reserved (already 0)
    return b;
}

/// Build a wire-shape OUCH 'E' (Executed) message of 40 bytes.
[[nodiscard]] std::array<uint8_t, ExecutedView::kSize> build_executed() {
    std::array<uint8_t, ExecutedView::kSize> b{};
    b[0] = 'E';
    ob::write_be64(b.data() + 1, /*timestamp=*/0xDEADBEEFCAFEBABEULL);
    static constexpr char kTok[15] = "ORD0000000001 ";
    std::memcpy(b.data() + 9, kTok, 14);
    ob::write_be32(b.data() + 23, 100);    // executed_shares
    ob::write_be32(b.data() + 27, 1500050); // execution_price
    b[31] = 'A';                                   // liquidity_flag
    ob::write_be64(b.data() + 32, /*match_no=*/9876543210ULL);
    return b;
}

} // namespace

// ---------------------------------------------------------------------------
// SoupBinTcpFramer: encode + decode round trip
// ---------------------------------------------------------------------------

static void BM_SoupBinFramerEncode(benchmark::State& state) {
    const std::size_t payload_len = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> payload(payload_len);
    std::vector<uint8_t> dst(payload_len + 3);
    SoupBinTcpFramer framer;
    for (auto _ : state) {
        auto n = framer.encode(dst.data(), payload.data(),
                               payload.size(), 'S');  // SequencedData
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(dst);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(payload_len + 3));
}
BENCHMARK(BM_SoupBinFramerEncode)->Arg(40)->Arg(66)->Arg(256);

static void BM_SoupBinFramerDecode(benchmark::State& state) {
    const std::size_t payload_len = static_cast<std::size_t>(state.range(0));
    std::vector<uint8_t> payload(payload_len);
    std::vector<uint8_t> wire(payload_len + 3);
    SoupBinTcpFramer framer_e;
    (void)framer_e.encode(wire.data(), payload.data(), payload.size(), 'S');

    SoupBinTcpFramer framer;
    for (auto _ : state) {
        auto r = framer.decode(wire.data(), wire.size());
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(wire.size()));
}
BENCHMARK(BM_SoupBinFramerDecode)->Arg(40)->Arg(66)->Arg(256);

static void BM_SoupBinFramerDecodePartial(benchmark::State& state) {
    // Only the 2-byte length header arrived; payload still en route.
    SoupBinTcpFramer framer;
    std::array<uint8_t, 2> wire{0x00, 0x42};  // wire_len=66 (typ AcceptedView)
    for (auto _ : state) {
        auto r = framer.decode(wire.data(), wire.size());
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_SoupBinFramerDecodePartial);

// ---------------------------------------------------------------------------
// OUCH AcceptedView — every field accessor in the realistic post-ack
// processing path (extract symbol + side + qty + price + state + token).
// ---------------------------------------------------------------------------

static void BM_OuchAcceptedConstruct(benchmark::State& state) {
    auto wire = build_accepted();
    for (auto _ : state) {
        AcceptedView v{wire.data(), wire.size()};
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_OuchAcceptedConstruct);

static void BM_OuchAcceptedFullAccess(benchmark::State& state) {
    auto wire = build_accepted();
    for (auto _ : state) {
        AcceptedView v{wire.data(), wire.size()};
        // Touch every primitive accessor — realistic shape of an
        // order-management on-accepted handler that fans out to risk +
        // position + audit log.
        benchmark::DoNotOptimize(v.msg_type());
        benchmark::DoNotOptimize(v.timestamp());
        benchmark::DoNotOptimize(v.token());
        benchmark::DoNotOptimize(v.side());
        benchmark::DoNotOptimize(v.shares());
        benchmark::DoNotOptimize(v.symbol());
        benchmark::DoNotOptimize(v.price());
        benchmark::DoNotOptimize(v.time_in_force());
        benchmark::DoNotOptimize(v.firm());
        benchmark::DoNotOptimize(v.display());
        benchmark::DoNotOptimize(v.order_reference());
        benchmark::DoNotOptimize(v.capacity());
        benchmark::DoNotOptimize(v.int_mkt_sweep());
        benchmark::DoNotOptimize(v.cross_type());
        benchmark::DoNotOptimize(v.order_state());
        benchmark::DoNotOptimize(v.bbo_weight());
    }
}
BENCHMARK(BM_OuchAcceptedFullAccess);

// ---------------------------------------------------------------------------
// OUCH ExecutedView — the per-fill hot path
// ---------------------------------------------------------------------------

static void BM_OuchExecutedConstruct(benchmark::State& state) {
    auto wire = build_executed();
    for (auto _ : state) {
        ExecutedView v{wire.data(), wire.size()};
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_OuchExecutedConstruct);

static void BM_OuchExecutedFullAccess(benchmark::State& state) {
    auto wire = build_executed();
    for (auto _ : state) {
        ExecutedView v{wire.data(), wire.size()};
        benchmark::DoNotOptimize(v.msg_type());
        benchmark::DoNotOptimize(v.timestamp());
        benchmark::DoNotOptimize(v.token());
        benchmark::DoNotOptimize(v.executed_shares());
        benchmark::DoNotOptimize(v.execution_price());
        benchmark::DoNotOptimize(v.liquidity_flag());
        benchmark::DoNotOptimize(v.match_number());
    }
}
BENCHMARK(BM_OuchExecutedFullAccess);

// ---------------------------------------------------------------------------
// OUCH inbound builders — the order-entry hot path.
//
// EnterOrder ('O'), ReplaceOrder ('U'), and CancelOrder ('X') are
// the three client → exchange messages an HFT order entry router
// emits. Every order, modification, and cancel goes through one of
// these `build()` functions before SoupBinTcpFramer wraps them and
// the kernel/DPDK stream sends them out. The earlier per-batch sweep
// added comprehensive correctness tests for these builders (sentinel
// rejection, oversize-token rejection, exact-boundary acceptance) but
// no microbench measured the encode cost itself. Pin it so a future
// refactor that adds runtime validation, error-path branching, or
// allocates anywhere accidentally surfaces in the bench output.
// ---------------------------------------------------------------------------

static void BM_OuchBuildEnterOrder(benchmark::State& state) {
    std::array<uint8_t, ob::EnterOrder::kSize> buf{};
    for (auto _ : state) {
        size_t n = ob::EnterOrder::build(
            buf.data(),
            "ABCDEFGHIJKLMN",  // 14-char token (max length)
            'B',               // buy
            100,               // shares
            "AAPL",            // symbol (right-padded to 8)
            1500000,           // $150.00 wire format
            0,                 // day order
            "FIRM");           // 4-char MPID
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OuchBuildEnterOrder);

static void BM_OuchBuildReplaceOrder(benchmark::State& state) {
    std::array<uint8_t, ob::ReplaceOrder::kSize> buf{};
    for (auto _ : state) {
        size_t n = ob::ReplaceOrder::build(
            buf.data(),
            "EXISTING_TOKN1",   // 14-char existing token
            "REPLACEMENT_T1",   // 14-char replacement
            200,                // new shares
            1525000,            // new price $152.50
            60);                // 60s TIF
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OuchBuildReplaceOrder);

static void BM_OuchBuildCancelOrder(benchmark::State& state) {
    std::array<uint8_t, ob::CancelOrder::kSize> buf{};
    for (auto _ : state) {
        size_t n = ob::CancelOrder::build(
            buf.data(),
            "CANCEL_TOKEN_1",   // 14-char token
            0);                 // 0 = cancel entire remaining
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OuchBuildCancelOrder);

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::off);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}

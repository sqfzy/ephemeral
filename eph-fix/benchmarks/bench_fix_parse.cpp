/// @file bench_fix_parse.cpp
/// FIX protocol benchmarks — parse, build, framer decode throughput.
///
/// Depends only on eph-fix (no DPDK required).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/fix.hpp"

using namespace eph::fix;

// ---------------------------------------------------------------------------
// Helpers: build valid FIX messages of varying complexity
// ---------------------------------------------------------------------------
namespace {

/// Build a simple NewOrderSingle (MsgType=D) with N body fields.
std::vector<uint8_t> build_new_order(size_t extra_fields = 0) {
    uint8_t buf[4096];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::SenderCompID, "SENDER01");
    b.set(tag::TargetCompID, "TARGET01");
    b.set_int(tag::MsgSeqNum, 42);
    b.set(tag::SendingTime, "20260322-12:00:00.000");
    b.set(tag::ClOrdID, "order-00001");
    b.set(tag::Symbol, "AAPL");
    b.set_int(tag::Side, 1);
    b.set_int(tag::OrderQty, 100);
    b.set_int(tag::OrdType, 2);
    b.set_double(tag::Price, 150.50);

    // Add extra fields to simulate larger messages
    for (size_t i = 0; i < extra_fields; ++i) {
        b.set_int(static_cast<uint32_t>(5000 + i),
                  static_cast<int64_t>(i * 100));
    }

    size_t len = b.finish("FIX.4.4");
    return {buf, buf + len};
}

/// Build a MarketDataSnapshot (MsgType=W) with repeating groups.
std::vector<uint8_t> build_market_data_snapshot() {
    uint8_t buf[4096];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "W");
    b.set(tag::SenderCompID, "FEED01");
    b.set(tag::TargetCompID, "CLIENT01");
    b.set_int(tag::MsgSeqNum, 1000);
    b.set(tag::SendingTime, "20260322-12:00:00.000");
    b.set(tag::MDReqID, "md-req-001");
    b.set(tag::Symbol, "TSLA");
    // Repeating group: 5 MD entries
    b.set_int(268, 5);  // NoMDEntries
    for (size_t i = 0; i < 5; ++i) {
        b.set_int(269, i % 2);  // MDEntryType: 0=Bid, 1=Offer
        b.set_double(270, 200.00 + i * 0.25, 2);  // MDEntryPx
        b.set_int(271, (i + 1) * 100);  // MDEntrySize
    }

    size_t len = b.finish("FIX.4.4");
    return {buf, buf + len};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Parse: single message
// ---------------------------------------------------------------------------

static void BM_FixParseNewOrder(benchmark::State& state) {
    auto msg = build_new_order();

    for (auto _ : state) {
        auto result = parse(msg.data(), msg.size());
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(msg.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParseNewOrder);

static void BM_FixParseMarketData(benchmark::State& state) {
    auto msg = build_market_data_snapshot();

    for (auto _ : state) {
        auto result = parse(msg.data(), msg.size());
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(msg.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParseMarketData);

// ---------------------------------------------------------------------------
// Parse: scaling with field count
// ---------------------------------------------------------------------------

static void BM_FixParseFieldCount(benchmark::State& state) {
    auto extra = static_cast<size_t>(state.range(0));
    auto msg = build_new_order(extra);

    for (auto _ : state) {
        auto result = parse(msg.data(), msg.size());
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(msg.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParseFieldCount)->Arg(0)->Arg(10)->Arg(50)->Arg(100);

// ---------------------------------------------------------------------------
// Field lookup after parse
// ---------------------------------------------------------------------------

static void BM_FixFieldLookup(benchmark::State& state) {
    auto msg = build_new_order();
    auto result = parse(msg.data(), msg.size());
    auto& view = *result;

    for (auto _ : state) {
        auto mt  = view.msg_type();
        auto sym = view.get(tag::Symbol);
        auto qty = view.get_int(tag::OrderQty);
        auto px  = view.get_double(tag::Price);
        benchmark::DoNotOptimize(mt);
        benchmark::DoNotOptimize(sym);
        benchmark::DoNotOptimize(qty);
        benchmark::DoNotOptimize(px);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixFieldLookup);

// ---------------------------------------------------------------------------
// Builder throughput
// ---------------------------------------------------------------------------

static void BM_FixBuildNewOrder(benchmark::State& state) {
    uint8_t buf[512];

    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "D");
        b.set(tag::SenderCompID, "SENDER01");
        b.set(tag::TargetCompID, "TARGET01");
        b.set_int(tag::MsgSeqNum, 42);
        b.set(tag::SendingTime, "20260322-12:00:00.000");
        b.set(tag::ClOrdID, "order-00001");
        b.set(tag::Symbol, "AAPL");
        b.set_int(tag::Side, 1);
        b.set_int(tag::OrderQty, 100);
        b.set_int(tag::OrdType, 2);
        b.set_double(tag::Price, 150.50);
        size_t len = b.finish("FIX.4.4");
        benchmark::DoNotOptimize(len);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixBuildNewOrder);

static void BM_FixBuildWithTimestamp(benchmark::State& state) {
    uint8_t buf[512];
    uint64_t epoch_ns = 1774191045'123'456'789ULL;

    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "D");
        b.set(tag::SenderCompID, "SENDER01");
        b.set(tag::TargetCompID, "TARGET01");
        b.set_int(tag::MsgSeqNum, 42);
        b.set_timestamp(tag::SendingTime, epoch_ns,
                        MessageBuilder::TimestampPrecision::kMicroseconds);
        b.set(tag::ClOrdID, "order-00001");
        b.set(tag::Symbol, "AAPL");
        b.set_int(tag::Side, 1);
        b.set_int(tag::OrderQty, 100);
        b.set_int(tag::OrdType, 2);
        b.set_double(tag::Price, 150.50);
        size_t len = b.finish("FIX.4.4");
        benchmark::DoNotOptimize(len);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixBuildWithTimestamp);

static void BM_FixBuildMarketData(benchmark::State& state) {
    uint8_t buf[2048];

    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "W");
        b.set(tag::SenderCompID, "FEED01");
        b.set(tag::TargetCompID, "CLIENT01");
        b.set_int(tag::MsgSeqNum, 1000);
        b.set(tag::SendingTime, "20260322-12:00:00.000");
        b.set(tag::MDReqID, "md-req-001");
        b.set(tag::Symbol, "TSLA");
        b.set_int(tag::NoMDEntries, 5);
        for (int i = 0; i < 5; ++i) {
            b.set_int(tag::MDEntryType, i % 2);
            b.set_double(tag::MDEntryPx, 200.00 + i * 0.25, 2);
            b.set_int(tag::MDEntrySize, (i + 1) * 100);
        }
        size_t len = b.finish("FIX.4.4");
        benchmark::DoNotOptimize(len);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixBuildMarketData);

static void BM_FixBuildResetReuse(benchmark::State& state) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));

    for (auto _ : state) {
        b.reset();
        b.set(tag::MsgType, "0"); // Heartbeat — minimal message
        b.set(tag::SenderCompID, "SENDER01");
        b.set(tag::TargetCompID, "TARGET01");
        b.set_int(tag::MsgSeqNum, 42);
        size_t len = b.finish("FIX.4.4");
        benchmark::DoNotOptimize(len);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixBuildResetReuse);

// ---------------------------------------------------------------------------
// Framer decode
// ---------------------------------------------------------------------------

static void BM_FixFramerDecode(benchmark::State& state) {
    auto msg = build_new_order();

    for (auto _ : state) {
        auto result = FixFramer{}.decode(msg.data(), msg.size());
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(msg.size()));
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixFramerDecode);

// ---------------------------------------------------------------------------
// Checksum computation
// ---------------------------------------------------------------------------

static void BM_FixChecksum(benchmark::State& state) {
    auto msg = build_new_order();

    for (auto _ : state) {
        bool ok = verify_checksum(msg.data(), msg.size());
        benchmark::DoNotOptimize(ok);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(msg.size()));
}
BENCHMARK(BM_FixChecksum);

// ---------------------------------------------------------------------------
// Build + parse roundtrip
// ---------------------------------------------------------------------------

static void BM_FixBuildParseRoundtrip(benchmark::State& state) {
    uint8_t buf[512];

    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "D");
        b.set(tag::SenderCompID, "SENDER");
        b.set(tag::Symbol, "AAPL");
        b.set_int(tag::Side, 1);
        b.set_int(tag::OrderQty, 100);
        b.set_double(tag::Price, 150.50);
        size_t len = b.finish("FIX.4.4");

        auto result = parse(buf, len);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixBuildParseRoundtrip);

BENCHMARK_MAIN();

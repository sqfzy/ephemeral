/// @file bench_fix_parse.cpp
/// FIX protocol benchmarks — parse, build, framer decode throughput.
///
/// Depends only on eph-fix (no DPDK required).

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/fix.hpp"

using namespace eph::fix;

namespace {
/// Silence WARN-level reject paths so reject-branch logs do not poison
/// the bench timing (set_double NaN-reject in particular).
struct LoggerSilencer {
    LoggerSilencer() { spdlog::set_level(spdlog::level::off); }
};
const LoggerSilencer g_silencer{};
}  // namespace

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

// ---------------------------------------------------------------------------
// parse_timestamp_value — isolated hot-path microbench.
//
// Every inbound FIX message header carries a SendingTime that the
// parser feeds through parse_timestamp_value (and execution-report
// readers do the same for TransactTime). Both call sites are on the
// per-message hot path: a steady-state market-data feed dispatching
// 1M msg/s also dispatches 1M timestamp parses/s, and the function
// internally walks the civil-date math (~50 LoC of integer arithmetic
// + the kMaxEpochSec overflow guard). Until now the only visibility
// into its cost was implicit inside BM_FixParseNewOrder, where it
// shares the budget with tag iteration / checksum / framing — making
// any future regression in the date math (e.g. a poorly inlined
// helper, a non-noexcept exception path) invisible at the benchmark
// level.
//
// The 4 valid FIX 4.4 frac shapes get distinct benches: a
// regression specific to one precision (e.g. the 9-digit ns extension
// growing a heap allocation) would otherwise hide behind the average.
// ---------------------------------------------------------------------------

static void BM_FixParseTimestampSeconds(benchmark::State& state) {
    // 17 chars — no fractional seconds.
    constexpr std::string_view ts = "20260322-12:00:00";
    for (auto _ : state) {
        auto r = eph::fix::detail::parse_timestamp_value(ts);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParseTimestampSeconds);

static void BM_FixParseTimestampMillis(benchmark::State& state) {
    // 21 chars — millisecond precision.
    constexpr std::string_view ts = "20260322-12:00:00.123";
    for (auto _ : state) {
        auto r = eph::fix::detail::parse_timestamp_value(ts);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParseTimestampMillis);

static void BM_FixParseTimestampMicros(benchmark::State& state) {
    // 24 chars — microsecond precision (modern CME / ICE feeds).
    constexpr std::string_view ts = "20260322-12:00:00.123456";
    for (auto _ : state) {
        auto r = eph::fix::detail::parse_timestamp_value(ts);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParseTimestampMicros);

static void BM_FixParseTimestampNanos(benchmark::State& state) {
    // 27 chars — nanosecond precision.
    constexpr std::string_view ts = "20260322-12:00:00.123456789";
    for (auto _ : state) {
        auto r = eph::fix::detail::parse_timestamp_value(ts);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParseTimestampNanos);

// Reject path — guards against a regression where the early
// length-rejection becomes more expensive (e.g. someone replaces the
// constexpr length compare with a runtime function call). The reject
// path is also on the hot path: an adversarial / malformed wire feed
// can produce arbitrary lengths, and the parser must reject them
// quickly to avoid amplifying an attack into measurable downtime.
static void BM_FixParseTimestampInvalidLength(benchmark::State& state) {
    // 23 chars — neither 17/21/24/27, must early-reject.
    constexpr std::string_view ts = "20260322-12:00:00.12345";
    for (auto _ : state) {
        auto r = eph::fix::detail::parse_timestamp_value(ts);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FixParseTimestampInvalidLength);

// ---------------------------------------------------------------------------
// Isolated builder primitive benchmarks (loop-cleanup R168)
//
// `set_int` / `set_double` / `set_decimal` / `set_price` / `set_bool` are the
// per-field encoding primitives invoked once for every typed field in every
// outbound FIX message. Existing benches embed them inside larger
// `BM_FixBuild*` flows but never measure them in isolation — so a regression
// in the integer formatter, the `format_double` path, the `set_decimal`
// validator, or the `set_price` mantissa/decimal expansion would only
// surface as a small drift in the multi-field flow benchmarks where it can
// hide behind noise. Pinning each primitive at a fixed cost makes
// regressions immediately attributable.
// ---------------------------------------------------------------------------

static void BM_BuilderSetIntSmall(benchmark::State& state) {
    uint8_t buf[256];
    int64_t v = 0;
    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set_int(tag::MsgSeqNum, ++v & 0xFFFF);  // 1-5 digit values
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BuilderSetIntSmall);

static void BM_BuilderSetIntLarge(benchmark::State& state) {
    // Wide-range value to exercise the full format_int loop, not the
    // small-integer fast path.
    uint8_t buf[256];
    int64_t v = 1'234'567'890'123LL;
    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set_int(tag::OrderQty, v);
        v = (v + 1) % 1'234'567'890'124LL;
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BuilderSetIntLarge);

static void BM_BuilderSetDouble(benchmark::State& state) {
    uint8_t buf[256];
    double v = 100.50;
    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set_double(tag::Price, v, 2);
        v += 0.01;
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BuilderSetDouble);

// set_double's reject branch — non-finite value short-circuits before
// `format_double`. Pin the early-exit cost so a future change that adds
// extra logging or validation higher in the function cannot regress
// the cheap-reject case unnoticed.
static void BM_BuilderSetDoubleRejectNaN(benchmark::State& state) {
    uint8_t buf[256];
    const double nan_v = std::numeric_limits<double>::quiet_NaN();
    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set_double(tag::Price, nan_v, 2);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BuilderSetDoubleRejectNaN);

static void BM_BuilderSetDecimal(benchmark::State& state) {
    uint8_t buf[256];
    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        // Realistic price string that exercises the validation loop
        // (sign, digit run, dot, fractional digits).
        b.set_decimal(tag::Price, "12345.6789");
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BuilderSetDecimal);

static void BM_BuilderSetPrice(benchmark::State& state) {
    uint8_t buf[256];
    int64_t mantissa = 12345678;
    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set_price(tag::Price, ++mantissa, /*decimals=*/4);
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BuilderSetPrice);

static void BM_BuilderSetBool(benchmark::State& state) {
    uint8_t buf[256];
    bool flip = false;
    for (auto _ : state) {
        MessageBuilder b(buf, sizeof(buf));
        b.set_bool(tag::PossDupFlag, flip);
        flip = !flip;
        benchmark::DoNotOptimize(buf);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BuilderSetBool);

BENCHMARK_MAIN();

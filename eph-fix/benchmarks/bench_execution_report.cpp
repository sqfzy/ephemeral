/// @file bench_execution_report.cpp
/// Microbenchmarks for `eph::fix::ExecutionReportView` and
/// `try_parse_execution_report`.
///
/// Coverage gap (batch 11): bench_fix_parse covers generic FIX parse +
/// build but not the ExecutionReport-specific accessors. Every fill,
/// ack, cancel-confirm, and reject the OMS receives lands as an
/// ExecutionReport (MsgType=8) — so the per-message accessor cost is
/// directly on the order-management hot path.
///
/// Why these particular cases:
/// - **TryParseExecReport**: full parse + MsgType validation in one
///   call. The realistic shape of the receive loop's first dispatch.
/// - **ExecReportFullFieldAccess**: extract every primary OMS field
///   from a ParsedExecutionReport — id, exec_type, ord_status, side,
///   last_px, last_qty_d, cum_qty_d, leaves_qty, transact_time. This
///   is the `OrderManager::on_execution_report` call's typical access
///   pattern.
/// - **ExecReportRejectNonExecReport**: passing a non-'8' MsgType
///   message — the optional rejection path. Confirms the MsgType=8
///   guard is cheap (one tag lookup + one byte compare).

#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/fix.hpp"

using namespace eph::fix;

namespace {

/// Build a representative ExecutionReport (Partial Fill) FIX message.
[[nodiscard]] std::vector<uint8_t> build_partial_fill_er() {
    uint8_t buf[1024];
    MessageBuilder b{buf, sizeof(buf)};
    b.set(tag::MsgType, "8");
    b.set(tag::SenderCompID, "EXCH01");
    b.set(tag::TargetCompID, "CLIENT01");
    b.set_int(tag::MsgSeqNum, 1234);
    b.set(tag::SendingTime, "20260322-15:30:00.123");
    b.set(tag::OrderID, "ORD-EXCH-00012345");
    b.set(tag::ClOrdID, "client-order-001");
    b.set(tag::ExecID, "EXEC-00098765");
    b.set_char(tag::ExecType, '1');     // Partial fill
    b.set_char(tag::OrdStatus, '1');    // Partially filled
    b.set(tag::Symbol, "AAPL");
    b.set_char(tag::Side, '1');         // Buy
    b.set_double(tag::LastPx,  150.25, 2);
    b.set_double(tag::LastQty, 50.0,   0);
    b.set_double(tag::AvgPx,   150.10, 2);
    b.set_double(tag::CumQty,  50.0,   0);
    b.set_double(tag::LeavesQty, 50.0, 0);
    b.set(tag::TransactTime, "20260322-15:30:00.120");
    const auto n = b.finish("FIX.4.4");
    return {buf, buf + n};
}

/// Build a non-ExecutionReport message — used to exercise the MsgType
/// rejection path of try_parse_execution_report.
[[nodiscard]] std::vector<uint8_t> build_new_order_d() {
    uint8_t buf[512];
    MessageBuilder b{buf, sizeof(buf)};
    b.set(tag::MsgType, "D");      // NewOrderSingle, not ER
    b.set(tag::SenderCompID, "CLIENT01");
    b.set(tag::TargetCompID, "EXCH01");
    b.set_int(tag::MsgSeqNum, 5);
    b.set(tag::SendingTime, "20260322-15:30:00.000");
    b.set(tag::ClOrdID, "abc");
    b.set(tag::Symbol, "AAPL");
    b.set_int(tag::Side, 1);
    b.set_int(tag::OrderQty, 100);
    b.set_int(tag::OrdType, 2);
    b.set_double(tag::Price, 150.50, 2);
    const auto n = b.finish("FIX.4.4");
    return {buf, buf + n};
}

} // namespace

// ---------------------------------------------------------------------------
// try_parse_execution_report — full parse + MsgType=8 validation
// ---------------------------------------------------------------------------

static void BM_TryParseExecReport(benchmark::State& state) {
    auto msg = build_partial_fill_er();
    for (auto _ : state) {
        auto r = try_parse_execution_report<256>(msg.data(), msg.size());
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(msg.size()));
}
BENCHMARK(BM_TryParseExecReport);

// ---------------------------------------------------------------------------
// ExecutionReportView field access — the OMS hot path
// ---------------------------------------------------------------------------

static void BM_ExecReportFullFieldAccess(benchmark::State& state) {
    auto msg = build_partial_fill_er();
    auto parsed = try_parse_execution_report<256>(msg.data(), msg.size());
    if (!parsed) state.SkipWithError("ER parse failed in setup");

    for (auto _ : state) {
        const auto& v = parsed->view;
        // Touch every field the OMS handler reads on a partial-fill path.
        benchmark::DoNotOptimize(v.cl_ord_id());
        benchmark::DoNotOptimize(v.order_id());
        benchmark::DoNotOptimize(v.exec_id());
        benchmark::DoNotOptimize(v.exec_type());
        benchmark::DoNotOptimize(v.ord_status());
        benchmark::DoNotOptimize(v.symbol());
        benchmark::DoNotOptimize(v.side());
        benchmark::DoNotOptimize(v.last_px());
        benchmark::DoNotOptimize(v.last_qty_d());
        benchmark::DoNotOptimize(v.avg_px());
        benchmark::DoNotOptimize(v.cum_qty_d());
    }
}
BENCHMARK(BM_ExecReportFullFieldAccess);

// ---------------------------------------------------------------------------
// MsgType rejection: pass a NewOrderSingle through and confirm the guard
// is cheap.
// ---------------------------------------------------------------------------

static void BM_ExecReportRejectNonER(benchmark::State& state) {
    auto msg = build_new_order_d();
    for (auto _ : state) {
        auto r = try_parse_execution_report<256>(msg.data(), msg.size());
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_ExecReportRejectNonER);

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::off);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}

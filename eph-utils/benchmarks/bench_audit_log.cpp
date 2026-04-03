/// @file bench_audit_log.cpp
/// Benchmarks for AuditLog — single-writer and multi-writer recording throughput.
/// AuditLog is on the order-entry hot path.

#include <cstdint>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/utils/audit_log.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// Single-writer recording
// ---------------------------------------------------------------------------

static void BM_AuditRecord_SingleWriter(benchmark::State& state) {
    AuditLog<65536> log;
    uint64_t oid = 0;
    for (auto _ : state) {
        log.record(AuditEvent::NewOrder, oid++, 50000.0, 1.5,
                   Side::Buy, 0);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AuditRecord_SingleWriter);

// Record with fill fields populated
static void BM_AuditRecord_WithFill(benchmark::State& state) {
    AuditLog<65536> log;
    uint64_t oid = 0;
    for (auto _ : state) {
        log.record(AuditEvent::Fill, oid++, 50000.0, 1.5,
                   Side::Buy, 0, 50001.0, 1.5);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AuditRecord_WithFill);

// ---------------------------------------------------------------------------
// Multi-writer recording (contended)
// ---------------------------------------------------------------------------

static void BM_AuditRecordMt(benchmark::State& state) {
    static AuditLog<65536> log;
    uint64_t oid = state.thread_index() * 10'000'000;
    for (auto _ : state) {
        log.record_mt(AuditEvent::NewOrder, oid++, 50000.0, 1.5,
                      Side::Buy, 0);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AuditRecordMt)->Threads(1)->Threads(2)->Threads(4);

// ---------------------------------------------------------------------------
// Read access (at/latest)
// ---------------------------------------------------------------------------

static void BM_AuditLatest(benchmark::State& state) {
    AuditLog<65536> log;
    // Fill with some data
    for (uint64_t i = 0; i < 1000; ++i) {
        log.record(AuditEvent::NewOrder, i, 50000.0, 1.5, Side::Buy, 0);
    }
    for (auto _ : state) {
        auto* e = log.latest();
        benchmark::DoNotOptimize(e);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AuditLatest);

BENCHMARK_MAIN();

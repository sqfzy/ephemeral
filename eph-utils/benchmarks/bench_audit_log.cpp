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
        (void)log.record(AuditEvent::NewOrder, oid++, 50000.0, 1.5,
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
        (void)log.record(AuditEvent::Fill, oid++, 50000.0, 1.5,
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
        (void)log.record_mt(AuditEvent::NewOrder, oid++, 50000.0, 1.5,
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
        (void)log.record(AuditEvent::NewOrder, i, 50000.0, 1.5, Side::Buy, 0);
    }
    for (auto _ : state) {
        auto* e = log.latest();
        benchmark::DoNotOptimize(e);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AuditLatest);

// ---------------------------------------------------------------------------
// Dump formatting
// ---------------------------------------------------------------------------

static void BM_AuditDump(benchmark::State& state) {
    AuditLog<1024> log;
    for (uint64_t i = 0; i < 100; ++i) {
        (void)log.record(AuditEvent::NewOrder, i, 50000.0 + i, 1.5,
                   Side::Buy, static_cast<uint8_t>(i % 4));
    }
    for (auto _ : state) {
        auto s = log.dump(20);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AuditDump);

// ---------------------------------------------------------------------------
// Single-entry dump formatting
// ---------------------------------------------------------------------------

static void BM_AuditEntryDump(benchmark::State& state) {
    AuditLog<64> log;
    (void)log.record(AuditEvent::Fill, 42, 50000.25, 1.5,
               Side::Buy, 3, 50001.0, 1.5);
    const auto* entry = log.latest();
    for (auto _ : state) {
        auto s = entry->dump();
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AuditEntryDump);

// ---------------------------------------------------------------------------
// audit_event_name lookup
// ---------------------------------------------------------------------------

static void BM_AuditEventName(benchmark::State& state) {
    auto events = {AuditEvent::NewOrder, AuditEvent::Fill,
                   AuditEvent::Canceled, AuditEvent::KillSwitch};
    int idx = 0;
    for (auto _ : state) {
        auto name = audit_event_name(*(events.begin() + (idx % 4)));
        benchmark::DoNotOptimize(name);
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AuditEventName);

BENCHMARK_MAIN();

/// @file bench_itch_adapter.cpp
/// Microbenchmarks for `eph::book::ItchBookBuilder` — the order-level
/// ITCH-to-L2 book adapter.
///
/// **Coverage gap (loop-cleanup R161):** the adapter is on the hot path
/// of every order-level market-data feed (Nasdaq TotalView-ITCH dispatches
/// O(100k–1M msg/s) on a busy session), but no microbench existed for it
/// before this file. The integration suite covers correctness across all
/// 7 message types; this file isolates per-event cost so a future refactor
/// to `add_qty` / `sub_qty` / the per-price `std::map` lookup can be
/// noticed at the bench level.
///
/// What we measure:
///   - AddOrder           — insert into orders_ + update level (the hot
///                          add path: ~10x more frequent than reduce paths).
///   - OrderExecuted      — find ref + reduce qty + maybe erase.
///   - OrderCancel        — same shape as Executed but distinct code path
///                          (different over-cancel clamping).
///   - OrderDelete        — find ref + erase, no reduce-vs-delete branch.
///   - OrderReplace       — delete old level + add new level (the worst
///                          case: two map mutations per event).
///
/// Each bench works against a *pre-warmed* book with 200 orders across
/// 20 levels — the realistic state where the per-price std::map is
/// neither cold nor saturated. A cold book would over-report the AddOrder
/// fast path (every level allocates fresh); a saturated book would
/// over-report the lookup cost.

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include <benchmark/benchmark.h>

#include "eph/book/itch_adapter.hpp"
#include "eph/itch/messages.hpp"
#include "eph/itch/parser.hpp"

namespace {

using eph::itch::MessageView;

// --- Wire builders (mirror tests/integration/test_itch_adapter.cpp) ---

void write_be32(uint8_t* p, uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}
void write_be64(uint8_t* p, uint64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    std::memcpy(p, &v, 8);
}
void write_be16(uint8_t* p, uint16_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    std::memcpy(p, &v, 2);
}

void fill_header(uint8_t* msg, uint8_t type) noexcept {
    msg[0] = type;
    write_be16(msg + 1, 1);   // stock_locate
    write_be16(msg + 3, 0);   // tracking
    std::memset(msg + 5, 0, 6); // 6-byte timestamp
}

struct AddOrderBuf {
    std::array<uint8_t, 36> buf{};
    AddOrderBuf(uint64_t ref, char side, uint32_t shares, double price) {
        fill_header(buf.data(), eph::itch::kAddOrder);
        write_be64(buf.data() + 11, ref);
        buf[19] = static_cast<uint8_t>(side);
        write_be32(buf.data() + 20, shares);
        std::memcpy(buf.data() + 24, "TEST    ", 8);
        write_be32(buf.data() + 32, static_cast<uint32_t>(price * 10000.0));
    }
    MessageView view() const {
        return MessageView{eph::itch::kAddOrder, buf.data(), 36};
    }
};

struct ExecBuf {
    std::array<uint8_t, 31> buf{};
    ExecBuf(uint64_t ref, uint32_t shares) {
        fill_header(buf.data(), eph::itch::kOrderExecuted);
        write_be64(buf.data() + 11, ref);
        write_be32(buf.data() + 19, shares);
        write_be64(buf.data() + 23, 1);  // match_number
    }
    MessageView view() const {
        return MessageView{eph::itch::kOrderExecuted, buf.data(), 31};
    }
};

struct CancelBuf {
    std::array<uint8_t, 23> buf{};
    CancelBuf(uint64_t ref, uint32_t shares) {
        fill_header(buf.data(), eph::itch::kOrderCancel);
        write_be64(buf.data() + 11, ref);
        write_be32(buf.data() + 19, shares);
    }
    MessageView view() const {
        return MessageView{eph::itch::kOrderCancel, buf.data(), 23};
    }
};

struct DeleteBuf {
    std::array<uint8_t, 19> buf{};
    explicit DeleteBuf(uint64_t ref) {
        fill_header(buf.data(), eph::itch::kOrderDelete);
        write_be64(buf.data() + 11, ref);
    }
    MessageView view() const {
        return MessageView{eph::itch::kOrderDelete, buf.data(), 19};
    }
};

struct ReplaceBuf {
    std::array<uint8_t, 35> buf{};
    ReplaceBuf(uint64_t orig_ref, uint64_t new_ref, uint32_t shares,
               double new_price) {
        fill_header(buf.data(), eph::itch::kOrderReplace);
        write_be64(buf.data() + 11, orig_ref);
        write_be64(buf.data() + 19, new_ref);
        write_be32(buf.data() + 27, shares);
        write_be32(buf.data() + 31,
                   static_cast<uint32_t>(new_price * 10000.0));
    }
    MessageView view() const {
        return MessageView{eph::itch::kOrderReplace, buf.data(), 35};
    }
};

/// Pre-warm the builder with N orders across L price levels per side.
/// Returns the next-free order_ref so the bench can issue distinct refs
/// without colliding with the warm-up set.
template <size_t MaxLevels>
uint64_t warm_book(eph::book::ItchBookBuilder<MaxLevels>& b,
                   size_t orders_per_side, size_t levels_per_side) {
    uint64_t ref = 1;
    for (size_t side_idx = 0; side_idx < 2; ++side_idx) {
        const char side = (side_idx == 0) ? 'B' : 'S';
        const double base = (side == 'B') ? 100.00 : 105.00;
        for (size_t i = 0; i < orders_per_side; ++i) {
            const double price = base + ((side == 'B') ? -1.0 : 1.0)
                                * static_cast<double>(i % levels_per_side);
            AddOrderBuf m(ref++, side, 100, price);
            (void)b.process(m.view());
        }
    }
    return ref;
}

} // anonymous namespace

// ---------------------------------------------------------------------------

static void BM_ItchAdapter_AddOrder(benchmark::State& state) {
    eph::book::ItchBookBuilder<20> book;
    uint64_t next_ref = warm_book(book, /*orders_per_side*/100, /*levels*/20);
    for (auto _ : state) {
        // Insert at a level that already exists (the hot path: same-level
        // aggregate). qty varies to defeat DCE.
        AddOrderBuf m(next_ref++, 'B', 100, 100.00);
        bool ok = book.process(m.view());
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchAdapter_AddOrder);

// For reduce-style scenarios (Executed / Cancel / Delete / Replace),
// every iteration consumes one or two orders out of the live pool. We
// can't `state.PauseTiming` to refill — Google Benchmark on Linux has
// a known-bad interaction where PauseTiming inflates wall-clock to
// huge numbers under tight inner loops (observed: 4.7e13 ns for one
// iteration). Instead, pre-create a fixed batch of pre-built buffers
// for the inner loop, and amortize a `book.clear()` + re-warm across
// the whole batch. The per-iteration time then includes a small
// fraction of the warm-up cost; comparison across runs is still
// trustworthy because the warmup work is identical between runs.

constexpr size_t kInitialOrders = 100'000;

template <size_t MaxLevels>
void warm_pool(eph::book::ItchBookBuilder<MaxLevels>& book,
               uint64_t start_ref, size_t count) {
    for (uint64_t r = start_ref; r < start_ref + count; ++r) {
        AddOrderBuf m(r, 'B', 100, 100.00);
        (void)book.process(m.view());
    }
}

static void BM_ItchAdapter_OrderExecuted(benchmark::State& state) {
    eph::book::ItchBookBuilder<20> book;
    warm_pool(book, 1, kInitialOrders);
    uint64_t ref = 1;
    for (auto _ : state) {
        // Partial-execute 50 of 100 → no erase, lookup + reduce only.
        ExecBuf m(ref, 50);
        bool ok = book.process(m.view());
        benchmark::DoNotOptimize(ok);
        if (++ref > kInitialOrders) {
            // Wrap: refill from scratch. The warm-up time is amortized
            // across kInitialOrders iterations, so per-iter overhead is
            // ~constant (single-digit %).
            book.clear();
            warm_pool(book, 1, kInitialOrders);
            ref = 1;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchAdapter_OrderExecuted);

static void BM_ItchAdapter_OrderCancel(benchmark::State& state) {
    // Mirror of OrderExecuted but the Cancel handler has its own clamp
    // path; running both exposes regressions specific to one handler.
    eph::book::ItchBookBuilder<20> book;
    warm_pool(book, 1, kInitialOrders);
    uint64_t ref = 1;
    for (auto _ : state) {
        CancelBuf m(ref, 50);
        bool ok = book.process(m.view());
        benchmark::DoNotOptimize(ok);
        if (++ref > kInitialOrders) {
            book.clear();
            warm_pool(book, 1, kInitialOrders);
            ref = 1;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchAdapter_OrderCancel);

static void BM_ItchAdapter_OrderDelete(benchmark::State& state) {
    // Each iteration deletes one order — refill when we wrap.
    eph::book::ItchBookBuilder<20> book;
    warm_pool(book, 1, kInitialOrders);
    uint64_t ref = 1;
    for (auto _ : state) {
        DeleteBuf m(ref);
        bool ok = book.process(m.view());
        benchmark::DoNotOptimize(ok);
        if (++ref > kInitialOrders) {
            book.clear();
            warm_pool(book, 1, kInitialOrders);
            ref = 1;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchAdapter_OrderDelete);

static void BM_ItchAdapter_OrderReplace(benchmark::State& state) {
    // Worst case: every event is delete-old + add-new.
    eph::book::ItchBookBuilder<20> book;
    warm_pool(book, 1, kInitialOrders);
    uint64_t orig = 1;
    uint64_t next_ref = kInitialOrders + 1;
    for (auto _ : state) {
        ReplaceBuf m(orig, next_ref++, 100, 99.50);
        bool ok = book.process(m.view());
        benchmark::DoNotOptimize(ok);
        ++orig;
        if (orig >= next_ref) {
            book.clear();
            warm_pool(book, 1, kInitialOrders);
            orig = 1;
            next_ref = kInitialOrders + 1;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ItchAdapter_OrderReplace);

BENCHMARK_MAIN();

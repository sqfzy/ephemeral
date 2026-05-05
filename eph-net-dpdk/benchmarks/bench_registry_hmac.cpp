/// @file bench_registry_hmac.cpp
/// Microbenchmark for the T2.3 cross-process registry HMAC primitives.
///
/// Measures the cold-path cost of every sign / verify / audit
/// operation introduced by the T2.3 series (commits b775310b..d44d7b22).
/// The CHANGELOG entries cite "~150-300 ns/aarch64" estimates for the
/// per-call cost; this binary produces the actual measured numbers
/// suitable for archive at `.artifacts/bench-registry-hmac-*.txt` and
/// future perf-budget reference.
///
/// Why these matter: the T2.3 wiring decided to put HMAC entirely
/// off the hot path (lookups don't verify; signing happens at
/// publish; auditing happens at 1 Hz on a control thread). That
/// design depended on each individual primitive being "cold-path
/// affordable" — i.e. cheap enough that 1 Hz sweeping a 1024-entry
/// directory or signing every slot at register doesn't blow the
/// budget. This bench validates those assumptions empirically.
///
/// Coverage:
///   - MpRegistry      sign/verify on 56-byte slot payload
///   - QueueAllocator  sign/verify on 2088-byte header payload (largest)
///   - IcmpDirectory   sign/verify on 16-byte entry payload (smallest)
///   - Audit cycles    full-sweep cost for 64-batch + 1024-entry directory
///
/// Run on EC2 aarch64 Graviton (or any host with the eph toolchain).
/// Default invocation:
///   xmake run bench_registry_hmac
/// To pin to a specific NUMA node (T3.6):
///   EPH_BENCH_NUMA_NODE=0 xmake run bench_registry_hmac

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <benchmark/benchmark.h>

#include "bench_helpers.hpp"
#include "eph/dpdk/detail/icmp_directory.hpp"
#include "eph/dpdk/detail/mp_registry.hpp"
#include "eph/dpdk/detail/queue_allocator.hpp"
#include "eph/net/hmac.hpp"

namespace ed  = eph::dpdk::detail;
namespace qai = eph::dpdk::detail::queue_allocator_impl;

namespace {

// ProcSlot / IcmpDirectoryEntry contain non-copyable std::atomic
// fields, so factories must fill in place rather than return by value.
void fill_realistic_proc_slot(ed::ProcSlot& s) {
    s.claimed.store(1, std::memory_order_relaxed);
    std::memset(s.tag, 0, sizeof(s.tag));
    std::memcpy(s.tag, "trader-strat-A", 14);
    s.queue_lo   = 4;
    s.queue_hi   = 12;
    s.port_lo    = 32768;
    s.port_hi    = 40000;
    s.lcore_mask = 0x0000'0000'0000'0FF0ULL;
    s.pid        = 4242;
    std::memset(s.hmac_tag, 0, sizeof(s.hmac_tag));
}

void fill_realistic_icmp_entry(ed::IcmpDirectoryEntry& e, uint8_t i = 3) {
    e.claimed.store(ed::kIcmpSlotPublished, std::memory_order_relaxed);
    e.proto      = 6;  // TCP
    e.owner_proc = i;
    e.src_ip     = 0x0a000004u;
    e.dst_ip     = 0xc0a80105u;
    e.src_port   = static_cast<uint16_t>(35000 + i);
    e.dst_port   = 8080;
    e.generation.store(1, std::memory_order_relaxed);
    std::memset(e.hmac_tag, 0, sizeof(e.hmac_tag));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// MpRegistry — 56-byte authenticated payload
// ─────────────────────────────────────────────────────────────────────

static void BM_PackMpRegistrySlot(benchmark::State& state) {
    ed::ProcSlot slot{}; fill_realistic_proc_slot(slot);
    std::array<uint8_t, ed::kSlotAuthBytes> out{};
    for (auto _ : state) {
        ed::pack_slot_for_hmac(slot, out);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_PackMpRegistrySlot);

static void BM_SignMpRegistrySlot(benchmark::State& state) {
    ed::ProcSlot slot{}; fill_realistic_proc_slot(slot);
    eph::net::HmacSha256Key key{std::string_view{"bench-key-mp"}};
    for (auto _ : state) {
        ed::sign_slot_in_place(slot, key);
        benchmark::DoNotOptimize(slot.hmac_tag);
    }
}
BENCHMARK(BM_SignMpRegistrySlot);

static void BM_VerifyMpRegistrySlot(benchmark::State& state) {
    ed::ProcSlot slot{}; fill_realistic_proc_slot(slot);
    eph::net::HmacSha256Key key{std::string_view{"bench-key-mp"}};
    ed::sign_slot_in_place(slot, key);
    for (auto _ : state) {
        const bool ok = ed::verify_slot(slot, key);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_VerifyMpRegistrySlot);

// ─────────────────────────────────────────────────────────────────────
// QueueAllocator — 2088-byte authenticated payload (largest)
// ─────────────────────────────────────────────────────────────────────

static void BM_PackQueueAllocatorHeader(benchmark::State& state) {
    qai::Header hdr{};
    qai::init_header(&hdr, 64);
    // Populate some bitmap + claim_gen state so HMAC sees realistic
    // entropy not all-zero.
    qai::set_bit(hdr, 0);
    qai::set_bit(hdr, 5);
    qai::set_bit(hdr, 17);
    hdr.claim_gen[0] = 1;
    hdr.claim_gen[5] = 7;
    hdr.claim_gen[17] = 11;
    hdr.generation.store(11, std::memory_order_relaxed);

    std::array<uint8_t, qai::kHeaderAuthBytes> out{};
    for (auto _ : state) {
        qai::pack_header_for_hmac(hdr, out);
        benchmark::DoNotOptimize(out);
    }
    pthread_mutex_destroy(&hdr.mutex);
}
BENCHMARK(BM_PackQueueAllocatorHeader);

static void BM_SignQueueAllocator(benchmark::State& state) {
    qai::Header hdr{};
    qai::init_header(&hdr, 64);
    qai::set_bit(hdr, 0);
    qai::set_bit(hdr, 5);
    hdr.claim_gen[0] = 1;
    hdr.claim_gen[5] = 7;
    hdr.generation.store(7, std::memory_order_relaxed);
    eph::net::HmacSha256Key key{std::string_view{"bench-key-qa"}};
    for (auto _ : state) {
        qai::sign_header_in_place(hdr, key);
        benchmark::DoNotOptimize(hdr.hmac_tag);
    }
    pthread_mutex_destroy(&hdr.mutex);
}
BENCHMARK(BM_SignQueueAllocator);

static void BM_VerifyQueueAllocator(benchmark::State& state) {
    qai::Header hdr{};
    qai::init_header(&hdr, 64);
    qai::set_bit(hdr, 0);
    qai::set_bit(hdr, 5);
    hdr.claim_gen[0] = 1;
    hdr.claim_gen[5] = 7;
    hdr.generation.store(7, std::memory_order_relaxed);
    eph::net::HmacSha256Key key{std::string_view{"bench-key-qa"}};
    qai::sign_header_in_place(hdr, key);
    for (auto _ : state) {
        const bool ok = qai::verify_header(hdr, key);
        benchmark::DoNotOptimize(ok);
    }
    pthread_mutex_destroy(&hdr.mutex);
}
BENCHMARK(BM_VerifyQueueAllocator);

// ─────────────────────────────────────────────────────────────────────
// IcmpDirectory — 16-byte authenticated payload (smallest)
// ─────────────────────────────────────────────────────────────────────

static void BM_PackIcmpEntry(benchmark::State& state) {
    ed::IcmpDirectoryEntry e{}; fill_realistic_icmp_entry(e);
    std::array<uint8_t, ed::kIcmpEntryAuthBytes> out{};
    for (auto _ : state) {
        ed::pack_icmp_entry_for_hmac(e, out);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_PackIcmpEntry);

static void BM_SignIcmpEntry(benchmark::State& state) {
    ed::IcmpDirectoryEntry e{}; fill_realistic_icmp_entry(e);
    eph::net::HmacSha256Key key{std::string_view{"bench-key-icmp"}};
    for (auto _ : state) {
        ed::sign_icmp_entry_in_place(e, key);
        benchmark::DoNotOptimize(e.hmac_tag);
    }
}
BENCHMARK(BM_SignIcmpEntry);

static void BM_VerifyIcmpEntry(benchmark::State& state) {
    ed::IcmpDirectoryEntry e{}; fill_realistic_icmp_entry(e);
    eph::net::HmacSha256Key key{std::string_view{"bench-key-icmp"}};
    ed::sign_icmp_entry_in_place(e, key);
    for (auto _ : state) {
        const bool ok = ed::verify_icmp_entry(e, key);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_VerifyIcmpEntry);

// ─────────────────────────────────────────────────────────────────────
// Audit cycles — what 1 Hz sweep / nicctl audit actually pay
// ─────────────────────────────────────────────────────────────────────

// 64 published entries × verify — typical 1 Hz sweep tick payload.
// Caller is `IcmpDirectoryHandle::audit_sweep_one_round(64)` — this
// version measures the verify loop in isolation (no class
// indirection).
static void BM_AuditSweepBatch64(benchmark::State& state) {
    eph::net::HmacSha256Key key{std::string_view{"bench-key-sweep"}};
    std::array<ed::IcmpDirectoryEntry, 64> entries{};
    for (size_t i = 0; i < entries.size(); ++i) {
        fill_realistic_icmp_entry(entries[i],
                                  static_cast<uint8_t>(i & 0xFF));
        entries[i].src_port =
            static_cast<uint16_t>(35000 + i);
        ed::sign_icmp_entry_in_place(entries[i], key);
    }
    for (auto _ : state) {
        size_t mismatches = 0;
        for (auto& e : entries) {
            if (!ed::verify_icmp_entry(e, key)) ++mismatches;
        }
        benchmark::DoNotOptimize(mismatches);
    }
}
BENCHMARK(BM_AuditSweepBatch64);

// Full 1024-entry sweep (entire directory in one batch — same shape
// as the nicctl audit thunk's "full sweep on demand"). Should be
// ~16× the BM_AuditSweepBatch64 number.
static void BM_AuditSweepFull1024(benchmark::State& state) {
    eph::net::HmacSha256Key key{std::string_view{"bench-key-full"}};
    std::array<ed::IcmpDirectoryEntry, 1024> entries{};
    for (size_t i = 0; i < entries.size(); ++i) {
        fill_realistic_icmp_entry(entries[i],
                                  static_cast<uint8_t>(i & 0xFF));
        entries[i].src_port =
            static_cast<uint16_t>(32768 + (i & 0xFFFF));
        ed::sign_icmp_entry_in_place(entries[i], key);
    }
    for (auto _ : state) {
        size_t mismatches = 0;
        for (auto& e : entries) {
            if (!ed::verify_icmp_entry(e, key)) ++mismatches;
        }
        benchmark::DoNotOptimize(mismatches);
    }
}
BENCHMARK(BM_AuditSweepFull1024);

// MpRegistry full audit — what `MpRegistryHandle::audit_all()` pays
// when called from `eph-nicctl audit`. Sized at 64 slots (the
// MpTopology::kMaxProcs cap).
static void BM_AuditAllMpRegistry64Slots(benchmark::State& state) {
    eph::net::HmacSha256Key key{std::string_view{"bench-key-all"}};
    std::array<ed::ProcSlot, 64> slots{};
    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        s.claimed.store(1, std::memory_order_relaxed);
        std::memset(s.tag, 0, sizeof(s.tag));
        std::memcpy(s.tag, "tenant", 6);
        s.queue_lo = static_cast<uint16_t>(i * 4);
        s.queue_hi = static_cast<uint16_t>(i * 4 + 4);
        s.port_lo  = 32768u + static_cast<uint32_t>(i * 256);
        s.port_hi  = 32768u + static_cast<uint32_t>((i + 1) * 256);
        s.lcore_mask = uint64_t{1} << (i & 63);
        s.pid = static_cast<int32_t>(1000 + i);
        ed::sign_slot_in_place(s, key);
    }
    for (auto _ : state) {
        size_t mismatches = 0;
        for (auto& s : slots) {
            if (s.claimed.load(std::memory_order_relaxed) == 0) continue;
            if (!ed::verify_slot(s, key)) ++mismatches;
        }
        benchmark::DoNotOptimize(mismatches);
    }
}
BENCHMARK(BM_AuditAllMpRegistry64Slots);

// Opt-in NUMA pin via `EPH_BENCH_NUMA_NODE` (T3.6). Single-socket
// hosts: env var ignored (no-op + warn). bench_registry_hmac runs
// purely in-CPU/cache (no DPDK PMD), so NUMA effects are minor —
// included for parity with the other benches.
int main(int argc, char** argv) {
    if (auto pinned = ::eph::dpdk::bench::apply_env_numa_pin()) {
        std::fprintf(stderr,
            "[bench_registry_hmac] pinned to NUMA node %d via "
            "EPH_BENCH_NUMA_NODE\n", *pinned);
    }
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}

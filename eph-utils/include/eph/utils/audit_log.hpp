#pragma once

/// @file audit_log.hpp
/// Structured audit trail for regulatory compliance (MiFID II / Reg NMS).
///
/// Every order-related event (submit, fill, cancel, reject) is recorded
/// with a nanosecond TSC timestamp into a fixed-size ring buffer. The
/// buffer is lock-free for single-writer use or spinlock-protected for
/// multi-writer scenarios.
///
/// Design priorities:
///   1. Zero allocation on hot path (fixed ring, no heap)
///   2. TSC timestamps for sub-microsecond accuracy
///   3. Trivially copyable entries for memcpy-safe persistence
///   4. Optional file flush for post-trade reporting
///
/// Usage:
///   eph::utils::AuditLog<8192> log;
///   log.record(AuditEvent::NewOrder, 12345, 50100.50, 1.5, Side::Buy, 0);
///   // ... later ...
///   log.flush_to_file("/var/log/audit/session_20260328.bin");

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/utils/time.hpp"

namespace eph::utils {

namespace detail {
/// @brief Lazily-initialized logger for the audit-log subsystem.
inline const std::shared_ptr<spdlog::logger>& audit_log_logger() {
    static auto l = [] {
        auto lg = spdlog::get("utils.audit_log");
        if (!lg) lg = spdlog::stdout_color_mt("utils.audit_log");
        return lg;
    }();
    return l;
}
} // namespace detail

/// Audit event types covering the full order lifecycle.
enum class AuditEvent : uint8_t {
    NewOrder       = 0,  ///< Order submitted to exchange
    Acknowledged   = 1,  ///< Exchange accepted (New)
    PartialFill    = 2,  ///< Partial execution
    Fill           = 3,  ///< Full execution
    CancelRequest  = 4,  ///< Cancel request sent
    Canceled       = 5,  ///< Cancel confirmed
    Rejected       = 6,  ///< Order rejected
    Amended        = 7,  ///< Order amended (price/qty change)
    SessionLogon   = 8,  ///< FIX session logon
    SessionLogout  = 9,  ///< FIX session logout
    ConnectionUp   = 10, ///< Transport connected
    ConnectionDown = 11, ///< Transport disconnected
    KillSwitch     = 12, ///< Emergency shutdown triggered
};

/// Human-readable name for audit events.
[[nodiscard]] inline constexpr std::string_view audit_event_name(AuditEvent e) noexcept {
    switch (e) {
    case AuditEvent::NewOrder:       return "NEW_ORDER";
    case AuditEvent::Acknowledged:   return "ACK";
    case AuditEvent::PartialFill:    return "PARTIAL_FILL";
    case AuditEvent::Fill:           return "FILL";
    case AuditEvent::CancelRequest:  return "CANCEL_REQ";
    case AuditEvent::Canceled:       return "CANCELED";
    case AuditEvent::Rejected:       return "REJECTED";
    case AuditEvent::Amended:        return "AMENDED";
    case AuditEvent::SessionLogon:   return "SESSION_LOGON";
    case AuditEvent::SessionLogout:  return "SESSION_LOGOUT";
    case AuditEvent::ConnectionUp:   return "CONN_UP";
    case AuditEvent::ConnectionDown: return "CONN_DOWN";
    case AuditEvent::KillSwitch:     return "KILL_SWITCH";
    }
    return "UNKNOWN";
}

/// Order side.
enum class Side : uint8_t {
    Buy  = 1,
    Sell = 2,
};

/// A single audit trail entry — fixed 64 bytes for cache-line alignment.
///
/// Trivially copyable for zero-overhead ring buffer storage and
/// binary file persistence.
struct alignas(64) AuditEntry {
    uint64_t   tsc;          ///< TSC timestamp (convert via TSC::to_ns)
    uint64_t   order_id;     ///< Application-assigned order ID
    double     price;        ///< Order/execution price
    double     quantity;     ///< Order/execution quantity
    double     fill_price;   ///< Execution price (for fills; 0 otherwise)
    double     fill_qty;     ///< Execution quantity (for fills; 0 otherwise)
    AuditEvent event;        ///< Event type
    Side       side;         ///< Buy or Sell
    uint8_t    venue_id;     ///< Venue identifier (application-defined)
    uint8_t    padding_[13]; ///< Pad to 64 bytes

    /// Format as a human-readable log line.
    [[nodiscard]] std::string dump() const {
        auto ns = TSC::to_ns(tsc);
        // Explicitly handle each Side enum value to avoid misrepresenting
        // uninitialized/zeroed entries (side == 0) as "SELL".
        auto side_str = [this]() -> std::string_view {
            switch (side) {
            case Side::Buy:  return "BUY";
            case Side::Sell: return "SELL";
            }
            return "???";
        }();
        return std::format("[{:>12}ns] {:12s} oid={} side={} px={:.4f} qty={:.4f}"
                           " fill_px={:.4f} fill_qty={:.4f} venue={}",
            ns.value_or(0), audit_event_name(event), order_id,
            side_str,
            price, quantity, fill_price, fill_qty, venue_id);
    }
};

static_assert(sizeof(AuditEntry) == 64, "AuditEntry must be 64 bytes (cache-line)");
static_assert(std::is_trivially_copyable_v<AuditEntry>, "AuditEntry must be trivially copyable");

/// Fixed-size ring buffer audit trail.
///
/// Single-writer: use record() from one thread (no synchronization needed).
/// Multi-writer: use record_mt() which uses a CAS spinloop on the write index.
///
/// @tparam Capacity Number of entries in the ring (must be power of 2).
///         Default 65536 entries × 64 bytes = 4MB.
template <size_t Capacity = 65536>
    requires (std::has_single_bit(Capacity))
class AuditLog {
public:
    static constexpr size_t capacity = Capacity;
    static constexpr size_t kMask = Capacity - 1;

    AuditLog() noexcept {
        // All committed flags are default-initialized to false via
        // value-initialization of the array, but be explicit for clarity.
        for (auto& c : committed_) {
            c.store(false, std::memory_order_relaxed);
        }
    }

    /// Record an audit event (single-writer, no synchronization).
    /// @return true if recorded without overwriting old data, false if the ring
    ///         buffer wrapped around (oldest entry was overwritten).
    /// @note NOT thread-safe. Must be called from a single writer thread only.
    ///       For concurrent writes, use record_mt() instead.
    [[nodiscard]] bool record(AuditEvent event, uint64_t order_id,
                double price, double quantity,
                Side side, uint8_t venue_id,
                double fill_price = 0.0, double fill_qty = 0.0) noexcept {
#ifndef NDEBUG
        {
            auto expected = std::thread::id{};
            auto current = std::this_thread::get_id();
            if (!record_owner_tid_.compare_exchange_strong(expected, current,
                    std::memory_order_relaxed) && expected != current) {
                assert(false && "AuditLog::record() called from multiple threads — use record_mt() instead");
            }
        }
#endif
        size_t idx = head_.load(std::memory_order_relaxed);
        bool overflowed = idx >= Capacity;
        // Log only on the exact wrap boundary to avoid flooding hot-path logs.
        // Subsequent overwrites are expected ring buffer behavior.
        if (idx == Capacity) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::audit_log_logger(),
                "AuditLog: ring buffer wrapped at capacity={}, "
                "oldest entries will be overwritten", Capacity);
        }
        size_t slot = idx & kMask;
        // Mark slot as in-progress so readers know this entry is being written.
        committed_[slot].store(false, std::memory_order_relaxed);
        auto& entry = entries_[slot];
        entry.tsc = TSC::now();
        entry.order_id = order_id;
        entry.price = price;
        entry.quantity = quantity;
        entry.fill_price = fill_price;
        entry.fill_qty = fill_qty;
        entry.event = event;
        entry.side = side;
        entry.venue_id = venue_id;
        std::memset(entry.padding_, 0, sizeof(entry.padding_));
        // Mark slot as fully written; readers acquire this to see consistent data.
        committed_[slot].store(true, std::memory_order_release);
        head_.store(idx + 1, std::memory_order_release);
        return !overflowed;
    }

    /// Record an audit event (multi-writer safe via atomic index reservation).
    /// @return true if recorded without overwriting old data, false if the ring
    ///         buffer wrapped around (oldest entry was overwritten).
    [[nodiscard]] bool record_mt(AuditEvent event, uint64_t order_id,
                   double price, double quantity,
                   Side side, uint8_t venue_id,
                   double fill_price = 0.0, double fill_qty = 0.0) noexcept {
        size_t idx = head_.fetch_add(1, std::memory_order_acq_rel);
        bool overflowed = idx >= Capacity;
        // Log only on the exact wrap boundary to avoid flooding hot-path logs.
        if (idx == Capacity) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::audit_log_logger(),
                "AuditLog: ring buffer wrapped at capacity={}, "
                "oldest entries will be overwritten", Capacity);
        }
        size_t slot = idx & kMask;
        // Mark slot as in-progress so readers see a torn write as uncommitted.
        committed_[slot].store(false, std::memory_order_relaxed);
        auto& entry = entries_[slot];
        entry.tsc = TSC::now();
        entry.order_id = order_id;
        entry.price = price;
        entry.quantity = quantity;
        entry.fill_price = fill_price;
        entry.fill_qty = fill_qty;
        entry.event = event;
        entry.side = side;
        entry.venue_id = venue_id;
        std::memset(entry.padding_, 0, sizeof(entry.padding_));
        // Signal that the slot is fully written; readers acquire this flag.
        committed_[slot].store(true, std::memory_order_release);
        return !overflowed;
    }

    /// Number of entries recorded (may exceed Capacity — wraps around).
    [[nodiscard]] size_t total_count() const noexcept {
        return head_.load(std::memory_order_acquire);
    }

    /// Number of entries currently in the ring (min of total_count, Capacity).
    [[nodiscard]] size_t count() const noexcept {
        size_t total = total_count();
        return total < Capacity ? total : Capacity;
    }

    /// Access entry at offset from newest (0 = most recent).
    /// Returns nullptr if offset >= count() or if the entry has not been
    /// fully committed yet (i.e., a concurrent writer is mid-write).
    [[nodiscard]] const AuditEntry* at(size_t offset) const noexcept {
        // Load head_ once to avoid TOCTOU race between count check and index calc.
        size_t head = head_.load(std::memory_order_acquire);
        size_t n = head < Capacity ? head : Capacity;
        if (offset >= n) return nullptr;
        size_t idx = (head - 1 - offset) & kMask;
        // Check that the slot has been fully written — a concurrent writer
        // may have reserved the index but not finished populating the entry.
        if (!committed_[idx].load(std::memory_order_acquire)) return nullptr;
        return &entries_[idx];
    }

    /// Most recent entry, or nullptr if empty.
    [[nodiscard]] const AuditEntry* latest() const noexcept {
        return at(0);
    }

    /// Flush all entries to a binary file. Returns number of entries written.
    /// Entries are written newest-first.
    [[nodiscard]] size_t flush_to_file(std::string_view path) const noexcept {
        FILE* f = std::fopen(std::string(path).c_str(), "wb");
        if (!f) {
            SPDLOG_LOGGER_ERROR(detail::audit_log_logger(),"AuditLog: failed to open '{}' for writing", path);
            return 0;
        }

        size_t n = count();
        size_t written = 0;
        for (size_t i = 0; i < n; ++i) {
            const auto* e = at(i);
            if (e && std::fwrite(e, sizeof(AuditEntry), 1, f) == 1) {
                ++written;
            }
        }

        std::fclose(f);
        SPDLOG_LOGGER_INFO(detail::audit_log_logger(),"AuditLog: flushed {} entries to '{}'", written, path);
        return written;
    }

    /// Dump the last N entries to a formatted string for logging/debugging.
    [[nodiscard]] std::string dump(size_t max_entries = 20) const {
        size_t n = std::min(max_entries, count());
        std::string result = std::format("AuditLog: {} entries (showing {})\n", count(), n);
        for (size_t i = 0; i < n; ++i) {
            const auto* e = at(i);
            if (e) result += e->dump() + "\n";
        }
        return result;
    }

private:
    std::array<AuditEntry, Capacity> entries_{};
    /// Per-slot write-completion flag. A reader must acquire-load this before
    /// reading the corresponding entry to avoid observing a partially-written
    /// slot (data race when the ring buffer wraps under concurrent writers).
    std::array<std::atomic<bool>, Capacity> committed_{};
    std::atomic<size_t> head_{0};

#ifndef NDEBUG
    /// Thread ownership guard for single-writer record(). Stores the TID of
    /// the first caller; subsequent calls from a different thread trigger an
    /// assert, catching accidental cross-thread misuse early.
    std::atomic<std::thread::id> record_owner_tid_{};
#endif
};

} // namespace eph::utils

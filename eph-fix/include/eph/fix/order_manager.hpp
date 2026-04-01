#pragma once

/// @file order_manager.hpp
/// Order lifecycle manager for HFT systems.
///
/// Tracks orders from submission through fills/cancels, with optional
/// integration with PositionTracker for automatic position updates on fills.
/// Header-only, single-threaded (callers serialize externally).

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/fix/execution_report.hpp"
#include "eph/fix/position.hpp"

namespace eph::fix {

/// Order lifecycle state.
enum class OrderState : uint8_t {
    PendingNew,      ///< Submitted, awaiting exchange ack.
    New,             ///< Exchange accepted.
    PartiallyFilled, ///< Some quantity executed.
    Filled,          ///< Fully executed (terminal).
    PendingCancel,   ///< Cancel requested, awaiting ack.
    Canceled,        ///< Canceled (terminal).
    Rejected,        ///< Exchange rejected (terminal).
};

/// Returns true if the state is terminal (no further transitions expected).
[[nodiscard]] inline constexpr bool is_terminal(OrderState s) noexcept {
    switch (s) {
    case OrderState::Filled:
    case OrderState::Canceled:
    case OrderState::Rejected:
        return true;
    default:
        return false;
    }
}

/// Tracked order with full lifecycle metadata.
struct ManagedOrder {
    std::string cl_ord_id;
    std::string symbol;
    char        side           = '0';  ///< '1' buy, '2' sell.
    double      orig_qty       = 0.0;
    double      price          = 0.0;
    double      filled_qty     = 0.0;
    double      avg_fill_price = 0.0;
    double      leaves_qty     = 0.0;
    OrderState  state          = OrderState::PendingNew;
    uint64_t    submit_time_ns = 0;
    uint64_t    last_update_ns = 0;
};

namespace detail {
inline spdlog::logger* fix_ordmgr_logger() noexcept {
    static auto l = [] {
        auto lg = spdlog::get("fix.ordmgr");
        if (!lg) lg = spdlog::stdout_color_mt("fix.ordmgr");
        return lg;
    }();
    return l.get();
}
} // namespace detail

/// Manages the lifecycle of orders from submission through terminal state.
///
/// Thread-safety: none -- callers must serialize access externally.
/// Designed for single-threaded event processing pipelines.
class OrderManager {
public:
    /// Transparent hash for heterogeneous lookup (string_view key without allocation).
    struct StringHash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
        size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    /// Register a new order (call after send).
    /// Returns false if cl_ord_id already exists.
    bool submit(std::string cl_ord_id, std::string symbol,
                char side, double qty, double price) noexcept
    {
        if (cl_ord_id.empty()) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "submit: rejected empty cl_ord_id");
            return false;
        }

        auto [it, inserted] = orders_.try_emplace(cl_ord_id);
        if (!inserted) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "submit: duplicate cl_ord_id={}", cl_ord_id);
            return false;
        }

        auto& order      = it->second;
        order.cl_ord_id  = std::move(cl_ord_id);
        order.symbol     = std::move(symbol);
        order.side       = side;
        order.orig_qty   = qty;
        order.price      = price;
        order.leaves_qty = qty;
        order.state      = OrderState::PendingNew;

        SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
            "submit: cl_ord_id={} symbol={} side={} qty={} price={}",
            order.cl_ord_id, order.symbol, order.side, order.orig_qty, order.price);

        return true;
    }

    /// Process an ExecutionReport. Updates order state and optionally
    /// updates the position tracker on fills.
    /// Returns false if the order is unknown or missing required fields.
    template <size_t MaxFields = 256>
    bool on_execution_report(const ExecutionReportView<MaxFields>& report,
                             PositionTracker* positions = nullptr) noexcept
    {
        // Extract cl_ord_id -- required to locate the order.
        auto cl_id = report.cl_ord_id();
        if (!cl_id) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "on_execution_report: missing cl_ord_id, ignoring");
            return false;
        }

        auto it = orders_.find(*cl_id);
        if (it == orders_.end()) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "on_execution_report: unknown cl_ord_id={}", *cl_id);
            return false;
        }

        auto& order = it->second;

        // Don't update terminal orders -- they're done.
        if (is_terminal(order.state)) {
            SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
                "on_execution_report: cl_ord_id={} already terminal (state={}), ignoring",
                order.cl_ord_id, static_cast<int>(order.state));
            return true;
        }

        auto exec_type = report.exec_type();
        if (!exec_type) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "on_execution_report: cl_ord_id={} missing ExecType", *cl_id);
            return false;
        }

        switch (*exec_type) {
        case ExecType::New:
        case ExecType::PendingNew:
            order.state = OrderState::New;
            break;

        case ExecType::PartialFill:
        case ExecType::Trade: {
            // Update fill quantities from the report.
            // NOTE: int64_t → double conversion loses precision for quantities
            // exceeding 2^53 (≈9.007e15). Acceptable for all practical fill sizes.
            auto last_qty = report.last_qty();
            auto last_px  = report.last_px();

            if (last_qty && last_px) {
                double fill_qty   = static_cast<double>(*last_qty);
                double fill_price = *last_px;

                if (*last_qty > (1LL << 53)) {
                    SPDLOG_LOGGER_ERROR(detail::fix_ordmgr_logger(),
                        "fill quantity {} exceeds double precision limit (2^53), "
                        "rejecting fill for cl_ord_id={}", *last_qty, order.cl_ord_id);
                    return false;
                }

                // Compute new running average fill price.
                double old_total = order.avg_fill_price * order.filled_qty;
                order.filled_qty += fill_qty;
                if (order.filled_qty > 0.0) {
                    order.avg_fill_price =
                        (old_total + fill_price * fill_qty) / order.filled_qty;
                }
                if (!std::isfinite(order.avg_fill_price)) {
                    SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                        "avg_fill_price became non-finite for cl_ord_id={}, "
                        "resetting to last fill_price={}", order.cl_ord_id, fill_price);
                    order.avg_fill_price = fill_price;
                }
                order.leaves_qty = order.orig_qty - order.filled_qty;

                // Forward fill to position tracker if provided.
                if (positions) {
                    positions->on_fill(order.symbol, order.side,
                                       fill_qty, fill_price);
                }

                SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
                    "on_execution_report: partial fill cl_ord_id={} fill_qty={} "
                    "fill_price={} filled_qty={} leaves_qty={}",
                    order.cl_ord_id, fill_qty, fill_price,
                    order.filled_qty, order.leaves_qty);
            }

            order.state = OrderState::PartiallyFilled;
            break;
        }

        case ExecType::Fill: {
            // NOTE: int64_t → double conversion loses precision for quantities
            // exceeding 2^53 (≈9.007e15). Acceptable for all practical fill sizes.
            auto last_qty = report.last_qty();
            auto last_px  = report.last_px();

            if (last_qty && last_px) {
                double fill_qty   = static_cast<double>(*last_qty);
                double fill_price = *last_px;

                if (*last_qty > (1LL << 53)) {
                    SPDLOG_LOGGER_ERROR(detail::fix_ordmgr_logger(),
                        "fill quantity {} exceeds double precision limit (2^53), "
                        "rejecting fill for cl_ord_id={}", *last_qty, order.cl_ord_id);
                    return false;
                }

                double old_total = order.avg_fill_price * order.filled_qty;
                order.filled_qty += fill_qty;
                if (order.filled_qty > 0.0) {
                    order.avg_fill_price =
                        (old_total + fill_price * fill_qty) / order.filled_qty;
                }
                if (!std::isfinite(order.avg_fill_price)) {
                    SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                        "avg_fill_price became non-finite for cl_ord_id={}, "
                        "resetting to last fill_price={}", order.cl_ord_id, fill_price);
                    order.avg_fill_price = fill_price;
                }
                order.leaves_qty = 0.0;

                if (positions) {
                    positions->on_fill(order.symbol, order.side,
                                       fill_qty, fill_price);
                }

                SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
                    "on_execution_report: full fill cl_ord_id={} fill_qty={} "
                    "fill_price={} total_filled={}",
                    order.cl_ord_id, fill_qty, fill_price, order.filled_qty);
            }

            order.state = OrderState::Filled;
            break;
        }

        case ExecType::Canceled:
            order.state     = OrderState::Canceled;
            order.leaves_qty = 0.0;
            SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
                "on_execution_report: canceled cl_ord_id={}", order.cl_ord_id);
            break;

        case ExecType::PendingCancel:
            order.state = OrderState::PendingCancel;
            SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
                "on_execution_report: pending cancel cl_ord_id={}", order.cl_ord_id);
            break;

        case ExecType::Rejected:
            order.state     = OrderState::Rejected;
            order.leaves_qty = 0.0;
            SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
                "on_execution_report: rejected cl_ord_id={}", order.cl_ord_id);
            break;

        default:
            // Unhandled exec types (Replaced, Stopped, Suspended, etc.)
            // Log but don't change state -- caller can handle if needed.
            SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
                "on_execution_report: unhandled ExecType={} for cl_ord_id={}",
                static_cast<char>(*exec_type), order.cl_ord_id);
            break;
        }

        return true;
    }

    /// Handle an OrderCancelReject (MsgType=9).
    ///
    /// When the exchange rejects a cancel/replace request, the order remains
    /// in its previous state (reverted from PendingCancel back to its prior
    /// active state). This prevents the order from being stuck in PendingCancel
    /// indefinitely.
    ///
    /// @param cl_ord_id    ClOrdID of the rejected cancel request
    /// @param cxl_rej_reason  CxlRejReason (tag 102) value, or -1 if absent
    /// @return true if order was found and updated, false otherwise
    bool on_cancel_reject(std::string_view cl_ord_id,
                          int cxl_rej_reason = -1) noexcept
    {
        if (cl_ord_id.empty()) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "on_cancel_reject: missing cl_ord_id, ignoring");
            return false;
        }

        auto it = orders_.find(cl_ord_id);
        if (it == orders_.end()) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "on_cancel_reject: unknown cl_ord_id={}", cl_ord_id);
            return false;
        }

        auto& order = it->second;

        // Only revert PendingCancel — if already terminal or in another state,
        // the reject is informational only.
        if (order.state == OrderState::PendingCancel) {
            // Revert to appropriate active state based on fill status.
            order.state = (order.filled_qty > 0.0)
                ? OrderState::PartiallyFilled
                : OrderState::New;
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "on_cancel_reject: cl_ord_id={} cancel rejected (reason={}), "
                "reverting from PendingCancel to {}",
                cl_ord_id, cxl_rej_reason, static_cast<int>(order.state));
        } else {
            SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
                "on_cancel_reject: cl_ord_id={} cancel rejected (reason={}) "
                "but order not in PendingCancel state (state={}), no state change",
                cl_ord_id, cxl_rej_reason, static_cast<int>(order.state));
        }

        return true;
    }

    /// Get order by cl_ord_id. Returns nullptr if not found.
    [[nodiscard]] const ManagedOrder* get(std::string_view cl_ord_id) const noexcept
    {
        auto it = orders_.find(cl_ord_id);
        if (it == orders_.end()) return nullptr;
        return &it->second;
    }

    /// Count of active (non-terminal) orders.
    [[nodiscard]] size_t active_count() const noexcept
    {
        size_t count = 0;
        for (const auto& [_, order] : orders_) {
            if (!is_terminal(order.state)) ++count;
        }
        return count;
    }

    /// All orders.
    [[nodiscard]] const std::unordered_map<std::string, ManagedOrder, StringHash, std::equal_to<>>&
    orders() const noexcept { return orders_; }

    /// Remove all terminal orders (filled/canceled/rejected).
    void purge_terminal() noexcept
    {
        [[maybe_unused]] size_t removed = 0;
        for (auto it = orders_.begin(); it != orders_.end(); ) {
            if (is_terminal(it->second.state)) {
                it = orders_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
            "purge_terminal: removed {} terminal orders, {} remaining",
            removed, orders_.size());
    }

    /// Mark order as PendingCancel. Returns false if order not found or
    /// already terminal.
    bool mark_pending_cancel(std::string_view cl_ord_id) noexcept
    {
        auto it = orders_.find(cl_ord_id);
        if (it == orders_.end()) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "mark_pending_cancel: unknown cl_ord_id={}", cl_ord_id);
            return false;
        }

        auto& order = it->second;
        if (is_terminal(order.state)) {
            SPDLOG_LOGGER_WARN(detail::fix_ordmgr_logger(),
                "mark_pending_cancel: cl_ord_id={} already terminal (state={})",
                cl_ord_id, static_cast<int>(order.state));
            return false;
        }

        order.state = OrderState::PendingCancel;
        SPDLOG_LOGGER_DEBUG(detail::fix_ordmgr_logger(),
            "mark_pending_cancel: cl_ord_id={}", cl_ord_id);
        return true;
    }

private:
    std::unordered_map<std::string, ManagedOrder, StringHash, std::equal_to<>> orders_;
};

} // namespace eph::fix

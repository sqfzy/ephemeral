#pragma once

/// @file execution_report.hpp
/// Zero-copy typed view over a parsed FIX ExecutionReport (MsgType=8).
///
/// Provides typed accessors for the key fields of an execution report:
/// order identifiers, execution/order status, fill prices and quantities.
/// All string_view values point into the original message buffer -- no copies.

#include <cstdint>
#include <optional>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/fix/parser.hpp"
#include "eph/fix/tags.hpp"

namespace eph::fix {

/// Execution type (tag 150).
/// Indicates the reason for the execution report.
enum class ExecType : char {
    New           = '0',
    PartialFill   = '1',
    Fill          = '2',
    DoneForDay    = '3',
    Canceled      = '4',
    Replaced      = '5',
    PendingCancel = '6',
    Stopped       = '7',
    Rejected      = '8',
    Suspended     = '9',
    PendingNew    = 'A',
    Calculated    = 'B',
    Expired       = 'C',
    PendingReplace = 'E',
    Trade         = 'F',
};

/// Order status (tag 39).
/// Current state of the order on the exchange.
enum class OrdStatus : char {
    New             = '0',
    PartiallyFilled = '1',
    Filled          = '2',
    DoneForDay      = '3',
    Canceled        = '4',
    Replaced        = '5',
    PendingCancel   = '6',
    Stopped         = '7',
    Rejected        = '8',
    Suspended       = '9',
    PendingNew      = 'A',
    Calculated      = 'B',
    Expired         = 'C',
    PendingReplace  = 'E',
};

namespace detail {
inline spdlog::logger* fix_execrpt_logger() noexcept {
    static auto l = [] {
        auto lg = spdlog::get("fix.execrpt");
        if (!lg) lg = spdlog::stdout_color_mt("fix.execrpt");
        return lg;
    }();
    return l.get();
}

/// Validate that a char is a known ExecType value.
[[nodiscard]] inline std::optional<ExecType> to_exec_type(char c) noexcept {
    switch (c) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case 'A': case 'B': case 'C': case 'E': case 'F':
        return static_cast<ExecType>(c);
    default:
        SPDLOG_LOGGER_DEBUG(fix_execrpt_logger(),
            "unknown ExecType char='{}'", c);
        return std::nullopt;
    }
}

/// Validate that a char is a known OrdStatus value.
[[nodiscard]] inline std::optional<OrdStatus> to_ord_status(char c) noexcept {
    switch (c) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case 'A': case 'B': case 'C': case 'E':
        return static_cast<OrdStatus>(c);
    default:
        SPDLOG_LOGGER_DEBUG(fix_execrpt_logger(),
            "unknown OrdStatus char='{}'", c);
        return std::nullopt;
    }
}
} // namespace detail

/// Zero-copy view of a FIX ExecutionReport (MsgType=8).
///
/// Wraps a BasicMessageView and provides typed accessors for the most
/// commonly needed execution report fields.  All returned string_view
/// values point into the original parse buffer -- no copies, no allocations.
///
/// Usage:
///   auto msg = eph::fix::parse<256>(buf);
///   ExecutionReportView<256> er(msg);
///   if (auto et = er.exec_type(); et && *et == ExecType::Fill) { ... }
template <size_t MaxFields = 256>
class ExecutionReportView {
public:
    explicit ExecutionReportView(const BasicMessageView<MaxFields>& msg) noexcept
        : msg_(msg) {
        SPDLOG_LOGGER_TRACE(detail::fix_execrpt_logger(),
            "ExecutionReportView constructed, field_count={}", msg_.field_count());
    }

    // -- Identifiers --

    /// Client order ID (tag 11).
    [[nodiscard]] std::optional<std::string_view> cl_ord_id() const noexcept {
        return msg_.get(tag::ClOrdID);
    }

    /// Exchange-assigned order ID (tag 37).
    [[nodiscard]] std::optional<std::string_view> order_id() const noexcept {
        return msg_.get(tag::OrderID);
    }

    /// Unique execution identifier (tag 17).
    [[nodiscard]] std::optional<std::string_view> exec_id() const noexcept {
        return msg_.get(tag::ExecID);
    }

    // -- Status --

    /// Execution type (tag 150) -- reason for this report.
    [[nodiscard]] std::optional<ExecType> exec_type() const noexcept {
        auto c = msg_.get_char(tag::ExecType);
        if (!c) return std::nullopt;
        return detail::to_exec_type(*c);
    }

    /// Current order status (tag 39).
    [[nodiscard]] std::optional<OrdStatus> ord_status() const noexcept {
        auto c = msg_.get_char(tag::OrdStatus);
        if (!c) return std::nullopt;
        return detail::to_ord_status(*c);
    }

    // -- Instrument / side --

    /// Symbol (tag 55).
    [[nodiscard]] std::optional<std::string_view> symbol() const noexcept {
        return msg_.get(tag::Symbol);
    }

    /// Side (tag 54) -- '1' = Buy, '2' = Sell.
    [[nodiscard]] std::optional<char> side() const noexcept {
        return msg_.get_char(tag::Side);
    }

    // -- Fill fields --

    /// Last fill price (tag 31).
    [[nodiscard]] std::optional<double> last_px() const noexcept {
        return msg_.get_double(tag::LastPx);
    }

    /// Last fill quantity (tag 32).
    [[nodiscard]] std::optional<int64_t> last_qty() const noexcept {
        return msg_.get_int(tag::LastQty);
    }

    // -- Cumulative fields --

    /// Average fill price (tag 6).
    [[nodiscard]] std::optional<double> avg_px() const noexcept {
        return msg_.get_double(tag::AvgPx);
    }

    /// Cumulative filled quantity (tag 14).
    [[nodiscard]] std::optional<int64_t> cum_qty() const noexcept {
        return msg_.get_int(tag::CumQty);
    }

    /// Remaining open quantity (tag 151).
    [[nodiscard]] std::optional<int64_t> leaves_qty() const noexcept {
        return msg_.get_int(tag::LeavesQty);
    }

    // -- Misc --

    /// Free-text field (tag 58), often used for reject reasons.
    [[nodiscard]] std::optional<std::string_view> text() const noexcept {
        return msg_.get(tag::Text);
    }

    // -- Convenience predicates --

    /// True if this execution report represents a fill (partial or full).
    /// Checks ExecType for PartialFill ('1'), Fill ('2'), or Trade ('F').
    [[nodiscard]] bool is_fill() const noexcept {
        auto et = exec_type();
        if (!et) return false;
        return *et == ExecType::PartialFill
            || *et == ExecType::Fill
            || *et == ExecType::Trade;
    }

    /// True if the order has reached a terminal state:
    /// Filled, Canceled, Rejected, Expired, or DoneForDay.
    [[nodiscard]] bool is_terminal() const noexcept {
        auto os = ord_status();
        if (!os) return false;
        switch (*os) {
        case OrdStatus::Filled:
        case OrdStatus::Canceled:
        case OrdStatus::Rejected:
        case OrdStatus::Expired:
        case OrdStatus::DoneForDay:
            return true;
        default:
            return false;
        }
    }

private:
    const BasicMessageView<MaxFields>& msg_;
};

} // namespace eph::fix

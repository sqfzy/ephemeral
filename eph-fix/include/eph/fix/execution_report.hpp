#pragma once

/// @file execution_report.hpp
/// Zero-copy typed view over a parsed FIX ExecutionReport (MsgType=8).
///
/// Provides typed accessors for the key fields of an execution report:
/// order identifiers, execution/order status, fill prices and quantities.
/// All string_view values point into the original message buffer -- no copies.

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/fix/parser.hpp"
#include "eph/fix/tags.hpp"

namespace eph::fix {

/// @brief Execution type (tag 150).
///
/// Indicates the reason for the execution report. Each value maps to the
/// FIX 4.4 single-character ExecType encoding.
enum class ExecType : char {
    New            = '0',  ///< Order accepted by the exchange.
    PartialFill    = '1',  ///< Partial quantity executed.
    Fill           = '2',  ///< Full quantity executed.
    DoneForDay     = '3',  ///< Order expired for the trading session.
    Canceled       = '4',  ///< Order successfully canceled.
    Replaced       = '5',  ///< Order successfully replaced (cancel/replace).
    PendingCancel  = '6',  ///< Cancel request received, pending execution.
    Stopped        = '7',  ///< Order stopped (guaranteed fill price).
    Rejected       = '8',  ///< Order rejected by the exchange.
    Suspended      = '9',  ///< Order suspended by the exchange.
    PendingNew     = 'A',  ///< Order received, pending acceptance.
    Calculated     = 'B',  ///< Calculated (e.g. post-trade allocation).
    Expired        = 'C',  ///< Order expired (time in force reached).
    PendingReplace = 'E',  ///< Replace request received, pending execution.
    Trade          = 'F',  ///< Trade (FIX 4.4+ -- equivalent to Fill for matching engines).
};

/// @brief Order status (tag 39).
///
/// Current state of the order on the exchange. Terminal states are Filled,
/// Canceled, Rejected, Expired, and DoneForDay.
enum class OrdStatus : char {
    New             = '0',  ///< Order accepted, working on the book.
    PartiallyFilled = '1',  ///< Some quantity executed, remainder working.
    Filled          = '2',  ///< Full quantity executed (terminal).
    DoneForDay      = '3',  ///< Order expired for the trading session (terminal).
    Canceled        = '4',  ///< Order canceled (terminal).
    Replaced        = '5',  ///< Order replaced -- new order is working.
    PendingCancel   = '6',  ///< Cancel request pending at the exchange.
    Stopped         = '7',  ///< Order stopped (guaranteed fill price).
    Rejected        = '8',  ///< Order rejected by the exchange (terminal).
    Suspended       = '9',  ///< Order suspended by the exchange.
    PendingNew      = 'A',  ///< Order received, pending acceptance.
    Calculated      = 'B',  ///< Calculated (post-trade).
    Expired         = 'C',  ///< Order expired (terminal).
    PendingReplace  = 'E',  ///< Replace request pending at the exchange.
};

/// Human-readable name for ExecType.
[[nodiscard]] constexpr std::string_view exec_type_name(ExecType t) noexcept {
    switch (t) {
    case ExecType::New:            return "New";
    case ExecType::PartialFill:    return "PartialFill";
    case ExecType::Fill:           return "Fill";
    case ExecType::DoneForDay:     return "DoneForDay";
    case ExecType::Canceled:       return "Canceled";
    case ExecType::Replaced:       return "Replaced";
    case ExecType::PendingCancel:  return "PendingCancel";
    case ExecType::Stopped:        return "Stopped";
    case ExecType::Rejected:       return "Rejected";
    case ExecType::Suspended:      return "Suspended";
    case ExecType::PendingNew:     return "PendingNew";
    case ExecType::Calculated:     return "Calculated";
    case ExecType::Expired:        return "Expired";
    case ExecType::PendingReplace: return "PendingReplace";
    case ExecType::Trade:          return "Trade";
    }
    return "Unknown";
}

/// Human-readable name for OrdStatus.
[[nodiscard]] constexpr std::string_view ord_status_name(OrdStatus s) noexcept {
    switch (s) {
    case OrdStatus::New:             return "New";
    case OrdStatus::PartiallyFilled: return "PartiallyFilled";
    case OrdStatus::Filled:          return "Filled";
    case OrdStatus::DoneForDay:      return "DoneForDay";
    case OrdStatus::Canceled:        return "Canceled";
    case OrdStatus::Replaced:        return "Replaced";
    case OrdStatus::PendingCancel:   return "PendingCancel";
    case OrdStatus::Stopped:         return "Stopped";
    case OrdStatus::Rejected:        return "Rejected";
    case OrdStatus::Suspended:       return "Suspended";
    case OrdStatus::PendingNew:      return "PendingNew";
    case OrdStatus::Calculated:      return "Calculated";
    case OrdStatus::Expired:         return "Expired";
    case OrdStatus::PendingReplace:  return "PendingReplace";
    }
    return "Unknown";
}

/// @brief Internal implementation details for the execution report module.
namespace detail {
/// @brief Get or create the spdlog logger for execution report processing.
/// @return Raw pointer to the "fix.execrpt" logger (never null after first call).
inline spdlog::logger* fix_execrpt_logger() noexcept {
    static auto l = [] {
        auto lg = spdlog::get("fix.execrpt");
        if (!lg) lg = spdlog::stdout_color_mt("fix.execrpt");
        return lg;
    }();
    return l.get();
}

/// @brief Validate that a char is a known ExecType value.
/// @param c  The character to validate.
/// @return The corresponding ExecType, or nullopt if unrecognized.
[[nodiscard]] inline std::optional<ExecType> to_exec_type(char c) noexcept {
    switch (c) {
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case 'A': case 'B': case 'C': case 'E': case 'F':
        return static_cast<ExecType>(c);
    default:
        SPDLOG_LOGGER_WARN(fix_execrpt_logger(),
            "unknown ExecType char='{}' (0x{:02x})", c,
            static_cast<unsigned char>(c));
        return std::nullopt;
    }
}

/// @brief Validate that a char is a known OrdStatus value.
/// @param c  The character to validate.
/// @return The corresponding OrdStatus, or nullopt if unrecognized.
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

/// @brief Zero-copy view of a FIX ExecutionReport (MsgType=8).
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
    /// @brief Construct a view over a parsed FIX message.
    /// @param msg  The parsed message to wrap. Must outlive this view.
    /// @warning The caller is responsible for verifying MsgType='8' before construction.
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
    ///
    /// @warning FIX 4.4 tag 32 has type Qty (decimal). This integer accessor
    /// returns nullopt when the wire value is fractional (e.g. crypto venues
    /// reporting "0.001"), silently dropping the fill from int-only callers.
    /// Use last_qty_d() for fractional Qty support.
    [[nodiscard]] std::optional<int64_t> last_qty() const noexcept {
        return msg_.get_int(tag::LastQty);
    }

    /// Last fill quantity (tag 32) as a double — supports fractional Qty.
    /// FIX 4.4 Qty is a decimal field; many crypto venues report fills like
    /// "0.001" and equity venues do not. Use this when the venue may report
    /// non-integer fill quantities.
    [[nodiscard]] std::optional<double> last_qty_d() const noexcept {
        return msg_.get_double(tag::LastQty);
    }

    // -- Cumulative fields --

    /// Average fill price (tag 6).
    [[nodiscard]] std::optional<double> avg_px() const noexcept {
        return msg_.get_double(tag::AvgPx);
    }

    /// Cumulative filled quantity (tag 14).
    /// @warning FIX 4.4 tag 14 has type Qty (decimal). See last_qty() warning.
    [[nodiscard]] std::optional<int64_t> cum_qty() const noexcept {
        return msg_.get_int(tag::CumQty);
    }

    /// Cumulative filled quantity (tag 14) as a double — supports fractional Qty.
    [[nodiscard]] std::optional<double> cum_qty_d() const noexcept {
        return msg_.get_double(tag::CumQty);
    }

    /// Remaining open quantity (tag 151).
    /// @warning FIX 4.4 tag 151 has type Qty (decimal). See last_qty() warning.
    [[nodiscard]] std::optional<int64_t> leaves_qty() const noexcept {
        return msg_.get_int(tag::LeavesQty);
    }

    /// Remaining open quantity (tag 151) as a double — supports fractional Qty.
    [[nodiscard]] std::optional<double> leaves_qty_d() const noexcept {
        return msg_.get_double(tag::LeavesQty);
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

// ---------------------------------------------------------------------------
// Wire-bytes convenience parser
// ---------------------------------------------------------------------------

/// @brief Owns a parsed BasicMessageView together with its ExecutionReportView.
///
/// ExecutionReportView stores a const-reference to BasicMessageView, so
/// both must live together.  This struct bundles them with correct lifetime
/// semantics and disables copy/move-assignment to prevent dangling references.
template <size_t MaxFields = 256>
struct ParsedExecutionReport {
    BasicMessageView<MaxFields>       msg;
    ExecutionReportView<MaxFields>    view;

    explicit ParsedExecutionReport(BasicMessageView<MaxFields> m) noexcept
        : msg(std::move(m)), view(msg) {}

    // Movable (view will reference the new msg after move-construction).
    ParsedExecutionReport(ParsedExecutionReport&& o) noexcept
        : msg(std::move(o.msg)), view(msg) {}

    ParsedExecutionReport& operator=(ParsedExecutionReport&&) = delete;
    ParsedExecutionReport(const ParsedExecutionReport&)            = delete;
    ParsedExecutionReport& operator=(const ParsedExecutionReport&) = delete;
};

/// @brief Parse raw FIX bytes and return a ParsedExecutionReport if MsgType=8.
///
/// Combines parse() + MsgType check + view creation in one call, which is
/// the common hot-path when processing wire bytes in a trading gateway.
///
/// @param data  Pointer to raw FIX wire bytes
/// @param len   Number of available bytes
/// @return ParsedExecutionReport on success, nullopt if parse fails or
///         the message is not an ExecutionReport (MsgType != '8').
template <size_t MaxFields = 256>
[[nodiscard]] inline std::optional<ParsedExecutionReport<MaxFields>>
try_parse_execution_report(const uint8_t* data, size_t len) noexcept {
    auto result = parse<MaxFields>(data, len);
    if (!result.has_value()) {
        SPDLOG_LOGGER_DEBUG(detail::fix_execrpt_logger(),
            "try_parse_execution_report: parse failed, len={}", len);
        return std::nullopt;
    }

    // Check MsgType (tag 35) == '8' (ExecutionReport)
    auto msg_type = result->get(tag::MsgType);
    if (!msg_type || *msg_type != "8") {
        SPDLOG_LOGGER_DEBUG(detail::fix_execrpt_logger(),
            "try_parse_execution_report: MsgType is '{}', expected '8'",
            msg_type.value_or("(absent)"));
        return std::nullopt;
    }

    return ParsedExecutionReport<MaxFields>{std::move(*result)};
}

} // namespace eph::fix

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations for FIX execution report enums
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::fix::ExecType> : std::formatter<std::string_view> {
    auto format(eph::fix::ExecType t, auto& ctx) const {
        return std::formatter<std::string_view>::format(eph::fix::exec_type_name(t), ctx);
    }
};

template <>
struct std::formatter<eph::fix::OrdStatus> : std::formatter<std::string_view> {
    auto format(eph::fix::OrdStatus s, auto& ctx) const {
        return std::formatter<std::string_view>::format(eph::fix::ord_status_name(s), ctx);
    }
};

#pragma once

/// @file error_traits.hpp
/// ErrorEnum concept and generic formatter — shared error infrastructure.
///
/// All error enums in the library (FrameError, SendError, ConnectionError,
/// ParseError, etc.) satisfy the ErrorEnum concept by providing an ADL-visible
/// error_name() function returning string_view.
///
/// ErrorEnumFormatter eliminates per-enum std::formatter boilerplate:
///   template <> struct std::formatter<MyError> : eph::core::ErrorEnumFormatter<MyError> {};

#include <concepts>
#include <format>
#include <string_view>
#include <type_traits>

namespace eph::core {

/// Concept for error enums that support human-readable names.
///
/// Satisfied when:
///   1. E is an enum type
///   2. An ADL-visible error_name(E) function exists returning string_view
///
/// All error enums in eph-core, eph-net, eph-fix, and eph-itch satisfy this.
template <typename E>
concept ErrorEnum = std::is_enum_v<E> && requires(E e) {
    { error_name(e) } -> std::convertible_to<std::string_view>;
};

/// Generic std::formatter base for any ErrorEnum type.
///
/// Usage (one-liner per enum):
///   template <> struct std::formatter<eph::net::FrameError>
///       : eph::core::ErrorEnumFormatter<eph::net::FrameError> {};
template <ErrorEnum E>
struct ErrorEnumFormatter : std::formatter<std::string_view> {
    auto format(E e, auto& ctx) const {
        return std::formatter<std::string_view>::format(error_name(e), ctx);
    }
};

} // namespace eph::core

// Backward-compatible aliases so existing eph::net:: references compile.
namespace eph::net {
template <typename E>
concept ErrorEnum = eph::core::ErrorEnum<E>;
template <eph::core::ErrorEnum E>
using ErrorEnumFormatter = eph::core::ErrorEnumFormatter<E>;
} // namespace eph::net

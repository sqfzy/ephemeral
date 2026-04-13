/// @file test_error_traits.cpp
/// Unit tests for ErrorEnum concept, ErrorEnumFormatter, and FrameError.

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <string_view>
#include <type_traits>

#include "eph/core/error_traits.hpp"
#include "eph/core/framer_concept.hpp"

namespace ec = eph::core;

// ---------------------------------------------------------------------------
// ErrorEnum concept — positive cases
// ---------------------------------------------------------------------------

TEST(ErrorEnum, FrameErrorSatisfiesConcept) {
    static_assert(ec::ErrorEnum<eph::net::FrameError>);
}

// A custom enum that satisfies ErrorEnum
namespace test_ns {
enum class MyError : uint8_t { kFoo, kBar };
[[nodiscard]] constexpr std::string_view error_name(MyError e) noexcept {
    switch (e) {
    case MyError::kFoo: return "foo";
    case MyError::kBar: return "bar";
    }
    return "unknown";
}
} // namespace test_ns

TEST(ErrorEnum, CustomEnumSatisfiesConcept) {
    static_assert(ec::ErrorEnum<test_ns::MyError>);
}

// ---------------------------------------------------------------------------
// ErrorEnum concept — negative cases
// ---------------------------------------------------------------------------

TEST(ErrorEnum, NonEnumDoesNotSatisfy) {
    static_assert(!ec::ErrorEnum<int>);
    static_assert(!ec::ErrorEnum<std::string>);
    static_assert(!ec::ErrorEnum<double>);
    static_assert(!ec::ErrorEnum<void*>);
}

// An enum without error_name ADL function
namespace test_ns {
enum class OrphanError : uint8_t { kX };
} // namespace test_ns

TEST(ErrorEnum, EnumWithoutErrorNameDoesNotSatisfy) {
    static_assert(!ec::ErrorEnum<test_ns::OrphanError>);
}

// A class type should not satisfy ErrorEnum
namespace test_ns {
struct NotAnEnum {
    int value;
};
} // namespace test_ns

TEST(ErrorEnum, ClassTypeDoesNotSatisfy) {
    static_assert(!ec::ErrorEnum<test_ns::NotAnEnum>);
}

// ---------------------------------------------------------------------------
// ErrorEnumFormatter — std::format integration
// ---------------------------------------------------------------------------

// Provide formatter for MyError
template <>
struct std::formatter<test_ns::MyError>
    : ec::ErrorEnumFormatter<test_ns::MyError> {};

TEST(ErrorEnumFormatter, FormatsCustomEnumValue) {
    auto s = std::format("{}", test_ns::MyError::kFoo);
    EXPECT_EQ(s, "foo");
}

TEST(ErrorEnumFormatter, FormatsAllCustomValues) {
    EXPECT_EQ(std::format("{}", test_ns::MyError::kFoo), "foo");
    EXPECT_EQ(std::format("{}", test_ns::MyError::kBar), "bar");
}

TEST(ErrorEnumFormatter, FormatsFrameError) {
    EXPECT_EQ(std::format("{}", eph::net::FrameError::kIncomplete), "incomplete");
    EXPECT_EQ(std::format("{}", eph::net::FrameError::kInvalidFormat), "invalid format");
    EXPECT_EQ(std::format("{}", eph::net::FrameError::kPayloadTooLarge), "payload too large");
}

TEST(ErrorEnumFormatter, WorksInFormatString) {
    auto s = std::format("error: {}", eph::net::FrameError::kInvalidFormat);
    EXPECT_EQ(s, "error: invalid format");
}

// ---------------------------------------------------------------------------
// FrameError error_name / frame_error_name
// ---------------------------------------------------------------------------

TEST(FrameError, ErrorNameViaADL) {
    using eph::net::FrameError;
    using eph::net::error_name;
    EXPECT_EQ(error_name(FrameError::kIncomplete), "incomplete");
    EXPECT_EQ(error_name(FrameError::kInvalidFormat), "invalid format");
    EXPECT_EQ(error_name(FrameError::kPayloadTooLarge), "payload too large");
}

TEST(FrameError, FrameErrorNameFunction) {
    using eph::net::FrameError;
    using eph::net::frame_error_name;
    EXPECT_EQ(frame_error_name(FrameError::kIncomplete), "incomplete");
    EXPECT_EQ(frame_error_name(FrameError::kInvalidFormat), "invalid format");
    EXPECT_EQ(frame_error_name(FrameError::kPayloadTooLarge), "payload too large");
}

TEST(FrameError, UnknownValueReturnsUnknown) {
    auto bad = static_cast<eph::net::FrameError>(255);
    EXPECT_EQ(eph::net::frame_error_name(bad), "unknown");
}

TEST(FrameError, IsConstexprEvaluable) {
    constexpr auto name = eph::net::frame_error_name(
        eph::net::FrameError::kIncomplete);
    static_assert(name == "incomplete");
}

// ---------------------------------------------------------------------------
// DecodedFrame struct layout
// ---------------------------------------------------------------------------

TEST(DecodedFrame, DefaultConstructible) {
    eph::net::DecodedFrame f{};
    EXPECT_EQ(f.payload, nullptr);
    EXPECT_EQ(f.payload_len, 0u);
    EXPECT_EQ(f.msg_type, 0u);
    EXPECT_FALSE(f.is_control);
    EXPECT_EQ(f.total_len, 0u);
}

TEST(DecodedFrame, FieldsAssignable) {
    uint8_t data[] = {0x01, 0x02, 0x03};
    eph::net::DecodedFrame f{
        .payload     = data,
        .payload_len = 3,
        .msg_type    = 2,
        .is_control  = false,
        .total_len   = 5,
    };
    EXPECT_EQ(f.payload, data);
    EXPECT_EQ(f.payload_len, 3u);
    EXPECT_EQ(f.msg_type, 2u);
    EXPECT_EQ(f.total_len, 5u);
}

// ---------------------------------------------------------------------------
// Backward-compatible eph::net:: aliases
// ---------------------------------------------------------------------------

TEST(ErrorEnumAlias, NetNamespaceAliasWorks) {
    static_assert(eph::net::ErrorEnum<eph::net::FrameError>);
    static_assert(eph::net::ErrorEnum<test_ns::MyError>);
    static_assert(!eph::net::ErrorEnum<int>);
}

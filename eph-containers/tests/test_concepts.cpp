/// @file test_concepts.cpp
/// Unit tests for TrivialData concept — gate for lock-free container element types.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "eph/containers/concepts.hpp"

using eph::containers::TrivialData;

// ---------------------------------------------------------------------------
// Positive cases — types that should satisfy TrivialData
// ---------------------------------------------------------------------------

TEST(TrivialData, PrimitiveIntegersSatisfy) {
    static_assert(TrivialData<uint8_t>);
    static_assert(TrivialData<uint16_t>);
    static_assert(TrivialData<uint32_t>);
    static_assert(TrivialData<uint64_t>);
    static_assert(TrivialData<int8_t>);
    static_assert(TrivialData<int16_t>);
    static_assert(TrivialData<int32_t>);
    static_assert(TrivialData<int64_t>);
    SUCCEED();
}

TEST(TrivialData, FloatingSatisfy) {
    static_assert(TrivialData<float>);
    static_assert(TrivialData<double>);
    SUCCEED();
}

TEST(TrivialData, BoolCharSatisfy) {
    static_assert(TrivialData<bool>);
    static_assert(TrivialData<char>);
    SUCCEED();
}

TEST(TrivialData, RawPointersSatisfy) {
    static_assert(TrivialData<int*>);
    static_assert(TrivialData<const char*>);
    static_assert(TrivialData<void*>);
    SUCCEED();
}

TEST(TrivialData, FixedSizeArraySatisfies) {
    static_assert(TrivialData<std::array<uint8_t, 64>>);
    static_assert(TrivialData<std::array<int, 4>>);
    SUCCEED();
}

// A typical HFT POD struct
struct alignas(64) MarketTick {
    uint64_t timestamp_ns;
    int64_t  price;
    uint32_t qty;
    uint16_t symbol_id;
    uint8_t  side;      // 0=bid, 1=ask
    uint8_t  padding;
};

TEST(TrivialData, PodStructSatisfies) {
    static_assert(TrivialData<MarketTick>);
    SUCCEED();
}

// A C-style enum
enum Side : uint8_t { kBid, kAsk };

TEST(TrivialData, CEnumSatisfies) {
    static_assert(TrivialData<Side>);
    SUCCEED();
}

// A scoped enum
enum class OrderType : uint8_t { kLimit, kMarket };

TEST(TrivialData, ScopedEnumSatisfies) {
    static_assert(TrivialData<OrderType>);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Negative cases — types that must NOT satisfy TrivialData
// ---------------------------------------------------------------------------

TEST(TrivialData, StdStringDoesNotSatisfy) {
    static_assert(!TrivialData<std::string>);
    SUCCEED();
}

TEST(TrivialData, StdVectorDoesNotSatisfy) {
    static_assert(!TrivialData<std::vector<int>>);
    SUCCEED();
}

TEST(TrivialData, UniquePtrDoesNotSatisfy) {
    static_assert(!TrivialData<std::unique_ptr<int>>);
    SUCCEED();
}

TEST(TrivialData, SharedPtrDoesNotSatisfy) {
    static_assert(!TrivialData<std::shared_ptr<int>>);
    SUCCEED();
}

// Struct with non-trivial destructor
struct HasDtor {
    ~HasDtor() { /* side effect */ }
};

TEST(TrivialData, NonTriviallyDestructibleDoesNotSatisfy) {
    static_assert(!TrivialData<HasDtor>);
    SUCCEED();
}

// Struct with non-trivial copy
struct HasCopy {
    HasCopy(const HasCopy&) {}
    HasCopy& operator=(const HasCopy&) { return *this; }
    HasCopy() = default;
};

TEST(TrivialData, NonTriviallyCopyableDoesNotSatisfy) {
    static_assert(!TrivialData<HasCopy>);
    SUCCEED();
}

// Struct that is not default initializable
struct NoDefault {
    NoDefault() = delete;
    NoDefault(int) {}
};

TEST(TrivialData, NotDefaultInitializableDoesNotSatisfy) {
    static_assert(!TrivialData<NoDefault>);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(TrivialData, EmptyStructSatisfies) {
    struct Empty {};
    static_assert(TrivialData<Empty>);
    SUCCEED();
}

TEST(TrivialData, NestedTrivialStructSatisfies) {
    struct Inner { uint32_t x; };
    struct Outer { Inner a; Inner b; uint64_t ts; };
    static_assert(TrivialData<Outer>);
    SUCCEED();
}

// Note: std::pair may or may not be trivially copyable depending on
// the standard library implementation. We test a plain struct instead.
struct TrivialPair { int a; double b; };

TEST(TrivialData, TrivialPairSatisfies) {
    static_assert(TrivialData<TrivialPair>);
    SUCCEED();
}

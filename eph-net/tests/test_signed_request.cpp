/// @file test_signed_request.cpp
/// @brief Unit tests for `eph::net::SignedRequest<Traits>` and the venue
///        traits (Binance / OKX / Bybit).
///
/// Test vectors:
///   * Binance: the canonical querystring example from the Binance Spot
///     SIGNED-endpoint docs (also reused in test_hmac.cpp's
///     ExchangeStyle.BinanceSignatureShapeIsLowercaseHex64). Reference:
///     https://binance-docs.github.io/apidocs/spot/en/#signed-trade-user_data-and-margin-endpoint-security
///   * OKX: the official V5 example from the OKX docs:
///       timestamp = 2020-12-08T09:08:57.715Z
///       method    = GET
///       requestPath = /api/v5/account/balance?ccy=BTC
///       body      = (empty)
///       secret    = 22582BD0CFF14C41EDBF1AB98506286D
///     The expected base64 signature was self-generated with Python `hmac`
///     + `hashlib` and cross-checked with `openssl dgst -sha256 -hmac ...
///     | xxd -r -p | base64`. Both renders agree on
///     `HiZhvSfMtWJA3uUIVXV3a/bSXNPCWvYFXoGCVS8V4zY=`. This is the value
///     the OKX docs example produces (the OKX docs themselves do not list
///     the expected signature inline — they list the input parameters).
///   * Bybit: a constructed example following the V5 docs' pre-MAC format:
///       ts        = 1658385579423
///       api_key   = XXXXXXXXXXX
///       recv_window = 5000
///       payload   = category=option&symbol=BTC-29JUL22-25000-C
///       secret    = BYBIT_SECRET
///     The expected hex signature was self-generated with Python `hmac`
///     and cross-checked with `openssl dgst -sha256 -hmac BYBIT_SECRET`.
///     Both renders agree on
///     `78767c0fe56aa2738036f199ed97dbe3236660653ce7a3da957ba6787627ecac`.

#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "eph/net/hmac.hpp"
#include "eph/net/http.hpp"
#include "eph/net/signed_request.hpp"

using eph::net::BinanceSignTraits;
using eph::net::BybitSignTraits;
using eph::net::HmacSha256Key;
using eph::net::HttpHeader;
using eph::net::OkxSignTraits;
using eph::net::SignedHeaderBundle;
using eph::net::SignedRequest;

namespace {

/// Look up a header value by name (linear scan — bundles have ≤4 headers).
/// Returns empty view if not found.
std::string_view find_header(
    const std::vector<HttpHeader>& hs, std::string_view name) {
    for (const auto& h : hs) {
        if (h.name == name) return h.value;
    }
    return {};
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// BinanceSpot_KnownVector
// Binance docs canonical example: known querystring + secret → known hex sig.
// ═════════════════════════════════════════════════════════════════════════════

TEST(BinanceSpot, KnownVector) {
    // The Binance docs use this exact secret + querystring as the
    // worked-out example for SIGNED endpoints. Expected hex below is what
    // `openssl dgst -sha256 -hmac <secret>` produces over the querystring.
    constexpr std::string_view kSecret =
        "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
    constexpr std::string_view kQuery =
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC"
        "&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
    constexpr std::string_view kExpectedHex =
        "c8db56825ae71d6d79447849e617115f"
        "4a920fa2acdcab2b053c4b2838bd6b71";

    SignedRequest<BinanceSignTraits> sr{HmacSha256Key{kSecret}};
    // ts_ms here matches the timestamp embedded in the querystring above
    // (1499827319559 ms). Binance signs whatever is on the wire — we
    // forward the pre-spliced query as-is.
    auto bundle = sr.headers_for_query(kQuery, /*ts_ms=*/1499827319559ULL);

    auto sig = find_header(bundle.headers, "X-MBX-SIGNATURE");
    ASSERT_FALSE(sig.empty()) << "X-MBX-SIGNATURE header missing";
    EXPECT_EQ(sig, kExpectedHex);

    // Timestamp header value must be the decimal rendering of ts_ms.
    auto ts = find_header(bundle.headers, "X-MBX-TIMESTAMP");
    EXPECT_EQ(ts, "1499827319559");

    // No recv-window header for Binance.
    EXPECT_TRUE(find_header(bundle.headers, "X-BAPI-RECV-WINDOW").empty());
}

TEST(BinanceSpot, BodyOverloadIsByteEqualToQueryOverload) {
    // For Binance the "query" and "body" forms sign the same way (we
    // concatenate query + body, where one of the two is empty). Sanity:
    // signing the same payload via either entry point yields the same sig.
    HmacSha256Key kq{std::string_view{"sec"}};
    HmacSha256Key kb{std::string_view{"sec"}};
    SignedRequest<BinanceSignTraits> sr_q{std::move(kq)};
    SignedRequest<BinanceSignTraits> sr_b{std::move(kb)};

    constexpr std::string_view kPayload = "symbol=BTCUSDT&timestamp=1700000000000";

    auto b1 = sr_q.headers_for_query(kPayload, 1700000000000ULL);
    auto b2 = sr_b.headers_for_body(kPayload,  1700000000000ULL);

    EXPECT_EQ(find_header(b1.headers, "X-MBX-SIGNATURE"),
              find_header(b2.headers, "X-MBX-SIGNATURE"));
}

// ═════════════════════════════════════════════════════════════════════════════
// OkxV5_KnownVector
// timestamp + method + path + body → base64
// ═════════════════════════════════════════════════════════════════════════════

TEST(OkxV5, KnownVector) {
    // OKX V5 example (constructed from docs; expected sig self-generated
    // with Python hmac+base64 and cross-checked with `openssl dgst -sha256
    // -hmac <secret> | xxd -r -p | base64`).
    constexpr std::string_view kSecret  = "22582BD0CFF14C41EDBF1AB98506286D";
    constexpr std::string_view kTsIso   = "2020-12-08T09:08:57.715Z";
    constexpr std::string_view kMethod  = "GET";
    constexpr std::string_view kPath    = "/api/v5/account/balance?ccy=BTC";
    constexpr std::string_view kBody    = "";
    constexpr std::string_view kExpectedB64 =
        "HiZhvSfMtWJA3uUIVXV3a/bSXNPCWvYFXoGCVS8V4zY=";

    SignedRequest<OkxSignTraits> sr{HmacSha256Key{kSecret}};
    auto bundle = sr.headers_for_okx(kMethod, kPath, kBody, kTsIso);

    auto sig = find_header(bundle.headers, "OK-ACCESS-SIGN");
    ASSERT_FALSE(sig.empty()) << "OK-ACCESS-SIGN header missing";
    EXPECT_EQ(sig, kExpectedB64);

    // OKX signature is base64 — must end with '=' for a 32-byte input
    // (32 bytes → 44 ascii chars including 1 '=' pad).
    EXPECT_EQ(sig.size(), 44u);
    EXPECT_EQ(sig.back(), '=');

    // The timestamp header carries the same string we signed with.
    auto ts = find_header(bundle.headers, "OK-ACCESS-TIMESTAMP");
    EXPECT_EQ(ts, kTsIso);
}

TEST(OkxV5, BuildToSignFormatIsTsMethodPathBody) {
    // White-box assertion: confirm the canonical concatenation order. A
    // future regression that reorders these fields would silently produce
    // the wrong signature on the wire.
    auto s = OkxSignTraits::build_to_sign(
        /*method=*/"POST",
        /*path=*/  "/api/v5/trade/order",
        /*body=*/  "{\"x\":1}",
        /*ts_iso=*/"2026-01-01T00:00:00.000Z");
    EXPECT_EQ(s,
              "2026-01-01T00:00:00.000Z"
              "POST"
              "/api/v5/trade/order"
              "{\"x\":1}");
}

// ═════════════════════════════════════════════════════════════════════════════
// BybitV5_KnownVector
// ts + api_key + recv_window + payload → hex
// ═════════════════════════════════════════════════════════════════════════════

TEST(BybitV5, KnownVector) {
    constexpr std::string_view kSecret      = "BYBIT_SECRET";
    constexpr std::string_view kTsMs        = "1658385579423";
    constexpr std::string_view kApiKey      = "XXXXXXXXXXX";
    constexpr std::string_view kRecvWindow  = "5000";
    constexpr std::string_view kPayload     = "category=option&symbol=BTC-29JUL22-25000-C";
    constexpr std::string_view kExpectedHex =
        "78767c0fe56aa2738036f199ed97dbe3236660653ce7a3da957ba6787627ecac";

    SignedRequest<BybitSignTraits> sr{HmacSha256Key{kSecret}};
    auto bundle = sr.headers_for_bybit(kApiKey, kTsMs, kRecvWindow, kPayload);

    auto sig = find_header(bundle.headers, "X-BAPI-SIGN");
    ASSERT_FALSE(sig.empty()) << "X-BAPI-SIGN header missing";
    EXPECT_EQ(sig, kExpectedHex);

    EXPECT_EQ(find_header(bundle.headers, "X-BAPI-TIMESTAMP"),    kTsMs);
    EXPECT_EQ(find_header(bundle.headers, "X-BAPI-RECV-WINDOW"),  kRecvWindow);
}

TEST(BybitV5, BuildToSignFormatIsTsKeyRecvPayload) {
    // White-box assertion: ts + api_key + recv + payload, in that order.
    auto s = BybitSignTraits::build_to_sign(
        /*api_key=*/    "MYKEY",
        /*ts_ms=*/      "1700000000000",
        /*recv_window=*/"5000",
        /*payload=*/    "category=spot");
    EXPECT_EQ(s, "1700000000000MYKEY5000category=spot");
}

// ═════════════════════════════════════════════════════════════════════════════
// HeadersInjectedCorrectly — each venue produces the right *set* of headers
// ═════════════════════════════════════════════════════════════════════════════

TEST(HeadersInjected, BinanceHasSigAndTsOnly) {
    SignedRequest<BinanceSignTraits> sr{HmacSha256Key{std::string_view{"k"}}};
    auto b = sr.headers_for_query("a=1&timestamp=2", /*ts_ms=*/2);
    EXPECT_EQ(b.headers.size(), 2u);
    EXPECT_FALSE(find_header(b.headers, "X-MBX-SIGNATURE").empty());
    EXPECT_FALSE(find_header(b.headers, "X-MBX-TIMESTAMP").empty());
    // No recv-window header for Binance.
    EXPECT_TRUE(find_header(b.headers, "X-MBX-RECV-WINDOW").empty());
}

TEST(HeadersInjected, OkxHasSigAndTsOnly) {
    SignedRequest<OkxSignTraits> sr{HmacSha256Key{std::string_view{"k"}}};
    auto b = sr.headers_for_okx("GET", "/x", "", "2026-01-01T00:00:00.000Z");
    EXPECT_EQ(b.headers.size(), 2u);
    EXPECT_FALSE(find_header(b.headers, "OK-ACCESS-SIGN").empty());
    EXPECT_FALSE(find_header(b.headers, "OK-ACCESS-TIMESTAMP").empty());
    // No recv-window header for OKX.
    EXPECT_TRUE(find_header(b.headers, "OK-ACCESS-RECV-WINDOW").empty());
}

TEST(HeadersInjected, BybitHasSigTsAndRecv) {
    SignedRequest<BybitSignTraits> sr{HmacSha256Key{std::string_view{"k"}}};
    auto b = sr.headers_for_bybit("KEY", "1700000000000", "5000", "x=1");
    EXPECT_EQ(b.headers.size(), 3u);
    EXPECT_FALSE(find_header(b.headers, "X-BAPI-SIGN").empty());
    EXPECT_FALSE(find_header(b.headers, "X-BAPI-TIMESTAMP").empty());
    EXPECT_EQ(find_header(b.headers, "X-BAPI-RECV-WINDOW"), "5000");
}

TEST(HeadersInjected, BybitEmptyRecvWindowOmitsHeader) {
    // recv_window="" is the documented sentinel for "do not emit the
    // X-BAPI-RECV-WINDOW header" — used by callers who want to fall back
    // to the venue default. The signature is computed with an empty
    // recv-window component (consistent with the on-wire header set).
    SignedRequest<BybitSignTraits> sr{HmacSha256Key{std::string_view{"k"}}};
    auto b = sr.headers_for_bybit("KEY", "1700000000000", "", "x=1");
    EXPECT_EQ(b.headers.size(), 2u);
    EXPECT_TRUE(find_header(b.headers, "X-BAPI-RECV-WINDOW").empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// Header value views must remain valid for the bundle's lifetime
// ═════════════════════════════════════════════════════════════════════════════

TEST(BundleLifetime, ValueViewsPointIntoBundleStorage) {
    SignedRequest<BinanceSignTraits> sr{HmacSha256Key{std::string_view{"k"}}};
    auto bundle = sr.headers_for_query("q=1&timestamp=1", 1ULL);

    // For each header we returned, its `value` should be a view into one
    // of the strings in `storage`. We don't pin a specific index — just
    // that some storage entry contains the same bytes.
    for (const auto& h : bundle.headers) {
        bool found = false;
        for (const auto& s : bundle.storage) {
            if (std::string_view{s} == h.value) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found)
            << "header value '" << h.value << "' not in bundle.storage";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// KeyRaiiClear — sensitive bytes are zeroed when the SignedRequest dies
// ═════════════════════════════════════════════════════════════════════════════

TEST(KeyRaiiClear, DestructorZeroesUnderlyingKey) {
    // SignedRequest holds an HmacSha256Key by value. The HmacSha256Key's
    // RAII contract (verified separately in test_hmac.cpp) zeroes its
    // 64-byte normalized buffer on destruction via OPENSSL_cleanse.
    //
    // Strategy here: placement-new a SignedRequest into a known-poison
    // buffer, then explicitly destroy it, and confirm the entire object
    // footprint (which IS the key buffer plus padding) is zero.
    using SR = SignedRequest<BinanceSignTraits>;
    alignas(SR) unsigned char storage[sizeof(SR)]{};
    std::memset(storage, 0xAA, sizeof(storage));

    {
        auto* p = new (storage) SR{HmacSha256Key{std::string_view{"hot-secret"}}};
        // Sanity: the buffer is no longer all 0xAA after construction
        // (the normalized key bytes are now in there).
        bool any_non_poison = false;
        for (auto b : storage) {
            if (b != 0xAA) {
                any_non_poison = true;
                break;
            }
        }
        EXPECT_TRUE(any_non_poison);
        p->~SR();
    }

    // After destruction the 64-byte normalized buffer of the contained
    // HmacSha256Key must be zero. The SignedRequest object layout is just
    // the HmacSha256Key (no other members), so the entire footprint zeros.
    // We assert on the first 64 bytes (the key's `normalized_` array).
    for (size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(storage[i], 0u)
            << "byte " << i << " of HmacSha256Key not zeroed on destroy";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Compile-time guarantees
// ═════════════════════════════════════════════════════════════════════════════

TEST(StaticGuarantees, NonCopyable) {
    static_assert(!std::is_copy_constructible_v<SignedRequest<BinanceSignTraits>>);
    static_assert(!std::is_copy_assignable_v<SignedRequest<BinanceSignTraits>>);
    static_assert(!std::is_copy_constructible_v<SignedRequest<OkxSignTraits>>);
    static_assert(!std::is_copy_constructible_v<SignedRequest<BybitSignTraits>>);
    SUCCEED();
}

TEST(StaticGuarantees, MovableNoexcept) {
    static_assert(std::is_nothrow_move_constructible_v<SignedRequest<BinanceSignTraits>>);
    static_assert(std::is_nothrow_move_constructible_v<SignedRequest<OkxSignTraits>>);
    static_assert(std::is_nothrow_move_constructible_v<SignedRequest<BybitSignTraits>>);
    SUCCEED();
}

TEST(StaticGuarantees, TraitsHaveCorrectHeaderNames) {
    static_assert(BinanceSignTraits::sign_header == "X-MBX-SIGNATURE");
    static_assert(BinanceSignTraits::ts_header   == "X-MBX-TIMESTAMP");
    static_assert(OkxSignTraits::sign_header     == "OK-ACCESS-SIGN");
    static_assert(OkxSignTraits::ts_header       == "OK-ACCESS-TIMESTAMP");
    static_assert(BybitSignTraits::sign_header   == "X-BAPI-SIGN");
    static_assert(BybitSignTraits::ts_header     == "X-BAPI-TIMESTAMP");
    static_assert(BybitSignTraits::recv_header   == "X-BAPI-RECV-WINDOW");
    SUCCEED();
}

// ═════════════════════════════════════════════════════════════════════════════
// Low-level escape hatch — sign_to_string returns the rendered signature
// ═════════════════════════════════════════════════════════════════════════════

TEST(LowLevelSign, BinanceSignToStringMatchesHeader) {
    SignedRequest<BinanceSignTraits> sr{HmacSha256Key{std::string_view{
        "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"}}};
    constexpr std::string_view kQuery =
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC"
        "&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
    auto sig = sr.sign_to_string(kQuery);
    EXPECT_EQ(sig,
              "c8db56825ae71d6d79447849e617115f"
              "4a920fa2acdcab2b053c4b2838bd6b71");
}

TEST(LowLevelSign, OkxSignToStringMatchesHeader) {
    SignedRequest<OkxSignTraits> sr{HmacSha256Key{std::string_view{
        "22582BD0CFF14C41EDBF1AB98506286D"}}};
    auto pre = OkxSignTraits::build_to_sign(
        "GET", "/api/v5/account/balance?ccy=BTC", "",
        "2020-12-08T09:08:57.715Z");
    auto sig = sr.sign_to_string(pre);
    EXPECT_EQ(sig, "HiZhvSfMtWJA3uUIVXV3a/bSXNPCWvYFXoGCVS8V4zY=");
}

TEST(LowLevelSign, BybitSignToStringMatchesHeader) {
    SignedRequest<BybitSignTraits> sr{HmacSha256Key{std::string_view{
        "BYBIT_SECRET"}}};
    auto pre = BybitSignTraits::build_to_sign(
        "XXXXXXXXXXX", "1658385579423", "5000",
        "category=option&symbol=BTC-29JUL22-25000-C");
    auto sig = sr.sign_to_string(pre);
    EXPECT_EQ(sig,
              "78767c0fe56aa2738036f199ed97dbe3236660653ce7a3da957ba6787627ecac");
}

// ═════════════════════════════════════════════════════════════════════════════
// Determinism — same inputs always produce the same headers
// ═════════════════════════════════════════════════════════════════════════════

TEST(Determinism, BinanceTwoCallsSameInputsAgree) {
    SignedRequest<BinanceSignTraits> sr{HmacSha256Key{std::string_view{"k"}}};
    auto b1 = sr.headers_for_query("a=1&timestamp=10", 10);
    auto b2 = sr.headers_for_query("a=1&timestamp=10", 10);
    EXPECT_EQ(find_header(b1.headers, "X-MBX-SIGNATURE"),
              find_header(b2.headers, "X-MBX-SIGNATURE"));
}

TEST(Determinism, OkxTwoCallsSameInputsAgree) {
    SignedRequest<OkxSignTraits> sr{HmacSha256Key{std::string_view{"k"}}};
    auto b1 = sr.headers_for_okx("GET", "/x", "", "2026-01-01T00:00:00.000Z");
    auto b2 = sr.headers_for_okx("GET", "/x", "", "2026-01-01T00:00:00.000Z");
    EXPECT_EQ(find_header(b1.headers, "OK-ACCESS-SIGN"),
              find_header(b2.headers, "OK-ACCESS-SIGN"));
}

TEST(Determinism, DifferentTimestampsProduceDifferentSigs) {
    SignedRequest<BinanceSignTraits> sr{HmacSha256Key{std::string_view{"k"}}};
    auto b1 = sr.headers_for_query("a=1&timestamp=1", 1);
    auto b2 = sr.headers_for_query("a=1&timestamp=2", 2);
    EXPECT_NE(find_header(b1.headers, "X-MBX-SIGNATURE"),
              find_header(b2.headers, "X-MBX-SIGNATURE"));
}

// ═════════════════════════════════════════════════════════════════════════════
// Move semantics — the SignedRequest can be moved without invalidating
// signatures that have already been produced
// ═════════════════════════════════════════════════════════════════════════════

TEST(Move, MoveSrcThenSignFromDstAgrees) {
    SignedRequest<BinanceSignTraits> sr1{HmacSha256Key{std::string_view{"my-secret"}}};
    auto before = sr1.headers_for_query("q=1&timestamp=1", 1);

    SignedRequest<BinanceSignTraits> sr2{std::move(sr1)};
    auto after = sr2.headers_for_query("q=1&timestamp=1", 1);

    EXPECT_EQ(find_header(before.headers, "X-MBX-SIGNATURE"),
              find_header(after.headers,  "X-MBX-SIGNATURE"));
}

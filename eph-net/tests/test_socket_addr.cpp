/// @file test_socket_addr.cpp
/// Unit tests for `eph::net::Ipv4Addr` and `eph::net::SocketAddr`.
///
/// Covers:
///   - constexpr factory paths (from_octets / from_be32)
///   - round-trip parse / to_string for canonical inputs
///   - strict rejection of malformed dotted-quad strings
///   - equality / inequality
///   - SocketAddr composition

#include <gtest/gtest.h>

#include "eph/net/socket_addr.hpp"

namespace en = eph::net;

// ---------------------------------------------------------------------------
// Ipv4Addr — construction
// ---------------------------------------------------------------------------

TEST(Ipv4Addr, DefaultIsAllZero) {
    en::Ipv4Addr a;
    EXPECT_EQ(a.octets[0], 0);
    EXPECT_EQ(a.octets[1], 0);
    EXPECT_EQ(a.octets[2], 0);
    EXPECT_EQ(a.octets[3], 0);
    EXPECT_EQ(a.to_string(), "0.0.0.0");
}

TEST(Ipv4Addr, FromOctetsCaptureOrder) {
    constexpr auto a = en::Ipv4Addr::from_octets(192, 168, 1, 42);
    static_assert(a.octets[0] == 192);
    static_assert(a.octets[3] == 42);
    EXPECT_EQ(a.to_string(), "192.168.1.42");
}

TEST(Ipv4Addr, FromBe32RoundTrip) {
    // 10.0.0.1 as big-endian: 0x0A000001
    constexpr auto a = en::Ipv4Addr::from_be32(0x0A000001u);
    static_assert(a.octets[0] == 10);
    static_assert(a.octets[3] == 1);
    EXPECT_EQ(a.to_be32(), 0x0A000001u);
    EXPECT_EQ(a.to_string(), "10.0.0.1");
}

// ---------------------------------------------------------------------------
// Ipv4Addr — parse (happy path)
// ---------------------------------------------------------------------------

TEST(Ipv4Addr, ParseCanonical) {
    auto r = en::Ipv4Addr::parse("192.168.0.1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->octets[0], 192);
    EXPECT_EQ(r->octets[3], 1);
}

TEST(Ipv4Addr, ParseEdges) {
    auto zero = en::Ipv4Addr::parse("0.0.0.0");
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(*zero, en::Ipv4Addr(0, 0, 0, 0));

    auto max = en::Ipv4Addr::parse("255.255.255.255");
    ASSERT_TRUE(max.has_value());
    EXPECT_EQ(*max, en::Ipv4Addr(255, 255, 255, 255));
}

TEST(Ipv4Addr, ParseToStringRoundTrip) {
    const char* cases[] = {
        "0.0.0.0", "1.2.3.4", "127.0.0.1", "10.20.30.40", "255.255.255.255",
    };
    for (auto* s : cases) {
        auto parsed = en::Ipv4Addr::parse(s);
        ASSERT_TRUE(parsed.has_value()) << "failed to parse " << s;
        EXPECT_EQ(parsed->to_string(), s);
    }
}

// ---------------------------------------------------------------------------
// Ipv4Addr — parse (error paths)
// ---------------------------------------------------------------------------

TEST(Ipv4Addr, ParseRejectsEmpty) {
    auto r = en::Ipv4Addr::parse("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(Ipv4Addr, ParseRejectsOutOfRangeOctet) {
    for (const char* s : {"256.0.0.0", "1.2.3.300", "999.0.0.0"}) {
        auto r = en::Ipv4Addr::parse(s);
        ASSERT_FALSE(r.has_value()) << "should reject " << s;
        EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    }
}

TEST(Ipv4Addr, ParseRejectsMissingComponents) {
    for (const char* s : {"1.2.3", "1.2", "1", "1.2.3.4.5"}) {
        auto r = en::Ipv4Addr::parse(s);
        ASSERT_FALSE(r.has_value()) << "should reject " << s;
    }
}

TEST(Ipv4Addr, ParseRejectsEmptyComponents) {
    for (const char* s : {"1..2.3", ".1.2.3", "1.2.3.", "1.2..3"}) {
        auto r = en::Ipv4Addr::parse(s);
        ASSERT_FALSE(r.has_value()) << "should reject " << s;
    }
}

TEST(Ipv4Addr, ParseRejectsNonDigitCharacters) {
    for (const char* s : {"1.2.3.a", "1.2.3.+4", "1.2.3. 4", " 1.2.3.4",
                          "1.2.3.4 ", "0x1.0.0.0"}) {
        auto r = en::Ipv4Addr::parse(s);
        ASSERT_FALSE(r.has_value()) << "should reject " << s;
    }
}

TEST(Ipv4Addr, ParseRejectsTooManyDigits) {
    // "2550" has 4 digits, invalid even though numerically in range after
    // leading zero: we reject anything > 3 digits per component.
    auto r = en::Ipv4Addr::parse("1.2.3.0001");
    ASSERT_FALSE(r.has_value());
}

// ---------------------------------------------------------------------------
// Security: leading zeros must be rejected (CVE-2021-29921 class).
//
// Different IPv4 parsers disagree on what `01.2.3.4` means: `inet_aton(3)`
// treats "01" as octal `1`, modern Python (>=3.9.5) and Go reject it
// outright, and a naive decimal parser (like ours pre-fix) silently
// accepts it as decimal `1`. The disagreement is the bug — when one
// component of a stack agrees a payload identifies host A while another
// resolves it to host B, attackers chain SSRF / auth-bypass primitives
// across the boundary. Rejecting leading zeros eliminates the parser-
// disagreement attack surface entirely.
//
// "0" alone is the only legitimate way for a component to start with
// '0'; anything longer that begins with '0' (e.g. "01", "007", "010")
// must be rejected.
// ---------------------------------------------------------------------------
TEST(Ipv4Addr, ParseRejectsLeadingZeroOctet) {
    for (const char* s : {
            // Two-digit leading zero in each position.
            "01.2.3.4", "1.02.3.4", "1.2.03.4", "1.2.3.04",
            // Three-digit leading zero forms (some are octal-valid like
            // "007"=7 in inet_aton, "010"=8, "0177"=127 — the disagreement
            // is exactly the attack we're closing).
            "001.2.3.4", "010.20.30.40", "007.0.0.1",
            // Edge: every octet starting with '0' but not just "0".
            "01.02.03.04",
        }) {
        auto r = en::Ipv4Addr::parse(s);
        ASSERT_FALSE(r.has_value())
            << "leading zero must be rejected (CVE-2021-29921 class): " << s;
        EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    }
}

// "0" alone is the canonical zero octet and must keep working.
TEST(Ipv4Addr, ParseAcceptsBareZeroOctet) {
    for (const char* s : {"0.0.0.0", "0.1.2.3", "1.0.2.3", "1.2.0.3", "1.2.3.0"}) {
        auto r = en::Ipv4Addr::parse(s);
        ASSERT_TRUE(r.has_value()) << s;
    }
}

// ---------------------------------------------------------------------------
// Ipv4Addr — equality
// ---------------------------------------------------------------------------

TEST(Ipv4Addr, EqualityAndInequality) {
    en::Ipv4Addr a{1, 2, 3, 4};
    en::Ipv4Addr b{1, 2, 3, 4};
    en::Ipv4Addr c{1, 2, 3, 5};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// ---------------------------------------------------------------------------
// SocketAddr
// ---------------------------------------------------------------------------

TEST(SocketAddr, ConstructionAndToString) {
    constexpr en::SocketAddr sa{en::Ipv4Addr{127, 0, 0, 1}, 8080};
    EXPECT_EQ(sa.port, 8080);
    EXPECT_EQ(sa.ip.to_string(), "127.0.0.1");
    EXPECT_EQ(sa.to_string(), "127.0.0.1:8080");
}

TEST(SocketAddr, Equality) {
    en::SocketAddr a{en::Ipv4Addr{10, 0, 0, 1}, 443};
    en::SocketAddr b{en::Ipv4Addr{10, 0, 0, 1}, 443};
    en::SocketAddr c{en::Ipv4Addr{10, 0, 0, 1}, 444};
    en::SocketAddr d{en::Ipv4Addr{10, 0, 0, 2}, 443};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
}

// Default-constructed SocketAddr must be 0.0.0.0:0 — the documented
// "not yet bound" sentinel that callers test with `port == 0`.
TEST(SocketAddr, DefaultConstructedIsAllZero) {
    en::SocketAddr sa{};
    EXPECT_EQ(sa.port, 0);
    EXPECT_EQ(sa.ip.to_string(), "0.0.0.0");
    EXPECT_EQ(sa.to_string(), "0.0.0.0:0");
}

// Boundary: port 0 prints as ":0" not omitted.
TEST(SocketAddr, ToStringWithPortZeroIsExplicit) {
    en::SocketAddr sa{en::Ipv4Addr{1, 2, 3, 4}, 0};
    EXPECT_EQ(sa.to_string(), "1.2.3.4:0");
}

// Boundary: port at uint16 max — ensures std::to_string handles 65535
// without truncation.
TEST(SocketAddr, ToStringWithPortMaxIsFiveDigits) {
    en::SocketAddr sa{en::Ipv4Addr{255, 255, 255, 255}, 65535};
    EXPECT_EQ(sa.to_string(), "255.255.255.255:65535");
}

// Ipv4Addr::operator== must distinguish all four octets.
TEST(Ipv4Addr, EqualityChecksAllOctets) {
    en::Ipv4Addr base{10, 20, 30, 40};
    EXPECT_EQ(base, (en::Ipv4Addr{10, 20, 30, 40}));
    EXPECT_NE(base, (en::Ipv4Addr{11, 20, 30, 40}));
    EXPECT_NE(base, (en::Ipv4Addr{10, 21, 30, 40}));
    EXPECT_NE(base, (en::Ipv4Addr{10, 20, 31, 40}));
    EXPECT_NE(base, (en::Ipv4Addr{10, 20, 30, 41}));
}

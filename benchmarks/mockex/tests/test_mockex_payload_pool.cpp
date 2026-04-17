/// @file tests/test_mockex_payload_pool.cpp
/// Unit tests for PayloadPool fixture loading and in-place T-field
/// patching. No child process spawned — pure header-only testing.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

#include "mockex/payload_pool.hpp"

namespace {

/// Write a JSONL fixture to /tmp and return its path.
std::string write_fixture(std::string_view contents) {
    const std::string path = "/tmp/mockex_pool_test_" +
                             std::to_string(::getpid()) + ".jsonl";
    std::ofstream f(path);
    f << contents;
    return path;
}

/// Count the `"T":<digits>` digit run starting at the first match.
size_t first_t_field_width(std::string_view s) {
    const size_t p = s.find("\"T\":");
    if (p == std::string_view::npos) return 0;
    size_t q = p + 4;
    size_t n = 0;
    while (q < s.size() && s[q] >= '0' && s[q] <= '9') {
        ++q; ++n;
    }
    return n;
}

} // namespace

TEST(PayloadPool, LoadsAndRotatesThroughSamples) {
    // Three frames, each with a 19-digit (padded) T value — matches
    // what the capture tool will emit for a realistic monotonic ns.
    const std::string fixture =
        R"({"e":"bookTicker","s":"BTCUSDT","b":"50000","a":"50001","T":0000000000000000001})" "\n"
        R"({"e":"bookTicker","s":"ETHUSDT","b":"3000","a":"3001","T":0000000000000000002})" "\n"
        R"({"e":"bookTicker","s":"SOLUSDT","b":"100","a":"101","T":0000000000000000003})" "\n";
    const auto path = write_fixture(fixture);

    auto pool_e = mockex::PayloadPool::load_from_jsonl(path);
    ASSERT_TRUE(pool_e.has_value()) << pool_e.error();
    auto& pool = *pool_e;
    EXPECT_EQ(pool.size(), 3u);

    // Distinct symbols on rotation → confirms we wrap correctly.
    auto a = pool.stamp_and_next(111);
    auto b = pool.stamp_and_next(222);
    auto c = pool.stamp_and_next(333);
    auto d = pool.stamp_and_next(444);  // wraps back to sample 0

    std::string_view sa(reinterpret_cast<const char*>(a.data()), a.size());
    std::string_view sb(reinterpret_cast<const char*>(b.data()), b.size());
    std::string_view sc(reinterpret_cast<const char*>(c.data()), c.size());
    std::string_view sd(reinterpret_cast<const char*>(d.data()), d.size());

    EXPECT_NE(sa.find("BTCUSDT"), std::string_view::npos);
    EXPECT_NE(sb.find("ETHUSDT"), std::string_view::npos);
    EXPECT_NE(sc.find("SOLUSDT"), std::string_view::npos);
    // Rotation wraps — fourth call uses sample 0 again; but t field
    // updates to 444, so the returned bytes differ from `a` by that
    // field only.
    EXPECT_NE(sd.find("BTCUSDT"), std::string_view::npos);

    std::remove(path.c_str());
}

TEST(PayloadPool, TFieldIsStampedInPlace) {
    const std::string fixture =
        R"({"e":"bookTicker","s":"BTCUSDT","T":0000000000000000001})" "\n";
    const auto path = write_fixture(fixture);
    auto pool = mockex::PayloadPool::load_from_jsonl(path).value();

    const uint64_t stamp = 1'710'000'123'456'789ull;
    auto bytes = pool.stamp_and_next(stamp);
    std::string_view s(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());

    // Padded to exactly the reserved width (19 here).
    const std::string expected = "\"T\":0001710000123456789}";
    EXPECT_NE(s.find(expected), std::string_view::npos)
        << "s=" << s;
    EXPECT_EQ(first_t_field_width(s), 19u);
}

TEST(PayloadPool, RejectsLinesWithoutTField) {
    const std::string fixture =
        R"({"e":"bookTicker","T":0000000000000000001})" "\n"
        R"({"e":"trade","p":"100"})" "\n"                        // no T
        R"({"e":"bookTicker","T":0000000000000000002})" "\n";
    const auto path = write_fixture(fixture);
    auto pool = mockex::PayloadPool::load_from_jsonl(path).value();
    EXPECT_EQ(pool.size(), 2u)
        << "line without \"T\" should be dropped, leaving 2 valid";
    std::remove(path.c_str());
}

TEST(PayloadPool, RejectsShortTFieldWidth) {
    // "T":42 has only 2 digits, below our min 19-digit requirement.
    // Line-level rejection logs a WARN and the pool ends up empty,
    // bubbling up as the "no valid payloads" sentinel — this keeps
    // load errors in one shape regardless of the underlying reason.
    const std::string fixture = R"({"e":"x","T":42})" "\n";
    const auto path = write_fixture(fixture);
    auto pool = mockex::PayloadPool::load_from_jsonl(path);
    ASSERT_FALSE(pool.has_value());
    EXPECT_NE(pool.error().find("no valid payloads"), std::string::npos);
    std::remove(path.c_str());
}

TEST(PayloadPool, EmptyFixtureIsError) {
    const auto path = write_fixture("");
    auto pool = mockex::PayloadPool::load_from_jsonl(path);
    ASSERT_FALSE(pool.has_value());
    EXPECT_NE(pool.error().find("no valid payloads"), std::string::npos);
    std::remove(path.c_str());
}

TEST(PayloadPool, LoadsCheckedInSyntheticFixtures) {
    // The real artifacts shipped in benchmarks/mockex/fixtures/ must
    // round-trip through the loader — this locks the synth_fixture.py
    // output format to PayloadPool's expectations.
    // MOCKEX_FIXTURES_DIR is injected by benchmarks/mockex/xmake.lua so
    // the test works regardless of xmake's cwd (which it resets to the
    // binary directory at run time).
    for (const char* name : {
            "ex_market_sample.jsonl",
            "ex_market_2p_sample.jsonl",
         }) {
        const std::string path = std::string{MOCKEX_FIXTURES_DIR} + "/" + name;
        auto pool = mockex::PayloadPool::load_from_jsonl(path);
        ASSERT_TRUE(pool.has_value()) << path << ": " << pool.error();
        EXPECT_GT(pool->size(), 0u) << path;
    }
}

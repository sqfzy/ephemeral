/// @file tests/unit/bench/test_load_bench_conf.cpp
/// Unit tests for `bench::load_bench_conf` (stage 3 of bench simplify).
///
/// Each test writes a temporary `bench.conf` to a fresh directory and
/// points `BENCH_CONFIG` at it, so tests are hermetic regardless of the
/// real file under benchmarks/latency/.

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "core/config.hpp"

using namespace bench;
namespace fs = std::filesystem;

namespace {

// RAII fixture: creates a unique tmp dir, writes a `bench.conf` with the
// given body, sets $BENCH_CONFIG to its absolute path, and cleans up on
// destruction. Tests should use one fixture per case to avoid env leakage.
struct TmpConf {
    fs::path dir;
    fs::path conf;

    explicit TmpConf(std::string_view body) {
        dir  = fs::temp_directory_path() /
               ("bench_conf_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir);
        conf = dir / "bench.conf";
        std::ofstream out{conf};
        out << body;
        out.close();
        ::setenv("BENCH_CONFIG", conf.c_str(), /*overwrite=*/1);
    }
    ~TmpConf() {
        ::unsetenv("BENCH_CONFIG");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

constexpr const char* kFullValid = R"(
# A complete, valid bench.conf for the happy-path test.
NIC_A=ens34
NIC_B=ens35
SERVER_IP=10.0.0.1
LOCAL_IP=10.0.0.2
GATEWAY_IP=10.0.0.254
CLIENT_CPU=2
MOCK_CPU=4
EAL_CORES=0,1
ALLOW_NON_ISOLATED=true
WARMUP=3
DURATION=15
SERVER_WORK_NS=500
TCP_PAYLOADS=64,128,1460
UDP_PAYLOADS=64,1472
WS_PAYLOADS=64,256,4096
MD_UDP_PAYLOADS=64,1400
INFLIGHTS=1,4,16
SYMBOLS=BTCUSDT,ETHUSDT
BOOKTICKER_US=2000
DEPTH_MS=20
TRADE_MEAN_MS=8
KLINE_S=2
DEPTH_BYTES=2048
)";

} // namespace

// ── happy path ──────────────────────────────────────────────────────────

TEST(LoadBenchConf, ValidConfigParsesAllFields) {
    TmpConf t{kFullValid};
    auto r = load_bench_conf();
    ASSERT_TRUE(r.has_value()) << r.error();
    const auto& c = *r;

    EXPECT_EQ(c.nic_a, "ens34");
    EXPECT_EQ(c.nic_b, "ens35");
    EXPECT_EQ(c.server_ip, "10.0.0.1");
    EXPECT_EQ(c.local_ip, "10.0.0.2");
    EXPECT_EQ(c.gateway_ip, "10.0.0.254");
    EXPECT_EQ(c.client_cpu, 2);
    EXPECT_EQ(c.mock_cpu, 4);
    EXPECT_EQ(c.eal_cores, "0,1");
    EXPECT_TRUE(c.allow_non_isolated);
    EXPECT_EQ(c.warmup.count(), 3);
    EXPECT_EQ(c.duration.count(), 15);
    EXPECT_EQ(c.server_work_ns, 500);

    ASSERT_EQ(c.tcp_payloads.size(), 3u);
    EXPECT_EQ(c.tcp_payloads[0], 64u);
    EXPECT_EQ(c.tcp_payloads[2], 1460u);
    ASSERT_EQ(c.udp_payloads.size(), 2u);
    ASSERT_EQ(c.ws_payloads.size(), 3u);
    ASSERT_EQ(c.md_udp_payloads.size(), 2u);
    ASSERT_EQ(c.inflights.size(), 3u);
    EXPECT_EQ(c.inflights[0], 1);
    EXPECT_EQ(c.inflights[2], 16);

    ASSERT_EQ(c.symbols.size(), 2u);
    EXPECT_EQ(c.symbols[0], "BTCUSDT");
    EXPECT_EQ(c.symbols[1], "ETHUSDT");

    EXPECT_EQ(c.bookticker_us, 2000);
    EXPECT_EQ(c.depth_ms, 20);
    EXPECT_EQ(c.trade_mean_ms, 8);
    EXPECT_EQ(c.kline_s, 2);
    EXPECT_EQ(c.depth_bytes, 2048u);
}

// ── required-field validation ───────────────────────────────────────────

TEST(LoadBenchConf, MissingRequiredFieldReturnsError) {
    // SERVER_IP omitted.
    constexpr const char* body = R"(
NIC_B=ens35
LOCAL_IP=10.0.0.2
GATEWAY_IP=10.0.0.254
CLIENT_CPU=2
MOCK_CPU=4
)";
    TmpConf t{body};
    auto r = load_bench_conf();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("SERVER_IP"), std::string::npos)
        << "error message should mention the missing field, got: " << r.error();
}

TEST(LoadBenchConf, MissingClientCpuReturnsError) {
    constexpr const char* body = R"(
NIC_B=ens35
SERVER_IP=10.0.0.1
LOCAL_IP=10.0.0.2
GATEWAY_IP=10.0.0.254
MOCK_CPU=4
)";
    TmpConf t{body};
    auto r = load_bench_conf();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("CLIENT_CPU"), std::string::npos)
        << "got: " << r.error();
}

// ── parsing edge cases ──────────────────────────────────────────────────

TEST(LoadBenchConf, CommentsAndBlankLinesIgnored) {
    constexpr const char* body = R"(
# top-level comment
   # indented comment

NIC_B=ens35
SERVER_IP=10.0.0.1   # inline comment after value
LOCAL_IP=10.0.0.2
GATEWAY_IP=10.0.0.254
CLIENT_CPU=2
MOCK_CPU=4
)";
    TmpConf t{body};
    auto r = load_bench_conf();
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->server_ip, "10.0.0.1");
}

TEST(LoadBenchConf, CsvPayloadListsParseCorrectly) {
    constexpr const char* body = R"(
NIC_B=ens35
SERVER_IP=10.0.0.1
LOCAL_IP=10.0.0.2
GATEWAY_IP=10.0.0.254
CLIENT_CPU=2
MOCK_CPU=4
TCP_PAYLOADS=64,128,256,512,1024,1460,4096,16384
INFLIGHTS=1,4,16,64
)";
    TmpConf t{body};
    auto r = load_bench_conf();
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->tcp_payloads.size(), 8u);
    EXPECT_EQ(r->tcp_payloads.front(), 64u);
    EXPECT_EQ(r->tcp_payloads.back(), 16384u);
    ASSERT_EQ(r->inflights.size(), 4u);
    EXPECT_EQ(r->inflights.back(), 64);
}

TEST(LoadBenchConf, UnknownKeysSilentlyIgnored) {
    // Forward compat: a binary built before NEW_FUTURE_KEY existed must
    // still parse a config that contains it.
    constexpr const char* body = R"(
NIC_B=ens35
SERVER_IP=10.0.0.1
LOCAL_IP=10.0.0.2
GATEWAY_IP=10.0.0.254
CLIENT_CPU=2
MOCK_CPU=4
NEW_FUTURE_KEY=hello
)";
    TmpConf t{body};
    auto r = load_bench_conf();
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}

// ── error: file does not exist ──────────────────────────────────────────

TEST(LoadBenchConf, MissingFileReturnsError) {
    ::setenv("BENCH_CONFIG", "/nonexistent/path/to/bench.conf", 1);
    auto r = load_bench_conf();
    ::unsetenv("BENCH_CONFIG");
    EXPECT_FALSE(r.has_value());
}

// ─────────────────────────────────────────────────────────────────────────
// ScenarioConfig — Phase 10 INI [lat_*] section parser
// ─────────────────────────────────────────────────────────────────────────
//
// Unlike LoadBenchConf, these tests construct the tmp file path via
// TmpConf but reach through `t.conf.c_str()` directly — ScenarioConfig
// takes an explicit path, not the $BENCH_CONFIG discovery chain.

namespace {

constexpr const char* kMultiSection = R"(
# global defaults
NIC_A=ens34
warmup_samples=1000

[lat_tcp]
port            = 20000
payload_size    = 256
duration_seconds = 10

[lat_udp]
port            = 20001
payload_size    = 512
duration_seconds = 20

[lat_ws]
port            = 20002
ws_path         = /echo
payload_size    = 128
)";

} // namespace

TEST(ScenarioConfig, LoadsSimpleSection) {
    TmpConf t{kMultiSection};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->size(), 3u);
    EXPECT_TRUE(r->has("port"));
    EXPECT_TRUE(r->has("payload_size"));
    EXPECT_TRUE(r->has("duration_seconds"));
}

TEST(ScenarioConfig, MissingSectionReturnsEmpty) {
    // Not an error — caller is expected to call get_u32() and fail there
    // if a required key is missing.
    TmpConf t{kMultiSection};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_nonexistent");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->size(), 0u);
    EXPECT_FALSE(r->has("port"));
}

TEST(ScenarioConfig, GetU32ReturnsParsedValue) {
    TmpConf t{kMultiSection};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    auto port = r.get_u32("port");
    ASSERT_TRUE(port.has_value()) << port.error();
    EXPECT_EQ(*port, 20000u);
}

TEST(ScenarioConfig, GetU32WithDefaultFallsBack) {
    TmpConf t{kMultiSection};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    auto inflight = r.get_u32("inflight", 42);
    ASSERT_TRUE(inflight.has_value());
    EXPECT_EQ(*inflight, 42u); // key not present → default
}

TEST(ScenarioConfig, GetU32ErrorOnMissingRequiredKey) {
    TmpConf t{kMultiSection};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    auto missing = r.get_u32("not_there");
    EXPECT_FALSE(missing.has_value());
}

TEST(ScenarioConfig, GetU32ErrorOnMalformedValue) {
    constexpr const char* body = R"(
[lat_tcp]
port = abc
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    auto port = r.get_u32("port");
    EXPECT_FALSE(port.has_value());
}

TEST(ScenarioConfig, GetU32OverflowDetected) {
    constexpr const char* body = R"(
[lat_tcp]
port = 9999999999
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    auto port = r.get_u32("port");
    EXPECT_FALSE(port.has_value());
}

TEST(ScenarioConfig, GetU64ParsesLargeValue) {
    constexpr const char* body = R"(
[lat_ex_market]
push_rate_hz = 1000000000000
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_ex_market").value();
    auto rate = r.get_u64("push_rate_hz");
    ASSERT_TRUE(rate.has_value());
    EXPECT_EQ(*rate, 1'000'000'000'000ull);
}

TEST(ScenarioConfig, GetDoubleParsesFloat) {
    constexpr const char* body = R"(
[lat_custom]
ratio = 0.75
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_custom").value();
    auto v = r.get_double("ratio");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 0.75);
}

TEST(ScenarioConfig, GetStringWithDefault) {
    TmpConf t{kMultiSection};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_ws").value();
    EXPECT_EQ(r.get_string("ws_path"), "/echo");
    EXPECT_EQ(r.get_string("missing_key", "default_val"), "default_val");
}

TEST(ScenarioConfig, HasReturnsTrueOnlyForPresentKeys) {
    TmpConf t{kMultiSection};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    EXPECT_TRUE(r.has("port"));
    EXPECT_FALSE(r.has("not_there"));
}

TEST(ScenarioConfig, SectionIsolationMultiSection) {
    // lat_tcp should NOT see lat_udp's keys (different payload_size).
    TmpConf t{kMultiSection};
    auto tcp = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    auto udp = ScenarioConfig::load(t.conf.c_str(), "lat_udp").value();
    EXPECT_EQ(tcp.get_u32("payload_size").value(), 256u);
    EXPECT_EQ(udp.get_u32("payload_size").value(), 512u);
    EXPECT_EQ(tcp.get_u32("duration_seconds").value(), 10u);
    EXPECT_EQ(udp.get_u32("duration_seconds").value(), 20u);
}

TEST(ScenarioConfig, CommentsSkippedInsideSection) {
    constexpr const char* body = R"(
[lat_tcp]
# first comment
port = 20000
  # indented comment
payload_size = 256
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    EXPECT_EQ(r.size(), 2u);
    EXPECT_EQ(r.get_u32("port").value(), 20000u);
    EXPECT_EQ(r.get_u32("payload_size").value(), 256u);
}

TEST(ScenarioConfig, WhitespaceToleranceAroundEquals) {
    constexpr const char* body = R"(
[lat_tcp]
a=1
b =2
c= 3
   d   =   4
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    EXPECT_EQ(r.size(), 4u);
    EXPECT_EQ(r.get_u32("a").value(), 1u);
    EXPECT_EQ(r.get_u32("b").value(), 2u);
    EXPECT_EQ(r.get_u32("c").value(), 3u);
    EXPECT_EQ(r.get_u32("d").value(), 4u);
}

TEST(ScenarioConfig, EmptySectionReturnsEmpty) {
    constexpr const char* body = R"(
[lat_tcp]
[lat_udp]
port = 20001
)";
    TmpConf t{body};
    auto tcp = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    auto udp = ScenarioConfig::load(t.conf.c_str(), "lat_udp").value();
    EXPECT_EQ(tcp.size(), 0u);
    EXPECT_EQ(udp.size(), 1u);
    EXPECT_EQ(udp.get_u32("port").value(), 20001u);
}

TEST(ScenarioConfig, ExactSectionNameMatch) {
    // `lat_tcp` must not match `lat_tcp_variant`.
    constexpr const char* body = R"(
[lat_tcp_variant]
port = 30000

[lat_tcp]
port = 20000
)";
    TmpConf t{body};
    auto a = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    auto b = ScenarioConfig::load(t.conf.c_str(), "lat_tcp_variant").value();
    EXPECT_EQ(a.get_u32("port").value(), 20000u);
    EXPECT_EQ(b.get_u32("port").value(), 30000u);
}

TEST(ScenarioConfig, LoadFailsOnMissingFile) {
    auto r = ScenarioConfig::load("/nonexistent/path.conf", "lat_tcp");
    EXPECT_FALSE(r.has_value());
}

TEST(ScenarioConfig, MalformedSectionHeaderIsError) {
    constexpr const char* body = R"(
[lat_tcp
port = 20000
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp");
    EXPECT_FALSE(r.has_value());
}

TEST(ScenarioConfig, MissingEqualsInSectionIsError) {
    constexpr const char* body = R"(
[lat_tcp]
port 20000
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp");
    EXPECT_FALSE(r.has_value());
}

TEST(ScenarioConfig, GlobalKeysIgnored) {
    // Keys outside any section must NOT be collected into ScenarioConfig
    // (they belong to CommonConfig/BenchConfig).
    constexpr const char* body = R"(
global_key = 1
NIC_A = ens34

[lat_tcp]
port = 20000
)";
    TmpConf t{body};
    auto r = ScenarioConfig::load(t.conf.c_str(), "lat_tcp").value();
    EXPECT_EQ(r.size(), 1u);
    EXPECT_FALSE(r.has("global_key"));
    EXPECT_FALSE(r.has("NIC_A"));
    EXPECT_TRUE(r.has("port"));
}

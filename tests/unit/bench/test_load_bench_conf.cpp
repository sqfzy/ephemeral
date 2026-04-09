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

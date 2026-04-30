/// @file tests/unit/bench/test_bench_conf.cpp
/// Contract tests for the toml++-backed `bench::load_bench_conf` loader.
///
/// Replaces the legacy `test_load_bench_conf.cpp` which exercised the
/// bash-style BenchConfig parser. Covers each ConfigError branch,
/// required-field fail-loud, optional defaults, scenario access
/// (present / missing / wrong type), and the generic `load_toml_table`
/// helper used by MMPP params.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/bench_conf.hpp"

using namespace bench;
namespace fs = std::filesystem;

namespace {

// RAII fixture: writes the given TOML body to a unique tmp file, exposes
// the absolute path, cleans up on destruction.
struct TmpToml {
    fs::path path;

    explicit TmpToml(std::string_view body) {
        path = fs::temp_directory_path() /
               ("bench_conf_test_" +
                std::to_string(::getpid()) + "_" +
                std::to_string(next_id()) + ".toml");
        std::ofstream f(path);
        f << body;
    }
    ~TmpToml() {
        std::error_code ec;
        fs::remove(path, ec);
    }

    static int next_id() {
        static int counter = 0;
        return ++counter;
    }
};

// Minimal valid config — every required field, nothing more.
constexpr std::string_view kMinimalValid = R"(
[networking]
nic_a      = "ens34"
nic_b      = "ens35"
server_ip  = "10.0.0.1"
client_ip  = "10.0.0.2"
gateway_ip = "10.0.0.254"

[cpu]
cpu_client = 4
cpu_mock   = 6
)";

} // namespace

// ─── ConfigError branches ──────────────────────────────────────────────

TEST(BenchConf, FileNotFoundOnEmptyPath) {
    auto r = load_bench_conf("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ConfigError::FileNotFound);
}

TEST(BenchConf, FileNotFoundOnMissingFile) {
    auto r = load_bench_conf("/nonexistent/path/nope.toml");
    ASSERT_FALSE(r.has_value());
    // toml++ reports this as either FileNotFound or ParseFailed
    // depending on the platform-layer error path; accept either.
    EXPECT_TRUE(r.error().code == ConfigError::FileNotFound ||
                r.error().code == ConfigError::ParseFailed);
}

TEST(BenchConf, ParseFailedOnSyntaxError) {
    TmpToml f{"[networking\nnic_a = not-closed"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ConfigError::ParseFailed);
    EXPECT_GT(r.error().line, 0);
}

TEST(BenchConf, MissingRequiredKey_NicB) {
    TmpToml f{R"(
[networking]
nic_a      = "ens34"
server_ip  = "1.2.3.4"
client_ip  = "1.2.3.5"
gateway_ip = "1.2.3.254"

[cpu]
cpu_client = 0
cpu_mock   = 1
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ConfigError::MissingRequiredKey);
    EXPECT_NE(r.error().key.find("nic_b"), std::string::npos);
}

TEST(BenchConf, MissingRequiredKey_CpuClient) {
    TmpToml f{R"(
[networking]
nic_a      = "ens34"
nic_b      = "ens35"
server_ip  = "1.2.3.4"
client_ip  = "1.2.3.5"
gateway_ip = "1.2.3.254"

[cpu]
cpu_mock = 1
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ConfigError::MissingRequiredKey);
    EXPECT_NE(r.error().key.find("cpu_client"), std::string::npos);
}

// ─── Happy path ────────────────────────────────────────────────────────

TEST(BenchConf, MinimalValidLoads) {
    TmpToml f{kMinimalValid};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());

    const auto& cfg = *r;
    EXPECT_EQ(cfg.networking.nic_a, "ens34");
    EXPECT_EQ(cfg.networking.nic_b, "ens35");
    EXPECT_EQ(cfg.networking.server_ip, "10.0.0.1");
    EXPECT_EQ(cfg.networking.client_ip, "10.0.0.2");
    EXPECT_EQ(cfg.networking.gateway_ip, "10.0.0.254");
    EXPECT_EQ(cfg.cpu.cpu_client, 4);
    EXPECT_EQ(cfg.cpu.cpu_mock, 6);

    // Defaults filled in for absent sections.
    EXPECT_EQ(cfg.measurement.warmup_samples, 1000u);
    EXPECT_EQ(cfg.cpu.eal_cores, "0,1");
    EXPECT_FALSE(cfg.cpu.allow_non_isolated);
    EXPECT_EQ(cfg.dpdk.nb_rx_queues, 1u);

    EXPECT_TRUE(cfg.scenario_names().empty());
    EXPECT_EQ(cfg.scenario("lat_tcp"), nullptr);
}

TEST(BenchConf, SectionsAndScenariosPopulated) {
    TmpToml f{R"(
[networking]
nic_a      = "ens34"
nic_b      = "ens35"
server_ip  = "1.2.3.4"
client_ip  = "1.2.3.5"
gateway_ip = "1.2.3.254"
netmask    = "255.255.240.0"
nic_b_pci  = "0000:28:00.0"

[cpu]
cpu_client         = 4
cpu_mock           = 6
eal_cores          = "2,3"
allow_non_isolated = true

[measurement]
warmup_samples = 2048
server_work_ns = 500

[dpdk.rss]
nb_rx_queues    = 4
lcore_per_queue = "4,5,6,7"

[scenarios.lat_tcp]
port             = 20000
payload_size     = 256
duration_seconds = 30

[scenarios.lat_ex_order]
port      = 20004
inflights = [1, 4, 16, 64]
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    const auto& cfg = *r;

    EXPECT_EQ(cfg.networking.netmask, "255.255.240.0");
    EXPECT_EQ(cfg.networking.nic_b_pci, "0000:28:00.0");
    EXPECT_EQ(cfg.cpu.eal_cores, "2,3");
    EXPECT_TRUE(cfg.cpu.allow_non_isolated);
    EXPECT_EQ(cfg.measurement.warmup_samples, 2048u);
    EXPECT_EQ(cfg.measurement.server_work_ns, 500u);
    EXPECT_EQ(cfg.dpdk.nb_rx_queues, 4u);
    EXPECT_EQ(cfg.dpdk.lcore_per_queue, "4,5,6,7");

    // Scenario lookup
    const Scenario* tcp = cfg.scenario("lat_tcp");
    ASSERT_NE(tcp, nullptr);
    EXPECT_EQ(tcp->name(), "lat_tcp");
    EXPECT_TRUE(tcp->has("port"));
    EXPECT_FALSE(tcp->has("missing_key"));

    EXPECT_EQ(tcp->get<uint16_t>("port").value(), 20000u);
    EXPECT_EQ(tcp->get<uint32_t>("duration_seconds").value(), 30u);
    EXPECT_EQ(tcp->get_or<uint64_t>("duration_seconds", 99), 30u);
    EXPECT_EQ(tcp->get_or<uint64_t>("not_there", 42), 42u);

    // Arrays
    const Scenario* ord = cfg.scenario("lat_ex_order");
    ASSERT_NE(ord, nullptr);
    auto infl_r = ord->get<std::vector<int>>("inflights");
    ASSERT_TRUE(infl_r.has_value()) << format_error(infl_r.error());
    EXPECT_EQ(*infl_r, (std::vector<int>{1, 4, 16, 64}));
}

TEST(BenchConf, ScenarioRequiredKeyMissing) {
    TmpToml f{std::string{kMinimalValid} + R"(
[scenarios.lat_tcp]
payload_size = 256
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    auto tcp = r->scenario("lat_tcp");
    ASSERT_NE(tcp, nullptr);
    auto port_r = tcp->get<uint16_t>("port");
    ASSERT_FALSE(port_r.has_value());
    EXPECT_EQ(port_r.error().code, ConfigError::MissingRequiredKey);
    EXPECT_NE(port_r.error().key.find("scenarios.lat_tcp.port"),
              std::string::npos);
}

TEST(BenchConf, ScenarioWrongType) {
    TmpToml f{std::string{kMinimalValid} + R"(
[scenarios.lat_tcp]
port = "not-a-number"
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    auto tcp = r->scenario("lat_tcp");
    ASSERT_NE(tcp, nullptr);
    auto port_r = tcp->get<uint16_t>("port");
    ASSERT_FALSE(port_r.has_value());
    EXPECT_EQ(port_r.error().code, ConfigError::WrongType);
}

// Out-of-range integer values silently truncate via `static_cast<T>(int64)`.
// The caller asks for a uint16_t; if the user typo'd `port = 70000` (or
// `port = -1`), today's code returns 4464 / 65535 with no diagnostic.
// Both produce a confusing "wrong port" connect failure later.
//
// The fix-side commit will surface these as ConfigError::WrongType with
// an actionable detail string. Pinning here as a failing regression test.
TEST(BenchConf, ScenarioIntegerOutOfRangeSilentTruncation) {
    TmpToml f_high{std::string{kMinimalValid} + R"(
[scenarios.lat_tcp]
port = 70000
)"};
    auto r_high = load_bench_conf(f_high.path.string());
    ASSERT_TRUE(r_high.has_value()) << format_error(r_high.error());
    auto tcp_h = r_high->scenario("lat_tcp");
    ASSERT_NE(tcp_h, nullptr);
    auto port_high = tcp_h->get<uint16_t>("port");
    EXPECT_FALSE(port_high.has_value())
        << "uint16_t port=70000 must reject (overflow), got value="
        << (port_high ? *port_high : 0);

    TmpToml f_neg{std::string{kMinimalValid} + R"(
[scenarios.lat_tcp]
port = -1
)"};
    auto r_neg = load_bench_conf(f_neg.path.string());
    ASSERT_TRUE(r_neg.has_value()) << format_error(r_neg.error());
    auto tcp_n = r_neg->scenario("lat_tcp");
    ASSERT_NE(tcp_n, nullptr);
    auto port_neg = tcp_n->get<uint16_t>("port");
    EXPECT_FALSE(port_neg.has_value())
        << "uint16_t port=-1 must reject (underflow), got value="
        << (port_neg ? *port_neg : 0);
}

TEST(BenchConf, MovedBenchConfigKeepsScenariosValid) {
    TmpToml f{std::string{kMinimalValid} + R"(
[scenarios.lat_tcp]
port = 20000
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value());
    BenchConfig moved = std::move(*r);
    auto tcp = moved.scenario("lat_tcp");
    ASSERT_NE(tcp, nullptr);
    // Deref through the moved-to config — must not crash.
    EXPECT_EQ(tcp->get<uint16_t>("port").value(), 20000u);
}

// ─── ParallelConfig (runs[] schema) ────────────────────────────────────

TEST(BenchConf, ParallelConfigAbsent_EmptyRuns) {
    TmpToml f{kMinimalValid};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    EXPECT_TRUE(r->parallel.runs.empty());
}

TEST(BenchConf, ParallelConfigSingleRun) {
    TmpToml f{std::string{kMinimalValid} + R"(
[parallel]
runs = [
  { scenario = "lat_tcp", lcore = 1, cpu = 4, queue = 0 },
]
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    ASSERT_EQ(r->parallel.runs.size(), 1u);
    EXPECT_EQ(r->parallel.runs[0].scenario, "lat_tcp");
    EXPECT_EQ(r->parallel.runs[0].lcore, 1u);
    EXPECT_EQ(r->parallel.runs[0].cpu, 4);
    EXPECT_EQ(r->parallel.runs[0].queue, 0u);
}

TEST(BenchConf, ParallelConfigMultipleRuns) {
    TmpToml f{std::string{kMinimalValid} + R"(
[parallel]
runs = [
  { scenario = "lat_tcp",       lcore = 1, cpu = 4, queue = 0 },
  { scenario = "lat_udp",       lcore = 2, cpu = 5, queue = 1 },
  { scenario = "lat_ws",        lcore = 3, cpu = 6, queue = 2 },
  { scenario = "lat_ex_md_udp", lcore = 4, cpu = 7, queue = 3 },
]
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    ASSERT_EQ(r->parallel.runs.size(), 4u);
    EXPECT_EQ(r->parallel.runs[0].scenario, "lat_tcp");
    EXPECT_EQ(r->parallel.runs[1].scenario, "lat_udp");
    EXPECT_EQ(r->parallel.runs[2].scenario, "lat_ws");
    EXPECT_EQ(r->parallel.runs[3].scenario, "lat_ex_md_udp");
    EXPECT_EQ(r->parallel.runs[0].queue, 0u);
    EXPECT_EQ(r->parallel.runs[3].queue, 3u);
    EXPECT_EQ(r->parallel.runs[3].cpu, 7);
}

TEST(BenchConf, ParallelConfigSameScenarioMultipleQueues) {
    // Two lat_tcp runs on different queues — fan-out load test pattern.
    TmpToml f{std::string{kMinimalValid} + R"(
[parallel]
runs = [
  { scenario = "lat_tcp", lcore = 1, cpu = 4, queue = 0 },
  { scenario = "lat_tcp", lcore = 2, cpu = 5, queue = 1 },
]
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    ASSERT_EQ(r->parallel.runs.size(), 2u);
    EXPECT_EQ(r->parallel.runs[0].scenario, "lat_tcp");
    EXPECT_EQ(r->parallel.runs[1].scenario, "lat_tcp");
    EXPECT_NE(r->parallel.runs[0].queue, r->parallel.runs[1].queue);
}

TEST(BenchConf, ParallelConfigRowMissingScenario_Skipped) {
    // Row without `scenario` is silently dropped (validation error
    // surfaces later when lat_multi_dpdk validates the runs[] list).
    TmpToml f{std::string{kMinimalValid} + R"(
[parallel]
runs = [
  { scenario = "lat_tcp", lcore = 1, cpu = 4, queue = 0 },
  { lcore = 2, cpu = 5, queue = 1 },
  { scenario = "lat_udp", lcore = 3, cpu = 6, queue = 2 },
]
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    ASSERT_EQ(r->parallel.runs.size(), 2u);
    EXPECT_EQ(r->parallel.runs[0].scenario, "lat_tcp");
    EXPECT_EQ(r->parallel.runs[1].scenario, "lat_udp");
}

TEST(BenchConf, ParallelConfigEmptyRuns) {
    TmpToml f{std::string{kMinimalValid} + R"(
[parallel]
runs = []
)"};
    auto r = load_bench_conf(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    EXPECT_TRUE(r->parallel.runs.empty());
}

// ─── format_error ──────────────────────────────────────────────────────

TEST(BenchConf, FormatErrorContainsKeyAndCode) {
    ConfigErrorInfo err{ConfigError::MissingRequiredKey,
                        "cpu.cpu_client",
                        "required TOML key is absent",
                        "/tmp/some.toml",
                        42};
    auto msg = format_error(err);
    EXPECT_NE(msg.find("missing-required-key"), std::string::npos);
    EXPECT_NE(msg.find("cpu.cpu_client"), std::string::npos);
    EXPECT_NE(msg.find("/tmp/some.toml"), std::string::npos);
    EXPECT_NE(msg.find(":42"), std::string::npos);
}

// ─── load_toml_table ───────────────────────────────────────────────────

TEST(LoadTomlTable, FlatKVSucceeds) {
    // MMPP params style: flat TOML, no sections.
    TmpToml f{R"(
lambda_quiet_hz = 2.69916
lambda_busy_hz  = 44235.1
seed_default    = 42
size_kde_anchors = [140, 141, 141, 141]
)"};
    auto r = load_toml_table(f.path.string());
    ASSERT_TRUE(r.has_value()) << format_error(r.error());

    auto& tbl = *r;
    EXPECT_DOUBLE_EQ(tbl["lambda_quiet_hz"].value<double>().value_or(0.0), 2.69916);
    EXPECT_DOUBLE_EQ(tbl["lambda_busy_hz"].value<double>().value_or(0.0), 44235.1);
    EXPECT_EQ(tbl["seed_default"].value<int64_t>().value_or(0), 42);

    const toml::array* arr = tbl["size_kde_anchors"].as_array();
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->size(), 4u);
}

TEST(LoadTomlTable, EmptyPathIsError) {
    auto r = load_toml_table("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ConfigError::FileNotFound);
}

TEST(LoadTomlTable, SyntaxErrorReportsLine) {
    TmpToml f{"valid = 1\nbroken == 2\nfollow = 3"};
    auto r = load_toml_table(f.path.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ConfigError::ParseFailed);
    EXPECT_GT(r.error().line, 0);
}

// ─── Real checked-in config.toml + MMPP fixtures parse through loader ───
//
// xmake runs test binaries from the build output directory; resolve
// fixture paths through the BENCH_REPO_ROOT macro injected by the
// xmake.lua entry for this test (see tests/unit/bench/xmake.lua).

#ifdef BENCH_REPO_ROOT
TEST(RealFixtures, ShippedConfigTomlLoads) {
    auto r = load_bench_conf(BENCH_REPO_ROOT "/benchmarks/latency/config.toml");
    ASSERT_TRUE(r.has_value()) << format_error(r.error());
    const auto& cfg = *r;
    EXPECT_EQ(cfg.networking.nic_a, "ens34");
    EXPECT_EQ(cfg.networking.nic_b, "ens35");
    EXPECT_GT(cfg.cpu.cpu_client, 0);
    EXPECT_GT(cfg.cpu.cpu_mock, 0);

    auto names = cfg.scenario_names();
    // The shipped config carries the seven canonical scenarios documented
    // in benchmarks/latency/README.md plus the two RSS-scaling helpers
    // (lat_rss_scaling / lat_rss_scaling_ws) added in the worktree-multi-
    // stream-bench merge. The loader-level guarantee we need is "every
    // [scenarios.<name>] section becomes one scenario", so assert the
    // floor (the seven documented) and any extras the config may pick up
    // over time. Hard-coding "exactly 7" silently rotted as RSS scenarios
    // were added — keep the floor + completeness check, drop the rigid
    // count.
    EXPECT_GE(names.size(), 7u) << "loader dropped sections from config.toml";
    for (const auto* expected : {"lat_tcp", "lat_udp", "lat_ws",
                                 "lat_ex_market", "lat_ex_order",
                                 "lat_ex_md_udp", "lat_ex_market_2p"}) {
        EXPECT_NE(cfg.scenario(expected), nullptr) << "missing " << expected;
    }

    const auto* ord = cfg.scenario("lat_ex_order");
    ASSERT_NE(ord, nullptr);
    // `inflights` was removed from config.toml in commit d1fbe4a9 — the
    // bench driver only honours `duration_seconds` and never consumed
    // the array. Verify the section still exists with its real keys.
    EXPECT_TRUE(ord->get<uint32_t>("duration_seconds").has_value());
}

TEST(RealFixtures, MmppParamsTomlLoads) {
    for (const auto* rel : {
            "/benchmarks/mockex/fixtures/ex_market_params.toml",
            "/benchmarks/mockex/fixtures/ex_market_2p_params.toml",
         }) {
        std::string p = std::string{BENCH_REPO_ROOT} + rel;
        auto r = load_toml_table(p);
        ASSERT_TRUE(r.has_value()) << p << ": " << format_error(r.error());
        auto& tbl = *r;
        EXPECT_GT(tbl["lambda_quiet_hz"].value<double>().value_or(0.0), 0.0);
        EXPECT_GT(tbl["lambda_busy_hz"].value<double>().value_or(0.0), 0.0);
        EXPECT_EQ(tbl["seed_default"].value<int64_t>().value_or(-1), 42);
        const toml::array* anc = tbl["size_kde_anchors"].as_array();
        ASSERT_NE(anc, nullptr);
        EXPECT_EQ(anc->size(), 6u);
    }
}
#endif

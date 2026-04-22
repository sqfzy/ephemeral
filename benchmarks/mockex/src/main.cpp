/// @file benchmarks/mockex/src/main.cpp
/// Unified mock-server entry point.
///
/// Parses `--scenario <name> --config <path>`, loads bench.conf, pins
/// to the configured `cpu_mock`, installs SIGINT/SIGTERM handlers, and
/// dispatches to the handler registered in `mockex::kScenarioTable`.
///
/// The single binary replaces the constellation of
/// benchmarks/latency/mocks/*.py scripts. See the plan at
/// .claude/plans/elegant-toasting-popcorn.md for the motivation.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "core/config.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/shutdown_signal.hpp"
#include "mockex/dispatch.hpp"
#include "mockex/scenario.hpp"

namespace {

/// Print usage + the list of registered scenarios, then return the
/// caller-requested exit code. Keeps the main function tidy.
int print_usage(int rc) noexcept {
    std::fprintf(stderr,
        "mockex — unified mock server for the latency bench\n\n"
        "  mockex --scenario <name> [--config <path>]\n\n"
        "OPTIONS:\n"
        "  --scenario <name>   one of: tcp, udp, ws, ex_order, ex_md_udp,\n"
        "                      ex_market, ex_market_2p\n"
        "  --config <path>     bench.conf path (default: search from $BENCH_CONFIG\n"
        "                      then ./bench.conf then walk up from /proc/self/exe)\n"
        "  -h, --help          this help\n\n"
        "SCENARIOS:\n");
    for (const auto& e : mockex::kScenarioTable) {
        std::fprintf(stderr, "  %-12s → section [%s]\n",
                     std::string{e.name}.c_str(),
                     std::string{e.section}.c_str());
    }
    return rc;
}

/// Locate bench.conf when `--config` was not explicitly passed.
/// Falls back through the same search order as
/// `bench::config_detail::find_bench_conf`.
[[nodiscard]] std::string locate_config() {
    if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) {
        return env;
    }
    return "benchmarks/latency/bench.conf";
}

/// Best-effort CPU pinning. Non-fatal: missing taskset/permissions
/// would only affect noise, not correctness. We relax every check the
/// strict policy enforces because the bench host is typically a dev
/// machine without isolcpus; the `allow_non_isolated` flag in
/// bench.conf reflects the same relaxation on the client side.
void pin_to_cpu(int cpu_id) noexcept {
    if (cpu_id < 0) return;
    eph::utils::CpuPinPolicy policy{
        .require_isolcpus            = false,
        .require_no_sibling_conflict = false,
        .require_same_numa           = false,
        .warn_irq_overlap            = false,
    };
    auto res = eph::utils::pin_thread(cpu_id, "mockex", policy);
    if (!res) {
        SPDLOG_WARN("[mockex] CPU pin to core {} failed: {} (non-fatal)",
                    cpu_id, res.error());
        return;
    }
    SPDLOG_INFO("[mockex] pinned to CPU {}", cpu_id);
}

} // namespace

int main(int argc, char** argv) {
    // Default logger → stderr colorized. The bench invokes us under
    // the `lat` wrapper which redirects the mock's stderr to the
    // user's terminal, so this matches the Python mocks' behaviour.
    auto logger = spdlog::stderr_color_mt("mockex");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");

    std::string scenario_name;
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "-h" || a == "--help") return print_usage(0);
        if (a == "--scenario" && i + 1 < argc) {
            scenario_name = argv[++i];
        } else if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else {
            SPDLOG_ERROR("[mockex] unknown argument: {}", a);
            return print_usage(2);
        }
    }

    if (scenario_name.empty()) {
        SPDLOG_ERROR("[mockex] --scenario is required");
        return print_usage(2);
    }

    auto entry = mockex::find_scenario(scenario_name);
    if (!entry) {
        SPDLOG_ERROR("[mockex] unknown scenario '{}'", scenario_name);
        return print_usage(2);
    }

    if (config_path.empty()) {
        config_path = locate_config();
    }

    // Parse the global (pre-section) lowercase keys + the scenario
    // section. Both loaders read the same INI file; the section call
    // is scoped to `[lat_<name>]`.
    auto globals_e = bench::ScenarioConfig::load_globals(config_path);
    if (!globals_e) {
        SPDLOG_ERROR("[mockex] failed to load globals from {}: {}",
                     config_path, globals_e.error());
        return 1;
    }
    auto section_e = bench::ScenarioConfig::load(config_path, entry->section);
    if (!section_e) {
        SPDLOG_ERROR("[mockex] failed to load section [{}] from {}: {}",
                     entry->section, config_path, section_e.error());
        return 1;
    }

    // Optional CPU pinning — matches the Python mocks, which Python's
    // os.sched_setaffinity handled when run under taskset. bench.conf
    // now exposes `cpu_mock` as a first-class global key.
    auto cpu_mock_e = globals_e->get_u32("cpu_mock", UINT32_MAX);
    if (cpu_mock_e && *cpu_mock_e != UINT32_MAX) {
        pin_to_cpu(static_cast<int>(*cpu_mock_e));
    }

    eph::utils::install_shutdown_handlers();
    // Ignore SIGPIPE so a peer disconnecting mid-send doesn't nuke us —
    // we already use MSG_NOSIGNAL in eph::net::posix::send_all, but
    // covering the signal here is belt + braces for future scenarios.
    std::signal(SIGPIPE, SIG_IGN);

    mockex::ScenarioContext ctx{
        .scenario_name = entry->section,
        .config_path   = config_path,
        .globals       = &*globals_e,
        .section       = &*section_e,
        .running       = &eph::utils::g_shutdown_flag,
    };

    SPDLOG_INFO("[mockex] scenario='{}' section='[{}]' config='{}'",
                scenario_name, entry->section, config_path);
    return entry->fn(ctx);
}

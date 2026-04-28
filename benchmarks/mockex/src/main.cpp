/// @file benchmarks/mockex/src/main.cpp
/// Unified mock-server entry point.
///
/// Parses `--scenario <name> --config <path>`, loads config.toml, pins
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

#include <sys/prctl.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

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
        "  --config <path>     config.toml path (default: $BENCH_CONFIG, then\n"
        "                      benchmarks/latency/config.toml)\n"
        "  -h, --help          this help\n\n"
        "SCENARIOS:\n");
    for (const auto& e : mockex::kScenarioTable) {
        std::fprintf(stderr, "  %-12s → section [%s]\n",
                     std::string{e.name}.c_str(),
                     std::string{e.section}.c_str());
    }
    return rc;
}

/// Locate config.toml when `--config` was not explicitly passed.
[[nodiscard]] std::string locate_config() {
    if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) {
        return env;
    }
    return "benchmarks/latency/config.toml";
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

    // Parse config.toml into the structured BenchConfig.
    auto cfg_e = bench::load_bench_conf(config_path);
    if (!cfg_e) {
        SPDLOG_ERROR("[mockex] failed to load {}: {}",
                     config_path, bench::format_error(cfg_e.error()));
        return 1;
    }
    const bench::Scenario* scenario_ptr = cfg_e->scenario(entry->section);
    if (scenario_ptr == nullptr) {
        SPDLOG_ERROR("[mockex] scenario [{}] not found in {}",
                     entry->section, config_path);
        return 1;
    }

    // CPU pinning: cpu_mock from the structured BenchConfig.
    if (cfg_e->cpu.cpu_mock >= 0) {
        pin_to_cpu(cfg_e->cpu.cpu_mock);
    }

    eph::utils::install_shutdown_handlers();
    // Ignore SIGPIPE so a peer disconnecting mid-send doesn't nuke us —
    // we already use MSG_NOSIGNAL in eph::net::posix::send_all, but
    // covering the signal here is belt + braces for future scenarios.
    std::signal(SIGPIPE, SIG_IGN);

    // Parent-death watchdog: when mockex is fork()ed by a test runner
    // (mockex/tests/test_mockex_*) and then exec'd into here via the
    // sequence `fork -> execl(mockex)`, PR_SET_PDEATHSIG persists across
    // execve for non-setuid binaries. If the test runner crashes without
    // SIGTERM-ing us, the kernel delivers SIGTERM here, which the
    // shutdown handler installed above converts to a clean
    // `g_shutdown_flag = false` — every scenario handler polls that flag
    // and exits its loop. Without this, mockex can survive 4+ hours
    // holding bench ports (observed pattern with the DPDK rss_fanout
    // mock dispatcher; the same orphan class applies here). Errors are
    // swallowed: defensive watchdog, not a hard requirement; the test
    // runners' explicit SIGTERM in their teardown still covers the
    // happy path.
    (void)::prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

    mockex::ScenarioContext ctx{
        .scenario_name = entry->section,
        .config_path   = config_path,
        .cfg           = &*cfg_e,
        .scenario      = scenario_ptr,
        .running       = &eph::utils::g_shutdown_flag,
    };

    SPDLOG_INFO("[mockex] scenario='{}' section='[{}]' config='{}'",
                scenario_name, entry->section, config_path);
    return entry->fn(ctx);
}

/// @file tools/eph-nicd.cpp
/// `eph-nicd` — the DPDK NIC daemon.
///
/// Owns the DPDK primary process for one NIC (one BDF). Application
/// processes attach as DPDK secondaries via `Platform::create(...)` once this
/// daemon is running. See plan section "S4" and "目标形态" for the daemon-led
/// model rationale.
///
/// Two startup modes:
///
///   1. Production (systemd):
///        eph-nicd --config-file=/etc/eph/0000:01:00.1.toml
///      — full schema parsed from toml; this is what the
///      `eph-nicd@<bdf>.service` unit invokes.
///
///   2. Dev / ad-hoc (used by `eph::dpdk::dev::ensure_local_daemon`):
///        `eph-nicd --no-config-file --pci=0000:01:00.1`
///        with optional `--total-queues=N`, `--daemon-lcore=L`,
///        `--promiscuous`.
///      `NicServiceConfig` is built from the CLI flags; every other field
///      keeps its in-struct default.
///
/// Logging goes to stderr (systemd captures journal automatically). Exit
/// codes:
///   0 — clean shutdown after SIGTERM/SIGINT
///   1 — argv / toml / Platform bring-up failure
///   2 — usage error

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <unistd.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/dpdk/detail/nic_config_toml.hpp"
#include "eph/dpdk/platform.hpp"

namespace {

constexpr const char* kVersion = "eph-nicd 0.1.0 (daemon-reshape S4)";

void print_usage(FILE* out) {
    std::fprintf(out,
        "%s\n"
        "\n"
        "Usage:\n"
        "  eph-nicd --config-file=<path>\n"
        "  eph-nicd --no-config-file --pci=<bdf> [--total-queues=N]\n"
        "                                        [--daemon-lcore=L]\n"
        "                                        [--promiscuous]\n"
        "  eph-nicd --help | --version\n"
        "\n"
        "Production deployment uses --config-file (systemd unit\n"
        "eph-nicd@<bdf>.service points at /etc/eph/<bdf>.toml).\n"
        "Dev / ad-hoc invocation uses --no-config-file and supplies fields\n"
        "from the CLI; everything else defaults to NicServiceConfig defaults.\n",
        kVersion);
}

/// @brief Strip "--flag=" prefix and return the value, or nullptr if no match.
const char* match_long_eq(const char* arg, std::string_view flag) noexcept {
    const std::size_t flag_len = flag.size();
    if (std::strncmp(arg, flag.data(), flag_len) != 0) return nullptr;
    if (arg[flag_len] != '=') return nullptr;
    return arg + flag_len + 1;
}

struct CliArgs {
    bool                use_config_file = true;  // default = production path
    std::string         config_path;
    std::string         pci;
    int                 total_queues  = -1;  // -1 = unset, leave default
    int                 daemon_lcore  = -1;
    bool                promiscuous_set   = false;
    bool                promiscuous_value = false;
    bool                show_help    = false;
    bool                show_version = false;
};

std::expected<CliArgs, std::string> parse_argv(int argc, char** argv) {
    CliArgs out{};
    bool no_config_file_flag = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            out.show_help = true;
            return out;
        }
        if (std::strcmp(a, "--version") == 0) {
            out.show_version = true;
            return out;
        }
        if (std::strcmp(a, "--no-config-file") == 0) {
            no_config_file_flag = true;
            continue;
        }
        if (std::strcmp(a, "--promiscuous") == 0) {
            out.promiscuous_set   = true;
            out.promiscuous_value = true;
            continue;
        }
        if (auto* v = match_long_eq(a, "--config-file")) {
            out.config_path = v;
            continue;
        }
        if (auto* v = match_long_eq(a, "--pci")) {
            out.pci = v;
            continue;
        }
        if (auto* v = match_long_eq(a, "--total-queues")) {
            char* end = nullptr;
            errno = 0;
            const long n = std::strtol(v, &end, 10);
            if (errno != 0 || end == v || *end != '\0' || n < 1 || n > 65535) {
                return std::unexpected(
                    std::string{"--total-queues: expected int in [1,65535], "
                                "got '"} + v + "'");
            }
            out.total_queues = static_cast<int>(n);
            continue;
        }
        if (auto* v = match_long_eq(a, "--daemon-lcore")) {
            char* end = nullptr;
            errno = 0;
            const long n = std::strtol(v, &end, 10);
            if (errno != 0 || end == v || *end != '\0' || n < 0 || n > 65535) {
                return std::unexpected(
                    std::string{"--daemon-lcore: expected int in [0,65535], "
                                "got '"} + v + "'");
            }
            out.daemon_lcore = static_cast<int>(n);
            continue;
        }
        return std::unexpected(std::string{"unknown argument: "} + a);
    }

    // Reconcile mode.
    if (no_config_file_flag) {
        out.use_config_file = false;
        if (!out.config_path.empty()) {
            return std::unexpected(
                std::string{"--no-config-file and --config-file are mutually "
                            "exclusive"});
        }
        if (out.pci.empty()) {
            return std::unexpected(
                std::string{"--no-config-file requires --pci=<bdf>"});
        }
    } else {
        if (out.config_path.empty()) {
            return std::unexpected(
                std::string{"either --config-file=<path> or --no-config-file "
                            "+ --pci=<bdf> must be supplied"});
        }
        // Caveat: we accept --pci alongside --config-file but ignore it,
        // matching how systemd templates can pass extra fields. Fail loud
        // if mixing-mode flags appear with a config-file.
        if (!out.pci.empty()) {
            return std::unexpected(
                std::string{"--pci is only valid with --no-config-file "
                            "(it is taken from the toml otherwise)"});
        }
        if (out.total_queues != -1 || out.daemon_lcore != -1 ||
            out.promiscuous_set) {
            return std::unexpected(
                std::string{"--total-queues / --daemon-lcore / --promiscuous "
                            "are only valid with --no-config-file"});
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    // ── Logging: stderr-only color sink. systemd journals it as the unit's
    //    StandardError=journal.
    {
        auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("eph-nicd", sink);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [eph-nicd] %v");
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);
    }

    SPDLOG_INFO("{} starting (pid={})", kVersion, ::getpid());

    // ── 1. Parse argv.
    auto args_r = parse_argv(argc, argv);
    if (!args_r) {
        SPDLOG_ERROR("argv: {}", args_r.error());
        print_usage(stderr);
        return 2;
    }
    auto& args = *args_r;
    if (args.show_help) {
        print_usage(stdout);
        return 0;
    }
    if (args.show_version) {
        std::printf("%s\n", kVersion);
        return 0;
    }

    // ── 2. Build NicServiceConfig.
    eph::dpdk::detail::ParsedNicConfig parsed{};
    eph::dpdk::NicServiceConfig         cfg{};
    std::string                         pci_owned;  // when CLI mode
    if (args.use_config_file) {
        SPDLOG_INFO("loading toml config from '{}'", args.config_path);
        auto p_r = eph::dpdk::detail::parse_nic_toml(args.config_path);
        if (!p_r) {
            SPDLOG_ERROR("config-file: {}", p_r.error());
            return 1;
        }
        parsed = std::move(*p_r);
        parsed.rebind();
        cfg = parsed.cfg;
    } else {
        // Build from CLI args. Defaults from NicServiceConfig in-struct.
        pci_owned = args.pci;
        cfg.pci   = pci_owned;
        if (args.total_queues != -1) {
            cfg.total_queues = static_cast<std::uint16_t>(args.total_queues);
        }
        if (args.daemon_lcore != -1) {
            cfg.daemon_lcore = static_cast<std::uint16_t>(args.daemon_lcore);
        }
        if (args.promiscuous_set) {
            cfg.promiscuous = args.promiscuous_value;
        }
        // Validate the CLI-built config to give a clean error before EAL.
        if (auto err = eph::dpdk::validate(cfg); !err.empty()) {
            SPDLOG_ERROR("--no-config-file: invalid config: {}", err);
            return 1;
        }
        SPDLOG_INFO(
            "no-config-file mode: pci='{}' total_queues={} daemon_lcore={} "
            "promiscuous={}",
            cfg.pci, cfg.total_queues, cfg.daemon_lcore, cfg.promiscuous);
    }

    // ── 3. Bring up Platform as primary.
    SPDLOG_INFO(
        "calling Platform::serve_nic (pci='{}', total_queues={}, "
        "daemon_lcore={})",
        cfg.pci, cfg.total_queues, cfg.daemon_lcore);
    auto plat_r = eph::dpdk::Platform::serve_nic(cfg);
    if (!plat_r) {
        SPDLOG_ERROR("Platform::serve_nic failed: {}", plat_r.error());
        return 1;
    }
    auto plat = std::move(*plat_r);
    SPDLOG_INFO(
        "daemon started (pci='{}' total_queues={} pid={} port_id={})",
        cfg.pci, cfg.total_queues, ::getpid(), plat.port_id());

    // ── 4. Block on signal. ~Platform handles teardown on return / drop.
    plat.join();

    SPDLOG_INFO("daemon exited cleanly (pid={})", ::getpid());
    return 0;
}

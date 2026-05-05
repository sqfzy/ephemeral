/// @file tools/eph-nicctl.cpp
/// `eph-nicctl` — operator tool for inspecting `eph-nicd` daemon state.
///
/// Connects to a running `eph-nicd` daemon by attaching as a DPDK
/// secondary (same `--file-prefix` derivation as `Platform::create`),
/// sends a single `eph_nicctl_query` `rte_mp_request_sync`, prints the
/// reply, and exits.
///
/// Subcommands (S6 minimum surface):
///
///   eph-nicctl peers --pci=<bdf>
///       Lists daemon-visible peer state. Today this prints the
///       claimed-queue bitmap (low 64 bits) plus the daemon's PoolState
///       diagnostic counters; per-peer slot detail is a follow-up
///       (queue_allocator's claim_gen[] does not currently track
///       per-claim PID/slot — adding that is a small extension to the
///       Header).
///
///   eph-nicctl stats --pci=<bdf>
///       Pretty-prints the QueueAllocator PoolState dump:
///       total / free / largest_free_run / generation / stale_releases.
///
/// Both subcommands take `--pci=<bdf>` to identify the target daemon.
/// The file_prefix is derived deterministically from pci, matching
/// what `Platform::serve_nic` / `Platform::create` use.
///
/// Implementation note (per S6 spec): nicctl is itself a DPDK secondary.
/// That's heavier than a unix-domain socket would be, but it (a) reuses
/// the `eph_nicctl_query` IPC machinery the daemon already speaks,
/// (b) requires zero additional state in the daemon, and (c) gracefully
/// degrades — if the daemon isn't running, the EAL secondary attach
/// itself fails fast with a clear error.
///
/// Exit codes:
///   0 — query succeeded, output printed
///   1 — daemon not running, EAL attach failed, IPC timeout, or
///       daemon-replied error
///   2 — usage error

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_eal.h>
#include <rte_errno.h>

#include "eph/dpdk/detail/bdf_sanitize.hpp"
#include "eph/dpdk/detail/mp_ipc.hpp"
#include "eph/dpdk/detail/queue_allocator.hpp"
#include "eph/dpdk/eal.hpp"

namespace {

constexpr const char* kVersion = "eph-nicctl 0.1.0 (daemon-reshape S6)";

void print_usage(FILE* out) {
    std::fprintf(out,
        "%s\n"
        "\n"
        "Usage:\n"
        "  eph-nicctl peers  --pci=<bdf>\n"
        "  eph-nicctl stats  --pci=<bdf>\n"
        "  eph-nicctl audit  --pci=<bdf>\n"
        "  eph-nicctl --help | --version\n"
        "\n"
        "Connects to the eph-nicd daemon for the given pci as a DPDK\n"
        "secondary, queries its state, prints a snapshot and exits.\n"
        "\n"
        "  peers / stats — QueueAllocator pool state snapshot\n"
        "  audit         — T2.3 HMAC tamper-detection scan across\n"
        "                  MpRegistry + QueueAllocator + IcmpDirectory.\n"
        "                  Returns 0 mismatches in healthy / unkeyed mode;\n"
        "                  non-zero on detected tampering.\n",
        kVersion);
}

const char* match_long_eq(const char* arg, std::string_view flag) noexcept {
    const std::size_t flag_len = flag.size();
    if (std::strncmp(arg, flag.data(), flag_len) != 0) return nullptr;
    if (arg[flag_len] != '=') return nullptr;
    return arg + flag_len + 1;
}

enum class Subcommand {
    None,
    Peers,
    Stats,
    Audit,
};

struct CliArgs {
    Subcommand   sub          = Subcommand::None;
    std::string  pci;
    bool         show_help    = false;
    bool         show_version = false;
};

std::expected<CliArgs, std::string> parse_argv(int argc, char** argv) {
    CliArgs out{};
    if (argc < 2) {
        return std::unexpected(
            std::string{"missing subcommand (try --help)"});
    }
    int i = 1;
    // First positional: subcommand or top-level flag
    {
        const char* a = argv[i];
        if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            out.show_help = true; return out;
        }
        if (std::strcmp(a, "--version") == 0) {
            out.show_version = true; return out;
        }
        if (std::strcmp(a, "peers") == 0) {
            out.sub = Subcommand::Peers;
        } else if (std::strcmp(a, "stats") == 0) {
            out.sub = Subcommand::Stats;
        } else if (std::strcmp(a, "audit") == 0) {
            out.sub = Subcommand::Audit;
        } else {
            return std::unexpected(
                std::string{"unknown subcommand: "} + a);
        }
        ++i;
    }
    for (; i < argc; ++i) {
        const char* a = argv[i];
        if (auto* v = match_long_eq(a, "--pci")) {
            out.pci = v;
            continue;
        }
        if (std::strcmp(a, "--help") == 0) {
            out.show_help = true; return out;
        }
        return std::unexpected(std::string{"unknown argument: "} + a);
    }
    if (out.pci.empty()) {
        return std::unexpected(
            std::string{"--pci=<bdf> is required for this subcommand"});
    }
    return out;
}

/// @brief Attach as a DPDK secondary to the daemon's EAL session.
/// Returns the file_prefix used so the caller can log it.
std::expected<std::string, std::string>
attach_as_secondary(const std::string& pci) {
    auto san = ::eph::dpdk::detail::sanitize_bdf_for_file_prefix(pci);
    if (!san) {
        return std::unexpected(
            std::string{"sanitize_bdf failed: "} + san.error().detail);
    }
    const std::string file_prefix = std::string{"eph_"} + *san;

    // Hand-assemble argv: --proc-type=secondary, --file-prefix, --no-shconf?
    // No: we DO need shconf because rte_mp_request_sync uses it. Just the
    // minimal set that gets us into a viable secondary.
    const std::string fp_arg = std::string{"--file-prefix="} + file_prefix;
    std::vector<std::string> argv_owned = {
        std::string{"eph-nicctl"},
        std::string{"--proc-type=secondary"},
        fp_arg,
        // Reduce log noise; we want clean stdout for our own output.
        std::string{"--log-level=lib.eal:warning"},
    };
    std::vector<char*> argv;
    argv.reserve(argv_owned.size());
    for (auto& s : argv_owned) argv.push_back(s.data());

    auto eal_r = ::eph::dpdk::eal_init(
        static_cast<int>(argv.size()), argv.data());
    if (!eal_r) {
        return std::unexpected(std::format(
            "EAL secondary attach failed (file_prefix='{}'): {}\n"
            "Hint: is `eph-nicd --no-config-file --pci={}` running? "
            "(`pgrep -af eph-nicd` to check)",
            file_prefix, eal_r.error(), pci));
    }
    return file_prefix;
}

int cmd_query(const CliArgs& args) {
    auto attach_r = attach_as_secondary(args.pci);
    if (!attach_r) {
        std::fprintf(stderr, "%s\n", attach_r.error().c_str());
        return 1;
    }
    const std::string& file_prefix = *attach_r;

    ::eph::dpdk::detail::NicctlQueryRequest req{};
    req.version       = 1;
    req.kind          = static_cast<uint8_t>(
        ::eph::dpdk::detail::NicctlQueryKind::Stats);
    req.requester_pid = static_cast<int32_t>(::getpid());

    auto reply_r = ::eph::dpdk::detail::mp_ipc_request_sync<
        ::eph::dpdk::detail::NicctlQueryRequest,
        ::eph::dpdk::detail::NicctlQueryReply>(
        ::eph::dpdk::detail::kNicctlQueryActionName,
        req,
        std::chrono::milliseconds{2000});
    if (!reply_r) {
        std::fprintf(stderr,
            "eph-nicctl: query IPC failed: %s\n"
            "(file_prefix='%s'; daemon may be unresponsive)\n",
            reply_r.error().detail, file_prefix.c_str());
        (void)::eph::dpdk::eal_cleanup();
        return 1;
    }
    const auto& reply = *reply_r;
    if (!reply.ok) {
        std::fprintf(stderr, "eph-nicctl: daemon error: %s\n", reply.error);
        (void)::eph::dpdk::eal_cleanup();
        return 1;
    }

    // Pretty-print, matching the subcommand variation.
    std::printf("eph-nicd daemon state (pci='%s', file_prefix='%s'):\n",
                args.pci.c_str(), file_prefix.c_str());
    std::printf("  total_queues:       %u\n",
                static_cast<unsigned>(reply.total_queues));
    std::printf("  free_queues:        %u\n",
                static_cast<unsigned>(reply.free_queues));
    std::printf("  claimed_queues:     %u\n",
                static_cast<unsigned>(reply.total_queues - reply.free_queues));
    std::printf("  largest_free_run:   %u\n",
                static_cast<unsigned>(reply.largest_free_run));
    std::printf("  generation:         %llu\n",
                static_cast<unsigned long long>(reply.generation));
    std::printf("  stale_releases:     %llu\n",
                static_cast<unsigned long long>(reply.stale_releases));
    std::printf("  daemon_port_id:     %u%s\n",
                static_cast<unsigned>(reply.daemon_port_id),
                reply.daemon_port_id == 0xFFFF ? " (test/sentinel)" : "");

    if (args.sub == Subcommand::Peers) {
        // Render the bitmap of claimed queues. The wire only carries
        // the low 64 bits today — bumping this to a full 256-bit
        // wire field is a future extension when a deployment actually
        // grants beyond 64 queues.
        std::printf("  claimed_bitmap_lo64 (queues 0..63):\n    ");
        const uint64_t bm = reply.owned_bitmap_lo64;
        bool any = false;
        for (int q = 0; q < 64; ++q) {
            if ((bm >> q) & 1ULL) {
                if (any) std::printf(" ");
                std::printf("%d", q);
                any = true;
            }
        }
        if (!any) std::printf("(none)");
        std::printf("\n");
        std::printf("  NOTE: per-peer pid/uid/attach-time tracking is a "
                    "follow-up — \n        S6 surfaces only the aggregated "
                    "allocator state today.\n");
    }

    (void)::eph::dpdk::eal_cleanup();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────
// T2.3 N: audit subcommand — query daemon for per-registry mismatches.
// Exit code 0 = healthy / unkeyed; 1 = IPC error; 2 = mismatches found.
// ─────────────────────────────────────────────────────────────────────
int cmd_audit(const CliArgs& args) {
    auto attach_r = attach_as_secondary(args.pci);
    if (!attach_r) {
        std::fprintf(stderr, "%s\n", attach_r.error().c_str());
        return 1;
    }
    const std::string& file_prefix = *attach_r;

    ::eph::dpdk::detail::NicctlAuditRequest req{};
    req.version       = 1;
    req.requester_pid = static_cast<int32_t>(::getpid());

    auto reply_r = ::eph::dpdk::detail::mp_ipc_request_sync<
        ::eph::dpdk::detail::NicctlAuditRequest,
        ::eph::dpdk::detail::NicctlAuditReply>(
        ::eph::dpdk::detail::kNicctlAuditActionName,
        req,
        std::chrono::milliseconds{2000});
    if (!reply_r) {
        std::fprintf(stderr,
            "eph-nicctl: audit IPC failed: %s\n"
            "(file_prefix='%s'; daemon may be unresponsive or this "
            "daemon predates T2.3 — confirm `eph-nicd` was rebuilt)\n",
            reply_r.error().detail, file_prefix.c_str());
        (void)::eph::dpdk::eal_cleanup();
        return 1;
    }
    const auto& reply = *reply_r;
    if (!reply.ok) {
        std::fprintf(stderr, "eph-nicctl: daemon error: %s\n", reply.error);
        (void)::eph::dpdk::eal_cleanup();
        return 1;
    }

    std::printf("eph-nicd registry HMAC audit (pci='%s', "
                "file_prefix='%s'):\n",
                args.pci.c_str(), file_prefix.c_str());
    if (!reply.hmac_enabled) {
        std::printf("\n  hmac_enabled: 0  (daemon running in unkeyed "
                    "mode — single-tenant deployment;\n"
                    "                    no tamper protection active. "
                    "Set\n"
                    "                    `enable_registry_hmac = true` "
                    "in /etc/eph/<bdf>.toml\n"
                    "                    and restart `eph-nicd@<bdf>` "
                    "to enable.)\n");
        (void)::eph::dpdk::eal_cleanup();
        return 0;
    }

    const unsigned mp_mm  = reply.mp_registry_mismatches;
    const unsigned qa_mm  = reply.queue_allocator_mismatches;
    const unsigned icmp_mm = reply.icmp_directory_mismatches;
    const unsigned total_mm = mp_mm + qa_mm + icmp_mm;

    std::printf("\n");
    std::printf("  Registry         Checked   Mismatches  Status\n");
    std::printf("  ──────────────  ────────  ──────────  ─────────────\n");
    std::printf("  MpRegistry      %8u  %10u  %s\n",
                static_cast<unsigned>(reply.mp_registry_total_slots_checked),
                mp_mm,
                mp_mm == 0 ? "✓ healthy" : "✗ TAMPER DETECTED");
    std::printf("  QueueAllocator  %8u  %10u  %s\n",
                static_cast<unsigned>(reply.queue_allocator_total_checked),
                qa_mm,
                qa_mm == 0 ? "✓ healthy" : "✗ TAMPER DETECTED");
    std::printf("  IcmpDirectory   %8u  %10u  %s\n",
                static_cast<unsigned>(reply.icmp_directory_total_checked),
                icmp_mm,
                icmp_mm == 0 ? "✓ healthy" : "✗ TAMPER DETECTED");
    std::printf("  ──────────────  ────────  ──────────  ─────────────\n");
    std::printf("  Total                     %10u  %s\n",
                total_mm,
                total_mm == 0 ? "All clean" : "TAMPER — investigate");

    (void)::eph::dpdk::eal_cleanup();
    if (total_mm > 0) {
        std::fprintf(stderr,
            "\n[WARN] %u mismatches detected. Check daemon journalctl "
            "for per-slot tamper details.\n", total_mm);
        return 2;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    {
        auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>("eph-nicctl", sink);
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [eph-nicctl] %v");
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::warn);
    }

    auto args_r = parse_argv(argc, argv);
    if (!args_r) {
        std::fprintf(stderr, "eph-nicctl: %s\n", args_r.error().c_str());
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

    switch (args.sub) {
        case Subcommand::Peers:
        case Subcommand::Stats:
            return cmd_query(args);
        case Subcommand::Audit:
            return cmd_audit(args);
        case Subcommand::None:
            print_usage(stderr);
            return 2;
    }
    return 2;
}

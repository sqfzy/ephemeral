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
///   eph-nicctl audit --pci=<bdf> [--watch] [--interval=<sec>]
///       T2.3 HMAC tamper-detection scan. With `--watch`, redraws on
///       every cycle (default 5 s); exits 0 on signal, 2 on first
///       detected tamper, 1 on persistent IPC failure (≥3 in a row).
///
/// All subcommands take `--pci=<bdf>` to identify the target daemon.
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

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <string>
#include <string_view>
#include <thread>
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
        "  eph-nicctl audit  --pci=<bdf> [--watch] [--interval=<sec>]\n"
        "  eph-nicctl --help | --version\n"
        "\n"
        "Connects to the eph-nicd daemon for the given pci as a DPDK\n"
        "secondary, queries its state, prints a snapshot and exits.\n"
        "\n"
        "  peers / stats — QueueAllocator pool state snapshot\n"
        "  audit         — T2.3 HMAC tamper-detection scan across\n"
        "                  MpRegistry + QueueAllocator + IcmpDirectory.\n"
        "                  Returns 0 mismatches in healthy / unkeyed mode;\n"
        "                  non-zero on detected tampering.\n"
        "\n"
        "  --watch       — keep auditing in a loop, redrawing each cycle.\n"
        "                  Exits on first non-zero mismatch (exit 2) or\n"
        "                  on SIGINT/SIGTERM (exit 0). Use with `audit`.\n"
        "  --interval=N  — seconds between watch cycles (default: 5).\n",
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
    bool         watch        = false;
    unsigned     interval_s   = 5;
};

std::expected<CliArgs, std::string> parse_argv(int argc, char** argv) {
    CliArgs out{};
    bool interval_seen = false;
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
        if (std::strcmp(a, "--watch") == 0) {
            out.watch = true;
            continue;
        }
        if (auto* v = match_long_eq(a, "--interval")) {
            char* end = nullptr;
            unsigned long n = std::strtoul(v, &end, 10);
            if (*v == '\0' || (end && *end != '\0') || n == 0 || n > 86400) {
                return std::unexpected(
                    std::string{"--interval=<sec> must be 1..86400, got: "} + v);
            }
            out.interval_s = static_cast<unsigned>(n);
            interval_seen = true;
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
    if (out.watch && out.sub != Subcommand::Audit) {
        return std::unexpected(
            std::string{"--watch is only valid with the `audit` subcommand"});
    }
    // `--interval` is only meaningful with `--watch` (which is audit-
    // only), so reject it on every other subcommand path. Catches the
    // silent-ignore case where an operator types
    // `eph-nicctl stats --pci=X --interval=10` and assumes it polls.
    if (interval_seen && (!out.watch || out.sub != Subcommand::Audit)) {
        return std::unexpected(
            std::string{"--interval=<sec> requires --watch on the "
                        "`audit` subcommand"});
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
// Single-shot: exit 0 healthy/unkeyed, 1 IPC error, 2 tamper detected.
// --watch    : redraw on every cycle; exit 0 on signal, 2 on first
//              non-zero mismatch, 1 on persistent IPC failure.
// ─────────────────────────────────────────────────────────────────────

// SIGINT/SIGTERM flag used by the watch loop. async-signal-safe writes only.
//
// Lock-free is required for the signal-handler store to be async-signal-
// safe (POSIX permits std::atomic<T>::store from a handler ONLY when
// is_always_lock_free; otherwise the store may take a hidden lock that
// races with whatever the interrupted thread held).
static_assert(std::atomic<int>::is_always_lock_free,
              "watch_signal_handler relies on lock-free atomic<int> store "
              "to be async-signal-safe");
std::atomic<int> g_stop_signal{0};

extern "C" void watch_signal_handler(int signo) noexcept {
    g_stop_signal.store(signo, std::memory_order_relaxed);
}

enum class AuditStatus {
    Ok,            // hmac_enabled and total_mm == 0
    Unkeyed,       // hmac_enabled == false
    Tamper,        // total_mm > 0
    IpcError,      // mp_ipc_request_sync failed
    DaemonError,   // reply.ok == false
};

struct AuditOutcome {
    AuditStatus status = AuditStatus::Ok;
    ::eph::dpdk::detail::NicctlAuditReply reply{};
    std::string err_detail;  // populated for IpcError / DaemonError
};

AuditOutcome run_audit_once() {
    AuditOutcome out{};
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
        out.status     = AuditStatus::IpcError;
        out.err_detail = reply_r.error().detail
            ? reply_r.error().detail : "(no detail)";
        return out;
    }
    out.reply = *reply_r;
    if (!out.reply.ok) {
        out.status     = AuditStatus::DaemonError;
        out.err_detail = out.reply.error;
        return out;
    }
    if (!out.reply.hmac_enabled) {
        out.status = AuditStatus::Unkeyed;
        return out;
    }
    const unsigned total_mm = out.reply.mp_registry_mismatches
                            + out.reply.queue_allocator_mismatches
                            + out.reply.icmp_directory_mismatches;
    out.status = (total_mm == 0) ? AuditStatus::Ok : AuditStatus::Tamper;
    return out;
}

// Render the audit reply table to stdout. Header line is caller's job —
// in watch mode we want a timestamp + cycle counter above the table.
void print_audit_body(const AuditOutcome& o) {
    if (o.status == AuditStatus::Unkeyed) {
        std::printf("\n  hmac_enabled: 0  (daemon running in unkeyed "
                    "mode — single-tenant deployment;\n"
                    "                    no tamper protection active. "
                    "Set\n"
                    "                    `enable_registry_hmac = true` "
                    "in /etc/eph/<bdf>.toml\n"
                    "                    and restart `eph-nicd@<bdf>` "
                    "to enable.)\n");
        return;
    }
    const auto& reply = o.reply;
    const unsigned mp_mm   = reply.mp_registry_mismatches;
    const unsigned qa_mm   = reply.queue_allocator_mismatches;
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
}

// Format current wall-clock as "YYYY-MM-DD HH:MM:SS" into a fixed buffer.
void format_now(char (&buf)[32]) noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    ::localtime_r(&now, &tm_buf);
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) {
        buf[0] = '?';
        buf[1] = '\0';
    }
}

// Sleep up to `total` milliseconds in 200 ms chunks so SIGINT/SIGTERM
// observed via g_stop_signal can interrupt within ~200 ms. Returns true
// if a stop signal was observed during the wait.
bool sleep_interruptible(std::chrono::milliseconds total) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + total;
    while (steady_clock::now() < deadline) {
        if (g_stop_signal.load(std::memory_order_relaxed) != 0) return true;
        const auto remaining = duration_cast<milliseconds>(
            deadline - steady_clock::now());
        const auto chunk = std::min(remaining, milliseconds{200});
        if (chunk.count() <= 0) break;
        std::this_thread::sleep_for(chunk);
    }
    return g_stop_signal.load(std::memory_order_relaxed) != 0;
}

int cmd_audit(const CliArgs& args) {
    auto attach_r = attach_as_secondary(args.pci);
    if (!attach_r) {
        std::fprintf(stderr, "%s\n", attach_r.error().c_str());
        return 1;
    }
    const std::string& file_prefix = *attach_r;

    if (!args.watch) {
        // Single-shot path: one request, print, cleanup, exit.
        const auto out = run_audit_once();
        if (out.status == AuditStatus::IpcError) {
            std::fprintf(stderr,
                "eph-nicctl: audit IPC failed: %s\n"
                "(file_prefix='%s'; daemon may be unresponsive or this "
                "daemon predates T2.3 — confirm `eph-nicd` was rebuilt)\n",
                out.err_detail.c_str(), file_prefix.c_str());
            (void)::eph::dpdk::eal_cleanup();
            return 1;
        }
        if (out.status == AuditStatus::DaemonError) {
            std::fprintf(stderr, "eph-nicctl: daemon error: %s\n",
                         out.err_detail.c_str());
            (void)::eph::dpdk::eal_cleanup();
            return 1;
        }
        std::printf("eph-nicd registry HMAC audit (pci='%s', "
                    "file_prefix='%s'):\n",
                    args.pci.c_str(), file_prefix.c_str());
        print_audit_body(out);
        (void)::eph::dpdk::eal_cleanup();
        if (out.status == AuditStatus::Tamper) {
            const unsigned total_mm = out.reply.mp_registry_mismatches
                                    + out.reply.queue_allocator_mismatches
                                    + out.reply.icmp_directory_mismatches;
            std::fprintf(stderr,
                "\n[WARN] %u mismatches detected. Check daemon journalctl "
                "for per-slot tamper details.\n", total_mm);
            return 2;
        }
        return 0;
    }

    // Watch mode: redraw every interval until SIGINT/SIGTERM or first
    // detected tamper. Install handlers; preserve the prior disposition so
    // we don't trample tests/process owners that have their own handlers.
    struct sigaction sa_new{};
    sa_new.sa_handler = &watch_signal_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;  // intentionally NOT SA_RESTART — we want the
                           // sleep loop to observe the flag promptly.
    struct sigaction sa_old_int{};
    struct sigaction sa_old_term{};
    ::sigaction(SIGINT,  &sa_new, &sa_old_int);
    ::sigaction(SIGTERM, &sa_new, &sa_old_term);

    std::printf("eph-nicd registry HMAC audit (pci='%s', "
                "file_prefix='%s')\n"
                "watch mode: interval=%us  (Ctrl+C to stop)\n",
                args.pci.c_str(), file_prefix.c_str(), args.interval_s);

    int exit_code = 0;
    unsigned cycle = 0;
    unsigned consecutive_ipc_errors = 0;
    constexpr unsigned kMaxConsecutiveIpcErrors = 3;

    while (g_stop_signal.load(std::memory_order_relaxed) == 0) {
        ++cycle;
        const auto out = run_audit_once();
        char ts[32];
        format_now(ts);
        // ANSI clear-screen + home; harmless on cooked terminals, but we
        // gate it behind isatty() to keep piped output clean.
        if (::isatty(::fileno(stdout))) {
            std::fputs("\x1b[2J\x1b[H", stdout);
        }
        std::printf("eph-nicd registry HMAC audit (pci='%s')\n"
                    "watch cycle #%u  at %s  (interval=%us, Ctrl+C to stop)\n",
                    args.pci.c_str(), cycle, ts, args.interval_s);

        if (out.status == AuditStatus::IpcError) {
            ++consecutive_ipc_errors;
            std::fprintf(stderr,
                "\n[WARN] IPC failure (%u/%u): %s\n",
                consecutive_ipc_errors, kMaxConsecutiveIpcErrors,
                out.err_detail.c_str());
            if (consecutive_ipc_errors >= kMaxConsecutiveIpcErrors) {
                std::fprintf(stderr,
                    "[ERROR] %u consecutive IPC failures; daemon "
                    "appears down. Exiting.\n", consecutive_ipc_errors);
                exit_code = 1;
                break;
            }
        } else if (out.status == AuditStatus::DaemonError) {
            std::fprintf(stderr, "\n[ERROR] daemon error: %s\n",
                         out.err_detail.c_str());
            exit_code = 1;
            break;
        } else {
            consecutive_ipc_errors = 0;
            print_audit_body(out);
            std::fflush(stdout);
            if (out.status == AuditStatus::Tamper) {
                const unsigned total_mm =
                      out.reply.mp_registry_mismatches
                    + out.reply.queue_allocator_mismatches
                    + out.reply.icmp_directory_mismatches;
                std::fprintf(stderr,
                    "\n[WARN] %u mismatches detected — exiting watch.\n",
                    total_mm);
                exit_code = 2;
                break;
            }
        }

        if (sleep_interruptible(
                std::chrono::seconds{args.interval_s})) {
            break;  // signal arrived during sleep
        }
    }

    // Restore prior signal dispositions before tearing down EAL.
    ::sigaction(SIGINT,  &sa_old_int,  nullptr);
    ::sigaction(SIGTERM, &sa_old_term, nullptr);

    if (exit_code == 0
        && g_stop_signal.load(std::memory_order_relaxed) != 0) {
        std::printf("\n[INFO] stopped on signal %d after %u cycle(s).\n",
                    g_stop_signal.load(std::memory_order_relaxed), cycle);
    }
    (void)::eph::dpdk::eal_cleanup();
    return exit_code;
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

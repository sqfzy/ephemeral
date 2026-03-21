/// @file ws_echo_client.cpp
/// DPDK WebSocket (WSS) echo client example.
///
/// Demonstrates the full ephemeral stack:
///   EAL init → Platform create → ARP resolve → DpdkTransport create
///   (TCP handshake → TLS 1.3 → WS Upgrade) → send/recv echo loop.
///
/// Usage:
///   # Minimal (af_packet on WSL2, no hugepages):
///   sudo ./ws_echo_client --vdev=net_af_packet0,iface=eth0 --no-pci \
///       -- --host echo.example.com --local-ip 172.20.0.2 \
///          --gateway-ip 172.20.0.1 --msg "hello dpdk"
///
///   # With hugepages and specific port:
///   sudo ./ws_echo_client --vdev=net_af_packet0,iface=eth0 --no-pci \
///       -- --host echo.example.com --port 443 --ws-path / \
///          --local-ip 172.20.0.2 --gateway-ip 172.20.0.1 \
///          --count 5 --interval 1000
///
/// EAL args go BEFORE the '--' separator; application args go AFTER.

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include <rte_ethdev.h>

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Application configuration
// ─────────────────────────────────────────────────────────────────────────────

struct AppConfig {
    // Server target
    std::string host{};
    uint16_t    port        = 443;
    std::string ws_path     = "/";

    // Network (must match the DPDK port's network)
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    local_port  = 0;  // 0 = random ephemeral

    // Echo parameters
    std::string message     = "hello from ephemeral dpdk";
    int         count       = 3;       // 0 = infinite
    int         interval_ms = 1000;

    // TLS
    std::string ca_cert{};
    bool        no_verify   = false;

    // DPDK platform
    uint16_t    dpdk_port   = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Global stop flag for signal handling
// ─────────────────────────────────────────────────────────────────────────────

static volatile sig_atomic_t g_stop = 0;

static void signal_handler(int /*sig*/) {
    g_stop = 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Kernel RST suppression (af_packet mode)
// ─────────────────────────────────────────────────────────────────────────────

/// When using af_packet PMD, the kernel's TCP stack also sees incoming SYN-ACKs
/// and sends RST (no matching socket). This kills the DPDK TCP handshake.
///
/// This guard installs a firewall rule (nftables preferred, iptables fallback)
/// to DROP outgoing RST packets matching our specific connection tuple.
/// Rules are tagged with a comment for crash recovery — stale rules from
/// previous crashed processes are cleaned up on construction.
class KernelRstGuard {
public:
    /// Firewall backend detected at runtime.
    enum class Backend { kNone, kNftables, kIptables };

    /// Install a precise RST DROP rule for the given connection tuple.
    /// Returns an error string if not root or firewall commands fail.
    static auto install(const std::string& src_ip,
                        const std::string& dst_ip,
                        uint16_t dst_port)
        -> std::expected<KernelRstGuard, std::string>
    {
        if (getuid() != 0) {
            return std::unexpected(
                "Not root — cannot install RST suppression; "
                "TCP handshake may fail with af_packet PMD");
        }

        auto backend = detect_backend();
        if (backend == Backend::kNone) {
            return std::unexpected(
                "Neither nft nor iptables found — cannot suppress kernel RST");
        }

        bool is_v6 = src_ip.find(':') != std::string::npos;
        auto comment = std::format("eph-dpdk-rst-{}", getpid());

        // Clean up stale rules from previous crashed processes
        cleanup_stale_rules(backend, is_v6);

        // Build and execute the install command
        std::string cmd = build_install_cmd(backend, is_v6, src_ip, dst_ip,
                                            dst_port, comment);

        spdlog::debug("Installing RST suppression: {}", cmd);
        if (std::system(cmd.c_str()) != 0) {
            return std::unexpected(std::format(
                "Failed to install RST suppression rule via {} for "
                "{} -> {}:{} (command: {})",
                backend_name(backend), src_ip, dst_ip, dst_port, cmd));
        }

        KernelRstGuard guard;
        guard.backend_  = backend;
        guard.src_ip_   = src_ip;
        guard.dst_ip_   = dst_ip;
        guard.dst_port_ = dst_port;
        guard.is_v6_    = is_v6;
        guard.comment_  = comment;
        guard.installed_ = true;

        spdlog::info("Kernel RST suppression installed via {} for {} -> {}:{} "
                     "[comment={}]",
                     backend_name(backend), src_ip, dst_ip, dst_port, comment);
        return guard;
    }

    ~KernelRstGuard() { remove(); }

    KernelRstGuard(const KernelRstGuard&) = delete;
    KernelRstGuard& operator=(const KernelRstGuard&) = delete;
    KernelRstGuard(KernelRstGuard&& o) noexcept
        : backend_(o.backend_),
          src_ip_(std::move(o.src_ip_)),
          dst_ip_(std::move(o.dst_ip_)),
          dst_port_(o.dst_port_),
          is_v6_(o.is_v6_),
          comment_(std::move(o.comment_)),
          installed_(o.installed_) {
        o.installed_ = false;
    }
    KernelRstGuard& operator=(KernelRstGuard&& o) noexcept {
        remove();
        backend_   = o.backend_;
        src_ip_    = std::move(o.src_ip_);
        dst_ip_    = std::move(o.dst_ip_);
        dst_port_  = o.dst_port_;
        is_v6_     = o.is_v6_;
        comment_   = std::move(o.comment_);
        installed_ = o.installed_;
        o.installed_ = false;
        return *this;
    }

private:
    KernelRstGuard() = default;

    static auto backend_name(Backend b) -> const char* {
        switch (b) {
        case Backend::kNftables: return "nftables";
        case Backend::kIptables: return "iptables";
        default: return "none";
        }
    }

    /// Detect the best available firewall backend.
    static auto detect_backend() -> Backend {
        if (std::system("nft --version >/dev/null 2>&1") == 0)
            return Backend::kNftables;
        if (std::system("iptables --version >/dev/null 2>&1") == 0)
            return Backend::kIptables;
        return Backend::kNone;
    }

    /// Build the command to install a precise RST DROP rule.
    static auto build_install_cmd(Backend backend, bool is_v6,
                                  const std::string& src_ip,
                                  const std::string& dst_ip,
                                  uint16_t dst_port,
                                  const std::string& comment) -> std::string {
        if (backend == Backend::kNftables) {
            // nftables: unified IPv4/IPv6 syntax via inet family
            auto family = is_v6 ? "ip6" : "ip";
            return std::format(
                // Ensure table and chain exist, then add rule
                "nft add table inet eph_dpdk 2>/dev/null; "
                "nft add chain inet eph_dpdk output "
                "  '{{ type filter hook output priority 0; policy accept; }}' 2>/dev/null; "
                "nft add rule inet eph_dpdk output "
                "  {} saddr {} {} daddr {} "
                "  tcp dport {} tcp flags rst "
                "  comment \"{}\" drop",
                family, src_ip, family, dst_ip, dst_port, comment);
        }
        // iptables/ip6tables
        auto ipt = is_v6 ? "ip6tables" : "iptables";
        return std::format(
            "{} -C OUTPUT -p tcp --tcp-flags RST RST "
            "  -s {} -d {} --dport {} "
            "  -m comment --comment \"{}\" "
            "  -j DROP 2>/dev/null || "
            "{} -A OUTPUT -p tcp --tcp-flags RST RST "
            "  -s {} -d {} --dport {} "
            "  -m comment --comment \"{}\" "
            "  -j DROP",
            ipt, src_ip, dst_ip, dst_port, comment,
            ipt, src_ip, dst_ip, dst_port, comment);
    }

    /// Clean up stale rules from previous crashed processes.
    /// Scans for rules with the "eph-dpdk-rst-" comment prefix.
    static void cleanup_stale_rules(Backend backend, bool is_v6) {
        if (backend == Backend::kNftables) {
            // List handles of rules with eph-dpdk-rst- comment, then delete
            std::string cmd =
                "nft -a list chain inet eph_dpdk output 2>/dev/null | "
                "grep 'comment.*eph-dpdk-rst-' | "
                "awk '{print $NF}' | "
                "while read handle; do "
                "  nft delete rule inet eph_dpdk output handle \"$handle\" 2>/dev/null; "
                "done";
            if (std::system(cmd.c_str()) == 0) {
                spdlog::debug("Cleaned up stale nftables RST suppression rules");
            }
        } else {
            // iptables: list rules with line numbers, find stale ones, delete
            auto ipt = is_v6 ? "ip6tables" : "iptables";
            // Delete in reverse order to preserve line numbers
            std::string cmd = std::format(
                "{} -L OUTPUT --line-numbers -n 2>/dev/null | "
                "grep 'eph-dpdk-rst-' | "
                "awk '{{print $1}}' | sort -rn | "
                "while read num; do "
                "  {} -D OUTPUT \"$num\" 2>/dev/null; "
                "done",
                ipt, ipt);
            if (std::system(cmd.c_str()) == 0) {
                spdlog::debug("Cleaned up stale {} RST suppression rules", ipt);
            }
        }
    }

    void remove() {
        if (!installed_) return;

        std::string cmd;
        if (backend_ == Backend::kNftables) {
            // Find and delete our specific rule by comment
            cmd = std::format(
                "nft -a list chain inet eph_dpdk output 2>/dev/null | "
                "grep '\"{}\"' | awk '{{print $NF}}' | "
                "while read handle; do "
                "  nft delete rule inet eph_dpdk output handle \"$handle\"; "
                "done",
                comment_);
        } else {
            auto ipt = is_v6_ ? "ip6tables" : "iptables";
            cmd = std::format(
                "{} -D OUTPUT -p tcp --tcp-flags RST RST "
                "  -s {} -d {} --dport {} "
                "  -m comment --comment \"{}\" "
                "  -j DROP 2>/dev/null",
                ipt, src_ip_, dst_ip_, dst_port_, comment_);
        }
        std::system(cmd.c_str());
        spdlog::info("Kernel RST suppression removed for {} -> {}:{} [{}]",
                     src_ip_, dst_ip_, dst_port_, backend_name(backend_));
        installed_ = false;
    }

    Backend     backend_   = Backend::kNone;
    std::string src_ip_;
    std::string dst_ip_;
    uint16_t    dst_port_  = 0;
    bool        is_v6_     = false;
    std::string comment_;
    bool        installed_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// CLI argument parsing (app args after '--')
// ─────────────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr << std::format(
        "Usage: {} [EAL args] -- [app args]\n"
        "\n"
        "App args:\n"
        "  --host HOST          WSS server hostname (required)\n"
        "  --port PORT          WSS server port (default: 443)\n"
        "  --ws-path PATH       WebSocket upgrade path (default: /)\n"
        "  --local-ip IP        Local IPv4 address on DPDK port (required)\n"
        "  --gateway-ip IP      Gateway IPv4 for ARP resolution (required)\n"
        "  --local-port PORT    Local TCP source port (default: random)\n"
        "  --msg TEXT           Message to send (default: \"hello from ephemeral dpdk\")\n"
        "  --count N            Number of messages, 0=infinite (default: 3)\n"
        "  --interval MS        Interval between sends in ms (default: 1000)\n"
        "  --ca-cert PATH       CA certificate file (default: system)\n"
        "  --no-verify          Disable TLS peer verification\n"
        "  --dpdk-port N        DPDK port ID (default: 0)\n"
        "  --help               Show this help\n",
        prog);
}

static AppConfig parse_app_args(int argc, char** argv) {
    AppConfig cfg{};

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];

        auto next_val = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << std::format("Error: {} requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--host")        { cfg.host       = std::string(next_val("--host")); }
        else if (arg == "--port")   { cfg.port       = static_cast<uint16_t>(std::stoi(std::string(next_val("--port")))); }
        else if (arg == "--ws-path"){ cfg.ws_path    = std::string(next_val("--ws-path")); }
        else if (arg == "--local-ip")   { cfg.local_ip   = std::string(next_val("--local-ip")); }
        else if (arg == "--gateway-ip") { cfg.gateway_ip = std::string(next_val("--gateway-ip")); }
        else if (arg == "--local-port") { cfg.local_port = static_cast<uint16_t>(std::stoi(std::string(next_val("--local-port")))); }
        else if (arg == "--msg")        { cfg.message    = std::string(next_val("--msg")); }
        else if (arg == "--count")      { cfg.count      = std::stoi(std::string(next_val("--count"))); }
        else if (arg == "--interval")   { cfg.interval_ms = std::stoi(std::string(next_val("--interval"))); }
        else if (arg == "--ca-cert")    { cfg.ca_cert    = std::string(next_val("--ca-cert")); }
        else if (arg == "--no-verify")  { cfg.no_verify  = true; }
        else if (arg == "--dpdk-port")  { cfg.dpdk_port  = static_cast<uint16_t>(std::stoi(std::string(next_val("--dpdk-port")))); }
        else if (arg == "--help")       { print_usage("ws_echo_client"); std::exit(0); }
        else {
            std::cerr << std::format("Unknown app arg: {}\n", arg);
            print_usage("ws_echo_client");
            std::exit(1);
        }
    }

    // Validate required args
    if (cfg.host.empty()) {
        std::cerr << "Error: --host is required\n";
        print_usage("ws_echo_client");
        std::exit(1);
    }
    if (cfg.local_ip.empty()) {
        std::cerr << "Error: --local-ip is required\n";
        print_usage("ws_echo_client");
        std::exit(1);
    }
    if (cfg.gateway_ip.empty()) {
        std::cerr << "Error: --gateway-ip is required\n";
        print_usage("ws_echo_client");
        std::exit(1);
    }

    // Generate random ephemeral port if not specified
    if (cfg.local_port == 0) {
        uint16_t rnd;
        RAND_bytes(reinterpret_cast<uint8_t*>(&rnd), sizeof(rnd));
        cfg.local_port = 49152 + (rnd % 16384);  // RFC 6335 dynamic range
    }

    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// DNS resolution (uses kernel getaddrinfo — called before DPDK takes over NIC)
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve hostname to IPv4 address (host byte order).
/// Uses the kernel network stack, so must be called before DPDK binds the NIC
/// (or when using af_packet which shares with the kernel).
static std::expected<uint32_t, std::string> resolve_hostname(const std::string& host) {
    // Try dotted-decimal first
    uint32_t ip = eph::dpdk::net::parse_ipv4(host.c_str());
    if (ip != 0) return ip;

    // DNS lookup via getaddrinfo
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int err = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (err != 0 || !result) {
        return std::unexpected(std::format(
            "DNS resolution failed for '{}': {}", host, gai_strerror(err)));
    }

    auto* addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    // Convert from network byte order to host byte order
    ip = ntohl(addr->sin_addr.s_addr);
    freeaddrinfo(result);

    spdlog::info("DNS resolved: {} -> {}",
                 host, eph::dpdk::net::format_ipv4(ip).data());
    return ip;
}

// ─────────────────────────────────────────────────────────────────────────────
// Split argv at '--' into EAL args and app args
// ─────────────────────────────────────────────────────────────────────────────

struct SplitArgs {
    int    eal_argc;
    char** eal_argv;
    int    app_argc;
    char** app_argv;
};

static SplitArgs split_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            return {
                .eal_argc = i,
                .eal_argv = argv,
                .app_argc = argc - i - 1,
                .app_argv = argv + i + 1,
            };
        }
    }
    // No '--' found: everything goes to EAL, no app args
    return {
        .eal_argc = argc,
        .eal_argv = argv,
        .app_argc = 0,
        .app_argv = nullptr,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Install signal handlers for graceful shutdown
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    spdlog::set_level(spdlog::level::info);

    // ── 1. Split CLI args ────────────────────────────────────────────────────
    auto [eal_argc, eal_argv, app_argc, app_argv] = split_args(argc, argv);
    auto cfg = parse_app_args(app_argc, app_argv);

    spdlog::info("ws_echo_client: target=wss://{}:{}{}", cfg.host, cfg.port, cfg.ws_path);
    spdlog::info("ws_echo_client: local={}:{}, gateway={}",
                 cfg.local_ip, cfg.local_port, cfg.gateway_ip);

    // ── 2. Resolve server hostname (via kernel stack, before DPDK init) ──────
    auto dns_result = resolve_hostname(cfg.host);
    if (!dns_result) {
        spdlog::error("{}", dns_result.error());
        return 1;
    }
    uint32_t server_ip = *dns_result;

    // ── 3. Initialize DPDK EAL ───────────────────────────────────────────────
    auto eal_result = eph::dpdk::eal_init(eal_argc, eal_argv);
    if (!eal_result) {
        spdlog::error("EAL init failed: {}", eal_result.error());
        return 1;
    }
    spdlog::info("EAL initialized ({} args consumed)", *eal_result);

    // ── 4–10 run in an inner scope so DPDK resources (Platform, Transport)
    //    are destroyed BEFORE eal_cleanup(). Calling DPDK APIs after
    //    eal_cleanup triggers use-after-free / segfault. ──────────────────────
    int exit_code = 0;
    int sent_count = 0;
    int recv_count = 0;
    do {
    // ── 4. Create DPDK Platform (NIC port) ───────────────────────────────
    eph::dpdk::PlatformConfig plat_cfg{
        .port_id         = cfg.dpdk_port,
        .nb_rx_queues    = 1,
        .nb_tx_queues    = 1,
        .nb_rx_desc      = 256,
        .nb_tx_desc      = 512,
        .mbuf_pool_size  = 4095,
        .mbuf_cache_size = 256,
    };
    static_assert(eph::dpdk::config_ok(eph::dpdk::PlatformConfig{
        .port_id = 0, .nb_rx_queues = 1, .nb_tx_queues = 1,
        .nb_rx_desc = 256, .nb_tx_desc = 512,
        .mbuf_pool_size = 4095, .mbuf_cache_size = 256,
    }));

    auto platform = eph::dpdk::Platform::create(plat_cfg);
    if (!platform) {
        spdlog::error("Platform create failed: {}", platform.error());
        exit_code = 1;
        break;
    }
    spdlog::info("Platform ready: port_id={}", platform->port_id());

    // ── 5. Get source MAC from NIC ───────────────────────────────────────
    rte_ether_addr src_mac{};
    if (int ret = rte_eth_macaddr_get(cfg.dpdk_port, &src_mac); ret != 0) {
        spdlog::error("Failed to get MAC address for port {}: {}",
                      cfg.dpdk_port, rte_strerror(-ret));
        exit_code = 1;
        break;
    }
    spdlog::info("Source MAC: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                 src_mac.addr_bytes[0], src_mac.addr_bytes[1],
                 src_mac.addr_bytes[2], src_mac.addr_bytes[3],
                 src_mac.addr_bytes[4], src_mac.addr_bytes[5]);

    // ── 6. Resolve gateway MAC via ARP ───────────────────────────────────
    uint32_t local_ip   = eph::dpdk::net::parse_ipv4(cfg.local_ip.c_str());
    uint32_t gateway_ip = eph::dpdk::net::parse_ipv4(cfg.gateway_ip.c_str());

    spdlog::info("Resolving gateway MAC for {}...",
                 eph::dpdk::net::format_ipv4(gateway_ip).data());

    auto arp_result = eph::dpdk::arp::resolve(
        cfg.dpdk_port, 0, platform->mempool(),
        src_mac, local_ip, gateway_ip,
        std::chrono::milliseconds{3000});

    if (!arp_result) {
        spdlog::error("ARP resolution failed: {}", arp_result.error());
        exit_code = 1;
        break;
    }
    rte_ether_addr dst_mac = *arp_result;
    spdlog::info("Gateway MAC resolved: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                 dst_mac.addr_bytes[0], dst_mac.addr_bytes[1],
                 dst_mac.addr_bytes[2], dst_mac.addr_bytes[3],
                 dst_mac.addr_bytes[4], dst_mac.addr_bytes[5]);

    // ── 7. Suppress kernel RST (required for af_packet mode) ─────────────
    // With af_packet PMD, the kernel TCP stack also sees incoming SYN-ACKs
    // and sends RST because it has no matching socket. The guard installs a
    // firewall rule to drop those RSTs, and removes it on destruction.
    auto server_ip_str = std::string(eph::dpdk::net::format_ipv4(server_ip).data());
    auto rst_result = KernelRstGuard::install(cfg.local_ip, server_ip_str, cfg.port);
    if (!rst_result) {
        spdlog::warn("{}", rst_result.error());
    }
    std::optional<KernelRstGuard> rst_guard;
    if (rst_result) {
        rst_guard = std::move(*rst_result);
    }

    // ── 8. Create DpdkTransport (TCP + TLS + WS) ─────────────────────────
    //
    // The Transport::create factory pattern:
    //   1. Call the TcpFactory lambda to get a connected TcpSession
    //   2. Perform TLS 1.3 handshake over the TCP session
    //   3. Send HTTP Upgrade to establish WebSocket
    //   4. Extract TLS keys for hot-path AEAD
    //   5. Launch TX/RX worker threads

    // IP dst = actual server IP (for IP header routing)
    // MAC dst = gateway MAC (for L2 forwarding to gateway)
    eph::dpdk::TcpConfig tcp_cfg{
        .tuple = {
            .src_ip   = local_ip,
            .dst_ip   = server_ip,
            .src_port = cfg.local_port,
            .dst_port = cfg.port,
        },
        .src_mac     = src_mac,
        .dst_mac     = dst_mac,
        .port_id     = cfg.dpdk_port,
        .tx_queue_id = 0,
        .rx_queue_id = 0,
    };

    auto* mempool = platform->mempool();

    // TcpFactory: creates a new TcpSession and performs TCP 3-way handshake
    auto tcp_factory = [&]()
        -> std::expected<std::unique_ptr<eph::dpdk::TcpSession>, std::string> {
        auto tcp = std::make_unique<eph::dpdk::TcpSession>(tcp_cfg, mempool);
        auto r = tcp->connect(std::chrono::milliseconds{5000});
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    eph::net::TransportConfig transport_cfg{
        .remote_host  = cfg.host,
        .remote_port  = cfg.port,
        .ws_path      = cfg.ws_path,
        .ca_cert_path = cfg.ca_cert,
        .verify_peer  = !cfg.no_verify,
        .tcp_timeout  = std::chrono::milliseconds{5000},
        .tls_timeout  = std::chrono::milliseconds{5000},
        .ws_timeout   = std::chrono::milliseconds{5000},
        .ping_interval = std::chrono::seconds{30},
    };

    spdlog::info("Connecting to wss://{}:{}{}...", cfg.host, cfg.port, cfg.ws_path);

    auto transport = eph::dpdk::DpdkTransport::create(
        std::move(tcp_factory), transport_cfg);

    if (!transport) {
        spdlog::error("Transport create failed: {}", transport.error());
        exit_code = 1;
        break;
    }
    spdlog::info("WSS connection established");

    // ── 9. Echo loop: send message, receive echo ─────────────────────────
    auto& tp = **transport;
    auto interval = std::chrono::milliseconds(cfg.interval_ms);

    while (!g_stop && tp.is_running()) {
        // Send
        if (cfg.count == 0 || sent_count < cfg.count) {
            std::string payload = (cfg.count > 1 || cfg.count == 0)
                ? std::format("[{}] {}", sent_count + 1, cfg.message)
                : cfg.message;

            int rc = tp.send_text(payload.data(), payload.size());
            if (rc == 0) {
                spdlog::info(">> Sent: {}", payload);
                ++sent_count;
            } else if (rc == -EAGAIN) {
                spdlog::warn("TX queue full, retrying...");
            } else {
                spdlog::error("Send failed: {}", rc);
                break;
            }
        }

        // Poll for echoed responses
        auto recv_deadline = std::chrono::steady_clock::now() + interval;
        while (std::chrono::steady_clock::now() < recv_deadline && !g_stop) {
            bool got = tp.recv([&](const uint8_t* data, uint16_t len) {
                std::string_view echo(reinterpret_cast<const char*>(data), len);
                spdlog::info("<< Recv: {}", echo);
                ++recv_count;
            });

            if (!got) {
                // Brief pause to avoid busy-spinning the app thread
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        // Done if we've sent and received all expected messages
        if (cfg.count > 0 && sent_count >= cfg.count && recv_count >= cfg.count) {
            break;
        }
    }

    // ── 10. Shutdown ─────────────────────────────────────────────────────
    spdlog::info("Shutting down...");
    tp.stop();

    auto stats = tp.stats();
    spdlog::info("Stats: tx_packets={} tx_bytes={} rx_packets={} rx_bytes={} "
                 "reconnects={} encrypt_err={} decrypt_err={}",
                 stats.tx_packets, stats.tx_bytes,
                 stats.rx_packets, stats.rx_bytes,
                 stats.reconnect_count,
                 stats.encrypt_errors, stats.decrypt_errors);

    } while (false); // All DPDK resources (transport, platform) destroyed here

    eph::dpdk::eal_cleanup();
    spdlog::info("Done. Sent={} Received={}", sent_count, recv_count);
    return exit_code;
}

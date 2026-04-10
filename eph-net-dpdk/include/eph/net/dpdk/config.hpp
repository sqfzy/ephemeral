#pragma once

/// @file config.hpp
/// Configuration aggregates for `DpdkTcpStream` / `DpdkUdpSocket` /
/// `DpdkPoller`. Part of Phase 4 of the v3.3 refactor.
///
/// These types are intentionally thin adapters over the existing
/// `eph::dpdk::TcpConfig` / `eph::dpdk::UdpConfig` structures in the legacy
/// `eph-dpdk` module. Phase 4 is additive: we do not duplicate the validation
/// / field definitions here — we re-export them under the new namespace so
/// v3.3 user code can write `eph::net::dpdk::StreamConfig` without pulling in
/// the legacy header name at the call site.
///
/// Phase 7 will migrate the underlying struct definitions into this module
/// and drop the re-export shim.

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "eph/dpdk/reactor.hpp"   // ReactorConfig (lcore / port / queue)
#include "eph/dpdk/tcp.hpp"       // TcpConfig
#include "eph/dpdk/udp.hpp"       // UdpConfig (legacy naming collides with our
                                  // eph::net::dpdk::UdpConfig below — we keep
                                  // the legacy type in eph::dpdk::UdpConfig,
                                  // and our config wrapper names it
                                  // LegacyUdpConfig at its single use-site).
#include "eph/net/reconnect_policy.hpp"

// Phase 5: the DPDK-side StreamConfig deliberately does NOT carry an
// `eph::net::TlsConfig` field because including `tls_constants.hpp`
// (which lives in the aws-lc-using eph-transport stack) would conflict
// with the **vcpkg openssl** that `eph::dpdk::tcp.hpp` already requires
// for `<openssl/rand.h>`. The two openssl implementations have
// ABI-incompatible typedefs (`CRYPTO_THREADID`, `ASN1_NULL`, ...) and
// CANNOT coexist in the same TU. See the BLOCKER note at the top of
// `eph/net/dpdk/tcp_stream.hpp` for the full story.
//
// Until Phase 5.5 / Phase 7 resolves the legacy `RAND_bytes` dependency,
// the DPDK-side TLS path is structurally complete in
// `eph/net/dpdk/detail/tls_state.hpp` but is not reachable from the
// `DpdkTcpStream::create()` factory.

namespace eph::net::dpdk {

// ---------------------------------------------------------------------------
// StreamConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `DpdkTcpStream::create`.
///
/// Phase 4 scope: the heavy DPDK wiring (MAC, mempool, port/queue IDs,
/// MSS, recv_window) lives inside `eph::dpdk::TcpConfig` and is preserved
/// verbatim via the `legacy` field. We add v3.3-only knobs on top (connect
/// timeout, reconnect policy) without duplicating any of the validated
/// low-level fields.
///
/// Design rationale for embedding the legacy config rather than flattening
/// its fields: Phase 4 is explicitly additive. Flattening would couple the
/// field list to two different validation paths and require Phase 7 to
/// re-harmonise them. Embedding keeps the single source of truth in
/// `eph::dpdk::TcpConfig` until Phase 7 deletes the legacy header.
struct StreamConfig {
    /// @brief Underlying DPDK TcpSession configuration (mempool, port/queue,
    ///        4-tuple, MAC addresses, MSS). Must be populated by the caller;
    ///        validation runs in `DpdkTcpStream::create()` via
    ///        `legacy.validate()`.
    ::eph::dpdk::TcpConfig legacy{};

    /// @brief The DPDK mempool used for TcpSession mbuf allocations. Kept as a
    ///        top-level field so callers can use designated initialization
    ///        even though the legacy TcpConfig does not itself hold a
    ///        mempool pointer (historically the mempool was passed separately
    ///        to the `TcpSession(config, pool)` ctor).
    ::rte_mempool* pool{nullptr};

    /// @brief TCP handshake deadline (SYN -> SYN/ACK -> ACK round trip).
    std::chrono::milliseconds connect_timeout{3000};

    /// @brief Reconnection policy applied by higher-level recovery code.
    ReconnectPolicyConfig reconnect{};

    // Phase 5 NOTE: TLS configuration omitted from the DPDK-side
    // StreamConfig — see the comment block at the top of this file for
    // the openssl/aws-lc TU conflict that blocks it. The DPDK TLS path
    // remains a structural stub until Phase 5.5.
};

// ---------------------------------------------------------------------------
// UdpConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `DpdkUdpSocket::create`.
///
/// Parallels `StreamConfig`: wraps the legacy `eph::dpdk::UdpConfig`
/// (fixed-peer UDP packet template) and adds v3.3-only knobs. The legacy
/// config carries src/dst MAC, IP, port, mempool, port_id, tx_queue_id, and
/// hw_cksum — exactly what `UdpPacketTemplate::init` needs.
struct UdpConfig {
    /// @brief Underlying DPDK UdpSender configuration.
    ::eph::dpdk::UdpConfig legacy{};
};

// ---------------------------------------------------------------------------
// PollerConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `DpdkPoller::create`.
///
/// DPDK lcore burst poll has no epoll equivalent: the Poller simply calls
/// `rte_eth_rx_burst` on a single {port_id, rx_queue_id} and routes each
/// mbuf to a registered Pollable based on its 4-tuple. We therefore reuse
/// `eph::dpdk::ReactorConfig` verbatim.
struct PollerConfig {
    /// @brief DPDK port ID on which to poll.
    uint16_t port_id{0};

    /// @brief DPDK RX queue index on the above port.
    uint16_t rx_queue_id{0};

    /// @brief CPU affinity for the thread that drives `poll()`. -1 = no pin.
    ///        The Poller does not spawn a thread of its own — affinity is a
    ///        hint for the user's polling thread.
    int rx_cpu{-1};

    /// @brief Initial registered-Pollable vector capacity reservation.
    std::size_t initial_capacity{16};
};

} // namespace eph::net::dpdk

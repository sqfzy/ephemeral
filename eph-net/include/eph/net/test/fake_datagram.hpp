#pragma once

/// @file test/fake_datagram.hpp
/// In-memory `Datagram` concept mock for unit tests.
///
/// `FakeDatagram`
/// is the UDP-side counterpart to `FakeStream`: injects/collects whole
/// datagrams, tracks multicast joins/leaves in an std::vector, and plugs
/// into `TestPoller` with zero syscalls.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "eph/core/error.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/socket_addr.hpp"

namespace eph::net::test {

// ---------------------------------------------------------------------------
// FakeDatagram
// ---------------------------------------------------------------------------

/// @brief In-memory mock satisfying `eph::net::Datagram`.
///
/// Unlike `FakeStream`, a datagram mock must preserve message boundaries
/// through both the rx queue (one entry per injected datagram) and the tx
/// queue (one entry per `send_to` call). This matches the UDP semantics
/// the real backends expose.
class FakeDatagram {
public:
    /// @brief Associated PacketView — contiguous read-only span.
    struct PacketView {
        const uint8_t* data_;
        std::size_t    length_;

        [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
        [[nodiscard]] std::size_t length() const noexcept { return length_; }
    };

    /// @brief No codec attached — tests layer a codec on top.
    using CodecType = void;

    /// @brief Receive callback: `(data, length, source_addr)`, matching the
    ///        real `eph::net::kernel::UdpSocket::OnDatagram` shape.
    using OnDatagram = std::function<void(const uint8_t*, uint16_t,
                                          const SocketAddr&)>;

    /// @brief One queued incoming datagram.
    struct RxEntry {
        std::vector<uint8_t> data;
        SocketAddr           src;
    };

    /// @brief One recorded outgoing datagram.
    struct TxEntry {
        std::vector<uint8_t> data;
        SocketAddr           dst;
    };

    FakeDatagram() = default;

    [[nodiscard]] static std::unique_ptr<FakeDatagram> create() {
        return std::make_unique<FakeDatagram>();
    }

    // ── Test-control API ───────────────────────────────────────────────────

    /// @brief Queue a simulated incoming datagram. Each call appends one
    ///        distinct datagram to the rx queue, preserving message
    ///        boundaries.
    void inject_datagram(std::span<const uint8_t> data, const SocketAddr& src) {
        RxEntry e;
        e.data.assign(data.begin(), data.end());
        e.src = src;
        rx_queue_.push_back(std::move(e));
    }

    /// @brief Return a const view of all tx datagrams recorded so far.
    [[nodiscard]] const std::vector<TxEntry>& collect_tx() const noexcept {
        return tx_queue_;
    }

    void clear_tx() noexcept { tx_queue_.clear(); }

    void set_attached(bool a) noexcept { attached_ = a; }

    /// @brief Accessor for the currently-joined multicast groups. Order is
    ///        the order in which `join_multicast` was called; duplicates are
    ///        preserved so tests can detect bad double-join code.
    [[nodiscard]] const std::vector<SocketAddr>& joined_groups() const noexcept {
        return joined_;
    }

    // ── Datagram concept implementation ────────────────────────────────────

    OnDatagram on_datagram;

    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send_to(std::span<const uint8_t> data, const SocketAddr& dst) {
        if (!attached_) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "FakeDatagram::send_to before attach"});
        }
        TxEntry e;
        e.data.assign(data.begin(), data.end());
        e.dst = dst;
        tx_queue_.push_back(std::move(e));
        return data.size();
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    join_multicast(const SocketAddr& group) {
        joined_.push_back(group);
        return {};
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    leave_multicast(const SocketAddr& group) {
        // Remove the first occurrence. Tests that care about multiple
        // concurrent joins on the same group can inspect `joined_groups`
        // directly.
        auto it = std::find(joined_.begin(), joined_.end(), group);
        if (it != joined_.end()) joined_.erase(it);
        return {};
    }

    [[nodiscard]] bool is_attached() const noexcept { return attached_; }

    // ── Pollable concept implementation ───────────────────────────────────

    /// @brief Drain the rx queue by invoking `on_datagram` once per queued
    ///        datagram. Returns the number of datagrams delivered.
    std::size_t poll_once_() noexcept {
        if (rx_queue_.empty()) return 0;
        std::size_t delivered = 0;
        if (on_datagram) {
            for (const auto& e : rx_queue_) {
                const auto len = static_cast<uint16_t>(
                    e.data.size() > 0xFFFF ? 0xFFFF : e.data.size());
                on_datagram(e.data.data(), len, e.src);
                ++delivered;
            }
        } else {
            delivered = rx_queue_.size();
        }
        rx_queue_.clear();
        return delivered;
    }

    [[nodiscard]] bool is_attached_() const noexcept { return attached_; }

    [[nodiscard]] void* native_handle() const noexcept {
        return const_cast<void*>(static_cast<const void*>(this));
    }

private:
    std::vector<RxEntry>    rx_queue_;
    std::vector<TxEntry>    tx_queue_;
    std::vector<SocketAddr> joined_;
    bool                    attached_{false};
};

// Compile-time concept conformance checks.
static_assert(Pollable<FakeDatagram>,
              "FakeDatagram must satisfy eph::net::Pollable");
static_assert(Datagram<FakeDatagram>,
              "FakeDatagram must satisfy eph::net::Datagram");

} // namespace eph::net::test

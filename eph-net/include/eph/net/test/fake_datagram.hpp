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
#include "eph/core/packet_view.hpp"
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
    /// @brief Associated PacketView — conforming `eph::core::PacketView`.
    ///
    /// Mirrors the kernel `SpanView` / DPDK `MbufView` layout so that real
    /// DatagramCodec implementations (e.g. Mold64Codec) can decode directly
    /// off a FakeDatagram in unit tests. Previously a 2-field stub, which
    /// made the mock drift from the real backends.
    struct PacketView {
        constexpr PacketView(uint8_t* base, std::size_t len,
                             uint64_t tsc = 0) noexcept
            : base_(base), head_(0), tail_(len), tsc_(tsc) {}

        [[nodiscard]] uint8_t* writable_data() noexcept {
            return base_ + head_;
        }
        [[nodiscard]] const uint8_t* data() const noexcept {
            return base_ + head_;
        }
        [[nodiscard]] std::size_t length() const noexcept {
            return tail_ - head_;
        }

        constexpr void trim_front(std::size_t n) noexcept { head_ += n; }
        constexpr void trim_back(std::size_t n) noexcept { tail_ -= n; }

        [[nodiscard]] uint64_t arrival_tsc() const noexcept { return tsc_; }

    private:
        uint8_t*    base_{nullptr};
        std::size_t head_{0};
        std::size_t tail_{0};
        uint64_t    tsc_{0};
    };

    /// @brief No codec attached — tests layer a codec on top.
    using CodecType = void;

    /// @brief Receive callback: `(app_datagram, peer)`, matching the real
    ///        `eph::net::kernel::UdpSocket::OnDatagram` shape.
    ///
    /// The span holds one decoded application datagram (post-codec if the
    /// fake is composed with a codec); `peer` is the source address.
    using OnDatagram = std::function<void(std::span<const uint8_t>,
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

    /// @brief Queue a simulated incoming datagram. `wire_bytes` are raw
    ///        pre-codec bytes (a whole datagram boundary). Each call appends
    ///        one distinct datagram to the rx queue, preserving boundaries.
    void inject_datagram(std::span<const uint8_t> wire_bytes, const SocketAddr& src) {
        RxEntry e;
        e.data.assign(wire_bytes.begin(), wire_bytes.end());
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
    send_to(std::span<const uint8_t> app_payload, const SocketAddr& dst) noexcept {
        if (!attached_) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "FakeDatagram::send_to before attach"});
        }
        TxEntry e;
        e.data.assign(app_payload.begin(), app_payload.end());
        e.dst = dst;
        tx_queue_.push_back(std::move(e));
        return app_payload.size();
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    join_multicast(const SocketAddr& group) noexcept {
        joined_.push_back(group);
        return {};
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    leave_multicast(const SocketAddr& group) noexcept {
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
                on_datagram(std::span<const uint8_t>(e.data.data(), e.data.size()),
                            e.src);
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
static_assert(::eph::core::PacketView<FakeDatagram::PacketView>,
              "FakeDatagram::PacketView must satisfy eph::core::PacketView");
static_assert(Pollable<FakeDatagram>,
              "FakeDatagram must satisfy eph::net::Pollable");
static_assert(Datagram<FakeDatagram>,
              "FakeDatagram must satisfy eph::net::Datagram");

} // namespace eph::net::test

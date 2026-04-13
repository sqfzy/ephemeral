/// @file test_ws_handshake.cpp
/// Unit tests for `eph::net::detail::perform_ws_handshake` using a fake
/// in-memory ByteSink — no sockets, no threads. Covers the success path
/// and every defined failure mode.


#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/detail/ws_handshake.hpp"
#include "eph/net/http.hpp"

using eph::core::Error;
using eph::core::ErrorInfo;
using eph::net::HttpHeader;
using eph::net::detail::perform_ws_handshake;
using eph::net::detail::ws_compute_accept;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// FakeByteSink — records every byte sent by the caller and serves a
// caller-configured script of recv chunks. If the script runs out before
// the caller finishes recving we return WouldBlock (which
// perform_ws_handshake retries until its deadline trips) or Disconnected
// (which bubbles straight up) depending on test intent.
// ─────────────────────────────────────────────────────────────────────────
struct FakeByteSink {
    std::vector<uint8_t> tx;       ///< everything the caller sent, concatenated
    std::vector<uint8_t> rx_script;///< bytes the recv() calls will deliver
    size_t               rx_off{0};///< how many bytes of rx_script already served
    bool                 recv_blocks{false};   ///< if true, recv returns WouldBlock
    bool                 recv_disconnect{false};///< if true, recv returns Disconnected

    std::expected<size_t, ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        tx.insert(tx.end(), data.begin(), data.end());
        return data.size();
    }

    std::expected<size_t, ErrorInfo>
    recv(uint8_t* buf, size_t cap) noexcept {
        if (recv_disconnect) {
            return std::unexpected(ErrorInfo{Error::Disconnected,
                                              "FakeByteSink: scripted disconnect"});
        }
        if (recv_blocks || rx_off >= rx_script.size()) {
            return std::unexpected(ErrorInfo{Error::WouldBlock,
                                              "FakeByteSink: scripted would-block"});
        }
        const size_t avail = rx_script.size() - rx_off;
        const size_t n     = std::min(cap, std::min<size_t>(avail, 64));
        std::memcpy(buf, rx_script.data() + rx_off, n);
        rx_off += n;
        return n;
    }
};

// Extract the Sec-WebSocket-Key the caller sent in its request. We do a
// naive substring search — the request is always well-formed (we built it)
// so we can rely on the header name appearing verbatim.
std::string extract_sent_key(const std::vector<uint8_t>& tx) {
    std::string_view s(reinterpret_cast<const char*>(tx.data()), tx.size());
    constexpr std::string_view needle = "Sec-WebSocket-Key: ";
    auto p = s.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();
    auto e = s.find("\r\n", p);
    if (e == std::string_view::npos) return {};
    return std::string(s.substr(p, e - p));
}

// Build a well-formed 101 response with caller-supplied Accept, Upgrade,
// and Connection values.
std::vector<uint8_t> make_response(std::string_view accept,
                                   std::string_view upgrade    = "websocket",
                                   std::string_view connection = "Upgrade",
                                   int              status     = 101,
                                   std::string_view reason     = "Switching Protocols",
                                   std::string_view extensions = {}) {
    std::string s;
    s += "HTTP/1.1 " + std::to_string(status) + " ";
    s += std::string(reason);
    s += "\r\n";
    s += "Upgrade: ";  s += std::string(upgrade);    s += "\r\n";
    s += "Connection: "; s += std::string(connection); s += "\r\n";
    s += "Sec-WebSocket-Accept: "; s += std::string(accept); s += "\r\n";
    if (!extensions.empty()) {
        s += "Sec-WebSocket-Extensions: "; s += std::string(extensions); s += "\r\n";
    }
    s += "\r\n";
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Success path — a correctly-formed 101 response validates cleanly
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, HappyPathReturnsOk) {
    FakeByteSink io;
    // We need to know the key first to precompute the Accept. Drive the
    // handshake in two passes: pass 1 captures the key, pass 2 supplies
    // the matching response. (perform_ws_handshake generates a fresh
    // random key each call, so we must observe it via FakeByteSink.tx.)
    //
    // Approach: configure rx_script AFTER the first send() has populated
    // `tx`. We simulate this by starting with `recv_blocks=true`, running
    // a mini driver, then swapping in the response. Instead, a simpler
    // alternative: record key from tx first via a zero-recv dry-run.
    //
    // Simplest path: use a driver that interleaves send→build response→recv.
    // Since perform_ws_handshake is synchronous we cannot interleave from
    // outside. We instead pre-load a DUMMY response, let the handshake
    // fail, then re-run with the captured key. Cleaner option: compute
    // the Accept AFTER we observe tx — but the handshake already failed
    // the first time. Use a two-phase FakeByteSink that swaps the
    // response script between the send() call and the first recv().
    //
    // The FakeByteSink we defined serves rx_script linearly — we can
    // populate it from a custom send override. Easiest: subclass inline.
    struct Swapping : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            // After the request is fully buffered, build the response.
            if (rx_script.empty()) {
                std::string_view s(reinterpret_cast<const char*>(tx.data()),
                                    tx.size());
                if (s.find("\r\n\r\n") != std::string_view::npos) {
                    auto key = extract_sent_key(tx);
                    auto acc = ws_compute_accept(key);
                    rx_script = make_response(acc);
                }
            }
            return r;
        }
    };

    Swapping sink;
    auto r = perform_ws_handshake(sink, "example.com", "/ws/feed",
                                    {}, std::chrono::seconds{1});
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);

    // Request wire check: method/target/host/mandatory headers present.
    std::string_view sent(reinterpret_cast<const char*>(sink.tx.data()),
                           sink.tx.size());
    EXPECT_NE(sent.find("GET /ws/feed HTTP/1.1\r\n"), std::string_view::npos);
    EXPECT_NE(sent.find("Host: example.com\r\n"), std::string_view::npos);
    EXPECT_NE(sent.find("Upgrade: websocket\r\n"), std::string_view::npos);
    EXPECT_NE(sent.find("Connection: Upgrade\r\n"), std::string_view::npos);
    EXPECT_NE(sent.find("Sec-WebSocket-Version: 13\r\n"),
              std::string_view::npos);
    EXPECT_NE(sent.find("Sec-WebSocket-Key: "), std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Sec-WebSocket-Accept is computed as SHA1(key + magic) + base64
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, AcceptDigestMatchesRfc6455Vector) {
    // RFC 6455 §1.3 worked example:
    //   key    = "dGhlIHNhbXBsZSBub25jZQ=="
    //   accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    std::string got = ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ==");
    EXPECT_EQ(got, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// ═══════════════════════════════════════════════════════════════════════
// 3. Non-101 status rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, NonOneHundredOneStatusRejected) {
    struct Bad : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)),
                                           "websocket", "Upgrade", 200, "OK");
            }
            return r;
        }
    };
    Bad sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 4. Missing Upgrade header rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, MissingUpgradeHeaderRejected) {
    struct NoUp : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                // Response with Upgrade replaced by a wrong value.
                std::string s = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: http2\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: ";
                s += ws_compute_accept(extract_sent_key(tx));
                s += "\r\n\r\n";
                rx_script.assign(s.begin(), s.end());
            }
            return r;
        }
    };
    NoUp sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 5. Missing "Connection: Upgrade" rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, MissingConnectionUpgradeRejected) {
    struct NoConn : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)),
                                           "websocket", "keep-alive");
            }
            return r;
        }
    };
    NoConn sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 5b. Case-insensitive "Connection: upgrade" accepted
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ConnectionCaseInsensitiveAccepted) {
    struct Mixed : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)),
                                           "WebSocket", "keep-alive, UpGrAdE");
            }
            return r;
        }
    };
    Mixed sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
}

// ═══════════════════════════════════════════════════════════════════════
// 6. Wrong Sec-WebSocket-Accept rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, WrongSecWebSocketAcceptRejected) {
    struct BadAcc : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response("ZGVsaWJlcmF0ZWx5X3dyb25nX3ZhbHVlPQ==");
            }
            return r;
        }
    };
    BadAcc sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 7. Parser error in response (malformed status line) → WsHandshakeFailed
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ParserErrorInResponseRejected) {
    struct Malformed : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                const char bad[] =
                    "HTTP/9.9 101 Switching Protocols\r\n\r\n";
                rx_script.assign(bad, bad + sizeof(bad) - 1);
            }
            return r;
        }
    };
    Malformed sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 8. Timeout waiting for response → Error::Timeout
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ReadDeadlineTriggersTimeout) {
    FakeByteSink sink;
    sink.recv_blocks = true;
    // Short deadline — 50ms is enough to build the request and then fail
    // the read phase quickly.
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::milliseconds{50});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Timeout);
}

// ═══════════════════════════════════════════════════════════════════════
// 9. extra_headers are serialized into the request
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ExtraHeadersAppear) {
    struct Good : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)));
            }
            return r;
        }
    };
    Good sink;
    HttpHeader extras[] = {
        {"Authorization", "Bearer abc"},
        {"X-Custom",      "ok"},
    };
    auto r = perform_ws_handshake(sink, "h", "/ws",
                                    std::span<const HttpHeader>(extras, 2),
                                    std::chrono::seconds{1});
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
    std::string_view s(reinterpret_cast<const char*>(sink.tx.data()),
                        sink.tx.size());
    EXPECT_NE(s.find("Authorization: Bearer abc\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("X-Custom: ok\r\n"), std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 10. Host header is set from the caller argument
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, HostHeaderFromArgument) {
    struct Good : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)));
            }
            return r;
        }
    };
    Good sink;
    auto r = perform_ws_handshake(sink, "stream.binance.com",
                                    "/ws/btcusdt@bookTicker",
                                    {}, std::chrono::seconds{1});
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
    std::string_view s(reinterpret_cast<const char*>(sink.tx.data()),
                        sink.tx.size());
    EXPECT_NE(s.find("Host: stream.binance.com\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("GET /ws/btcusdt@bookTicker HTTP/1.1\r\n"),
              std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 11. Leftover post-handshake bytes are captured for caller consumption
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, LeftoverBytesCapturedForCaller) {
    struct WithTail : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(ws_compute_accept(extract_sent_key(tx)));
                // Append 5 "leftover" bytes that belong to the next layer.
                const uint8_t tail[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xAA};
                rx_script.insert(rx_script.end(), tail, tail + sizeof(tail));
            }
            return r;
        }
    };
    WithTail sink;
    std::vector<uint8_t> leftover;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1}, &leftover);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
    ASSERT_EQ(leftover.size(), 5u);
    EXPECT_EQ(leftover[0], 0xDE);
    EXPECT_EQ(leftover[4], 0xAA);
}

// ═══════════════════════════════════════════════════════════════════════
// 12. Server-initiated extensions (permessage-deflate) rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(WsHandshake, ServerExtensionsRejected) {
    struct WithExt : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(
                    ws_compute_accept(extract_sent_key(tx)),
                    "websocket", "Upgrade", 101,
                    "Switching Protocols", "permessage-deflate");
            }
            return r;
        }
    };
    WithExt sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

TEST(WsHandshake, ServerExtensionsRejectedEvenUnknown) {
    struct WithExt : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(
                    ws_compute_accept(extract_sent_key(tx)),
                    "websocket", "Upgrade", 101,
                    "Switching Protocols", "x-some-unknown-extension");
            }
            return r;
        }
    };
    WithExt sink;
    auto r = perform_ws_handshake(sink, "h", "/ws", {},
                                    std::chrono::seconds{1});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsHandshakeFailed);
}

/// @file test_http_connect.cpp
/// Unit tests for `eph::net::detail::perform_http_connect` over an
/// in-memory FakeByteSink — no sockets, no threads. Covers the success path
/// and every defined failure mode.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/detail/http_connect.hpp"
#include "eph/net/proxy.hpp"

using eph::core::Error;
using eph::core::ErrorInfo;
using eph::net::ProxyConfig;
using eph::net::detail::perform_http_connect;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// FakeByteSink — captures every send() and serves a scripted rx stream
// whose contents are set by the test body (often patched in from an
// override of send() after observing the request).
// ─────────────────────────────────────────────────────────────────────────
struct FakeByteSink {
    std::vector<uint8_t> tx;
    std::vector<uint8_t> rx_script;
    size_t               rx_off{0};
    bool                 recv_blocks{false};
    bool                 recv_disconnect{false};

    std::expected<size_t, ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        tx.insert(tx.end(), data.begin(), data.end());
        return data.size();
    }

    std::expected<size_t, ErrorInfo>
    recv(uint8_t* buf, size_t cap) noexcept {
        if (recv_disconnect) {
            return std::unexpected(ErrorInfo{Error::Disconnected,
                                              "FakeByteSink: disconnected"});
        }
        if (recv_blocks || rx_off >= rx_script.size()) {
            return std::unexpected(ErrorInfo{Error::WouldBlock,
                                              "FakeByteSink: would-block"});
        }
        const size_t avail = rx_script.size() - rx_off;
        // Deliberately chunked to exercise the incremental parse loop.
        const size_t n     = std::min(cap, std::min<size_t>(avail, 64));
        std::memcpy(buf, rx_script.data() + rx_off, n);
        rx_off += n;
        return n;
    }
};

ProxyConfig make_proxy(bool with_auth = false) {
    ProxyConfig p{.host = "10.0.0.1", .port = 3128};
    p.timeout = std::chrono::seconds{1};
    if (with_auth) {
        p.basic_auth_user = "user";
        p.basic_auth_pass = "pass";
    }
    return p;
}

std::vector<uint8_t> make_response(int status,
                                   std::string_view reason,
                                   std::string_view tail = "") {
    std::string s;
    s += "HTTP/1.1 ";
    s += std::to_string(status);
    s += ' ';
    s += std::string(reason);
    s += "\r\n";
    s += "Content-Length: 0\r\n";
    s += "\r\n";
    s += std::string(tail);
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Success path — a 200 response unblocks the tunnel
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, SuccessPathReturnsOk) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(200, "Connection established");
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    auto r = perform_http_connect(sink, p, "binance.com", 443);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);

    // The request must have been a CONNECT with the full host:port target.
    std::string_view sent(reinterpret_cast<const char*>(sink.tx.data()),
                           sink.tx.size());
    EXPECT_NE(sent.find("CONNECT binance.com:443 HTTP/1.1\r\n"),
              std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Success with Basic auth — Proxy-Authorization header is serialized
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, SuccessWithBasicAuthHeader) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(200, "OK");
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy(true);
    auto r = perform_http_connect(sink, p, "binance.com", 443);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);

    std::string_view sent(reinterpret_cast<const char*>(sink.tx.data()),
                           sink.tx.size());
    // RFC 7617 Basic auth for "user:pass" = "dXNlcjpwYXNz"
    EXPECT_NE(sent.find("Proxy-Authorization: Basic dXNlcjpwYXNz\r\n"),
              std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 3. 407 Proxy Auth Required → ProxyAuthRequired
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, ProxyAuthRequiredOn407) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(407, "Proxy Authentication Required");
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    auto r = perform_http_connect(sink, p, "h", 443);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::ProxyAuthRequired);
}

// ═══════════════════════════════════════════════════════════════════════
// 4. 502 Bad Gateway → ProxyHandshakeFailed
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, ProxyHandshakeFailedOn502) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(502, "Bad Gateway");
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    auto r = perform_http_connect(sink, p, "h", 443);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::ProxyHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 5. 503 Service Unavailable → ProxyHandshakeFailed
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, ProxyHandshakeFailedOn503) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(503, "Service Unavailable");
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    auto r = perform_http_connect(sink, p, "h", 443);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::ProxyHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 6. Malformed response → ProxyHandshakeFailed
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, MalformedResponseRejected) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                // Wrong HTTP version token triggers the parser's rejection
                // logic (same failure mode exercised by the WS tests).
                const char bad[] =
                    "HTTP/9.9 200 Connection established\r\n\r\n";
                rx_script.assign(bad, bad + sizeof(bad) - 1);
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    auto r = perform_http_connect(sink, p, "h", 443);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::ProxyHandshakeFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 7. Read deadline trips → Error::Timeout
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, ReadDeadlineTriggersTimeout) {
    FakeByteSink sink;
    sink.recv_blocks = true;
    auto p = make_proxy();
    p.timeout = std::chrono::milliseconds{50};
    auto r = perform_http_connect(sink, p, "h", 443);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Timeout);
}

// ═══════════════════════════════════════════════════════════════════════
// 8. Partial response then drop → Timeout
//
// The sink delivers the first few bytes of a response, then starts
// returning WouldBlock forever. The parse loop stays Incomplete, the
// deadline ticks past, and perform_http_connect reports Timeout.
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, PartialResponseThenDropIsTimeout) {
    struct Partial : FakeByteSink {
        int served{0};
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                const char frag[] = "HTTP/1.1 200 Co";
                rx_script.assign(frag, frag + sizeof(frag) - 1);
            }
            return r;
        }
        std::expected<size_t, ErrorInfo>
        recv(uint8_t* buf, size_t cap) noexcept {
            if (served >= 1) {
                return std::unexpected(ErrorInfo{Error::WouldBlock,
                                                   "Partial: drained"});
            }
            auto r = FakeByteSink::recv(buf, cap);
            if (r) ++served;
            return r;
        }
    };
    Partial sink;
    auto p = make_proxy();
    p.timeout = std::chrono::milliseconds{80};
    auto r = perform_http_connect(sink, p, "h", 443);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Timeout);
}

// ═══════════════════════════════════════════════════════════════════════
// 9. Request line format — exact CONNECT / HTTP/1.1
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, RequestLineFormat) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(200, "OK");
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    auto r = perform_http_connect(sink, p, "upstream.example.com", 9443);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);

    std::string_view s(reinterpret_cast<const char*>(sink.tx.data()),
                        sink.tx.size());
    // Exact request line (44 chars incl. the CRLF).
    constexpr std::string_view expected_req_line =
        "CONNECT upstream.example.com:9443 HTTP/1.1\r\n";
    EXPECT_EQ(s.substr(0, expected_req_line.size()), expected_req_line);
    // The body terminator must be present.
    EXPECT_NE(s.find("\r\n\r\n"), std::string_view::npos);
    // Proxy-Connection always set for CONNECT.
    EXPECT_NE(s.find("Proxy-Connection: Keep-Alive\r\n"),
              std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 10. Host header carries the target host:port
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, HostHeaderMatchesTargetAuthority) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(200, "OK");
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    auto r = perform_http_connect(sink, p, "binance.com", 443);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);

    std::string_view s(reinterpret_cast<const char*>(sink.tx.data()),
                        sink.tx.size());
    EXPECT_NE(s.find("Host: binance.com:443\r\n"), std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 11. Over-read bytes after the response are captured in leftover_out
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, OverReadBytesCapturedInLeftover) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                // Append raw extra bytes after the terminator — these
                // belong to the caller's next protocol layer.
                const std::string_view extra{"XTRAB"};
                rx_script = make_response(200, "OK", extra);
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    std::vector<uint8_t> leftover;
    auto r = perform_http_connect(sink, p, "h", 443, &leftover);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
    ASSERT_EQ(leftover.size(), 5u);
    EXPECT_EQ(std::string_view(
        reinterpret_cast<const char*>(leftover.data()), leftover.size()),
        "XTRAB");
}

// ═══════════════════════════════════════════════════════════════════════
// 12. Basic auth base64 encoding obeys RFC 7617 "user:pass" example
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, BasicAuthBase64RoundTrip) {
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) {
                rx_script = make_response(200, "OK");
            }
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    // Override default user:pass with the RFC 7617 Aladdin vector. We need
    // the leading space ("open sesame") to exercise the base64 padding.
    p.basic_auth_user = "Aladdin";
    p.basic_auth_pass = "open sesame";

    auto r = perform_http_connect(sink, p, "h", 443);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);

    std::string_view s(reinterpret_cast<const char*>(sink.tx.data()),
                        sink.tx.size());
    EXPECT_NE(s.find(
        "Proxy-Authorization: Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==\r\n"),
        std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 13. Target-host validation — empty / oversized rejected up front
//     (regression: format_host_port silently truncated to 253 bytes,
//     which would have produced a CONNECT for the wrong host)
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpConnect, EmptyTargetHostRejected) {
    FakeByteSink sink;
    auto p = make_proxy();
    auto r = perform_http_connect(sink, p, /*target_host=*/"", 443);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
    // Must not have sent anything to the proxy.
    EXPECT_TRUE(sink.tx.empty());
}

TEST(HttpConnect, OversizedTargetHostRejected) {
    FakeByteSink sink;
    auto p = make_proxy();
    // 254 bytes — one over the DNS limit; previously silently truncated.
    std::string huge(254, 'a');
    auto r = perform_http_connect(sink, p, huge, 443);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
    EXPECT_TRUE(sink.tx.empty());
}

TEST(HttpConnect, MaxLenTargetHostAccepted) {
    // 253 bytes — exactly at the DNS limit; must build and send (the proxy
    // mock will swallow it as a normal request).
    struct Swap : FakeByteSink {
        std::expected<size_t, ErrorInfo>
        send(std::span<const uint8_t> data) noexcept {
            auto r = FakeByteSink::send(data);
            if (rx_script.empty()) rx_script = make_response(200, "OK");
            return r;
        }
    };
    Swap sink;
    auto p = make_proxy();
    std::string max_host(253, 'b');
    auto r = perform_http_connect(sink, p, max_host, 443);
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
    EXPECT_FALSE(sink.tx.empty());
}

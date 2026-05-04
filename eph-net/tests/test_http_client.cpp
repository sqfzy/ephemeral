/// @file test_http_client.cpp
/// @brief End-to-end tests for `eph::net::HttpClient<S>` against an
///        in-process kernel HTTP server.
///
/// The companion file `test_http_parser.cpp` covers the underlying
/// `parse_http_request` / `parse_http_response` / `build_http_request`
/// surface (migrated baseline parser tests). This file targets the
/// `HttpClient` class itself: send / wait / parse / keep-alive /
/// timeout / hostile-server defenses.
///
/// The server is a deliberately minimal hand-rolled HTTP/1.1 implementation
/// (~80 lines) running in a worker thread on a `posix::tcp_bind_listen`-
/// returned ephemeral port. It satisfies just enough of the protocol to
/// drive each test case — handlers are pluggable via `std::function` so
/// each test can inject the wire bytes it needs (including malformed
/// responses for the negative cases).
///
/// Per the auto-mode brief: the server lives in this file (rather than a
/// separate header) so future readers can audit the contract without
/// chasing imports.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/error.hpp"
#include "eph/net/http_client.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/posix_listener.hpp"
#include "eph/net/socket_addr.hpp"

namespace en  = eph::net;
namespace enk = eph::net::kernel;
namespace ec  = eph::codec;

using PlainStream = enk::KernelTcpStream<ec::RawStreamCodec, /*EnableTls=*/false>;
using Client      = en::HttpClient<PlainStream>;

namespace {

// =============================================================================
// MiniHttpServer — minimal single-client HTTP/1.1 server for the test fixture.
//
// Design notes:
//   * Binds 127.0.0.1 + ephemeral port so tests are parallel-safe.
//   * Reads up to MAX_REQ_BYTES bytes from the client, looks for "\r\n\r\n",
//     parses Content-Length to know when the request body is done, then
//     hands the request to a `RequestHandler` callback that returns the raw
//     wire-format response bytes (or empty for "do not respond — used to
//     drive the timeout test).
//   * Honours keep-alive: stays in the read-handle-write loop until the
//     client closes or the handler returns an empty reply.
//   * No buffering tricks beyond a 64 KiB scratch — REST responses fit.
// =============================================================================
class MiniHttpServer {
public:
    /// @brief Handler signature: receives the parsed request as raw bytes
    ///        (full request including CRLF and body), returns wire-format
    ///        response bytes. An empty return means "do not respond" — the
    ///        connection will hang until the client times out.
    using RequestHandler =
        std::function<std::string(std::string_view request_bytes)>;

    explicit MiniHttpServer(RequestHandler handler) noexcept
        : handler_(std::move(handler)) {}

    ~MiniHttpServer() { stop(); }

    /// @brief Bind + listen on 127.0.0.1:<auto>, return assigned port.
    [[nodiscard]] uint16_t start() {
        auto bind_r = en::posix::tcp_bind_listen("127.0.0.1", 0, /*backlog=*/2);
        if (!bind_r) {
            ADD_FAILURE() << "tcp_bind_listen failed: " << bind_r.error();
            return 0;
        }
        listen_fd_ = *bind_r;
        sockaddr_in addr{};
        socklen_t   alen = sizeof(addr);
        ::getsockname(listen_fd_,
                      reinterpret_cast<sockaddr*>(&addr), &alen);
        port_ = ::ntohs(addr.sin_port);
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { run_(); });
        return port_;
    }

    void stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

    [[nodiscard]] uint16_t port() const noexcept { return port_; }

private:
    static constexpr std::size_t MAX_REQ_BYTES = 64 * 1024;

    void run_() {
        // Accept one client at a time (sequential server is plenty for
        // single-threaded tests).
        while (running_.load(std::memory_order_acquire)) {
            auto fd_r = en::posix::accept_one(listen_fd_, running_);
            if (!fd_r || *fd_r < 0) return;
            handle_client_(*fd_r);
            ::close(*fd_r);
        }
    }

    void handle_client_(int client_fd) {
        // Keep-alive loop: serve as many requests as the client sends until
        // it closes or the handler signals "no response".
        std::string buf;
        buf.reserve(4096);
        while (running_.load(std::memory_order_acquire)) {
            // Find the end of the request headers in `buf`.
            std::size_t req_end = std::string::npos;
            std::size_t cl      = 0;
            bool        have_cl = false;
            while (running_.load(std::memory_order_acquire)) {
                // Look for "\r\n\r\n" in current buffer.
                auto hpos = buf.find("\r\n\r\n");
                if (hpos != std::string::npos) {
                    // Inspect Content-Length (case-insensitive).
                    have_cl = parse_content_length_(
                        std::string_view{buf.data(), hpos + 2}, cl);
                    if (have_cl) {
                        if (buf.size() >= hpos + 4 + cl) {
                            req_end = hpos + 4 + cl;
                            break;
                        }
                    } else {
                        req_end = hpos + 4;
                        break;
                    }
                }
                if (buf.size() > MAX_REQ_BYTES) return; // give up, oversized
                char     chunk[2048];
                ssize_t  n = ::recv(client_fd, chunk, sizeof(chunk), 0);
                if (n <= 0) return;        // client closed
                buf.append(chunk, static_cast<std::size_t>(n));
            }

            std::string request{buf.data(), req_end};
            buf.erase(0, req_end);
            std::string response = handler_(request);
            if (response.empty()) {
                // Hang until the client drops — used by the timeout case.
                while (running_.load(std::memory_order_acquire)) {
                    char dummy[64];
                    ssize_t n = ::recv(client_fd, dummy, sizeof(dummy), 0);
                    if (n <= 0) return;
                }
                return;
            }
            std::size_t off = 0;
            while (off < response.size()) {
                ssize_t n = ::send(client_fd, response.data() + off,
                                   response.size() - off, 0);
                if (n <= 0) return;
                off += static_cast<std::size_t>(n);
            }
            // Loop back for the next request (keep-alive). If the response
            // told the client to close, the next recv will see EOF.
        }
    }

    /// @brief Extract Content-Length from a header block (case-insensitive,
    ///        first occurrence wins). Returns true and sets `out` if found.
    static bool parse_content_length_(std::string_view headers,
                                      std::size_t&     out) noexcept {
        // Walk line by line.
        std::size_t pos = 0;
        while (pos < headers.size()) {
            std::size_t eol = headers.find("\r\n", pos);
            if (eol == std::string_view::npos) return false;
            std::string_view line = headers.substr(pos, eol - pos);
            pos = eol + 2;
            // Find ':'.
            auto colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            std::string_view name  = line.substr(0, colon);
            std::string_view value = line.substr(colon + 1);
            // Trim trailing OWS from name.
            while (!name.empty() &&
                   (name.back() == ' ' || name.back() == '\t')) {
                name.remove_suffix(1);
            }
            // Case-insensitive name compare against "Content-Length".
            constexpr std::string_view kCl = "content-length";
            if (name.size() != kCl.size()) continue;
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                char c = name[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
                if (c != kCl[i]) { match = false; break; }
            }
            if (!match) continue;
            // Trim leading OWS from value.
            while (!value.empty() &&
                   (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            std::size_t v = 0;
            for (char c : value) {
                if (c == ' ' || c == '\t' || c == '\r') break;
                if (c < '0' || c > '9') return false;
                v = v * 10 + static_cast<std::size_t>(c - '0');
            }
            out = v;
            return true;
        }
        return false;
    }

    RequestHandler    handler_;
    std::atomic<bool> running_{false};
    int               listen_fd_{-1};
    uint16_t          port_{0};
    std::thread       worker_;
};

// -----------------------------------------------------------------------------
// Test fixture: spins up a poller + connected stream + HttpClient pointing at
// a MiniHttpServer.
// -----------------------------------------------------------------------------
struct ClientFixture : public ::testing::Test {
    std::unique_ptr<enk::KernelPoller> poller;
    std::unique_ptr<MiniHttpServer>    server;
    std::unique_ptr<Client>            client;

    /// @brief Build a fixture with the supplied server handler. The handler
    ///        is consulted for every request (keep-alive too).
    void setup_with_handler(MiniHttpServer::RequestHandler handler) {
        server = std::make_unique<MiniHttpServer>(std::move(handler));
        const uint16_t port = server->start();
        ASSERT_GT(port, 0u);

        auto pr = enk::KernelPoller::create();
        ASSERT_TRUE(pr.has_value()) << pr.error().detail;
        poller = std::move(*pr);

        enk::StreamConfig cfg{};
        cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
        cfg.reasm_capacity   = 64 * 1024;
        cfg.connect_timeout  = std::chrono::milliseconds{1000};

        auto sr = PlainStream::create(cfg);
        ASSERT_TRUE(sr.has_value()) << sr.error().detail;
        auto stream = std::move(*sr);

        ASSERT_TRUE(poller->add(stream.get()).has_value());
        client = std::make_unique<Client>(std::move(stream));
    }

    void TearDown() override {
        if (client && client->stream() && poller) {
            (void)poller->remove(client->stream());
        }
        client.reset();
        poller.reset();
        if (server) server->stop();
    }

    /// @brief A poll callback that drives the kernel poller for ~1ms.
    [[nodiscard]] std::function<void()> poll_fn() noexcept {
        return [this]() noexcept {
            (void)poller->poll(std::chrono::milliseconds{1});
        };
    }
};

[[nodiscard]] std::string_view header_lookup(
    std::span<const en::HttpHeader> hs, std::string_view name) noexcept {
    auto ieq = [](std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char ca = a[i]; char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
            if (ca != cb) return false;
        }
        return true;
    };
    for (const auto& h : hs) {
        if (ieq(h.name, name)) return h.value;
    }
    return {};
}

[[nodiscard]] std::span<const uint8_t> as_bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

[[nodiscard]] std::string body_str(const Client::Response& r) noexcept {
    return std::string{reinterpret_cast<const char*>(r.body.data()),
                       r.body.size()};
}

} // namespace

// =============================================================================
// 1. GetRequestRoundTrip — happy-path GET / returning 200 + body.
// =============================================================================
TEST_F(ClientFixture, GetRequestRoundTrip) {
    setup_with_handler([](std::string_view req) -> std::string {
        // Sanity-check that the client emitted a GET / line.
        EXPECT_NE(req.find("GET / HTTP/1.1\r\n"), std::string_view::npos);
        const std::string body = "hello-world";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: text/plain\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "\r\n" + body;
    });

    Client::Request req{
        .method  = "GET",
        .path    = "/",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(), std::chrono::milliseconds{500});
    ASSERT_TRUE(rsp.has_value()) << rsp.error().detail;
    EXPECT_EQ(rsp->status_code, 200);
    EXPECT_EQ(body_str(*rsp), "hello-world");
    EXPECT_EQ(header_lookup(rsp->headers, "Content-Type"), "text/plain");
}

// =============================================================================
// 2. PostRequestWithBody — POST /endpoint, server echoes body back.
// =============================================================================
TEST_F(ClientFixture, PostRequestWithBody) {
    setup_with_handler([](std::string_view req) -> std::string {
        // Locate the request body — anything after "\r\n\r\n".
        auto sep = req.find("\r\n\r\n");
        EXPECT_NE(sep, std::string_view::npos);
        std::string body{req.substr(sep + 4)};
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/octet-stream\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "\r\n" + body;
    });

    constexpr std::string_view payload =
        R"({"symbol":"BTCUSDT","side":"BUY"})";
    Client::Request req{
        .method  = "POST",
        .path    = "/endpoint",
        .headers = {
            en::HttpHeader{"Host",         "127.0.0.1"},
            en::HttpHeader{"Content-Type", "application/json"},
        },
        .body    = as_bytes(payload),
    };
    auto rsp = client->request(req, poll_fn(), std::chrono::milliseconds{500});
    ASSERT_TRUE(rsp.has_value()) << rsp.error().detail;
    EXPECT_EQ(rsp->status_code, 200);
    EXPECT_EQ(body_str(*rsp), payload);
}

// =============================================================================
// 3. MultipleRequestsOnSameStream — keep-alive, three sequential GETs.
// =============================================================================
TEST_F(ClientFixture, MultipleRequestsOnSameStream) {
    std::atomic<int> calls{0};
    setup_with_handler([&calls](std::string_view req) -> std::string {
        EXPECT_NE(req.find("GET "), std::string_view::npos);
        const int n = ++calls;
        const std::string body = "reply-" + std::to_string(n);
        return "HTTP/1.1 200 OK\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "\r\n" + body;
    });

    for (int i = 1; i <= 3; ++i) {
        Client::Request req{
            .method  = "GET",
            .path    = std::string_view{i == 1 ? "/a" : i == 2 ? "/b" : "/c"},
            .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
            .body    = {},
        };
        auto rsp = client->request(req, poll_fn(),
                                   std::chrono::milliseconds{500});
        ASSERT_TRUE(rsp.has_value()) << "iter " << i << ": " << rsp.error().detail;
        EXPECT_EQ(rsp->status_code, 200);
        EXPECT_EQ(body_str(*rsp), "reply-" + std::to_string(i));
    }
    EXPECT_EQ(calls.load(), 3);
}

// REGRESSION: HttpClient::Response::headers must outlive subsequent
// request() calls on the same client. The previous implementation kept
// the deep-copied header strings in a HttpClient-owned `headers_storage_`
// member that was cleared at the start of every request(), so any
// Response the caller still held after a second request() saw its
// header views point at destroyed std::string objects (use-after-free).
// The fix moves the owned storage INSIDE Response. This test holds the
// first response across a second request() and verifies the first
// response's headers still read correctly.
TEST_F(ClientFixture, ResponseHeadersSurviveNextRequest) {
    std::atomic<int> calls{0};
    setup_with_handler([&calls](std::string_view) -> std::string {
        const int n = ++calls;
        // Distinct header content per call so we can confirm the held
        // response sees its OWN headers, not the second request's.
        const std::string body = "b-" + std::to_string(n);
        return "HTTP/1.1 200 OK\r\n"
               "X-Call-N: " + std::to_string(n) + "\r\n"
               "Content-Type: text/plain\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n"
               "\r\n" + body;
    });

    Client::Request req{
        .method  = "GET",
        .path    = "/",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp1 = client->request(req, poll_fn(), std::chrono::milliseconds{500});
    ASSERT_TRUE(rsp1.has_value()) << rsp1.error().detail;
    // Snapshot the header values into separately-owned strings BEFORE
    // the second request runs — but ALSO keep the original string_views
    // inside rsp1.headers and assert they still point at live storage
    // after the second request finishes.
    const std::string ct_first    = std::string{header_lookup(rsp1->headers, "Content-Type")};
    const std::string n_first_hdr = std::string{header_lookup(rsp1->headers, "X-Call-N")};
    EXPECT_EQ(ct_first,    "text/plain");
    EXPECT_EQ(n_first_hdr, "1");

    // Issue a second request that forces the client to reuse its rx
    // buffer + materialize a new Response. If the old fix (client-owned
    // headers_storage_) is still in effect, this clears rsp1's backing
    // strings and the assertion below reads UAF memory — under ASan
    // this would diagnose; on release the result is silently corrupt.
    auto rsp2 = client->request(req, poll_fn(), std::chrono::milliseconds{500});
    ASSERT_TRUE(rsp2.has_value()) << rsp2.error().detail;
    EXPECT_EQ(header_lookup(rsp2->headers, "X-Call-N"), "2");

    // The held response's header views must STILL match what we observed
    // before the second request — owned storage lives inside rsp1.
    EXPECT_EQ(header_lookup(rsp1->headers, "Content-Type"), ct_first);
    EXPECT_EQ(header_lookup(rsp1->headers, "X-Call-N"),    n_first_hdr);
    EXPECT_EQ(body_str(*rsp1), "b-1");
}

// =============================================================================
// 4. TimeoutOnHangingServer — server accepts, never replies. Client times out.
// =============================================================================
TEST_F(ClientFixture, TimeoutOnHangingServer) {
    setup_with_handler([](std::string_view) -> std::string {
        // Empty reply tells MiniHttpServer to hang until the client closes.
        return "";
    });

    Client::Request req{
        .method  = "GET",
        .path    = "/never",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    const auto t0 = std::chrono::steady_clock::now();
    auto rsp = client->request(req, poll_fn(), std::chrono::milliseconds{200});
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    ASSERT_FALSE(rsp.has_value());
    EXPECT_EQ(rsp.error().code, eph::core::Error::Timeout);
    // Loose lower bound (deadline — slack); upper bound is generous to absorb
    // CI scheduler jitter.
    EXPECT_GE(elapsed, std::chrono::milliseconds{180});
    EXPECT_LE(elapsed, std::chrono::milliseconds{2000});
}

// =============================================================================
// 5. ChunkedTransferEncodingRejected — server returns chunked, parser rejects.
// =============================================================================
TEST_F(ClientFixture, ChunkedTransferEncodingRejected) {
    setup_with_handler([](std::string_view) -> std::string {
        // A chunked response. parse_http_response rejects any
        // Transfer-Encoding header (smuggling defense, phase-9 §D-1).
        return "HTTP/1.1 200 OK\r\n"
               "Transfer-Encoding: chunked\r\n"
               "\r\n"
               "5\r\nhello\r\n0\r\n\r\n";
    });

    Client::Request req{
        .method  = "GET",
        .path    = "/chunked",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(), std::chrono::milliseconds{500});
    ASSERT_FALSE(rsp.has_value());
    EXPECT_EQ(rsp.error().code, eph::core::Error::CodecBad);
}

// =============================================================================
// 6. TooLargeBodyRejected — server emits Content-Length larger than the
//    HttpClient's max_response_bytes cap. Either the parser rejects the
//    Content-Length up front (Error::CodecOverflow against the parser's
//    16 MiB hard cap) OR the accumulator hits the client cap mid-stream
//    (Error::CodecOverflow from HttpClient itself).
// =============================================================================
TEST_F(ClientFixture, TooLargeBodyRejected) {
    setup_with_handler([](std::string_view) -> std::string {
        // 200 KiB body — much larger than the 64 KiB cap we'll set below.
        const std::size_t n = 200 * 1024;
        std::string body(n, 'x');
        return "HTTP/1.1 200 OK\r\n"
               "Content-Length: " + std::to_string(n) + "\r\n"
               "\r\n" + body;
    });

    // Tear down the default fixture client and rebuild with a tighter cap.
    if (client && client->stream() && poller) {
        (void)poller->remove(client->stream());
    }
    client.reset();
    {
        enk::StreamConfig cfg{};
        cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, server->port()};
        cfg.reasm_capacity  = 64 * 1024;
        cfg.connect_timeout = std::chrono::milliseconds{1000};

        auto sr = PlainStream::create(cfg);
        ASSERT_TRUE(sr.has_value()) << sr.error().detail;
        auto stream = std::move(*sr);
        ASSERT_TRUE(poller->add(stream.get()).has_value());

        en::HttpClientConfig hcfg{};
        hcfg.max_response_bytes = 64 * 1024; // strictly smaller than 200 KiB
        client = std::make_unique<Client>(std::move(stream), hcfg);
    }

    Client::Request req{
        .method  = "GET",
        .path    = "/big",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(),
                               std::chrono::milliseconds{1500});
    ASSERT_FALSE(rsp.has_value());
    EXPECT_EQ(rsp.error().code, eph::core::Error::CodecOverflow);
}

// =============================================================================
// Pre-condition rejections — InvalidConfig / Disconnected before any I/O.
//
// These guards short-circuit `request()` before reading the wire so they
// can be exercised without a real server. The existing fixture-driven
// happy-path tests don't hit them; lock them so a refactor that tries to
// "simplify" by removing a guard surfaces immediately. Each test sets up
// just enough fixture state to reach the specific guard.
// =============================================================================

TEST_F(ClientFixture, RequestEmptyMethodRejected) {
    setup_with_handler([](std::string_view) -> std::string { return ""; });

    Client::Request req{
        .method  = "",  // ← empty
        .path    = "/",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(),
                               std::chrono::milliseconds{500});
    ASSERT_FALSE(rsp.has_value());
    EXPECT_EQ(rsp.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{rsp.error().detail}.find("method and path"),
              std::string_view::npos)
        << "detail: " << rsp.error().detail;
}

TEST_F(ClientFixture, RequestEmptyPathRejected) {
    setup_with_handler([](std::string_view) -> std::string { return ""; });

    Client::Request req{
        .method  = "GET",
        .path    = "",  // ← empty
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(),
                               std::chrono::milliseconds{500});
    ASSERT_FALSE(rsp.has_value());
    EXPECT_EQ(rsp.error().code, eph::core::Error::InvalidConfig);
}

TEST_F(ClientFixture, RequestNonPositiveTimeoutRejected) {
    setup_with_handler([](std::string_view) -> std::string { return ""; });

    Client::Request req{
        .method  = "GET",
        .path    = "/",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    // Zero
    auto rsp_zero = client->request(req, poll_fn(),
                                    std::chrono::milliseconds{0});
    ASSERT_FALSE(rsp_zero.has_value());
    EXPECT_EQ(rsp_zero.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{rsp_zero.error().detail}.find("timeout must be > 0"),
              std::string_view::npos)
        << "detail: " << rsp_zero.error().detail;

    // Negative — explicitly representable in chrono::milliseconds as a
    // signed type; the guard must catch it.
    auto rsp_neg = client->request(req, poll_fn(),
                                   std::chrono::milliseconds{-1});
    ASSERT_FALSE(rsp_neg.has_value());
    EXPECT_EQ(rsp_neg.error().code, eph::core::Error::InvalidConfig);
}

TEST_F(ClientFixture, RequestNullPollFnRejected) {
    setup_with_handler([](std::string_view) -> std::string { return ""; });

    Client::Request req{
        .method  = "GET",
        .path    = "/",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, /*poll_fn=*/{},
                               std::chrono::milliseconds{500});
    ASSERT_FALSE(rsp.has_value());
    EXPECT_EQ(rsp.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{rsp.error().detail}.find("poll_fn is null"),
              std::string_view::npos)
        << "detail: " << rsp.error().detail;
}

// =============================================================================
// Peer-side connection-close error injection
//
// These cover the "server crashed mid-response" / "ELB cut us off" hot
// failure modes that the per-field guards do NOT exercise. The existing
// TimeoutOnHangingServer covers "no bytes ever arrive"; the cases below
// cover the more pernicious "some bytes, then EOF" path that has bitten
// every HTTP client at some point in its life. The MiniHttpServer handler
// sends back a partial wire-format response and the worker thread closes
// the client fd as soon as the bytes drain.
//
// Each test pins a specific cut-point (header CRLF boundary, mid-body
// after Content-Length, headers + 1 byte) so a regression that drains
// silently on any of them surfaces as a precise diagnostic instead of
// a generic "got the wrong error class".
// =============================================================================

TEST_F(ClientFixture, ResponseClosedMidBodyReturnsConnectionError) {
    // Server promises Content-Length: 100, sends "HTTP/.../OK\r\n...\r\n\r\n"
    // + only 50 bytes of body, then hangs up. The client must surface
    // an error rather than treating the truncated buffer as a complete
    // response — the Content-Length contract makes this a hard error.
    setup_with_handler([](std::string_view) -> std::string {
        std::string r = "HTTP/1.1 200 OK\r\n"
                        "Content-Length: 100\r\n"
                        "\r\n";
        r.append(50, 'x');  // half the promised body
        return r;
    });

    Client::Request req{
        .method  = "GET",
        .path    = "/short",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(),
                               std::chrono::milliseconds{500});
    ASSERT_FALSE(rsp.has_value())
        << "client must reject Content-Length-truncated response, not accept";
    // PeerClosed (the canonical EOF-mid-stream code) or Disconnected
    // is acceptable — both surface "connection vanished" to the caller.
    EXPECT_TRUE(rsp.error().code == eph::core::Error::Disconnected
             || rsp.error().code == eph::core::Error::Timeout)
        << "got code=" << static_cast<int>(rsp.error().code)
        << " detail=" << rsp.error().detail;
}

TEST_F(ClientFixture, ResponseClosedAfterHeadersReturnsConnectionError) {
    // Headers complete (with Content-Length: 50) then immediate EOF.
    // The client cannot fabricate the missing 50 bytes from thin air,
    // so this must surface as a connection-class error.
    setup_with_handler([](std::string_view) -> std::string {
        return "HTTP/1.1 200 OK\r\n"
               "Content-Length: 50\r\n"
               "\r\n";  // and nothing else
    });

    Client::Request req{
        .method  = "GET",
        .path    = "/empty-after-headers",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(),
                               std::chrono::milliseconds{500});
    ASSERT_FALSE(rsp.has_value());
    EXPECT_TRUE(rsp.error().code == eph::core::Error::Disconnected
             || rsp.error().code == eph::core::Error::Timeout)
        << "got code=" << static_cast<int>(rsp.error().code)
        << " detail=" << rsp.error().detail;
}

TEST_F(ClientFixture, ResponseClosedMidHeadersReturnsConnectionError) {
    // Server sends only the status line + first header — no terminating
    // CRLFCRLF, no Content-Length yet — then hangs up. Classic early-
    // truncation case (proxy died, K8s SIGTERM, etc).
    setup_with_handler([](std::string_view) -> std::string {
        return "HTTP/1.1 200 OK\r\n"
               "Server: died-here";  // no CRLF after this header
    });

    Client::Request req{
        .method  = "GET",
        .path    = "/mid-headers",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(),
                               std::chrono::milliseconds{500});
    ASSERT_FALSE(rsp.has_value());
    EXPECT_TRUE(rsp.error().code == eph::core::Error::Disconnected
             || rsp.error().code == eph::core::Error::Timeout)
        << "got code=" << static_cast<int>(rsp.error().code)
        << " detail=" << rsp.error().detail;
}

TEST_F(ClientFixture, ResponseGarbageStatusLineIsCodecBad) {
    // Wire bytes that are not a valid HTTP/1.1 status line at all.
    // parse_http_response must reject as CodecBad — silently treating
    // garbage as a response would let an attacker MITM inject arbitrary
    // body claims via a downstream proxy that mangled the upstream
    // bytes. (The chunked case above is similar but more specific.)
    setup_with_handler([](std::string_view) -> std::string {
        return "<<<HELLO this is not HTTP>>>\r\n"
               "more garbage\r\n\r\n";
    });

    Client::Request req{
        .method  = "GET",
        .path    = "/garbage",
        .headers = { en::HttpHeader{"Host", "127.0.0.1"} },
        .body    = {},
    };
    auto rsp = client->request(req, poll_fn(),
                               std::chrono::milliseconds{500});
    ASSERT_FALSE(rsp.has_value());
    EXPECT_EQ(rsp.error().code, eph::core::Error::CodecBad)
        << "garbage status line must surface as CodecBad, got code="
        << static_cast<int>(rsp.error().code)
        << " detail=" << rsp.error().detail;
}

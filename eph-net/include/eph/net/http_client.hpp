#pragma once

/// @file http_client.hpp
/// @brief Backend-agnostic HTTP/1.1 request/response client.
///
/// `HttpClient<S>` is a thin façade around any type satisfying the
/// `eph::net::Stream` concept: it owns the stream, builds wire-format
/// requests via `build_http_request`, and incrementally parses replies via
/// `parse_http_response`. The actual byte I/O is delegated to the underlying
/// `Stream` (kernel epoll, DPDK lcore, or in-memory FakeStream); the
/// `HttpClient` just orchestrates send → poll → parse.
///
/// ## Scope (per `.artifacts/phase-9-scope-decision.md` §D-1)
///
/// SUPPORTED:
///   * HTTP/1.1 only
///   * `Content-Length`-bounded request and response bodies
///   * Keep-alive — multiple `request()` calls can reuse a single stream
///   * Caller-supplied headers (Authorization, Content-Type, X-API-KEY, …)
///   * Caller-supplied timeout per request
///
/// EXPLICITLY REJECTED (returns `Error::CodecBad` / `Error::CodecOverflow`):
///   * `Transfer-Encoding: chunked` — the parser already rejects, so any
///     server response carrying this surfaces as `CodecBad` from the parser
///   * Redirects (3xx) — returned to the caller as-is, no auto-follow
///   * `Expect: 100-continue` — not emitted
///   * Cookies — not stored or replayed
///   * Connection pool — one stream per `HttpClient`; for parallelism, use
///     N HttpClient instances over N streams
///
/// ## Lifetime
///
/// `HttpClient` owns its `Stream` via `unique_ptr`. The caller is responsible
/// for attaching the stream to a poller before the first `request()`, and for
/// passing a `poll_fn` callback that drives that poller. The `HttpClient` is
/// not a poller — it is a request/response sequencer that uses the caller's
/// poll loop to make progress.
///
/// ## Logger
///
/// All non-trivial paths log via the `"net.http_client"` named spdlog logger
/// (created lazily on first use).
///
/// ## Thread safety
///
/// `request()` is not re-entrant — issuing two concurrent calls on the same
/// `HttpClient` instance is undefined. Sequential calls are fine.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "eph/core/log.hpp"

#include "eph/core/error.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/http.hpp"
#include "eph/net/tcp_state.hpp"

namespace eph::net {

namespace detail {

/// @brief Lazy logger accessor. Uses spdlog's default sink the first time
///        the named logger is requested so callers do not need to register
///        a sink before issuing requests.
[[nodiscard]] inline spdlog::logger* http_client_logger() noexcept {
    static spdlog::logger* l = ::eph::log::get("net.http_client");
    return l;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// HttpClientConfig
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Per-`HttpClient` knobs. Defaults are tuned for HFT REST APIs
///        (Binance / OKX / Coinbase) where typical responses are < 64 KiB.
struct HttpClientConfig {
    /// @brief Initial reservation for the receive accumulator. Grows up to
    ///        `max_response_bytes` on demand. 64 KiB matches the kernel
    ///        TcpStream's `reasm_capacity` default and avoids a reallocation
    ///        for the common case.
    std::size_t initial_rx_capacity{64 * 1024};

    /// @brief Hard cap on accumulated response bytes (status line + headers
    ///        + body). Exceeding this returns `Error::CodecOverflow`. 1 MiB
    ///        is large enough for any realistic exchange REST response and
    ///        small enough to bound the cost of a misbehaving peer.
    std::size_t max_response_bytes{1024 * 1024};

    /// @brief Inline buffer used for the outgoing request bytes. Sized for
    ///        a reasonable max (status line + ~32 headers + a small body).
    ///        Requests exceeding this fail with `Error::BufferFull`. For
    ///        large POST bodies, raise this knob.
    std::size_t request_buf_capacity{16 * 1024};

    /// @brief Maximum HttpHeader records the parser may store for one
    ///        response. Bounded by `kMaxHeaderCount` regardless.
    std::size_t max_response_headers{32};

    /// @brief Add `Connection: keep-alive` automatically when the caller
    ///        does not supply a Connection header. Set to false to opt out
    ///        (e.g. for one-shot probes where the caller wants the server
    ///        to close after the reply).
    bool send_connection_keep_alive{true};
};

// ─────────────────────────────────────────────────────────────────────────────
// HttpClient
// ─────────────────────────────────────────────────────────────────────────────

/// @brief HTTP/1.1 request/response client templated on a `Stream` type.
///
/// `S` must satisfy the `eph::net::Stream` concept (kernel TcpStream, DPDK
/// TcpStream, FakeStream, etc.). The codec underlying `S` should be
/// `RawStreamCodec` (or any codec that delivers received bytes verbatim) —
/// the HttpClient does its own framing via `parse_http_response`.
///
/// Typical usage:
///
/// @code
///   auto poller = KernelPoller::create().value();
///   StreamConfig cfg{ .remote = ..., .reasm_capacity = 64*1024 };
///   auto stream = KernelTcpStream<RawStreamCodec, false>::create(cfg).value();
///   poller->add(stream.get()).value();
///
///   HttpClient<KernelTcpStream<RawStreamCodec, false>> cli{std::move(stream)};
///
///   HttpClient::Request req{
///       .method = "GET",
///       .path   = "/api/v3/ping",
///       .headers = { {"Host", "api.binance.com"} },
///       .body   = {}
///   };
///   auto rsp = cli.request(req,
///                          [&] noexcept { (void)poller->poll(); },
///                          std::chrono::milliseconds{500});
/// @endcode
template <Stream S>
class HttpClient {
public:
    /// @brief Application-layer request descriptor.
    ///
    /// `method` and `path` are views into caller storage and must outlive
    /// `request()`. `headers` is owned (vector) for ergonomic construction.
    struct Request {
        std::string_view              method;   ///< "GET", "POST", "DELETE", …
        std::string_view              path;     ///< "/api/v3/order?…"
        std::vector<HttpHeader>       headers;  ///< user-supplied headers
        std::span<const uint8_t>      body;     ///< empty for GET-style
    };

    /// @brief Application-layer response. All fields are owned by the
    ///        Response itself — the response outlives the next `request()`
    ///        call independently of the HttpClient.
    ///
    /// `headers[i]` are zero-copy views into `headers_storage` which is
    /// owned by THIS Response (one std::string per header name + one per
    /// header value, in the order they appeared on the wire). The struct
    /// is non-copyable because copying would clone `headers_storage` to
    /// fresh std::string objects with different addresses while leaving
    /// the views pointing at the originals — copy-after-destroy of the
    /// source would yield dangling views (use-after-free). Move is
    /// O(1) ptr-swap on the underlying vectors so individual std::string
    /// objects keep their addresses across moves.
    struct Response {
        int                           status_code{0};
        /// Names/values are views into `headers_storage`.
        std::vector<HttpHeader>       headers;
        std::vector<uint8_t>          body;
        /// Owned backing storage for `headers`. Two entries per header
        /// (name, value), in arrival order. Treat as opaque from the
        /// caller side — read through `headers`.
        std::vector<std::string>      headers_storage;

        Response() = default;
        Response(const Response&)            = delete;
        Response& operator=(const Response&) = delete;
        Response(Response&&)            noexcept = default;
        Response& operator=(Response&&) noexcept = default;
    };

    /// @brief Construct around an attached, ready-to-use stream.
    ///
    /// Pre-condition: `stream` should already be attached to a Poller and
    /// in `TcpState::Established`. The HttpClient does not perform connect
    /// itself — that is the responsibility of the stream factory.
    explicit HttpClient(std::unique_ptr<S> stream,
                        HttpClientConfig   cfg = {}) noexcept
        : stream_(std::move(stream)),
          cfg_(cfg) {
        rx_buffer_.reserve(cfg_.initial_rx_capacity);
        request_buffer_.resize(cfg_.request_buf_capacity);
        EPH_LOG_DEBUG(detail::http_client_logger(),
            "HttpClient: constructed, max_response_bytes={}, "
            "request_buf_capacity={}",
            cfg_.max_response_bytes, cfg_.request_buf_capacity);
    }

    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&)                 = default;
    HttpClient& operator=(HttpClient&&)      = default;

    /// @brief Issue one HTTP request and block (driving `poll_fn`) until the
    ///        response is fully received, the deadline elapses, or an error
    ///        occurs.
    ///
    /// Error codes:
    ///   * `Error::Timeout`         — `timeout` elapsed before complete
    ///   * `Error::CodecBad`        — server emitted unsupported framing
    ///                                (Transfer-Encoding, malformed status,
    ///                                bare LF, etc.) per phase-9 §D-1
    ///   * `Error::CodecOverflow`   — accumulated bytes > max_response_bytes
    ///   * `Error::BufferFull`      — outgoing request didn't fit in
    ///                                `request_buf_capacity`
    ///   * `Error::Disconnected`    — peer closed before/during exchange
    ///   * `Error::NotAttached`     — stream is not attached to a Poller
    ///   * `Error::InvalidConfig`   — malformed Request (empty method/path,
    ///                                etc.)
    [[nodiscard]] std::expected<Response, core::ErrorInfo>
    request(const Request&            req,
            std::function<void()>     poll_fn,
            std::chrono::milliseconds timeout) noexcept {
        [[maybe_unused]] auto* log = detail::http_client_logger();

        // ── Pre-conditions ───────────────────────────────────────────────
        if (!stream_) {
            EPH_LOG_ERROR(log, "HttpClient::request: stream is null");
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "HttpClient::request: stream is null"});
        }
        if (!poll_fn) {
            EPH_LOG_ERROR(log, "HttpClient::request: poll_fn is null");
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "HttpClient::request: poll_fn is null"});
        }
        if (req.method.empty() || req.path.empty()) {
            EPH_LOG_WARN(log,
                "HttpClient::request: empty method or path");
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "HttpClient::request: method and path must be non-empty"});
        }
        if (timeout <= std::chrono::milliseconds::zero()) {
            EPH_LOG_WARN(log,
                "HttpClient::request: non-positive timeout {}ms",
                timeout.count());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "HttpClient::request: timeout must be > 0"});
        }
        if (stream_->state() != TcpState::Established) {
            EPH_LOG_WARN(log,
                "HttpClient::request: stream not Established (state={})",
                static_cast<int>(stream_->state()));
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "HttpClient::request: stream not in Established state"});
        }

        EPH_LOG_DEBUG(log,
            "HttpClient::request: {} {} body_bytes={} timeout={}ms",
            std::string(req.method), std::string(req.path),
            req.body.size(), timeout.count());

        // ── Build effective headers (caller's + auto Connection / CL) ────
        // We store on-stack copies for ephemeral header values (Content-Length,
        // Connection) so the views remain valid until build_http_request
        // returns.
        std::string content_length_value;
        std::vector<HttpHeader> effective_headers;
        effective_headers.reserve(req.headers.size() + 2);

        // Caller-supplied headers come first so user can override defaults.
        bool caller_set_connection     = false;
        bool caller_set_content_length = false;
        for (const auto& h : req.headers) {
            effective_headers.push_back(h);
            if (iequal_local(h.name, "Connection"))     caller_set_connection = true;
            if (iequal_local(h.name, "Content-Length")) caller_set_content_length = true;
        }
        // Auto-add Content-Length for non-empty bodies if the caller didn't.
        if (!req.body.empty() && !caller_set_content_length) {
            content_length_value = std::to_string(req.body.size());
            effective_headers.push_back(
                HttpHeader{"Content-Length", content_length_value});
        }
        // Auto-add Connection: keep-alive (default on) so the server keeps the
        // socket open for the next request().
        if (cfg_.send_connection_keep_alive && !caller_set_connection) {
            effective_headers.push_back(
                HttpHeader{"Connection", "keep-alive"});
        }

        // ── Serialize the request ────────────────────────────────────────
        auto built = build_http_request(
            request_buffer_.data(), request_buffer_.size(),
            req.method, req.path,
            std::span<const HttpHeader>(effective_headers.data(),
                                        effective_headers.size()),
            req.body);
        if (!built) {
            EPH_LOG_WARN(log,
                "HttpClient::request: build_http_request failed: {}",
                built.error().detail);
            return std::unexpected(built.error());
        }
        EPH_LOG_DEBUG(log,
            "HttpClient::request: serialized {} bytes", *built);

        // ── Send (one blocking-ish send, since the kernel buffers anyway) ─
        std::span<const uint8_t> tx_view{request_buffer_.data(), *built};
        std::size_t              total_sent = 0;
        // Drive the poll loop while sending in case the kernel write buffer
        // is full and we get a partial send. Small REST requests almost
        // always fit in one syscall, so this is the cold path.
        //
        // We use steady_clock for deadline math because (a) the request loop
        // is *not* a hot path (one HTTP exchange spans milliseconds-to-seconds
        // of wall time, dwarfing the TSC vs steady_clock differential), and
        // (b) it side-steps requiring callers to TSC::init() before
        // constructing the client. Hot-path latency is in the underlying
        // Stream::send/recv path, where TSC is already used.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (total_sent < tx_view.size()) {
            auto sr = stream_->send(tx_view.subspan(total_sent));
            if (!sr) {
                if (sr.error().code == core::Error::WouldBlock) {
                    poll_fn();
                    if (std::chrono::steady_clock::now() >= deadline) {
                        EPH_LOG_WARN(log,
                            "HttpClient::request: send timed out after "
                            "{} of {} bytes",
                            total_sent, tx_view.size());
                        return std::unexpected(core::ErrorInfo{
                            core::Error::Timeout,
                            "HttpClient::request: send timed out"});
                    }
                    continue;
                }
                EPH_LOG_WARN(log,
                    "HttpClient::request: send failed at {}/{}: {}",
                    total_sent, tx_view.size(), sr.error().detail);
                return std::unexpected(sr.error());
            }
            if (*sr == 0) {
                // 0 bytes from a successful send is a peer-disconnect signal.
                EPH_LOG_WARN(log,
                    "HttpClient::request: send returned 0 (peer closed?)");
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "HttpClient::request: send returned 0 bytes"});
            }
            total_sent += *sr;
        }

        // ── Receive: install a hijack on_message that appends bytes to our
        //    rx_buffer_, then drive the poller until parse_http_response
        //    returns a complete response (or we time out / overflow).
        rx_buffer_.clear();
        auto saved_on_message = std::move(stream_->on_message);
        stream_->on_message = [this](std::span<const uint8_t> bytes) noexcept {
            // Defensive cap — the main parse loop also checks overflow once
            // the response is incomplete, but a runaway server delivering
            // a single multi-MB chunk on one on_message firing would
            // otherwise grow `rx_buffer_` past `max_response_bytes` before
            // the parse loop runs. Append at most enough bytes to put us
            // ONE byte above the cap; the parse loop then trips its
            // overflow branch on the next iteration. Excess bytes are
            // dropped on the floor — the connection is about to be torn
            // down anyway, so preserving the tail buys nothing.
            const std::size_t cap   = cfg_.max_response_bytes;
            const std::size_t have  = rx_buffer_.size();
            if (have > cap) {
                // Already over — discard further bytes; parse loop will
                // surface the overflow on its next turn.
                return;
            }
            // Allow overshoot by exactly one byte so the strict `>` check
            // in the parse loop fires.
            const std::size_t budget = (cap - have) + 1;
            const std::size_t take   = std::min(bytes.size(), budget);
            rx_buffer_.insert(rx_buffer_.end(),
                              bytes.begin(),
                              bytes.begin() + static_cast<std::ptrdiff_t>(take));
        };

        // Restore on_message on every exit path (success or error).
        struct OnMessageRestorer {
            S*                        s;
            typename S::OnMessage     saved;
            ~OnMessageRestorer() noexcept { if (s) s->on_message = std::move(saved); }
        } restorer{stream_.get(), std::move(saved_on_message)};

        // Reserve parser header storage. parse_http_response cannot use more
        // than `kMaxHeaderCount` regardless of caller storage, but we cap
        // further by the user's `max_response_headers`.
        std::vector<HttpHeader> parsed_storage(
            std::max<std::size_t>(cfg_.max_response_headers, 1));

        while (true) {
            // Try to parse the current accumulator.
            std::span<const uint8_t> view{rx_buffer_.data(), rx_buffer_.size()};
            auto pr = parse_http_response(view,
                std::span<HttpHeader>(parsed_storage.data(),
                                      parsed_storage.size()));
            if (!pr) {
                EPH_LOG_WARN(log,
                    "HttpClient::request: parse failed: {} (rx_bytes={})",
                    pr.error().detail, rx_buffer_.size());
                return std::unexpected(pr.error());
            }
            if (pr->has_value()) {
                // Got a complete response.
                Response out;
                out.status_code = static_cast<int>((*pr)->value.status_code);
                // Materialize headers into owned vector. Header views still
                // alias rx_buffer_ — we keep rx_buffer_ alive for the
                // lifetime of `out` by copying the body separately, but the
                // header values point into rx_buffer_'s storage. To make the
                // returned Response truly self-contained we deep-copy via a
                // dedicated headers_storage_ buffer.
                materialize_response_(out, (*pr)->value);
                EPH_LOG_DEBUG(log,
                    "HttpClient::request: completed status={} body_bytes={}",
                    out.status_code, out.body.size());
                // Drain consumed bytes from rx_buffer_ so the next request()
                // starts clean (in keep-alive case the parser has already
                // sliced off only the response — any pipeline/extra bytes
                // remain queued for the next call).
                std::size_t consumed = (*pr)->consumed;
                if (consumed >= rx_buffer_.size()) {
                    rx_buffer_.clear();
                } else {
                    rx_buffer_.erase(rx_buffer_.begin(),
                                     rx_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
                }
                return out;
            }
            // Not yet complete; check overflow.
            if (rx_buffer_.size() > cfg_.max_response_bytes) {
                EPH_LOG_WARN(log,
                    "HttpClient::request: response exceeds max ({} > {})",
                    rx_buffer_.size(), cfg_.max_response_bytes);
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecOverflow,
                    "HttpClient::request: response exceeds max_response_bytes"});
            }
            // Check stream liveness — peer may have FIN'd.
            if (stream_->state() != TcpState::Established) {
                EPH_LOG_WARN(log,
                    "HttpClient::request: stream left Established mid-recv "
                    "(state={}, rx_bytes={})",
                    static_cast<int>(stream_->state()),
                    rx_buffer_.size());
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "HttpClient::request: peer closed before response complete"});
            }
            // Check timeout.
            if (std::chrono::steady_clock::now() >= deadline) {
                EPH_LOG_WARN(log,
                    "HttpClient::request: timed out (rx_bytes={})",
                    rx_buffer_.size());
                return std::unexpected(core::ErrorInfo{
                    core::Error::Timeout,
                    "HttpClient::request: response timeout"});
            }
            // Pump the poller.
            poll_fn();
        }
    }

    /// @brief Access the underlying stream for direct configuration
    ///        (e.g. setting socket options before the first request).
    [[nodiscard]] S* stream() noexcept { return stream_.get(); }

    /// @brief Const-overload for read-only inspection.
    [[nodiscard]] const S* stream() const noexcept { return stream_.get(); }

private:
    std::unique_ptr<S>       stream_;
    HttpClientConfig         cfg_;
    std::vector<uint8_t>     rx_buffer_;        ///< accumulator for incoming bytes
    std::vector<uint8_t>     request_buffer_;   ///< scratch for build_http_request
    // Note: response header storage now lives inside the returned Response
    // itself (Response::headers_storage), so each Response owns its own
    // backing strings independently of the client. See materialize_response_.

    /// @brief Local case-insensitive ASCII compare (avoids dragging in
    ///        a public utility just for the keep-alive defaulting).
    [[nodiscard]] static constexpr bool
    iequal_local(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char ca = a[i];
            char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
            if (ca != cb) return false;
        }
        return true;
    }

    /// @brief Deep-copy parsed headers + body into the owned Response so it
    ///        outlives subsequent rx_buffer_ mutations *and* subsequent
    ///        request() calls on the same client.
    ///
    /// The parser returns `string_view` headers aliasing `rx_buffer_`; if we
    /// returned those directly the caller would see dangling views as soon
    /// as the next request() trims rx_buffer_. The previous implementation
    /// staged the deep copies into a HttpClient-owned `headers_storage_`
    /// member, but that introduced a different UAF: the next request()
    /// call cleared the same storage, invalidating string_views in any
    /// previously-returned Response the caller still held. The fix is to
    /// place the owned storage INSIDE `Response`, so each Response owns
    /// its own header backing buffer independently of the client.
    void materialize_response_(Response&           out,
                               const HttpResponse& parsed) noexcept {
        // Stable strings backing each header's name + value live on `out`.
        out.headers_storage.clear();
        out.headers_storage.reserve(parsed.headers.size() * 2);
        out.headers.clear();
        out.headers.reserve(parsed.headers.size());
        for (const auto& h : parsed.headers) {
            out.headers_storage.emplace_back(h.name);
            std::string& nm = out.headers_storage.back();
            out.headers_storage.emplace_back(h.value);
            std::string& vl = out.headers_storage.back();
            out.headers.push_back(
                HttpHeader{std::string_view{nm}, std::string_view{vl}});
        }
        // Body deep-copy (the parser body is a span into rx_buffer_).
        out.body.assign(parsed.body.begin(), parsed.body.end());
    }
};

} // namespace eph::net

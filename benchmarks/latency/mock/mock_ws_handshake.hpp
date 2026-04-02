/// @file mock_ws_handshake.hpp
/// Server-side WebSocket HTTP Upgrade handshake handler for mock benchmarks.
///
/// Reads the client's HTTP Upgrade request, computes Sec-WebSocket-Accept
/// per RFC 6455, and sends the 101 Switching Protocols response.
/// Uses OpenSSL (aws-lc) for SHA1 and Base64.

#pragma once

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <openssl/evp.h>

#include <spdlog/spdlog.h>

namespace bench::mock {

namespace detail {

/// RFC 6455 magic GUID appended to Sec-WebSocket-Key before hashing.
inline constexpr std::string_view kWebSocketGUID =
    "258EAFA5-E914-47DA-95CA-5AB5DC4AB2C8";

/// Compute Sec-WebSocket-Accept value from the client's Sec-WebSocket-Key.
/// Returns the base64-encoded SHA-1 hash of (key + GUID), or empty on failure.
inline std::string compute_accept_key(std::string_view client_key) {
    // Concatenate client key with magic GUID
    std::string input;
    input.reserve(client_key.size() + kWebSocketGUID.size());
    input.append(client_key);
    input.append(kWebSocketGUID);

    // SHA-1 digest
    unsigned char sha1_hash[EVP_MAX_MD_SIZE];
    unsigned int sha1_len = 0;

    if (EVP_Digest(input.data(), input.size(), sha1_hash, &sha1_len,
                   EVP_sha1(), nullptr) != 1) {
        spdlog::error("mock_ws_handshake: EVP_Digest (SHA1) failed");
        return {};
    }

    // Base64 encode — EVP_EncodeBlock writes a null-terminated string
    // Output size: 4 * ceil(sha1_len / 3) + 1
    char b64_buf[64];
    int b64_len = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(b64_buf), sha1_hash, sha1_len);
    if (b64_len <= 0) {
        spdlog::error("mock_ws_handshake: EVP_EncodeBlock failed");
        return {};
    }

    return std::string(b64_buf, static_cast<size_t>(b64_len));
}

/// Extract the value of a named HTTP header from raw headers.
/// Searches for "header_name: " (case-sensitive) and returns the value
/// up to the next \r\n. Returns empty string_view if not found.
inline std::string_view extract_header(std::string_view headers,
                                       std::string_view header_name) {
    // Build search pattern: "Header-Name: "
    std::string needle;
    needle.reserve(header_name.size() + 2);
    needle.append(header_name);
    needle.append(": ");

    auto pos = headers.find(needle);
    if (pos == std::string_view::npos) return {};

    auto value_start = pos + needle.size();
    auto eol = headers.find("\r\n", value_start);
    if (eol == std::string_view::npos) eol = headers.size();

    return headers.substr(value_start, eol - value_start);
}

} // namespace detail

/// Handle WebSocket HTTP Upgrade on a connected socket fd.
///
/// Reads the client's GET /ws... request, computes Sec-WebSocket-Accept,
/// sends 101 Switching Protocols response.
///
/// @param client_fd  Connected TCP socket file descriptor.
/// @return true on success, false on failure (timeout, bad request, send error).
///
/// Timeout: 3 seconds for the entire handshake.
inline bool handle_ws_upgrade(int client_fd) {
    constexpr int kHandshakeTimeoutMs = 3000;
    constexpr size_t kMaxHeaderSize = 4096;

    spdlog::debug("mock_ws_handshake: waiting for HTTP Upgrade on fd={}", client_fd);

    // Read HTTP request until we see the end-of-headers marker "\r\n\r\n"
    char buf[kMaxHeaderSize];
    size_t total_read = 0;

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kHandshakeTimeoutMs);

    while (total_read < kMaxHeaderSize) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            spdlog::error("mock_ws_handshake: timeout waiting for HTTP headers");
            return false;
        }

        struct pollfd pfd{};
        pfd.fd = client_fd;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (ret < 0) {
            if (errno == EINTR) continue;
            spdlog::error("mock_ws_handshake: poll() failed: {}", std::strerror(errno));
            return false;
        }
        if (ret == 0) {
            spdlog::error("mock_ws_handshake: poll() timeout");
            return false;
        }

        ssize_t n = ::recv(client_fd, buf + total_read,
                           kMaxHeaderSize - total_read, 0);
        if (n <= 0) {
            spdlog::error("mock_ws_handshake: recv() returned {} (errno={})",
                          n, std::strerror(errno));
            return false;
        }
        total_read += static_cast<size_t>(n);

        // Check if we have the complete header block
        std::string_view headers(buf, total_read);
        if (headers.find("\r\n\r\n") != std::string_view::npos) {
            break;
        }
    }

    std::string_view headers(buf, total_read);
    spdlog::debug("mock_ws_handshake: received {} bytes of HTTP headers", total_read);

    // Extract Sec-WebSocket-Key
    auto ws_key = detail::extract_header(headers, "Sec-WebSocket-Key");
    if (ws_key.empty()) {
        spdlog::error("mock_ws_handshake: missing Sec-WebSocket-Key header");
        return false;
    }
    spdlog::debug("mock_ws_handshake: client key=\"{}\"", ws_key);

    // Compute accept key
    std::string accept_key = detail::compute_accept_key(ws_key);
    if (accept_key.empty()) {
        spdlog::error("mock_ws_handshake: failed to compute accept key");
        return false;
    }

    // Build and send 101 response
    std::string response;
    response.reserve(256);
    response.append("HTTP/1.1 101 Switching Protocols\r\n");
    response.append("Upgrade: websocket\r\n");
    response.append("Connection: Upgrade\r\n");
    response.append("Sec-WebSocket-Accept: ");
    response.append(accept_key);
    response.append("\r\n\r\n");

    size_t sent = 0;
    while (sent < response.size()) {
        ssize_t n = ::send(client_fd, response.data() + sent,
                           response.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            spdlog::error("mock_ws_handshake: send() failed: {}", std::strerror(errno));
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    spdlog::debug("mock_ws_handshake: 101 Switching Protocols sent, "
                  "accept_key=\"{}\"", accept_key);
    return true;
}

} // namespace bench::mock

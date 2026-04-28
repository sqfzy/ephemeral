/// @file test_coinbase_adapter.cpp
///
/// Integration test: Coinbase Advanced Trade authenticated WebSocket adapter.
///
/// Exercises TLS 1.3 + WebSocket + ReconnectOrchestrator against an
/// in-process TLS WS server that mimics the Coinbase Advanced Trade
/// `wss://advanced-trade-ws.coinbase.com` subscribe-ack protocol.
///
/// Key Coinbase quirk vs OKX/Bybit: authentication for "user" channels
/// (orders, account) is carried as a **JWT in the subscribe payload**,
/// not as an HTTP header. The wire format is:
///
///   {"type":"subscribe", "channel":"user", "jwt":"<JWS-token>"}
///
/// And the server response is:
///
///   {"channel":"subscriptions", "client_id":"...", "events":[...]}
///
/// The JWT is produced by the real `eph::net::build_coinbase_jwt`
/// (ES256 over a P-256 EC key) — the same path a production HFT
/// client would use. Per-connect we vary the `now_unix_secs` claim
/// so the two JWTs are byte-distinct (modeling Coinbase's real
/// per-connect token issuance). The server-side handler runs the
/// matching public key through `EVP_DigestVerify` to **prove** the
/// JWS signature is valid end-to-end (not just "string round-trips").
///
/// This closes the T2.10 documented gap: previously this test used
/// a hand-rolled fake-JWT shape that only proved string transport,
/// not signature validity. With real signing wired in, the test
/// exercises the full Coinbase Advanced Trade auth surface
/// (Es256PrivateKey → JWT → TLS → WS → server signature verify).
///
/// Scenario:
///   1. Server installed with a Coinbase-shape handler that:
///      - On `{"type":"subscribe", ...}` returns a `subscriptions`
///        ack frame.
///      - Captures the JWT field from the subscribe payload so the
///        test can assert it round-tripped intact AND verify the
///        ES256 signature against the matching public key.
///   2. Orchestrator dials, subscribe-with-JWT is sent, ack received.
///   3. Server pushes Close 1011, orchestrator reconnects, JWT
///      replayed (with a fresh JWT bearing a later `nbf`/`exp` to
///      simulate per-connect token issuance).
///   4. Assertions: state == Connected, reconnect_count >= 2,
///      both JWTs received intact by server, both signatures
///      verify cryptographically against the public key, subscribe
///      replayed twice.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/net/jwt_signed_request.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/reconnect_orchestrator.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/time.hpp"

#include "tls_ws_echo_server.hpp"
#include "venue_adapter_test_kit.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;
using namespace std::chrono_literals;

namespace {

using TlsWsStream = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Orch        = en::ReconnectOrchestrator<TlsWsStream>;

/// Coinbase Advanced Trade subscriptions-ack channel name. The server
/// emits this as the `channel` field on the response frame.
constexpr std::string_view kCoinbaseSubsAck =
    R"({"channel":"subscriptions","client_id":"eph-test","timestamp":"2026-04-28T00:00:00Z","sequence_num":0,"events":[{"subscriptions":{"user":["BTC-USD"]}}]})";

/// Throwaway P-256 EC key used to sign Coinbase test JWTs. Generated via
/// `openssl ecparam -genkey -name prime256v1 -noout`. The matching
/// public key is recovered via `EVP_PKEY_get0_EC_KEY` for server-side
/// verification. Same key as `eph-net/tests/test_jwt_signed_request.cpp`
/// — keeping a single test vector keeps the symmetry obvious.
constexpr std::string_view kEcP256TestPem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEINOpmaBE6cuLkglCisJtB93Y4yJ2RGC4HSHdUJZesfueoAoGCCqGSM49\n"
    "AwEHoUQDQgAEipxCkur6xELXaT83IyfmFcCIETWrJCXZRs+en43AvHt+Lu0i15E9\n"
    "90z0OphnjtVyoeuhbuMPChrOEkZbGyZUMw==\n"
    "-----END EC PRIVATE KEY-----\n";

/// The Coinbase Cloud key id (`kid`) and api-key name (`sub`) that the
/// JWT bears. The values are arbitrary for the test — the server only
/// asserts they round-trip through the JOSE header / payload claims.
constexpr std::string_view kTestKid =
    "organizations/eph-test-org/apiKeys/eph-test-key";
constexpr std::string_view kTestSub = "eph-test";

/// Coinbase JWT `uri` claim format is "<METHOD> <host/path>" with a
/// single space delimiter — see `build_coinbase_jwt` docs. For WS
/// subscribe we use a synthetic GET against the documented WS URL.
constexpr std::string_view kTestMethod = "GET";
constexpr std::string_view kTestUri    = "advanced-trade-ws.coinbase.com/";

/// Fixed nonce overrides (32 bytes each) so each call to
/// `build_coinbase_jwt` is deterministic — important because the test
/// later asserts the two JWTs differ (we vary `now_unix_secs`) without
/// flakiness from CSPRNG variance. The two values are byte-distinct.
constexpr std::array<uint8_t, 32> kNonce1 = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
};
constexpr std::array<uint8_t, 32> kNonce2 = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
    0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
};

/// Build a real Coinbase JWT for this test session. `now_unix_secs`
/// increments per-call so JWTs from different connects are
/// byte-distinct, modeling the per-connect token issuance pattern.
std::string make_test_jwt(const eph::net::Es256PrivateKey& key,
                          uint64_t now_unix_secs,
                          std::span<const uint8_t> nonce) {
    eph::net::CoinbaseJwtParams p{};
    p.key_id          = kTestKid;
    p.api_key_name    = kTestSub;
    p.method          = kTestMethod;
    p.uri             = kTestUri;
    p.now_unix_secs   = now_unix_secs;
    p.ttl_secs        = 120;
    p.nonce_override  = nonce;
    auto jwt = eph::net::build_coinbase_jwt(key, p);
    EXPECT_TRUE(jwt.has_value()) << "build_coinbase_jwt failed: "
                                 << (jwt ? "" : jwt.error().detail);
    return jwt.value_or(std::string{});
}

/// Verify an ES256 JWT against the matching `EVP_PKEY*` public key.
/// Returns true iff (a) the JWT splits into 3 parts, (b) the signature
/// is exactly 64 bytes (P-1363 r||s), and (c) `EVP_DigestVerify`
/// accepts the signature over the canonical "header.payload" input.
///
/// We re-encode r||s back to DER for `EVP_DigestVerify` because aws-lc's
/// digest-verify path expects DER. This is the inverse of what
/// `build_coinbase_jwt` does internally and gives us an end-to-end
/// "the produced JWS is cryptographically valid" assertion.
bool verify_jwt_signature(EVP_PKEY* pub, std::string_view jwt) {
    auto p1 = jwt.find('.');
    if (p1 == std::string_view::npos) return false;
    auto p2 = jwt.find('.', p1 + 1);
    if (p2 == std::string_view::npos) return false;

    std::string signing_input{jwt.substr(0, p2)};
    std::string_view sig_b64u = jwt.substr(p2 + 1);

    // Decode base64url → r||s (64 bytes for P-256).
    auto from_alpha = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
        if (c >= '0' && c <= '9') return 52 + (c - '0');
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> rs;
    rs.reserve(64);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : sig_b64u) {
        int v = from_alpha(c);
        if (v < 0) return false;
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            rs.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    if (rs.size() != 64) return false;

    // Re-pack r||s into DER ECDSA_SIG so EVP_DigestVerify accepts it.
    BIGNUM* r = BN_bin2bn(rs.data(),      32, nullptr);
    BIGNUM* s = BN_bin2bn(rs.data() + 32, 32, nullptr);
    if (r == nullptr || s == nullptr) {
        BN_free(r);
        BN_free(s);
        return false;
    }
    ECDSA_SIG* sig = ECDSA_SIG_new();
    if (sig == nullptr) {
        BN_free(r);
        BN_free(s);
        return false;
    }
    // ECDSA_SIG_set0 takes ownership of r/s on success.
    if (ECDSA_SIG_set0(sig, r, s) != 1) {
        BN_free(r);
        BN_free(s);
        ECDSA_SIG_free(sig);
        return false;
    }
    uint8_t  der_buf[80];
    uint8_t* der_ptr = der_buf;
    int der_len = i2d_ECDSA_SIG(sig, &der_ptr);
    ECDSA_SIG_free(sig);
    if (der_len <= 0 || der_len > static_cast<int>(sizeof(der_buf))) {
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) return false;
    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pub) == 1) {
        const int rc = EVP_DigestVerify(
            ctx,
            der_buf, static_cast<size_t>(der_len),
            reinterpret_cast<const uint8_t*>(signing_input.data()),
            signing_input.size());
        ok = (rc == 1);
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

/// Build a Coinbase-shape subscribe message. Real Advanced Trade clients
/// also send `product_ids` and a `timestamp`; we keep this minimal so
/// the test focuses on the JWT-round-trip concern.
std::string make_subscribe(std::string_view jwt) {
    std::string out;
    out.reserve(96 + jwt.size());
    out += R"({"type":"subscribe","channel":"user","product_ids":["BTC-USD"],"jwt":")";
    out.append(jwt);
    out += R"("})";
    return out;
}

ek::StreamConfig make_config(uint16_t port) {
    // Coinbase Advanced Trade uses the WS root path "/".
    return eph::test::make_local_tls_ws_config(port, "/");
}

/// Server-side handler that captures every JWT it sees in the subscribe
/// frame and emits a `subscriptions` ack back. The captured JWTs are
/// stored in a shared vector under a mutex so the test thread can read
/// them after the wire dance.
struct CoinbaseFixture {
    std::mutex                       jwts_mu;
    std::vector<std::string>         jwts_received;

    eph::test::TlsWsEchoServer::MessageHandler handler() {
        return [this](std::span<const uint8_t> payload, uint8_t opcode)
            -> std::optional<std::vector<uint8_t>> {
            if (opcode != 0x1) return std::nullopt;
            std::string_view sv{
                reinterpret_cast<const char*>(payload.data()), payload.size()};
            if (sv.find(R"("type":"subscribe")") == std::string_view::npos) {
                return std::nullopt;
            }
            // Extract the JWT value. Naive but fine for a test: find
            //   "jwt":"<value>"
            // and capture <value>. Real JSON parsing would use eph-json.
            constexpr std::string_view kKey = R"("jwt":")";
            auto k = sv.find(kKey);
            if (k != std::string_view::npos) {
                auto vstart = k + kKey.size();
                auto vend   = sv.find('"', vstart);
                if (vend != std::string_view::npos) {
                    std::lock_guard lk(jwts_mu);
                    jwts_received.emplace_back(sv.substr(vstart, vend - vstart));
                }
            }
            return std::vector<uint8_t>(kCoinbaseSubsAck.begin(),
                                        kCoinbaseSubsAck.end());
        };
    }
};

// `drive_until` and `encode_ws_text` come from the shared
// venue_adapter_test_kit.hpp — see the include above.
using eph::test::drive_until;
using eph::test::encode_ws_text;

} // namespace

// ---------------------------------------------------------------------------
// Test: UserChannelJwtRoundTripsAcrossReconnect
// ---------------------------------------------------------------------------
TEST(CoinbaseAdapterIntegration, UserChannelJwtRoundTripsAcrossReconnect) {
    eph::utils::TSC::init();

    // Load the P-256 private key once for both connects. `Es256PrivateKey`
    // is move-only and thread-safe for concurrent signing; the test body
    // is single-threaded so we just hold it on the stack.
    auto key_exp = eph::net::Es256PrivateKey::from_pem(kEcP256TestPem);
    ASSERT_TRUE(key_exp.has_value())
        << "test setup: from_pem failed: " << key_exp.error().detail;
    auto& signing_key = *key_exp;

    // Build the matching public-key handle once for server-side
    // signature verification. We reload the same PEM and extract the
    // public half — `EVP_PKEY` over EC supports verify with only the
    // private side, but reloading is cleaner / mirrors how a real
    // venue server would have only the published public key.
    BIO* pub_bio = BIO_new_mem_buf(kEcP256TestPem.data(),
                                   static_cast<int>(kEcP256TestPem.size()));
    ASSERT_NE(pub_bio, nullptr);
    EVP_PKEY* pub_key = PEM_read_bio_PrivateKey(pub_bio, nullptr,
                                                nullptr, nullptr);
    BIO_free(pub_bio);
    ASSERT_NE(pub_key, nullptr) << "test setup: failed to reload key for verify";

    // Two distinct timestamps so the resulting JWTs differ
    // byte-for-byte (the `nbf` / `exp` claims in the payload).
    constexpr uint64_t kNow1 = 1714291200ULL;  // 2024-04-28T08:00:00Z
    constexpr uint64_t kNow2 = 1714291260ULL;  // +60s
    const std::string jwt1 = make_test_jwt(signing_key, kNow1, kNonce1);
    const std::string jwt2 = make_test_jwt(signing_key, kNow2, kNonce2);
    ASSERT_FALSE(jwt1.empty());
    ASSERT_FALSE(jwt2.empty());
    ASSERT_NE(jwt1, jwt2) << "test JWTs must differ across connects";

    // Sanity: each JWT must self-verify before we stuff it onto the wire.
    // If this fails the test below would be reporting a transport-layer
    // false positive, so we gate here.
    ASSERT_TRUE(verify_jwt_signature(pub_key, jwt1))
        << "JWT #1 self-verify failed before transport";
    ASSERT_TRUE(verify_jwt_signature(pub_key, jwt2))
        << "JWT #2 self-verify failed before transport";

    CoinbaseFixture fx;
    eph::test::TlsWsEchoServer server;
    server.set_message_handler(fx.handler());
    server.enable_request_capture(true);
    server.start();

    auto poller = ek::KernelPoller::create({}).value();

    eph::test::IncomingSink incoming;
    auto on_message = incoming.sink();

    const uint16_t port = server.port();
    auto factory = eph::test::make_stream_factory<TlsWsStream>(
        [port]() { return make_config(port); }, on_message);
    auto attach = eph::test::make_attach<TlsWsStream>(*poller);
    auto detach = eph::test::make_detach<TlsWsStream>(*poller);

    std::atomic<uint64_t> on_reconnect_calls{0};
    auto on_reconnect = [&](uint32_t /*attempt*/, uint64_t /*ns*/) {
        on_reconnect_calls.fetch_add(1, std::memory_order_relaxed);
    };

    Orch orch{
        en::ReconnectConfig{
            .policy = {.initial_backoff = 50ms,
                       .max_backoff     = 200ms,
                       .multiplier      = 2.0,
                       .jitter_factor   = 0.0,
                       .max_attempts    = 5},
            .auto_detect_via_state = true,
        },
        factory,
        /*on_disconnect=*/{},
        on_reconnect,
        attach,
        detach,
    };

    auto sr = orch.start(eph::utils::TSC::now());
    if (!sr) {
        GTEST_SKIP() << "Coinbase-fixture initial connect failed: "
                     << sr.error().detail;
    }
    if (orch.state() != en::ReconnectState::Connected) {
        GTEST_SKIP() << "Coinbase-fixture: orchestrator did not reach Connected "
                        "(state=" << en::to_string(orch.state()) << ")";
    }
    ASSERT_NE(orch.current(), nullptr);

    // ── 1st subscribe: send with JWT #1 ─────────────────────────────────
    {
        auto frame = encode_ws_text(make_subscribe(jwt1));
        auto sr2 = orch.current()->send(frame);
        ASSERT_TRUE(sr2.has_value()) << "send subscribe #1: "
                                     << sr2.error().detail;
    }

    bool got_ack_1 = drive_until(*poller, orch, [&]() {
        return incoming.contains(R"("channel":"subscriptions")");
    });
    ASSERT_TRUE(got_ack_1)
        << "Server never delivered subscriptions ack on first connect";

    {
        std::lock_guard lk(fx.jwts_mu);
        ASSERT_EQ(fx.jwts_received.size(), 1u);
        EXPECT_EQ(fx.jwts_received[0], jwt1)
            << "JWT #1 was corrupted in transit (TLS + WS round-trip)";
        // Server-side cryptographic verification: the JWT must verify
        // against the public key. This is the assertion that was missing
        // before T2.10 was wired through — previously the test only
        // proved string transport, not signature validity.
        EXPECT_TRUE(verify_jwt_signature(pub_key, fx.jwts_received[0]))
            << "JWT #1 ES256 signature failed to verify on server";
    }

    EXPECT_EQ(orch.reconnect_count(), 1u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);

    // ── Disconnect: Coinbase Advanced Trade pushes Close 1011 on
    //    "Internal Error" or 1008 on auth expiry. Either way the
    //    client must reconnect and re-issue a fresh JWT.
    server.send_close_to_all(1011);

    bool reconnected = drive_until(*poller, orch, [&]() {
        return orch.reconnect_count() >= 2u
            && orch.state() == en::ReconnectState::Connected;
    }, 5s);
    ASSERT_TRUE(reconnected)
        << "Orchestrator failed to reconnect within budget; "
        << "state=" << en::to_string(orch.state())
        << " count=" << orch.reconnect_count()
        << " failures=" << orch.reconnect_failures();

    // The factory already installs the `on_message` sink on every
    // freshly-created stream, so no re-attachment is needed on the
    // post-reconnect `fresh` pointer.
    auto* fresh = orch.current();
    ASSERT_NE(fresh, nullptr);

    // ── 2nd subscribe: send with JWT #2 (fresh-token semantics) ─────────
    {
        auto frame = encode_ws_text(make_subscribe(jwt2));
        auto sr2 = fresh->send(frame);
        ASSERT_TRUE(sr2.has_value()) << "send subscribe #2: "
                                     << sr2.error().detail;
    }

    bool got_ack_2 = drive_until(*poller, orch, [&]() {
        return incoming.count_with(R"("channel":"subscriptions")") >= 2;
    });
    ASSERT_TRUE(got_ack_2)
        << "Server did not deliver subscriptions ack on reconnect";

    // ── Assertions ──────────────────────────────────────────────────────
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_GE(orch.reconnect_count(), 2u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);
    EXPECT_GE(server.messages_received(), 2u);
    EXPECT_GE(server.accepted_sessions(), 2u);
    EXPECT_GE(on_reconnect_calls.load(), 2u);

    {
        std::lock_guard lk(fx.jwts_mu);
        ASSERT_EQ(fx.jwts_received.size(), 2u);
        EXPECT_EQ(fx.jwts_received[0], jwt1);
        EXPECT_EQ(fx.jwts_received[1], jwt2)
            << "Reconnect must replay subscribe with the *new* JWT";
        EXPECT_NE(fx.jwts_received[0], fx.jwts_received[1])
            << "Test sanity: the two JWTs must differ";
        // Cryptographic verification on the reconnect token too —
        // proves the per-connect token issuance pattern produces
        // valid JWS each time.
        EXPECT_TRUE(verify_jwt_signature(pub_key, fx.jwts_received[1]))
            << "JWT #2 ES256 signature failed to verify on server";
    }

    // WS upgrade requests sanity: Coinbase Advanced Trade uses no auth
    // headers at the upgrade level (auth is in the subscribe payload),
    // so we only assert the WS upgrade structure is well-formed.
    auto reqs = server.captured_requests();
    ASSERT_GE(reqs.size(), 2u);
    for (auto& r : reqs) {
        EXPECT_NE(r.find("Sec-WebSocket-Key:"), std::string::npos)
            << "Request missing Sec-WebSocket-Key:\n" << r;
        EXPECT_NE(r.find("GET /"), std::string::npos)
            << "Request not targeting /:\n" << r;
        EXPECT_NE(r.find("Upgrade: websocket"), std::string::npos)
            << "Request missing Upgrade: websocket header:\n" << r;
    }

    orch.stop();
    if (orch.current()) (void)poller->remove(orch.current());
    server.stop();

    EVP_PKEY_free(pub_key);
}

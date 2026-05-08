/// @file test_mockex_tls_server.cpp
/// Smoke tests for `mockex::tls::TlsServer` cert loading.
///
/// Stage 1: verify the header compiles and `TlsServer::create()` happy
/// path + bad-path semantics work. Stage 3 will add full handshake
/// roundtrip with a real client (eph-net `TlsSession`).

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "mockex/tls_server.hpp"

namespace fs = std::filesystem;

namespace {

// MOCKEX_FIXTURES_DIR is defined by mockex/xmake.lua at configure time
// (`-DMOCKEX_FIXTURES_DIR="..."`).
fs::path fixtures_dir() {
#ifndef MOCKEX_FIXTURES_DIR
#  error "MOCKEX_FIXTURES_DIR must be defined by the build system"
#endif
    return fs::path{MOCKEX_FIXTURES_DIR};
}

mockex::tls::TlsServerConfig good_cfg() {
    auto base = fixtures_dir() / "tls";
    return {
        .cert_path = (base / "server.crt").string(),
        .key_path  = (base / "server.key").string(),
    };
}

} // namespace

TEST(MockexTlsServer, CreateHappyPath) {
    auto cfg = good_cfg();
    ASSERT_TRUE(fs::exists(cfg.cert_path)) << cfg.cert_path;
    ASSERT_TRUE(fs::exists(cfg.key_path))  << cfg.key_path;

    auto srv_r = mockex::tls::TlsServer::create(cfg);
    ASSERT_TRUE(srv_r.has_value()) << srv_r.error();
    ASSERT_NE(*srv_r, nullptr);
}

TEST(MockexTlsServer, CreateMissingCertFile) {
    mockex::tls::TlsServerConfig cfg{
        .cert_path = "/nonexistent/server.crt",
        .key_path  = (fixtures_dir() / "tls" / "server.key").string(),
    };
    auto srv_r = mockex::tls::TlsServer::create(cfg);
    EXPECT_FALSE(srv_r.has_value());
}

TEST(MockexTlsServer, CreateMissingKeyFile) {
    mockex::tls::TlsServerConfig cfg{
        .cert_path = (fixtures_dir() / "tls" / "server.crt").string(),
        .key_path  = "/nonexistent/server.key",
    };
    auto srv_r = mockex::tls::TlsServer::create(cfg);
    EXPECT_FALSE(srv_r.has_value());
}

TEST(MockexTlsServer, CreateEmptyPathsRejected) {
    auto srv_r = mockex::tls::TlsServer::create({});
    EXPECT_FALSE(srv_r.has_value());
}

// TLS 1.2 negotiation enabled — SSL_CTX must accept the AEAD-only cipher
// list. Smoke-only at this stage; the real 1.2 client/server handshake is
// covered by the integration suite added in Stage 4.
TEST(MockexTlsServer, CreateWithTls12MinVersion) {
    auto cfg = good_cfg();
    cfg.min_version = eph::net::TlsVersion::Tls12;
    auto srv_r = mockex::tls::TlsServer::create(cfg);
    ASSERT_TRUE(srv_r.has_value()) << srv_r.error();
    ASSERT_NE(*srv_r, nullptr);
}

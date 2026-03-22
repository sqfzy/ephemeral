/// @file bench_transport_pipeline.cpp
/// End-to-end transport pipeline benchmark using loopback socketpair.
///
/// Measures the full data path latency and throughput:
///   TX: WS frame encode → TLS encrypt → write(fd)
///   RX: read(fd) → TLS decrypt → WS frame decode
///
/// Parameters:
///   - Payload size: 64 / 256 / 1024 bytes
///   - TLS: on / off (plain WS vs WSS)
///
/// Uses Unix socketpair to avoid external network dependencies.
/// Results include kernel socketpair overhead — subtract BM_SocketpairRoundtrip
/// baseline to isolate pure protocol processing cost.

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include "eph/net/tls_record.hpp"
#include "eph/net/websocket.hpp"

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

/// RAII wrapper for socketpair file descriptors.
struct SocketPair {
    int tx_fd = -1;
    int rx_fd = -1;

    static SocketPair create() {
        int fds[2];
        SocketPair sp;
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            return sp;
        }
        // Increase buffer size for high-throughput benchmarks
        int buf_size = 1 << 20; // 1 MB
        ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
        ::setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        sp.tx_fd = fds[0];
        sp.rx_fd = fds[1];
        return sp;
    }

    ~SocketPair() {
        if (tx_fd >= 0) ::close(tx_fd);
        if (rx_fd >= 0) ::close(rx_fd);
    }

    SocketPair() = default;
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    SocketPair(SocketPair&& o) noexcept : tx_fd(o.tx_fd), rx_fd(o.rx_fd) {
        o.tx_fd = o.rx_fd = -1;
    }
    SocketPair& operator=(SocketPair&&) = delete;
};

/// Write exactly n bytes to fd, retrying on partial writes.
bool write_all(int fd, const uint8_t* buf, size_t n) {
    size_t written = 0;
    while (written < n) {
        auto r = ::write(fd, buf + written, n - written);
        if (r <= 0) return false;
        written += static_cast<size_t>(r);
    }
    return true;
}

/// Read exactly n bytes from fd, retrying on partial reads.
bool read_all(int fd, uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        auto r = ::read(fd, buf + got, n - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

/// Pre-build TLS state for encrypt/decrypt round-trip.
eph::net::TlsHotState make_tls_state(uint32_t seed = 42) {
    eph::net::TlsHotState state{};
    fill_random(state.write.key, eph::net::tls_const::kAes256KeyLen, seed);
    fill_random(state.write.iv,  eph::net::tls_const::kTls13NonceLen, seed + 1);
    std::memcpy(state.read.key, state.write.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(state.read.iv,  state.write.iv,  eph::net::tls_const::kTls13NonceLen);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Baseline: raw socketpair round-trip (to isolate kernel overhead)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_SocketpairRoundtrip(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    auto sp = SocketPair::create();
    if (sp.tx_fd < 0) { state.SkipWithError("socketpair failed"); return; }

    std::vector<uint8_t> tx_buf(sz, 0xAA);
    std::vector<uint8_t> rx_buf(sz);

    for ([[maybe_unused]] auto _ : state) {
        write_all(sp.tx_fd, tx_buf.data(), sz);
        read_all(sp.rx_fd, rx_buf.data(), sz);
        benchmark::DoNotOptimize(rx_buf.data());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}

// ─────────────────────────────────────────────────────────────────────────────
// Plain WS pipeline: WS encode → write → read → WS decode (no TLS)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Pipeline_PlainWS(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    auto sp = SocketPair::create();
    if (sp.tx_fd < 0) { state.SkipWithError("socketpair failed"); return; }

    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz, 10);

    // TX buffers — client frame (masked)
    auto ws_tmpl = eph::net::ws::FrameTemplate::for_binary();
    size_t ws_max = eph::net::ws::kMaxFrameHeaderLen + sz;
    std::vector<uint8_t> ws_buf(ws_max);

    // RX buffer — server frame (unmasked), so build it once for the wire format
    // We simulate a round-trip: encode as client (masked), transmit, receive,
    // decode. The decoded payload should match the original.
    std::vector<uint8_t> rx_buf(ws_max);

    for ([[maybe_unused]] auto _ : state) {
        // TX: WS encode (client, masked)
        size_t ws_len = ws_tmpl.encode(ws_buf.data(), payload.data(), sz);

        // TX: write to socketpair
        write_all(sp.tx_fd, ws_buf.data(), ws_len);

        // RX: read from socketpair
        read_all(sp.rx_fd, rx_buf.data(), ws_len);

        // RX: WS decode
        auto frame = eph::net::ws::decode_frame(rx_buf.data(), ws_len);
        benchmark::DoNotOptimize(frame);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}

// ─────────────────────────────────────────────────────────────────────────────
// WSS pipeline: WS encode → TLS encrypt → write → read → TLS decrypt → WS decode
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Pipeline_WSS(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    auto sp = SocketPair::create();
    if (sp.tx_fd < 0) { state.SkipWithError("socketpair failed"); return; }

    // TLS crypto setup
    auto hot = make_tls_state();
    auto enc = eph::net::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    eph::net::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.key, hot.write.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.iv,  hot.write.iv,  eph::net::tls_const::kTls13NonceLen);
    auto dec = eph::net::TlsRecordCrypto::create(dec_hot);
    if (!dec) { state.SkipWithError(dec.error()); return; }

    // Payload
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz, 10);

    // TX buffers
    auto ws_tmpl = eph::net::ws::FrameTemplate::for_binary();
    // +1 for TLS content type byte
    size_t ws_max = eph::net::ws::kMaxFrameHeaderLen + sz + 1;
    std::vector<uint8_t> ws_buf(ws_max);
    uint16_t tls_max = eph::net::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(ws_max));
    std::vector<uint8_t> tls_buf(tls_max);

    // RX buffers
    std::vector<uint8_t> rx_tls_buf(tls_max);
    std::vector<uint8_t> rx_plain_buf(ws_max + 16);

    for ([[maybe_unused]] auto _ : state) {
        // TX: WS encode
        size_t ws_len = ws_tmpl.encode(ws_buf.data(), payload.data(), sz);

        // TX: TLS encrypt
        uint16_t tls_len = enc->encrypt(
            ws_buf.data(), static_cast<uint16_t>(ws_len), tls_buf.data());

        // TX: write to socketpair
        write_all(sp.tx_fd, tls_buf.data(), tls_len);

        // RX: read from socketpair
        read_all(sp.rx_fd, rx_tls_buf.data(), tls_len);

        // RX: TLS decrypt
        uint16_t dec_len;
        dec->decrypt(rx_tls_buf.data(), tls_len, rx_plain_buf.data(), dec_len);

        // RX: WS decode
        auto frame = eph::net::ws::decode_frame(rx_plain_buf.data(), dec_len);
        benchmark::DoNotOptimize(frame);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}

// ─────────────────────────────────────────────────────────────────────────────
// WSS pipeline with batching: send N messages, then receive N
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Pipeline_WSS_Burst(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    auto burst = static_cast<size_t>(state.range(1));
    auto sp = SocketPair::create();
    if (sp.tx_fd < 0) { state.SkipWithError("socketpair failed"); return; }

    // TLS crypto setup
    auto hot = make_tls_state();
    auto enc = eph::net::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    eph::net::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.key, hot.write.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.iv,  hot.write.iv,  eph::net::tls_const::kTls13NonceLen);
    auto dec = eph::net::TlsRecordCrypto::create(dec_hot);
    if (!dec) { state.SkipWithError(dec.error()); return; }

    // Payload
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz, 10);

    // Buffers
    auto ws_tmpl = eph::net::ws::FrameTemplate::for_binary();
    size_t ws_max = eph::net::ws::kMaxFrameHeaderLen + sz + 1;
    std::vector<uint8_t> ws_buf(ws_max);
    uint16_t tls_max = eph::net::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(ws_max));
    std::vector<uint8_t> tls_buf(tls_max);
    std::vector<uint8_t> rx_tls_buf(tls_max);
    std::vector<uint8_t> rx_plain_buf(ws_max + 16);

    // Track per-message TLS record sizes for RX (each may differ slightly)
    std::vector<uint16_t> record_sizes(burst);

    for ([[maybe_unused]] auto _ : state) {
        // TX burst: encode + encrypt + write N messages
        for (size_t i = 0; i < burst; ++i) {
            size_t ws_len = ws_tmpl.encode(ws_buf.data(), payload.data(), sz);
            uint16_t tls_len = enc->encrypt(
                ws_buf.data(), static_cast<uint16_t>(ws_len), tls_buf.data());
            record_sizes[i] = tls_len;
            write_all(sp.tx_fd, tls_buf.data(), tls_len);
        }

        // RX burst: read + decrypt + decode N messages
        for (size_t i = 0; i < burst; ++i) {
            read_all(sp.rx_fd, rx_tls_buf.data(), record_sizes[i]);
            uint16_t dec_len;
            dec->decrypt(rx_tls_buf.data(), record_sizes[i],
                         rx_plain_buf.data(), dec_len);
            auto frame = eph::net::ws::decode_frame(rx_plain_buf.data(), dec_len);
            benchmark::DoNotOptimize(frame);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(burst));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(burst * sz));
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

static void PipelinePayloadArgs(benchmark::internal::Benchmark* b) {
    for (int sz : {64, 256, 1024}) b->Arg(sz);
}

// Baseline: raw socketpair
BENCHMARK(BM_SocketpairRoundtrip)->Apply(PipelinePayloadArgs);

// Plain WS (no TLS)
BENCHMARK(BM_Pipeline_PlainWS)->Apply(PipelinePayloadArgs);

// WSS (with TLS)
BENCHMARK(BM_Pipeline_WSS)->Apply(PipelinePayloadArgs);

// WSS burst: payload × burst_size
static void BurstArgs(benchmark::internal::Benchmark* b) {
    for (int sz : {64, 256, 1024}) {
        for (int burst : {1, 8, 32}) {
            b->Args({sz, burst});
        }
    }
}
BENCHMARK(BM_Pipeline_WSS_Burst)->Apply(BurstArgs);

BENCHMARK_MAIN();

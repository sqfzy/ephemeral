// libFuzzer harness for eph::net::parse_http_response.
//
// HttpClient feeds whatever bytes come back from the server straight into
// parse_http_response — a hostile or buggy reverse proxy can craft any
// status line / header block / body segment, so the parser must be
// robust against arbitrary byte streams. ASan + UBSan are enabled so
// any out-of-bounds read or signed-overflow surfaces immediately.
//
// Build (from repo root):
//   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 -O1 -g \
//       -Ieph-net/include -Ieph-core/include -Ieph-utils/include \
//       -lspdlog \
//       eph-net/fuzzers/fuzz_http_response.cpp \
//       -o fuzz_http_response
//
// Run:
//   mkdir -p /tmp/http_resp_fuzz
//   cp eph-net/fuzzers/corpus/fuzz_http_response/* /tmp/http_resp_fuzz/
//   ./fuzz_http_response -max_total_time=600 -max_len=8192 /tmp/http_resp_fuzz

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "eph/net/http.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // 32 header slots is the typical HttpClient setup — exceeds any sane
    // exchange response (Binance / OKX / Coinbase return 8-15 headers
    // for a typical /api/v3 GET). Static, not heap-allocated, so ASan
    // catches any out-of-bounds writes from a header-count overflow.
    std::array<eph::net::HttpHeader, 32> headers{};

    auto buf = std::span<const uint8_t>(data, size);
    auto result = eph::net::parse_http_response(buf, std::span(headers));

    // Two acceptable outcomes:
    //   (1) std::unexpected(ErrorInfo) — parser rejected, no UB.
    //   (2) std::optional — either nullopt (need more bytes) or a
    //       ParseResult with status_code in [0, 999] and consumed <= size.
    // Anything else is a bug; sanitizer hits trip __builtin_trap below.
    if (result.has_value() && result->has_value()) {
        const auto& pr = **result;
        if (pr.consumed > size) {
            // Parser claimed to consume more than was given — index OOB
            // would otherwise propagate to the caller's RX buffer pop.
            __builtin_trap();
        }
        if (pr.value.status_code > 999) {
            // 3-ASCII-digit guard above caps at 999; anything past that
            // is a parser arithmetic bug.
            __builtin_trap();
        }
    }
    return 0;
}

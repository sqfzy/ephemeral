/// @file mock_data_gen.hpp
/// Generates mock JSON data for benchmark scenarios.
///
/// All functions write directly into caller-provided buffers with no heap
/// allocation. Fixed price/quantity values — benchmarks measure latency,
/// not data correctness.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace bench::mock {

/// Generate a mock bookTicker JSON message.
///
/// Format: {"s":"BTCUSDT","b":"50000.00","a":"50001.00","T":1234567890123456789}
///
/// @param buf       Output buffer (not null-terminated on return).
/// @param buf_size  Size of the output buffer in bytes.
/// @param symbol    Trading pair symbol (e.g. "BTCUSDT").
/// @param tsc_ns    TSC-derived nanosecond timestamp for the "T" field.
/// @return Number of bytes written to buf, or 0 if the buffer is too small.
inline size_t generate_book_ticker(char* buf, size_t buf_size,
                                   std::string_view symbol,
                                   uint64_t tsc_ns) {
    // snprintf returns the number of chars that *would* be written (excl. '\0')
    int n = std::snprintf(buf, buf_size,
        R"({"s":"%.*s","b":"50000.00","a":"50001.00","T":%llu})",
        static_cast<int>(symbol.size()), symbol.data(),
        static_cast<unsigned long long>(tsc_ns));

    if (n < 0 || static_cast<size_t>(n) >= buf_size) {
        return 0; // buffer too small or encoding error
    }
    return static_cast<size_t>(n);
}

/// Generate a mock ExecutionReport JSON response.
///
/// Format: {"e":"executionReport","s":"BTCUSDT","S":"BUY","o":"LIMIT",
///          "X":"NEW","p":"50000.00","q":"0.001","T":1234567890123456789}
///
/// This is NOT an echo of the client order request — it is an independent
/// response structure simulating a real exchange acknowledgement.
///
/// @param buf       Output buffer (not null-terminated on return).
/// @param buf_size  Size of the output buffer in bytes.
/// @param symbol    Trading pair symbol (e.g. "BTCUSDT").
/// @param side      Order side (e.g. "BUY" or "SELL").
/// @param tsc_ns    TSC-derived nanosecond timestamp for the "T" field
///                  (stamped at response generation time).
/// @return Number of bytes written to buf, or 0 if the buffer is too small.
inline size_t generate_execution_report(char* buf, size_t buf_size,
                                        std::string_view symbol,
                                        std::string_view side,
                                        uint64_t tsc_ns) {
    int n = std::snprintf(buf, buf_size,
        R"({"e":"executionReport","s":"%.*s","S":"%.*s","o":"LIMIT",)"
        R"("X":"NEW","p":"50000.00","q":"0.001","T":%llu})",
        static_cast<int>(symbol.size()), symbol.data(),
        static_cast<int>(side.size()), side.data(),
        static_cast<unsigned long long>(tsc_ns));

    if (n < 0 || static_cast<size_t>(n) >= buf_size) {
        return 0;
    }
    return static_cast<size_t>(n);
}

} // namespace bench::mock

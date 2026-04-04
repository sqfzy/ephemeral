// Fuzz harness for WebSocket frame decoder (eph::net::ws::decode_frame).
//
// Build:
//   clang++ -fsanitize=fuzzer,address -std=c++23 \
//       -Ieph-transport/include -Ieph-core/include \
//       -lfmt -lspdlog \
//       eph-transport/fuzzers/fuzz_ws_decode.cpp -o fuzz_ws_decode
//
// Run:
//   ./fuzz_ws_decode -max_total_time=600

#include <cstddef>
#include <cstdint>

#include "eph/transport/detail/websocket.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Feed raw bytes to the WebSocket frame decoder.
    // decode_frame returns std::expected<DecodedFrame, DecodeError>;
    // we simply let it parse and check for crashes / undefined behavior.
    auto result = eph::net::ws::decode_frame(data, size);

    if (result.has_value()) {
        // Exercise accessors on successfully decoded frames to catch
        // out-of-bounds reads in member functions.
        const auto& frame = result.value();

        [[maybe_unused]] auto ctrl = frame.is_control();
        [[maybe_unused]] auto d    = frame.is_data();
        [[maybe_unused]] auto ping = frame.is_ping();
        [[maybe_unused]] auto pong = frame.is_pong();

        if (frame.is_close()) {
            [[maybe_unused]] auto code   = frame.close_status_code();
            [[maybe_unused]] auto reason = frame.close_reason();
            [[maybe_unused]] auto reason_unmasked = frame.close_reason_unmasked();
        }
    }

    return 0;
}

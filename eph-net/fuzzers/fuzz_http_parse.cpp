// Fuzz harness for HTTP response parser (eph::net::parse_http_response).
//
// Build:
//   clang++ -fsanitize=fuzzer,address -std=c++23 \
//       -Ieph-net/include -Ieph-core/include \
//       -lfmt -lspdlog \
//       eph-net/fuzzers/fuzz_http_parse.cpp -o fuzz_http_parse
//
// Run:
//   ./fuzz_http_parse -max_total_time=600

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "eph/net/http_message.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);

    // Exercise the response parser. Returns std::expected<HttpResponse, std::string>;
    // we let it parse and look for crashes / undefined behavior.
    auto result = eph::net::parse_http_response(input);

    if (result.has_value()) {
        const auto& resp = result.value();

        // Exercise accessor methods to catch latent issues.
        [[maybe_unused]] auto info     = resp.is_informational();
        [[maybe_unused]] auto success  = resp.is_success();
        [[maybe_unused]] auto redir    = resp.is_redirect();
        [[maybe_unused]] auto cli_err  = resp.is_client_error();
        [[maybe_unused]] auto srv_err  = resp.is_server_error();
        [[maybe_unused]] auto err      = resp.is_error();
        [[maybe_unused]] auto json     = resp.to_json();

        // Exercise header lookup with a fixed name.
        [[maybe_unused]] auto ct       = resp.header("Content-Type");
        [[maybe_unused]] auto has_ct   = resp.has_header("Content-Type");
    }

    // Also exercise the completeness check on the same input.
    [[maybe_unused]] auto complete = eph::net::is_http_response_complete(input);

    return 0;
}

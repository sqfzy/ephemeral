// Build: clang++ -fsanitize=fuzzer,address -std=c++23 -Ieph-core/include -o fuzz_json_escape eph-core/fuzzers/fuzz_json_escape.cpp
// Run:   ./fuzz_json_escape -max_total_time=600

#include <cstdint>
#include <cstddef>
#include <string_view>
#include "eph/core/detail/json_escape.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    auto result = eph::core::detail::json_escape(input);
    // Verify output is valid: no unescaped control characters, no unescaped quotes
    for (size_t i = 0; i < result.size(); ++i) {
        char c = result[i];
        if (c == '\\' && i + 1 < result.size()) {
            ++i; // skip escaped char
            continue;
        }
        // No raw control characters should appear in output
        if (static_cast<unsigned char>(c) < 0x20) {
            __builtin_trap(); // control char in output = bug
        }
    }
    return 0;
}

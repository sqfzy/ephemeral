/// @file payload_pool.hpp
/// Rotating pool of pre-captured JSON payloads with O(1) `"T"` field
/// patching on the hot path.
///
/// The mock pushes real Binance `bookTicker` bytes rather than
/// synthetic templates so the client's JSON parser receives realistic
/// field ordering, decimal precision, and occasional escape sequences.
/// To keep the timestamp "server-stamped at the moment of send", we
/// rewrite the `"T":<digits>` field in place right before `send()`.
///
/// Fixture format (one JSON per line):
///
///     {"e":"bookTicker","s":"BTCUSDT",...,"T":1710000000000123456}
///
/// The pool pre-scans each line at load time, finds the offset and
/// width of the `"T":<digits>` run, and verifies the width is large
/// enough to accommodate the current monotonic_raw_ns (~18-19 digits
/// in 2026). On patch, we zero-pad the stamp to the reserved width so
/// the surrounding byte layout is preserved — crucial for any
/// parser-side caches that key on offsets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

namespace mockex {

/// One pre-parsed fixture record: the JSON bytes plus the offset +
/// width of the `"T":<digits>` field that will be rewritten per send.
struct PooledPayload {
    std::vector<uint8_t> bytes;   ///< mutable copy, rewritten in place
    size_t               t_value_offset = 0;  ///< byte index of first digit
    size_t               t_value_width  = 0;  ///< number of reserved digits
};

/// Fixed-capacity pool of captured payloads. Rotation is a monotonic
/// counter mod size — no allocation on the hot path.
class PayloadPool {
public:
    // Default ctor is private (see below) so the only way to obtain a
    // populated `PayloadPool` is `load_from_jsonl`, which guarantees
    // `!payloads_.empty()`. Hot-path code can therefore skip a
    // size-zero check on every `stamp_and_next`.
    PayloadPool(PayloadPool&&) noexcept            = default;
    PayloadPool& operator=(PayloadPool&&) noexcept = default;
    PayloadPool(const PayloadPool&)                = delete;
    PayloadPool& operator=(const PayloadPool&)     = delete;

    /// Load a JSONL file of real exchange frames. Each line must
    /// contain a `"T":<digits>` field; lines that don't are skipped
    /// with a WARN — a partial pool is better than a crash, and a
    /// zero-length result surfaces as a clean error upstream.
    ///
    /// `min_t_width` is the minimum digit width we require the field
    /// to carry so that future monotonic_raw_ns values (up to ~19
    /// digits through year ~2286) can be written without reallocating
    /// the payload. The fitter pads on capture so this always
    /// succeeds for real fixtures; we still verify defensively here.
    [[nodiscard]] static std::expected<PayloadPool, std::string>
    load_from_jsonl(std::string_view path, size_t min_t_width = 19) noexcept {
        std::ifstream in{std::string{path}};
        if (!in) {
            return std::unexpected("PayloadPool: failed to open " +
                                    std::string{path});
        }
        PayloadPool pool;
        std::string line;
        size_t lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            if (line.empty()) continue;
            // `#` prefix is a human-readable comment (e.g. the header
            // the synth_fixture tool writes); skip silently.
            if (!line.empty() && line[0] == '#') continue;

            auto parsed = parse_line_(line, min_t_width);
            if (!parsed) {
                SPDLOG_WARN("[PayloadPool] line {} rejected: {}", lineno,
                            parsed.error());
                continue;
            }
            pool.payloads_.push_back(std::move(*parsed));
        }
        if (pool.payloads_.empty()) {
            return std::unexpected("PayloadPool: no valid payloads in " +
                                    std::string{path});
        }
        SPDLOG_INFO("[PayloadPool] loaded {} payloads from {}",
                    pool.payloads_.size(), path);
        return pool;
    }

    /// Number of pre-loaded samples.
    [[nodiscard]] size_t size() const noexcept { return payloads_.size(); }

    /// Access the i-th raw payload for inspection (tests only).
    [[nodiscard]] std::string_view raw_at(size_t i) const noexcept {
        return std::string_view(
            reinterpret_cast<const char*>(payloads_[i].bytes.data()),
            payloads_[i].bytes.size());
    }

    /// Return the next payload with its `"T"` field stamped to `t_ns`.
    /// Returns a mutable view into internal storage — the caller must
    /// `send()` it before the next `stamp_and_next()` call, because
    /// the same slot may be rewritten on the subsequent rotation.
    [[nodiscard]] std::span<const uint8_t>
    stamp_and_next(uint64_t t_ns) noexcept {
        PooledPayload& p = payloads_[cursor_];
        cursor_ = (cursor_ + 1) % payloads_.size();
        write_padded_digits_(p.bytes.data() + p.t_value_offset,
                             p.t_value_width, t_ns);
        return std::span<const uint8_t>(p.bytes);
    }

private:
    /// Parse one JSON line, locating the `"T":<digits>` run.
    /// Returns the populated PooledPayload or an error string.
    [[nodiscard]] static std::expected<PooledPayload, std::string>
    parse_line_(std::string_view line, size_t min_t_width) noexcept {
        // Find the first `"T":` run. Binance frames carry exactly one
        // `"T"` field; if a future fixture ever contains a nested `T`
        // in a sub-object we'd grab the first occurrence, which is
        // still what the client reads (scan_json_uint_field also
        // grabs the first match).
        constexpr std::string_view needle = "\"T\":";
        const size_t key_pos = line.find(needle);
        if (key_pos == std::string_view::npos) {
            return std::unexpected("no \"T\":<digits> field");
        }
        size_t p = key_pos + needle.size();
        const size_t digit_start = p;
        while (p < line.size() && line[p] >= '0' && line[p] <= '9') ++p;
        const size_t width = p - digit_start;
        if (width < min_t_width) {
            // Real binance captures are padded by the capture tool. If
            // we ever load a hand-written fixture with a short field,
            // reject it with an actionable message.
            return std::unexpected("\"T\" field width " +
                std::to_string(width) + " < min required " +
                std::to_string(min_t_width) +
                " (capture tool should left-pad the digits)");
        }

        PooledPayload out;
        out.bytes.assign(line.begin(), line.end());
        out.t_value_offset = digit_start;
        out.t_value_width  = width;
        return out;
    }

    /// Write `value` as a fixed-width decimal, zero-padded on the
    /// left. If `value` exceeds what `width` can express, truncate
    /// the most-significant digits — the mock then emits an obviously
    /// wrong timestamp that the client's sample-validator can flag,
    /// which is strictly better than overwriting neighbour bytes.
    static void
    write_padded_digits_(uint8_t* dst, size_t width, uint64_t value) noexcept {
        for (size_t i = width; i-- > 0;) {
            dst[i] = static_cast<uint8_t>('0' + (value % 10));
            value /= 10;
        }
    }

    // Private default ctor — only the static factory is allowed to
    // produce a pool, and it never returns one with empty payloads_.
    PayloadPool() = default;

    std::vector<PooledPayload> payloads_;
    size_t                     cursor_{0};
};

} // namespace mockex

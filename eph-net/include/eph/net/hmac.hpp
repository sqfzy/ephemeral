#pragma once

/// @file hmac.hpp
/// @brief Typed HMAC-SHA256 primitive for exchange REST API request signing.
///
/// ## Why this exists
///
/// Crypto exchange REST APIs (Binance, Bybit, OKX, …) require HMAC-SHA256
/// signatures of canonicalized request query strings or bodies. The hot
/// path is: serialize → sign → hex-encode → put in header. HFT trading
/// loops issue thousands of signed requests per second, so this primitive
/// must be:
///
///   1. zero-alloc and `noexcept` on the signing call
///   2. safe with respect to sensitive key material (zero-on-destroy,
///      non-copyable, stored in `alignas(64)` to avoid straddling cache
///      lines / pages that might be swapped)
///   3. built on a vetted cryptographic primitive (aws-lc here — the same
///      library used by the TLS stack in `eph-net/detail/tls_*.hpp`)
///
/// ## API shape — typed Key + Tag (ring / Tokio style)
///
/// Rather than a raw `hmac_sha256(key_bytes, data) -> array<32>` function,
/// we separate the expensive key-normalization step (which can hash a
/// long key) from the per-call signing, using distinct types:
///
///   * `HmacSha256Key`  — RAII wrapper holding the 64-byte normalized
///                        key buffer. Constructed once per API secret,
///                        cleared on destruction with `OPENSSL_cleanse`
///                        (aws-lc's non-optimizable memset), non-copyable
///                        to prevent accidental sprawl of secret bytes.
///   * `HmacSha256Tag`  — a typed 32-byte MAC output. Has `to_hex(span)`
///                        which writes exactly 64 lowercase ASCII hex
///                        characters into a caller-supplied buffer
///                        (zero-alloc). An allocating `to_hex()` overload
///                        exists as a convenience but must NOT be used on
///                        the HFT hot path.
///   * `hmac_sha256_sign(key, data)` — the one-shot signing free function.
///                        Zero-alloc, noexcept, uses aws-lc's `HMAC()`.
///
/// Lowercase hex output matches the Binance/Bybit REST signature wire
/// convention.
///
/// ## Threading
///
/// `HmacSha256Key` is not thread-safe for construction/destruction, but is
/// thread-safe for concurrent signing: `HMAC()` is a stateless one-shot
/// that only reads the key buffer. Per the HFT threading model, each
/// venue adapter owns its own key on a single trading thread, so no
/// synchronization is needed in practice.
///
/// ## Not supported (by design)
///
///   * Streaming signing (`Init`/`Update`/`Final`) — the exchange signing
///     use case always has the full payload materialized in a small
///     contiguous buffer, so one-shot `HMAC()` wins on both simplicity
///     and instruction count.
///   * Constant-time verify — not needed by outgoing signed requests.
///     Incoming verification (for webhook-style inbound) would use a
///     separate `hmac_sha256_verify` helper; not yet implemented.
///   * Base64 encoding — deferred to the adapter layer that needs it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <openssl/crypto.h>   // OPENSSL_cleanse
#include <openssl/evp.h>      // EVP_sha256
#include <openssl/hmac.h>     // HMAC
#include <openssl/sha.h>      // SHA256

#include <spdlog/spdlog.h>

namespace eph::net {

// Forward declarations so Key can friend the sign functions before Tag is
// fully defined below.
struct HmacSha256Tag;
class HmacSha256Key;

[[nodiscard]] inline HmacSha256Tag hmac_sha256_sign(
    const HmacSha256Key&     key,
    std::span<const uint8_t> msg) noexcept;

[[nodiscard]] inline HmacSha256Tag hmac_sha256_sign(
    const HmacSha256Key& key,
    std::string_view     msg) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// HmacSha256Key — typed, RAII, zero-on-destroy
// ─────────────────────────────────────────────────────────────────────────────

/// @brief RAII HMAC-SHA256 key.
///
/// Holds a 64-byte normalized key buffer (the HMAC "ipad/opad block size").
/// Normalization rules per RFC 2104 §2:
///
///   * If `raw.size() <= 64`: copy as-is, zero-pad the remainder to 64 bytes.
///   * If `raw.size()  > 64`: apply SHA-256 to the raw key, place the
///     32-byte result in the buffer, zero-pad the remaining 32 bytes.
///
/// The buffer is cleared on destruction using `OPENSSL_cleanse` (a
/// compiler-barrier memset from aws-lc that cannot be optimized away even
/// at LTO — unlike plain `std::memset` on a stack variable going out of
/// scope).
///
/// Non-copyable so sensitive bytes do not sprawl across stacks; movable so
/// the key can be owned by composing structs and returned from factories.
class HmacSha256Key {
public:
    /// @brief Construct from raw byte span. Normalizes in-place.
    explicit HmacSha256Key(std::span<const uint8_t> raw) noexcept {
        // Zero the whole buffer first so the tail is always zero-padded.
        // Use volatile-write via OPENSSL_cleanse? No — at construction the
        // buffer is untouched so a plain memset is fine. Cleanse only
        // matters at destruction when a DCE pass might skip the write.
        std::memset(normalized_, 0, sizeof(normalized_));

        if (raw.size() > 64) {
            // Long key → hash to 32 bytes, tail stays zero-padded.
            SHA256(raw.data(), raw.size(), normalized_);
            SPDLOG_TRACE("HmacSha256Key: hashed long key ({} bytes) to 32B",
                         raw.size());
        } else if (!raw.empty()) {
            // memcpy(_, nullptr, 0) is ISO C UB even though zero-byte. An
            // empty `std::span` may carry a null `data()` pointer (the
            // standard does not require it to be non-null when size==0),
            // so guard explicitly. The buffer is already zeroed above so
            // an empty key normalizes to all-zero — same RFC 2104 result
            // as a single-byte zero-padded key, with no UB on the way.
            std::memcpy(normalized_, raw.data(), raw.size());
            SPDLOG_TRACE("HmacSha256Key: copied key ({} bytes), zero-padded",
                         raw.size());
        } else {
            SPDLOG_TRACE("HmacSha256Key: empty key (degenerate; "
                         "normalized to zeros)");
        }
    }

    /// @brief Convenience overload for textual keys (e.g. ASCII API secret).
    explicit HmacSha256Key(std::string_view raw) noexcept
        : HmacSha256Key(std::span<const uint8_t>{
              reinterpret_cast<const uint8_t*>(raw.data()), raw.size()}) {}

    HmacSha256Key(const HmacSha256Key&)            = delete;
    HmacSha256Key& operator=(const HmacSha256Key&) = delete;

    /// @brief Move constructor — clears the source buffer so the moved-from
    /// key never lingers in memory.
    HmacSha256Key(HmacSha256Key&& other) noexcept {
        std::memcpy(normalized_, other.normalized_, sizeof(normalized_));
        OPENSSL_cleanse(other.normalized_, sizeof(other.normalized_));
    }

    HmacSha256Key& operator=(HmacSha256Key&& other) noexcept {
        if (this != &other) {
            OPENSSL_cleanse(normalized_, sizeof(normalized_));
            std::memcpy(normalized_, other.normalized_, sizeof(normalized_));
            OPENSSL_cleanse(other.normalized_, sizeof(other.normalized_));
        }
        return *this;
    }

    /// @brief Destructor — clears the key material.
    ///
    /// Uses aws-lc's `OPENSSL_cleanse`, which is a memset that the compiler
    /// and linker (even at LTO) will not elide, because the function is
    /// defined out-of-TU with a side-effecting inline assembly barrier.
    /// This is the aws-lc equivalent of glibc's `explicit_bzero` and it is
    /// portable across aws-lc / BoringSSL / OpenSSL.
    ~HmacSha256Key() noexcept {
        OPENSSL_cleanse(normalized_, sizeof(normalized_));
    }

private:
    // 64 bytes = HMAC-SHA256 block size (RFC 2104).
    // alignas(64) so the buffer sits at the start of a cache line, which
    // (a) ensures OPENSSL_cleanse and HMAC input touch a known-aligned
    // region and (b) avoids straddling a 4KB page that could be swapped
    // independently of other stack state.
    alignas(64) uint8_t normalized_[64]{};

    friend HmacSha256Tag hmac_sha256_sign(
        const HmacSha256Key&, std::span<const uint8_t>) noexcept;
    friend HmacSha256Tag hmac_sha256_sign(
        const HmacSha256Key&, std::string_view) noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// HmacSha256Tag — typed 32-byte MAC output
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Typed 32-byte HMAC-SHA256 output.
///
/// Stored as `std::array<uint8_t, 32>` under the hood so binary comparison
/// and copying are trivial. The type exists to make signatures
/// self-documenting (`HmacSha256Tag` vs the ambiguous `array<u8,32>`) and
/// to expose a hex-encoding method for the REST wire format.
struct HmacSha256Tag {
    std::array<uint8_t, 32> bytes{};

    /// @brief Zero-alloc hex encoding.
    ///
    /// Writes exactly 64 lowercase ASCII hex characters into `out`. The
    /// span size is a compile-time extent (`std::span<uint8_t, 64>`) so
    /// the caller cannot pass the wrong size — a stack buffer of
    /// `uint8_t[64]` decays automatically.
    ///
    /// HFT-safe: zero allocations, zero branches per byte beyond the
    /// nibble lookup, noexcept.
    ///
    /// @return Always 64 (the number of bytes written). Returned as a
    ///         `size_t` so callers can chain into `{out, written}` span
    ///         constructors without casting.
    size_t to_hex(std::span<uint8_t, 64> out) const noexcept {
        static constexpr char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < 32; ++i) {
            out[i * 2]     = static_cast<uint8_t>(kHex[bytes[i] >> 4]);
            out[i * 2 + 1] = static_cast<uint8_t>(kHex[bytes[i] & 0x0F]);
        }
        return 64;
    }

    /// @brief Allocating hex encoding — convenience only.
    ///
    /// Returns a 64-character lowercase hex `std::string`. Allocates, so
    /// do NOT call on the HFT hot path; use the span-based overload there.
    [[nodiscard]] std::string to_hex() const {
        std::string s(64, '\0');
        // Write the hex directly into the string's storage — the previous
        // implementation bounced through a stack buffer + memcpy for no
        // benefit (batch2-round4 LOW-1). The cast is safe because
        // std::string::data() is contiguous writable char* since C++17.
        to_hex(std::span<uint8_t, 64>{
            reinterpret_cast<uint8_t*>(s.data()), 64});
        return s;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// One-shot sign — hot path API
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Sign `msg` with the normalized key and return the 32-byte tag.
///
/// `msg` is the message bytes the caller wants authenticated (e.g. the
/// canonical request string before sending to a venue). Uses aws-lc's
/// `HMAC()` one-shot. Because the key is already normalized to 64 bytes at
/// construction, the per-call work is: two SHA-256 compressions over the
/// ipad/opad blocks plus the message. No allocation, no dynamic context,
/// noexcept.
///
/// If `HMAC()` ever returned null (it cannot, for SHA-256 with a valid
/// key buffer, on any aws-lc build we target), the tag would be
/// zero-initialized by the `HmacSha256Tag{}` return — the caller will
/// see an incorrect-but-deterministic signature. We assert on the
/// output length as a defensive check.
[[nodiscard]] inline HmacSha256Tag hmac_sha256_sign(
    const HmacSha256Key&     key,
    std::span<const uint8_t> msg) noexcept {
    HmacSha256Tag tag{};
    unsigned int  tag_len = 0;

    // HMAC() signature (aws-lc / openssl):
    //   uint8_t* HMAC(const EVP_MD*, const void* key, int key_len,
    //                 const uint8_t* data, size_t data_len,
    //                 uint8_t* out, unsigned int* out_len);
    const uint8_t* result = HMAC(
        EVP_sha256(),
        key.normalized_, 64,                   // always 64 — key is normalized
        msg.data(), msg.size(),
        tag.bytes.data(), &tag_len);

    // Defensive: on any sane aws-lc build these are guaranteed, so this
    // check is compile-time constant-folded away in release.
    if (result == nullptr || tag_len != 32) [[unlikely]] {
        SPDLOG_ERROR("hmac_sha256_sign: HMAC() returned unexpected state "
                     "(result={}, tag_len={})",
                     static_cast<const void*>(result), tag_len);
        // The tag is INPUT to HMAC() — aws-lc may have written partial
        // bytes before bailing out (e.g. on an invalid EVP_MD or after
        // the inner SHA but before the outer one). The function-local
        // `tag{}` initialiser zeroed it pre-call, but we explicitly
        // OPENSSL_cleanse it here so a partial write is wiped before
        // we hand the value back: a "deterministic-but-incorrect"
        // signature that the venue rejects with a clean signature-
        // mismatch is far better diagnostics than half-MAC bits the
        // caller might log to a slow-path observability sink.
        OPENSSL_cleanse(tag.bytes.data(), tag.bytes.size());
    }
    return tag;
}

/// @brief Convenience overload for `std::string_view` payloads.
[[nodiscard]] inline HmacSha256Tag hmac_sha256_sign(
    const HmacSha256Key& key,
    std::string_view     msg) noexcept {
    return hmac_sha256_sign(
        key,
        std::span<const uint8_t>{
            reinterpret_cast<const uint8_t*>(msg.data()), msg.size()});
}

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time guarantees (fail fast if the type shape regresses)
// ─────────────────────────────────────────────────────────────────────────────

static_assert(!std::is_copy_constructible_v<HmacSha256Key>,
              "HmacSha256Key must be non-copyable to contain secret sprawl");
static_assert(!std::is_copy_assignable_v<HmacSha256Key>,
              "HmacSha256Key must be non-copy-assignable");
static_assert(std::is_nothrow_move_constructible_v<HmacSha256Key>,
              "HmacSha256Key move must be noexcept for HFT use");
static_assert(std::is_nothrow_destructible_v<HmacSha256Key>,
              "HmacSha256Key destructor must be noexcept");
static_assert(sizeof(HmacSha256Tag) == 32,
              "HmacSha256Tag must be exactly 32 bytes (no padding)");

} // namespace eph::net

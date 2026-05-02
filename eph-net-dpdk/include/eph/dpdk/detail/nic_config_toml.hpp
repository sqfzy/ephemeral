#pragma once

/// @file detail/nic_config_toml.hpp
/// TOML parser for `/etc/eph/<bdf>.toml` → `NicServiceConfig`.
///
/// Schema (one file per NIC, e.g. `/etc/eph/0000:01:00.1.toml`):
///
/// @code{.toml}
/// pci             = "0000:01:00.1"   # required
/// total_queues    = 16                # default 16
/// rss_key         = "auto"            # "auto" = kDefaultRssKey,
///                                     #   else 80-hex-char string (40 bytes)
/// promiscuous     = false             # default false
/// nb_rx_desc      = 1024              # default 1024
/// nb_tx_desc      = 1024              # default 1024
/// mbuf_pool_size  = 8191              # default 8191 (must be 2^n - 1)
/// mbuf_cache_size = 256               # default 256 (must be < mbuf_pool_size)
/// daemon_lcore    = 0                 # default 0
/// default         = false             # marks this NIC as the implicit
///                                     #   default for apps that omit pci
///                                     #   (S6 wires this in; S4 only parses it)
/// @endcode
///
/// Diagnostics: every error path returns `std::unexpected("nic_config_toml: "
/// "<field>: <reason>")` so the caller (eph-nicd) can log a single line that
/// names the offending field and the rule it violated. Empty / missing fields
/// fall back to `NicServiceConfig`'s in-struct defaults; only `pci` is
/// required.
///
/// Compatibility: the toml++ headers (v3.4.0) co-compile cleanly with aws-lc's
/// OpenSSL-compatible headers (verified at S4 pre-flight). No
/// /tmp/gcc14-wrap typedef-isolation hack is needed for this TU.
///
/// Hot path: NONE. This is consumed exactly once at daemon startup.

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>

#include <spdlog/spdlog.h>

#include "eph/dpdk/platform.hpp"  // NicServiceConfig + validate(NicServiceConfig)

namespace eph::dpdk::detail {

/// @brief Parse a hex string of exactly 80 characters into a 40-byte RSS key.
///
/// Accepts `[0-9a-fA-F]{80}` only. Spaces / `0x` prefixes / colons are NOT
/// stripped — ops files should keep the raw hex form for diff stability.
[[nodiscard]] inline std::expected<std::array<std::uint8_t, 40>, std::string>
parse_rss_key_hex(std::string_view hex) noexcept {
    if (hex.size() != 80) {
        return std::unexpected(std::string{"rss_key: expected exactly 80 hex "
                                           "chars (40 bytes), got "} +
                               std::to_string(hex.size()));
    }
    auto from_hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    std::array<std::uint8_t, 40> out{};
    for (size_t i = 0; i < 40; ++i) {
        const int hi = from_hex(hex[2 * i]);
        const int lo = from_hex(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return std::unexpected(
                std::string{"rss_key: non-hex character at offset "} +
                std::to_string(2 * i));
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}

/// @brief Owning-string variant of `NicServiceConfig` produced by the parser.
///
/// `NicServiceConfig::pci` is a `std::string_view`. The parser owns the
/// underlying string buffer here so the view in the returned config stays
/// valid for the lifetime of the wrapper.
struct ParsedNicConfig {
    std::string      pci_owned;
    NicServiceConfig cfg;
    bool             is_default = false;  // toml `default = true` flag

    /// Make the view in `cfg.pci` point at our owned buffer. Must be called
    /// after every move/copy that would invalidate the previous string_view.
    void rebind() noexcept { cfg.pci = pci_owned; }
};

/// @brief Parse a NIC config toml file into a `NicServiceConfig`.
///
/// On success the returned `ParsedNicConfig::cfg.pci` is a string_view into
/// `ParsedNicConfig::pci_owned` — keep the wrapper alive while the config is
/// in use. Returns the validated config; structural validation
/// (`validate(NicServiceConfig)`) is the LAST step so toml-level errors are
/// reported in their own diagnostic family.
[[nodiscard]] inline std::expected<ParsedNicConfig, std::string>
parse_nic_toml(std::string_view toml_path) {
    namespace fs = std::filesystem;
    const fs::path p{toml_path};

    // 1. Existence check — toml++ would throw / return a parse error too,
    //    but this gives a more actionable diagnostic.
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        SPDLOG_ERROR("nic_config_toml: file not found at '{}'", toml_path);
        return std::unexpected(
            std::string{"nic_config_toml: file not found: "} +
            std::string{toml_path});
    }

    // 2. Parse.
    toml::table tbl;
    try {
        tbl = toml::parse_file(toml_path);
    } catch (const toml::parse_error& e) {
        const auto src = e.source();
        SPDLOG_ERROR(
            "nic_config_toml: parse error at line {} col {}: {} (file '{}')",
            src.begin.line, src.begin.column, e.description(), toml_path);
        return std::unexpected(std::string{"nic_config_toml: parse error: "} +
                               std::string{e.description()});
    }

    // 3. Pull each field with type-checked accessors.
    ParsedNicConfig out{};

    // pci — required, must be a string.
    if (auto* node = tbl.get("pci")) {
        if (auto v = node->value<std::string>()) {
            out.pci_owned = std::move(*v);
        } else {
            return std::unexpected(
                std::string{"nic_config_toml: pci: expected string"});
        }
    } else {
        return std::unexpected(
            std::string{"nic_config_toml: pci: required field is missing"});
    }
    if (out.pci_owned.empty()) {
        return std::unexpected(
            std::string{"nic_config_toml: pci: must be non-empty"});
    }

    // total_queues — uint16, default 16.
    if (auto* node = tbl.get("total_queues")) {
        auto v = node->value<int64_t>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: total_queues: expected integer"});
        if (*v < 1 || *v > 65535) return std::unexpected(
            std::string{"nic_config_toml: total_queues: out of range "
                        "[1, 65535]"});
        out.cfg.total_queues = static_cast<std::uint16_t>(*v);
    }

    // rss_key — "auto" or 80-char hex; default kDefaultRssKey.
    if (auto* node = tbl.get("rss_key")) {
        auto v = node->value<std::string>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: rss_key: expected string"});
        if (*v == "auto") {
            // already kDefaultRssKey by default-construction
        } else {
            auto key_r = parse_rss_key_hex(*v);
            if (!key_r) {
                return std::unexpected(std::string{"nic_config_toml: "} +
                                       key_r.error());
            }
            out.cfg.rss_key = *key_r;
        }
    }

    // promiscuous — bool, default false.
    if (auto* node = tbl.get("promiscuous")) {
        auto v = node->value<bool>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: promiscuous: expected bool"});
        out.cfg.promiscuous = *v;
    }

    // nb_rx_desc — uint16, default 1024.
    if (auto* node = tbl.get("nb_rx_desc")) {
        auto v = node->value<int64_t>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: nb_rx_desc: expected integer"});
        if (*v < 1 || *v > 65535) return std::unexpected(
            std::string{"nic_config_toml: nb_rx_desc: out of range "
                        "[1, 65535]"});
        out.cfg.nb_rx_desc = static_cast<std::uint16_t>(*v);
    }

    // nb_tx_desc — uint16, default 1024.
    if (auto* node = tbl.get("nb_tx_desc")) {
        auto v = node->value<int64_t>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: nb_tx_desc: expected integer"});
        if (*v < 1 || *v > 65535) return std::unexpected(
            std::string{"nic_config_toml: nb_tx_desc: out of range "
                        "[1, 65535]"});
        out.cfg.nb_tx_desc = static_cast<std::uint16_t>(*v);
    }

    // mbuf_pool_size — uint32, default 8191; must be 2^n - 1.
    if (auto* node = tbl.get("mbuf_pool_size")) {
        auto v = node->value<int64_t>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: mbuf_pool_size: expected integer"});
        if (*v < 1 || *v > static_cast<int64_t>(UINT32_MAX)) {
            return std::unexpected(
                std::string{"nic_config_toml: mbuf_pool_size: out of "
                            "uint32_t range"});
        }
        out.cfg.mbuf_pool_size = static_cast<std::uint32_t>(*v);
        if (!is_power_of_two_minus_one(out.cfg.mbuf_pool_size)) {
            return std::unexpected(
                std::string{"nic_config_toml: mbuf_pool_size: must be 2^n-1 "
                            "(e.g. 1023, 4095, 8191)"});
        }
    }

    // mbuf_cache_size — uint16, default 256; checked by validate().
    if (auto* node = tbl.get("mbuf_cache_size")) {
        auto v = node->value<int64_t>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: mbuf_cache_size: expected integer"});
        if (*v < 0 || *v > 65535) return std::unexpected(
            std::string{"nic_config_toml: mbuf_cache_size: out of range "
                        "[0, 65535]"});
        out.cfg.mbuf_cache_size = static_cast<std::uint16_t>(*v);
    }

    // daemon_lcore — uint16, default 0.
    if (auto* node = tbl.get("daemon_lcore")) {
        auto v = node->value<int64_t>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: daemon_lcore: expected integer"});
        if (*v < 0 || *v > 65535) return std::unexpected(
            std::string{"nic_config_toml: daemon_lcore: out of range "
                        "[0, 65535]"});
        out.cfg.daemon_lcore = static_cast<std::uint16_t>(*v);
    }

    // default — bool, default false. Parsed but not enforced here; S6 wires
    // the resolution when an app passes empty cfg.pci.
    if (auto* node = tbl.get("default")) {
        auto v = node->value<bool>();
        if (!v) return std::unexpected(
            std::string{"nic_config_toml: default: expected bool"});
        out.is_default = *v;
    }

    out.rebind();

    // 4. Final structural validation (consistency between fields).
    if (auto err = ::eph::dpdk::validate(out.cfg); !err.empty()) {
        SPDLOG_ERROR(
            "nic_config_toml: structural validate rejected config from '{}': "
            "{}", toml_path, err);
        return std::unexpected(
            std::string{"nic_config_toml: validate: "} + std::string{err});
    }

    SPDLOG_INFO(
        "nic_config_toml: parsed '{}' → pci='{}', total_queues={}, "
        "promiscuous={}, daemon_lcore={}, default={}",
        toml_path, out.pci_owned, out.cfg.total_queues, out.cfg.promiscuous,
        out.cfg.daemon_lcore, out.is_default);
    return out;
}

} // namespace eph::dpdk::detail

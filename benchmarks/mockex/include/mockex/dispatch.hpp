/// @file dispatch.hpp
/// Scenario-name → handler function table.
///
/// Each echo / push scenario registers its `run(ctx)` function here under
/// the same key that the `lat` wrapper uses when spawning the mock
/// (`tcp`, `ud`, `ws`, `ex_order`, `ex_md_udp`, ...). The main binary
/// looks up the handler at startup and exec's it; unknown scenarios fail
/// fast with a listing of the valid keys.
///
/// Kept compile-time — the table is a `constexpr std::array` of POD
/// pairs, so lookup is a linear scan over ≤ 10 entries with zero runtime
/// heap allocation.
#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "mockex/scenario.hpp"
#include "mockex/scenarios/ex_market_2p_push.hpp"
#include "mockex/scenarios/ex_market_push.hpp"
#include "mockex/scenarios/ex_md_udp_echo.hpp"
#include "mockex/scenarios/ex_order_echo.hpp"
#include "mockex/scenarios/rss_scaling_push.hpp"
#include "mockex/scenarios/rss_scaling_ws_push.hpp"
#include "mockex/scenarios/tcp_echo.hpp"
#include "mockex/scenarios/udp_echo.hpp"
#include "mockex/scenarios/ws_echo.hpp"

namespace mockex {

struct ScenarioEntry {
    std::string_view name;   ///< lat script's scenario keyword
    std::string_view section;///< bench.conf section header (without brackets)
    ScenarioFn       fn;
};

/// Full registry of 9 scenarios: 5 echo (tcp / udp / ws /
/// ex_order / ex_md_udp) + 4 push (ex_market / ex_market_2p /
/// rss_scaling / rss_scaling_ws). The `section` column mirrors
/// config.toml exactly so a future audit script can cross-check
/// that every `[scenarios.lat_*]` section has a handler.
inline constexpr std::array<ScenarioEntry, 9> kScenarioTable{{
    {"tcp",            "lat_tcp",            &scenarios::tcp_echo_run},
    {"udp",            "lat_udp",            &scenarios::udp_echo_run},
    {"ws",             "lat_ws",             &scenarios::ws_echo_run},
    {"ex_order",       "lat_ex_order",       &scenarios::ex_order_echo_run},
    {"ex_md_udp",      "lat_ex_md_udp",      &scenarios::ex_md_udp_echo_run},
    {"ex_market",      "lat_ex_market",      &scenarios::ex_market_push_run},
    {"ex_market_2p",   "lat_ex_market_2p",   &scenarios::ex_market_2p_push_run},
    {"rss_scaling",    "lat_rss_scaling",    &scenarios::rss_scaling_push_run},
    {"rss_scaling_ws", "lat_rss_scaling_ws", &scenarios::rss_scaling_ws_push_run},
}};

/// Look up a scenario by its CLI keyword (the same token the `lat`
/// wrapper passes as `--scenario`). Returns nullopt on unknown names.
[[nodiscard]] inline std::optional<ScenarioEntry>
find_scenario(std::string_view name) noexcept {
    for (const auto& e : kScenarioTable) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}

} // namespace mockex

/// @file scenarios/ex_md_udp_echo.hpp
/// Port of benchmarks/latency/mocks/ex_md_udp_echo.py — a UDP echo
/// structurally identical to `udp_echo` but with its own bench.conf
/// section (`[lat_ex_md_udp]`) and port. Phase 11.1 D-1 collapsed the
/// old Mold64 push mock into a plain echo; the only reason this
/// scenario exists separately is so the client can measure md-sized
/// datagrams (256-1400 B) without sharing a port with the generic
/// udp_echo sweep.
#pragma once

#include "mockex/scenario.hpp"
#include "mockex/scenarios/udp_echo.hpp"

namespace mockex::scenarios {

[[nodiscard]] inline int ex_md_udp_echo_run(const ScenarioContext& ctx) noexcept {
    return udp_echo_impl(ctx, "ex_md_udp_echo");
}

} // namespace mockex::scenarios

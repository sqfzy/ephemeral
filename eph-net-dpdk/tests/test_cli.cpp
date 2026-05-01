/// @file eph-net-dpdk/tests/test_cli.cpp
/// Unit tests for `eph::dpdk::cli::try_consume`, `validate`, and
/// `to_eal_config` — the CLI helper that backs the EAL-related flags
/// (`--pci`, `--pin`, `--lcores`, `--port-id`/`--dpdk-port`) shared by
/// `examples/*.cpp`.
///
/// These tests do not touch DPDK at runtime; they only exercise the
/// pure parser/validator/lowerer trio.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/dpdk/cli.hpp"

using namespace eph::dpdk;
using cli::EalArgs;

// ──────────────────────────────────────────────────────────────────────
// try_consume — positive matches
// ──────────────────────────────────────────────────────────────────────

TEST(CliTryConsume, PciAccumulates) {
    EalArgs a{};
    auto r1 = cli::try_consume(a, "--pci", "0000:28:00.0");
    ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error());
    EXPECT_EQ(*r1, 2);

    auto r2 = cli::try_consume(a, "--pci", "0000:28:00.1");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 2);

    ASSERT_EQ(a.pci.size(), 2u);
    EXPECT_EQ(a.pci[0], "0000:28:00.0");
    EXPECT_EQ(a.pci[1], "0000:28:00.1");
}

TEST(CliTryConsume, PinDelegatesToParsePinSpec) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--pin", "3=17:rx-worker");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error());
    EXPECT_EQ(*r, 2);
    ASSERT_EQ(a.pins.size(), 1u);
    EXPECT_EQ(a.pins[0].lcore_id, 3);
    EXPECT_EQ(a.pins[0].cpu_id, 17);
    EXPECT_EQ(a.pins[0].role, "rx-worker");
}

TEST(CliTryConsume, LcoresRaw) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--lcores", "(0-3)@(4-7)");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error());
    EXPECT_EQ(*r, 2);
    EXPECT_EQ(a.lcores_raw, "(0-3)@(4-7)");
}

TEST(CliTryConsume, PortIdSetsFlagAndValue) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--port-id", "1");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error());
    EXPECT_EQ(*r, 2);
    EXPECT_EQ(a.port_id, 1);
    EXPECT_TRUE(a.port_id_set);
}

TEST(CliTryConsume, DpdkPortAliasMatchesPortId) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--dpdk-port", "2");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error());
    EXPECT_EQ(*r, 2);
    EXPECT_EQ(a.port_id, 2);
    EXPECT_TRUE(a.port_id_set);
}

TEST(CliTryConsume, PortIdSetEvenWhenValueIsZero) {
    // Default-constructed port_id is 0; the flag must distinguish
    // "explicit 0" from "left at default" via port_id_set.
    EalArgs a{};
    auto r = cli::try_consume(a, "--port-id", "0");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(a.port_id, 0);
    EXPECT_TRUE(a.port_id_set);
}

// ──────────────────────────────────────────────────────────────────────
// try_consume — negative matches
// ──────────────────────────────────────────────────────────────────────

TEST(CliTryConsume, UnknownFlagReturnsZero) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--host", "stream.binance.com");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0);   // not consumed; caller handles it
    EXPECT_TRUE(a.pci.empty());
}

TEST(CliTryConsume, PciMissingValueRejected) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--pci", nullptr);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("--pci"), std::string::npos);
}

TEST(CliTryConsume, PinMissingValueRejected) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--pin", nullptr);
    ASSERT_FALSE(r.has_value());
}

TEST(CliTryConsume, PinMalformedSpecRejected) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--pin", "garbage");
    ASSERT_FALSE(r.has_value());
    // Error string comes straight from parse_pin_spec.
    EXPECT_NE(r.error().find("expected 'lcore=cpu[:role]'"), std::string::npos);
    EXPECT_TRUE(a.pins.empty());
}

TEST(CliTryConsume, PortIdNonNumericRejected) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--port-id", "abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_FALSE(a.port_id_set);
}

TEST(CliTryConsume, PortIdOverflowRejected) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--port-id", "70000");   // > UINT16_MAX
    ASSERT_FALSE(r.has_value());
    EXPECT_FALSE(a.port_id_set);
}

TEST(CliTryConsume, PortIdNegativeRejected) {
    EalArgs a{};
    auto r = cli::try_consume(a, "--port-id", "-1");
    ASSERT_FALSE(r.has_value());
    EXPECT_FALSE(a.port_id_set);
}

// ──────────────────────────────────────────────────────────────────────
// validate — typed/raw lcore-spec exclusivity
// ──────────────────────────────────────────────────────────────────────

TEST(CliValidate, EmptyArgsAccepted) {
    EalArgs a{};
    EXPECT_TRUE(cli::validate(a).has_value());
}

TEST(CliValidate, OnlyPinsAccepted) {
    EalArgs a{};
    a.pins.push_back(LcorePin{0, 4, ""});
    EXPECT_TRUE(cli::validate(a).has_value());
}

TEST(CliValidate, OnlyLcoresAccepted) {
    EalArgs a{};
    a.lcores_raw = "0-3";
    EXPECT_TRUE(cli::validate(a).has_value());
}

TEST(CliValidate, BothPinsAndLcoresRejected) {
    EalArgs a{};
    a.pins.push_back(LcorePin{0, 4, ""});
    a.lcores_raw = "0-3";
    auto r = cli::validate(a);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("mutually exclusive"), std::string::npos);
}

// ──────────────────────────────────────────────────────────────────────
// to_eal_config — field mapping
// ──────────────────────────────────────────────────────────────────────

TEST(CliToEalConfig, ProgramNameWired) {
    EalArgs a{};
    auto cfg = cli::to_eal_config(std::move(a), "my_program");
    EXPECT_EQ(cfg.program_name, "my_program");
}

TEST(CliToEalConfig, PciFlowsToAllowedDevs) {
    EalArgs a{};
    a.pci = {"0000:28:00.0", "0000:28:00.1"};
    auto cfg = cli::to_eal_config(std::move(a), "p");
    ASSERT_EQ(cfg.allowed_devs.size(), 2u);
    EXPECT_EQ(cfg.allowed_devs[0], "0000:28:00.0");
    EXPECT_EQ(cfg.allowed_devs[1], "0000:28:00.1");
}

TEST(CliToEalConfig, LcoresRawFlowsToLcoresVec) {
    EalArgs a{};
    a.lcores_raw = "(0-3)@(4-7)";
    auto cfg = cli::to_eal_config(std::move(a), "p");
    ASSERT_EQ(cfg.lcores.size(), 1u);
    EXPECT_EQ(cfg.lcores[0], "(0-3)@(4-7)");
}

TEST(CliToEalConfig, EmptyLcoresRawProducesEmptyLcoresVec) {
    // Typed-pin path: caller leaves lcores_raw empty so init_with_pins
    // can emit --lcores=... itself. EalConfig::lcores must be empty.
    EalArgs a{};
    a.pins.push_back(LcorePin{0, 4, ""});
    auto cfg = cli::to_eal_config(std::move(a), "p");
    EXPECT_TRUE(cfg.lcores.empty());
}

// ──────────────────────────────────────────────────────────────────────
// End-to-end: simulate an argv loop
// ──────────────────────────────────────────────────────────────────────

TEST(CliEndToEnd, MixedAppAndEalFlagsLoop) {
    // Simulate what an example's main() does: walk argv, hand each
    // token to try_consume, and on miss fall through to per-app
    // flag handling. The "app flags" here are --host/--duration; the
    // app loop knows they take a value, so it consumes argv[++i]
    // explicitly after a try_consume miss.
    char const* argv[] = {
        "prog",
        "--pci",     "0000:28:00.0",
        "--host",    "stream.binance.com",
        "--pin",     "0=4:ws",
        "--port-id", "1",
        "--duration", "30",
    };
    constexpr int argc = sizeof(argv) / sizeof(argv[0]);

    EalArgs eal{};
    std::string app_host;
    int         app_duration = 0;

    for (int i = 1; i < argc; ++i) {
        char const* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        auto consumed = cli::try_consume(eal, argv[i], next);
        ASSERT_TRUE(consumed.has_value()) << (consumed ? "" : consumed.error());
        if (*consumed > 0) {
            i += *consumed - 1;
            continue;
        }
        // App-specific dispatch: caller owns next-arg consumption.
        std::string_view a = argv[i];
        if      (a == "--host"     && next) { app_host = next; ++i; }
        else if (a == "--duration" && next) { app_duration = std::atoi(next); ++i; }
        else FAIL() << "unhandled token: " << a;
    }

    // EAL state — three flags consumed.
    ASSERT_EQ(eal.pci.size(), 1u);
    EXPECT_EQ(eal.pci[0], "0000:28:00.0");
    ASSERT_EQ(eal.pins.size(), 1u);
    EXPECT_EQ(eal.pins[0].lcore_id, 0);
    EXPECT_EQ(eal.pins[0].cpu_id, 4);
    EXPECT_EQ(eal.pins[0].role, "ws");
    EXPECT_EQ(eal.port_id, 1);
    EXPECT_TRUE(eal.port_id_set);

    // App state — both unknowns parsed by the caller.
    EXPECT_EQ(app_host, "stream.binance.com");
    EXPECT_EQ(app_duration, 30);

    EXPECT_TRUE(cli::validate(eal).has_value());
}

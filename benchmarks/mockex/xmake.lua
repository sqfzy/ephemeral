-- ===========================================================================
-- benchmarks/mockex — unified mock-server binary for the latency bench suite
-- ===========================================================================
--
-- Single binary `mockex` replaces the previous constellation of Python
-- scripts under benchmarks/latency/mocks/. Dispatch is via `--scenario
-- <name>` matching the bench.conf section `[lat_<name>]`. All scenario
-- families are wired (echo: tcp / udp / ws / ex_order / ex_md_udp;
-- push: ex_market / ex_market_2p / rss_scaling / rss_scaling_ws). See
-- include/mockex/scenarios/ for the per-scenario handlers and
-- include/mockex/dispatch.hpp for the kScenarioTable registration.
--
-- See .claude/plans/elegant-toasting-popcorn.md for the original blueprint.

target("mockex")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("src/main.cpp")
    add_includedirs("include")
    -- Reuse the bench core helpers (BenchConfig, monotonic_raw_ns).
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_deps("eph-core", "eph-utils", "eph-net")
    add_packages("spdlog", "tomlplusplus")
    add_cxflags("-fno-omit-frame-pointer")
    set_symbols("debug")

-- Per-scenario integration tests: spawn a `mockex --scenario X` child
-- and drive it with raw POSIX sockets, asserting protocol correctness.
-- Tests follow the eph-test rule (binary, group=tests, default=false,
-- gtest auto-linked).
for _, file in ipairs(os.files(path.join(os.scriptdir(), "tests/test_*.cpp"))) do
    local name = path.basename(file)
    target(name)
        add_rules("eph-test")
        add_files(file)
        add_includedirs("include")
        add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
        add_deps("eph-core", "eph-utils", "eph-net")
        -- Runtime exec dep: tests fork+execl the mockex binary at
        -- MOCKEX_BINARY_PATH (defined below). Without this `add_deps`,
        -- `xmake build -g tests` won't pull mockex (it lives in
        -- group=benchmarks), so the test target builds green but
        -- `xmake run` blows up at execl with ENOENT.
        add_deps("mockex")
        add_packages("spdlog", "tomlplusplus")
        -- Pass the mockex build target's output directory at configure time so
        -- the test can fork the child without searching $PATH. xmake provides
        -- `$(targetdir)` at build but we want a plain string at configure, so
        -- compute it explicitly for the current (arch, mode) pair.
        on_load(function (t)
            local build_dir = path.join(
                os.projectdir(), "build",
                "linux", os.arch(), get_config("mode") or "release")
            t:add("defines", 'MOCKEX_BINARY_PATH="' ..
                path.join(build_dir, "mockex") .. '"')
            -- Absolute path to the checked-in fixture directory; tests
            -- that load *_sample.jsonl can't rely on cwd because xmake
            -- run changes it to the binary dir.
            t:add("defines", 'MOCKEX_FIXTURES_DIR="' ..
                path.join(os.projectdir(), "benchmarks/mockex/fixtures") .. '"')
        end)
end

target("eph-book")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/book/**.hpp)")
    add_headerfiles("include/(eph/book.hpp)")
    add_deps("eph-core", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-book")
end

-- Module benchmarks (depend on eph-json for input data; eph-itch
-- pulled in for the itch_adapter bench — all are header-only so the
-- transitive deps are zero binary cost on benches that don't use
-- the extra modules).
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-book", "eph-json", "eph-itch")
end

target("eph-transport")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/transport/**.hpp)")
    add_deps("eph-core", "eph-utils", "eph-containers", { public = true })
    add_packages("spdlog", "aws-lc", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests
-- Tests depend only on eph-transport itself.  The `using namespace eph::net`
-- declarations in some test files refer to types defined in eph-transport
-- (which lives in the eph::net namespace for historical reasons), not to
-- the eph-net library, so no reverse dependency is required.
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-transport")
end

-- Module benchmarks
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-transport")
end

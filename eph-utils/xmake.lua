target("eph-utils")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/utils/**.hpp)")
    add_headerfiles("include/(eph/utils.hpp)")  -- umbrella
    -- NOTE: eph/version.hpp was moved to eph-core in c4b01264; the install
    -- rule lives there now and is reached via the public eph-core dep.
    add_deps("eph-core", { public = true })
    add_packages("spdlog", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-utils")
end

-- Module benchmarks
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-utils")
end

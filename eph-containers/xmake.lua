target("eph-containers")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/containers/**.hpp)")
    add_deps("eph-utils", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-containers")
end

-- Module benchmarks
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-containers")
        add_packages("tabulate")
        add_includedirs(path.join(os.projectdir(), "benchmarks"))
end

target("eph-core")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/core/**.hpp)")
    add_headerfiles("include/(eph/version.hpp)")
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

    on_config(function (target)
        import("lib.detect.check_cxxsnippets")
        local ok = check_cxxsnippets({test = [[
            #include <expected>
            #include <format>
            void test() {
                std::expected<int, std::string> e = 42;
                auto s = std::format("{}", *e);
            }
        ]]}, {configs = {languages = "c++23"}})
        if not ok then
            raise("C++23 not supported by current compiler.\n"
                  .. "  GCC >= 13 or Clang >= 17 required.\n"
                  .. "  Amazon Linux 2023: `dnf install gcc14-g++` then\n"
                  .. "    xmake f --cxx=/usr/bin/g++-14 --ld=/usr/bin/g++-14\n"
                  .. "  Arch / Ubuntu: the distro compiler is recent enough.")
        end
    end)

-- Module tests
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-core")
end

-- Module benchmarks
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-core")
end

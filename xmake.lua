set_project("eph")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build" })

if is_mode("release") then
	set_optimize("fastest")
end

add_requires("numactl", "tabulate", "benchmark", { optional = true })
add_requires("gtest", { system = false, configs = { main = true } })

option("use_numa")
set_default(false)
set_showmenu(true)
set_description("Enable NUMA support")
add_defines("USE_NUMA")

-----------------------------------------------------------------------------
-- 核心库
-----------------------------------------------------------------------------
target("eph")
set_kind("headeronly")
add_includedirs("include", { public = true })
add_headerfiles("include/(eph/**.hpp)")

-----------------------------------------------------------------------------
-- benchmarks
-----------------------------------------------------------------------------
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
	local name = path.basename(file)

	target(name)
	set_kind("binary")
	set_group("benchmarks")
	add_files(file)
	add_deps("eph")
	add_packages("tabulate")
	add_packages("benchmark")
end

-----------------------------------------------------------------------------
-- tests
-----------------------------------------------------------------------------
for _, file in ipairs(os.files("tests/**.cpp")) do
	local name = path.basename(file)
	target(name)
	set_kind("binary")
	set_group("tests")
	add_files(file)
	add_deps("eph")
	add_packages("gtest")
	-- set_policy("build.sanitizer.address", true)
	-- set_policy("build.sanitizer.undefined", true)
	-- set_policy("build.sanitizer.thread", true)
	-- set_policy("build.sanitizer.undefined", true)
end


-----------------------------------------------------------------------------
-- examples
-----------------------------------------------------------------------------
for _, file in ipairs(os.files("examples/**.cpp")) do
	local name = path.basename(file)

	target(name)
	set_kind("binary")
	set_group("examples")
	add_files(file)
	add_deps("eph")
	add_cxflags("-fno-omit-frame-pointer", "-march=native", {force = true})
	set_symbols("debug")
end

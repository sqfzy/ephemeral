/// @file tests/unit/bench/test_no_dead_headers.cpp
/// Lint test: every header under `benchmarks/latency/core/` must be
/// reachable through the include graph rooted at the `lat_*.cpp`
/// scenario sources, the `mock_*.hpp` exchange mocks, or another
/// `core/*.hpp` that itself is reachable.
///
/// **Why**: this directory has accumulated dead `core/*.hpp` files
/// twice already (most recently `core/udp_client.hpp`, deleted in
/// the cleanup that introduced this test). Each time the death goes
/// unnoticed for weeks because the headers compile fine — they just
/// aren't used by anything that's actually built. A grep-based lint
/// catches it immediately.
///
/// The test walks the include graph by string-matching `#include
/// "..."` directives in the source/header files. It is intentionally
/// shallow (no preprocessor; no transitive `<system>` resolution) so
/// it catches the same kind of dead-code regression a structural
/// audit would, with no build-system gymnastics.
///
/// On failure, the test prints the unreachable header(s) so the dev
/// can either delete them or wire them up.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

namespace {

/// Walk up from CWD looking for the project root (benchmarks/latency must
/// exist underneath it). Returns empty path on failure.
fs::path find_project_root() {
    fs::path p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "benchmarks" / "latency" / "core")) return p;
        if (!p.has_parent_path()) break;
        p = p.parent_path();
    }
    return {};
}

/// Read file → string. Returns empty on error (test will fail naturally).
std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Extract every quoted include directive: `#include "..."`. We don't
/// resolve angle-bracket includes because the dead-header concern is
/// strictly local to `benchmarks/latency/core/`.
std::vector<std::string> quoted_includes(const std::string& src) {
    // Custom delimiter so the embedded `"` doesn't terminate the raw string.
    static const std::regex inc_re(R"RX(#\s*include\s*"([^"]+)")RX");
    std::vector<std::string> out;
    auto it  = std::sregex_iterator(src.begin(), src.end(), inc_re);
    auto end = std::sregex_iterator{};
    for (; it != end; ++it) out.push_back((*it)[1].str());
    return out;
}

/// Resolve an `#include "..."` directive against the includer's directory.
/// Returns the absolute path of the target if it exists, or empty if it
/// doesn't (the include points into another module — irrelevant for our
/// dead-header check).
fs::path resolve_include(const fs::path& includer_dir, const std::string& spec) {
    fs::path candidate = includer_dir / spec;
    if (fs::exists(candidate)) return fs::weakly_canonical(candidate);
    return {};
}

/// Walk the include graph from a set of roots, marking every header
/// that is transitively reachable.
std::set<fs::path> reachable_from(const std::vector<fs::path>& roots) {
    std::set<fs::path> visited;
    std::vector<fs::path> stack(roots.begin(), roots.end());
    while (!stack.empty()) {
        fs::path cur = stack.back();
        stack.pop_back();
        if (!fs::exists(cur)) continue;
        auto canon = fs::weakly_canonical(cur);
        if (!visited.insert(canon).second) continue;

        for (const auto& spec : quoted_includes(slurp(cur))) {
            auto target = resolve_include(cur.parent_path(), spec);
            if (!target.empty() && visited.find(target) == visited.end()) {
                stack.push_back(target);
            }
        }
    }
    return visited;
}

} // namespace

TEST(BenchNoDeadHeaders, EveryCoreHeaderIsReachableFromAScenarioOrTest) {
    auto root = find_project_root();
    ASSERT_FALSE(root.empty()) << "could not locate project root from " << fs::current_path();

    const fs::path bench_lat = root / "benchmarks" / "latency";
    const fs::path bench_core = bench_lat / "core";
    const fs::path test_unit_bench = root / "tests" / "unit" / "bench";

    // Roots = every lat_*.cpp + every mock_*.hpp + every test under
    // tests/unit/bench/. The mock headers are roots because they're
    // included by the lat_*.cpp scenarios but they themselves include
    // core/* headers that aren't otherwise reachable.
    std::vector<fs::path> roots;
    for (const auto& entry : fs::recursive_directory_iterator(bench_lat)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        const auto fname = p.filename().string();
        const bool is_lat_cpp = fname.starts_with("lat_") && p.extension() == ".cpp";
        const bool is_mock_hpp = fname.starts_with("mock_") && p.extension() == ".hpp";
        if (is_lat_cpp || is_mock_hpp) roots.push_back(p);
    }
    if (fs::exists(test_unit_bench)) {
        for (const auto& entry : fs::directory_iterator(test_unit_bench)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cpp") {
                roots.push_back(entry.path());
            }
        }
    }
    ASSERT_FALSE(roots.empty()) << "found no lat_*/mock_* roots under " << bench_lat;

    auto reachable = reachable_from(roots);

    // Now collect every header under core/ and check each one is in
    // the reachable set.
    std::vector<std::string> dead;
    for (const auto& entry : fs::directory_iterator(bench_core)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".hpp") continue;
        auto canon = fs::weakly_canonical(entry.path());
        if (reachable.find(canon) == reachable.end()) {
            dead.push_back(entry.path().filename().string());
        }
    }

    if (!dead.empty()) {
        std::ostringstream msg;
        msg << "found " << dead.size()
            << " dead header(s) under benchmarks/latency/core/ "
               "(no scenario, mock, or unit test reaches them through "
               "the #include graph):\n";
        for (const auto& d : dead) msg << "  - core/" << d << "\n";
        msg << "Either wire them up or delete them.";
        FAIL() << msg.str();
    }
}

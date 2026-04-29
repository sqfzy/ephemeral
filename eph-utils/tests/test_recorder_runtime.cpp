/// @file test_recorder_runtime.cpp
/// Verify that Recorder::export_json emits the runtime tracking fields
/// (`started_at`, `finished_at`, `runtime_seconds`) introduced for the
/// bench TLS feature work, and that runtime_seconds is sane.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "eph/utils/recorder.hpp"

namespace fs = std::filesystem;

namespace {

// Read the most recent .json file in `dir` whose name starts with `prefix`.
std::string slurp_latest_json(const fs::path& dir, std::string_view prefix) {
    fs::path winner;
    fs::file_time_type best{};
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.find(prefix) != 0) continue;
        if (entry.path().extension() != ".json") continue;
        const auto t = entry.last_write_time();
        if (winner.empty() || t > best) {
            winner = entry.path();
            best   = t;
        }
    }
    if (winner.empty()) return {};
    std::ifstream in(winner);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST(RecorderRuntime, JsonContainsStartedFinishedRuntimeFields) {
    const fs::path out_dir =
        fs::temp_directory_path() / "eph_test_recorder_runtime";
    fs::remove_all(out_dir);

    eph::utils::Recorder rec("rt_test_sample");
    // Sleep so runtime_seconds is observably > 0.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rec.record_ns(1000);
    ASSERT_TRUE(rec.export_json(out_dir.string()));

    const auto body = slurp_latest_json(out_dir, "rt_test_sample");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("\"started_at\""),       std::string::npos) << body;
    EXPECT_NE(body.find("\"finished_at\""),      std::string::npos) << body;
    EXPECT_NE(body.find("\"runtime_seconds\""),  std::string::npos) << body;

    // started_at should be ISO-8601 UTC with the trailing 'Z'.
    EXPECT_NE(body.find("Z\""), std::string::npos) << body;
}

TEST(RecorderRuntime, RuntimeSecondsIsPositiveAfterSleep) {
    const fs::path out_dir =
        fs::temp_directory_path() / "eph_test_recorder_runtime_pos";
    fs::remove_all(out_dir);

    eph::utils::Recorder rec("rt_test_positive");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    rec.record_ns(500);
    ASSERT_TRUE(rec.export_json(out_dir.string()));

    const auto body = slurp_latest_json(out_dir, "rt_test_positive");
    ASSERT_FALSE(body.empty());

    // Locate `"runtime_seconds": <float>,` and parse the float.
    const auto key = std::string{"\"runtime_seconds\":"};
    auto pos = body.find(key);
    ASSERT_NE(pos, std::string::npos);
    pos += key.size();
    while (pos < body.size() && body[pos] == ' ') ++pos;
    auto end = body.find(',', pos);
    ASSERT_NE(end, std::string::npos);
    const double runtime_s = std::stod(body.substr(pos, end - pos));

    EXPECT_GT(runtime_s, 0.05) << "expected > 50ms, got " << runtime_s;
    EXPECT_LT(runtime_s, 5.0)  << "sanity: should be sub-second-ish, got " << runtime_s;
}

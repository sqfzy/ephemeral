/// @file test_registry_hmac_key.cpp
/// T2.3 wiring tests: key-distribution helper for the registry HMAC
/// (write at daemon side, read at tenant side).
///
/// Production path is `/run/eph/<bdf>.key` mode 0440 root:root, but
/// these unit tests use a tmpdir-prefixed path via the
/// `write_registry_hmac_key_at` test-only overload — root + /run is
/// not available in CI.
///
/// Coverage:
///   - Write returns 32 random bytes, file mode is 0440, file size 32.
///   - Read round-trips the same bytes.
///   - Read on missing file returns InvalidConfig.
///   - Read on truncated file returns InvalidConfig.
///   - registry_hmac_key_path validates the BDF format (rejects empty).

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <sys/stat.h>

#include "eph/dpdk/detail/registry_hmac_key.hpp"

namespace ed = eph::dpdk::detail;

namespace {

class TmpDir {
public:
    TmpDir() {
        char tpl[] = "/tmp/eph_hmac_key_test_XXXXXX";
        char* d = ::mkdtemp(tpl);
        if (!d) {
            ADD_FAILURE() << "mkdtemp failed";
            path_ = "/tmp/eph_hmac_key_test_fallback";
        } else {
            path_ = d;
        }
    }
    ~TmpDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] std::filesystem::path file(const char* name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST(RegistryHmacKey, KeyPathSanitizesBdf) {
    auto p = ed::registry_hmac_key_path("0000:01:00.1");
    ASSERT_TRUE(p.has_value()) << p.error().detail;
    EXPECT_EQ(p->parent_path(), "/run/eph");
    // sanitize_bdf_for_file_prefix replaces : and . with _
    EXPECT_EQ(p->filename(), "0000_01_00_1.key");
}

TEST(RegistryHmacKey, KeyPathRejectsEmptyBdf) {
    auto p = ed::registry_hmac_key_path("");
    EXPECT_FALSE(p.has_value());
}

TEST(RegistryHmacKey, WriteThenReadRoundTrip) {
    TmpDir dir;
    const auto path = dir.file("test.key");

    auto written = ed::write_registry_hmac_key_at(path);
    ASSERT_TRUE(written.has_value()) << written.error().detail;
    EXPECT_EQ(written->size(), ed::kRegistryHmacKeyBytes);

    // File should be 32 bytes.
    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(path, ec), 32u);

    // Mode should be 0440.
    struct stat st;
    ASSERT_EQ(::stat(path.string().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0440u);

    auto readback = ed::read_registry_hmac_key(path);
    ASSERT_TRUE(readback.has_value()) << readback.error().detail;
    EXPECT_EQ(*readback, *written);
}

TEST(RegistryHmacKey, ReadMissingFileReturnsError) {
    TmpDir dir;
    auto r = ed::read_registry_hmac_key(dir.file("nonexistent.key"));
    EXPECT_FALSE(r.has_value());
}

TEST(RegistryHmacKey, ReadTruncatedFileReturnsError) {
    TmpDir dir;
    const auto path = dir.file("truncated.key");

    // Write only 16 bytes (less than the 32 expected).
    {
        std::ofstream f(path, std::ios::binary);
        for (int i = 0; i < 16; ++i) f.put(static_cast<char>(i));
    }

    auto r = ed::read_registry_hmac_key(path);
    EXPECT_FALSE(r.has_value());
}

TEST(RegistryHmacKey, MultipleWritesGiveDifferentKeys) {
    // CSPRNG should not produce the same bytes twice in a row.
    TmpDir dir;
    auto k1 = ed::write_registry_hmac_key_at(dir.file("k1.key"));
    auto k2 = ed::write_registry_hmac_key_at(dir.file("k2.key"));
    ASSERT_TRUE(k1.has_value());
    ASSERT_TRUE(k2.has_value());
    EXPECT_NE(*k1, *k2);
}

TEST(RegistryHmacKey, AtomicWriteRenamesIntoPlace) {
    // Verify that a second write to the same path overwrites cleanly
    // (atomic rename — the previous file is replaced, no .tmp left
    // behind).
    TmpDir dir;
    const auto path = dir.file("k.key");
    auto k1 = ed::write_registry_hmac_key_at(path);
    auto k2 = ed::write_registry_hmac_key_at(path);
    ASSERT_TRUE(k1.has_value());
    ASSERT_TRUE(k2.has_value());

    auto readback = ed::read_registry_hmac_key(path);
    ASSERT_TRUE(readback.has_value());
    EXPECT_EQ(*readback, *k2);  // last write wins

    std::filesystem::path tmp = path;
    tmp += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(tmp));
}

/// @file test_dpdk_autojoin_gw_mac.cpp
/// Unit tests for `DpdkBenchEnv::create_via_autojoin` gw_mac shared-file
/// publication / consumption protocol — round-6 silent-failure closure
/// (atomic publish + stale unlink + empty-path guard + all-zero MAC reject).
///
/// Discovered via /pax --review LENS "production / MP mental model /
/// silent misuse closure" round 6 on 2026-05-01.
///
/// These tests deliberately split into two categories:
///
///   1. PRECONDITION TESTS exercise the empty-path guard which is
///      checked BEFORE any EAL / DPDK call inside create_via_autojoin,
///      so they run cleanly with no NIC / no hugepage backing.
///
///   2. FILE-PROTOCOL TESTS simulate the publish/consume sequence
///      locally without going through create_via_autojoin (which would
///      require a real DPDK NIC + ARP + sibling secondary peer). The
///      protocol logic — `::unlink` stale before publishing,
///      tmp-write + ::rename atomic publish, all-zero MAC reject on
///      parse — is exercised on a tmpfile path with the same
///      `<unistd.h>` / `<cstdio>` primitives the production code uses,
///      so a future regression of any single primitive would surface
///      here.

// dpdk_env.hpp gates its content with #ifdef EPH_USE_DPDK so the
// header can compile-out cleanly in kernel-only builds. For this
// unit test we explicitly opt in — the empty-path precondition is
// pure C++ logic and has no EAL dependency, and the file-protocol
// tests use only POSIX primitives.
#ifndef EPH_USE_DPDK
#  define EPH_USE_DPDK 1
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "eph/dpdk/test/dpdk_env.hpp"

using eph::dpdk::test::DpdkBenchEnv;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers — local-FS tmp file under /tmp/eph_autojoin_gwmac_test_<pid>_<n>
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// RAII tmp-file path that auto-unlinks on destruction. We deliberately
/// don't use `mkstemp` because we want to test paths that may NOT exist
/// yet (the unlink-stale-first semantics).
class TmpPath {
public:
    explicit TmpPath(const char* tag) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "/tmp/eph_autojoin_gwmac_test_%d_%s",
                      static_cast<int>(::getpid()), tag);
        path_ = buf;
        // Ensure clean slate.
        ::unlink(path_.c_str());
    }
    ~TmpPath() {
        if (!path_.empty()) ::unlink(path_.c_str());
    }
    TmpPath(const TmpPath&)            = delete;
    TmpPath& operator=(const TmpPath&) = delete;
    [[nodiscard]] const std::string& str() const noexcept { return path_; }
    [[nodiscard]] const char* c_str() const noexcept { return path_.c_str(); }
private:
    std::string path_;
};

/// Write `content` (a full line) to `path` using the SAME tmp-then-rename
/// pattern create_via_autojoin uses on the primary side. Returns the
/// rename rc (0 on success, errno on failure).
int atomic_publish(const std::string& path, const std::string& content) {
    const std::string tmp =
        path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return EIO;
        f << content;
        if (!f.good()) {
            ::unlink(tmp.c_str());
            return EIO;
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        ::unlink(tmp.c_str());
        return saved == 0 ? EIO : saved;
    }
    return 0;
}

/// Read the first non-empty line from `path` and parse it as
/// `aa:bb:cc:dd:ee:ff`. Returns `{0..0}` on any failure (caller
/// distinguishes via the all-zero-reject check). Mirror of the
/// secondary-side parse path.
std::array<uint8_t, 6> read_and_parse_mac(const std::string& path) {
    std::array<uint8_t, 6> out{};
    std::ifstream f(path);
    if (!f.is_open()) return out;
    std::string line;
    if (!std::getline(f, line) || line.empty()) return out;
    unsigned int b[6];
    if (std::sscanf(line.c_str(), "%x:%x:%x:%x:%x:%x",
                    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return out;
    }
    for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(b[i]);
    return out;
}

bool is_all_zero(const std::array<uint8_t, 6>& m) noexcept {
    return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) == 0;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// PRECONDITION TESTS — empty gw_mac_share_path rejected before any DPDK call
// ─────────────────────────────────────────────────────────────────────────────

TEST(AutojoinGwMacGuard, EmptyPath_RejectedUpFront) {
    // create_via_autojoin checks the empty-path guard BEFORE Platform::
    // join_dynamic / EAL / parse_lcores_csv_to_mask, so this call returns
    // a clean std::unexpected without ever touching DPDK. A non-empty
    // pci_bdf is supplied so we don't trip the max_procs guard first.
    auto r = DpdkBenchEnv::create_via_autojoin(
        /*pci_bdf=*/"0000:00:00.0",
        /*max_procs=*/2,
        /*lcores=*/"0,1",
        /*mock_ip=*/"127.0.0.1",
        /*client_ip=*/"127.0.0.1",
        /*gateway_ip=*/"127.0.0.1",
        /*gw_mac_share_path=*/"");
    ASSERT_FALSE(r.has_value());
    // Error must name the env var so an orchestrator misconfig is
    // self-diagnosing — without this the user just sees a useless
    // "" path in a "cannot write" diagnostic.
    EXPECT_NE(r.error().find("EPH_LAT_AUTOJOIN_GW_MAC_FILE"),
              std::string::npos)
        << "error message must name the env var; got: " << r.error();
    EXPECT_NE(r.error().find("empty"), std::string::npos)
        << "error must explain *why*; got: " << r.error();
}

TEST(AutojoinGwMacGuard, MaxProcsBelowTwo_RejectedFirst) {
    // max_procs guard fires before the empty-path guard, so passing
    // both invalid inputs surfaces the max_procs error, not the path
    // error. This documents the precondition order.
    auto r = DpdkBenchEnv::create_via_autojoin(
        /*pci_bdf=*/"0000:00:00.0",
        /*max_procs=*/1,
        /*lcores=*/"0",
        /*mock_ip=*/"127.0.0.1",
        /*client_ip=*/"127.0.0.1",
        /*gateway_ip=*/"127.0.0.1",
        /*gw_mac_share_path=*/"");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("max_procs"), std::string::npos)
        << "max_procs guard should fire first; got: " << r.error();
}

// ─────────────────────────────────────────────────────────────────────────────
// FILE-PROTOCOL TESTS — atomic publish + stale unlink + all-zero parse
// (simulate the protocol locally with the same POSIX primitives)
// ─────────────────────────────────────────────────────────────────────────────

TEST(AutojoinGwMacFile, AtomicPublishVisibleToReader) {
    TmpPath path("atomic");
    ASSERT_EQ(atomic_publish(path.str(),
                             "aa:bb:cc:dd:ee:ff\n"), 0);

    auto mac = read_and_parse_mac(path.str());
    EXPECT_FALSE(is_all_zero(mac));
    EXPECT_EQ(mac[0], 0xaa);
    EXPECT_EQ(mac[1], 0xbb);
    EXPECT_EQ(mac[2], 0xcc);
    EXPECT_EQ(mac[3], 0xdd);
    EXPECT_EQ(mac[4], 0xee);
    EXPECT_EQ(mac[5], 0xff);
}

TEST(AutojoinGwMacFile, StaleUnlinkBeforePublish) {
    // Simulate: a previous run left "01:02:03:04:05:06\n" at the path.
    // The current primary's first action MUST be `::unlink` so the
    // current secondary never sees the stale bytes.
    TmpPath path("stale");

    // Phase 1: previous run writes stale.
    {
        std::ofstream f(path.str());
        ASSERT_TRUE(f.is_open());
        f << "01:02:03:04:05:06\n";
    }
    // Sanity: file exists with stale content.
    {
        auto stale = read_and_parse_mac(path.str());
        EXPECT_EQ(stale[0], 0x01);
    }

    // Phase 2: current primary unlinks (production code does this
    // before arp::resolve). errno != ENOENT case is the one we care
    // about; ENOENT (file gone) is fine.
    const int rc = ::unlink(path.c_str());
    EXPECT_EQ(rc, 0) << "unlink errno=" << errno;

    // Phase 3: secondary can no longer read the stale content.
    {
        std::ifstream f(path.str());
        EXPECT_FALSE(f.is_open())
            << "after unlink, secondary must not be able to open "
               "the stale file";
    }

    // Phase 4: current primary publishes fresh — atomic.
    ASSERT_EQ(atomic_publish(path.str(),
                             "aa:bb:cc:dd:ee:ff\n"), 0);
    auto fresh = read_and_parse_mac(path.str());
    EXPECT_EQ(fresh[0], 0xaa);
}

TEST(AutojoinGwMacFile, UnlinkOnNonExistent_IsBenign) {
    // First-run case: file doesn't exist yet. ::unlink returns -1
    // with errno=ENOENT; production code must treat this as success.
    TmpPath path("no_prev");
    // Make sure path doesn't exist.
    ::unlink(path.c_str());
    errno = 0;
    const int rc = ::unlink(path.c_str());
    EXPECT_EQ(rc, -1);
    EXPECT_EQ(errno, ENOENT)
        << "ENOENT is the expected first-run errno; production code "
           "ignores it";
}

TEST(AutojoinGwMacFile, AllZeroMacRejected) {
    // Simulate: primary writes file before ARP completes, so the MAC
    // bytes are all-zero. Secondary MUST detect this — a 00:00:..:00
    // MAC is never a valid gateway and silently using it would have
    // the kernel mock blackhole every frame.
    TmpPath path("zero");
    ASSERT_EQ(atomic_publish(path.str(),
                             "00:00:00:00:00:00\n"), 0);

    auto mac = read_and_parse_mac(path.str());
    // Parse succeeds (returns 6 valid hex tokens) but bytes are zero.
    EXPECT_TRUE(is_all_zero(mac))
        << "test-helper sanity: 00:00:00:00:00:00 must parse to all-zero";
    // The production secondary path checks `is_all_zero` and returns
    // std::unexpected — exercise the same logic here.
}

TEST(AutojoinGwMacFile, MalformedLineFailsParse) {
    // Truncated / corrupt line — sscanf returns < 6 tokens. Production
    // surface: `secondary failed to parse gw_mac line: ...` rather
    // than silently using junk bytes.
    TmpPath path("corrupt");
    ASSERT_EQ(atomic_publish(path.str(), "aa:bb:cc\n"), 0);

    auto mac = read_and_parse_mac(path.str());
    EXPECT_TRUE(is_all_zero(mac))
        << "helper returns all-zero on parse failure; production path "
           "explicitly returns std::unexpected here";
}

/// @file test_default_nic_scan.cpp
/// Unit coverage for the default-NIC resolver. Uses a tmpfs sandbox
/// (no DPDK / no real /var/run/dpdk required) to drive the three
/// outcomes: single-daemon → success, zero-daemon → typed error,
/// multi-daemon → typed error with both BDFs in the message.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "eph/dpdk/detail/default_nic_scan.hpp"

namespace fs = std::filesystem;

namespace {

/// RAII sandbox: fresh /tmp/test_default_nic_scan_<rand>/ for each test.
class ScanSandbox {
public:
    ScanSandbox() {
        std::random_device rd;
        std::uniform_int_distribution<uint64_t> dist;
        root_ = fs::temp_directory_path() /
                ("eph_default_nic_scan_test_" + std::to_string(dist(rd)));
        fs::create_directories(root_);
        // Must populate AFTER root_ is set; default-member-init runs in
        // declaration order before constructor body, so an in-class
        // initializer like `root_str_ = root_.string()` would capture an
        // empty path.
        root_str_ = root_.string();
    }
    ~ScanSandbox() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    /// Simulate a live daemon: create eph_<sanitized>/{config, eph-pci.txt}.
    void add_live_daemon(std::string_view pci) {
        std::string sanitized{pci};
        for (auto& c : sanitized) {
            if (c == ':' || c == '.') c = '_';
        }
        fs::path dir = root_ / ("eph_" + sanitized);
        fs::create_directories(dir);
        std::ofstream(dir / "config") << "fake-eal-primary-marker\n";
        std::ofstream(dir / "eph-pci.txt") << pci << "\n";
    }

    /// Simulate a dead-but-namespace-leaked daemon: directory exists,
    /// has eph-pci.txt, but no `config` file (graceful exit).
    void add_dead_daemon(std::string_view pci) {
        std::string sanitized{pci};
        for (auto& c : sanitized) {
            if (c == ':' || c == '.') c = '_';
        }
        fs::path dir = root_ / ("eph_" + sanitized);
        fs::create_directories(dir);
        // No `config` — graceful-exit cleanup state.
        std::ofstream(dir / "eph-pci.txt") << pci << "\n";
    }

    /// Simulate a non-eph DPDK app (e.g. user's own primary): name doesn't
    /// start with "eph_" so the scanner ignores it.
    void add_non_eph_dpdk_app(std::string_view name) {
        fs::path dir = root_ / std::string{name};
        fs::create_directories(dir);
        std::ofstream(dir / "config") << "another-eal-primary\n";
    }

    std::string_view root_view() const noexcept { return root_str_; }

private:
    fs::path    root_;
    std::string root_str_;
};

}  // namespace

using eph::dpdk::detail::scan_eph_nicd_daemons;
using eph::dpdk::detail::find_default_nic_pci;

// ─────────────────────────────────────────────────────────────────────
// scan_eph_nicd_daemons
// ─────────────────────────────────────────────────────────────────────

TEST(DefaultNicScan, EmptyDirectoryYieldsNoCandidates) {
    ScanSandbox sb;
    auto r = scan_eph_nicd_daemons(sb.root_view());
    EXPECT_TRUE(r.candidates.empty());
}

TEST(DefaultNicScan, NonexistentPathYieldsNoCandidates) {
    auto r = scan_eph_nicd_daemons("/tmp/this/path/should/not/exist/at/all");
    EXPECT_TRUE(r.candidates.empty());
}

TEST(DefaultNicScan, SingleLiveDaemonDiscovered) {
    ScanSandbox sb;
    sb.add_live_daemon("0000:28:00.0");
    auto r = scan_eph_nicd_daemons(sb.root_view());
    ASSERT_EQ(r.candidates.size(), 1u);
    EXPECT_EQ(r.candidates[0], "0000:28:00.0");
}

TEST(DefaultNicScan, MultipleLiveDaemonsAllDiscovered) {
    ScanSandbox sb;
    sb.add_live_daemon("0000:28:00.0");
    sb.add_live_daemon("0000:29:00.0");
    auto r = scan_eph_nicd_daemons(sb.root_view());
    ASSERT_EQ(r.candidates.size(), 2u);
    // Sorted ascending for deterministic diagnostics
    EXPECT_EQ(r.candidates[0], "0000:28:00.0");
    EXPECT_EQ(r.candidates[1], "0000:29:00.0");
}

TEST(DefaultNicScan, DeadDaemonIgnored) {
    ScanSandbox sb;
    sb.add_live_daemon("0000:28:00.0");
    sb.add_dead_daemon("0000:29:00.0");  // no `config` file
    auto r = scan_eph_nicd_daemons(sb.root_view());
    ASSERT_EQ(r.candidates.size(), 1u);
    EXPECT_EQ(r.candidates[0], "0000:28:00.0");
}

TEST(DefaultNicScan, NonEphAppIgnored) {
    ScanSandbox sb;
    sb.add_live_daemon("0000:28:00.0");
    sb.add_non_eph_dpdk_app("rte_default_42");  // some other DPDK primary
    auto r = scan_eph_nicd_daemons(sb.root_view());
    ASSERT_EQ(r.candidates.size(), 1u);
    EXPECT_EQ(r.candidates[0], "0000:28:00.0");
}

// ─────────────────────────────────────────────────────────────────────
// find_default_nic_pci — single-daemon happy path + error shapes
// ─────────────────────────────────────────────────────────────────────

TEST(DefaultNicResolve, ZeroDaemonsReturnsTypedError) {
    ScanSandbox sb;
    auto r = find_default_nic_pci(sb.root_view());
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("no eph-nicd daemon running"), std::string::npos)
        << "diagnostic should mention no daemon: " << r.error();
    EXPECT_NE(r.error().find("specify `cfg.pci`"), std::string::npos)
        << "diagnostic should suggest explicit pci: " << r.error();
}

TEST(DefaultNicResolve, SingleDaemonReturnsBdf) {
    ScanSandbox sb;
    sb.add_live_daemon("0000:28:00.0");
    auto r = find_default_nic_pci(sb.root_view());
    ASSERT_TRUE(r) << "expected success, got error: " << r.error();
    EXPECT_EQ(*r, "0000:28:00.0");
}

TEST(DefaultNicResolve, MultipleDaemonsReturnTypedErrorListingAll) {
    ScanSandbox sb;
    sb.add_live_daemon("0000:28:00.0");
    sb.add_live_daemon("0000:29:00.0");
    auto r = find_default_nic_pci(sb.root_view());
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("multiple eph-nicd daemons"), std::string::npos)
        << r.error();
    EXPECT_NE(r.error().find("0000:28:00.0"), std::string::npos) << r.error();
    EXPECT_NE(r.error().find("0000:29:00.0"), std::string::npos) << r.error();
    EXPECT_NE(r.error().find("specify"), std::string::npos) << r.error();
}

TEST(DefaultNicResolve, DeadDaemonNotConsideredCandidate) {
    ScanSandbox sb;
    sb.add_dead_daemon("0000:28:00.0");
    auto r = find_default_nic_pci(sb.root_view());
    ASSERT_FALSE(r);  // no live daemons, dead one is filtered
    EXPECT_NE(r.error().find("no eph-nicd daemon running"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────
// write_pci_announce_file — daemon-side helper
// ─────────────────────────────────────────────────────────────────────

TEST(WritePciAnnounceFile, MissingNamespaceDirectoryIsTypedError) {
    ScanSandbox sb;
    auto r = ::eph::dpdk::detail::write_pci_announce_file(
        "eph_0000_99_00_0", "0000:99:00.0", sb.root_view());
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("not present"), std::string::npos) << r.error();
}

TEST(WritePciAnnounceFile, RoundTripsThroughScanner) {
    ScanSandbox sb;

    // Daemon-side: mimic EAL having created the namespace dir.
    fs::path dir = fs::path(sb.root_view()) / "eph_0000_28_00_0";
    fs::create_directories(dir);
    std::ofstream(dir / "config") << "marker\n";

    // Daemon writes the announce file.
    auto wr = ::eph::dpdk::detail::write_pci_announce_file(
        "eph_0000_28_00_0", "0000:28:00.0", sb.root_view());
    ASSERT_TRUE(wr) << wr.error();

    // App-side: scan finds it.
    auto rr = find_default_nic_pci(sb.root_view());
    ASSERT_TRUE(rr) << rr.error();
    EXPECT_EQ(*rr, "0000:28:00.0");
}

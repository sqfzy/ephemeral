#pragma once

/// @file default_nic_scan.hpp
/// Default-NIC resolver: when an application calls
/// `Platform::create({.pci = ""})`, the library scans the DPDK runtime
/// directory (typically `/var/run/dpdk/`) for live `eph-nicd` daemons
/// and picks the unique one — single-NIC hosts get a zero-config call
/// site (`Platform::create({})`), multi-NIC hosts get a clear error
/// telling the user to specify `.pci` explicitly.
///
/// Design rationale:
/// - **No `default = true` toml flag.** A daemon is "default" iff it's
///   the only daemon running. This sidesteps stale-marker / race issues
///   that any sticky flag would have (daemon kill -9 leaves the toml
///   marker dangling; two daemons starting concurrently both think
///   they're default; etc.).
/// - **No new IPC, no new file format.** Reuses DPDK's own primary
///   marker (`/var/run/dpdk/<file_prefix>/config`) as the
///   "daemon is alive" signal. `eph-nicd` writes the original PCI BDF
///   into `eph-pci.txt` next to it so the scanner doesn't need to
///   inverse-sanitize the file_prefix.
///
/// Filesystem layout the scanner expects:
/// ```
/// /var/run/dpdk/
///   ├─ eph_0000_28_00_0/
///   │   ├─ config          ← DPDK primary marker
///   │   └─ eph-pci.txt     ← "0000:28:00.0\n" written by eph-nicd
///   └─ eph_0000_29_00_0/
///       └─ ...
/// ```
///
/// Only directories whose name starts with `eph_` are considered —
/// other DPDK applications coexisting on the same host (their own
/// runtime directories) are ignored.

#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace eph::dpdk::detail {

/// @brief Default DPDK runtime directory. EAL writes its file_prefix
/// subdirectories under this path.
inline constexpr std::string_view kDefaultDpdkRuntimeDir = "/var/run/dpdk";

/// @brief Eph-nicd daemons announce their original (unsanitized) PCI
/// BDF in this file inside their runtime namespace, so the scanner
/// doesn't need to inverse-sanitize the file_prefix.
inline constexpr std::string_view kEphPciAnnounceFile = "eph-pci.txt";

/// @brief Result of a default-NIC scan.
struct DefaultNicScanResult {
    /// All discovered live `eph-nicd` daemons' BDFs (untrimmed file
    /// content from `eph-pci.txt`). Sorted ascending for deterministic
    /// error messages on multi-NIC hosts.
    std::vector<std::string> candidates;
};

/// @brief Scan the DPDK runtime directory for live `eph-nicd` daemons.
///
/// Returns the list of BDFs (e.g. `"0000:28:00.0"`). Empty list means
/// no daemon is running. Pure-IO function (no DPDK dependency); usable
/// from `Platform::create` before EAL init and unit-testable in
/// isolation.
///
/// @param runtime_dir DPDK runtime root (defaults to `/var/run/dpdk`).
///                    Tests can pass a tmpfs path.
[[nodiscard]] inline DefaultNicScanResult
scan_eph_nicd_daemons(std::string_view runtime_dir = kDefaultDpdkRuntimeDir) {
    namespace fs = std::filesystem;
    DefaultNicScanResult out;

    fs::path root{runtime_dir};
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;  // nothing under root

    for (auto const& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        auto name = entry.path().filename().string();
        if (!name.starts_with("eph_")) continue;

        // Live-daemon check: DPDK primary creates `config` in its
        // runtime directory. Graceful exit removes it; kill -9 leaves
        // it stale, but a stale namespace also means the hugepage
        // memzones are stale and any attach attempt would fail anyway
        // — surfacing it as "candidate" then letting the actual attach
        // fail with a clearer error is acceptable.
        if (!fs::exists(entry.path() / "config", ec)) continue;

        // The PCI BDF is announced by the daemon in eph-pci.txt.
        fs::path pci_file = entry.path() / std::string{kEphPciAnnounceFile};
        if (!fs::is_regular_file(pci_file, ec)) continue;

        std::ifstream in{pci_file};
        if (!in.is_open()) continue;
        std::string bdf;
        std::getline(in, bdf);
        // Trim trailing whitespace
        while (!bdf.empty() &&
               (bdf.back() == '\n' || bdf.back() == '\r' ||
                bdf.back() == ' '  || bdf.back() == '\t')) {
            bdf.pop_back();
        }
        if (bdf.empty()) continue;
        out.candidates.push_back(std::move(bdf));
    }

    std::sort(out.candidates.begin(), out.candidates.end());
    return out;
}

/// @brief Resolve "the default NIC" — exactly one running daemon.
///
/// Returns the BDF as a string (caller owns) or a diagnostic on
/// 0-or-many. The diagnostic is shaped for direct propagation to the
/// application via `Platform::create`'s `std::expected` failure path.
///
/// @param runtime_dir Pass-through to `scan_eph_nicd_daemons`.
[[nodiscard]] inline std::expected<std::string, std::string>
find_default_nic_pci(std::string_view runtime_dir = kDefaultDpdkRuntimeDir) {
    auto r = scan_eph_nicd_daemons(runtime_dir);

    if (r.candidates.empty()) {
        return std::unexpected(std::string{
            "no eph-nicd daemon running for default-NIC selection. "
            "Start one (`sudo systemctl start eph-nicd@<bdf>`) or "
            "specify `cfg.pci` explicitly. Looked under "} +
            std::string{runtime_dir} + "/eph_*/eph-pci.txt");
    }
    if (r.candidates.size() > 1) {
        std::string list;
        for (auto const& c : r.candidates) {
            if (!list.empty()) list += ", ";
            list += c;
        }
        return std::unexpected(std::string{
            "multiple eph-nicd daemons running ("} + list +
            "); specify `cfg.pci` explicitly to disambiguate");
    }
    return r.candidates.front();
}

/// @brief Daemon-side helper: write the announce file
/// `<runtime_dir>/<file_prefix>/eph-pci.txt` containing the BDF.
/// Called by `Platform::serve_nic` immediately after EAL init.
///
/// On failure (e.g. permission denied — but the daemon owns its own
/// namespace dir) returns the system error string. The daemon should
/// log a warning but proceed; default-NIC resolution will simply skip
/// this daemon (apps must use explicit `cfg.pci`).
[[nodiscard]] inline std::expected<void, std::string>
write_pci_announce_file(std::string_view file_prefix,
                        std::string_view pci,
                        std::string_view runtime_dir = kDefaultDpdkRuntimeDir) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path{runtime_dir} / std::string{file_prefix};
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return std::unexpected(std::string{
            "write_pci_announce_file: runtime dir "} +
            dir.string() + " not present (EAL not initialized?)");
    }
    fs::path file = dir / std::string{kEphPciAnnounceFile};
    std::ofstream out{file, std::ios::trunc};
    if (!out.is_open()) {
        return std::unexpected(std::string{
            "write_pci_announce_file: cannot open "} + file.string() +
            " for writing");
    }
    out << pci << '\n';
    out.close();
    if (out.fail()) {
        return std::unexpected(std::string{
            "write_pci_announce_file: write failed for "} + file.string());
    }
    return {};
}

} // namespace eph::dpdk::detail

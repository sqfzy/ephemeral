#pragma once

/// @file lcore_pin.hpp
/// EAL lcore × cpu pin integration — declarative LcorePin spec, pure-function
/// `--lcores` argv builder, and (in stage 5) RAII-managed registration into
/// `eph::utils`'s process-wide pin registry.
///
/// Why a typed spec instead of writing `EalConfig::lcores` strings directly?
/// DPDK's `--lcores` syntax has a long tail (`(0-3)@(8,9)` set-of-sets) that
/// is a footgun to parse correctly. By making the caller fill structured
/// `LcorePin{lcore_id, cpu_id, role}` values, we avoid writing a parser, get
/// pre-EAL validation against the same registry that `pin_thread` consults,
/// and feed `role` into conflict diagnostics (`"cpu 4 already occupied by
/// lcore-0(rx-worker)"`).
///
/// Scope: 1:1 lcore→cpu mapping. Set-of-sets / coremask / service-core
/// (`-s`) syntaxes are out of scope — fall back to the raw escape hatch via
/// `EalConfig::lcores` / `EalConfig::extra_args` if you need them, with the
/// understanding that the raw path does not participate in registry-aware
/// validation.

#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "eph/dpdk/detail/logger.hpp"
#include "eph/utils/cpu.hpp"  // CpuPinPolicy, register_external_pin, validate_pin_policy

namespace eph::dpdk {

namespace detail {
inline spdlog::logger* lcore_pin_logger() {
    return get_logger<LoggerName{"dpdk.lcore_pin"}>();
}
} // namespace detail

/// @brief Declarative spec for "EAL lcore N runs pinned on physical cpu M".
///
/// The fields are deliberately POD so the caller can construct vectors /
/// arrays at compile time (`constexpr std::array pins = { LcorePin{...},
/// ... };`). `role` is a free-form short label (e.g. `"rx-worker"`,
/// `"tx-shaper"`) used purely for diagnostic messages — keep it under
/// ~20 characters for readable conflict errors.
///
/// Validation (cpu_id >= 0, no duplicate lcore_id, no duplicate cpu_id,
/// SMT/NUMA/IRQ checks) is the job of `register_lcore_pins` (stage 5);
/// `build_lcore_argv` itself does *no* semantic checking.
struct LcorePin {
    std::uint16_t lcore_id;   ///< EAL-visible logical core id (0..RTE_MAX_LCORE-1)
    int           cpu_id;     ///< Physical cpu id EAL will bind the lcore to
    std::string   role;       ///< Short diagnostic label; empty allowed
};

/// @brief Serialize a LcorePin span into a single EAL `--lcores=...` argv token.
///
/// Output shape (DPDK-recommended modern syntax):
///
/// @code
///   build_lcore_argv({}) ==                         ""
///   build_lcore_argv({{0,4,"rx"}}) ==              "--lcores=0@4"
///   build_lcore_argv({{0,4,"rx"},{1,5,"tx"}}) ==   "--lcores=0@4,1@5"
/// @endcode
///
/// The result is a single token suitable for appending to the EAL argv
/// vector via `EalConfig::extra_args.push_back(build_lcore_argv(pins))`.
///
/// @note Pure function: no validation, no registry side effects, no logging.
///       `lcore_id` collisions / negative `cpu_id` / 1:N mappings are not
///       detected here — `register_lcore_pins` is the validation gate.
///
/// @param pins  span of LcorePin entries (any size, including zero)
/// @return single argv token, or empty string if `pins` is empty
[[nodiscard]] inline std::string
build_lcore_argv(std::span<LcorePin const> pins) {
    if (pins.empty()) return {};
    std::string out;
    out.reserve(12 + pins.size() * 8);  // "--lcores=" + ~8 chars per entry
    out.append("--lcores=");
    bool first = true;
    for (auto const& p : pins) {
        if (!first) out.push_back(',');
        first = false;
        out.append(std::to_string(p.lcore_id));
        out.push_back('@');
        out.append(std::to_string(p.cpu_id));
    }
    return out;
}

// ──────────────────────────────────────────────────────────────────────
// register_lcore_pins + RegisteredLcoreGuard
// ──────────────────────────────────────────────────────────────────────

/// @brief Move-only RAII guard for a batch of `register_external_pin` writes.
///
/// Holds the cpu ids that `register_lcore_pins` successfully registered;
/// destruction (or move-from) unregisters them in one sweep. This lets
/// the caller treat lcore registration as a transaction: either every
/// requested cpu is in the registry, or none of them are. Specifically
/// covers the EalGuard::init_with_pins path (stage 6) where registration
/// must roll back if `rte_eal_init` fails after we already touched the
/// registry.
///
/// Move semantics: a moved-from guard owns nothing and its destructor
/// is a no-op. `release()` is the explicit equivalent — the only intended
/// caller is `EalGuard::init_with_pins` after a successful EAL init,
/// where the guard's responsibility transfers into the EalGuard's
/// owned `pin_guard_` field.
class RegisteredLcoreGuard {
public:
    RegisteredLcoreGuard() noexcept = default;

    ~RegisteredLcoreGuard() noexcept {
        for (int cpu : registered_cpus_) {
            eph::utils::unregister_external_pin(cpu);
        }
    }

    RegisteredLcoreGuard(RegisteredLcoreGuard&& other) noexcept
        : registered_cpus_(std::move(other.registered_cpus_)) {
        other.registered_cpus_.clear();
    }

    RegisteredLcoreGuard& operator=(RegisteredLcoreGuard&& other) noexcept {
        if (this != &other) {
            for (int cpu : registered_cpus_) {
                eph::utils::unregister_external_pin(cpu);
            }
            registered_cpus_ = std::move(other.registered_cpus_);
            other.registered_cpus_.clear();
        }
        return *this;
    }

    RegisteredLcoreGuard(RegisteredLcoreGuard const&)            = delete;
    RegisteredLcoreGuard& operator=(RegisteredLcoreGuard const&) = delete;

    /// Number of cpus this guard would unregister on destruction.
    [[nodiscard]] std::size_t size() const noexcept { return registered_cpus_.size(); }

    /// Empty? (moved-from / default-constructed)
    [[nodiscard]] bool empty() const noexcept { return registered_cpus_.empty(); }

    /// Read-only view of the cpus this guard owns. Useful for diagnostics
    /// and test verification; do NOT mutate via this view.
    [[nodiscard]] std::span<int const> registered_cpus() const noexcept {
        return registered_cpus_;
    }

    /// Relinquish ownership without unregistering anything. After release,
    /// destruction is a no-op. Intended for EalGuard::init_with_pins to
    /// transfer ownership; general callers should prefer move semantics.
    void release() noexcept { registered_cpus_.clear(); }

private:
    friend std::expected<RegisteredLcoreGuard, std::string>
    register_lcore_pins(std::span<LcorePin const>, eph::utils::CpuPinPolicy);

    explicit RegisteredLcoreGuard(std::vector<int>&& cpus) noexcept
        : registered_cpus_(std::move(cpus)) {}

    std::vector<int> registered_cpus_;
};

/// @brief Pre-EAL: validate every LcorePin under @p policy, then register
///        their cpus into the process-wide pin registry.
///
/// Transaction semantics:
///   - If every pin passes `eph::utils::detail::validate_pin_policy`
///     (isolcpus / SMT sibling / NUMA / IRQ checks per `policy`) **and**
///     every cpu is registerable (not already in the registry), the
///     returned guard owns all of them and destruction releases them.
///   - If any check or registration fails, every cpu staged so far is
///     rolled back via `unregister_external_pin`, and the function
///     returns `unexpected` with a message naming the offending pin.
///
/// The role label written into the registry is `"lcore-{lcore_id}({role})"`,
/// so subsequent `pin_thread` conflict errors read `cpu N already pinned by
/// lcore-0(rx-worker)`.
///
/// @param pins    span of LcorePin entries; empty span returns an empty guard
/// @param policy  validation strictness (default: relaxed, matches CpuPinPolicy{})
/// @return guard on full success; unexpected with diagnostic on first failure
[[nodiscard]] inline std::expected<RegisteredLcoreGuard, std::string>
register_lcore_pins(std::span<LcorePin const> pins,
                    eph::utils::CpuPinPolicy policy = {}) {
    [[maybe_unused]] auto* log = detail::lcore_pin_logger();
    std::vector<int> staged;
    staged.reserve(pins.size());

    auto rollback = [&staged]() noexcept {
        for (int c : staged) eph::utils::unregister_external_pin(c);
        staged.clear();
    };

    for (std::size_t i = 0; i < pins.size(); ++i) {
        auto const& p = pins[i];

        if (p.cpu_id < 0) {
            rollback();
            return std::unexpected(std::format(
                "register_lcore_pins: pin[{}] (lcore={}) has invalid cpu_id={}",
                i, p.lcore_id, p.cpu_id));
        }

        // Same four checks pin_thread runs (isolcpus / SMT sibling / NUMA /
        // IRQ-warn). Reuses the helper so policy semantics stay coherent
        // across both registrars.
        if (auto v = eph::utils::detail::validate_pin_policy(p.cpu_id, policy);
            !v) {
            rollback();
            return std::unexpected(std::format(
                "register_lcore_pins: pin[{}] (lcore={},cpu={}): {}",
                i, p.lcore_id, p.cpu_id, v.error()));
        }

        std::string role = std::format("lcore-{}({})", p.lcore_id, p.role);
        if (auto r = eph::utils::register_external_pin(p.cpu_id, std::move(role));
            !r) {
            rollback();
            return std::unexpected(std::format(
                "register_lcore_pins: pin[{}] (lcore={},cpu={}): {}",
                i, p.lcore_id, p.cpu_id, r.error()));
        }
        staged.push_back(p.cpu_id);
    }

    SPDLOG_LOGGER_DEBUG(log,
        "register_lcore_pins: registered {} lcore(s)", staged.size());
    return RegisteredLcoreGuard{std::move(staged)};
}

} // namespace eph::dpdk

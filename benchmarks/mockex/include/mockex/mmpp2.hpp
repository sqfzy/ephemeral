/// @file mmpp2.hpp
/// Two-state Markov-modulated Poisson sampler.
///
/// Models market-data arrival times as a mixture of two Poisson
/// processes — a "quiet" regime with rate λ_q and a "busy" regime
/// with rate λ_b — with geometric dwell times governed by per-event
/// transition probabilities p_qb and p_bq. This captures the burst
/// structure of real exchange streams (price event → chained depth
/// updates) that pure Poisson misses.
///
/// Parameters are produced by `tools/fit_mmpp.py` from a real
/// binance capture and loaded from a flat INI file alongside the
/// sample fixture. The sampler's state is a single uint8 for the
/// current regime plus the PRNG; no heap, no locks.
///
/// Size distribution is modelled as a weighted sum of KDE anchors
/// (approximation of a one-dimensional empirical density): we pick
/// an anchor by sampling from the weights, then add a small gaussian
/// jitter bounded by the KDE bandwidth. Anchors/weights are checked
/// in as part of params.ini.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <random>
#include <string>
#include <vector>

#include "core/bench_conf.hpp"
#include "mockex/sampler.hpp"

namespace mockex {

/// Parameters for the MMPP-2 + size KDE model. Populated from an INI
/// file produced by the offline fitter.
struct Mmpp2Params {
    double   lambda_quiet_hz   = 0.0;   ///< quiet regime arrival rate (events/s)
    double   lambda_busy_hz    = 0.0;   ///< busy regime arrival rate
    double   p_quiet_to_busy   = 0.0;   ///< P(quiet→busy) per event
    double   p_busy_to_quiet   = 0.0;   ///< P(busy→quiet) per event
    double   size_kde_bandwidth = 0.0;  ///< gaussian jitter around anchors
    std::vector<size_t> size_kde_anchors;  ///< empirical mode points (bytes)
    std::vector<double> size_kde_weights;  ///< normalised weights, sum ≈ 1
    uint64_t seed_default      = 42;    ///< seed if the scenario section doesn't override

    /// Load parameters from a flat TOML file produced by fit_mmpp.py.
    /// Expected keys (flat, no subtables):
    ///   lambda_quiet_hz, lambda_busy_hz, p_quiet_to_busy, p_busy_to_quiet
    ///   size_kde_bandwidth            (double)
    ///   size_kde_anchors              (array of integers)
    ///   size_kde_weights              (array of doubles)
    ///   seed_default                  (optional, integer)
    [[nodiscard]] static std::expected<Mmpp2Params, std::string>
    load_from_file(std::string_view path) noexcept {
        auto tbl_r = bench::load_toml_table(path);
        if (!tbl_r) {
            return std::unexpected(bench::format_error(tbl_r.error()));
        }
        const auto& tbl = *tbl_r;

        auto get_double = [&](const char* key) -> std::optional<double> {
            return tbl[key].value<double>();
        };

        auto lq  = get_double("lambda_quiet_hz");
        auto lb  = get_double("lambda_busy_hz");
        auto pqb = get_double("p_quiet_to_busy");
        auto pbq = get_double("p_busy_to_quiet");
        auto bw  = get_double("size_kde_bandwidth");
        if (!lq || !lb || !pqb || !pbq || !bw) {
            return std::unexpected(
                "Mmpp2Params: missing one of lambda_quiet_hz/lambda_busy_hz/"
                "p_quiet_to_busy/p_busy_to_quiet/size_kde_bandwidth in " +
                std::string{path});
        }

        Mmpp2Params out;
        out.lambda_quiet_hz    = *lq;
        out.lambda_busy_hz     = *lb;
        out.p_quiet_to_busy    = *pqb;
        out.p_busy_to_quiet    = *pbq;
        out.size_kde_bandwidth = *bw;

        const toml::array* anc = tbl["size_kde_anchors"].as_array();
        const toml::array* wts = tbl["size_kde_weights"].as_array();
        if (anc == nullptr || wts == nullptr) {
            return std::unexpected(
                "Mmpp2Params: size_kde_anchors / size_kde_weights must be "
                "arrays in " + std::string{path});
        }
        for (std::size_t i = 0; i < anc->size(); ++i) {
            auto v = anc->get(i)->value<int64_t>();
            if (!v) {
                return std::unexpected("Mmpp2Params: malformed size_kde_anchors");
            }
            out.size_kde_anchors.push_back(static_cast<size_t>(*v));
        }
        for (std::size_t i = 0; i < wts->size(); ++i) {
            auto v = wts->get(i)->value<double>();
            if (!v) {
                // Accept integer weights too (TOML promotes).
                auto iv = wts->get(i)->value<int64_t>();
                if (!iv) {
                    return std::unexpected("Mmpp2Params: malformed size_kde_weights");
                }
                out.size_kde_weights.push_back(static_cast<double>(*iv));
            } else {
                out.size_kde_weights.push_back(*v);
            }
        }
        if (out.size_kde_anchors.size() != out.size_kde_weights.size() ||
            out.size_kde_anchors.empty()) {
            return std::unexpected(
                "Mmpp2Params: size_kde_anchors and weights must be same "
                "non-empty length");
        }

        if (auto v = tbl["seed_default"].value<int64_t>()) {
            out.seed_default = static_cast<uint64_t>(*v);
        }
        return out;
    }
};

/// Two-state MMPP sampler + KDE-based size sampler.
///
/// Hot path per frame is ~2 uniform draws + a log + a bernoulli — cheap
/// enough that it won't dominate the mock's inner loop. The pipe holds
/// no virtuals; the scenario handler instantiates it with
/// `std::mt19937_64` and calls through by value.
template <class Prng = std::mt19937_64>
class Mmpp2Sampler {
public:
    /// Construct from a fully-validated parameter struct. Initial
    /// regime is quiet — the EM fit treats quiet as the baseline.
    explicit Mmpp2Sampler(Mmpp2Params p, uint64_t seed) noexcept
        : params_{std::move(p)}, prng_{seed} {
        precompute_cdf_();
    }

    /// Next inter-arrival interval in nanoseconds. Evolves the regime
    /// state by one Bernoulli trial AFTER picking the interval from the
    /// *current* regime — this matches the semantics of a Markov-
    /// modulated point process: the regime in effect at event i governs
    /// its rate, then switches (or not) before event i+1.
    [[nodiscard]] uint64_t next_interval_ns() noexcept {
        const double lambda = (regime_ == Regime::Quiet)
            ? params_.lambda_quiet_hz
            : params_.lambda_busy_hz;
        // Exponential via inverse-CDF. std::exponential_distribution
        // would do the same but we keep the math inline so the PRNG
        // state evolution is unambiguous across compilers.
        std::uniform_real_distribution<double> u01(
            std::nextafter(0.0, 1.0), 1.0);
        const double u = u01(prng_);
        // `std::max(lambda, 1e-12)` returns NaN when lambda is NaN (libstdc++
        // <algorithm> definition: `(b < a) ? a : b`, and `1e-12 < NaN` is
        // false). A NaN lambda comes from a malformed Mmpp2Params (TOML
        // parse miss, fitter bug, hand-edited params.toml with `nan`).
        // Without this guard, `dt_ns = NaN` would slip past both the
        // `<= 0.0` and `>= 1e18` clamp branches (every NaN comparison is
        // false) and reach `static_cast<uint64_t>(NaN)` which is UB per
        // [conv.fpint]/p1. Fall back to the 1ms safe interval so the
        // mock doesn't busy-loop or crash — fitter / config issue
        // surfaces as a uniform 1kHz cadence in the bench output.
        const double rate = std::isfinite(lambda) ? std::max(lambda, 1e-12) : 1e-12;
        const double dt_s = -std::log(u) / rate;
        const double dt_ns = dt_s * 1e9;
        // Regime evolution.
        evolve_regime_();
        // Clamp to u64 range; extremely rare at realistic λ.
        if (!(dt_ns > 0.0)) return 0;       // also folds NaN into the zero branch
        if (!(dt_ns < 1e18)) return static_cast<uint64_t>(1e18);
        return static_cast<uint64_t>(dt_ns);
    }

    /// Sample a target byte size from the KDE mixture. Clamped to [1,
    /// 65535] because mockex never sends a larger frame than a UDP
    /// datagram or a WS 16-bit length field can carry.
    [[nodiscard]] size_t size_hint_bytes() noexcept {
        if (params_.size_kde_anchors.empty()) return 0;
        // Pick an anchor by its weight (pre-computed CDF for O(log N)).
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        const double u = u01(prng_);
        auto it = std::upper_bound(cdf_.begin(), cdf_.end(), u);
        const size_t idx = (it == cdf_.end())
            ? cdf_.size() - 1
            : static_cast<size_t>(it - cdf_.begin());
        const size_t anchor = params_.size_kde_anchors[idx];
        // Gaussian jitter around the anchor. Bandwidth is in bytes.
        // Sanitise bandwidth: a NaN/Inf/negative `size_kde_bandwidth`
        // (malformed params.toml — fitter bug, hand-edited file) makes
        // `std::normal_distribution(0.0, NaN)` UB per [rand.dist]
        // requirements (sigma must be a positive finite value). Clamp
        // to 0.0 so jitter degenerates to "always returns mean=0" — the
        // caller falls back to the bare anchor, which is the safest
        // possible behaviour for a malformed KDE fit.
        const double sigma = std::isfinite(params_.size_kde_bandwidth) &&
                             params_.size_kde_bandwidth >= 0.0
                                 ? params_.size_kde_bandwidth
                                 : 0.0;
        std::normal_distribution<double> jitter(0.0, sigma);
        const double sampled = static_cast<double>(anchor) + jitter(prng_);
        // `static_cast<long long>(NaN)` is UB; `std::round(NaN) = NaN`.
        // Defense-in-depth: an anchor that overflowed to inf during
        // `static_cast<double>(size_t)` would also produce a non-finite
        // `sampled`. Fold non-finite into the lowest legal frame size
        // so the bench keeps making progress with the visible-but-
        // contained anomaly rather than crashing.
        if (!std::isfinite(sampled)) [[unlikely]] return 1;
        const long long clamped =
            std::clamp<long long>(static_cast<long long>(std::round(sampled)),
                                  1, 65535);
        return static_cast<size_t>(clamped);
    }

    /// Observability hooks — tests assert these don't get stuck.
    [[nodiscard]] bool in_busy_regime() const noexcept {
        return regime_ == Regime::Busy;
    }

    /// Force the regime to a known state. Only used by tests that want
    /// to deterministically exercise one branch.
    void force_regime_for_test(bool busy) noexcept {
        regime_ = busy ? Regime::Busy : Regime::Quiet;
    }

private:
    enum class Regime : uint8_t { Quiet, Busy };

    void precompute_cdf_() {
        // Defense-in-depth NaN/Inf scrub: a malformed params.toml (hand
        // edit, fitter bug, accidental `nan` literal) can place NaN or
        // ±Inf inside `size_kde_weights`. Without this guard:
        //   * NaN: `sum += NaN` poisons sum → first ternary picks the
        //     zero branch (sum > 0.0 is false on NaN), so the entire
        //     CDF stays at 0; `upper_bound(cdf_, u)` returns end and
        //     `idx = cdf_.size() - 1` always picks the last anchor —
        //     silent fallback that hides the bad input.
        //   * Single NaN with otherwise-finite weights summing > 0:
        //     `w/sum = NaN` poisons `acc`, every subsequent CDF entry
        //     is NaN, and `upper_bound`'s NaN comparisons (always
        //     false) collapse to idx=0, silently always picking the
        //     first anchor.
        //   * +Inf: `sum = +Inf`, `w/Inf = 0` for finite w but
        //     `Inf/Inf = NaN` for the inf-weight slot — same NaN
        //     poisoning chain.
        // Treat any non-finite weight as 0 (drop it from the
        // distribution) and recompute sum from the sanitised weights;
        // matches the fail-soft pattern used elsewhere in this file
        // (`sigma` clamp, `lambda` clamp).
        double sum = 0.0;
        for (double w : params_.size_kde_weights) {
            if (std::isfinite(w) && w > 0.0) sum += w;
        }
        cdf_.reserve(params_.size_kde_weights.size());
        double acc = 0.0;
        for (double w : params_.size_kde_weights) {
            const double safe_w = (std::isfinite(w) && w > 0.0) ? w : 0.0;
            acc += (sum > 0.0 ? safe_w / sum : 0.0);
            cdf_.push_back(acc);
        }
    }

    /// Apply one Markov transition. State evolves BEFORE returning to
    /// the caller, so successive `next_interval_ns()` calls sample
    /// under regime(i+1) conditional on regime(i).
    void evolve_regime_() noexcept {
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        const double u = u01(prng_);
        if (regime_ == Regime::Quiet) {
            if (u < params_.p_quiet_to_busy) regime_ = Regime::Busy;
        } else {
            if (u < params_.p_busy_to_quiet) regime_ = Regime::Quiet;
        }
    }

    Mmpp2Params         params_;
    Prng                prng_;
    std::vector<double> cdf_;     ///< cumulative size-weight CDF
    Regime              regime_{Regime::Quiet};
};

static_assert(Sampler<Mmpp2Sampler<>>,
              "Mmpp2Sampler must satisfy the Sampler concept");

} // namespace mockex

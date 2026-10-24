// Native port of std-t98-cracker series_solver.py / guided_ai.py (GPL-3.0).
// See docs/apps/std_t98_cracker.md for provenance and acceptance criteria.
#pragma once
#include "pn.hpp"
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace spear::std_t98::secret {
using Valid4 = std::array<bool, 4>;
inline constexpr Valid4 kAllValid{true, true, true, true};
struct SeriesKey {
    uint8_t series = 0;
    uint16_t key = 0;
    auto operator<=>(const SeriesKey&) const = default;
};
struct LinearCandidate {
    SeriesKey id;
    double score = 0;
};
class SeriesSession {
public:
    void add(const Burst& burst, const Valid4& valid = kAllValid);
    std::vector<LinearCandidate> candidates() const;
    double candidate_evidence(SeriesKey id) const;
    uint64_t pairs() const { return pairs_; }
private:
    std::array<double, 196> sums_{};
    std::optional<Frame49> previous_;
    uint64_t pairs_ = 0;
};

class GuidedSession {
public:
    using Block = std::array<Burst, 5>;
    // Returns plain logit - encrypted logit for each candidate, FP32.
    using Scorer = std::function<std::vector<float>(const Block&, std::span<const SeriesKey>)>;
    struct Result {
        std::optional<SeriesKey> accepted;
        double score = 0, margin = 0;
    };
    explicit GuidedSession(Scorer scorer, double threshold = 20, double margin = 6);
    void add(const Burst& burst, const Valid4& valid = kAllValid);
    Result assess();
    // Exposed to validate selection independently from the statistical solver.
    Result verify(std::span<const LinearCandidate> candidates);
    // Direct Hybrid validation of remembered keys; no linear solver ranking.
    Result verify_cached(std::span<const SeriesKey> candidates);
    bool ready() const { return blocks_.size() >= 2; }
    uint64_t evaluations() const { return evaluations_; }
    std::vector<uint64_t> block_starts() const;
private:
    Result verify_ids(std::span<const SeriesKey> selected);
    struct Segment {
        uint64_t start;
        Block data;
        std::map<SeriesKey, float> scores;
    };
    SeriesSession linear_;
    Scorer scorer_;
    double threshold_, margin_;
    uint64_t bursts_ = 0, evaluations_ = 0;
    Block pending_{};
    std::size_t pending_size_ = 0;
    std::deque<Segment> blocks_;
};
class Hybrid;
GuidedSession::Scorer hybrid_scorer(const Hybrid& hybrid);
} // namespace spear::std_t98::secret

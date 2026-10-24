#include "guided.hpp"
#include "hybrid.hpp"
#include "ambe/bits.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace spear::std_t98::secret {
namespace {
const auto& differences() {
    static const auto table = [] {
        std::array<std::array<uint16_t, 196>, 16> out{};
        for (std::size_t s = 0; s < 16; ++s) {
            std::array<uint16_t, 196> coefficients{};
            for (int i = 0; i < 15; ++i) coefficients[static_cast<std::size_t>(i)] = static_cast<uint16_t>(1u << i);
            for (std::size_t i = 15; i < 196; ++i)
                for (std::size_t b = 0; b < 15; ++b)
                    if (kSeriesMasks[s] & (1u << b)) coefficients[i] ^= coefficients[i - 15 + b];
            for (std::size_t phase = 0; phase < 4; ++phase)
                for (std::size_t bit = 0; bit < 49; ++bit) {
                    const auto mapped = static_cast<std::size_t>(ambe::kThumbDv[bit]);
                    out[s][phase * 49 + bit] = coefficients[phase * 49 + mapped] ^ coefficients[((phase + 3) % 4) * 49 + mapped];
                }
        }
        return out;
    }();
    return table;
}
double evidence(double score, double count) {
    const double p = std::clamp(.5 + score / (2 * count), .5, 1 - 1e-12);
    return std::max(0., count * (1 + p * std::log2(p) + (1-p) * std::log2(1-p)) - 19);
}
}
void SeriesSession::add(const Burst& burst, const Valid4& valid) {
    for (std::size_t phase = 0; phase < 4; ++phase) {
        if (valid[phase] && previous_) {
            for (std::size_t bit = 0; bit < 49; ++bit)
                sums_[phase * 49 + bit] += burst[phase][bit] == (*previous_)[bit] ? 1 : -1;
            ++pairs_;
        }
        if (valid[phase]) previous_ = burst[phase];
        else previous_.reset();
    }
}
double SeriesSession::candidate_evidence(SeriesKey id) const {
    if (!pairs_) return 0;
    const auto pn = pn_sequence(id.key, id.series);
    double score = 0;
    for (std::size_t phase = 0; phase < 4; ++phase)
        for (std::size_t bit = 0; bit < 49; ++bit) {
            const auto mapped = static_cast<std::size_t>(ambe::kThumbDv[bit]);
            score += sums_[phase*49+bit] * (pn[phase*49+mapped] == pn[((phase+3)%4)*49+mapped] ? 1 : -1);
        }
    return evidence(score, static_cast<double>(pairs_)*49);
}
std::vector<LinearCandidate> SeriesSession::candidates() const {
    if (!pairs_) return {};
    // Maintain only the global top five; the transform scratch buffer is shared
    // across series, and no full key/PN matrix is allocated per channel.
    struct Ranked { double value; unsigned index; };
    std::vector<Ranked> best;
    auto before = [](const Ranked& a, const Ranked& b) {
        return a.value > b.value || (a.value == b.value && a.index < b.index);
    };
    std::vector<double> spectrum(32768);
    for (unsigned s = 0; s < 16; ++s) {
        std::fill(spectrum.begin(), spectrum.end(), 0.);
        for (std::size_t i = 0; i < 196; ++i) spectrum[differences()[s][i]] += sums_[i];
        for (std::size_t width = 1; width < spectrum.size(); width *= 2)
            for (std::size_t start = 0; start < spectrum.size(); start += 2 * width)
                for (std::size_t i = 0; i < width; ++i) {
                    const double left = spectrum[start + i], right = spectrum[start + width + i];
                    spectrum[start + i] = left + right;
                    spectrum[start + width + i] = left - right;
                }
        for (unsigned state = s ? 1u : 0u; state < 32768; ++state) {
            Ranked r{spectrum[state], s * 32768 + state};
            if (best.size() == 5 && !before(r, best.back())) continue;
            best.insert(std::lower_bound(best.begin(), best.end(), r, before), r);
            if (best.size() > 5) best.pop_back();
        }
    }
    std::vector<LinearCandidate> out;
    for (const auto& r : best) {
        const auto series = static_cast<uint8_t>(r.index / 32768);
        out.push_back({{series, key_for_state(static_cast<uint16_t>(r.index % 32768), series)}, evidence(r.value, static_cast<double>(pairs_) * 49)});
    }
    return out;
}
GuidedSession::GuidedSession(Scorer scorer, double threshold, double margin)
    : scorer_(std::move(scorer)), threshold_(threshold), margin_(margin) {
    if (!scorer_ || !std::isfinite(threshold) || threshold <= 0 || !std::isfinite(margin) || margin < 0)
        throw std::invalid_argument("invalid guided AI scorer/threshold/margin");
}
void GuidedSession::add(const Burst& burst, const Valid4& valid) {
    linear_.add(burst, valid);
    ++bursts_;
    if (!std::all_of(valid.begin(), valid.end(), [](bool v) { return v; })) {
        pending_size_ = 0;
        return;
    }
    pending_[pending_size_++] = burst;
    if (pending_size_ == 5) {
        blocks_.push_back({bursts_ - 5, pending_, {}});
        pending_size_ = 0;
        if (blocks_.size() > 3) blocks_.pop_front();
    }
}
GuidedSession::Result GuidedSession::assess() {
    if (blocks_.size() < 2) return {};
    return verify(linear_.candidates());
}
GuidedSession::Result GuidedSession::verify(std::span<const LinearCandidate> candidates) {
    std::vector<SeriesKey> selected;
    for (std::size_t i = 0; i < std::min<std::size_t>(5, candidates.size()); ++i)
        if (candidates[i].score >= threshold_) selected.push_back(candidates[i].id);
    return verify_ids(selected);
}
GuidedSession::Result GuidedSession::verify_cached(std::span<const SeriesKey> candidates) {
    auto result = verify_ids(candidates);
    // Classification alone can mistake a wrong key for speech, especially on
    // repeated silence. Validate only the winning known key's PN consistency;
    // this does not run the full-series transform or propose any new keys.
    if (result.accepted && linear_.candidate_evidence(*result.accepted) < threshold_) result.accepted.reset();
    return result;
}
GuidedSession::Result GuidedSession::verify_ids(std::span<const SeriesKey> selected) {
    Result result;
    if (blocks_.size() < 2 || selected.empty()) return result;
    for (auto& block : blocks_) {
        // Retain only the keys in this pass (cache or solver shortlist).
        std::erase_if(block.scores, [&](const auto& entry) {
            return std::find(selected.begin(), selected.end(), entry.first) == selected.end();
        });
        std::vector<SeriesKey> missing;
        for (auto id : selected) if (!block.scores.contains(id)) missing.push_back(id);
        if (missing.empty()) continue;
        // Bound temporary model buffers even with many remembered codes.
        for (std::size_t start = 0; start < missing.size(); start += 32) {
            auto batch = std::span<const SeriesKey>(missing).subspan(start, std::min<std::size_t>(32, missing.size()-start));
            const auto scores = scorer_(block.data, batch);
            if (scores.size() != batch.size()) throw std::runtime_error("hybrid: wrong score count");
            for (std::size_t i = 0; i < scores.size(); ++i) {
                if (!std::isfinite(scores[i])) throw std::runtime_error("hybrid: non-finite logits");
                block.scores[batch[i]] = scores[i];
            }
            evaluations_ += scores.size();
        }
    }
    struct Ranked { SeriesKey id; double mean; unsigned positive; };
    std::vector<Ranked> ranking;
    for (auto id : selected) {
        Ranked r{id, 0, 0};
        for (const auto& block : blocks_) {
            const double score = block.scores.at(id);
            r.mean += score;
            r.positive += score > 0 ? 1u : 0u;
        }
        r.mean /= static_cast<double>(blocks_.size());
        ranking.push_back(r);
    }
    std::stable_sort(ranking.begin(), ranking.end(), [](const Ranked& a, const Ranked& b) {
        return a.mean > b.mean || (a.mean == b.mean && a.id.key < b.id.key);
    });
    const auto& top = ranking.front();
    result.score = top.mean;
    result.margin = top.mean - (ranking.size() > 1 ? std::max(0., ranking[1].mean) : 0.);
    if (top.mean > 0 && result.margin >= margin_ && top.positive >= 2) result.accepted = top.id;
    return result;
}
std::vector<uint64_t> GuidedSession::block_starts() const {
    std::vector<uint64_t> starts;
    for (const auto& block : blocks_) starts.push_back(block.start);
    return starts;
}
GuidedSession::Scorer hybrid_scorer(const Hybrid& hybrid) {
    return [&hybrid](const GuidedSession::Block& block, std::span<const SeriesKey> ids) {
        std::vector<float> input(ids.size() * 980), logits(ids.size() * 2), margins(ids.size());
        for (std::size_t n = 0; n < ids.size(); ++n) {
            const auto pn = pn_sequence(ids[n].key, ids[n].series);
            for (std::size_t b = 0; b < 5; ++b)
                for (int phase = 0; phase < 4; ++phase) {
                    const auto ks = keystream_thumbdv(pn, phase);
                    for (std::size_t bit = 0; bit < 49; ++bit)
                        input[n * 980 + b * 196 + static_cast<std::size_t>(phase) * 49 + bit] =
                            static_cast<float>(block[b][static_cast<std::size_t>(phase)][bit] ^ ks[bit]);
                }
        }
        std::string error;
        if (!hybrid.logits(input.data(), ids.size(), logits.data(), &error)) throw std::runtime_error(error);
        for (std::size_t n = 0; n < ids.size(); ++n) margins[n] = logits[n * 2] - logits[n * 2 + 1];
        return margins;
    };
}
} // namespace spear::std_t98::secret

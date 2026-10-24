#include "sync_correlator.hpp"

#include <algorithm>

namespace spear::std_t98 {

SyncCorrelator::SyncCorrelator(double ratio, double locked_ratio, int max_misses, int packet_len)
    : locked_ratio_(locked_ratio), max_misses_(max_misses), packet_len_(packet_len) {
    for (float s : kSyncWord) energy_ += s * s;
    set_threshold_ratio(ratio);
    locked_threshold_ = energy_ * locked_ratio_;
    packet_.reserve(static_cast<std::size_t>(packet_len_));
}

void SyncCorrelator::set_threshold_ratio(double ratio) {
    threshold_ = energy_ * ratio;
    st_.threshold = threshold_;
}

double SyncCorrelator::sse_at(uint64_t abs) const {
    double sse = 0;
    for (std::size_t k = 0; k < kSyncWord.size(); ++k) {
        const double d = buf_[static_cast<std::size_t>(abs - base_) + k] - kSyncWord[k];
        sse += d * d;
    }
    return sse;
}

void SyncCorrelator::process(std::span<const float> symbols, uint64_t first_symbol_index, const CandidateFn& on_candidate, const ResolveFn& on_resolve) {
    if (!have_base_) { base_ = pos_ = first_symbol_index; have_base_ = true; }
    buf_.insert(buf_.end(), symbols.begin(), symbols.end());
    while (step(on_candidate, on_resolve)) {}
    trim();
}

bool SyncCorrelator::step(const CandidateFn& on_candidate, const ResolveFn& on_resolve) {
    const auto len = static_cast<uint64_t>(packet_len_);
    if (state_ == State::Search) {
        if (!have(pos_ + kSyncWord.size())) return false;
        const double sse = sse_at(pos_);
        st_.best_metric = std::min(st_.best_metric, sse);
        if (sse > threshold_) { ++pos_; return true; }
        if (!have(pos_ + len)) return false;   // 192 シンボルが揃うまで待つ(pos_ は進めない)
        ++st_.detections;
        candidate(pos_, sse, true, on_candidate, on_resolve);
        return true;
    }
    // TENTATIVE / LOCKED: 予測位置 ±1 シンボル(symbol sync の 1 シンボルの滑りを拾う)。同点なら予測位置
    if (!have(next_ + len + 1)) return false;
    const double thr = state_ == State::Locked ? locked_threshold_ : threshold_;
    double best = sse_at(next_);
    uint64_t start = next_;
    for (const uint64_t s : {next_ - 1, next_ + 1}) {
        const double v = sse_at(s);
        if (v < best) { best = v; start = s; }
    }
    st_.best_metric = std::min(st_.best_metric, best);
    const bool sync_ok = best <= thr;
    candidate(sync_ok ? start : next_, sync_ok ? best : sse_at(next_), sync_ok, on_candidate, on_resolve);
    return true;
}

void SyncCorrelator::candidate(uint64_t start, double sse, bool sync_ok, const CandidateFn& on_candidate, const ResolveFn& on_resolve) {
    const auto off = static_cast<std::ptrdiff_t>(start - base_);
    packet_.assign(buf_.begin() + off, buf_.begin() + off + packet_len_);
    st_.last_metric = sse;
    const bool crc_ok = on_candidate({packet_, start, sse, sync_ok});
    const bool good = sync_ok || crc_ok;
    const auto len = static_cast<uint64_t>(packet_len_);
    switch (state_) {
    case State::Search:
        next_ = start + len;
        if (crc_ok) {
            state_ = State::Locked; ++st_.locks; misses_ = 0;
            on_resolve(start, true);
        } else {
            state_ = State::Tentative;
            held_.push_back({start, sync_ok});
        }
        break;
    case State::Tentative:
        // 確認は予測位置の同期語(厳しい閾値)だけ。CRC だけでは確認しない — SACCH の CRC は 6 bit で、雑音の偽同期の次の
        // 予測位置で 1/64 の確率で通る(2026-09-20 の録音の空きチャネルで 3 回)
        if (sync_ok) {
            next_ = start + len;
            state_ = State::Locked; ++st_.locks; misses_ = 0;
            resolve_held(true, on_resolve);
            on_resolve(start, true);
        } else {   // 偽同期だった: 保留とこの候補を捨て、保留した同期語の次のシンボルから探し直す
            const uint64_t first = held_.front().start;
            held_.push_back({start, sync_ok});
            resolve_held(false, on_resolve);
            state_ = State::Search;
            pos_ = first + 1;
        }
        break;
    case State::Locked:
        next_ = start + len;
        if (good) {
            resolve_held(true, on_resolve);
            misses_ = 0;
            if (!sync_ok) ++st_.flywheel;
            on_resolve(start, true);
        } else {
            held_.push_back({start, sync_ok});
            if (++misses_ >= max_misses_) {   // 同期喪失: 保留を捨て、最初の保留の位置から探し直す(次の呼の先頭を逃さない)
                const uint64_t first = held_.front().start;
                resolve_held(false, on_resolve);
                state_ = State::Search;
                pos_ = first;
                misses_ = 0;
            }
        }
        break;
    }
}

void SyncCorrelator::resolve_held(bool emit, const ResolveFn& on_resolve) {
    for (const auto& h : held_) {
        if (!emit) ++st_.dropped;
        else if (!h.sync_ok) ++st_.flywheel;
        on_resolve(h.start, emit);
    }
    held_.clear();
}

void SyncCorrelator::trim() {
    uint64_t keep = state_ == State::Search ? pos_ : next_ - 1;
    if (!held_.empty()) keep = std::min(keep, held_.front().start);
    if (keep <= base_) return;
    const auto n = std::min<uint64_t>(keep - base_, buf_.size());
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
    base_ += n;
}

} // namespace spear::std_t98

// フレーム同期(同期語検出 + 同期の状態機械)。
// 同期語の検出は ../std-t98-tools/core/rf/sync_word_correlator.py の SSE モード(直近 10 シンボルと同期語の二乗誤差 ≤ energy × ratio)。
// その上に状態機械を置く(2026-10-16、JR TRAIN RADIO の Synchronizer から。弱い局が途切れる対策):
//   SEARCH    — 1 シンボルずつ厳しい閾値(sync_ratio)で探す。見つけたら 192 シンボルを候補にする
//   TENTATIVE — 候補 1 つ(CRC 不良)を保留。次の予測位置(+192)で同期語(厳しい閾値)が取れれば LOCKED、取れなければ
//               保留を捨て、保留した候補の次のシンボルから探し直す(雑音の偽同期を出さず、偽同期の陰の本物も逃さない)
//   LOCKED    — 毎フレーム予測位置 ±1 シンボルだけを緩い閾値(locked_ratio)で見る。同期語が崩れていても予測位置で候補を作り、
//               本物かどうかは CRC に任せる(フライホイール)。同期語か CRC が取れたフレームは「良」。良でないフレームは保留し、
//               次の良で出す。max_misses 回続けて良でなければ同期喪失 — 保留を捨て、最初の保留の位置から探し直す
// CRC だけで「良」とするのは LOCKED の間だけ(SACCH の CRC は 6 bit なので、雑音から捕捉する根拠には弱い)。SEARCH で同期語と
// CRC が揃えば即 LOCKED にする(同期語と CRC の両方が通るのは信号の確かな証拠)。
// 候補の CRC は呼び出し側(受信機)が判定する: on_candidate が CRC の可否を返し、on_resolve が各候補を出す / 捨てるを
// 候補の順に 1 回ずつ通知する。電力は関与しない — フレームを出すかどうかは同期と CRC だけで決める。
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <vector>

namespace spear::std_t98 {

inline constexpr std::array<float, 10> kSyncWord = {-3, +1, -3, +3, -3, -3, +3, +3, -1, +3};

struct SyncStats {
    uint64_t detections = 0;    // SEARCH での同期語検出(厳しい閾値)
    uint64_t locks = 0;         // LOCKED に入った回数
    uint64_t flywheel = 0;      // 同期語が崩れていたが出したフレーム(LOCKED の予測位置、CRC か後続の良で確認)
    uint64_t dropped = 0;       // 捨てた候補(TENTATIVE の偽同期、同期喪失前の保留)
    double last_metric = 0;     // 直近の候補の SSE
    double best_metric = 1e9;   // これまでの最小 SSE(閾値にどれだけ近づいたか)
    double threshold = 0;       // SEARCH の閾値
};

struct SyncCandidate {
    std::span<const float> symbols;   // 192
    uint64_t first_symbol_index = 0;  // 同期語先頭(provenance)
    double sse = 0;
    bool sync_ok = false;             // 同期語が閾値内(SEARCH / TENTATIVE は厳しい、LOCKED は緩い)
};

class SyncCorrelator {
public:
    enum class State { Search, Tentative, Locked };
    using CandidateFn = std::function<bool(const SyncCandidate&)>;        // 戻り値: CRC OK
    using ResolveFn = std::function<void(uint64_t first_symbol_index, bool emit)>;

    explicit SyncCorrelator(double ratio = 0.2, double locked_ratio = 0.6, int max_misses = 3, int packet_len = 192);
    void set_threshold_ratio(double ratio);
    void process(std::span<const float> symbols, uint64_t first_symbol_index, const CandidateFn& on_candidate, const ResolveFn& on_resolve);
    const SyncStats& stats() const { return st_; }
    State state() const { return state_; }
    bool locked() const { return state_ == State::Locked; }

private:
    double sse_at(uint64_t abs) const;                         // buf_ 上の abs から 10 シンボルの SSE
    bool have(uint64_t abs_end) const { return abs_end <= base_ + buf_.size(); }
    bool step(const CandidateFn& on_candidate, const ResolveFn& on_resolve);   // 1 つ進める。入力待ちなら false
    void candidate(uint64_t start, double sse, bool sync_ok, const CandidateFn& on_candidate, const ResolveFn& on_resolve);
    void resolve_held(bool emit, const ResolveFn& on_resolve);
    void trim();

    double energy_ = 0, threshold_ = 0, locked_threshold_ = 0, locked_ratio_;
    int max_misses_, packet_len_;
    std::deque<float> buf_;
    uint64_t base_ = 0;              // buf_[0] の絶対シンボル番号
    bool have_base_ = false;
    uint64_t pos_ = 0;               // SEARCH: 次に調べる同期語先頭
    uint64_t next_ = 0;              // TENTATIVE / LOCKED: 予測した次の同期語先頭
    State state_ = State::Search;
    int misses_ = 0;
    struct Held { uint64_t start; bool sync_ok; };
    std::deque<Held> held_;          // 保留中の候補(古い順)
    std::vector<float> packet_;
    SyncStats st_;
};

} // namespace spear::std_t98

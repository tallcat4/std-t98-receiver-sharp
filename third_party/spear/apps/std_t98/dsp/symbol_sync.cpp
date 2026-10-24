#include "symbol_sync.hpp"

#include <algorithm>

namespace spear::std_t98 {

SymbolSync::SymbolSync(double sps, double loop_bw, double damping, double ted_gain, double max_dev)
    : nominal_(sps), min_p_(sps - max_dev), max_p_(sps + max_dev), avg_period_(sps), inst_period_(sps), ted_gain_(ted_gain) {
    // max_dev は GR symbol_sync_ff と同じ「samples/symbol の絶対値」(0.02 → 26.04 ± 0.02)。比率と誤解して ±0.5 sample にすると
    // 平均周期が誤った値に流れ着いて局所解から抜けられなくなる(実機で「一度ロックしないと永遠にアイが開かない」挙動)。
    // GNU Radio clock_tracking_loop の利得(loop_bw, damping → alpha, beta)
    const double theta = loop_bw / (damping + 1.0 / (4.0 * damping));
    const double d = 1.0 + 2.0 * damping * theta + theta * theta;
    alpha_ = 4.0 * damping * theta / d;
    beta_ = 4.0 * theta * theta / d;
    // TED 利得で正規化(GR: alpha/ted_gain, beta/ted_gain)
    alpha_ /= ted_gain_;
    beta_ /= ted_gain_;
    keep_ = static_cast<std::size_t>(std::ceil(sps * 2.0)) + 4;
    reset();
}

void SymbolSync::reset() {
    buf_.assign(keep_, 0.f);
    next_ = static_cast<double>(keep_);   // 最初のシンボル点は新規入力の先頭付近
    consumed_ = 0;
    avg_period_ = inst_period_ = nominal_;
    prev_sym_ = 0.f;
    last_err_ = 0;
}

float SymbolSync::interp(std::size_t base, double mu) const {
    const float a = buf_[base], b = base + 1 < buf_.size() ? buf_[base + 1] : buf_[base];
    return a + static_cast<float>(mu) * (b - a);
}

std::size_t SymbolSync::process(std::span<const float> in, std::vector<float>& out, std::vector<double>* positions) {
    buf_.insert(buf_.end(), in.begin(), in.end());
    std::size_t produced = 0;
    // 補間に +1 が要るので末尾 1 サンプル手前まで
    while (next_ + 1.0 < static_cast<double>(buf_.size())) {
        const double mid_t = next_ - inst_period_ / 2.0;
        if (mid_t < 0) { next_ += inst_period_; continue; }
        const auto ib = static_cast<std::size_t>(std::floor(next_));
        const float cur = interp(ib, next_ - static_cast<double>(ib));
        const auto mb = static_cast<std::size_t>(std::floor(mid_t));
        const float mid = interp(mb, mid_t - static_cast<double>(mb));
        // Gardner TED (real): e = (prev - cur) * mid
        double err = static_cast<double>((prev_sym_ - cur) * mid) * ted_gain_;
        err = std::clamp(err, -1.0, 1.0);
        last_err_ = err;
        // 2 次ループ(GR clock_tracking_loop 相当)
        // 積分項(平均周期)は nominal ± max_dev に固定。比例項(瞬時周期)は位相補正のため自由(GR と同じく ≤ 0 だけ防ぐ)
        avg_period_ = std::clamp(avg_period_ + beta_ * err, min_p_, max_p_);
        inst_period_ = avg_period_ + alpha_ * err;
        if (inst_period_ <= 0.0) inst_period_ = avg_period_;
        out.push_back(cur);
        if (positions) positions->push_back(static_cast<double>(consumed_) + next_ - static_cast<double>(keep_));
        ++produced;
        prev_sym_ = cur;
        next_ += inst_period_;
    }
    // 消費済みを捨てる(keep_ 分の履歴は残す)
    const double drop_d = std::max(0.0, next_ - static_cast<double>(keep_));
    const auto drop = static_cast<std::size_t>(std::floor(drop_d));
    if (drop > 0 && drop <= buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(drop));
        next_ -= static_cast<double>(drop);
        consumed_ += drop;
    }
    return produced;
}

} // namespace spear::std_t98

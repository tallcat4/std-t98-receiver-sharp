// S.P.E.A.R. dsp — PFB チャネライザ(共通 DSP: docs/dsp-boundary.md の基準を満たす)
//   * 数学的定義のみ(チャネル数・プロトタイプフィルタは引数)。用途の定数を含まない
//   * §1.3 が名指しする性能要衝。マルチチャネル App が使う
//   * 参照ベクトル(単一トーンの分離)で検証できる
//
// critically-sampled polyphase filter bank。M チャネル、各出力 rate = in_rate / M。
// bin c は中心からのオフセット c*(in_rate/M) のチャネル(c=0 が DC、上位 bin が負周波数)。
#pragma once

#include "stage.hpp"
#include "spear/core/types.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace spear::dsp {

class PfbChannelizer {
public:
    // prototype: 低域プロトタイプフィルタ(長さは内部で M の倍数にパディング)。
    PfbChannelizer(std::size_t num_channels, double in_rate, const std::vector<float>& prototype);
    ~PfbChannelizer();
    PfbChannelizer(const PfbChannelizer&) = delete;
    PfbChannelizer& operator=(const PfbChannelizer&) = delete;

    std::size_t channels() const { return m_; }
    double out_rate() const { return in_rate_ / static_cast<double>(m_); }
    StageInfo info() const;
    void reset();

    // in を処理し、チャネルごとの出力を outs[c] に追記する(outs.size() == channels())。
    // 入力は M サンプル単位で消費する(端数は次回へ持ち越し)。
    void process(std::span<const cf32> in, std::vector<std::vector<cf32>>& outs);

private:
    std::size_t m_;              // チャネル数
    double in_rate_;
    std::size_t taps_per_branch_;
    std::vector<std::vector<float>> branch_taps_;   // [m][t]
    std::vector<std::vector<cf32>> delay_;          // [m] 各 branch の遅延線(長さ taps_per_branch_)
    std::vector<cf32> carry_;                        // M 未満の端数入力
    void* fft_ = nullptr;                            // FFTW plan
    void* buf_ = nullptr;                            // M 点 in/out
    std::vector<float> proto_;
};

} // namespace spear::dsp

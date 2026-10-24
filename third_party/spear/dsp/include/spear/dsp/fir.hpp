// S.P.E.A.R. dsp — FIR 設計と decimating FIR(共通 DSP: docs/dsp-boundary.md の基準を満たす)
//   * 数学的定義のみ(係数・decimation 比は引数)。用途の定数を含まない
//   * channelizer / decimator は性能要衝 (§1.3)。全 App が使う
//   * 参照ベクトルで検証できる
// 段の規約 (stage.hpp) に従い、StageInfo(rate 比と群遅延)を宣言する。
#pragma once

#include "stage.hpp"
#include "spear/core/types.hpp"

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace spear::dsp {

// 窓関数法(Hamming)の低域 FIR。cutoff_hz / sample_rate は正規化前の値。taps は奇数に丸める。
std::vector<float> design_lowpass(double sample_rate, double cutoff_hz, std::size_t taps);

// FIR + 間引き。入力 in_rate、出力 in_rate / decim。状態(履歴)を持つ。
// T は cf32 または float。係数は実数。
template <class T>
class FirDecimator {
public:
    FirDecimator() = default;
    FirDecimator(std::string name, double in_rate, std::vector<float> taps, std::size_t decim);

    StageInfo info() const { return info_; }
    void reset();
    // in を処理し out に書く。out は in.size()/decim + 1 以上の容量が要る。戻り値 = 出力数。
    std::size_t process(std::span<const T> in, std::span<T> out);
    std::size_t decim() const { return decim_; }
    std::size_t taps() const { return taps_.size(); }

private:
    StageInfo info_;
    std::vector<float> taps_;
    std::vector<float> taps_rev_;   // 時間順(最古が先頭)
    std::vector<float> taps_kernel_; // 内積カーネル用(cf32 では re/im 用に 2 倍展開)。decim > 1 の経路で使う
    std::vector<T> buf_;            // 作業バッファ(呼び出しごとの確保を避ける)
    std::size_t decim_ = 1;
    std::vector<T> hist_;     // 直近 taps-1 サンプル(循環ではなく末尾保持)
    std::size_t phase_ = 0;   // 次の出力までに消費すべき入力数
};

extern template class FirDecimator<cf32>;
extern template class FirDecimator<float>;

// FIR 補間(zero-stuff + 低域 FIR を polyphase で)。出力 rate = in_rate * interp。
// taps は補間後 rate で設計した低域(利得は内部で interp 倍する)。
template <class T>
class FirInterpolator {
public:
    FirInterpolator() = default;
    FirInterpolator(std::string name, double in_rate, std::vector<float> taps, std::size_t interp);
    StageInfo info() const { return info_; }
    void reset();
    // out は in.size()*interp 以上の容量が要る。戻り値 = 出力数。
    std::size_t process(std::span<const T> in, std::span<T> out);
    std::size_t interp() const { return interp_; }
private:
    StageInfo info_;
    std::size_t interp_ = 1;
    std::vector<std::vector<float>> phase_taps_;   // [p][k] = taps[k*interp + p] * interp
    std::vector<std::vector<float>> phase_taps_rev_; // 同、時間順(カーネル用。cf32 では 2 倍展開)
    std::vector<T> buf_;
    std::vector<T> hist_;                           // 直近 taps_per_phase-1 入力
    std::size_t taps_per_phase_ = 0;
};
extern template class FirInterpolator<cf32>;
extern template class FirInterpolator<float>;

} // namespace spear::dsp

#include "spear/dsp/fir.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spear::dsp {
namespace {
// 内積カーネル。累算器を 8 本に分けて和の順序を固定し、-ffast-math なしでも自動ベクトル化できる形にする
// (実測: 520 tap の実数 FIR 30 本 @62.5 kHz が素朴なループの ~5 倍速)。taps は時間順(h[0] が最古)。
// 対象機(FZ-G2, i5-10310U)は AVX2/FMA を持つが、バイナリは既定の x86-64(SSE2)で組む。カーネルだけ target_clones で
// AVX2+FMA 版を併せて生成し、実行時に選ぶ(8 Msps × 47 tap の複素 FIR で SSE2 の約 3 倍)。FMA は丸めが 1 ulp 変わり得る。
// サニタイザ付きビルドでは使わない(ifunc の resolver が TSan の初期化前に走って起動時に落ちる)。
#if defined(_WIN32) || !defined(__GNUC__) || !defined(__x86_64__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define SPEAR_KERNEL_CLONES inline
#else
#define SPEAR_KERNEL_CLONES __attribute__((target_clones("avx2,fma", "default")))
#endif
SPEAR_KERNEL_CLONES
float dot(const float* x, const float* h, std::size_t n) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
    std::size_t k = 0;
    for (; k + 8 <= n; k += 8) {
        a0 += x[k] * h[k]; a1 += x[k + 1] * h[k + 1]; a2 += x[k + 2] * h[k + 2]; a3 += x[k + 3] * h[k + 3];
        a4 += x[k + 4] * h[k + 4]; a5 += x[k + 5] * h[k + 5]; a6 += x[k + 6] * h[k + 6]; a7 += x[k + 7] * h[k + 7];
    }
    for (; k < n; ++k) a0 += x[k] * h[k];
    return ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7));
}
// 複素 × 実。x を float 配列(re, im, re, im, …)として見て、係数を 2 倍に展開した h2(h2[2k] = h2[2k+1] = h[k])との内積を
// 8 本の累算器で取る(実数版と同じ形なので同じように自動ベクトル化される)。偶数番の累算器が実部、奇数番が虚部。
// 4 本ずつの複素ループ(以前の形)は re/im のインターリーブ読み出しがベクトル化されず、8 Msps × 47 tap で ~10 ns/sample かかっていた。
SPEAR_KERNEL_CLONES
cf32 dot(const cf32* x, const float* h2, std::size_t n) {
    const float* xf = reinterpret_cast<const float*>(x);
    const std::size_t n2 = 2 * n;
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
    std::size_t k = 0;
    for (; k + 8 <= n2; k += 8) {
        a0 += xf[k] * h2[k]; a1 += xf[k + 1] * h2[k + 1]; a2 += xf[k + 2] * h2[k + 2]; a3 += xf[k + 3] * h2[k + 3];
        a4 += xf[k + 4] * h2[k + 4]; a5 += xf[k + 5] * h2[k + 5]; a6 += xf[k + 6] * h2[k + 6]; a7 += xf[k + 7] * h2[k + 7];
    }
    for (; k < n2; k += 2) { a0 += xf[k] * h2[k]; a1 += xf[k + 1] * h2[k + 1]; }
    return {(a0 + a2) + (a4 + a6), (a1 + a3) + (a5 + a7)};
}
// y[m] += a · x[m](間引きなし FIR のタップ外側ループ用。restrict で別名なしを宣言し、自動ベクトル化させる)
SPEAR_KERNEL_CLONES
void axpy(float* __restrict y, const float* __restrict x, float a, std::size_t n) {
    for (std::size_t m = 0; m < n; ++m) y[m] += x[m] * a;
}
// T に応じたカーネル用の係数列(float: 時間順そのまま、cf32: 2 倍展開)
template <class T> std::vector<float> kernel_taps(const std::vector<float>& taps_rev);
template <> std::vector<float> kernel_taps<float>(const std::vector<float>& t) { return t; }
template <> std::vector<float> kernel_taps<cf32>(const std::vector<float>& t) {
    std::vector<float> h2(2 * t.size());
    for (std::size_t k = 0; k < t.size(); ++k) h2[2 * k] = h2[2 * k + 1] = t[k];
    return h2;
}
} // namespace

std::vector<float> design_lowpass(double sample_rate, double cutoff_hz, std::size_t taps) {
    if (taps % 2 == 0) ++taps;
    std::vector<float> h(taps);
    const double fc = cutoff_hz / sample_rate;           // 正規化 (cycles/sample)
    const double m = static_cast<double>(taps - 1) / 2.0;
    double sum = 0;
    for (std::size_t n = 0; n < taps; ++n) {
        const double k = static_cast<double>(n) - m;
        const double sinc = k == 0 ? 2.0 * fc : std::sin(2.0 * std::numbers::pi * fc * k) / (std::numbers::pi * k);
        const double w = 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(taps - 1));
        h[n] = static_cast<float>(sinc * w);
        sum += h[n];
    }
    for (auto& v : h) v = static_cast<float>(v / sum);   // DC 利得 1
    return h;
}

template <class T>
FirDecimator<T>::FirDecimator(std::string name, double in_rate, std::vector<float> taps, std::size_t decim)
    : taps_(std::move(taps)), decim_(decim == 0 ? 1 : decim) {
    taps_rev_.assign(taps_.rbegin(), taps_.rend());   // 畳み込み用に時間順(最古が先頭)
    taps_kernel_ = kernel_taps<T>(taps_rev_);
    info_.name = std::move(name);
    info_.in_rate = in_rate;
    info_.out_rate = in_rate / static_cast<double>(decim_);
    info_.group_delay_in = static_cast<double>(taps_.size() - 1) / 2.0;   // 線形位相 FIR
    reset();
}

template <class T>
void FirDecimator<T>::reset() {
    hist_.assign(taps_.size() > 0 ? taps_.size() - 1 : 0, T{});
    phase_ = 0;
}

// T を float の並びとして見たときの要素数(cf32 = 2)
template <class T> constexpr std::size_t floats_per = sizeof(T) / sizeof(float);

template <class T>
std::size_t FirDecimator<T>::process(std::span<const T> in, std::span<T> out) {
    // 作業バッファ: 履歴 + 入力 を連結して畳み込む
    const std::size_t nt = taps_.size();
    auto& buf = buf_;
    buf.clear();
    buf.reserve(hist_.size() + in.size());
    buf.insert(buf.end(), hist_.begin(), hist_.end());
    buf.insert(buf.end(), in.begin(), in.end());
    std::size_t produced = 0;
    const std::size_t nin = in.size();
    if (decim_ == 1) {
        // 間引きなし: タップ外側の形 out[i] += h[k] · x[i + k]。出力ごとの内積呼び出しより速い
        // (8 Msps × 47 tap で 1/4。内積は出力ごとの呼び出し・ベクトル化の前後処理が支配的で、タップ数によらず ~11 ns/出力かかる)。
        // T を float の並びとして axpy にする(cf32 は re/im とも同じ実係数)。
        produced = std::min(nin, out.size());
        constexpr std::size_t fp = floats_per<T>;
        float* of = reinterpret_cast<float*>(out.data());
        const float* bf = reinterpret_cast<const float*>(buf.data());
        const std::size_t nf = produced * fp;
        std::fill_n(of, nf, 0.f);
        // L1 に収まる塊(1024 float)ごとに全タップを回す(塊なしで全長を回すと L2 帯域に律速され 2 倍遅い)
        constexpr std::size_t kChunk = 1024;
        for (std::size_t c = 0; c < nf; c += kChunk) {
            const std::size_t len = std::min(kChunk, nf - c);
            for (std::size_t k = 0; k < nt; ++k) axpy(of + c, bf + c + k * fp, taps_rev_[k], len);
        }
        phase_ = 0;
    } else {
        // 出力 sample は入力位置 i(= buf[i + nt - 1] が最新)で計算。phase_ で間引き位相を保つ。
        const T* bp = buf.data();
        const float* h = taps_kernel_.data();
        T* op = out.data();
        const std::size_t cap = out.size(), decim = decim_;
        std::size_t i = phase_;
        for (; i < nin; i += decim) {
            if (produced < cap) op[produced++] = dot(bp + i, h, nt);
        }
        phase_ = i - nin;
    }
    // 履歴更新
    if (nt > 1) {
        const std::size_t keep = nt - 1;
        hist_.assign(buf.end() - static_cast<long>(keep), buf.end());
    }
    return produced;
}

template class FirDecimator<cf32>;
template class FirDecimator<float>;

template <class T>
FirInterpolator<T>::FirInterpolator(std::string name, double in_rate, std::vector<float> taps, std::size_t interp)
    : interp_(interp == 0 ? 1 : interp) {
    while (taps.size() % interp_ != 0) taps.push_back(0.f);
    taps_per_phase_ = taps.size() / interp_;
    phase_taps_.assign(interp_, std::vector<float>(taps_per_phase_, 0.f));
    for (std::size_t k = 0; k < taps_per_phase_; ++k)
        for (std::size_t p = 0; p < interp_; ++p)
            phase_taps_[p][k] = taps[k * interp_ + p] * static_cast<float>(interp_);
    phase_taps_rev_.resize(interp_);
    for (std::size_t p = 0; p < interp_; ++p) phase_taps_rev_[p] = kernel_taps<T>(std::vector<float>(phase_taps_[p].rbegin(), phase_taps_[p].rend()));   // カーネル形(cf32 は 2 倍展開)
    info_.name = std::move(name);
    info_.in_rate = in_rate;
    info_.out_rate = in_rate * static_cast<double>(interp_);
    // 群遅延: 補間後 sample で (N-1)/2 → 入力 sample では /interp
    info_.group_delay_in = static_cast<double>(taps.size() - 1) / 2.0 / static_cast<double>(interp_);
    reset();
}

template <class T>
void FirInterpolator<T>::reset() {
    hist_.assign(taps_per_phase_ > 0 ? taps_per_phase_ - 1 : 0, T{});
}

template <class T>
std::size_t FirInterpolator<T>::process(std::span<const T> in, std::span<T> out) {
    auto& buf = buf_;
    buf.clear();
    buf.reserve(hist_.size() + in.size());
    buf.insert(buf.end(), hist_.begin(), hist_.end());
    buf.insert(buf.end(), in.begin(), in.end());
    std::size_t produced = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const T* p = buf.data() + i;   // p[taps_per_phase_-1] が最新
        for (std::size_t ph = 0; ph < interp_; ++ph) {
            if (produced < out.size()) out[produced++] = dot(p, phase_taps_rev_[ph].data(), taps_per_phase_);
        }
    }
    if (taps_per_phase_ > 1) hist_.assign(buf.end() - static_cast<long>(taps_per_phase_ - 1), buf.end());
    return produced;
}

template class FirInterpolator<cf32>;
template class FirInterpolator<float>;

} // namespace spear::dsp

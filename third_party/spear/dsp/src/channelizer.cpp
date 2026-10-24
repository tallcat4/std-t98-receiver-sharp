#include "spear/dsp/channelizer.hpp"

#include <fftw3.h>

#include "spear/dsp/fftw_planner.hpp"

namespace spear::dsp {

using detail::fftw_planner_mutex;

PfbChannelizer::PfbChannelizer(std::size_t num_channels, double in_rate, const std::vector<float>& prototype)
    : m_(num_channels), in_rate_(in_rate), proto_(prototype) {
    // prototype を M の倍数へパディング
    while (proto_.size() % m_ != 0) proto_.push_back(0.f);
    taps_per_branch_ = proto_.size() / m_;
    // polyphase 分解: branch m の t 番目 tap = h[t*M + m]
    branch_taps_.assign(m_, std::vector<float>(taps_per_branch_, 0.f));
    for (std::size_t t = 0; t < taps_per_branch_; ++t)
        for (std::size_t m = 0; m < m_; ++m)
            branch_taps_[m][t] = proto_[t * m_ + m];
    delay_.assign(m_, std::vector<cf32>(taps_per_branch_, cf32{0.f, 0.f}));
    auto* buf = fftwf_alloc_complex(m_);
    buf_ = buf;
    {
        std::lock_guard lk(fftw_planner_mutex());
        // channelizer は IFFT(time->channel)。in-place。
        fft_ = fftwf_plan_dft_1d(static_cast<int>(m_), buf, buf, FFTW_BACKWARD, FFTW_MEASURE);
    }
}

PfbChannelizer::~PfbChannelizer() {
    std::lock_guard lk(fftw_planner_mutex());
    if (fft_) fftwf_destroy_plan(static_cast<fftwf_plan>(fft_));
    if (buf_) fftwf_free(static_cast<fftwf_complex*>(buf_));
}

StageInfo PfbChannelizer::info() const {
    StageInfo s;
    s.name = "pfb_channelizer";
    s.in_rate = in_rate_;
    s.out_rate = out_rate();
    s.group_delay_in = static_cast<double>(proto_.size() - 1) / 2.0;
    return s;
}

void PfbChannelizer::reset() {
    for (auto& d : delay_) std::fill(d.begin(), d.end(), cf32{0.f, 0.f});
    carry_.clear();
}

void PfbChannelizer::process(std::span<const cf32> in, std::vector<std::vector<cf32>>& outs) {
    if (outs.size() != m_) outs.assign(m_, {});
    // carry と in を連結して M サンプル単位で処理
    std::vector<cf32> data;
    data.reserve(carry_.size() + in.size());
    data.insert(data.end(), carry_.begin(), carry_.end());
    data.insert(data.end(), in.begin(), in.end());
    auto* buf = static_cast<fftwf_complex*>(buf_);

    std::size_t pos = 0;
    for (; pos + m_ <= data.size(); pos += m_) {
        // commutator: 入力 j を branch (M-1-j) へ。各 branch の遅延線を1つ進める。
        for (std::size_t j = 0; j < m_; ++j) {
            const std::size_t br = m_ - 1 - j;
            auto& dl = delay_[br];
            // shift(新しいサンプルを先頭へ)
            for (std::size_t t = taps_per_branch_ - 1; t > 0; --t) dl[t] = dl[t - 1];
            dl[0] = data[pos + j];
        }
        // 各 branch の FIR 出力を FFT バッファへ
        for (std::size_t m = 0; m < m_; ++m) {
            cf32 acc{0.f, 0.f};
            const auto& dl = delay_[m];
            const auto& tp = branch_taps_[m];
            for (std::size_t t = 0; t < taps_per_branch_; ++t) acc += dl[t] * tp[t];
            buf[m][0] = acc.real();
            buf[m][1] = acc.imag();
        }
        fftwf_execute(static_cast<fftwf_plan>(fft_));
        for (std::size_t c = 0; c < m_; ++c) outs[c].emplace_back(buf[c][0], buf[c][1]);
    }
    carry_.assign(data.begin() + static_cast<long>(pos), data.end());
}

} // namespace spear::dsp

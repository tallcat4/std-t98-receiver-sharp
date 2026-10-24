#pragma once
#include "spear/core/types.hpp"
#include "spear/dsp/stage.hpp"
#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

namespace t98 {
// SPEAR's polyphase prototype and commutator, evaluated only at bin 0.
// The caller has already mixed the selected RF channel to DC. For bin 0,
// every inverse-FFT twiddle is 1, so summing the branches gives exactly the
// same filter response without computing or allocating any other channels.
class CenterChannelizer {
    std::size_t m_, taps_;
    double rate_;
    std::vector<float> prototype_;
    std::vector<std::vector<float>> coefficients_;
    std::vector<std::vector<spear::cf32>> delay_;
    std::vector<spear::cf32> carry_;
public:
    CenterChannelizer(std::size_t decimation, double rate, std::vector<float> prototype)
        : m_(decimation), rate_(rate), prototype_(std::move(prototype)) {
        if (!m_ || prototype_.empty()) throw std::invalid_argument("Invalid channel filter");
        while (prototype_.size() % m_) prototype_.push_back(0.f);
        taps_ = prototype_.size() / m_;
        coefficients_.assign(m_, std::vector<float>(taps_));
        delay_.assign(m_, std::vector<spear::cf32>(taps_));
        for (std::size_t t = 0; t < taps_; ++t)
            for (std::size_t m = 0; m < m_; ++m) coefficients_[m][t] = prototype_[t*m_+m];
    }
    spear::dsp::StageInfo info() const {
        spear::dsp::StageInfo s;
        s.name = "selected_channel";
        s.in_rate = rate_;
        s.out_rate = rate_ / m_;
        s.group_delay_in = (prototype_.size()-1) / 2.0;
        return s;
    }
    void process(std::span<const spear::cf32> input, std::vector<std::vector<spear::cf32>>& output) {
        if (output.size() != 1) output.assign(1, {});
        std::vector<spear::cf32> data;
        data.reserve(carry_.size() + input.size());
        data.insert(data.end(), carry_.begin(), carry_.end());
        data.insert(data.end(), input.begin(), input.end());
        std::size_t pos = 0;
        for (; pos + m_ <= data.size(); pos += m_) {
            for (std::size_t j = 0; j < m_; ++j) {
                auto& d = delay_[m_-1-j];
                for (std::size_t t = taps_-1; t > 0; --t) d[t] = d[t-1];
                d[0] = data[pos+j];
            }
            spear::cf32 selected{};
            for (std::size_t m = 0; m < m_; ++m) {
                spear::cf32 branch{};
                for (std::size_t t = 0; t < taps_; ++t) branch += delay_[m][t] * coefficients_[m][t];
                selected += branch;
            }
            output[0].push_back(selected);
        }
        carry_.assign(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end());
    }
};
}

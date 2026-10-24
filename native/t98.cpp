#include "t98.h"
#include "receiver.hpp"
#include "blip25.h"
#include "secret/pn.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <numbers>
#include <stdexcept>

using namespace spear;
using namespace spear::std_t98;
namespace {
thread_local std::string error;
// Fixed phase bank, streaming windowed-sinc resampler. Retains history and
// absolute position across arbitrary SDR# callback boundaries.
class Resampler {
    static constexpr int taps = 64, phases = 1024;
    double step, next = 32;
    uint64_t base = 0;
    std::vector<cf32> history = std::vector<cf32>(32);
    std::vector<float> coefficients;
public:
    explicit Resampler(double rate) : step(rate / 50000.), coefficients(taps * phases) {
        double cutoff = std::min(0.45, 20000. / rate);
        for (int p = 0; p < phases; ++p) {
            double sum = 0;
            for (int k = 0; k < taps; ++k) {
                double x = k - 31 - double(p) / phases;
                double sinc = std::abs(x) < 1e-12 ? 2 * cutoff : std::sin(2 * std::numbers::pi * cutoff * x) / (std::numbers::pi * x);
                double w = .42 + .5 * std::cos(std::numbers::pi * x / 32) + .08 * std::cos(2 * std::numbers::pi * x / 32);
                coefficients[p*taps+k] = float(sinc*w); sum += sinc*w;
            }
            for (int k = 0; k < taps; ++k) coefficients[p*taps+k] /= float(sum);
        }
    }
    void process(std::span<const cf32> input, std::vector<cf32>& output) {
        history.insert(history.end(), input.begin(), input.end());
        output.clear();
        while (uint64_t(next) + 32 < base + history.size()) {
            auto center = uint64_t(next);
            int phase = std::min(phases-1, int((next-center)*phases));
            cf32 value{};
            for (int k = 0; k < taps; ++k) value += history[size_t(center-base-31+k)] * coefficients[phase*taps+k];
            output.push_back(value); next += step;
        }
        uint64_t retain = uint64_t(next) > 32 ? uint64_t(next)-32 : 0;
        size_t drop = size_t(std::min<uint64_t>(history.size(), retain-base));
        history.erase(history.begin(), history.begin()+drop); base += drop;
    }
};
struct Context {
    double rate, phase = 0, correction = 0;
    cf32 previous{};
    double frequency_sum=0;
    int frequency_count=0;
    uint64_t input = 0, output = 0;
    Resampler resampler;
    Receiver receiver;
    blip25_decoder voice{};
    std::vector<cf32> mixed, converted;
    std::deque<t98_frame> frames;
    std::deque<std::array<float,64>> eyes;
    uint64_t eye_count=0;
    static ReceiverConfig config() { ReceiverConfig c; c.in_rate=50000; c.pfb_channels=8; return c; }
    explicit Context(double r) : rate(r), resampler(r), receiver(config()) {
        blip25_init(&voice);
        Observer observer;
        observer.eye = [this](int, std::span<const float> values) {
            // 600 traces/sec preserves the eye's density without drawing all 2400 symbols.
            if((eye_count++%4)!=0) return;
            std::array<float,64> trace{}; std::copy_n(values.begin(), 64, trace.begin());
            eyes.push_back(trace); if (eyes.size()>256) eyes.pop_front();
        };
        observer.symbols = [this](int, std::span<const float> values) {
            // Decision-directed fine AFC only once frame synchronization is established.
            // Clamp corrections to the selected narrowband channel.
            if (!receiver.metrics(0).locked || values.empty()) return;
            double residual=0;
            for (float v : values) {
                float q = v < -2 ? -3 : v < 0 ? -1 : v < 2 ? 1 : 3;
                residual += std::clamp(double(v-q), -1., 1.);
            }
            correction = std::clamp(correction + residual / values.size() * 315 * .015, -1200.,1200.);
        };
        observer.frame = [this](const Frame& f) {
            if (!f.rich.valid) return;
            t98_frame event{};
            event.symbol=f.symbol_index; event.traffic=f.rich.f; event.mode=f.rich.m;
            event.crc=f.rich.f ? f.sacch.crc_ok : f.pich.crc_ok;
            event.user=f.sacch.user_code; event.maker=f.sacch.maker_code; event.call=f.sacch.call_stat;
            if (!f.rich.f && f.pich.crc_ok) std::strncpy(event.csm, f.pich.csm.c_str(), 15);
            if (f.tch_payload.size()==36) std::copy(f.tch_payload.begin(),f.tch_payload.end(),event.payload);
            if (f.tch_llr.size()==288) std::copy(f.tch_llr.begin(),f.tch_llr.end(),event.llr);
            if (frames.size()>=128) throw std::runtime_error("Frame output queue overflow");
            frames.push_back(event);
        };
        receiver.set_observer(std::move(observer));
    }
};
}
void* t98_create(double rate) {
    try {
        if (!std::isfinite(rate) || rate < 12000 || rate > 3200000) throw std::invalid_argument("IQ rate must be 12 kHz..3.2 MHz");
        return new Context(rate);
    } catch(const std::exception& e) { error=e.what(); return nullptr; }
}
void t98_destroy(void* h) { delete static_cast<Context*>(h); }
int t98_process(void* h, const float* iq, int count) {
    try {
        if (!h || !iq || count<0 || count>65536) throw std::invalid_argument("Invalid IQ block");
        auto& c=*static_cast<Context*>(h); c.mixed.resize(count);
        for(int i=0;i<count;++i) {
            if (!std::isfinite(iq[2*i]) || !std::isfinite(iq[2*i+1])) throw std::invalid_argument("Non-finite IQ sample");
            const cf32 raw(iq[2*i],iq[2*i+1]);
            const cf32 difference=raw*std::conj(c.previous);c.previous=raw;
            c.frequency_sum+=std::atan2(difference.imag(),difference.real())*c.rate/(2*std::numbers::pi);
            if(++c.frequency_count>=int(c.rate*.08)) {
                const double estimate=c.frequency_sum/c.frequency_count;
                if(!c.receiver.metrics(0).locked && std::abs(estimate)<1800)
                    c.correction=std::clamp(c.correction+.35*(estimate-c.correction),-1200.,1200.);
                c.frequency_sum=0;c.frequency_count=0;
            }
            c.mixed[i]=raw*std::polar(1.f,float(c.phase));
            c.phase=std::remainder(c.phase-2*std::numbers::pi*c.correction/c.rate,2*std::numbers::pi);
        }
        c.resampler.process(c.mixed,c.converted);
        c.receiver.process(c.converted,c.output); c.output+=c.converted.size(); c.input+=count;
        return 0;
    } catch(const std::exception& e) { error=e.what(); return -1; }
}
int t98_read(void* h,t98_frame* out) {
    if(!h || !out) return -1;
    auto& q=static_cast<Context*>(h)->frames;
    if(q.empty()) return 0;
    *out=q.front();q.pop_front();return 1;
}
int t98_status(void* h,t98_stats* s,float* eye,int capacity) {
    if(!h || !s || capacity<0) return -1;
    auto& c=*static_cast<Context*>(h);const auto& m=c.receiver.metrics(0);
    *s={c.input,m.frames,m.sacch_ok+m.pich_ok,m.power_db,c.correction,int(m.locked),int(m.active)};
    int n=std::min(int(c.eyes.size()),capacity/64);
    if(eye) for(int i=0;i<n;++i) std::copy(c.eyes[c.eyes.size()-n+i].begin(),c.eyes[c.eyes.size()-n+i].end(),eye+i*64);
    return n*64;
}
int t98_eye_read(void* h,float* eye,int capacity) {
    if(!h || !eye || capacity<64) return -1;
    auto& q=static_cast<Context*>(h)->eyes;
    int n=std::min(int(q.size()),capacity/64);
    for(int i=0;i<n;++i) {std::copy(q.front().begin(),q.front().end(),eye+i*64);q.pop_front();}
    return n*64;
}
const char* t98_error() { return error.c_str(); }
void t98_voice_reset(void* h) { if(h) blip25_init(&static_cast<Context*>(h)->voice); }
int t98_voice(void* h,const int8_t* llr,int series,int key,int16_t* pcm) {
    if(!h || !llr || !pcm || series<0 || series>15 || key<0 || key>32767) return -1;
    auto& c=*static_cast<Context*>(h);
    const auto pn=secret::pn_sequence(uint16_t(key),uint8_t(series));
    for(int f=0;f<4;++f) {
        uint8_t bits[49]{};
        int e=blip25_fec_3600_soft(llr+72*f,bits,nullptr);
        for(int b=0;b<49;++b) bits[b]^=pn[49*f+b];
        blip25_decode_bits(&c.voice,bits,e<0?BLIP25_LOST:0,pcm+160*f);
    }
    return 0;
}

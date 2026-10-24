#include "receiver.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>

namespace spear::std_t98 {

// ---- チャネルごとの状態 ----
struct Receiver::Channel {
    dsp::FirInterpolator<cf32> up;        // 6250 → 62500
    cf32 quad_prev{0.f, 0.f};
    float quad_gain = 1.f;
    dsp::FirDecimator<float> rrc;         // decim 1(単なる FIR)
    SymbolSync sync;
    SyncCorrelator corr;
    std::vector<cf32> iq_up;
    std::vector<float> disc, filt, syms;
    std::vector<double> sym_pos;          // 各シンボルの判定点(62.5 kHz 絶対 sample index)
    std::deque<std::pair<uint64_t, double>> pos_hist;   // (symbol_index, sample) 直近
    std::vector<float> eye_buf;           // フィルタ後の履歴(アイパターン用)、eye_base が eye_buf[0] の絶対 sample index
    uint64_t eye_base = 0, eye_fed = 0;   // eye_fed = これまで symbol sync に渡した総サンプル数(sym_pos と同じ座標)
    std::deque<double> eye_pending;       // トレース未生成の判定点
    std::deque<Frame> pending;            // 同期の状態機械が出す / 捨てるを決めるまでの復号済み候補(候補の順)
    uint64_t symbol_index = 0;            // 出力シンボルの通し番号
    uint64_t sample_index = 0;            // 62.5 kHz sample の通し番号(このチャネル)
    double unlocked_s = 1e9;              // 同期を失ってからの時間(活動の保持)
};

Receiver::Receiver(ReceiverConfig cfg) : cfg_(cfg) {
    if (cfg_.num_channels != 1) throw std::invalid_argument("This receiver supports exactly one selected channel");
    const double post1 = cfg_.pfb_channels * cfg_.spacing_hz;   // 300 kHz
    const auto decim1 = static_cast<std::size_t>(std::llround(cfg_.in_rate / post1));
    resamp1_ = std::make_unique<dsp::FirDecimator<cf32>>("resamp1", cfg_.in_rate,
        dsp::design_lowpass(cfg_.in_rate, post1 * 0.45, 63), decim1);
    // PFB プロトタイプ: cutoff = bin/2、遷移 = cutoff/2(std-t98 と同じ)。taps = 48 × 8
    const double bin = cfg_.spacing_hz;
    pfb_ = std::make_unique<t98::CenterChannelizer>(static_cast<std::size_t>(cfg_.pfb_channels), post1,
        dsp::design_lowpass(post1, bin / 2.0, static_cast<std::size_t>(cfg_.pfb_channels) * 8 - 1));
    // channel map(std-t98 build_channel_map): 下側 15 ch は上位 bin、中心 = bin 0、上側は bin 1..14
    const int neg = cfg_.num_channels / 2, pos = cfg_.num_channels - neg - 1;
    for (int i = 0; i < neg; ++i) channel_map_.push_back(cfg_.pfb_channels - neg + i);
    channel_map_.push_back(0);
    for (int i = 1; i <= pos; ++i) channel_map_.push_back(i);

    const double demod_rate = bin * cfg_.resamp2;     // 62500
    const double sps = demod_rate / cfg_.baud;        // 26.04
    rx_taps_ = make_rx_taps(demod_rate, cfg_.baud, 0.2, static_cast<int>(sps) * 20);
    const auto up_taps = dsp::design_lowpass(demod_rate, bin * 0.48, 10 * cfg_.resamp2 + 1);
    for (int c = 0; c < cfg_.num_channels; ++c) {
        auto ch = std::make_unique<Channel>();
        ch->up = dsp::FirInterpolator<cf32>("resamp2", bin, up_taps, static_cast<std::size_t>(cfg_.resamp2));
        ch->quad_gain = static_cast<float>(demod_rate / (2.0 * std::numbers::pi * cfg_.fsk_dev_hz));
        ch->rrc = dsp::FirDecimator<float>("rrc_rx", demod_rate, rx_taps_, 1);
        ch->sync = SymbolSync(sps, cfg_.sym_loop_bw, cfg_.sym_damping, cfg_.sym_ted_gain, cfg_.sym_max_dev);
        ch->corr = SyncCorrelator(cfg_.sync_ratio, cfg_.sync_locked_ratio, cfg_.sync_max_misses, kFrameSymbols);
        ch_.push_back(std::move(ch));
    }
    metrics_.assign(static_cast<std::size_t>(cfg_.num_channels), {});
    // provenance: 62.5 kHz sample → in index(resamp1 → pfb → up)
    prov_chan_ = dsp::Provenance::identity().then(resamp1_->info()).then(pfb_->info()).then(ch_[0]->up.info());
}

Receiver::~Receiver() = default;

int Receiver::channel_to_bin(int ch) const { return channel_map_[static_cast<std::size_t>(ch)]; }

void Receiver::set_sync_ratio(double r) {
    cfg_.sync_ratio = r;
    for (auto& c : ch_) c->corr.set_threshold_ratio(r);
}

std::size_t Receiver::taps_resamp1() const { return resamp1_->taps(); }

void Receiver::process(std::span<const cf32> in, uint64_t first_index) {
    if (!have_in0_) { in0_ = first_index; have_in0_ = true; }
    // 0) 個体の周波数誤差を打ち消す(ブロック内は連続位相、ブロック境界も rot_phase_ で連続)
    if (cfg_.freq_err_hz != 0) {
        rot_.resize(in.size());
        const double step = -2.0 * std::numbers::pi * cfg_.freq_err_hz / cfg_.in_rate;
        for (std::size_t i = 0; i < in.size(); ++i) {
            rot_[i] = in[i] * cf32(static_cast<float>(std::cos(rot_phase_)), static_cast<float>(std::sin(rot_phase_)));
            rot_phase_ += step;
            if (rot_phase_ > std::numbers::pi) rot_phase_ -= 2 * std::numbers::pi; else if (rot_phase_ < -std::numbers::pi) rot_phase_ += 2 * std::numbers::pi;
        }
        in = rot_;
    }
    // 1) resamp1
    buf1_.resize(in.size() / resamp1_->decim() + 2);
    const std::size_t n1 = resamp1_->process(in, buf1_);
    if (obs_.band) obs_.band(std::span<const cf32>(buf1_.data(), n1), band_index_);
    band_index_ += n1;
    // 2) PFB
    for (auto& b : bins_) b.clear();
    pfb_->process(std::span<const cf32>(buf1_.data(), n1), bins_);
    // 3) チャネルごと。復調・シンボル同期・フレーム同期は常に全チャネルで回す(止めたり 0 を入れたりしない)
    for (int c = 0; c < cfg_.num_channels; ++c) {
        auto& ch = *ch_[c];
        auto& m = metrics_[static_cast<std::size_t>(c)];
        const auto& bin = bins_[static_cast<std::size_t>(channel_to_bin(c))];
        if (bin.empty()) continue;
        double pw = 0;
        for (auto x : bin) pw += std::norm(x);
        m.power_db = 10.0 * std::log10(pw / static_cast<double>(bin.size()) + 1e-20);
        // 補間 ×10
        ch.iq_up.resize(bin.size() * static_cast<std::size_t>(cfg_.resamp2) + 2);
        const std::size_t n2 = ch.up.process(bin, ch.iq_up);
        const uint64_t iq_first = ch.sample_index;
        ch.sample_index += n2;
        if (obs_.channel_iq) obs_.channel_iq(c, std::span<const cf32>(ch.iq_up.data(), n2), iq_first);
        // 4) quadrature demod
        ch.disc.resize(n2);
        for (std::size_t i = 0; i < n2; ++i) {
            const cf32 x = ch.iq_up[i];
            const cf32 p = x * std::conj(ch.quad_prev);
            ch.quad_prev = x;
            ch.disc[i] = std::atan2(p.imag(), p.real()) * ch.quad_gain;
        }
        if (obs_.discriminator) obs_.discriminator(c, std::span<const float>(ch.disc.data(), n2));
        // 5) RRC rx + ×0.23
        ch.filt.resize(n2 + 1);
        const std::size_t n3 = ch.rrc.process(std::span<const float>(ch.disc.data(), n2), ch.filt);
        for (std::size_t i = 0; i < n3; ++i) ch.filt[i] *= cfg_.post_filt_gain;
        if (obs_.filtered) obs_.filtered(c, std::span<const float>(ch.filt.data(), n3));
        // 6) symbol sync + ×5
        ch.syms.clear();
        ch.sym_pos.clear();
        const std::size_t ns = ch.sync.process(std::span<const float>(ch.filt.data(), n3), ch.syms, &ch.sym_pos);
        for (auto& s : ch.syms) s *= cfg_.post_sync_gain;
        m.sps = ch.sync.period();
        // アイパターン: symbol sync の判定点 p を基準に [p − sps/2, p + 1.5 sps] の 2 シンボル幅を切り出す
        // (判定点がトレースの 1/4 と 3/4 に来る)。振幅はシンボルと同じ ±1/±3 スケール。履歴は常に進め、
        // トレースを出すのは活動中だけ(バッファを捨てると出し始めに先頭が欠ける)
        if (obs_.eye) {
            const double sps = ch.sync.period();
            if (ch.eye_buf.empty()) ch.eye_base = ch.eye_fed;
            ch.eye_buf.insert(ch.eye_buf.end(), ch.filt.begin(), ch.filt.begin() + static_cast<long>(n3));
            for (double p : ch.sym_pos) ch.eye_pending.push_back(p);
            std::vector<float> trace(static_cast<std::size_t>(cfg_.eye_points));
            const double avail = static_cast<double>(ch.eye_base + ch.eye_buf.size());
            while (!ch.eye_pending.empty()) {
                const double start = ch.eye_pending.front() - 0.5 * sps;
                if (start + 2.0 * sps + 1.0 >= avail) break;
                ch.eye_pending.pop_front();
                if (start < static_cast<double>(ch.eye_base)) continue;
                for (int k = 0; k < cfg_.eye_points; ++k) {
                    const double t = start - static_cast<double>(ch.eye_base) + 2.0 * sps * k / (cfg_.eye_points - 1);
                    const auto i0 = static_cast<std::size_t>(t);
                    const double mu = t - static_cast<double>(i0);
                    trace[static_cast<std::size_t>(k)] = (ch.eye_buf[i0] + static_cast<float>(mu) * (ch.eye_buf[i0 + 1] - ch.eye_buf[i0])) * cfg_.post_sync_gain;
                }
                if (m.active) obs_.eye(c, trace);
            }
            // 未処理の判定点より前は捨てる(4 シンボル分は残す)
            const double keep_from = (ch.eye_pending.empty() ? avail : ch.eye_pending.front()) - 2.0 * sps - 2.0;
            const auto drop = static_cast<std::size_t>(std::max(0.0, keep_from - static_cast<double>(ch.eye_base)));
            if (drop > 0 && drop <= ch.eye_buf.size()) { ch.eye_buf.erase(ch.eye_buf.begin(), ch.eye_buf.begin() + static_cast<long>(drop)); ch.eye_base += drop; }
        } else {
            ch.eye_buf.clear(); ch.eye_pending.clear();
        }
        ch.eye_fed += n3;
        if (obs_.symbols && ns) obs_.symbols(c, ch.syms);
        // 7) 同期語 → フレーム
        const uint64_t sym_first = ch.symbol_index;
        ch.symbol_index += ns;
        for (std::size_t i = 0; i < ns; ++i) ch.pos_hist.emplace_back(sym_first + i, ch.sym_pos[i]);
        while (ch.pos_hist.size() > 600) ch.pos_hist.pop_front();
        // フレームを出すかどうかは同期の状態機械と CRC だけで決める(dsp/sync_correlator.hpp)。
        // 候補はここで復号して CRC を返し、出す / 捨てるの通知(候補の順)を待って出す
        auto on_candidate = [&](const SyncCandidate& cand) {
            const uint64_t sym_idx = cand.first_symbol_index;
            Frame f;
            f.channel = c;
            f.symbol_index = sym_idx;
            f.sync_sse = cand.sse;
            f.sync_ok = cand.sync_ok;
            // provenance (§3.6): フレーム先頭シンボルの判定点(62.5 kHz sample)→ 段の宣言から radio.rx の index へ
            double ch_sample = static_cast<double>(sym_idx) * (ch.rrc.info().in_rate / cfg_.baud);
            for (const auto& [si, pos] : ch.pos_hist) if (si == sym_idx) { ch_sample = pos; break; }
            f.input_sample_index = in0_ + static_cast<uint64_t>(std::max(0.0, prov_chan_.input_index(ch_sample)));
            std::vector<float> raw(cand.symbols.begin(), cand.symbols.end());
            f.raw = quantize(raw);
            f.fields = parse_frame(dewhiten(f.raw));
            f.rich = decode_rich(f.fields.rich);
            bool crc_ok = false;
            if (f.rich.valid && f.rich.f == 0) {
                f.pich = decode_pich(f.fields.tch1);
                crc_ok = f.pich.crc_ok;
            } else if (f.rich.valid && f.rich.f == 1) {
                f.sacch = decode_sacch(f.fields.sacch);
                crc_ok = f.sacch.crc_ok;
                f.tch_payload = traffic_blocks_to_payload(split_traffic_blocks(f.fields));
                f.tch_llr = traffic_llr(raw);
            }
            ch.pending.push_back(std::move(f));
            return crc_ok;
        };
        auto on_resolve = [&](uint64_t, bool emit) {   // 候補の順に通知される(先頭 == その候補)
            Frame f = std::move(ch.pending.front());
            ch.pending.pop_front();
            if (!emit) return;
            m.frames++;
            if (f.rich.valid && f.rich.f == 0 && f.pich.crc_ok) { m.pich_ok++; m.csm = f.pich.csm; }
            if (f.rich.valid && f.rich.f == 1 && f.sacch.crc_ok) m.sacch_ok++;
            m.last_frame_symbol_index = f.symbol_index;
            if (obs_.frame) obs_.frame(f);
        };
        ch.corr.process(ch.syms, sym_first, on_candidate, on_resolve);
        m.locked = ch.corr.locked();
        // 活動 = 同期中、または同期を失って active_hold_s 以内(アイは次のブロックから。1 ブロック = 数 ms)
        ch.unlocked_s = m.locked ? 0 : ch.unlocked_s + static_cast<double>(bin.size()) / cfg_.spacing_hz;
        m.active = ch.unlocked_s < cfg_.active_hold_s;
        m.locks = ch.corr.stats().locks;
        m.flywheel_frames = ch.corr.stats().flywheel;
        m.dropped_frames = ch.corr.stats().dropped;
        m.sync_detections = ch.corr.stats().detections;
        m.best_sse = ch.corr.stats().best_metric;
    }
}

} // namespace spear::std_t98

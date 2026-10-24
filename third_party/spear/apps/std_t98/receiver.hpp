// SPEAR STD-T98 receiver adapted for exactly one selected channel.
// The CLI mixes that channel to DC; CenterChannelizer extracts only bin 0.
// Filter design, discriminator, symbol sync and frame sync remain SPEAR-derived.
#pragma once

#include "dsp/rrc.hpp"
#include "dsp/symbol_sync.hpp"
#include "dsp/sync_correlator.hpp"
#include "protocol/protocol.hpp"
#include "spear/core/types.hpp"
#include "center_channelizer.hpp"
#include "spear/dsp/fir.hpp"
#include "spear/dsp/stage.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace spear::std_t98 {

struct ReceiverConfig {
    double in_rate = 1.2e6;          // radio.rx(300 kHz の整数倍)
    int    pfb_channels = 48;
    int    num_channels = 1;             // This tool accepts exactly one selected channel.
    double spacing_hz = 6250;
    int    resamp2 = 10;             // 6250 → 62500
    double baud = 2400;
    double fsk_dev_hz = 315;         // 1 level あたり
    double freq_err_hz = 0;          // 入力を -freq_err 回転する量: 帯域中心を DC へ戻す分 + そのファイル固有の残留誤差(golden / --freq-err)。
                                     // 実機の個体誤差は Core の Radio が LO 側で打ち消す(radio.freq_err_ppm)ので App は足さない
    // チャネルの活動(表示・アイ・周波数誤差の推定)= フレーム同期中。同期を失っても active_hold_s の間は活動中とする
    // (送信の切れ目や同期喪失の判定で表示がちらつかないように。JR TRAIN RADIO の「直近 1 秒に同期」と同じ)
    double active_hold_s = 1.0;
    double sync_ratio = 0.2;         // 捕捉(SEARCH / TENTATIVE)の SSE 閾値 = 同期語エネルギー × ratio
    double sync_locked_ratio = 0.6;  // 同期中の予測位置 ±1 での SSE 閾値(緩い)
    int    sync_max_misses = 3;      // 同期語も CRC も取れないフレームがこれだけ続いたら同期喪失(3 × 80 ms)
    double sym_loop_bw = 0.06, sym_damping = 1.1, sym_ted_gain = 0.1, sym_max_dev = 0.02;   // GR symbol_sync_ff の引数そのまま(max_dev は samples/symbol)
    float  post_filt_gain = 0.23f, post_sync_gain = 5.f;
    int    eye_points = 64;          // アイパターン 1 トレース(2 シンボル)の点数
};

struct Frame {
    int      channel = 0;            // 0..29(ch1 = 351.20000 MHz)
    uint64_t symbol_index = 0;       // そのチャネルのシンボル通し番号(フレーム先頭)
    uint64_t input_sample_index = 0; // radio.rx の sample index に逆算した先頭(provenance §3.6)
    double   sync_sse = 0;
    bool     sync_ok = true;         // 同期語が閾値内。false = 同期中に予測位置で取ったフレーム(フライホイール、CRC か後続のフレームで確認済み)
    Symbols  raw;                    // 量子化済み 192 シンボル(dewhiten 前)
    FrameFields fields;              // dewhiten 後のフィールド(ビット文字列)
    Rich  rich;
    Sacch sacch;                     // rich.f == 1(トラフィック)のとき有効
    Pich  pich;                      // rich.f == 0(同期バースト)のとき有効
    std::vector<uint8_t> tch_payload; // トラフィック時: 4 ブロック × 9 バイト(AMBE 3600、硬判定)
    std::vector<int8_t>  tch_llr;     // トラフィック時: 4 ブロック × 72 の軟判定(traffic_llr、AMBE の FEC はこちらで復号する)
};

struct ChannelMetrics {
    double power_db = -999;          // 直近ブロックのチャネル電力(数値の表示だけ。判定には使わない)
    bool   locked = false;           // フレーム同期中(呼の有無はこれで決める)
    bool   active = false;           // 活動中 = 同期中か、同期を失って active_hold_s 以内(表示・アイ用)
    uint64_t locks = 0;              // 同期を捕捉した回数
    uint64_t flywheel_frames = 0;    // 同期語が崩れていたが出したフレーム
    uint64_t dropped_frames = 0;     // 捨てた候補(雑音の偽同期、同期喪失前の保留)
    double sps = 0;                  // symbol sync の推定周期
    uint64_t sync_detections = 0, frames = 0, sacch_ok = 0, pich_ok = 0;
    double best_sse = 1e9;
    uint64_t last_frame_symbol_index = 0;
    std::string csm;                 // 最後に CRC OK だった PICH の CSM
};

// 中間 stream の観測。全チャネル分呼ばれる(App が選択チャネルだけを publish する)。
struct Observer {
    std::function<void(std::span<const cf32> iq, uint64_t first_index)> band;                // resamp1 後(pfb_channels × spacing Hz)、全チャネル俯瞰用
    std::function<void(int ch, std::span<const cf32> iq, uint64_t first_index)> channel_iq;   // 62.5 kHz
    std::function<void(int ch, std::span<const float> disc)> discriminator;                   // 62.5 kHz
    std::function<void(int ch, std::span<const float> filtered)> filtered;                    // 62.5 kHz、×0.23 後
    std::function<void(int ch, std::span<const float> symbols)> symbols;                      // 2400/s
    std::function<void(int ch, std::span<const float> eye_trace)> eye;                        // eye_points 点 = 2 シンボル、判定点は 1/4・3/4、±1/±3 スケール
    std::function<void(const Frame&)> frame;
};

class Receiver {
public:
    explicit Receiver(ReceiverConfig cfg);
    ~Receiver();
    const ReceiverConfig& config() const { return cfg_; }
    // チャネル i の中心周波数オフセット(radio.rx 中心から)
    double channel_offset_hz(int ch) const { return (ch - cfg_.num_channels / 2) * cfg_.spacing_hz; }
    int channel_to_bin(int ch) const;

    void set_sync_ratio(double r);
    void set_observer(Observer o) { obs_ = std::move(o); }

    // 入力ブロック(cfg.in_rate)。first_index = radio.rx の sample index(provenance)
    void process(std::span<const cf32> in, uint64_t first_index);
    const ChannelMetrics& metrics(int ch) const { return metrics_[ch]; }
    std::size_t taps_resamp1() const;
    const dsp::Provenance& provenance_to_channel() const { return prov_chan_; }   // 62.5 kHz sample → in index
    double symbol_rate() const { return cfg_.baud; }
    double band_rate() const { return cfg_.pfb_channels * cfg_.spacing_hz; }

private:
    struct Channel;
    ReceiverConfig cfg_;
    Observer obs_;
    std::unique_ptr<dsp::FirDecimator<cf32>> resamp1_;
    std::unique_ptr<t98::CenterChannelizer> pfb_;
    std::vector<std::unique_ptr<Channel>> ch_;
    std::vector<ChannelMetrics> metrics_;
    std::vector<int> channel_map_;
    dsp::Provenance prov_chan_;
    std::vector<float> rx_taps_;
    std::vector<cf32> rot_;               // freq_err 補正後の入力
    double rot_phase_ = 0;
    std::vector<cf32> buf1_;
    std::vector<std::vector<cf32>> bins_;
    uint64_t band_index_ = 0;   // resamp1 出力の通し番号
    uint64_t in0_ = 0;          // 最初のブロックの radio.rx index(provenance の原点)
    bool have_in0_ = false;
};

} // namespace spear::std_t98

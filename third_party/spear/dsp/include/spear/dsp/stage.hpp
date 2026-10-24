// S.P.E.A.R. dsp — 段(stage)の規約 (docs/dsp-boundary.md)
//
// 共通 DSP が提供するのはアルゴリズムより「規約」である。App 内の DSP チェーンもこの規約で書けば、
//   * 入出力 rate と群遅延を宣言するので、provenance(§3.6: 出力 index → 入力 index 範囲)が機械的に求まる
//   * TAP() の置き場が段の境界に統一される
//   * 段単位で参照ベクトルのテストが書ける
// 規約であって基底クラスの強制ではない(テンプレートで扱う)。最低限:
//   struct MyStage {
//       using input_type = cf32; using output_type = float;
//       StageInfo info() const;                                   // rate 比と群遅延
//       std::size_t process(std::span<const input_type> in, std::span<output_type> out); // 生成した out 数を返す
//       void reset();
//   };
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace spear::dsp {

struct StageInfo {
    std::string name;
    double in_rate  = 0;       // 入力 sample rate [Hz]
    double out_rate = 0;       // 出力 sample rate [Hz]
    double group_delay_in = 0; // 群遅延 [入力 sample]。FIR なら (N-1)/2、IIR なら近似値を宣言
    double ratio() const { return in_rate > 0 ? out_rate / in_rate : 1.0; }
};

// provenance: 出力 index → 対応する入力 index(群遅延補正込み)。チェーン全体は各段の合成。
// 「rate 変換・group delay の補正は各 App 内で手計算し、根拠をコメントで残す」(§3.6) を、
// 段が宣言した数値から計算する形にする。手計算より間違えにくく、根拠が info() に残る。
// 段の時間規約: 出力 k は入力 [k·decim − (N−1), k·decim] から計算する(因果、FirDecimator::process)。
// 入力の事象は群遅延ぶん **遅れて** 出力に現れるので、出力 index → 入力 index は群遅延を **引く**。
// (ADS-B の合成 PPM テストがサンプル単位で確認する。以前は足していたため 2 × 群遅延ぶん遅い index を返していた)
struct Provenance {
    double scale = 1.0;   // 出力 index 1 あたりの入力 index
    double offset = 0.0;  // 入力 index の補正(群遅延の合計、入力 index 単位、負)

    static Provenance identity() { return {}; }
    // 後段 s を合成する(this = これまでのチェーン、s = 追加する段)
    Provenance then(const StageInfo& s) const {
        Provenance p;
        p.scale = scale / s.ratio();
        p.offset = offset - s.group_delay_in * scale;
        return p;
    }
    // チェーン出力 index → 元 stream の sample index(実数。切り出しは floor/ceil で幅を取る)
    double input_index(double output_index) const { return output_index * scale + offset; }
};

} // namespace spear::dsp

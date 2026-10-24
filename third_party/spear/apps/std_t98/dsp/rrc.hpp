// STD-T98 用 RRC 系フィルタ設計(../std-t98-tools/firdes.py の移植)
// 用途定数(α=0.2、逆 sinc 補償)を含むので App 内(docs/dsp-boundary.md)。
#pragma once
#include <vector>
namespace spear::std_t98 {
// which: "tx" = H·P(f)(送信整形)、"rx" = H/P(f)(受信、sinc 補償)
std::vector<float> design_taps(double fs, double sym_rate, double alpha, int ntaps, bool rx);
// ルートナイキスト(√H、sinc 補償なし)。送信整形(ARIB STD-T98 の規定)とテスト変調器に使う
std::vector<float> make_rrc_taps(double fs, double sym_rate = 2400.0, double alpha = 0.2, int ntaps = 257);
inline std::vector<float> make_rx_taps(double fs, double sym_rate = 2400.0, double alpha = 0.2, int ntaps = 257) { return design_taps(fs, sym_rate, alpha, ntaps, true); }
inline std::vector<float> make_tx_taps(double fs, double sym_rate = 2400.0, double alpha = 0.2, int ntaps = 257) { return design_taps(fs, sym_rate, alpha, ntaps, false); }
}

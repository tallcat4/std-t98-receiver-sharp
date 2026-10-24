// STD-T98 protocol decoder — 純 C++ 移植 (../std-t98-tools/core/protocol/)
//
// SDR にも GNU Radio にも Qt にも依存しない。決定的でテスト可能。
// フレーム: SW(20) RICH(16) SACCH(60) TCH1(144) TCH2(144) = 384 bit = 192 symbols(4値 FSK)。
// 参照: std-t98-tools の Python 実装。等価性は apps/std_t98/tests の golden test が保証する。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace spear::std_t98 {

// 4値シンボル: -3, -1, +1, +3(int8)
using Symbols = std::vector<int8_t>;

inline constexpr int kFrameSymbols = 192;   // packet_len
inline constexpr int kSwSymbols = 10;        // 同期語 10 シンボル(20 bit)

// float の軟判定シンボルを {-3,-1,+1,+3} に量子化
Symbols quantize(const std::vector<float>& raw);

// デホワイトニング(先頭 10 シンボル = SW はそのまま、以降 182 シンボルに LFSR 列を乗算)
Symbols dewhiten(const Symbols& sym);

// シンボル列 → ビット列("0"/"1" の文字列)。各シンボル 2 bit。
//   +1→00  +3→01  -1→10  -3→11
std::string symbols_to_bits(const Symbols& sym);

// フレームのフィールド分割(ビット文字列)
struct FrameFields {
    std::string sw, rich, sacch, tch1, tch2;
};
FrameFields split_frame(const std::string& bits);
FrameFields parse_frame(const Symbols& dewhitened);   // dewhiten 済みシンボル → フィールド

// ---- RICH (16 bit) ----
struct Rich {
    int f = 0, res1 = 0, res2 = 0, m = 0, d = 0, parity = 0;
    bool parity_ok = false;
    bool valid = false;   // 16 bit で pair が正しく取れたか
};
Rich decode_rich(const std::string& rich_bits);

// ---- SACCH (60 bit) / PICH (144 bit): 畳み込み符号 K=5 + CRC ----
struct Sacch {
    int f = 0, wr = 0, msg_type = 0, call_stat = 0, user_code = 0, maker_code = 0;
    bool crc_ok = false;
    int bit_errors = 0;
    std::string crc_recv, crc_calc;
};
Sacch decode_sacch(const std::string& sacch_bits);   // 60 bit

struct Pich {
    std::string csm;        // 9 hex 桁
    std::string reserve;    // 44 bit
    bool crc_ok = false;
    int bit_errors = 0;
    std::string crc_recv, crc_calc;
};
Pich decode_pich(const std::string& pich_bits);       // 144 bit

// ---- TCH: 音声ブロック(AMBE 3600)----
// TCH1/TCH2 (各 144 bit) を 4 つの 72-bit ブロックに分け、9 バイトの payload にする
std::array<std::string, 4> split_traffic_blocks(const FrameFields& f);
std::vector<uint8_t> traffic_blocks_to_payload(const std::array<std::string, 4>& blocks); // 4*9=36 bytes

// TCH の軟判定: 量子化前の 192 シンボル(±1/±3 スケール)→ 4 ブロック × 72 の LLR(int8、送信順 = traffic_blocks_to_payload の
// ビット順。llr[2k] がシンボル k の上位ビット)。符号が硬判定(正 = 1)、絶対値が確からしさ。デホワイトは符号の反転なのでそのまま掛ける。
// ビット対応(+1→00 +3→01 −1→10 −3→11)から、等間隔 4 値の max-log 近似で 上位 = −y、下位 = |y| − 2(× kLlrScale、±127 で飽和)
inline constexpr float kLlrScale = 32.f;
std::vector<int8_t> traffic_llr(const std::vector<float>& raw);   // 288 = 4 × 72

} // namespace spear::std_t98

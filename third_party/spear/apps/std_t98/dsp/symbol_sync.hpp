// Gardner TED によるシンボルタイミング再生(実数、1 sample/symbol 出力)。
// GNU Radio digital.symbol_sync_ff(TED_GARDNER) 相当の 2 次ループ。App 内(2 つ目の App で抽出候補)。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace spear::std_t98 {

class SymbolSync {
public:
    SymbolSync() = default;
    // sps: 公称 samples/symbol、loop_bw: 正規化ループ帯域、damping、ted_gain、max_dev: 平均周期の許容偏差 [samples/symbol](GR と同じ絶対値)
    SymbolSync(double sps, double loop_bw, double damping, double ted_gain, double max_dev);
    void reset();
    // in を処理し、回復したシンボル(1/symbol)を out に追記。戻り値: 出力数。
    // positions(任意)には各シンボルの判定点を「このオブジェクトが受け取った累計サンプル」上の絶対 index で追記(provenance)。
    std::size_t process(std::span<const float> in, std::vector<float>& out, std::vector<double>* positions = nullptr);
    double period() const { return avg_period_; }   // 現在の推定 sps
    double last_error() const { return last_err_; }

private:
    float interp(std::size_t base, double mu) const;   // buf_[base + mu](線形)
    double nominal_ = 26.0, min_p_ = 25, max_p_ = 27;
    double avg_period_ = 26.0, inst_period_ = 26.0;
    double alpha_ = 0, beta_ = 0, ted_gain_ = 0.1;
    double next_ = 0;            // 次のシンボル点(buf_ 内の実数 index)
    float prev_sym_ = 0.f;
    double last_err_ = 0;
    std::vector<float> buf_;     // 履歴 + 入力
    uint64_t consumed_ = 0;      // buf_ 先頭が絶対 index のいくつに当たるか
    std::size_t keep_ = 64;      // 履歴として残すサンプル数(≥ 1 symbol + 補間余裕)
};

} // namespace spear::std_t98

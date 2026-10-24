// FFTW のプランナ(plan 生成・破棄)はプロセス全体でスレッド安全でない。
// dsp 内で FFTW plan を作る全箇所(spectrum, channelizer)がこの 1 つの mutex で直列化する。
#pragma once
#include <mutex>
namespace spear::dsp::detail { std::mutex& fftw_planner_mutex(); }

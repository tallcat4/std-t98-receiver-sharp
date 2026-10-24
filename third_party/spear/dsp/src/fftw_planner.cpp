#include "spear/dsp/fftw_planner.hpp"
namespace spear::dsp::detail { std::mutex& fftw_planner_mutex() { static std::mutex m; return m; } }

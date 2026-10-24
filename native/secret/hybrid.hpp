#pragma once
#include <cstddef>
#include <memory>
#include <string>

namespace spear::std_t98::secret {
// FP32 inference for the original AMBE2HybridClassifier checkpoint.
// Conv1d/BatchNorm/GELU -> two bidirectional LSTMs -> attention -> classifier.
// No model conversion, quantization, Python, CUDA or inference runtime dependency.
class Hybrid {
public:
    Hybrid();
    ~Hybrid();
    bool load_embedded(std::string* error);
    bool logits(const float* input,std::size_t count,float* output,std::string* error=nullptr) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

#include "hybrid.hpp"
#include "model_data.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <emmintrin.h>

namespace spear::std_t98::secret {
namespace {
// SSE2 is part of the Windows x64 baseline. Four independent accumulators keep
// GEMV fast without fast-math, reduced-precision weights or CPU-specific binaries.
float dot(const float* a,const float* b,int n) {
    __m128 s0=_mm_setzero_ps(),s1=s0,s2=s0,s3=s0;
    int i=0;
    for(;i+16<=n;i+=16) {
        s0=_mm_add_ps(s0,_mm_mul_ps(_mm_loadu_ps(a+i),_mm_loadu_ps(b+i)));
        s1=_mm_add_ps(s1,_mm_mul_ps(_mm_loadu_ps(a+i+4),_mm_loadu_ps(b+i+4)));
        s2=_mm_add_ps(s2,_mm_mul_ps(_mm_loadu_ps(a+i+8),_mm_loadu_ps(b+i+8)));
        s3=_mm_add_ps(s3,_mm_mul_ps(_mm_loadu_ps(a+i+12),_mm_loadu_ps(b+i+12)));
    }
    s0=_mm_add_ps(_mm_add_ps(s0,s1),_mm_add_ps(s2,s3));
    float v[4];_mm_storeu_ps(v,s0);float result=(v[0]+v[1])+(v[2]+v[3]);
    for(;i<n;++i) result+=a[i]*b[i];
    return result;
}
float gelu(float x) { return .5f*x*(1.f+std::erf(x*.7071067811865475f)); }
float sigmoid(float x) { return 1.f/(1.f+std::exp(-x)); }
using Weights=std::map<std::string,std::span<const float>>;
std::span<const float> weight(const Weights& all,const std::string& name,std::size_t count) {
    auto it=all.find(name);
    if(it==all.end() || it->second.size()!=count) throw std::runtime_error("Invalid model weight: "+name);
    return it->second;
}
struct Conv {
    int inputs,outputs;
    std::vector<float> kernel;
    std::span<const float> bias,bn_scale,bn_bias,mean,variance;
    Conv(const Weights& all,int block,int in,int out):inputs(in),outputs(out),kernel(out*in*3) {
        std::string c="cnn."+std::to_string(block*4),b="cnn."+std::to_string(block*4+1);
        auto original=weight(all,c+".weight",out*in*3);
        // Reorder O,I,K to O,K,I to multiply a contiguous three-frame window.
        for(int o=0;o<out;++o) for(int k=0;k<3;++k) for(int i=0;i<in;++i)
            kernel[(o*3+k)*in+i]=original[(o*in+i)*3+k];
        bias=weight(all,c+".bias",out);bn_scale=weight(all,b+".weight",out);bn_bias=weight(all,b+".bias",out);
        mean=weight(all,b+".running_mean",out);variance=weight(all,b+".running_var",out);
    }
    std::vector<float> run(const std::vector<float>& input) const {
        std::vector<float> padded(22*inputs),result(20*outputs);
        std::copy(input.begin(),input.end(),padded.begin()+inputs);
        for(int t=0;t<20;++t) for(int o=0;o<outputs;++o) {
            float value=dot(padded.data()+t*inputs,kernel.data()+o*inputs*3,inputs*3)+bias[o];
            value=(value-mean[o])/std::sqrt(variance[o]+1e-5f)*bn_scale[o]+bn_bias[o];
            result[t*outputs+o]=gelu(value);
        }
        return result;
    }
};
struct Lstm {
    std::span<const float> wi,wh,bi,bh;
    bool reverse;
    Lstm(const Weights& all,int layer,bool rev):reverse(rev) {
        std::string suffix="_l"+std::to_string(layer)+(rev?"_reverse":"");
        wi=weight(all,"lstm.weight_ih"+suffix,512*256);wh=weight(all,"lstm.weight_hh"+suffix,512*128);
        bi=weight(all,"lstm.bias_ih"+suffix,512);bh=weight(all,"lstm.bias_hh"+suffix,512);
    }
    void run(const std::vector<float>& input,std::vector<float>& output) const {
        std::array<float,128> hidden{},cell{};
        std::array<float,512> gates{};
        for(int step=0;step<20;++step) {
            int t=reverse?19-step:step;
            for(int row=0;row<512;++row)
                gates[row]=(dot(input.data()+t*256,wi.data()+row*256,256)+bi[row])+
                    (dot(hidden.data(),wh.data()+row*128,128)+bh[row]);
            // PyTorch's gate order is input, forget, candidate, output.
            for(int j=0;j<128;++j) {
                cell[j]=sigmoid(gates[128+j])*cell[j]+sigmoid(gates[j])*std::tanh(gates[256+j]);
                hidden[j]=sigmoid(gates[384+j])*std::tanh(cell[j]);
                output[t*256+(reverse?128:0)+j]=hidden[j];
            }
        }
    }
};
}
struct Hybrid::Impl {
    Weights all;
    std::vector<Conv> conv;
    std::vector<Lstm> lstm;
    std::span<const float> attention,attention_bias,linear1,bias1,linear2,bias2;
    Impl() {
        for(const auto& spec:t98_weights)
            all.emplace(spec.name,std::span(reinterpret_cast<const float*>(t98_model_data+spec.offset),spec.count));
        conv.emplace_back(all,0,49,64);conv.emplace_back(all,1,64,128);conv.emplace_back(all,2,128,256);
        for(int layer=0;layer<2;++layer) {lstm.emplace_back(all,layer,false);lstm.emplace_back(all,layer,true);}
        attention=weight(all,"attn_pool.attn.weight",256);attention_bias=weight(all,"attn_pool.attn.bias",1);
        linear1=weight(all,"classifier.0.weight",128*256);bias1=weight(all,"classifier.0.bias",128);
        linear2=weight(all,"classifier.3.weight",2*128);bias2=weight(all,"classifier.3.bias",2);
    }
    void run(const float* input,float* output) const {
        std::vector<float> values(input,input+980);
        for(const auto& layer:conv) values=layer.run(values);
        for(int layer=0;layer<2;++layer) {
            std::vector<float> next(20*256);
            lstm[layer*2].run(values,next);lstm[layer*2+1].run(values,next);values=std::move(next);
        }
        std::array<float,20> scores{};
        for(int t=0;t<20;++t) scores[t]=dot(values.data()+t*256,attention.data(),256)+attention_bias[0];
        float maximum=*std::max_element(scores.begin(),scores.end()),sum=0;
        for(float& score:scores) {score=std::exp(score-maximum);sum+=score;}
        std::array<float,256> pooled{};
        for(int t=0;t<20;++t) for(int j=0;j<256;++j) pooled[j]+=values[t*256+j]*(scores[t]/sum);
        std::array<float,128> hidden{};
        for(int j=0;j<128;++j) hidden[j]=gelu(dot(pooled.data(),linear1.data()+j*256,256)+bias1[j]);
        for(int j=0;j<2;++j) output[j]=dot(hidden.data(),linear2.data()+j*128,128)+bias2[j];
    }
};
Hybrid::Hybrid()=default;
Hybrid::~Hybrid()=default;
bool Hybrid::load_embedded(std::string* error) {
    try {impl_=std::make_unique<Impl>();return true;} catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
bool Hybrid::logits(const float* input,std::size_t count,float* output,std::string* error) const {
    try {
        if(!impl_ || !input || !output || count>1024) throw std::invalid_argument("Invalid hybrid input");
        for(std::size_t i=0;i<count;++i) impl_->run(input+980*i,output+2*i);
        return true;
    } catch(const std::exception& e) {if(error)*error=e.what();return false;}
}
}

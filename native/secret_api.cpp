#include "t98.h"
#include "secret/guided.hpp"
#include "secret/hybrid.hpp"
#include "ambe/bits.hpp"
#include "blip25.h"
#include <memory>
#include <stdexcept>
#include <algorithm>

using namespace spear::std_t98;
namespace {
thread_local std::string secret_error;
struct Context {
    secret::Hybrid model;
    std::unique_ptr<secret::GuidedSession> solver;
    int64_t session=-1;
    uint64_t next=0;
    t98_secret_result latest{};
    std::vector<secret::SeriesKey> cache;
    bool cache_pending=false,cache_hit=false;
    Context() {std::string error;if(!model.load_embedded(&error)) throw std::runtime_error(error);}
};
}
void* t98_secret_create() {
    try {return new Context();} catch(const std::exception& e) {secret_error=e.what();return nullptr;}
}
void t98_secret_destroy(void* h) {delete static_cast<Context*>(h);}
const char* t98_secret_error() {return secret_error.c_str();}
int t98_secret_set_cache(void* h,const int32_t* pairs,int count) {
    try {
        if(!h || count<0 || count>16*32767 || (count && !pairs)) throw std::invalid_argument("Invalid secret cache");
        std::vector<secret::SeriesKey> cache;
        for(int i=0;i<count;++i) {
            int s=pairs[2*i],k=pairs[2*i+1];
            if(s<0 || s>15 || k<1 || k>32767) throw std::invalid_argument("Invalid cached code");
            cache.push_back({uint8_t(s),uint16_t(k)});
        }
        std::sort(cache.begin(),cache.end());cache.erase(std::unique(cache.begin(),cache.end()),cache.end());
        static_cast<Context*>(h)->cache=std::move(cache);return 0;
    } catch(const std::exception& e) {secret_error=e.what();return -1;}
}
int t98_secret_cache_hit(void* h) {return h && static_cast<Context*>(h)->cache_hit ? 1 : 0;}
int t98_secret_process(void* h,int64_t session,uint64_t first,const int8_t* llr,int count,t98_secret_result* result) {
    try {
        if(!h || !llr || !result || count<1 || count>64 || session<0 || first>UINT64_MAX-uint64_t(count)) throw std::invalid_argument("Invalid secret window");
        auto& c=*static_cast<Context*>(h);
        if(!c.solver || c.session!=session || first>c.next) {
            c.solver=std::make_unique<secret::GuidedSession>(secret::hybrid_scorer(c.model));
            c.session=session;c.next=first;c.latest={session,first,-1,-1,0,0};
            c.cache_pending=!c.cache.empty();c.cache_hit=false;
        }
        bool added=false;
        for(int i=0;i<count;++i) {
            if(first+uint64_t(i)<c.next) continue;
            if(c.cache_hit) {c.next=first+i+1;continue;}
            secret::Burst burst{};secret::Valid4 valid{};
            for(int f=0;f<4;++f) {
                uint8_t bits[49]{};
                valid[f]=blip25_fec_3600_soft(llr+i*288+f*72,bits,nullptr)>=0;
                for(int b=0;b<49;++b) burst[f][b]=bits[ambe::kThumbDv[b]];
            }
            c.solver->add(burst,valid);c.next=first+i+1;added=true;
            if(c.cache_pending && c.solver->ready()) {
                // Plaintext competes too, so cached keys cannot win by default.
                auto candidates=c.cache;candidates.push_back({0,0});
                auto r=c.solver->verify_cached(candidates);c.cache_pending=false;
                if(r.accepted) {
                    c.cache_hit=true;
                    c.latest={session,c.next,int(r.accepted->series),int(r.accepted->key),r.score,r.margin};
                }
            }
        }
        if(added && !c.cache_hit && !c.cache_pending) {
            auto r=c.solver->assess();
            c.latest={session,c.next,r.accepted?int(r.accepted->series):-1,r.accepted?int(r.accepted->key):-1,r.score,r.margin};
            if(r.accepted && r.accepted->key && std::find(c.cache.begin(),c.cache.end(),*r.accepted)==c.cache.end()) c.cache.push_back(*r.accepted);
        }
        c.latest.next=c.next;
        *result=c.latest;return 0;
    } catch(const std::exception& e) {secret_error=e.what();return -1;}
}
int t98_hybrid_logits(void* h,const float* input,int count,float* output) {
    if(!h || count<0) return -1;
    return static_cast<Context*>(h)->model.logits(input,count,output,&secret_error)?0:-1;
}

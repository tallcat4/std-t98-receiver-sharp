#pragma once
#include <stdint.h>
#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t symbol;
    int32_t traffic, crc, user, maker, call, mode;
    char csm[16];
    uint8_t payload[36];
    int8_t llr[288];
} t98_frame;
typedef struct {
    uint64_t samples, frames, crc_ok;
    double power, frequency_error;
    int32_t locked, active;
} t98_stats;
API void* t98_create(double rate);
API void t98_destroy(void* handle);
API int t98_process(void* handle, const float* iq, int count);
API int t98_read(void* handle, t98_frame* frame);
API int t98_status(void* handle, t98_stats* stats, float* eye, int capacity);
API int t98_eye_read(void* handle,float* traces,int capacity);
API const char* t98_error(void);
API int t98_voice(void* handle, const int8_t* llr, int series, int key, int16_t* pcm);
API void t98_voice_reset(void* handle);
typedef struct {
    int64_t session;
    uint64_t next;
    int32_t series,key; // -1 = not confirmed; key 0 = plaintext
    double score,margin;
} t98_secret_result;
API void* t98_secret_create(void);
API void t98_secret_destroy(void* handle);
API const char* t98_secret_error(void);
// Packed int32 pairs: series, key. Only nonzero privacy keys are persisted.
API int t98_secret_set_cache(void* handle,const int32_t* pairs,int count);
API int t98_secret_cache_hit(void* handle);
API int t98_secret_process(void* handle,int64_t session,uint64_t first,const int8_t* llr,int count,t98_secret_result* result);
API int t98_hybrid_logits(void* handle,const float* input,int count,float* output);
#ifdef __cplusplus
}
#endif

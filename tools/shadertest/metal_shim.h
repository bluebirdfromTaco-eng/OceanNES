// Minimal shim that lets the Metal shader source compile and run as C++ on the
// CPU, so the filters can be checked without a Mac GPU.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

typedef float float2 __attribute__((ext_vector_type(2)));
typedef float float3 __attribute__((ext_vector_type(3)));
typedef float float4 __attribute__((ext_vector_type(4)));
typedef unsigned uint2 __attribute__((ext_vector_type(2)));
typedef unsigned short ushort;
typedef unsigned short ushort4 __attribute__((ext_vector_type(4)));
typedef unsigned uint;

#define vertex
#define fragment
#define constant const
#define M_PI_F 3.14159265358979f

static inline float2 mk_float2(float a) { return (float2){a, a}; }
static inline float2 mk_float2(float a, float b) { return (float2){a, b}; }
static inline float3 mk_float3(float a) { return (float3){a, a, a}; }
static inline float3 mk_float3(float a, float b, float c) { return (float3){a, b, c}; }
static inline float4 mk_float4(float a) { return (float4){a, a, a, a}; }
static inline float4 mk_float4(float a, float b, float c, float d) { return (float4){a, b, c, d}; }
static inline float4 mk_float4(float2 a, float b, float c) { return (float4){a.x, a.y, b, c}; }
static inline float4 mk_float4(float3 a, float b) { return (float4){a.x, a.y, a.z, b}; }
static inline uint2 mk_uint2(float2 a) { return (uint2){(unsigned)a.x, (unsigned)a.y}; }
static inline uint2 mk_uint2(unsigned a, unsigned b) { return (uint2){a, b}; }

namespace metal {
static inline float clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float clamp(double x, double a, double b) { return clamp((float)x, (float)a, (float)b); }
static inline int clamp(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
template <class V> static inline V vclamp(V x, V a, V b) { V r = x; for (int i = 0; i < (int)(sizeof(V) / sizeof(float)); i++) r[i] = clamp((float)x[i], (float)a[i], (float)b[i]); return r; }
static inline float2 clamp(float2 x, float2 a, float2 b) { return (float2){clamp(x.x, a.x, b.x), clamp(x.y, a.y, b.y)}; }
static inline float3 clamp(float3 x, float3 a, float3 b) { return (float3){clamp(x.x, a.x, b.x), clamp(x.y, a.y, b.y), clamp(x.z, a.z, b.z)}; }
static inline float max(float a, float b) { return a > b ? a : b; }
static inline float2 max(float2 a, float2 b) { return (float2){max(a.x, b.x), max(a.y, b.y)}; }
static inline float3 max(float3 a, float3 b) { return (float3){max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)}; }
static inline float mix(float a, float b, float t) { return a + (b - a) * t; }
static inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
static inline float length(float2 a) { return std::sqrt(dot(a, a)); }
static inline float3 pow(float3 a, float3 b) { return (float3){std::pow(a.x, b.x), std::pow(a.y, b.y), std::pow(a.z, b.z)}; }
static inline float2 floor(float2 a) { return (float2){std::floor(a.x), std::floor(a.y)}; }
static inline float floor(float a) { return std::floor(a); }
static inline float2 fract(float2 a) { return a - floor(a); }
static inline float smoothstep(float e0, float e1, float x) { float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); }
static inline float2 abs(float2 a) { return (float2){std::fabs(a.x), std::fabs(a.y)}; }
static inline float abs(float a) { return std::fabs(a); }
using std::exp; using std::sqrt; using std::sin; using std::cos;

namespace filter { enum { linear, nearest }; }
namespace address { enum { clamp_to_edge }; }
struct sampler { constexpr sampler(int, int) {} };

template <class T> struct texture2d;
template <> struct texture2d<ushort> {
    int w, h; std::vector<ushort> d;
    ushort4 read(uint2 c) const { ushort v = d[(size_t)c.y * w + c.x]; return (ushort4){v, 0, 0, 0}; }
};
template <> struct texture2d<float> {
    int w, h; std::vector<float4> d;
    float4 read(uint2 c) const { return d[(size_t)c.y * w + c.x]; }
    float4 at(int x, int y) const { x = x < 0 ? 0 : x >= w ? w - 1 : x; y = y < 0 ? 0 : y >= h ? h - 1 : y; return d[(size_t)y * w + x]; }
    float4 sample(sampler, float2 uv) const {
        float fx = uv.x * w - 0.5f, fy = uv.y * h - 0.5f;
        int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        float ax = fx - x0, ay = fy - y0;
        return (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
    }
};
}

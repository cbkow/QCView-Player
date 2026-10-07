/* SPDX-License-Identifier: GPL-3.0-only
 * minColorAE. Copyright (C) 2026 cbkow.
 * Derived from OpenDRT v1.1.0 by Jed Smith (https://github.com/jedypod/open-display-transform), GPLv3;
 * modified 2026-09-30 to 2026-10-04, see CHANGES-FROM-OPENDRT.md. Not affiliated with or endorsed by OpenDRT.
 */
/* DRT core — C++ shim.
 *
 * Gives opendrt_kernel.h the DCTL dialect on a plain C++ compiler: float2 / float3
 * with component-wise operators, make_float*, the _xxxf intrinsics, and the
 * qualifier macros. No include guard ON PURPOSE: this file is included once per
 * namespace (drt:: by opendrt.h, dctl_ref::dctl:: by tools/dctl_ref.cpp, where the
 * same shim lets the untouched upstream DCTL compile as C++). <cmath> must already
 * be included at file scope by whoever includes this.
 *
 * Every intrinsic takes and returns float. That is deliberate: DCTL, MSL, HLSL and
 * GLSL have no double, so an expression like `2.0*PI` is float there. Taking float
 * arguments narrows a double before the call and keeps the C++ results on the same
 * float path as the GPU (the upstream DCTL has two such literals).
 */

struct float2 { float x; float y; };
struct float3 { float x; float y; float z; };

static inline float2 make_float2(float x, float y) { float2 r; r.x = x; r.y = y; return r; }
static inline float3 make_float3(float x, float y, float z) { float3 r; r.x = x; r.y = y; r.z = z; return r; }

static inline float3 operator+(float3 a, float3 b) { return make_float3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline float3 operator+(float3 a, float b)  { return make_float3(a.x + b, a.y + b, a.z + b); }
static inline float3 operator+(float a, float3 b)  { return make_float3(a + b.x, a + b.y, a + b.z); }
static inline float3 operator-(float3 a, float3 b) { return make_float3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline float3 operator-(float3 a, float b)  { return make_float3(a.x - b, a.y - b, a.z - b); }
static inline float3 operator-(float a, float3 b)  { return make_float3(a - b.x, a - b.y, a - b.z); }
static inline float3 operator-(float3 a)           { return make_float3(-a.x, -a.y, -a.z); }
static inline float3 operator*(float3 a, float3 b) { return make_float3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float3 operator*(float3 a, float b)  { return make_float3(a.x * b, a.y * b, a.z * b); }
static inline float3 operator*(float a, float3 b)  { return make_float3(a * b.x, a * b.y, a * b.z); }
static inline float3 operator/(float3 a, float3 b) { return make_float3(a.x / b.x, a.y / b.y, a.z / b.z); }
static inline float3 operator/(float3 a, float b)  { return make_float3(a.x / b, a.y / b, a.z / b); }
static inline float3 operator/(float a, float3 b)  { return make_float3(a / b.x, a / b.y, a / b.z); }
static inline float3 &operator+=(float3 &a, float3 b) { a = a + b; return a; }
static inline float3 &operator+=(float3 &a, float b)  { a = a + b; return a; }
static inline float3 &operator-=(float3 &a, float3 b) { a = a - b; return a; }
static inline float3 &operator-=(float3 &a, float b)  { a = a - b; return a; }
static inline float3 &operator*=(float3 &a, float3 b) { a = a * b; return a; }
static inline float3 &operator*=(float3 &a, float b)  { a = a * b; return a; }
static inline float3 &operator/=(float3 &a, float3 b) { a = a / b; return a; }
static inline float3 &operator/=(float3 &a, float b)  { a = a / b; return a; }

#define __DEVICE__ static inline
#define __CONSTANT__ static const

static inline float _powf(float a, float b)  { return std::pow(a, b); }
static inline float _expf(float a)           { return std::exp(a); }
static inline float _exp2f(float a)          { return std::exp2(a); }
static inline float _exp10f(float a)         { return std::pow(10.0f, a); }
static inline float _logf(float a)           { return std::log(a); }
static inline float _log2f(float a)          { return std::log2(a); }
static inline float _sqrtf(float a)          { return std::sqrt(a); }
static inline float _fabs(float a)           { return std::fabs(a); }
static inline float _fmod(float a, float b)  { return std::fmod(a, b); }
static inline float _floorf(float a)         { return std::floor(a); }
static inline float _atan2f(float a, float b){ return std::atan2(a, b); }
static inline float _fminf(float a, float b) { return std::fmin(a, b); }
static inline float _fmaxf(float a, float b) { return std::fmax(a, b); }

#define DRT_PARAMS_ARG const DrtParams &
#define DRT_AGX_ARG const DrtAgxParams &

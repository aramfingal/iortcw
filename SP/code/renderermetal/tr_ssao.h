/*
 * tr_ssao.h — SAO compute kernel parameter structs (T2)
 *
 * Host (C++) <-> MSL shared layout for the Scalable Ambient Obscurance
 * compute kernel (code/renderermetal/glsl/sao_compute.metal, T3).
 *
 * ── GL2 ref: PARITY DEVIATION (ledger E3 / B6, Path B — SAO compute) ────────
 *
 *   GL2's ssao_fp.glsl used two loose uniforms:
 *       uniform sampler2D u_ScreenDepthMap;       // gl2: ssao_fp.glsl:1
 *       uniform vec4      u_ViewInfo;              // gl2: ssao_fp.glsl:3
 *           // .x = zfar/znear, .y = zfar, .z = 1/width, .w = 1/height
 *   …and a fullscreen-quad vertex shader (ssao_vp.glsl:1–13) dispatched from
 *   tr_backend.c:1067–1128 via three sequential FBO-bound fragment passes.
 *
 *   Our Metal replacement is Apple's Scalable Ambient Obscurance (SAO) compute
 *   kernel — a single compute dispatch over the depth buffer with 36 spiral taps
 *   and temporal distribution. The algorithm is intentionally different; the
 *   r_ssao cvar gate and the final SSAO blend step at tr_backend.cpp:1476–1490
 *   are retained with full parity.
 *
 *   Authoritative deviation record:
 *       .squad/decisions/inbox/dallas-ssao-deviation-decision.md (2026-06-07)
 *
 * ── OQ-1 RESOLUTION (option a) ───────────────────────────────────────────────
 *
 *   ioquake3's backEnd.viewParms.projectionMatrix is OpenGL convention
 *   (right-handed, clip Z ∈ [−1, 1]).  The SAO kernel's NDC→camera-space
 *   reconstruction assumes Metal convention (clip Z ∈ [0, 1]).
 *
 *   RESOLUTION: the HOST (T4 wiring) computes a Metal-convention projection
 *   matrix (and its inverse) before upload.  The kernel math is byte-for-byte
 *   identical to the Apple SAO reference.  Do NOT pass the raw GL-convention
 *   projectionMatrix from backEnd.viewParms directly.
 *
 * ── INCLUDE STRATEGY ─────────────────────────────────────────────────────────
 *
 *   Host (.cpp / .mm):
 *       #include "tr_ssao.h"
 *       __METAL_VERSION__ is NOT defined by clang/clang++ → simd types are used
 *       via <simd/simd.h> (simd_float2, simd_float4x4, uint32_t).
 *
 *   Compute kernel (.metal):
 *       #include "tr_ssao.h"  (via -I code/renderermetal in xcrun/Makefile)
 *       __METAL_VERSION__ IS defined by metal → native MSL types (float2,
 *       float4x4, uint) are used directly without <simd/simd.h>.
 *
 *   Pure C (.c): not intended; the T4 host dispatch code is C++.
 *
 * ── BUFFER BINDING CONTRACT ──────────────────────────────────────────────────
 *
 *   These indices are for the SAO compute encoder only.  They live in the
 *   compute pipeline namespace and do NOT conflict with the render pipeline
 *   binding contracts in the generic / lightall / postprocess shaders
 *   (render and compute encoders are disjoint).
 *
 *   [[buffer(METAL_SAO_BUFFER_FRAME_PARAMS)]]   = 0  → MetalSAOFrameParams
 *   [[buffer(METAL_SAO_BUFFER_CAMERA_PARAMS)]]  = 1  → MetalSAOCameraParams
 *
 *   Texture arguments (also compute-only):
 *   [[texture(METAL_SAO_TEXTURE_OUTPUT)]]       = 0  → R8Unorm AO output   (write)
 *   [[texture(METAL_SAO_TEXTURE_DEPTH)]]        = 1  → Depth32Float input  (read)
 *   [[texture(METAL_SAO_TEXTURE_DEPTH_MIP)]]    = 2  → Depth mip chain     (read)
 *
 *   See .squad/decisions/inbox/vasquez-ssao-binding-contract.md for the
 *   rationale, Apple SAO reference indices, and T4 wiring instructions.
 */

#pragma once

/* ── Type aliases: host simd ↔ MSL native types ─────────────────────────── */
/*
 * On the host compiler (__METAL_VERSION__ not defined) we pull in Apple's
 * <simd/simd.h> and alias to the standard simd_ C types.  On the MSL compiler
 * (__METAL_VERSION__ is defined) we alias to the MSL built-ins directly so the
 * same struct definitions below compile in both translation units.
 *
 * Memory layout is identical: simd_float2 == float2 (8 bytes, align 8),
 * simd_float4x4 == float4x4 (64 bytes, column-major, align 16).
 */
#ifndef __METAL_VERSION__
    #include <simd/simd.h>
    typedef simd_float2   sao_float2_t;
    typedef simd_float4x4 sao_float4x4_t;
    typedef uint32_t      sao_uint_t;
#else
    typedef float2        sao_float2_t;
    typedef float4x4      sao_float4x4_t;
    typedef uint          sao_uint_t;
#endif

/* ── Buffer binding indices (compute encoder) ────────────────────────────── */
#define METAL_SAO_BUFFER_FRAME_PARAMS   0
#define METAL_SAO_BUFFER_CAMERA_PARAMS  1

/* ── Texture binding indices (compute encoder) ───────────────────────────── */
#define METAL_SAO_TEXTURE_OUTPUT        0   /* R8Unorm — AO result (write) */
#define METAL_SAO_TEXTURE_DEPTH         1   /* Depth32Float — scene depth (read) */
#define METAL_SAO_TEXTURE_DEPTH_MIP     2   /* Depth32Float mip chain (read) */

/* ── MetalSAOFrameParams ─────────────────────────────────────────────────── */
/*
 * Per-frame screen-space parameters consumed by the SAO kernel.
 *
 * Apple SAO reference: AAPLFrameConstants (subset of fields used by
 *     scalableAmbientObscurance in for-copilot/Renderer/Shaders/
 *     AAPLAmbientObscurance.metal:91–178).
 *
 * Byte layout (must match on host and kernel):
 *   Offset  0 : invPhysicalSize  float2 / simd_float2  (8 B, align 8)
 *   Offset  8 : frameCounter     uint   / uint32_t     (4 B, align 4)
 *   Offset 12 : _pad0            uint   / uint32_t     (4 B — explicit pad to 16)
 *   Total: 16 bytes.
 *
 * invPhysicalSize: reciprocal of the physical (backing) render target size in
 *     pixels, i.e. float2(1.0/width, 1.0/height).  Used to convert integer
 *     thread coordinates to [0,1] NDC (see Apple kernel line 17).
 *
 * frameCounter: monotonically incrementing frame index; drives the temporal
 *     dither seed (seed += frameData.frameCounter, kernel line 137).  The host
 *     must increment this every frame; wrapping at UINT32_MAX is fine.
 */
typedef struct {
    sao_float2_t   invPhysicalSize;   /* Apple SAO ref: frameData.invPhysicalSize   */
    sao_uint_t     frameCounter;      /* Apple SAO ref: frameData.frameCounter       */
    sao_uint_t     _pad0;             /* explicit pad — struct size must be 16 bytes */
} MetalSAOFrameParams;

/* ── MetalSAOCameraParams ────────────────────────────────────────────────── */
/*
 * Camera projection matrices consumed by the SAO kernel.
 *
 * Apple SAO reference: AAPLCameraParams (subset used by the SAO kernel).
 * Kernel field accesses (for-copilot/Renderer/Shaders/AAPLAmbientObscurance.metal):
 *   invProjectionMatrix: columns[0][0], [1][1], [2][3], [3][3], [3][0], [3][1]
 *       → used in GetCameraSpacePositionFromDepth (line 24–31) and
 *         GetOffsetCameraSpacePositionFromDepth (line 62–68)
 *   projectionMatrix: [1][1] only
 *       → radius = 0.5 * height * projectionMatrix[1][1]  (kernel line 104)
 *
 * IMPORTANT (OQ-1 resolution, option a):
 *   Both matrices must use Metal NDC convention (clip Z ∈ [0, 1]).
 *   The host (T4) must NOT use backEnd.viewParms.projectionMatrix directly
 *   (that is GL convention, clip Z ∈ [−1, 1]).  Instead, build a Metal
 *   projection matrix from the camera FOV/near/far and invert it.
 *
 * Byte layout:
 *   Offset   0 : invProjectionMatrix  float4x4 / simd_float4x4  (64 B, align 16)
 *   Offset  64 : projectionMatrix     float4x4 / simd_float4x4  (64 B, align 16)
 *   Total: 128 bytes.
 */
typedef struct {
    sao_float4x4_t  invProjectionMatrix;  /* Metal-convention inverse proj matrix */
    sao_float4x4_t  projectionMatrix;     /* Metal-convention projection matrix    */
} MetalSAOCameraParams;

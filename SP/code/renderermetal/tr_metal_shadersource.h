// tr_metal_shadersource.h — MSL source string provider for Phase 5 bring-up
// GL2 ref: (no direct GL2 equivalent — Metal-specific bootstrap module)
//
// GENERATED FROM glsl/*.metal — keep in sync;
// TODO: replace with build-time stringify (Ripley/cmake) once all shaders are ported.
//
// Ownership: Lambert (Shading/Draw Implementer)
// Phase 5 M5.2 — only "generic" and "texturecolor" are available; all others return NULL.
//
#ifndef TR_METAL_SHADERSOURCE_H
#define TR_METAL_SHADERSOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "../qcommon/q_shared.h"

// Returns the embedded MSL source string for the named shader program.
// name:     shader program name (e.g. "generic", "texturecolor")
// fragment: qtrue → return fragment stage source; qfalse → return vertex stage source
// Returns NULL if no MSL has been translated for this shader (non-fatal; Phase 5 deferred).
const char *GLMetal_GetShaderSource(const char *name, qboolean fragment);

// Returns the self-contained MSL source for the SAO compute kernel.
// GL2 ref: PARITY DEVIATION — replaces code/renderergl2/glsl/ssao_fp.glsl (ledger E3/B6).
// Entry point: scalableAmbientObscurance
// Binding contract: buffer(0)=MetalSAOFrameParams, buffer(1)=MetalSAOCameraParams;
//                   texture(0)=AO write (R8Unorm), texture(1)=depth read, texture(2)=depth-mip read.
// Integration contract (T4b / Ripley): getter name and kernel entry-point are fixed.
const char *GLMetal_GetSAOComputeSource(void);

#ifdef __cplusplus
}

// ── PHASE5-M5.2 Metal helpers (C++ only) ────────────────────────────────────
// These require Metal-cpp types; only visible to C++ translation units.
#include <Metal/Metal.hpp>

// 1×1 RGBA8Unorm white texture — placeholder until the MTL::Texture side-table
// lands in Phase 4.  Created lazily on first call; released at shutdown.
// Used for all fragment texture slots during Phase 5 bring-up.
MTL::Texture *GLMetal_GetPlaceholderWhiteTexture(void);

// Default linear-mip sampler state — created lazily, released at shutdown.
MTL::SamplerState *GLMetal_GetDefaultSamplerState(void);

// Track the last image bound to each TMU (set by tr_shade.cpp wrappers).
// Used so R_DrawElements can identify which image is in each slot.
struct image_s;
void        GLMetal_RecordBoundImage(struct image_s *image, int tmu);
struct image_s *GLMetal_GetBoundImage(int tmu);

#endif // __cplusplus

#endif // TR_METAL_SHADERSOURCE_H

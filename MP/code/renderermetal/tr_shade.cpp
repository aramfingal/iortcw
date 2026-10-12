/*
===========================================================================

Return to Castle Wolfenstein multiplayer GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.

This file is part of the Return to Castle Wolfenstein multiplayer GPL Source Code (RTCW MP Source Code).

RTCW MP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW MP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW MP Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the RTCW MP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW MP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
// tr_shade.cpp — Metal renderer port of tr_shade.c
// GL2 ref: tr_shade.c
//
// Phase-3 PORT: all 19 functions + tess global from tr_shade.c are present.
//   7  REAL        — pure CPU logic: RB_BeginSurface, ComputeTexMods,
//                    ComputeDeformValues, ComputeShaderColors, ComputeFogValues,
//                    ComputeFogColorMask, RB_CalcShaderVertexAttribs
//  12  PARITY-SEAM — real CPU control flow preserved; raw qgl*/GPU-submit calls
//                    annotated; GLSL_* / GL_* seams are already real in their
//                    own TUs (tr_glsl.cpp, tr_backend.cpp, tr_vbo.cpp).
//   0  PARITY-STUB — none
//
// COPY-THEN-ANNOTATE: source copied from code/renderergl2/tr_shade.c then
// annotated with PARITY-SEAM markers.  Tabs preserved.
//
// Scaffolds removed from tr_scaffold_stubs.cpp:
//   - shaderCommands_t tess (tr_shade.c:62, owner: Lambert)
//   - RB_StageIteratorGeneric (tr_shade.c:1633, owner: Lambert)
//   - R_DrawElements (tr_shade.c:41, owner: Lambert)
//   - RB_BeginSurface (tr_shade.c:289, owner: Lambert)
//   - RB_EndSurface (tr_shade.c:1791, owner: Lambert)
//
// New stub added to tr_scaffold_stubs.cpp:
//   - RB_ShadowTessEnd (tr_shadows.c:158, owner: Hudson)
//
// Include order: q_shared.h first (for vec_t/qboolean types), then
// tr_extramath.h in C++ context so Mat4* keeps C++ linkage, then tr_local.h.

extern "C" {
#include "../qcommon/q_shared.h"
}

#include "tr_extramath.h"

extern "C" {
#include "tr_local.h"
#include "tr_dsa.h"
}

// PHASE5-M5.2: Metal draw infrastructure — included OUTSIDE extern "C" because
// tr_metal_shadersource.h pulls in Metal-cpp headers with C++ templates.
// Both headers wrap their own C declarations in extern "C" internally.
#include "tr_metal_device.h"
#include "tr_metal_shadersource.h"

extern "C" uint64_t GLMetal_VertexDescHashForAttribs( uint32_t attribs );
MTL::RenderPipelineState *GLMetal_GetPipelineForState( const char *programName, uint64_t vertexDescHash, uint32_t stateBits );

struct MetalDrawUniforms
{
	float mvp[16];
	float baseColor[4];
	float vertColor[4];
	float diffuseTexMatrix[8][4];
	float time;
	int32_t deformGen;
	float deformParams[5];
	int32_t colorGen;
	int32_t alphaGen;
	int32_t tcGen0;
	int32_t _padTcGen[2];
	float tcGen0Vector0[4];
	float tcGen0Vector1[4];
	float localViewOrigin[4];
	int32_t alphaTest;
	int32_t _padTail[3];

	// RGBAGEN / FOG uniforms — appended at offset 336; existing offsets above are STABLE.
	// vec3 GL uniforms are stored as float[4] (pad w=0) to match MSL float4 constant-buffer rules.
	// All six vec3/vec4 blocks are 16 bytes each → 6×16 = 96 bytes (offset 336–431).
	// portalRange + fogEyeT are two floats (8 bytes); _padRgbaFog[2] brings tail to 448 (28×16). ✓
	//
	// Final layout (byte offsets within this struct):
	//   336 : ambientLight[4]   — UNIFORM_AMBIENTLIGHT  (vec3, w=0)
	//   352 : directedLight[4]  — UNIFORM_DIRECTEDLIGHT (vec3, w=0)
	//   368 : modelLightDir[4]  — UNIFORM_MODELLIGHTDIR (vec3, w=0)
	//   384 : fogDistance[4]    — UNIFORM_FOGDISTANCE   (vec4)
	//   400 : fogDepth[4]       — UNIFORM_FOGDEPTH      (vec4)
	//   416 : fogColorMask[4]   — UNIFORM_FOGCOLORMASK  (vec4)
	//   432 : portalRange       — UNIFORM_PORTALRANGE   (float)
	//   436 : fogEyeT           — UNIFORM_FOGEYET       (float)
	//   440 : _padRgbaFog[2]    — explicit tail pad to 16-byte boundary
	//   448 : (end of struct)
	//
	// Vasquez must mirror in GenericDrawUniforms as:
	//   float4 u_AmbientLight;   // offset 336
	//   float4 u_DirectedLight;  // offset 352
	//   float4 u_ModelLightDir;  // offset 368
	//   float4 u_FogDistance;    // offset 384
	//   float4 u_FogDepth;       // offset 400
	//   float4 u_FogColorMask;   // offset 416
	//   float  u_PortalRange;    // offset 432
	//   float  u_FogEyeT;        // offset 436
	//   float2 _padRgbaFog;      // offset 440  (explicit — do NOT omit)
	float ambientLight[4];      // UNIFORM_AMBIENTLIGHT  (vec3, w=0)
	float directedLight[4];     // UNIFORM_DIRECTEDLIGHT (vec3, w=0)
	float modelLightDir[4];     // UNIFORM_MODELLIGHTDIR (vec3, w=0)
	float fogDistance[4];       // UNIFORM_FOGDISTANCE   (vec4)
	float fogDepth[4];          // UNIFORM_FOGDEPTH      (vec4)
	float fogColorMask[4];      // UNIFORM_FOGCOLORMASK  (vec4)
	float portalRange;          // UNIFORM_PORTALRANGE   (float)
	float fogEyeT;              // UNIFORM_FOGEYET       (float)
	float vertexLerp;           // 440  UNIFORM_VERTEXLERP (float) — MSL u_VertexLerp
	float zFadeLowest;          // 444  UNIFORM_ZFADELOWEST (float) — RTCW AGEN_NORMALZFADE
	float fireRiseDir[4];       // 448  UNIFORM_FIRERISEDIR (vec3) — RTCW; w = UNIFORM_ZFADEHIGHEST
	                            // 464  struct total (29×16)
};

// Lightall uniform block — superset of MetalDrawUniforms.
// First member `base` is byte-identical to MetalDrawUniforms (0–447), so generic
// fill code reuses R_FillMetalDrawUniforms.  Lightall-specific fields start at
// byte 448.  All fields obey Metal constant-buffer alignment: vec3 GL uniforms
// stored as float[4] (w=0), scalars packed in a 16-byte slot with explicit pad.
//
// BINDING CONTRACT (for Vasquez / MSL LightallDrawUniforms):
//   vertex   [[buffer(1)]] — full MetalLightallUniforms (672 bytes)
//   fragment [[buffer(0)]] — same MetalLightallUniforms (672 bytes)
//   Texture/sampler bindings (TMU index → [[texture(n)]]):
//     TB_DIFFUSEMAP  (0) → [[texture(0)]]  u_DiffuseMap
//     TB_LIGHTMAP    (1) → [[texture(1)]]  u_LightMap
//     TB_NORMALMAP   (2) → [[texture(2)]]  u_NormalMap
//     TB_DELUXEMAP   (3) → [[texture(3)]]  u_DeluxeMap
//     TB_SPECULARMAP (4) → [[texture(4)]]  u_SpecularMap
//     TB_SHADOWMAP   (5) → [[texture(5)]]  u_ShadowMap
//     TB_CUBEMAP     (6) → [[texture(6)]]  u_CubeMap
//
// Full byte layout (both host C and MSL must match exactly):
//   SHARED PREFIX (base, 448 bytes — see MetalDrawUniforms for field list)
//   Offset 448  : modelMatrix[16]          float4x4  UNIFORM_MODELMATRIX
//   Offset 512  : enableTextures[4]        float4    UNIFORM_ENABLETEXTURES
//   Offset 528  : viewOrigin[4]            float4    UNIFORM_VIEWORIGIN (vec3, w=0)
//   Offset 544  : lightOrigin[4]           float4    UNIFORM_LIGHTORIGIN (vec4)
//   Offset 560  : lightRadius              float     UNIFORM_LIGHTRADIUS
//   Offset 564  : primaryLightRadius       float     UNIFORM_PRIMARYLIGHTRADIUS
//   Offset 568  : _padLightallScalars[2]   float[2]  explicit pad
//   Offset 576  : primaryLightOrigin[4]    float4    UNIFORM_PRIMARYLIGHTORIGIN (vec4)
//   Offset 592  : primaryLightColor[4]     float4    UNIFORM_PRIMARYLIGHTCOLOR (vec3, w=0)
//   Offset 608  : primaryLightAmbient[4]   float4    UNIFORM_PRIMARYLIGHTAMBIENT (vec3, w=0)
//   Offset 624  : normalScale[4]           float4    UNIFORM_NORMALSCALE (vec4)
//   Offset 640  : specularScale[4]         float4    UNIFORM_SPECULARSCALE (vec4)
//   Offset 656  : cubeMapInfo[4]           float4    UNIFORM_CUBEMAPINFO (vec4)
//   Offset 672  : (end — 42 × 16) ✓
struct MetalLightallUniforms
{
	// bytes 0–447: identical prefix shared with generic
	MetalDrawUniforms base;

	// lightall-specific fields (byte 448 onward)
	float modelMatrix[16];          // 448 UNIFORM_MODELMATRIX      mat4 (64B)
	float enableTextures[4];        // 512 UNIFORM_ENABLETEXTURES    vec4 (16B)
	float viewOrigin[4];            // 528 UNIFORM_VIEWORIGIN        vec3→float[4] (16B)
	float lightOrigin[4];           // 544 UNIFORM_LIGHTORIGIN       vec4 (16B)
	float lightRadius;              // 560 UNIFORM_LIGHTRADIUS       float (4B)
	float primaryLightRadius;       // 564 UNIFORM_PRIMARYLIGHTRADIUS float (4B)
	float _padLightallScalars[2];   // 568 explicit pad to 576
	float primaryLightOrigin[4];    // 576 UNIFORM_PRIMARYLIGHTORIGIN vec4 (16B)
	float primaryLightColor[4];     // 592 UNIFORM_PRIMARYLIGHTCOLOR  vec3→float[4] (16B)
	float primaryLightAmbient[4];   // 608 UNIFORM_PRIMARYLIGHTAMBIENT vec3→float[4] (16B)
	float normalScale[4];           // 624 UNIFORM_NORMALSCALE       vec4 (16B)
	float specularScale[4];         // 640 UNIFORM_SPECULARSCALE     vec4 (16B)
	float cubeMapInfo[4];           // 656 UNIFORM_CUBEMAPINFO       vec4 (16B)
	                                // 672 end of struct = 42 × 16 ✓
};

// Compile-time size guard — struct must be a multiple of 16 bytes for Metal constant arrays.
static_assert( sizeof( MetalDrawUniforms ) == 464,
			   "MetalDrawUniforms must be 464 bytes (29×16): the MSL GenericDrawUniforms mirrors it" );
static_assert( sizeof( MetalLightallUniforms ) == 688,
			   "MetalLightallUniforms must be 688 bytes (43×16) to satisfy Metal constant-buffer alignment" );
static_assert( offsetof( MetalLightallUniforms, modelMatrix ) == 464,
			   "MetalLightallUniforms::modelMatrix must start at byte 464" );

// ── fogpass uniform block ─────────────────────────────────────────────────────
// GL2 ref: fogpass_vp.glsl (u_FogDistance, u_FogDepth, u_FogEyeT, u_DeformGen,
//          u_DeformParams[5], u_Time, u_ModelViewProjectionMatrix, u_Color)
//          fogpass_fp.glsl (u_Color)
//
// NOTE: u_Color here is UNIFORM_COLOR (the fog colour), NOT UNIFORM_BASECOLOR.
// Animation (u_VertexLerp / u_BoneMatrix) is DEFERRED — not present here.
//
// BINDING CONTRACT for Vasquez (MSL FogpassUniforms):
//   vertex   [[buffer(1)]] — MetalFogpassUniforms (144 bytes)
//   fragment [[buffer(0)]] — MetalFogpassUniforms (144 bytes)
//   No additional textures required by fogpass shaders.
//
// Byte layout (host C and MSL must match exactly):
//   Offset   0 : mvp[16]           float4x4  UNIFORM_MODELVIEWPROJECTIONMATRIX (64B)
//   Offset  64 : fogDistance[4]    float4    UNIFORM_FOGDISTANCE   (vec4, 16B)
//   Offset  80 : fogDepth[4]       float4    UNIFORM_FOGDEPTH      (vec4, 16B)
//   Offset  96 : color[4]          float4    UNIFORM_COLOR         (vec4, 16B)
//   Offset 112 : fogEyeT           float     UNIFORM_FOGEYET       (4B)
//   Offset 116 : deformGen         int32_t   UNIFORM_DEFORMGEN     (4B)
//   Offset 120 : deformParams[5]   float[5]  UNIFORM_DEFORMPARAMS  (20B)
//   Offset 140 : time              float     UNIFORM_TIME          (4B)
//   Offset 144 : (end — 9×16) ✓
struct MetalFogpassUniforms
{
	float mvp[16];              //   0  UNIFORM_MODELVIEWPROJECTIONMATRIX  mat4 (64B)
	float fogDistance[4];       //  64  UNIFORM_FOGDISTANCE                vec4 (16B)
	float fogDepth[4];          //  80  UNIFORM_FOGDEPTH                   vec4 (16B)
	float color[4];             //  96  UNIFORM_COLOR (fog colour)         vec4 (16B)
	float fogEyeT;              // 112  UNIFORM_FOGEYET                    float (4B)
	int32_t deformGen;          // 116  UNIFORM_DEFORMGEN                  int   (4B)
	float deformParams[5];      // 120  UNIFORM_DEFORMPARAMS               float[5] (20B)
	float time;                 // 140  UNIFORM_TIME                       float (4B)
	float vertexLerp;           // 144  UNIFORM_VERTEXLERP                 float (4B)
	float _pad[3];              // 148
	                            // 160  end of struct = 10×16 ✓
};
static_assert( sizeof( MetalFogpassUniforms ) == 160,
			   "MetalFogpassUniforms must be 160 bytes (10×16) to satisfy Metal constant-buffer alignment" );

// ── dlight uniform block ──────────────────────────────────────────────────────
// GL2 ref: dlight_vp.glsl (u_DlightInfo, u_DeformGen, u_DeformParams[5], u_Time,
//          u_Color, u_ModelViewProjectionMatrix)
//          dlight_fp.glsl (u_AlphaTest, u_DiffuseMap)
//
// NOTE: u_Color is UNIFORM_COLOR (dlight tint/modulation), not UNIFORM_BASECOLOR.
//       u_DiffuseMap sampled at [[texture(0)]] by fragment stage (see Vasquez contract).
//
// BINDING CONTRACT for Vasquez (MSL DlightUniforms):
//   vertex   [[buffer(1)]] — MetalDlightUniforms (128 bytes)
//   fragment [[buffer(0)]] — MetalDlightUniforms (128 bytes)
//   fragment [[texture(0)]] — u_DiffuseMap (TB_DIFFUSEMAP slot 0)
//
// Byte layout (host C and MSL must match exactly):
//   Offset   0 : mvp[16]           float4x4  UNIFORM_MODELVIEWPROJECTIONMATRIX (64B)
//   Offset  64 : dlightInfo[4]     float4    UNIFORM_DLIGHTINFO    (vec4, 16B)
//   Offset  80 : color[4]          float4    UNIFORM_COLOR         (vec4, 16B)
//   Offset  96 : deformGen         int32_t   UNIFORM_DEFORMGEN     (4B)
//   Offset 100 : alphaTest         int32_t   UNIFORM_ALPHATEST     (4B)
//   Offset 104 : deformParams[5]   float[5]  UNIFORM_DEFORMPARAMS  (20B)
//   Offset 124 : time              float     UNIFORM_TIME          (4B)
//   Offset 128 : (end — 8×16) ✓
struct MetalDlightUniforms
{
	float mvp[16];              //   0  UNIFORM_MODELVIEWPROJECTIONMATRIX  mat4 (64B)
	float dlightInfo[4];        //  64  UNIFORM_DLIGHTINFO                 vec4 (16B)
	float color[4];             //  80  UNIFORM_COLOR (dlight tint)        vec4 (16B)
	int32_t deformGen;          //  96  UNIFORM_DEFORMGEN                  int   (4B)
	int32_t alphaTest;          // 100  UNIFORM_ALPHATEST                  int   (4B)
	float deformParams[5];      // 104  UNIFORM_DEFORMPARAMS               float[5] (20B)
	float time;                 // 124  UNIFORM_TIME                       float (4B)
	                            // 128  end of struct = 8×16 ✓
};
static_assert( sizeof( MetalDlightUniforms ) == 128,
			   "MetalDlightUniforms must be 128 bytes (8×16) to satisfy Metal constant-buffer alignment" );

// ── pshadow uniform block ────────────────────────────────────────────────────
// GL2 ref: pshadow_vp.glsl / pshadow_fp.glsl uniform set (tr_shade.c:1004-1032)
// Per-object projected shadow CONSUMER: projects shadow mask onto lit surfaces.
//
// BINDING CONTRACT (MSL PshadowUniforms must be byte-identical):
//   vertex   [[buffer(1)]] — MetalPshadowUniforms (160 bytes)
//   fragment [[buffer(0)]] — MetalPshadowUniforms (160 bytes)
//   fragment [[texture(0)]] — u_ShadowMap (TB_DIFFUSEMAP slot 0, depth2d<float>)
//
// Byte layout:
//   Offset   0 : mvp[16]           float4x4  UNIFORM_MODELVIEWPROJECTIONMATRIX (64B)
//   Offset  64 : lightOrigin[4]    float4    UNIFORM_LIGHTORIGIN    (vec4, 16B)
//   Offset  80 : lightForward[4]   float4    UNIFORM_LIGHTFORWARD   (vec3→vec4, 16B)
//   Offset  96 : lightRight[4]     float4    UNIFORM_LIGHTRIGHT     (vec3→vec4, 16B)
//   Offset 112 : lightUp[4]        float4    UNIFORM_LIGHTUP        (vec3→vec4, 16B)
//   Offset 128 : lightRadius       float     UNIFORM_LIGHTRADIUS    (4B)
//   Offset 132 : alphaTest         int32_t   UNIFORM_ALPHATEST      (4B)
//   Offset 136 : _pad[6]           float[6]  padding to 160         (24B)
//   Offset 160 : (end — 10×16) ✓
struct MetalPshadowUniforms
{
	float mvp[16];              //   0  UNIFORM_MODELVIEWPROJECTIONMATRIX  mat4 (64B)
	float lightOrigin[4];       //  64  UNIFORM_LIGHTORIGIN                vec4 (16B)
	float lightForward[4];      //  80  UNIFORM_LIGHTFORWARD               vec3→vec4 (16B)
	float lightRight[4];        //  96  UNIFORM_LIGHTRIGHT                 vec3→vec4 (16B)
	float lightUp[4];           // 112  UNIFORM_LIGHTUP                    vec3→vec4 (16B)
	float lightRadius;          // 128  UNIFORM_LIGHTRADIUS                float (4B)
	int32_t alphaTest;          // 132  UNIFORM_ALPHATEST                  int (4B)
	float _pad[6];              // 136  padding                            (24B)
	                            // 160  end of struct = 10×16 ✓
};
static_assert( sizeof( MetalPshadowUniforms ) == 160,
			   "MetalPshadowUniforms must be 160 bytes (10×16) to satisfy Metal constant-buffer alignment" );

// ── shadowfill uniform block ─────────────────────────────────────────────────
// GL2 ref: shadowfill_vp.glsl / shadowfill_fp.glsl (tr_shade.c:1565-1612)
// Depth-only PRODUCER: renders occluder geometry into pshadow map depth.
//
// BINDING CONTRACT (MSL ShadowfillUniforms must be byte-identical):
//   vertex   [[buffer(1)]] — MetalShadowfillUniforms (192 bytes)
//   fragment [[buffer(0)]] — MetalShadowfillUniforms (192 bytes)
//   No textures needed (depth-only pass).
//
// Byte layout:
//   Offset   0 : mvp[16]           float4x4  UNIFORM_MODELVIEWPROJECTIONMATRIX (64B)
//   Offset  64 : modelMatrix[16]   float4x4  UNIFORM_MODELMATRIX               (64B)
//   Offset 128 : lightOrigin[4]    float4    UNIFORM_LIGHTORIGIN               (16B)
//   Offset 144 : lightRadius       float     UNIFORM_LIGHTRADIUS               (4B)
//   Offset 148 : deformGen         int32_t   UNIFORM_DEFORMGEN                 (4B)
//   Offset 152 : deformParams[5]   float[5]  UNIFORM_DEFORMPARAMS              (20B)
//   Offset 172 : time              float     UNIFORM_TIME                      (4B)
//   Offset 176 : alphaTest         int32_t   UNIFORM_ALPHATEST                 (4B)
//   Offset 180 : vertexLerp        float     UNIFORM_VERTEXLERP                (4B)
//   Offset 184 : _pad[2]           float[2]  padding to 192                    (8B)
//   Offset 192 : (end — 12×16) ✓
struct MetalShadowfillUniforms
{
	float mvp[16];              //   0  UNIFORM_MODELVIEWPROJECTIONMATRIX  mat4 (64B)
	float modelMatrix[16];      //  64  UNIFORM_MODELMATRIX                mat4 (64B)
	float lightOrigin[4];       // 128  UNIFORM_LIGHTORIGIN                vec4 (16B)
	float lightRadius;          // 144  UNIFORM_LIGHTRADIUS                float (4B)
	int32_t deformGen;          // 148  UNIFORM_DEFORMGEN                  int (4B)
	float deformParams[5];      // 152  UNIFORM_DEFORMPARAMS               float[5] (20B)
	float time;                 // 172  UNIFORM_TIME                       float (4B)
	int32_t alphaTest;          // 176  UNIFORM_ALPHATEST                  int (4B)
	float vertexLerp;           // 180  UNIFORM_VERTEXLERP                 float (4B)
	float _pad[2];              // 184  padding                            (8B)
	                            // 192  end of struct = 12×16 ✓
};
static_assert( sizeof( MetalShadowfillUniforms ) == 192,
			   "MetalShadowfillUniforms must be 192 bytes (12×16) to satisfy Metal constant-buffer alignment" );

// ── postprocess fullscreen-blit uniform block ────────────────────────────────
// GL2 ref: tr_fbo.c:584-588 (FBO_BlitFromTexture uniform set)
// MetalPostprocessUniforms is declared in tr_local.h so Hudson can use the same
// host layout from device.mm.  Vasquez's MSL twin must be byte-identical.
//
// BINDING CONTRACT for Vasquez/Hudson:
//   vertex   [[buffer(1)]] — MetalPostprocessUniforms (112 bytes)
//   fragment [[buffer(0)]] — MetalPostprocessUniforms (112 bytes)
//   fragment [[texture(0)]] — source color texture (TB_COLORMAP)
//
// Byte layout (host C/C++ and MSL must match exactly):
//   Offset   0 : mvp[16]                 float4x4  UNIFORM_MODELVIEWPROJECTIONMATRIX (64B)
//   Offset  64 : color[4]                float4    UNIFORM_COLOR                     (16B)
//   Offset  80 : invTexRes[2]            float2    UNIFORM_INVTEXRES                  (8B)
//   Offset  88 : autoExposureMinMax[2]   float2    UNIFORM_AUTOEXPOSUREMINMAX         (8B)
//                ^ offset 80–95 is one packed 16-byte float4 slot
//   Offset  96 : toneMinAvgMaxLinear[4]  float4    UNIFORM_TONEMINAVGMAXLINEAR vec3, w=0 (16B)
//   Offset 112 : (end — 7×16) ✓
static_assert( sizeof( MetalPostprocessUniforms ) == 112,
			   "MetalPostprocessUniforms must be 112 bytes (7×16) to satisfy Metal constant-buffer alignment" );

// GL2 ref: tr_glsl.c:676-724 (uniform location/cache lookup)
static const void *R_MetalUniformPtr( shaderProgram_t *prog, int uniformNum ) {
	return prog->uniformBuffer + prog->uniformBufferOffsets[uniformNum];
}

// GL2 ref: tr_glsl.c:752-898 (qglProgramUniform* cache consumed before qglDrawElements)
static void R_FillMetalDrawUniforms( shaderProgram_t *prog, MetalDrawUniforms *out ) {
	Com_Memset( out, 0, sizeof(*out ) );

	// Metal draw-time upload equivalent of GL2's per-uniform glUniform state:
	// collect the CPU uniform cache into one C-layout constant block for both
	// generic and lightall shaders so new permutations can consume fields without
	// adding per-field encoder calls.
	memcpy( out->mvp, R_MetalUniformPtr( prog, UNIFORM_MODELVIEWPROJECTIONMATRIX ), sizeof( out->mvp ) );
	memcpy( out->baseColor, R_MetalUniformPtr( prog, UNIFORM_BASECOLOR ), sizeof( out->baseColor ) );
	memcpy( out->vertColor, R_MetalUniformPtr( prog, UNIFORM_VERTCOLOR ), sizeof( out->vertColor ) );
	memcpy( out->diffuseTexMatrix[0], R_MetalUniformPtr( prog, UNIFORM_DIFFUSETEXMATRIX0 ), sizeof( out->diffuseTexMatrix ) );

	qboolean haveTexMatrix = qfalse;
	for (int i = 0; i < 8 && !haveTexMatrix; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			if ( out->diffuseTexMatrix[i][j] != 0.0f ) {
				haveTexMatrix = qtrue;
				break;
			}
		}
	}
	if ( !haveTexMatrix ) {
		for (int i = 0; i < 8; i += 2)
		{
			out->diffuseTexMatrix[i][0] = 1.0f;
			out->diffuseTexMatrix[i + 1][1] = 1.0f;
		}
	}

	out->time = tess.shaderTime;
	out->deformGen = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_DEFORMGEN );
	memcpy( out->deformParams, R_MetalUniformPtr( prog, UNIFORM_DEFORMPARAMS ), sizeof( out->deformParams ) );
	out->colorGen = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_COLORGEN );
	out->alphaGen = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_ALPHAGEN );
	out->tcGen0 = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_TCGEN0 );
	memcpy( out->tcGen0Vector0, R_MetalUniformPtr( prog, UNIFORM_TCGEN0VECTOR0 ), sizeof( vec3_t ) );
	memcpy( out->tcGen0Vector1, R_MetalUniformPtr( prog, UNIFORM_TCGEN0VECTOR1 ), sizeof( vec3_t ) );
	memcpy( out->localViewOrigin, R_MetalUniformPtr( prog, UNIFORM_LOCALVIEWORIGIN ), sizeof( vec3_t ) );
	out->alphaTest = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_ALPHATEST );

	// RGBAGEN uniforms — vec3 GL sources: copy sizeof(vec3_t)=12 bytes; w stays 0 (from Com_Memset).
	memcpy( out->ambientLight,  R_MetalUniformPtr( prog, UNIFORM_AMBIENTLIGHT ),  sizeof( vec3_t ) );
	memcpy( out->directedLight, R_MetalUniformPtr( prog, UNIFORM_DIRECTEDLIGHT ), sizeof( vec3_t ) );
	memcpy( out->modelLightDir, R_MetalUniformPtr( prog, UNIFORM_MODELLIGHTDIR ), sizeof( vec3_t ) );
	out->portalRange = *(const float *)R_MetalUniformPtr( prog, UNIFORM_PORTALRANGE );

	// GL2 ref: generic_vp.glsl:67 / lightall_vp.glsl:59 (u_VertexLerp)
	out->vertexLerp = *(const float *)R_MetalUniformPtr( prog, UNIFORM_VERTEXLERP );

	// FOG uniforms — vec4 GL sources: copy full 16 bytes; fogEyeT is a scalar float.
	memcpy( out->fogDistance,  R_MetalUniformPtr( prog, UNIFORM_FOGDISTANCE ),  sizeof( out->fogDistance ) );
	memcpy( out->fogDepth,     R_MetalUniformPtr( prog, UNIFORM_FOGDEPTH ),     sizeof( out->fogDepth ) );
	memcpy( out->fogColorMask, R_MetalUniformPtr( prog, UNIFORM_FOGCOLORMASK ), sizeof( out->fogColorMask ) );
	out->fogEyeT = *(const float *)R_MetalUniformPtr( prog, UNIFORM_FOGEYET );

	// RTCW: fire rise direction (vertex deform, AGEN_NORMALZFADE) and the z fade range
	memcpy( out->fireRiseDir, R_MetalUniformPtr( prog, UNIFORM_FIRERISEDIR ), sizeof( vec3_t ) );
	out->zFadeLowest = *(const float *)R_MetalUniformPtr( prog, UNIFORM_ZFADELOWEST );
	out->fireRiseDir[3] = *(const float *)R_MetalUniformPtr( prog, UNIFORM_ZFADEHIGHEST );
}

// GL2 ref: tr_glsl.c:752-898 (per-uniform GL uploads; lightall superset)
// Fills MetalLightallUniforms: first delegates the 448-byte generic prefix to
// R_FillMetalDrawUniforms, then copies the 12 lightall-specific fields.
// vec3 uniforms occupy float[4] slots (w left 0 by Com_Memset via base fill).
static void R_FillMetalLightallUniforms( shaderProgram_t *prog, MetalLightallUniforms *out ) {
	Com_Memset( out, 0, sizeof(*out ) );

	// Reuse generic fill for the shared 448-byte prefix (mvp, colors, tex matrices,
	// deform, tcgen, fog, rgbagen, etc.) — field layout is byte-identical.
	R_FillMetalDrawUniforms( prog, &out->base );

	// UNIFORM_MODELMATRIX — mat4 (64 bytes)
	memcpy( out->modelMatrix, R_MetalUniformPtr( prog, UNIFORM_MODELMATRIX ), sizeof( out->modelMatrix ) );

	// UNIFORM_ENABLETEXTURES — vec4 (16 bytes)
	memcpy( out->enableTextures, R_MetalUniformPtr( prog, UNIFORM_ENABLETEXTURES ), sizeof( out->enableTextures ) );

	// UNIFORM_VIEWORIGIN — vec3 (copy 12 bytes; w=0 from Com_Memset)
	memcpy( out->viewOrigin, R_MetalUniformPtr( prog, UNIFORM_VIEWORIGIN ), sizeof( vec3_t ) );

	// UNIFORM_LIGHTORIGIN — vec4 (16 bytes)
	memcpy( out->lightOrigin, R_MetalUniformPtr( prog, UNIFORM_LIGHTORIGIN ), sizeof( out->lightOrigin ) );

	// UNIFORM_LIGHTRADIUS — float (4 bytes)
	out->lightRadius = *(const float *)R_MetalUniformPtr( prog, UNIFORM_LIGHTRADIUS );

	// UNIFORM_PRIMARYLIGHTRADIUS — float (4 bytes)
	out->primaryLightRadius = *(const float *)R_MetalUniformPtr( prog, UNIFORM_PRIMARYLIGHTRADIUS );

	// UNIFORM_PRIMARYLIGHTORIGIN — vec4 (16 bytes)
	memcpy( out->primaryLightOrigin, R_MetalUniformPtr( prog, UNIFORM_PRIMARYLIGHTORIGIN ), sizeof( out->primaryLightOrigin ) );

	// UNIFORM_PRIMARYLIGHTCOLOR — vec3 (12 bytes; w=0)
	memcpy( out->primaryLightColor, R_MetalUniformPtr( prog, UNIFORM_PRIMARYLIGHTCOLOR ), sizeof( vec3_t ) );

	// UNIFORM_PRIMARYLIGHTAMBIENT — vec3 (12 bytes; w=0)
	memcpy( out->primaryLightAmbient, R_MetalUniformPtr( prog, UNIFORM_PRIMARYLIGHTAMBIENT ), sizeof( vec3_t ) );

	// UNIFORM_NORMALSCALE — vec4 (16 bytes)
	memcpy( out->normalScale, R_MetalUniformPtr( prog, UNIFORM_NORMALSCALE ), sizeof( out->normalScale ) );

	// UNIFORM_SPECULARSCALE — vec4 (16 bytes)
	memcpy( out->specularScale, R_MetalUniformPtr( prog, UNIFORM_SPECULARSCALE ), sizeof( out->specularScale ) );

	// UNIFORM_CUBEMAPINFO — vec4 (16 bytes)
	memcpy( out->cubeMapInfo, R_MetalUniformPtr( prog, UNIFORM_CUBEMAPINFO ), sizeof( out->cubeMapInfo ) );
}

// GL2 ref: fogpass_vp.glsl / fogpass_fp.glsl uniform set
// Fills MetalFogpassUniforms from the GL2 CPU uniform cache.
// Animation fields (u_VertexLerp, u_BoneMatrix) are DEFERRED — not filled here.
// u_Color = UNIFORM_COLOR (fog colour, distinct from UNIFORM_BASECOLOR).
static void R_FillMetalFogpassUniforms( shaderProgram_t *prog, MetalFogpassUniforms *out ) {
	Com_Memset( out, 0, sizeof(*out ) );

	// UNIFORM_MODELVIEWPROJECTIONMATRIX — mat4 (64 bytes)
	memcpy( out->mvp, R_MetalUniformPtr( prog, UNIFORM_MODELVIEWPROJECTIONMATRIX ), sizeof( out->mvp ) );

	// UNIFORM_FOGDISTANCE — vec4 (16 bytes)
	memcpy( out->fogDistance, R_MetalUniformPtr( prog, UNIFORM_FOGDISTANCE ), sizeof( out->fogDistance ) );

	// UNIFORM_FOGDEPTH — vec4 (16 bytes)
	memcpy( out->fogDepth, R_MetalUniformPtr( prog, UNIFORM_FOGDEPTH ), sizeof( out->fogDepth ) );

	// UNIFORM_COLOR — vec4 (16 bytes); fog colour, NOT baseColor
	memcpy( out->color, R_MetalUniformPtr( prog, UNIFORM_COLOR ), sizeof( out->color ) );

	// UNIFORM_FOGEYET — float (4 bytes)
	out->fogEyeT = *(const float *)R_MetalUniformPtr( prog, UNIFORM_FOGEYET );

	// UNIFORM_DEFORMGEN — int (4 bytes)
	out->deformGen = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_DEFORMGEN );

	// UNIFORM_DEFORMPARAMS — float[5] (20 bytes)
	memcpy( out->deformParams, R_MetalUniformPtr( prog, UNIFORM_DEFORMPARAMS ), sizeof( out->deformParams ) );

	// UNIFORM_TIME — float (4 bytes)
	out->time = tess.shaderTime;

	// UNIFORM_VERTEXLERP — float (4 bytes)
	out->vertexLerp = *(const float *)R_MetalUniformPtr( prog, UNIFORM_VERTEXLERP );
}

// GL2 ref: dlight_vp.glsl / dlight_fp.glsl uniform set
// Fills MetalDlightUniforms from the GL2 CPU uniform cache.
// u_Color = UNIFORM_COLOR (dlight tint/modulation, not UNIFORM_BASECOLOR).
// u_DiffuseMap texture bound via GLMetal_BindTexturesToCurrentEncoder at [[texture(0)]].
static void R_FillMetalDlightUniforms( shaderProgram_t *prog, MetalDlightUniforms *out ) {
	Com_Memset( out, 0, sizeof(*out ) );

	// UNIFORM_MODELVIEWPROJECTIONMATRIX — mat4 (64 bytes)
	memcpy( out->mvp, R_MetalUniformPtr( prog, UNIFORM_MODELVIEWPROJECTIONMATRIX ), sizeof( out->mvp ) );

	// UNIFORM_DLIGHTINFO — vec4 (16 bytes); xyz=light pos, w=1/radius
	memcpy( out->dlightInfo, R_MetalUniformPtr( prog, UNIFORM_DLIGHTINFO ), sizeof( out->dlightInfo ) );

	// UNIFORM_COLOR — vec4 (16 bytes); dlight tint/modulation colour
	memcpy( out->color, R_MetalUniformPtr( prog, UNIFORM_COLOR ), sizeof( out->color ) );

	// UNIFORM_DEFORMGEN — int (4 bytes)
	out->deformGen = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_DEFORMGEN );

	// UNIFORM_ALPHATEST — int (4 bytes); consumed by fragment stage
	out->alphaTest = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_ALPHATEST );

	// UNIFORM_DEFORMPARAMS — float[5] (20 bytes)
	memcpy( out->deformParams, R_MetalUniformPtr( prog, UNIFORM_DEFORMPARAMS ), sizeof( out->deformParams ) );

	// UNIFORM_TIME — float (4 bytes)
	out->time = tess.shaderTime;
}

// GL2 ref: tr_shade.c:1004-1032 (pshadow uniform set before draw)
// Fills MetalPshadowUniforms from the GL2 CPU uniform cache.
// u_ShadowMap bound via GLMetal_BindTexturesToCurrentEncoder at [[texture(0)]].
static void R_FillMetalPshadowUniforms( shaderProgram_t *prog, MetalPshadowUniforms *out ) {
	Com_Memset( out, 0, sizeof(*out ) );

	// UNIFORM_MODELVIEWPROJECTIONMATRIX — mat4 (64 bytes)
	memcpy( out->mvp, R_MetalUniformPtr( prog, UNIFORM_MODELVIEWPROJECTIONMATRIX ), sizeof( out->mvp ) );

	// UNIFORM_LIGHTORIGIN — vec4 (16 bytes)
	memcpy( out->lightOrigin, R_MetalUniformPtr( prog, UNIFORM_LIGHTORIGIN ), sizeof( out->lightOrigin ) );

	// UNIFORM_LIGHTFORWARD — vec3 stored as vec4 (16 bytes, w=0)
	memcpy( out->lightForward, R_MetalUniformPtr( prog, UNIFORM_LIGHTFORWARD ), 3 * sizeof( float ) );

	// UNIFORM_LIGHTRIGHT — vec3 stored as vec4 (16 bytes, w=0)
	memcpy( out->lightRight, R_MetalUniformPtr( prog, UNIFORM_LIGHTRIGHT ), 3 * sizeof( float ) );

	// UNIFORM_LIGHTUP — vec3 stored as vec4 (16 bytes, w=0)
	memcpy( out->lightUp, R_MetalUniformPtr( prog, UNIFORM_LIGHTUP ), 3 * sizeof( float ) );

	// UNIFORM_LIGHTRADIUS — float (4 bytes)
	out->lightRadius = *(const float *)R_MetalUniformPtr( prog, UNIFORM_LIGHTRADIUS );

	// UNIFORM_ALPHATEST — int (4 bytes)
	out->alphaTest = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_ALPHATEST );
}

// GL2 ref: tr_shade.c:1565-1612 (shadowfill uniform set before draw)
// Fills MetalShadowfillUniforms from the GL2 CPU uniform cache.
static void R_FillMetalShadowfillUniforms( shaderProgram_t *prog, MetalShadowfillUniforms *out ) {
	Com_Memset( out, 0, sizeof(*out ) );

	// UNIFORM_MODELVIEWPROJECTIONMATRIX — mat4 (64 bytes)
	memcpy( out->mvp, R_MetalUniformPtr( prog, UNIFORM_MODELVIEWPROJECTIONMATRIX ), sizeof( out->mvp ) );

	// UNIFORM_MODELMATRIX — mat4 (64 bytes)
	memcpy( out->modelMatrix, R_MetalUniformPtr( prog, UNIFORM_MODELMATRIX ), sizeof( out->modelMatrix ) );

	// UNIFORM_LIGHTORIGIN — vec4 (16 bytes)
	memcpy( out->lightOrigin, R_MetalUniformPtr( prog, UNIFORM_LIGHTORIGIN ), sizeof( out->lightOrigin ) );

	// UNIFORM_LIGHTRADIUS — float (4 bytes)
	out->lightRadius = *(const float *)R_MetalUniformPtr( prog, UNIFORM_LIGHTRADIUS );

	// UNIFORM_DEFORMGEN — int (4 bytes)
	out->deformGen = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_DEFORMGEN );

	// UNIFORM_DEFORMPARAMS — float[5] (20 bytes)
	memcpy( out->deformParams, R_MetalUniformPtr( prog, UNIFORM_DEFORMPARAMS ), sizeof( out->deformParams ) );

	// UNIFORM_TIME — float (4 bytes)
	out->time = tess.shaderTime;

	// UNIFORM_ALPHATEST — int (4 bytes)
	out->alphaTest = *(const int32_t *)R_MetalUniformPtr( prog, UNIFORM_ALPHATEST );

	// UNIFORM_VERTEXLERP — float (4 bytes)
	out->vertexLerp = *(const float *)R_MetalUniformPtr( prog, UNIFORM_VERTEXLERP );
}

// GL2 ref: tr_fbo.c:584-588 (FBO_BlitFromTexture postprocess uniform set)
// Fills MetalPostprocessUniforms from the GL2 CPU uniform cache for fullscreen blits.
extern "C" void R_FillMetalPostprocessUniforms( shaderProgram_t *program, MetalPostprocessUniforms *out ) {
	Com_Memset( out, 0, sizeof(*out ) );

	// UNIFORM_MODELVIEWPROJECTIONMATRIX — mat4 (64 bytes)
	memcpy( out->mvp, R_MetalUniformPtr( program, UNIFORM_MODELVIEWPROJECTIONMATRIX ), sizeof( out->mvp ) );

	// UNIFORM_COLOR — vec4 (16 bytes)
	memcpy( out->color, R_MetalUniformPtr( program, UNIFORM_COLOR ), sizeof( out->color ) );

	// UNIFORM_INVTEXRES + UNIFORM_AUTOEXPOSUREMINMAX — two vec2 values packed into one 16-byte slot
	memcpy( out->invTexRes, R_MetalUniformPtr( program, UNIFORM_INVTEXRES ), sizeof( out->invTexRes ) );
	memcpy( out->autoExposureMinMax, R_MetalUniformPtr( program, UNIFORM_AUTOEXPOSUREMINMAX ), sizeof( out->autoExposureMinMax ) );

	// UNIFORM_TONEMINAVGMAXLINEAR — vec3 (copy 12 bytes; w=0 from Com_Memset)
	memcpy( out->toneMinAvgMaxLinear, R_MetalUniformPtr( program, UNIFORM_TONEMINAVGMAXLINEAR ), sizeof( vec3_t ) );
}

// GL-COMPAT: GL constants used in PARITY-SEAM blocks.  Guard-defined per-TU
// so this file compiles without OpenGL headers.

#ifndef GL_TRIANGLES
#define GL_TRIANGLES              0x0004
#endif
#ifndef GL_LINES
#define GL_LINES                  0x0001
#endif
#ifndef GL_FALSE
#define GL_FALSE                  0
#endif
#ifndef GL_SHORT
#define GL_SHORT                  0x1402
#endif
#ifndef GL_FLOAT
#define GL_FLOAT                  0x1406
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER           0x8892
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW            0x88E0
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM        0x8B8D
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING   0x85B5
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING   0x8894
#endif
#ifndef GL_DEPTH_WRITEMASK
#define GL_DEPTH_WRITEMASK        0x0B72
#endif
#ifndef GL_POLYGON_OFFSET_FILL
#define GL_POLYGON_OFFSET_FILL    0x8037
#endif

// GL-COMPAT: GLsizeiptr — pointer-sized integer for buffer offsets/sizes.
// Not declared in tr_local.h; matches OpenGL spec (ptrdiff_t width).
#ifndef GL_SIZEIPTR_DEFINED
#define GL_SIZEIPTR_DEFINED
typedef ptrdiff_t GLsizeiptr;
#endif


/*

  THIS ENTIRE FILE IS BACK END

  This file deals with applying shaders to surface data in the tess struct.
*/


/*
==================
R_DrawElements

==================
*/

// ── Per-surface GPU upload cache ─────────────────────────────────────────────
// Every stage/dlight/fog/pshadow pass of one surface draws the same tess vertex and index
// data (per-stage colors and texcoords are computed in the shaders). Inside
// RB_StageIteratorGeneric the streams are therefore copied into frame memory once and the
// same buffer ranges are rebound for the following passes, instead of re-copying (and
// formerly allocating a new MTLBuffer for) every stream on every pass.
// Outside that scope (2D quads, debug draws, sky sides) tess may change between draws, so
// data is always uploaded fresh.
#define METAL_TESS_STREAM_SLOTS 12
static struct {
	qboolean active;
	int numVertexes;
	qboolean streamValid[METAL_TESS_STREAM_SLOTS];
	MTL::Buffer *streamBuffer[METAL_TESS_STREAM_SLOTS];
	NS::UInteger streamOffset[METAL_TESS_STREAM_SLOTS];
	qboolean indexValid;
	int indexFirst;
	int indexCount;
	MTL::Buffer *indexBuffer;
	NS::UInteger indexOffset;
} s_tessUpload;

static void R_MetalBeginTessUploadScope( void ) {
	Com_Memset( &s_tessUpload, 0, sizeof( s_tessUpload ) );
	s_tessUpload.active = qtrue;
}

static void R_MetalEndTessUploadScope( void ) {
	Com_Memset( &s_tessUpload, 0, sizeof( s_tessUpload ) );
}

// aliasSlot >= 0 binds the same range at a second slot: tess holds one (already lerped)
// frame, so the old-frame attributes of vertex-animation permutations read the same data.
static void R_MetalBindTessStream( int slot, const void *data, size_t stride, int numVertexes, int aliasSlot = -1 ) {
	if ( s_tessUpload.active && s_tessUpload.numVertexes != numVertexes ) {
		// Defensive: the vertex count changed inside the scope, drop cached streams.
		Com_Memset( s_tessUpload.streamValid, 0, sizeof( s_tessUpload.streamValid ) );
		s_tessUpload.indexValid = qfalse;
		s_tessUpload.numVertexes = numVertexes;
	}

	if ( s_tessUpload.active && s_tessUpload.streamValid[slot] ) {
		GLMetal_SetVertexBuffer( s_tessUpload.streamBuffer[slot], s_tessUpload.streamOffset[slot], (NS::UInteger)slot );
		if ( aliasSlot >= 0 ) {
			GLMetal_SetVertexBuffer( s_tessUpload.streamBuffer[slot], s_tessUpload.streamOffset[slot], (NS::UInteger)aliasSlot );
		}
		return;
	}

	const size_t size = (size_t)numVertexes * stride;
	MTL::Buffer *buffer = NULL;
	NS::UInteger offset = 0;
	void *dst = GLMetal_FrameAlloc( size, 16, &buffer, &offset );
	if ( !dst ) {
		return;
	}
	memcpy( dst, data, size );
	GLMetal_SetVertexBuffer( buffer, offset, (NS::UInteger)slot );
	if ( aliasSlot >= 0 ) {
		GLMetal_SetVertexBuffer( buffer, offset, (NS::UInteger)aliasSlot );
	}

	if ( s_tessUpload.active ) {
		s_tessUpload.streamValid[slot]  = qtrue;
		s_tessUpload.streamBuffer[slot] = buffer;
		s_tessUpload.streamOffset[slot] = offset;
	}
}

static qboolean R_MetalGetTessIndices( int numIndexes, int firstIndex, MTL::Buffer **outBuffer, NS::UInteger *outOffset ) {
	if ( s_tessUpload.active && s_tessUpload.indexValid &&
		 s_tessUpload.indexFirst == firstIndex && s_tessUpload.indexCount == numIndexes ) {
		*outBuffer = s_tessUpload.indexBuffer;
		*outOffset = s_tessUpload.indexOffset;
		return qtrue;
	}

	const size_t size = (size_t)numIndexes * sizeof( glIndex_t );
	void *dst = GLMetal_FrameAlloc( size, 16, outBuffer, outOffset );
	if ( !dst ) {
		return qfalse;
	}
	memcpy( dst, tess.indexes + firstIndex, size );

	if ( s_tessUpload.active ) {
		s_tessUpload.indexValid  = qtrue;
		s_tessUpload.indexFirst  = firstIndex;
		s_tessUpload.indexCount  = numIndexes;
		s_tessUpload.indexBuffer = *outBuffer;
		s_tessUpload.indexOffset = *outOffset;
	}
	return qtrue;
}

// Bind the streams of a static model VAO (see R_MetalCreateMdvSurfaceBuffer) for the
// attributes the program's vertex descriptor declares. The arrays use the tess formats, so
// the slots are the same as for tess draws; slots 9-11 carry the old frame for vertex
// animation. Attributes the model does not have read constant data, like a disabled GL
// vertex array. GL2 ref: tr_surface.c:1247-1290 (qglVertexAttribPointer with frame offsets).
static void R_MetalBindVaoStreams( const vao_t *vao, uint32_t attribs ) {
	MTL::Buffer *buffer = (MTL::Buffer *)vao->metalBuffer;
	const NS::UInteger nv = vao->metalNumVerts;
	NS::UInteger frame = 0, oldFrame = 0;

	if ( vao->metalNumFrames > 1 ) {
		const refEntity_t *refEnt = &backEnd.currentEntity->e;
		frame    = (NS::UInteger)refEnt->frame    % vao->metalNumFrames;
		oldFrame = (NS::UInteger)refEnt->oldframe % vao->metalNumFrames;
	}

	const NS::UInteger xyzStride = sizeof( vec4_t ), packedStride = sizeof( int16_t ) * 4;

	if ( attribs & ATTR_POSITION ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsXyz + frame * nv * xyzStride, 2 );
	}
	if ( attribs & ATTR_TEXCOORD ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsSt, 3 );
	}
	if ( attribs & ATTR_NORMAL ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsNormal + frame * nv * packedStride, 4 );
	}
	if ( attribs & ATTR_COLOR ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsWhite, 5 );
	}
	if ( attribs & ATTR_LIGHTCOORD ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsZero, 6 );
	}
	if ( attribs & ATTR_TANGENT ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsTangent + frame * nv * packedStride, 7 );
	}
	if ( attribs & ATTR_LIGHTDIRECTION ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsZero, 8 );
	}
	if ( attribs & ATTR_POSITION2 ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsXyz + oldFrame * nv * xyzStride, 9 );
	}
	if ( attribs & ATTR_NORMAL2 ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsNormal + oldFrame * nv * packedStride, 10 );
	}
	if ( attribs & ATTR_TANGENT2 ) {
		GLMetal_SetVertexBuffer( buffer, vao->metalOfsTangent + oldFrame * nv * packedStride, 11 );
	}
}

void R_DrawElements( int numIndexes, int firstIndex ) {
	if ( tess.useCacheVao ) {
		VaoCache_DrawElements( numIndexes, firstIndex );
		return;
	}

	// PHASE5-M5.2: Metal draw path.
	// GL2 original: qglDrawElements(GL_TRIANGLES, ...) — GL2 ref: tr_shade.c:49
	// Metal path: look up the active program; if it has a compiled pipeline, encode the draw.
	// If metalPipeline is NULL (deferred shader) or no encoder is available, skip silently.
	if ( numIndexes <= 0 || tess.numVertexes <= 0 ) {
		return;
	}

	shaderProgram_t *prog = GLSL_GetActiveMetalProgram();
	if ( !prog ) {
		return;
	}
	if ( !prog->metalPipeline ) {
		return;
	}

	MTL::RenderCommandEncoder *enc = GLMetal_CurrentRenderEncoder();
	if ( !enc ) {
		return;
	}

	// PSO for this program permutation under the current blend state and render-target
	// formats. The program id indexes a small per-program cache (no string keys per draw).
	// There is deliberately no fallback to prog->metalPipeline: that PSO is built for the
	// scene FBO formats, and binding it to a pass with other attachment formats is invalid.
	// GL2 ref: glUseProgram(program) discriminates permutations by GL program object ID.
	MTL::RenderPipelineState *pso;
	if ( prog->metalProgramId >= 0 ) {
		pso = GLMetal_GetPipelineForProgram( prog->metalProgramId, glState.glStateBits );
	} else
	{
		const char *pipelineKey = ( prog->metalProgramKey[0] ) ? prog->metalProgramKey : prog->name;
		pso = GLMetal_GetPipelineForState( pipelineKey, GLMetal_VertexDescHashForAttribs( prog->attribs ), glState.glStateBits );
	}
	if ( !pso ) {
		return;
	}
	GLMetal_SetRenderPipelineState( pso );
	GLMetal_SetDepthState( glState.glStateBits );
	// GL2 ref: GL_Cull face-culling state (tr_backend.c:GL_Cull). Map tracked
	// glState.faceCulling to the Metal cull mode (0=none,1=front,2=back).
	{
		int _cm = ( glState.faceCulling == CT_TWO_SIDED ) ? 0
				: ( glState.faceCulling == CT_FRONT_SIDED ) ? 1 : 2;
		GLMetal_SetCullMode( _cm );
	}

	// ── Vertex attribute data — separate buffer slots 2-8 ────────────────
	// Slot assignment matches GLSL_BuildMetalVertexDescriptor in tr_glsl.cpp.
	// Streams live in per-frame memory (see R_MetalBindTessStream).
	// GL2 ref: qglBindVertexArray / qglVertexAttribPointer (tr_shade.c:~100)
	const uint32_t attribs = prog->attribs;
	const int nv = tess.numVertexes;

	// Static model surface (RB_SurfaceVaoMdvMesh): vertex data comes from the VAO's GPU
	// buffer, not from tess. GL2 ref: tr_surface.c:1233-1290 (R_BindVao + frame offsets).
	const vao_t *staticVao = tess.metalStaticVao;

	if ( staticVao ) {
		R_MetalBindVaoStreams( staticVao, attribs );
	} else
	{
		// Entities always select the vertex-animation lightall permutations (as GL2 does,
		// tr_shade.c:1136), also when they are drawn from tess; alias the old frame then.
		if ( attribs & ATTR_POSITION ) {
			R_MetalBindTessStream( 2, tess.xyz, sizeof( vec4_t ), nv, ( attribs & ATTR_POSITION2 ) ? 9 : -1 );
		}
		if ( attribs & ATTR_TEXCOORD ) {
			R_MetalBindTessStream( 3, tess.texCoords, sizeof( vec2_t ), nv );
		}
		if ( attribs & ATTR_LIGHTCOORD ) {
			R_MetalBindTessStream( 6, tess.lightCoords, sizeof( vec2_t ), nv );
		}
		if ( attribs & ATTR_NORMAL ) {
			R_MetalBindTessStream( 4, tess.normal, sizeof( int16_t ) * 4, nv, ( attribs & ATTR_NORMAL2 ) ? 10 : -1 );
		}
		if ( attribs & ATTR_COLOR ) {
			R_MetalBindTessStream( 5, tess.color, sizeof( uint16_t ) * 4, nv );
		}
		if ( attribs & ATTR_TANGENT ) {
			R_MetalBindTessStream( 7, tess.tangent, sizeof( int16_t ) * 4, nv, ( attribs & ATTR_TANGENT2 ) ? 11 : -1 );
		} else if ( attribs & ATTR_TANGENT2 ) {
			R_MetalBindTessStream( 11, tess.tangent, sizeof( int16_t ) * 4, nv ); // declared without ATTR_TANGENT when r_normalMapping is on
		}
		if ( attribs & ATTR_LIGHTDIRECTION ) {
			R_MetalBindTessStream( 8, tess.lightdir, sizeof( int16_t ) * 4, nv );
		}
	}

	// ── Uniform blocks ───────────────────────────────────────────────────
	// Struct layout matches Vasquez's binding contract:
	//   texturecolor → 64-byte mvp ([[buffer(1)]] vert) + 16-byte color ([[buffer(0)]] frag)
	//   lightall     → MetalLightallUniforms (672 bytes, [[buffer(1)]] vert, [[buffer(0)]] frag)
	//   fogpass      → MetalFogpassUniforms  (144 bytes, same slots)
	//   dlight       → MetalDlightUniforms   (128 bytes, same slots)
	//   generic/all others → MetalDrawUniforms (448 bytes, same slots)
	// Blocks shared by both stages are written to frame memory once and the same range is
	// bound at both stages. The layout is resolved per program at init (metalUniformLayout).
	// GL2 ref: qglUniform* calls before draw.
	switch ( prog->metalUniformLayout )
	{
	case METAL_UNIFORMS_TEXTURECOLOR:
	{
		MTL::Buffer *buffer = NULL;
		NS::UInteger offset = 0;
		void *dst = GLMetal_FrameAlloc( sizeof( float ) * 16, 256, &buffer, &offset );
		if ( !dst ) {
			return;
		}
		memcpy( dst, prog->uniformBuffer + prog->uniformBufferOffsets[UNIFORM_MODELVIEWPROJECTIONMATRIX], sizeof( float ) * 16 );
		GLMetal_SetVertexBuffer( buffer, offset, 1 );

		dst = GLMetal_FrameAlloc( sizeof( float ) * 4, 256, &buffer, &offset );
		if ( !dst ) {
			return;
		}
		memcpy( dst, prog->uniformBuffer + prog->uniformBufferOffsets[UNIFORM_COLOR], sizeof( float ) * 4 );
		GLMetal_SetFragmentBuffer( buffer, offset, 0 );
		break;
	}
	case METAL_UNIFORMS_LIGHTALL:
	{
		// Vasquez's MSL LightallDrawUniforms must be byte-identical (672 bytes).
		MetalLightallUniforms lightallU;
		R_FillMetalLightallUniforms( prog, &lightallU );
		GLMetal_SetSharedUniforms( &lightallU, sizeof( lightallU ), 1, 0 );
		break;
	}
	case METAL_UNIFORMS_FOGPASS:
	{
		// GL2 ref: fogpass_vp.glsl / fogpass_fp.glsl.
		// Animation permutations (USE_VERTEX_ANIMATION/USE_BONE_ANIMATION) are DEFERRED.
		MetalFogpassUniforms fogpassU;
		R_FillMetalFogpassUniforms( prog, &fogpassU );
		GLMetal_SetSharedUniforms( &fogpassU, sizeof( fogpassU ), 1, 0 );
		break;
	}
	case METAL_UNIFORMS_DLIGHT:
	{
		// GL2 ref: dlight_vp.glsl / dlight_fp.glsl.
		// u_DiffuseMap sampled at [[texture(0)]] (TB_DIFFUSEMAP, bound by GLMetal_BindTexturesToCurrentEncoder).
		MetalDlightUniforms dlightU;
		R_FillMetalDlightUniforms( prog, &dlightU );
		GLMetal_SetSharedUniforms( &dlightU, sizeof( dlightU ), 1, 0 );
		break;
	}
	case METAL_UNIFORMS_PSHADOW:
	{
		// GL2 ref: pshadow_vp.glsl / pshadow_fp.glsl (tr_shade.c:1004-1032).
		// u_ShadowMap (depth2d) sampled at [[texture(0)]] (TB_DIFFUSEMAP).
		MetalPshadowUniforms pshadowU;
		R_FillMetalPshadowUniforms( prog, &pshadowU );
		GLMetal_SetSharedUniforms( &pshadowU, sizeof( pshadowU ), 1, 0 );
		break;
	}
	case METAL_UNIFORMS_SHADOWFILL:
	{
		// GL2 ref: shadowfill_vp.glsl / shadowfill_fp.glsl (tr_shade.c:1565-1612).
		// No textures needed — depth write is the output.
		MetalShadowfillUniforms shadowfillU;
		R_FillMetalShadowfillUniforms( prog, &shadowfillU );
		GLMetal_SetSharedUniforms( &shadowfillU, sizeof( shadowfillU ), 1, 0 );
		break;
	}
	case METAL_UNIFORMS_DRAW:
	default:
	{
		// generic and all other shaders — 448-byte block.
		MetalDrawUniforms drawU;
		R_FillMetalDrawUniforms( prog, &drawU );
		GLMetal_SetSharedUniforms( &drawU, sizeof( drawU ), 1, 0 );
		break;
	}
	}

	// ── Diffuse textures/samplers from per-TMU binding table ──────────────
	// Real textures resolve texnum → MTL::Texture through the device side-table,
	// with white fallback for unbound/missing entries.
	// GL2 ref: qglBindTexture per texture unit — tr_backend.c:43
	GLMetal_BindTexturesToCurrentEncoder();

	// ── Index buffer + draw call ──────────────────────────────────────────
	// GL2 ref: qglDrawElements(GL_TRIANGLES, numIndexes, GL_INDEX_TYPE, ...) tr_shade.c:49
	MTL::Buffer *idxBuf = NULL;
	NS::UInteger idxOffset = 0;
	qboolean haveIndices;
	if ( staticVao ) {
		idxBuf = (MTL::Buffer *)staticVao->metalBuffer;
		idxOffset = staticVao->metalOfsIndexes + (NS::UInteger)firstIndex * sizeof( glIndex_t );
		haveIndices = qtrue;
	} else
	{
		haveIndices = R_MetalGetTessIndices( numIndexes, firstIndex, &idxBuf, &idxOffset );
	}
	if ( haveIndices ) {
		constexpr MTL::IndexType kIdxType =
			( sizeof( glIndex_t ) == 2 ) ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
		enc->drawIndexedPrimitives(
			MTL::PrimitiveTypeTriangle,
			(NS::UInteger)numIndexes,
			kIdxType,
			idxBuf,
			idxOffset );
	}
	// ── end PHASE5-M5.2 Metal draw path ──────────────────────────────────────
}


/*
=============================================================

SURFACE SHADERS

=============================================================
*/

shaderCommands_t tess;


/*
=================
R_BindAnimatedImageToTMU

=================
*/
static void R_BindAnimatedImageToTMU( textureBundle_t *bundle, int tmu ) {
	int64_t index;

	if ( bundle->isVideoMap ) {
		ri.CIN_RunCinematic( bundle->videoMapHandle );
		ri.CIN_UploadCinematic( bundle->videoMapHandle );
		// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp;
		//   maps to MTLRenderCommandEncoder setFragmentTexture:atIndex:. GL2 ref: tr_shade.c:77
		GL_BindToTMU( tr.scratchImage[bundle->videoMapHandle], tmu );
		return;
	}

	if ( bundle->numImageAnimations <= 1 ) {
		if ( bundle->isLightmap && ( backEnd.refdef.rdflags & RDF_SNOOPERVIEW ) ) {
			GL_BindToTMU( tr.whiteImage, tmu );
		} else {
			GL_BindToTMU( bundle->image[0], tmu );
		}
		return;
	}

	// it is necessary to do this messy calc to make sure animations line up
	// exactly with waveforms of the same frequency
	index = tess.shaderTime * bundle->imageAnimationSpeed * FUNCTABLE_SIZE;
	index >>= FUNCTABLE_SIZE2;

	if ( index < 0 ) {
		index = 0;  // may happen with shader time offsets
	}

	// Windows x86 doesn't load renderer DLL with 64 bit modulus
	//index %= bundle->numImageAnimations;
	while ( index >= bundle->numImageAnimations ) {
		index -= bundle->numImageAnimations;
	}

	if ( bundle->isLightmap && ( backEnd.refdef.rdflags & RDF_SNOOPERVIEW ) ) {
		GL_BindToTMU( tr.whiteImage, tmu );
	} else {
		GL_BindToTMU( bundle->image[ index ], tmu );
	}
}


/*
================
DrawTris

Draws triangle outlines for debugging
================
*/
static void DrawTris( shaderCommands_t *input ) {
	// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:113
	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );

	// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:115
	GL_State( GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE );
	// PARITY-SEAM(Metal-device): qglDepthRange(0,0) → MTLRenderCommandEncoder viewport
	//   depth range / MTLDepthBias. GL2 ref: tr_shade.c:116

	{
		shaderProgram_t *sp = &tr.textureColorShader;
		vec4_t color;

		// PARITY-SEAM(Metal-device): GLSL_BindProgram → seamed in tr_glsl.cpp. GL2 ref: tr_shade.c:122
		GLSL_BindProgram( sp );

		GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
		VectorSet4( color, 1, 1, 1, 1 );
		GLSL_SetUniformVec4( sp, UNIFORM_COLOR, color );
		GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 0 );

		R_DrawElements( input->numIndexes, input->firstIndex );
	}

	// PARITY-SEAM(Metal-device): qglDepthRange(0,1) → restore full depth range. GL2 ref: tr_shade.c:132
}


/*
================
DrawNormals

Draws vertex normals for debugging
================
*/
static void DrawNormals( shaderCommands_t *input ) {
	if ( !input ) {
		return;
	}

	const int vertexCount = input->numVertexes;
	if ( vertexCount <= 0 ) {
		return;
	}
	if ( !input->shader || input->shader->isSky ) {
		return;                                           // do not draw over sky/clouds
	}
	if ( input->shader->numDeforms > 0 ) {
		return;                                 // skip vertex-deformed surfaces

	}
	// Skip animated entities that are interpolating; otherwise normals will appear to "swim".
	if ( backEnd.currentEntity != &tr.worldEntity ) {
		const trRefEntity_t* refEnt = backEnd.currentEntity;
		if ( refEnt && refEnt->e.backlerp > 0 ) {
			return;
		}
	}

	const float lineLength = 6.0f;

	// xyz is vec4_t (x,y,z,1)
	const float*      positionsXYZW   = (const float*)input->xyz;
	const int16_t*    packedNormals   = (const int16_t*)input->normal;
	const float*      floatNormals    = (const float*)input->normal;

	const vao_t*      boundVao        = glState.currentVao;
	qboolean usePacked       = qfalse;

	if ( boundVao ) {
		const vaoAttrib_t* a = &boundVao->attribs[ATTR_INDEX_NORMAL];
		usePacked = ( a->type == GL_SHORT && a->normalized ) ? qtrue : qfalse;
	} else {
		const float sx = packedNormals[0] * ( 1.0f / 32767.0f );
		const float sy = packedNormals[1] * ( 1.0f / 32767.0f );
		const float sz = packedNormals[2] * ( 1.0f / 32767.0f );

		const float lp = sx * sx + sy * sy + sz * sz;
		const float lf = floatNormals[0] * floatNormals[0]
						 + floatNormals[1] * floatNormals[1]
						 + floatNormals[2] * floatNormals[2];

		usePacked = ( fabsf( lp - 1.0f ) <= fabsf( lf - 1.0f ) ) ? qtrue : qfalse;
	}

	const GLsizeiptr floatsPerVertex = 6;
	const GLsizeiptr maxFloatCount = floatsPerVertex * (GLsizeiptr)vertexCount;
	const GLsizeiptr maxByteCount = (GLsizeiptr)sizeof( float ) * maxFloatCount;

	float* lineVertices = (float*)ri.Hunk_AllocateTempMemory( (int)maxByteCount );
	float* writePtr = lineVertices;

	// Build the line list
	for (int i = 0; i < vertexCount; ++i, positionsXYZW += 4) {
		vec3_t n;

		if ( usePacked ) {
			R_VaoUnpackNormal( n, (int16_t*)packedNormals );
			packedNormals += 4;
		} else {
			n[0] = floatNormals[0];
			n[1] = floatNormals[1];
			n[2] = floatNormals[2];
			floatNormals += 4;
		}

		// endpoints in object/world space
		const float startX = positionsXYZW[0];
		const float startY = positionsXYZW[1];
		const float startZ = positionsXYZW[2];

		const float endX = startX + n[0] * lineLength;
		const float endY = startY + n[1] * lineLength;
		const float endZ = startZ + n[2] * lineLength;


		// emit line
		*writePtr++ = startX; *writePtr++ = startY; *writePtr++ = startZ;
		*writePtr++ = endX;   *writePtr++ = endY;   *writePtr++ = endZ;
	}

	const GLsizei numVerts = (GLsizei)( ( writePtr - lineVertices ) / 3 );
	if ( numVerts == 0 ) {
		ri.Hunk_FreeTempMemory( lineVertices );
		return;
	}

	// save bindings
	GLint prevProg = 0, prevVao = 0, prevArrayBuf = 0;
	// PARITY-SEAM(Metal-device): qglGetIntegerv(GL_CURRENT_PROGRAM) → query current MTL pipeline state.
	// PARITY-SEAM(Metal-device): qglGetIntegerv(GL_VERTEX_ARRAY_BINDING) → query bound VAO.
	// PARITY-SEAM(Metal-device): qglGetIntegerv(GL_ARRAY_BUFFER_BINDING). GL2 ref: tr_shade.c:231
	(void)prevProg; (void)prevVao; (void)prevArrayBuf;

	// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:233
	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
	// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:234
	GL_State( GLS_POLYMODE_LINE );
	GLboolean prevDepthMask = GL_FALSE;
	// PARITY-SEAM(Metal-device): qglGetBooleanv(GL_DEPTH_WRITEMASK). GL2 ref: tr_shade.c:236
	// PARITY-SEAM(Metal-device): qglDepthMask(GL_FALSE) → MTLDepthStencilDescriptor
	//   depthWriteEnabled = NO. GL2 ref: tr_shade.c:237
	(void)prevDepthMask;


	R_BindNullVao();

	// temp VAO/VBO  (GPU resources — seamed)
	GLuint vao = 0, vbo = 0;
	// PARITY-SEAM(Metal-device): qglGenVertexArrays / qglBindVertexArray → MTLDevice
	//   newBufferWithLength:options:. GL2 ref: tr_shade.c:244-245
	// PARITY-SEAM(Metal-device): qglGenBuffers / qglBindBuffer / qglBufferData →
	//   MTLBuffer contents upload. GL2 ref: tr_shade.c:247-249
	(void)vao; (void)vbo;

	shaderProgram_t* program = &tr.textureColorShader;
	// PARITY-SEAM(Metal-device): GLSL_BindProgram → seamed in tr_glsl.cpp. GL2 ref: tr_shade.c:252
	GLSL_BindProgram( program );
	GLSL_SetUniformMat4( program, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );

	vec4_t white;
	VectorSet4( white, 1, 1, 1, 1 );
	GLSL_SetUniformVec4( program, UNIFORM_COLOR, white );

	// PARITY-SEAM(Metal-device): qglEnableVertexAttribArray / qglVertexAttribPointer →
	//   MTLVertexAttributeDescriptor format/offset/bufferIndex. GL2 ref: tr_shade.c:260-261
	// PARITY-SEAM(Metal-device): qglDrawArrays(GL_LINES) → MTLRenderCommandEncoder
	//   drawPrimitives:vertexStart:vertexCount:. GL2 ref: tr_shade.c:263
	// PARITY-SEAM(Metal-device): qglDisableVertexAttribArray. GL2 ref: tr_shade.c:265

	// restore
	// PARITY-SEAM(Metal-device): qglDepthMask / qglBindBuffer / qglBindVertexArray restore.
	// PARITY-SEAM(Metal-device): qglDeleteBuffers / qglDeleteVertexArrays. GL2 ref: tr_shade.c:271-272
	// PARITY-SEAM(Metal-device): qglUseProgram → MTLRenderCommandEncoder setRenderPipelineState:.

	ri.Hunk_FreeTempMemory( lineVertices );
}

/*
==============
RB_BeginSurface

We must set some things up before beginning any tesselation,
because a surface may be forced to perform a RB_End due
to overflow.
==============
*/
void RB_BeginSurface( shader_t *shader, int fogNum, int cubemapIndex ) {

	shader_t *state = ( shader->remappedShader ) ? shader->remappedShader : shader;

	tess.numIndexes = 0;
	tess.firstIndex = 0;
	tess.numVertexes = 0;
	tess.shader = state;
	tess.fogNum = fogNum;
	tess.cubemapIndex = cubemapIndex;
	tess.dlightBits = 0;        // will be OR'd in by surface functions
	tess.pshadowBits = 0;       // will be OR'd in by surface functions
	tess.xstages = state->stages;
	tess.numPasses = state->numUnfoggedPasses;
	tess.currentStageIteratorFunc = state->optimalStageIteratorFunc;
	tess.useInternalVao = qtrue;
	tess.useCacheVao = qfalse;
	tess.metalStaticVao = NULL;

	tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
	if ( tess.shader->clampTime && tess.shaderTime >= tess.shader->clampTime ) {
		tess.shaderTime = tess.shader->clampTime;
	}

	if ( backEnd.viewParms.flags & VPF_SHADOWMAP ) {
		tess.currentStageIteratorFunc = RB_StageIteratorGeneric;
	}
}



extern float EvalWaveForm( const waveForm_t *wf );
extern float EvalWaveFormClamped( const waveForm_t *wf );


static void ComputeTexMods( shaderStage_t *pStage, int bundleNum, vec4_t outMatrix[8] ) {
	int tm;
	float matrix[6];
	float tmpmatrix[6];
	float currentmatrix[6];
	float turb[2];
	textureBundle_t *bundle = &pStage->bundle[bundleNum];
	qboolean hasTurb = qfalse;

	currentmatrix[0] = 1.0f; currentmatrix[2] = 0.0f; currentmatrix[4] = 0.0f;
	currentmatrix[1] = 0.0f; currentmatrix[3] = 1.0f; currentmatrix[5] = 0.0f;

	for ( tm = 0; tm < bundle->numTexMods ; tm++ ) {
		switch ( bundle->texMods[tm].type )
		{

		case TMOD_NONE:
			matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = 0.0f;
			matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = 0.0f;
			break;

		case TMOD_TURBULENT:
			RB_CalcTurbulentFactors( &bundle->texMods[tm].wave, &turb[0], &turb[1] );
			break;

		case TMOD_ENTITY_TRANSLATE:
			RB_CalcScrollTexMatrix( backEnd.currentEntity->e.shaderTexCoord, matrix );
			break;

		case TMOD_SCROLL:
			RB_CalcScrollTexMatrix( bundle->texMods[tm].scroll,
									matrix );
			break;

		case TMOD_SCALE:
			RB_CalcScaleTexMatrix( bundle->texMods[tm].scale,
								   matrix );
			break;

		case TMOD_STRETCH:
			RB_CalcStretchTexMatrix( &bundle->texMods[tm].wave,
									 matrix );
			break;

		case TMOD_TRANSFORM:
			RB_CalcTransformTexMatrix( &bundle->texMods[tm],
									   matrix );
			break;

		case TMOD_ROTATE:
			RB_CalcRotateTexMatrix( bundle->texMods[tm].rotateSpeed,
									matrix );
			break;

		default:
			ri.Error( ERR_DROP, "ERROR: unknown texmod '%d' in shader '%s'", bundle->texMods[tm].type, tess.shader->name );
			break;
		}

		switch ( bundle->texMods[tm].type )
		{
		case TMOD_TURBULENT:
			outMatrix[tm * 2 + 0][0] = 1; outMatrix[tm * 2 + 0][1] = 0; outMatrix[tm * 2 + 0][2] = 0;
			outMatrix[tm * 2 + 1][0] = 0; outMatrix[tm * 2 + 1][1] = 1; outMatrix[tm * 2 + 1][2] = 0;

			outMatrix[tm * 2 + 0][3] = turb[0];
			outMatrix[tm * 2 + 1][3] = turb[1];

			hasTurb = qtrue;
			break;

		case TMOD_NONE:
		case TMOD_ENTITY_TRANSLATE:
		case TMOD_SCROLL:
		case TMOD_SCALE:
		case TMOD_STRETCH:
		case TMOD_TRANSFORM:
		case TMOD_ROTATE:
		default:
			outMatrix[tm * 2 + 0][0] = matrix[0]; outMatrix[tm * 2 + 0][1] = matrix[2]; outMatrix[tm * 2 + 0][2] = matrix[4];
			outMatrix[tm * 2 + 1][0] = matrix[1]; outMatrix[tm * 2 + 1][1] = matrix[3]; outMatrix[tm * 2 + 1][2] = matrix[5];

			outMatrix[tm * 2 + 0][3] = 0;
			outMatrix[tm * 2 + 1][3] = 0;

			tmpmatrix[0] = matrix[0] * currentmatrix[0] + matrix[2] * currentmatrix[1];
			tmpmatrix[1] = matrix[1] * currentmatrix[0] + matrix[3] * currentmatrix[1];

			tmpmatrix[2] = matrix[0] * currentmatrix[2] + matrix[2] * currentmatrix[3];
			tmpmatrix[3] = matrix[1] * currentmatrix[2] + matrix[3] * currentmatrix[3];

			tmpmatrix[4] = matrix[0] * currentmatrix[4] + matrix[2] * currentmatrix[5] + matrix[4];
			tmpmatrix[5] = matrix[1] * currentmatrix[4] + matrix[3] * currentmatrix[5] + matrix[5];

			currentmatrix[0] = tmpmatrix[0];
			currentmatrix[1] = tmpmatrix[1];
			currentmatrix[2] = tmpmatrix[2];
			currentmatrix[3] = tmpmatrix[3];
			currentmatrix[4] = tmpmatrix[4];
			currentmatrix[5] = tmpmatrix[5];
			break;
		}
	}

	// if turb isn't used, only one matrix is needed
	if ( !hasTurb ) {
		tm = 0;

		outMatrix[tm * 2 + 0][0] = currentmatrix[0]; outMatrix[tm * 2 + 0][1] = currentmatrix[2]; outMatrix[tm * 2 + 0][2] = currentmatrix[4];
		outMatrix[tm * 2 + 1][0] = currentmatrix[1]; outMatrix[tm * 2 + 1][1] = currentmatrix[3]; outMatrix[tm * 2 + 1][2] = currentmatrix[5];

		outMatrix[tm * 2 + 0][3] = 0;
		outMatrix[tm * 2 + 1][3] = 0;
		tm++;
	}

	for ( ; tm < TR_MAX_TEXMODS ; tm++ ) {
		outMatrix[tm * 2 + 0][0] = 1; outMatrix[tm * 2 + 0][1] = 0; outMatrix[tm * 2 + 0][2] = 0;
		outMatrix[tm * 2 + 1][0] = 0; outMatrix[tm * 2 + 1][1] = 1; outMatrix[tm * 2 + 1][2] = 0;

		outMatrix[tm * 2 + 0][3] = 0;
		outMatrix[tm * 2 + 1][3] = 0;
	}
}


static void ComputeDeformValues( int *deformGen, vec5_t deformParams ) {
	// u_DeformGen
	*deformGen = DGEN_NONE;
	if ( !ShaderRequiresCPUDeforms( tess.shader ) ) {
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.shader->deforms[0];

		switch ( ds->deformation )
		{
		case DEFORM_WAVE:
			*deformGen = ds->deformationWave.func;

			deformParams[0] = ds->deformationWave.base;
			deformParams[1] = ds->deformationWave.amplitude;
			deformParams[2] = ds->deformationWave.phase;
			deformParams[3] = ds->deformationWave.frequency;
			deformParams[4] = ds->deformationSpread;
			break;

		case DEFORM_BULGE:
			*deformGen = DGEN_BULGE;

			deformParams[0] = 0;
			deformParams[1] = ds->bulgeHeight;     // amplitude
			deformParams[2] = ds->bulgeWidth;      // phase
			deformParams[3] = ds->bulgeSpeed;      // frequency
			deformParams[4] = 0;
			break;

		default:
			break;
		}
	}
}


static void ProjectDlightTexture( void ) {
	int l;
	vec3_t origin;
	float scale;
	float radius;
	int deformGen;
	vec5_t deformParams;

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}

	if ( backEnd.refdef.rdflags & RDF_SNOOPERVIEW ) {  // no dlights for snooper
		return;
	}

	ComputeDeformValues( &deformGen, deformParams );

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t    *dl;
		shaderProgram_t *sp;
		vec4_t vector;

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;   // this surface definitely doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];
		VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		scale = 1.0f / radius;

		sp = &tr.dlightShader[deformGen == DGEN_NONE ? 0 : 1];

		backEnd.pc.c_dlightDraws++;

		// PARITY-SEAM(Metal-device): GLSL_BindProgram → seamed in tr_glsl.cpp. GL2 ref: tr_shade.c:523
		GLSL_BindProgram( sp );

		GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );

		GLSL_SetUniformFloat( sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation );

		GLSL_SetUniformInt( sp, UNIFORM_DEFORMGEN, deformGen );
		if ( deformGen != DGEN_NONE ) {
			GLSL_SetUniformFloat5( sp, UNIFORM_DEFORMPARAMS, deformParams );
			GLSL_SetUniformFloat( sp, UNIFORM_TIME, tess.shaderTime );

			if ( tess.shader->deforms[0].deformationWave.frequency < 0 ) {
				vec3_t worldUp;

				if ( VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
					VectorSet( backEnd.currentEntity->e.fireRiseDir, 0, 0, 1 );
				}

				if ( backEnd.currentEntity->e.hModel ) {    // world surfaces dont have an axis
					VectorRotate( backEnd.currentEntity->e.fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
				} else {
					VectorCopy( backEnd.currentEntity->e.fireRiseDir, worldUp );
				}

				GLSL_SetUniformVec3( sp, UNIFORM_FIRERISEDIR, worldUp );
			}
		}

		vector[0] = dl->color[0];
		vector[1] = dl->color[1];
		vector[2] = dl->color[2];
		vector[3] = 1.0f;
		GLSL_SetUniformVec4( sp, UNIFORM_COLOR, vector );

		vector[0] = origin[0];
		vector[1] = origin[1];
		vector[2] = origin[2];
		vector[3] = scale;
		GLSL_SetUniformVec4( sp, UNIFORM_DLIGHTINFO, vector );

		// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:548
		GL_BindToTMU( tr.dlightImage, TB_COLORMAP );

		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		if ( dl->additive ) {
			// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:553
			GL_State( GLS_ATEST_GT_0 | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		} else {
			// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:556
			GL_State( GLS_ATEST_GT_0 | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}

		GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 1 );

		R_DrawElements( tess.numIndexes, tess.firstIndex );

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		backEnd.pc.c_dlightIndexes += tess.numIndexes;
		backEnd.pc.c_dlightVertexes += tess.numVertexes;
	}
}


static void ComputeShaderColors( shaderStage_t *pStage, vec4_t baseColor, vec4_t vertColor, int blend ) {
	qboolean isBlend = ( ( ( blend & GLS_SRCBLEND_BITS ) == GLS_SRCBLEND_DST_COLOR )
						 || ( ( blend & GLS_SRCBLEND_BITS ) == GLS_SRCBLEND_ONE_MINUS_DST_COLOR )
						 || ( ( blend & GLS_DSTBLEND_BITS ) == GLS_DSTBLEND_SRC_COLOR )
						 || ( ( blend & GLS_DSTBLEND_BITS ) == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR ) ) ? qtrue : qfalse;

	qboolean is2DDraw = ( backEnd.currentEntity == &backEnd.entity2D ) ? qtrue : qfalse;

	float overbright = ( isBlend || is2DDraw ) ? 1.0f : (float)( 1 << tr.overbrightBits );

	fog_t *fog;

	baseColor[0] =
		baseColor[1] =
			baseColor[2] =
				baseColor[3] = 1.0f;

	vertColor[0] =
		vertColor[1] =
			vertColor[2] =
				vertColor[3] = 0.0f;

	//
	// rgbGen
	//
	switch ( pStage->rgbGen )
	{
	case CGEN_EXACT_VERTEX:
	case CGEN_EXACT_VERTEX_LIT:
		baseColor[0] =
			baseColor[1] =
				baseColor[2] =
					baseColor[3] = 0.0f;

		vertColor[0] =
			vertColor[1] =
				vertColor[2] = overbright;
		vertColor[3] = 1.0f;
		break;
	case CGEN_CONST:
		baseColor[0] = pStage->constantColor[0] / 255.0f;
		baseColor[1] = pStage->constantColor[1] / 255.0f;
		baseColor[2] = pStage->constantColor[2] / 255.0f;
		baseColor[3] = pStage->constantColor[3] / 255.0f;
		break;
	case CGEN_VERTEX:
	case CGEN_VERTEX_LIT:
		baseColor[0] =
			baseColor[1] =
				baseColor[2] =
					baseColor[3] = 0.0f;

		vertColor[0] =
			vertColor[1] =
				vertColor[2] =
					vertColor[3] = 1.0f;
		break;
	case CGEN_ONE_MINUS_VERTEX:
		baseColor[0] =
			baseColor[1] =
				baseColor[2] = 1.0f;

		vertColor[0] =
			vertColor[1] =
				vertColor[2] = -1.0f;
		break;
	case CGEN_FOG:
		fog = tr.world->fogs + tess.fogNum;

		baseColor[0] = ( (unsigned char *)( &fog->colorInt ) )[0] / 255.0f;
		baseColor[1] = ( (unsigned char *)( &fog->colorInt ) )[1] / 255.0f;
		baseColor[2] = ( (unsigned char *)( &fog->colorInt ) )[2] / 255.0f;
		baseColor[3] = ( (unsigned char *)( &fog->colorInt ) )[3] / 255.0f;
		break;
	case CGEN_WAVEFORM:
		baseColor[0] =
			baseColor[1] =
				baseColor[2] = RB_CalcWaveColorSingle( &pStage->rgbWave );
		break;
	case CGEN_ENTITY:
		if ( backEnd.currentEntity ) {
			baseColor[0] = ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[0] / 255.0f;
			baseColor[1] = ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[1] / 255.0f;
			baseColor[2] = ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[2] / 255.0f;
			baseColor[3] = ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[3] / 255.0f;
		}
		break;
	case CGEN_ONE_MINUS_ENTITY:
		if ( backEnd.currentEntity ) {
			baseColor[0] = 1.0f - ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[0] / 255.0f;
			baseColor[1] = 1.0f - ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[1] / 255.0f;
			baseColor[2] = 1.0f - ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[2] / 255.0f;
			baseColor[3] = 1.0f - ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[3] / 255.0f;
		}
		break;
	case CGEN_IDENTITY:
	case CGEN_LIGHTING_DIFFUSE:
		baseColor[0] =
			baseColor[1] =
				baseColor[2] = overbright;
		break;
	case CGEN_IDENTITY_LIGHTING:
	case CGEN_BAD:
		break;
	}

	//
	// alphaGen
	//
	switch ( pStage->alphaGen )
	{
	case AGEN_SKIP:
		break;
	case AGEN_CONST:
		baseColor[3] = pStage->constantColor[3] / 255.0f;
		vertColor[3] = 0.0f;
		break;
	case AGEN_WAVEFORM:
		baseColor[3] = RB_CalcWaveAlphaSingle( &pStage->alphaWave );
		vertColor[3] = 0.0f;
		break;
	case AGEN_ENTITY:
		if ( backEnd.currentEntity ) {
			baseColor[3] = ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[3] / 255.0f;
		}
		vertColor[3] = 0.0f;
		break;
	case AGEN_ONE_MINUS_ENTITY:
		if ( backEnd.currentEntity ) {
			baseColor[3] = 1.0f - ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[3] / 255.0f;
		}
		vertColor[3] = 0.0f;
		break;
	case AGEN_NORMALZFADE:
		baseColor[3] = pStage->constantColor[3] / 255.0f;
		if ( backEnd.currentEntity && backEnd.currentEntity->e.hModel ) {
			baseColor[3] *= ( (unsigned char *)backEnd.currentEntity->e.shaderRGBA )[3] / 255.0f;
		}
		vertColor[3] = 0.0f;
		break;
	case AGEN_VERTEX:
		baseColor[3] = 0.0f;
		vertColor[3] = 1.0f;
		break;
	case AGEN_ONE_MINUS_VERTEX:
		baseColor[3] = 1.0f;
		vertColor[3] = -1.0f;
		break;
	case AGEN_IDENTITY:
	case AGEN_LIGHTING_SPECULAR:
	case AGEN_PORTAL:
		// Done entirely in vertex program
		baseColor[3] = 1.0f;
		vertColor[3] = 0.0f;
		break;
	}

}


static void ComputeFogValues( vec4_t fogDistanceVector, vec4_t fogDepthVector, float *eyeT, glfog_t *glFog ) {
	// from RB_CalcFogTexCoords()
	fog_t  *fog;
	vec3_t local;

	if ( glFog ) {
		VectorSubtract( backEnd.or.origin, backEnd.viewParms.or.origin, local );
		fogDistanceVector[0] = -backEnd.or.modelMatrix[2];
		fogDistanceVector[1] = -backEnd.or.modelMatrix[6];
		fogDistanceVector[2] = -backEnd.or.modelMatrix[10];
		fogDistanceVector[3] = DotProduct( local, backEnd.viewParms.or.axis[0] );

		fogDepthVector[0] = glFog->start;
		fogDepthVector[1] = glFog->end;
		fogDepthVector[2] = glFog->density;
		fogDepthVector[3] = 1.0;

		return;
	}

	if ( !tess.fogNum ) {
		return;
	}

	fog = tr.world->fogs + tess.fogNum;

	VectorSubtract( backEnd.or.origin, backEnd.viewParms.or.origin, local );
	fogDistanceVector[0] = -backEnd.or.modelMatrix[2];
	fogDistanceVector[1] = -backEnd.or.modelMatrix[6];
	fogDistanceVector[2] = -backEnd.or.modelMatrix[10];
	fogDistanceVector[3] = DotProduct( local, backEnd.viewParms.or.axis[0] );

	// scale the fog vectors based on the fog's thickness
	VectorScale4( fogDistanceVector, fog->tcScale, fogDistanceVector );

	// rotate the gradient vector for this orientation
	if ( fog->hasSurface ) {
		fogDepthVector[0] = fog->surface[0] * backEnd.or.axis[0][0] +
							fog->surface[1] * backEnd.or.axis[0][1] + fog->surface[2] * backEnd.or.axis[0][2];
		fogDepthVector[1] = fog->surface[0] * backEnd.or.axis[1][0] +
							fog->surface[1] * backEnd.or.axis[1][1] + fog->surface[2] * backEnd.or.axis[1][2];
		fogDepthVector[2] = fog->surface[0] * backEnd.or.axis[2][0] +
							fog->surface[1] * backEnd.or.axis[2][1] + fog->surface[2] * backEnd.or.axis[2][2];
		fogDepthVector[3] = -fog->surface[3] + DotProduct( backEnd.or.origin, fog->surface );

		*eyeT = DotProduct( backEnd.or.viewOrigin, fogDepthVector ) + fogDepthVector[3];
	} else {
		*eyeT = 1;  // non-surface fog always has eye inside
	}
}


static void ComputeFogColorMask( shaderStage_t *pStage, vec4_t fogColorMask ) {
	switch ( pStage->adjustColorsForFog )
	{
	case ACFF_MODULATE_RGB:
		fogColorMask[0] =
			fogColorMask[1] =
				fogColorMask[2] = 1.0f;
		fogColorMask[3] = 0.0f;
		break;
	case ACFF_MODULATE_ALPHA:
		fogColorMask[0] =
			fogColorMask[1] =
				fogColorMask[2] = 0.0f;
		fogColorMask[3] = 1.0f;
		break;
	case ACFF_MODULATE_RGBA:
		fogColorMask[0] =
			fogColorMask[1] =
				fogColorMask[2] =
					fogColorMask[3] = 1.0f;
		break;
	default:
		fogColorMask[0] =
			fogColorMask[1] =
				fogColorMask[2] =
					fogColorMask[3] = 0.0f;
		break;
	}
}


static void ForwardDlight( void ) {
	int l;
	//vec3_t	origin;
	//float	scale;
	float radius;

	int deformGen;
	vec5_t deformParams;

	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;

	shaderCommands_t *input = &tess;
	shaderStage_t *pStage = tess.xstages[0];

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}

	ComputeDeformValues( &deformGen, deformParams );

	ComputeFogValues( fogDistanceVector, fogDepthVector, &eyeT, NULL );

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t    *dl;
		shaderProgram_t *sp;
		vec4_t vector;
		vec4_t texMatrix[8];

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;   // this surface definitely doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];
		//VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		//scale = 1.0f / radius;

		//if (pStage->glslShaderGroup == tr.lightallShader)
		{
			int index = pStage->glslShaderIndex;

			index &= ~LIGHTDEF_LIGHTTYPE_MASK;
			index |= LIGHTDEF_USE_LIGHT_VECTOR;

			sp = &tr.lightallShader[index];
		}

		backEnd.pc.c_lightallDraws++;

		// PARITY-SEAM(Metal-device): GLSL_BindProgram → seamed in tr_glsl.cpp. GL2 ref: tr_shade.c:847
		GLSL_BindProgram( sp );

		GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
		GLSL_SetUniformVec3( sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );
		GLSL_SetUniformVec3( sp, UNIFORM_LOCALVIEWORIGIN, backEnd.or.viewOrigin );

		GLSL_SetUniformFloat( sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation );

		if ( ( deformGen != DGEN_NONE && tess.shader->deforms[0].deformationWave.frequency < 0 )
			 || pStage->alphaGen == AGEN_NORMALZFADE ) {
			vec3_t worldUp;

			if ( VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
				VectorSet( backEnd.currentEntity->e.fireRiseDir, 0, 0, 1 );
			}

			if ( backEnd.currentEntity->e.hModel ) {    // world surfaces dont have an axis
				VectorRotate( backEnd.currentEntity->e.fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
			} else {
				VectorCopy( backEnd.currentEntity->e.fireRiseDir, worldUp );
			}

			GLSL_SetUniformVec3( sp, UNIFORM_FIRERISEDIR, worldUp );
		}

		GLSL_SetUniformInt( sp, UNIFORM_DEFORMGEN, deformGen );
		if ( deformGen != DGEN_NONE ) {
			GLSL_SetUniformFloat5( sp, UNIFORM_DEFORMPARAMS, deformParams );
			GLSL_SetUniformFloat( sp, UNIFORM_TIME, tess.shaderTime );
		}

		if ( input->fogNum ) {
			vec4_t fogColorMask;

			GLSL_SetUniformVec4( sp, UNIFORM_FOGDISTANCE, fogDistanceVector );
			GLSL_SetUniformVec4( sp, UNIFORM_FOGDEPTH, fogDepthVector );
			GLSL_SetUniformFloat( sp, UNIFORM_FOGEYET, eyeT );

			ComputeFogColorMask( pStage, fogColorMask );

			GLSL_SetUniformVec4( sp, UNIFORM_FOGCOLORMASK, fogColorMask );
		}

		{
			vec4_t baseColor;
			vec4_t vertColor;

			ComputeShaderColors( pStage, baseColor, vertColor, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

			GLSL_SetUniformVec4( sp, UNIFORM_BASECOLOR, baseColor );
			GLSL_SetUniformVec4( sp, UNIFORM_VERTCOLOR, vertColor );
		}

		if ( pStage->alphaGen == AGEN_PORTAL ) {
			GLSL_SetUniformFloat( sp, UNIFORM_PORTALRANGE, tess.shader->portalRange );
		} else if ( pStage->alphaGen == AGEN_NORMALZFADE )     {
			float lowest, highest;
			//qboolean zombieEffect = qfalse;

			lowest = pStage->zFadeBounds[0];
			if ( lowest == -1000 ) {    // use entity alpha
				lowest = backEnd.currentEntity->e.shaderTime;
				//zombieEffect = qtrue;
			}
			highest = pStage->zFadeBounds[1];
			if ( highest == -1000 ) {   // use entity alpha
				highest = backEnd.currentEntity->e.shaderTime;
				//zombieEffect = qtrue;
			}

			// TODO: Handle normalzfade zombie effect

			GLSL_SetUniformFloat( sp, UNIFORM_ZFADELOWEST, lowest );
			GLSL_SetUniformFloat( sp, UNIFORM_ZFADEHIGHEST, highest );
		}

		GLSL_SetUniformInt( sp, UNIFORM_COLORGEN, pStage->rgbGen );
		GLSL_SetUniformInt( sp, UNIFORM_ALPHAGEN, pStage->alphaGen );

		GLSL_SetUniformVec3( sp, UNIFORM_DIRECTEDLIGHT, dl->color );

		VectorSet( vector, 0, 0, 0 );
		GLSL_SetUniformVec3( sp, UNIFORM_AMBIENTLIGHT, vector );

		VectorCopy( dl->origin, vector );
		vector[3] = 1.0f;
		GLSL_SetUniformVec4( sp, UNIFORM_LIGHTORIGIN, vector );

		GLSL_SetUniformFloat( sp, UNIFORM_LIGHTRADIUS, radius );

		GLSL_SetUniformVec4( sp, UNIFORM_NORMALSCALE, pStage->normalScale );
		GLSL_SetUniformVec4( sp, UNIFORM_SPECULARSCALE, pStage->specularScale );

		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:908
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 0 );

		GLSL_SetUniformMat4( sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix );

		if ( pStage->bundle[TB_DIFFUSEMAP].image[0] ) {
			// PARITY-SEAM(Metal-device): R_BindAnimatedImageToTMU → GL_BindToTMU chain. GL2 ref: tr_shade.c:914
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP );
		}

		// bind textures that are sampled and used in the glsl shader, and
		// bind whiteImage to textures that are sampled but zeroed in the glsl shader
		//
		// alternatives:
		//  - use the last bound texture
		//     -> costs more to sample a higher res texture then throw out the result
		//  - disable texture sampling in glsl shader with #ifdefs, as before
		//     -> increases the number of shaders that must be compiled
		//

		if ( pStage->bundle[TB_NORMALMAP].image[0] ) {
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP );
		} else if ( r_normalMapping->integer )     {
			// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:931
			GL_BindToTMU( tr.whiteImage, TB_NORMALMAP );
		}

		if ( pStage->bundle[TB_SPECULARMAP].image[0] ) {
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP );
		} else if ( r_specularMapping->integer )     {
			// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:938
			GL_BindToTMU( tr.whiteImage, TB_SPECULARMAP );
		}

		{
			vec4_t enableTextures;

			VectorSet4( enableTextures, 0.0f, 0.0f, 0.0f, 0.0f );
			GLSL_SetUniformVec4( sp, UNIFORM_ENABLETEXTURES, enableTextures );
		}

		if ( r_dlightMode->integer >= 2 ) {
			// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:948
			GL_BindToTMU( tr.shadowCubemaps[l], TB_SHADOWMAP );
		}

		ComputeTexMods( pStage, TB_DIFFUSEMAP, texMatrix );
		GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0] );
		GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1] );
		GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2] );
		GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3] );
		GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4] );
		GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5] );
		GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6] );
		GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7] );

		GLSL_SetUniformInt( sp, UNIFORM_TCGEN0, pStage->bundle[0].tcGen );

		//
		// draw
		//

		R_DrawElements( input->numIndexes, input->firstIndex );

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		backEnd.pc.c_dlightIndexes += tess.numIndexes;
		backEnd.pc.c_dlightVertexes += tess.numVertexes;
	}
}


static void ProjectPshadowVBOGLSL( void ) {
	int l;
	vec3_t origin;
	float radius;

	int deformGen;
	vec5_t deformParams;

	shaderCommands_t *input = &tess;

	if ( !backEnd.refdef.num_pshadows ) {
		return;
	}

	ComputeDeformValues( &deformGen, deformParams );

	for ( l = 0 ; l < backEnd.refdef.num_pshadows ; l++ ) {
		pshadow_t   *ps;
		shaderProgram_t *sp;
		vec4_t vector;

		if ( !( tess.pshadowBits & ( 1 << l ) ) ) {
			continue;   // this surface definitely doesn't have any of this shadow
		}

		ps = &backEnd.refdef.pshadows[l];
		VectorCopy( ps->lightOrigin, origin );
		radius = ps->lightRadius;

		sp = &tr.pshadowShader;

		// PARITY-SEAM(Metal-device): GLSL_BindProgram → seamed in tr_glsl.cpp. GL2 ref: tr_shade.c:1006
		GLSL_BindProgram( sp );

		GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );

		VectorCopy( origin, vector );
		vector[3] = 1.0f;
		GLSL_SetUniformVec4( sp, UNIFORM_LIGHTORIGIN, vector );

		VectorScale( ps->lightViewAxis[0], 1.0f / ps->viewRadius, vector );
		GLSL_SetUniformVec3( sp, UNIFORM_LIGHTFORWARD, vector );

		VectorScale( ps->lightViewAxis[1], 1.0f / ps->viewRadius, vector );
		GLSL_SetUniformVec3( sp, UNIFORM_LIGHTRIGHT, vector );

		VectorScale( ps->lightViewAxis[2], 1.0f / ps->viewRadius, vector );
		GLSL_SetUniformVec3( sp, UNIFORM_LIGHTUP, vector );

		GLSL_SetUniformFloat( sp, UNIFORM_LIGHTRADIUS, radius );

		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:1027
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
		GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 0 );

		// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:1030
		GL_BindToTMU( tr.pshadowMaps[l], TB_DIFFUSEMAP );

		//
		// draw
		//

		R_DrawElements( input->numIndexes, input->firstIndex );

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		//backEnd.pc.c_dlightIndexes += tess.numIndexes;
	}
}


/*
===================
RB_FogPass

Blends a fog texture on top of everything else
===================
*/
static void RB_FogPass( int wolfFog ) {
	fog_t       *fog = NULL;
	vec4_t color;
	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;
	shaderProgram_t *sp;
	glfog_t *glFog = NULL;

	int deformGen;
	vec5_t deformParams;

	if ( tr.refdef.rdflags & RDF_SNOOPERVIEW ) { // no fog pass in snooper
		return;
	}

	if ( wolfFog ) {
		if ( backEnd.projection2D ) {
			return;
		}

		if ( backEnd.refdef.rdflags & RDF_DRAWINGSKY ) {
			if ( glfogsettings[FOG_SKY].registered ) {
				glFog = &glfogsettings[FOG_SKY];
			}
		}

		if ( skyboxportal && backEnd.refdef.rdflags & RDF_SKYBOXPORTAL ) {
			if ( glfogsettings[FOG_PORTALVIEW].registered ) {
				glFog = &glfogsettings[FOG_PORTALVIEW];
			}
		} else {
			if ( glfogNum > FOG_NONE ) {
				glFog = &glfogsettings[FOG_CURRENT];
			}
		}

		if ( !glFog ) {
			return;
		}
	}

	ComputeDeformValues( &deformGen, deformParams );

	{
		int index = 0;

		if ( deformGen != DGEN_NONE ) {
			index |= FOGDEF_USE_DEFORM_VERTEXES;
		}

		if ( glState.vertexAnimation ) {
			index |= FOGDEF_USE_VERTEX_ANIMATION;
		} else if ( glState.boneAnimation ) {
			index |= FOGDEF_USE_BONE_ANIMATION;
		}

		if ( wolfFog ) {
			if ( glFog->mode == GL_LINEAR ) {
				index |= FOGDEF_USE_WOLF_FOG_LINEAR;
			} else { // if (glFog->mode == GL_EXP)
				index |= FOGDEF_USE_WOLF_FOG_EXPONENTIAL;
			}
		}

		sp = &tr.fogShader[index];
	}

	backEnd.pc.c_fogDraws++;

	// PARITY-SEAM(Metal-device): GLSL_BindProgram → seamed in tr_glsl.cpp. GL2 ref: tr_shade.c:1080
	GLSL_BindProgram( sp );

	if ( !wolfFog ) {
		fog = tr.world->fogs + tess.fogNum;
	}

	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );

	GLSL_SetUniformFloat( sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation );

	if ( glState.boneAnimation ) {
		GLSL_SetUniformMat4BoneMatrix( sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation );
	}

	GLSL_SetUniformInt( sp, UNIFORM_DEFORMGEN, deformGen );
	if ( deformGen != DGEN_NONE ) {
		GLSL_SetUniformFloat5( sp, UNIFORM_DEFORMPARAMS, deformParams );
		GLSL_SetUniformFloat( sp, UNIFORM_TIME, tess.shaderTime );

		if ( tess.shader->deforms[0].deformationWave.frequency < 0 ) {
			vec3_t worldUp;

			if ( VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
				VectorSet( backEnd.currentEntity->e.fireRiseDir, 0, 0, 1 );
			}

			if ( backEnd.currentEntity->e.hModel ) {    // world surfaces dont have an axis
				VectorRotate( backEnd.currentEntity->e.fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
			} else {
				VectorCopy( backEnd.currentEntity->e.fireRiseDir, worldUp );
			}

			GLSL_SetUniformVec3( sp, UNIFORM_FIRERISEDIR, worldUp );
		}
	}

	if ( wolfFog ) {
		color[0] = glFog->color[0];
		color[1] = glFog->color[1];
		color[2] = glFog->color[2];
		color[3] = glFog->color[3];
	} else
	{
		color[0] = ( (unsigned char *)( &fog->colorInt ) )[0] / 255.0f;
		color[1] = ( (unsigned char *)( &fog->colorInt ) )[1] / 255.0f;
		color[2] = ( (unsigned char *)( &fog->colorInt ) )[2] / 255.0f;
		color[3] = ( (unsigned char *)( &fog->colorInt ) )[3] / 255.0f;
	}
	GLSL_SetUniformVec4( sp, UNIFORM_COLOR, color );

	ComputeFogValues( fogDistanceVector, fogDepthVector, &eyeT, glFog );

	GLSL_SetUniformVec4( sp, UNIFORM_FOGDISTANCE, fogDistanceVector );
	GLSL_SetUniformVec4( sp, UNIFORM_FOGDEPTH, fogDepthVector );
	GLSL_SetUniformFloat( sp, UNIFORM_FOGEYET, eyeT );

	if ( tess.shader->fogPass == FP_EQUAL ) {
		// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:1113
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	} else {
		// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:1115
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	}
	GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 0 );

	R_DrawElements( tess.numIndexes, tess.firstIndex );
}


static unsigned int RB_CalcShaderVertexAttribs( shaderCommands_t *input ) {
	unsigned int vertexAttribs = input->shader->vertexAttribs;

	if ( glState.vertexAnimation ) {
		vertexAttribs |= ATTR_POSITION2;
		if ( vertexAttribs & ATTR_NORMAL ) {
			vertexAttribs |= ATTR_NORMAL2;
			vertexAttribs |= ATTR_TANGENT2;
		}
	}

	return vertexAttribs;
}

static void RB_IterateStagesGeneric( shaderCommands_t *input ) {
	int stage;

	glfog_t *glFog;
	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;

	int deformGen;
	vec5_t deformParams;

	qboolean renderToCubemap = ( tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo ) ? qtrue : qfalse;

	ComputeDeformValues( &deformGen, deformParams );

	for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ )
	{
		shaderStage_t *pStage = input->xstages[stage];
		shaderProgram_t *sp;
		vec4_t texMatrix[8];

		if ( !pStage ) {
			break;
		}

		glFog = NULL;

		if ( backEnd.depthFill ) {
			if ( pStage->glslShaderGroup == tr.lightallShader ) {
				int index = 0;

				if ( backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity ) {
					if ( glState.boneAnimation ) {
						index |= LIGHTDEF_ENTITY_BONE_ANIMATION;
					} else
					{
						index |= LIGHTDEF_ENTITY_VERTEX_ANIMATION;
					}
				}

				if ( pStage->stateBits & GLS_ATEST_BITS ) {
					index |= LIGHTDEF_USE_TCGEN_AND_TCMOD;
				}

				sp = &pStage->glslShaderGroup[index];
			} else
			{
				int shaderAttribs = 0;

				if ( tess.shader->numDeforms && !ShaderRequiresCPUDeforms( tess.shader ) ) {
					shaderAttribs |= GENERICDEF_USE_DEFORM_VERTEXES;
				}

				if ( glState.vertexAnimation ) {
					shaderAttribs |= GENERICDEF_USE_VERTEX_ANIMATION;
				} else if ( glState.boneAnimation )     {
					shaderAttribs |= GENERICDEF_USE_BONE_ANIMATION;
				}

				if ( pStage->stateBits & GLS_ATEST_BITS ) {
					shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
				}

				sp = &tr.genericShader[shaderAttribs];
			}
		} else if ( pStage->glslShaderGroup == tr.lightallShader )     {
			int index = pStage->glslShaderIndex;

			if ( backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity ) {
				if ( glState.boneAnimation ) {
					index |= LIGHTDEF_ENTITY_BONE_ANIMATION;
				} else
				{
					index |= LIGHTDEF_ENTITY_VERTEX_ANIMATION;
				}
			}

			if ( r_sunlightMode->integer && ( backEnd.viewParms.flags & VPF_USESUNLIGHT ) && ( index & LIGHTDEF_LIGHTTYPE_MASK ) ) {
				index |= LIGHTDEF_USE_SHADOWMAP;
			}

			if ( r_lightmap->integer && ( ( index & LIGHTDEF_LIGHTTYPE_MASK ) == LIGHTDEF_USE_LIGHTMAP ) ) {
				index = LIGHTDEF_USE_TCGEN_AND_TCMOD;
			}

			sp = &pStage->glslShaderGroup[index];

			backEnd.pc.c_lightallDraws++;
		} else
		{
			if ( r_wolffog->integer && pStage->adjustColorsForFog && !backEnd.projection2D ) {
				if ( !tess.shader->noFog || pStage->isFogged ) {
					if ( backEnd.refdef.rdflags & RDF_DRAWINGSKY ) {
						if ( glfogsettings[FOG_SKY].registered ) {
							glFog = &glfogsettings[FOG_SKY];
						}
					}

					if ( skyboxportal && backEnd.refdef.rdflags & RDF_SKYBOXPORTAL ) {
						if ( glfogsettings[FOG_PORTALVIEW].registered ) {
							glFog = &glfogsettings[FOG_PORTALVIEW];
						}
					} else {
						if ( glfogNum > FOG_NONE ) {
							glFog = &glfogsettings[FOG_CURRENT];
						}
					}
				}
			}

			sp = GLSL_GetGenericShaderProgram( stage, glFog );

			backEnd.pc.c_genericDraws++;
		}

		// PARITY-SEAM(Metal-device): GLSL_BindProgram → seamed in tr_glsl.cpp. GL2 ref: tr_shade.c:1255
		GLSL_BindProgram( sp );

		GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
		GLSL_SetUniformVec3( sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );
		GLSL_SetUniformVec3( sp, UNIFORM_LOCALVIEWORIGIN, backEnd.or.viewOrigin );

		GLSL_SetUniformFloat( sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation );

		if ( glState.boneAnimation ) {
			GLSL_SetUniformMat4BoneMatrix( sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation );
		}

		if ( ( deformGen != DGEN_NONE && tess.shader->deforms[0].deformationWave.frequency < 0 )
			 || pStage->alphaGen == AGEN_NORMALZFADE ) {
			vec3_t worldUp;

			if ( VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
				VectorSet( backEnd.currentEntity->e.fireRiseDir, 0, 0, 1 );
			}

			if ( backEnd.currentEntity->e.hModel ) {    // world surfaces dont have an axis
				VectorRotate( backEnd.currentEntity->e.fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
			} else {
				VectorCopy( backEnd.currentEntity->e.fireRiseDir, worldUp );
			}

			GLSL_SetUniformVec3( sp, UNIFORM_FIRERISEDIR, worldUp );
		}

		GLSL_SetUniformInt( sp, UNIFORM_DEFORMGEN, deformGen );
		if ( deformGen != DGEN_NONE ) {
			GLSL_SetUniformFloat5( sp, UNIFORM_DEFORMPARAMS, deformParams );
			GLSL_SetUniformFloat( sp, UNIFORM_TIME, tess.shaderTime );
		}

		if ( input->fogNum || glFog ) {
			ComputeFogValues( fogDistanceVector, fogDepthVector, &eyeT, glFog );

			GLSL_SetUniformVec4( sp, UNIFORM_FOGDISTANCE, fogDistanceVector );
			GLSL_SetUniformVec4( sp, UNIFORM_FOGDEPTH, fogDepthVector );
			GLSL_SetUniformFloat( sp, UNIFORM_FOGEYET, eyeT );
		}

		if ( ( pStage->stateBits & GLS_ATEST_BITS ) == GLS_ATEST_GT_0 ) {
			GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 1 );
		} else if ( ( pStage->stateBits & GLS_ATEST_BITS ) == GLS_ATEST_LT_80 )       {
			GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 2 );
		} else if ( ( pStage->stateBits & GLS_ATEST_BITS ) == GLS_ATEST_GE_80 )       {
			GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 3 );
		} else
		{
			GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 0 );
		}


		{
			vec4_t baseColor;
			vec4_t vertColor;
			int fadeStart, fadeEnd;

			ComputeShaderColors( pStage, baseColor, vertColor, pStage->stateBits );

			//----(SA)	fading model stuff
			if ( backEnd.currentEntity ) {
				fadeStart = backEnd.currentEntity->e.fadeStartTime;
			} else
			{
				fadeStart = 0;
			}

			if ( fadeStart ) {
				fadeEnd = backEnd.currentEntity->e.fadeEndTime;

				if ( fadeStart > tr.refdef.time ) {
					// has not started to fade yet
					GL_State( pStage->stateBits );
				} else
				{
					unsigned int tempState;
					float alphaval;

					if ( fadeEnd < tr.refdef.time ) {
						// entity faded out completely
						continue;
					}

					alphaval = (float)( fadeEnd - tr.refdef.time ) / (float)( fadeEnd - fadeStart );

					tempState = pStage->stateBits;
					// remove the current blend, and don't write to Z buffer
					tempState &= ~( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_DEPTHMASK_TRUE );
					// set the blend to src_alpha, dst_one_minus_src_alpha
					tempState |= ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
					GL_State( tempState );
					GL_Cull( CT_FRONT_SIDED );
					// modulate the alpha component of each vertex in the render list
					baseColor[3] *= alphaval;
					vertColor[3] *= alphaval;
				}
			} else
			{
				GL_State( pStage->stateBits );
			}
			//----(SA)	end

			GLSL_SetUniformVec4( sp, UNIFORM_BASECOLOR, baseColor );
			GLSL_SetUniformVec4( sp, UNIFORM_VERTCOLOR, vertColor );
		}

		if ( pStage->rgbGen == CGEN_LIGHTING_DIFFUSE ) {
			vec4_t vec;

			VectorScale( backEnd.currentEntity->ambientLight, 1.0f / 255.0f, vec );
			GLSL_SetUniformVec3( sp, UNIFORM_AMBIENTLIGHT, vec );

			VectorScale( backEnd.currentEntity->directedLight, 1.0f / 255.0f, vec );
			GLSL_SetUniformVec3( sp, UNIFORM_DIRECTEDLIGHT, vec );

			VectorCopy( backEnd.currentEntity->lightDir, vec );
			vec[3] = 0.0f;
			GLSL_SetUniformVec4( sp, UNIFORM_LIGHTORIGIN, vec );
			GLSL_SetUniformVec3( sp, UNIFORM_MODELLIGHTDIR, backEnd.currentEntity->modelLightDir );

			GLSL_SetUniformFloat( sp, UNIFORM_LIGHTRADIUS, 0.0f );
		}

		if ( pStage->alphaGen == AGEN_PORTAL ) {
			GLSL_SetUniformFloat( sp, UNIFORM_PORTALRANGE, tess.shader->portalRange );
		} else if ( pStage->alphaGen == AGEN_NORMALZFADE )     {
			float lowest, highest;
			//qboolean zombieEffect = qfalse;

			lowest = pStage->zFadeBounds[0];
			if ( lowest == -1000 ) {    // use entity alpha
				lowest = backEnd.currentEntity->e.shaderTime;
				//zombieEffect = qtrue;
			}
			highest = pStage->zFadeBounds[1];
			if ( highest == -1000 ) {   // use entity alpha
				highest = backEnd.currentEntity->e.shaderTime;
				//zombieEffect = qtrue;
			}

			// TODO: Handle normalzfade zombie effect

			GLSL_SetUniformFloat( sp, UNIFORM_ZFADELOWEST, lowest );
			GLSL_SetUniformFloat( sp, UNIFORM_ZFADEHIGHEST, highest );
		}

		GLSL_SetUniformInt( sp, UNIFORM_COLORGEN, pStage->rgbGen );
		GLSL_SetUniformInt( sp, UNIFORM_ALPHAGEN, pStage->alphaGen );

		if ( input->fogNum || glFog ) {
			vec4_t fogColorMask;

			ComputeFogColorMask( pStage, fogColorMask );

			GLSL_SetUniformVec4( sp, UNIFORM_FOGCOLORMASK, fogColorMask );
		}

		if ( r_lightmap->integer ) {
			vec4_t st[2];
			VectorSet4( st[0], 1.0f, 0.0f, 0.0f, 0.0f );
			VectorSet4( st[1], 0.0f, 1.0f, 0.0f, 0.0f );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX0, st[0] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX1, st[1] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX2, st[0] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX3, st[1] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX4, st[0] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX5, st[1] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX6, st[0] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX7, st[1] );

			GLSL_SetUniformInt( sp, UNIFORM_TCGEN0, TCGEN_LIGHTMAP );
		} else
		{
			ComputeTexMods( pStage, TB_DIFFUSEMAP, texMatrix );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6] );
			GLSL_SetUniformVec4( sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7] );

			GLSL_SetUniformInt( sp, UNIFORM_TCGEN0, pStage->bundle[0].tcGen );
			if ( pStage->bundle[0].tcGen == TCGEN_VECTOR ) {
				vec3_t vec;

				VectorCopy( pStage->bundle[0].tcGenVectors[0], vec );
				GLSL_SetUniformVec3( sp, UNIFORM_TCGEN0VECTOR0, vec );
				VectorCopy( pStage->bundle[0].tcGenVectors[1], vec );
				GLSL_SetUniformVec3( sp, UNIFORM_TCGEN0VECTOR1, vec );
			}
		}

		GLSL_SetUniformMat4( sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix );

		GLSL_SetUniformVec4( sp, UNIFORM_NORMALSCALE, pStage->normalScale );

		{
			vec4_t specularScale;
			Vector4Copy( pStage->specularScale, specularScale );

			if ( renderToCubemap ) {
				// force specular to nonmetal if rendering cubemaps
				if ( r_pbr->integer ) {
					specularScale[1] = 0.0f;
				}
			}

			GLSL_SetUniformVec4( sp, UNIFORM_SPECULARSCALE, specularScale );
		}

		//GLSL_SetUniformFloat(sp, UNIFORM_MAPLIGHTSCALE, backEnd.refdef.mapLightScale);

		//
		// do multitexture
		//
		if ( backEnd.depthFill ) {
			if ( !( pStage->stateBits & GLS_ATEST_BITS ) ) {
				// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:1411
				GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
			} else if ( pStage->bundle[TB_COLORMAP].image[0] != 0 ) {
				R_BindAnimatedImageToTMU( &pStage->bundle[TB_COLORMAP], TB_COLORMAP );
			}
		} else if ( pStage->glslShaderGroup == tr.lightallShader )   {
			int i;
			vec4_t enableTextures;

			if ( r_sunlightMode->integer && ( backEnd.viewParms.flags & VPF_USESUNLIGHT ) && ( pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK ) ) {
				// FIXME: screenShadowImage is NULL if no framebuffers
				if ( tr.screenShadowImage ) {
					// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:1424
					GL_BindToTMU( tr.screenShadowImage, TB_SHADOWMAP );
				}
				GLSL_SetUniformVec3( sp, UNIFORM_PRIMARYLIGHTAMBIENT, backEnd.refdef.sunAmbCol );
				if ( r_pbr->integer ) {
					vec3_t color;

					color[0] = backEnd.refdef.sunCol[0] * backEnd.refdef.sunCol[0];
					color[1] = backEnd.refdef.sunCol[1] * backEnd.refdef.sunCol[1];
					color[2] = backEnd.refdef.sunCol[2] * backEnd.refdef.sunCol[2];
					GLSL_SetUniformVec3( sp, UNIFORM_PRIMARYLIGHTCOLOR, color );
				} else
				{
					GLSL_SetUniformVec3( sp, UNIFORM_PRIMARYLIGHTCOLOR, backEnd.refdef.sunCol );
				}
				GLSL_SetUniformVec4( sp, UNIFORM_PRIMARYLIGHTORIGIN,  backEnd.refdef.sunDir );
			}

			VectorSet4( enableTextures, 0, 0, 0, 0 );
			if ( ( r_lightmap->integer == 1 || r_lightmap->integer == 2 ) && pStage->bundle[TB_LIGHTMAP].image[0] ) {
				for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
				{
					if ( i == TB_COLORMAP ) {
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_LIGHTMAP], i );
					} else {
						// PARITY-SEAM(Metal-device): GL_BindToTMU. GL2 ref: tr_shade.c:1450
						GL_BindToTMU( tr.whiteImage, i );
					}
				}
			} else if ( r_lightmap->integer == 3 && pStage->bundle[TB_DELUXEMAP].image[0] )     {
				for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
				{
					if ( i == TB_COLORMAP ) {
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_DELUXEMAP], i );
					} else {
						// PARITY-SEAM(Metal-device): GL_BindToTMU. GL2 ref: tr_shade.c:1459
						GL_BindToTMU( tr.whiteImage, i );
					}
				}
			} else
			{
				qboolean light = ( ( pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK ) != 0 ) ? qtrue : qfalse;
				qboolean fastLight = ( !( r_normalMapping->integer || r_specularMapping->integer ) ) ? qtrue : qfalse;

				if ( pStage->bundle[TB_DIFFUSEMAP].image[0] ) {
					R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP );
				}

				if ( pStage->bundle[TB_LIGHTMAP].image[0] ) {
					R_BindAnimatedImageToTMU( &pStage->bundle[TB_LIGHTMAP], TB_LIGHTMAP );
				} else {
					// Metal: shared lightall MSL always samples TB_LIGHTMAP. Bind white so non-lightmap
					// (vertex-lit) surfaces get a neutral lightmap (1,1,1,1) instead of a stale binding.
					GL_BindToTMU( tr.whiteImage, TB_LIGHTMAP );
				}

				// bind textures that are sampled and used in the glsl shader, and
				// bind whiteImage to textures that are sampled but zeroed in the glsl shader
				//
				// alternatives:
				//  - use the last bound texture
				//     -> costs more to sample a higher res texture then throw out the result
				//  - disable texture sampling in glsl shader with #ifdefs, as before
				//     -> increases the number of shaders that must be compiled
				//
				if ( light && !fastLight ) {
					if ( pStage->bundle[TB_NORMALMAP].image[0] ) {
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP );
						enableTextures[0] = 1.0f;
					} else if ( r_normalMapping->integer )     {
						// PARITY-SEAM(Metal-device): GL_BindToTMU. GL2 ref: tr_shade.c:1491
						GL_BindToTMU( tr.whiteImage, TB_NORMALMAP );
					}

					if ( pStage->bundle[TB_DELUXEMAP].image[0] ) {
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_DELUXEMAP], TB_DELUXEMAP );
						enableTextures[1] = 1.0f;
					} else if ( r_deluxeMapping->integer )     {
						// PARITY-SEAM(Metal-device): GL_BindToTMU. GL2 ref: tr_shade.c:1498
						GL_BindToTMU( tr.whiteImage, TB_DELUXEMAP );
					}

					if ( pStage->bundle[TB_SPECULARMAP].image[0] ) {
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP );
						enableTextures[2] = 1.0f;
					} else if ( r_specularMapping->integer )     {
						// PARITY-SEAM(Metal-device): GL_BindToTMU. GL2 ref: tr_shade.c:1507
						GL_BindToTMU( tr.whiteImage, TB_SPECULARMAP );
					}
				}

				enableTextures[3] = ( r_cubeMapping->integer && !( tr.viewParms.flags & VPF_NOCUBEMAPS ) && input->cubemapIndex ) ? 1.0f : 0.0f;
			}

			GLSL_SetUniformVec4( sp, UNIFORM_ENABLETEXTURES, enableTextures );
		} else if ( pStage->bundle[1].image[0] != 0 )   {
			R_BindAnimatedImageToTMU( &pStage->bundle[0], 0 );
			R_BindAnimatedImageToTMU( &pStage->bundle[1], 1 );
		} else
		{
			//
			// set state
			//
			R_BindAnimatedImageToTMU( &pStage->bundle[0], 0 );
		}

		//
		// testing cube map
		//
		if ( !( tr.viewParms.flags & VPF_NOCUBEMAPS ) && input->cubemapIndex && r_cubeMapping->integer ) {
			vec4_t vec;
			cubemap_t *cubemap = &tr.cubemaps[input->cubemapIndex - 1];

			// FIXME: cubemap image could be NULL if cubemap isn't renderer or loaded
			if ( cubemap->image ) {
				// PARITY-SEAM(Metal-device): GL_BindToTMU → seamed in tr_image.cpp. GL2 ref: tr_shade.c:1538
				GL_BindToTMU( cubemap->image, TB_CUBEMAP );
			}

			VectorSubtract( cubemap->origin, backEnd.viewParms.or.origin, vec );
			vec[3] = 1.0f;

			VectorScale4( vec, 1.0f / cubemap->parallaxRadius, vec );

			GLSL_SetUniformVec4( sp, UNIFORM_CUBEMAPINFO, vec );
		}

		//
		// draw
		//
		R_DrawElements( input->numIndexes, input->firstIndex );

		// allow skipping out to show just lightmaps during development
		if ( r_lightmap->integer && ( pStage->bundle[0].isLightmap || pStage->bundle[1].isLightmap ) ) {
			break;
		}

		if ( backEnd.depthFill ) {
			break;
		}
	}
}


static void RB_RenderShadowmap( shaderCommands_t *input ) {
	int deformGen;
	vec5_t deformParams;

	ComputeDeformValues( &deformGen, deformParams );

	{
		shaderProgram_t *sp = &tr.shadowmapShader[0];

		if ( glState.vertexAnimation ) {
			sp = &tr.shadowmapShader[SHADOWMAPDEF_USE_VERTEX_ANIMATION];
		} else if ( glState.boneAnimation )     {
			sp = &tr.shadowmapShader[SHADOWMAPDEF_USE_BONE_ANIMATION];
		}

		vec4_t vector;

		// PARITY-SEAM(Metal-device): GLSL_BindProgram → seamed in tr_glsl.cpp. GL2 ref: tr_shade.c:1586
		GLSL_BindProgram( sp );

		GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );

		GLSL_SetUniformMat4( sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix );

		GLSL_SetUniformFloat( sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation );

		if ( glState.boneAnimation ) {
			GLSL_SetUniformMat4BoneMatrix( sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation );
		}

		GLSL_SetUniformInt( sp, UNIFORM_DEFORMGEN, deformGen );
		if ( deformGen != DGEN_NONE ) {
			GLSL_SetUniformFloat5( sp, UNIFORM_DEFORMPARAMS, deformParams );
			GLSL_SetUniformFloat( sp, UNIFORM_TIME, tess.shaderTime );

			if ( tess.shader->deforms[0].deformationWave.frequency < 0 ) {
				vec3_t worldUp;

				if ( VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
					VectorSet( backEnd.currentEntity->e.fireRiseDir, 0, 0, 1 );
				}

				if ( backEnd.currentEntity->e.hModel ) {    // world surfaces dont have an axis
					VectorRotate( backEnd.currentEntity->e.fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
				} else {
					VectorCopy( backEnd.currentEntity->e.fireRiseDir, worldUp );
				}

				GLSL_SetUniformVec3( sp, UNIFORM_FIRERISEDIR, worldUp );
			}
		}

		VectorCopy( backEnd.viewParms.or.origin, vector );
		vector[3] = 1.0f;
		GLSL_SetUniformVec4( sp, UNIFORM_LIGHTORIGIN, vector );
		GLSL_SetUniformFloat( sp, UNIFORM_LIGHTRADIUS, backEnd.viewParms.zFar );

		// PARITY-SEAM(Metal-device): GL_State → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:1611
		GL_State( 0 );
		GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 0 );

		//
		// do multitexture
		//
		//if ( pStage->glslShaderGroup )
		{
			//
			// draw
			//

			R_DrawElements( input->numIndexes, input->firstIndex );
		}
	}
}



/*
** RB_StageIteratorGeneric
*/
static void RB_StageIteratorGenericInner( void );

void RB_StageIteratorGeneric( void ) {
	// tess is fixed for the duration of the iterator (after the deform step, which runs
	// before any draw), so all passes can share one upload of its streams.
	R_MetalBeginTessUploadScope();
	RB_StageIteratorGenericInner();
	R_MetalEndTessUploadScope();
}

static void RB_StageIteratorGenericInner( void ) {
	shaderCommands_t *input;
	unsigned int vertexAttribs = 0;

	input = &tess;

	if ( !input->numVertexes || !input->numIndexes ) {
		return;
	}

	if ( tess.useInternalVao ) {
		RB_DeformTessGeometry();
	}

	vertexAttribs = RB_CalcShaderVertexAttribs( input );

	if ( tess.useInternalVao ) {
		// PARITY-SEAM(Metal-device): RB_UpdateTessVao → seamed in tr_vbo.cpp;
		//   maps to MTLDevice newBufferWithBytes/setVertexBuffer. GL2 ref: tr_shade.c:1654
		RB_UpdateTessVao( vertexAttribs );
	} else
	{
		backEnd.pc.c_staticVaoDraws++;
	}

	//
	// log this call
	//
	if ( r_logFile->integer ) {
		// PARITY-SEAM(Metal-device): GLimp_LogComment → no-op under Metal (sdl_glimp.c).
		// GLimp_LogComment( va("--- RB_StageIteratorGeneric( %s ) ---\n", tess.shader->name) );
	}

	//
	// set face culling appropriately
	//
	if ( input->shader->cullType == CT_TWO_SIDED ) {
		// PARITY-SEAM(Metal-device): GL_Cull → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:1676
		GL_Cull( CT_TWO_SIDED );
	} else
	{
		qboolean cullFront = ( input->shader->cullType == CT_FRONT_SIDED ) ? qtrue : qfalse;

		if ( backEnd.viewParms.flags & VPF_DEPTHSHADOW ) {
			cullFront = cullFront ? qfalse : qtrue;
		}

		if ( backEnd.viewParms.isMirror ) {
			cullFront = cullFront ? qfalse : qtrue;
		}

		if ( backEnd.currentEntity && backEnd.currentEntity->mirrored ) {
			cullFront = cullFront ? qfalse : qtrue;
		}

		if ( cullFront ) {
			// PARITY-SEAM(Metal-device): GL_Cull → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:1692
			GL_Cull( CT_FRONT_SIDED );
		} else {
			// PARITY-SEAM(Metal-device): GL_Cull → seamed in tr_backend.cpp. GL2 ref: tr_shade.c:1694
			GL_Cull( CT_BACK_SIDED );
		}
	}

	// set polygon offset if necessary
	if ( input->shader->polygonOffset ) {
		// GL2 ref: tr_shade.c:1698-1701 qglEnable(GL_POLYGON_OFFSET_FILL)
		GLMetal_SetDepthBias( r_offsetUnits->value, r_offsetFactor->value, 0.0f );
	}

	//
	// render depth if in depthfill mode
	//
	if ( backEnd.depthFill ) {
		RB_IterateStagesGeneric( input );

		//
		// reset polygon offset
		//
		if ( input->shader->polygonOffset ) {
			// GL2 ref: tr_shade.c:1713-1716 qglDisable(GL_POLYGON_OFFSET_FILL)
			GLMetal_SetDepthBias( 0.0f, 0.0f, 0.0f );
		}

		return;
	}

	//
	// render shadowmap if in shadowmap mode
	//
	if ( backEnd.viewParms.flags & VPF_SHADOWMAP ) {
		if ( input->shader->sort == SS_OPAQUE ) {
			RB_RenderShadowmap( input );
		}
		//
		// reset polygon offset
		//
		if ( input->shader->polygonOffset ) {
			// GL2 ref: tr_shade.c:1733-1736 qglDisable(GL_POLYGON_OFFSET_FILL)
			GLMetal_SetDepthBias( 0.0f, 0.0f, 0.0f );
		}

		return;
	}

	//
	//
	// call shader function
	//
	RB_IterateStagesGeneric( input );

	//
	// pshadows!
	//
	if ( glRefConfig.framebufferObject && r_shadows->integer == 4 && tess.pshadowBits
		 && tess.shader->sort <= SS_OPAQUE && !( tess.shader->surfaceFlags & ( SURF_NODLIGHT | SURF_SKY ) ) ) {
		ProjectPshadowVBOGLSL();
	}


	//
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE && r_lightmap->integer == 0
		 && !( tess.shader->surfaceFlags & ( SURF_NODLIGHT | SURF_SKY ) ) ) {
		if ( tess.shader->numUnfoggedPasses == 1 && tess.xstages[0]->glslShaderGroup == tr.lightallShader
			 && ( tess.xstages[0]->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK ) && r_dlightMode->integer ) {
			ForwardDlight();
		} else
		{
			ProjectDlightTexture();
		}
	}

	//
	// now do fog
	//
	if ( tess.fogNum && tess.shader->fogPass ) {
		RB_FogPass( 0 );
	}

	//
	// RTCW fog
	// may not match original RTCW fog, since that's done per stage
	//
	if ( r_wolffog->integer && tess.shader->fogPass && tess.shader->sort <= SS_OPAQUE ) {
		int stage, stageFog = 0;

		if ( tess.shader->noFog ) {
			// make sure at least one stage has fog
			for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ )
			{
				shaderStage_t *pStage = tess.xstages[stage];

				if ( !pStage ) {
					break;
				}

				if ( pStage->isFogged ) {
					stageFog = 1;
					break;
				}
			}
		}

		if ( !tess.shader->noFog || stageFog ) {
			RB_FogPass( 1 );
		}
	}

	//
	// reset polygon offset
	//
	if ( input->shader->polygonOffset ) {
		// GL2 ref: tr_shade.c:1782-1785 qglDisable(GL_POLYGON_OFFSET_FILL)
		GLMetal_SetDepthBias( 0.0f, 0.0f, 0.0f );
	}


}

/*
** RB_EndSurface
*/
void RB_EndSurface( void ) {
	shaderCommands_t *input;

	input = &tess;

	if ( input->numIndexes == 0 || input->numVertexes == 0 ) {
		return;
	}

	if ( input->indexes[SHADER_MAX_INDEXES - 1] != 0 ) {
		ri.Error( ERR_DROP, "RB_EndSurface() - SHADER_MAX_INDEXES hit" );
	}
	if ( input->xyz[SHADER_MAX_VERTEXES - 1][0] != 0 ) {
		ri.Error( ERR_DROP, "RB_EndSurface() - SHADER_MAX_VERTEXES hit" );
	}

	if ( tess.shader == tr.shadowShader ) {
		RB_ShadowTessEnd();
		return;
	}

	// for debugging of sort order issues, stop rendering after a given sort value
	if ( r_debugSort->integer && r_debugSort->integer < tess.shader->sort ) {
		return;
	}

	if ( tess.useCacheVao ) {
		// PARITY-SEAM(Metal-device): VaoCache_Commit → seamed in tr_vbo.cpp;
		//   flushes queued index data to GPU buffer. GL2 ref: tr_shade.c:1821
		VaoCache_Commit();
	}

	//
	// update performance counters
	//
	backEnd.pc.c_shaders++;
	backEnd.pc.c_vertexes += tess.numVertexes;
	backEnd.pc.c_indexes += tess.numIndexes;
	backEnd.pc.c_totalIndexes += tess.numIndexes * tess.numPasses;

	//
	// call off to shader specific tess end function
	//
	// PARITY-SEAM(Metal-device): tess.currentStageIteratorFunc() dispatches to
	//   RB_StageIteratorGeneric or RB_StageIteratorSky which contain the actual
	//   GPU submit chain. GL2 ref: tr_shade.c:1834
	// PHASE5-M5.2: 2D draw submission enabled.
	tess.currentStageIteratorFunc();

	//
	// draw debugging stuff
	//
	if ( r_showtris->integer ) {
		DrawTris( input );
	}
	if ( r_shownormals->integer ) {
		DrawNormals( input );
	}
	// clear shader so we can tell we don't have any unclosed surfaces
	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.firstIndex = 0;
	tess.useCacheVao = qfalse;
	tess.useInternalVao = qfalse;
	tess.metalStaticVao = NULL;

	// PARITY-SEAM(Metal-device): GLimp_LogComment → no-op under Metal (sdl_glimp.c).
	// GLimp_LogComment( "----------\n" );
}

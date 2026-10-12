/*
===========================================================================
Copyright (C) 2006-2009 Robert Beckebans <trebor_7@users.sourceforge.net>

This file is part of XreaL source code.

XreaL source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

XreaL source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with XreaL source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_glsl.cpp: GLSL GPU shader-program manager — Metal renderer port of tr_glsl.c
// GL2 ref: tr_glsl.c
//
// Include order: q_shared.h first (for vec_t/qboolean types), then tr_extramath.h
// in C++ context so Mat4*/SpheresIntersect declarations keep C++ linkage matching
// tr_extramath.cpp.  The tr_extramath.h include guard prevents re-declaration when
// tr_local.h later includes it inside extern "C".
extern "C" {
#include "../qcommon/q_shared.h"   // vec_t, vec3_t, qboolean, etc.
}

#include "tr_extramath.h"           // Mat4* — C++ linkage matches tr_extramath.cpp

extern "C" {
#include "tr_local.h"   // tr_extramath.h skipped here via include guard
#include "tr_dsa.h"
}

// PHASE5-M5.2: Metal pipeline infrastructure — included OUTSIDE extern "C" because
// tr_metal_shadersource.h includes Metal-cpp headers with C++ templates.
// tr_metal_shadersource.h wraps its own C declarations in extern "C" internally.
#include "tr_metal_shadersource.h"
#include "tr_metal_device.h"

#include <map>

// The MSL in tr_metal_shadersource.cpp spells these enum values out as numbers (GLSL got
// them from the header GLSL_GetShaderHeader builds). RTCW's enums differ from Quake 3's
// and differ between the single and multi player trees, so a mismatch must not compile.
static_assert( DGEN_WAVE_SIN == 1 && DGEN_WAVE_SQUARE == 2 && DGEN_WAVE_TRIANGLE == 3 &&
			   DGEN_WAVE_SAWTOOTH == 4 && DGEN_WAVE_INVERSE_SAWTOOTH == 5 && DGEN_BULGE == 7 && DGEN_MOVE == 8,
			   "deformGen values in the MSL sources are out of date" );
static_assert( TCGEN_LIGHTMAP == 2 && TCGEN_TEXTURE == 3 && TCGEN_ENVIRONMENT_MAPPED == 4 &&
			   TCGEN_FOG == 6 && TCGEN_VECTOR == 7,
			   "texCoordGen values in the MSL sources are out of date" );
static_assert( CGEN_LIGHTING_DIFFUSE == 11, "colorGen values in the MSL sources are out of date" );
static_assert( AGEN_NORMALZFADE == 4 && AGEN_LIGHTING_SPECULAR == 7 && AGEN_PORTAL == 9,
			   "alphaGen values in the MSL sources are out of date" );
#include <string>
#include <utility>
#include <vector>

extern "C" uint64_t GLMetal_VertexDescHashForAttribs( uint32_t attribs );

// GL-COMPAT: GL_VERTEX_SHADER / GL_FRAGMENT_SHADER — GLenum constants used in CPU
// control-flow for shader-type branching.  Actual GPU compilation is PARITY-SEAM;
// values preserved so CPU logic compiles identically.
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER   0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif

// GL-COMPAT: status / info-log query tokens (used inside PARITY-SEAM blocks only;
// declared here so surrounding CPU control-flow compiles).
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS  0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS     0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_SHADER_SOURCE_LENGTH
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#endif
#ifndef GL_ACTIVE_UNIFORMS
#define GL_ACTIVE_UNIFORMS 0x8B86
#endif
#ifndef GL_FALSE
#define GL_FALSE 0
#endif

// GL-COMPAT: qglesMajorVersion = 0 under Metal (no GLES path).
#ifndef qglesMajorVersion
#define qglesMajorVersion 0
#endif

// PARITY-SEAM(Metal-device): fallbackShader_* string pointers are defined by generated
// GLSL source headers (e.g. glsl/bokeh_vp.glsl → bokeh_vp.c).  Under Metal these GLSL
// sources are not compiled into the binary (GLSL→MSL translation is a DEFERRED
// design dependency — see lambert-port-tr_glsl.md disposition).  Declare as NULL
// pointers so the CPU permutation loops in GLSL_InitGPUShaders compile; the actual
// GPU upload path is a PARITY-SEAM and never reached during Phase 3 bring-up.
static const char *fallbackShader_bokeh_vp           = NULL;
static const char *fallbackShader_bokeh_fp           = NULL;
static const char *fallbackShader_calclevels4x_vp    = NULL;
static const char *fallbackShader_calclevels4x_fp    = NULL;
static const char *fallbackShader_depthblur_vp       = NULL;
static const char *fallbackShader_depthblur_fp       = NULL;
static const char *fallbackShader_dlight_vp          = NULL;
static const char *fallbackShader_dlight_fp          = NULL;
static const char *fallbackShader_down4x_vp          = NULL;
static const char *fallbackShader_down4x_fp          = NULL;
static const char *fallbackShader_fogpass_vp         = NULL;
static const char *fallbackShader_fogpass_fp         = NULL;
static const char *fallbackShader_generic_vp         = NULL;
static const char *fallbackShader_generic_fp         = NULL;
static const char *fallbackShader_lightall_vp        = NULL;
static const char *fallbackShader_lightall_fp        = NULL;
static const char *fallbackShader_pshadow_vp         = NULL;
static const char *fallbackShader_pshadow_fp         = NULL;
static const char *fallbackShader_shadowfill_vp      = NULL;
static const char *fallbackShader_shadowfill_fp      = NULL;
static const char *fallbackShader_shadowmask_vp      = NULL;
static const char *fallbackShader_shadowmask_fp      = NULL;
static const char *fallbackShader_ssao_vp            = NULL;
static const char *fallbackShader_ssao_fp            = NULL;
static const char *fallbackShader_texturecolor_vp    = NULL;
static const char *fallbackShader_texturecolor_fp    = NULL;
static const char *fallbackShader_tonemap_vp         = NULL;
static const char *fallbackShader_tonemap_fp         = NULL;
static const char *fallbackShader_greyscale_vp       = NULL;
static const char *fallbackShader_greyscale_fp       = NULL;

// PHASE5-M5.2: Active Metal program pointer — updated in GLSL_BindProgram;
// read in GLSL_GetActiveMetalProgram (called by R_DrawElements draw dispatch).
// GL2 equivalent: glUseProgram sets the current GL program object.
static shaderProgram_t *glMetal_activeProgram = NULL;

// GL2 ref: tr_glsl.c:60-155 (uniformsInfo table)
typedef struct uniformInfo_s
{
	char *name;
	int type;
}
uniformInfo_t;

// These must be in the same order as in uniform_t in tr_local.h.
static uniformInfo_t uniformsInfo[] =
{
	{ (char *)"u_DiffuseMap",  GLSL_INT },
	{ (char *)"u_LightMap",    GLSL_INT },
	{ (char *)"u_NormalMap",   GLSL_INT },
	{ (char *)"u_DeluxeMap",   GLSL_INT },
	{ (char *)"u_SpecularMap", GLSL_INT },

	{ (char *)"u_TextureMap", GLSL_INT },
	{ (char *)"u_LevelsMap",  GLSL_INT },
	{ (char *)"u_CubeMap",    GLSL_INT },

	{ (char *)"u_ScreenImageMap", GLSL_INT },
	{ (char *)"u_ScreenDepthMap", GLSL_INT },

	{ (char *)"u_ShadowMap",  GLSL_INT },
	{ (char *)"u_ShadowMap2", GLSL_INT },
	{ (char *)"u_ShadowMap3", GLSL_INT },
	{ (char *)"u_ShadowMap4", GLSL_INT },

	{ (char *)"u_ShadowMvp",  GLSL_MAT16 },
	{ (char *)"u_ShadowMvp2", GLSL_MAT16 },
	{ (char *)"u_ShadowMvp3", GLSL_MAT16 },
	{ (char *)"u_ShadowMvp4", GLSL_MAT16 },

	{ (char *)"u_EnableTextures", GLSL_VEC4 },

	{ (char *)"u_DiffuseTexMatrix0",  GLSL_VEC4 },
	{ (char *)"u_DiffuseTexMatrix1",  GLSL_VEC4 },
	{ (char *)"u_DiffuseTexMatrix2",  GLSL_VEC4 },
	{ (char *)"u_DiffuseTexMatrix3",  GLSL_VEC4 },
	{ (char *)"u_DiffuseTexMatrix4",  GLSL_VEC4 },
	{ (char *)"u_DiffuseTexMatrix5",  GLSL_VEC4 },
	{ (char *)"u_DiffuseTexMatrix6",  GLSL_VEC4 },
	{ (char *)"u_DiffuseTexMatrix7",  GLSL_VEC4 },

	{ (char *)"u_TCGen0",        GLSL_INT },
	{ (char *)"u_TCGen0Vector0", GLSL_VEC3 },
	{ (char *)"u_TCGen0Vector1", GLSL_VEC3 },

	{ (char *)"u_DeformGen",    GLSL_INT },
	{ (char *)"u_DeformParams", GLSL_FLOAT5 },

	{ (char *)"u_ColorGen",  GLSL_INT },
	{ (char *)"u_AlphaGen",  GLSL_INT },
	{ (char *)"u_Color",     GLSL_VEC4 },
	{ (char *)"u_BaseColor", GLSL_VEC4 },
	{ (char *)"u_VertColor", GLSL_VEC4 },

	{ (char *)"u_DlightInfo",    GLSL_VEC4 },
	{ (char *)"u_LightForward",  GLSL_VEC3 },
	{ (char *)"u_LightUp",       GLSL_VEC3 },
	{ (char *)"u_LightRight",    GLSL_VEC3 },
	{ (char *)"u_LightOrigin",   GLSL_VEC4 },
	{ (char *)"u_ModelLightDir", GLSL_VEC3 },
	{ (char *)"u_LightRadius",   GLSL_FLOAT },
	{ (char *)"u_AmbientLight",  GLSL_VEC3 },
	{ (char *)"u_DirectedLight", GLSL_VEC3 },

	{ (char *)"u_PortalRange", GLSL_FLOAT },

	{ (char *)"u_FogDistance",  GLSL_VEC4 },
	{ (char *)"u_FogDepth",     GLSL_VEC4 },
	{ (char *)"u_FogEyeT",      GLSL_FLOAT },
	{ (char *)"u_FogColorMask", GLSL_VEC4 },

	{ (char *)"u_ModelMatrix",               GLSL_MAT16 },
	{ (char *)"u_ModelViewProjectionMatrix", GLSL_MAT16 },

	{ (char *)"u_Time",          GLSL_FLOAT },
	{ (char *)"u_VertexLerp",   GLSL_FLOAT },
	{ (char *)"u_NormalScale",   GLSL_VEC4 },
	{ (char *)"u_SpecularScale", GLSL_VEC4 },

	{ (char *)"u_ViewInfo",        GLSL_VEC4 },
	{ (char *)"u_ViewOrigin",      GLSL_VEC3 },
	{ (char *)"u_LocalViewOrigin", GLSL_VEC3 },
	{ (char *)"u_ViewForward",     GLSL_VEC3 },
	{ (char *)"u_ViewLeft",        GLSL_VEC3 },
	{ (char *)"u_ViewUp",          GLSL_VEC3 },

	{ (char *)"u_InvTexRes",           GLSL_VEC2 },
	{ (char *)"u_AutoExposureMinMax",  GLSL_VEC2 },
	{ (char *)"u_ToneMinAvgMaxLinear", GLSL_VEC3 },

	{ (char *)"u_PrimaryLightOrigin",  GLSL_VEC4  },
	{ (char *)"u_PrimaryLightColor",   GLSL_VEC3  },
	{ (char *)"u_PrimaryLightAmbient", GLSL_VEC3  },
	{ (char *)"u_PrimaryLightRadius",  GLSL_FLOAT },

	{ (char *)"u_CubeMapInfo", GLSL_VEC4 },

	{ (char *)"u_AlphaTest", GLSL_INT },

	{ (char *)"u_BoneMatrix", GLSL_MAT16_BONEMATRIX },

	{ (char *)"u_FireRiseDir", GLSL_VEC3 },
	{ (char *)"u_ZFadeLowest", GLSL_FLOAT },
	{ (char *)"u_ZFadeHighest", GLSL_FLOAT },
	{ (char *)"u_Greyscale", GLSL_FLOAT }
};

static void GLSL_GetShaderHeader( GLenum shaderType, const GLchar *extra, char *dest, int size ) {
	float fbufWidthScale, fbufHeightScale;

	dest[0] = '\0';

	// PARITY-SEAM(Metal-device): GLSL #version / #extension / precision header strings are GLSL-specific.
	// Under Metal the equivalent is MTL source preamble managed by Vasquez shader-translation layer.
	// CPU control-flow (glRefConfig.glslMajorVersion comparisons, qglesMajorVersion, shaderType branch)
	// is kept REAL so permutation logic stays structurally correct for future Metal bring-up.
	if ( glRefConfig.glslMajorVersion > 1 || ( glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 30 ) ) {
		if ( qglesMajorVersion >= 3 && glRefConfig.glslMajorVersion >= 3 ) {
			Q_strcat( dest, size, "#version 300 es\n" );
		} else if ( glRefConfig.glslMajorVersion > 1 || ( glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 50 ) ) {
			Q_strcat( dest, size, "#version 150\n" );
		} else {
			Q_strcat( dest, size, "#version 130\n" );
		}

		if ( extra ) {
			Q_strcat( dest, size, extra );
		}

		if ( qglesMajorVersion >= 2 ) {
			Q_strcat( dest, size, "precision mediump float;\n" );
			Q_strcat( dest, size, "precision mediump sampler2DShadow;\n" );
		}

		if ( shaderType == GL_VERTEX_SHADER ) {
			Q_strcat( dest, size, "#define attribute in\n" );
			Q_strcat( dest, size, "#define varying out\n" );
		} else
		{
			Q_strcat( dest, size, "#define varying in\n" );

			Q_strcat( dest, size, "out vec4 out_Color;\n" );
			Q_strcat( dest, size, "#define gl_FragColor out_Color\n" );
			Q_strcat( dest, size, "#define texture2D texture\n" );
			Q_strcat( dest, size, "#define textureCubeLod textureLod\n" );
			Q_strcat( dest, size, "#define shadow2D texture\n" );
		}
	} else
	{
		if ( qglesMajorVersion >= 2 ) {
			Q_strcat( dest, size, "#version 100\n" );

			if ( extra ) {
				Q_strcat( dest, size, extra );
			}

			Q_strcat( dest, size, "precision mediump float;\n" );

			if ( glRefConfig.shadowSamplers ) {
				Q_strcat( dest, size, "precision mediump sampler2DShadow;\n" );
				Q_strcat( dest, size, "#define shadow2D(a,b) shadow2DEXT(a,b)\n" );
			}
		} else
		{
			Q_strcat( dest, size, "#version 120\n" );

			if ( extra ) {
				Q_strcat( dest, size, extra );
			}

			Q_strcat( dest, size, "#define shadow2D(a,b) shadow2D(a,b).r\n" );
		}
	}

	Q_strcat( dest, size, "#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n" );

	Q_strcat( dest, size,
			  va( "#ifndef deformGen_t\n"
				  "#define deformGen_t\n"
				  "#define DGEN_WAVE_SIN %i\n"
				  "#define DGEN_WAVE_SQUARE %i\n"
				  "#define DGEN_WAVE_TRIANGLE %i\n"
				  "#define DGEN_WAVE_SAWTOOTH %i\n"
				  "#define DGEN_WAVE_INVERSE_SAWTOOTH %i\n"
				  "#define DGEN_BULGE %i\n"
				  "#define DGEN_MOVE %i\n"
				  "#endif\n",
				  DGEN_WAVE_SIN,
				  DGEN_WAVE_SQUARE,
				  DGEN_WAVE_TRIANGLE,
				  DGEN_WAVE_SAWTOOTH,
				  DGEN_WAVE_INVERSE_SAWTOOTH,
				  DGEN_BULGE,
				  DGEN_MOVE ) );

	Q_strcat( dest, size,
			  va( "#ifndef tcGen_t\n"
				  "#define tcGen_t\n"
				  "#define TCGEN_LIGHTMAP %i\n"
				  "#define TCGEN_TEXTURE %i\n"
				  "#define TCGEN_ENVIRONMENT_MAPPED %i\n"
				  "#define TCGEN_FOG %i\n"
				  "#define TCGEN_VECTOR %i\n"
				  "#endif\n",
				  TCGEN_LIGHTMAP,
				  TCGEN_TEXTURE,
				  TCGEN_ENVIRONMENT_MAPPED,
				  TCGEN_FOG,
				  TCGEN_VECTOR ) );

	Q_strcat( dest, size,
			  va( "#ifndef colorGen_t\n"
				  "#define colorGen_t\n"
				  "#define CGEN_LIGHTING_DIFFUSE %i\n"
				  "#endif\n",
				  CGEN_LIGHTING_DIFFUSE ) );

	Q_strcat( dest, size,
			  va( "#ifndef alphaGen_t\n"
				  "#define alphaGen_t\n"
				  "#define AGEN_LIGHTING_SPECULAR %i\n"
				  "#define AGEN_PORTAL %i\n"
				  "#define AGEN_NORMALZFADE %i\n"
				  "#endif\n",
				  AGEN_LIGHTING_SPECULAR,
				  AGEN_PORTAL,
				  AGEN_NORMALZFADE ) );

	fbufWidthScale = 1.0f / ( (float)glConfig.vidWidth );
	fbufHeightScale = 1.0f / ( (float)glConfig.vidHeight );
	Q_strcat( dest, size,
			  va( "#ifndef r_FBufScale\n#define r_FBufScale vec2(%f, %f)\n#endif\n", fbufWidthScale, fbufHeightScale ) );

	if ( r_pbr->integer ) {
		Q_strcat( dest, size, "#define USE_PBR\n" );
	}

	if ( r_cubeMapping->integer ) {
		int cubeMipSize = r_cubemapSize->integer;
		int numRoughnessMips = 0;

		while ( cubeMipSize )
		{
			cubeMipSize >>= 1;
			numRoughnessMips++;
		}
		numRoughnessMips = MAX( 1, numRoughnessMips - 2 );
		Q_strcat( dest, size, va( "#define ROUGHNESS_MIPS float(%d)\n", numRoughnessMips ) );
	}

	Q_strcat( dest, size, "#line 0\n" );
}

static int GLSL_CompileGPUShader( GLuint program, GLuint *prevShader, const GLchar *buffer, int size, GLenum shaderType ) {
	// PARITY-SEAM(Metal-device): qglCreateShader / qglShaderSource / qglCompileShader /
	// qglGetShaderiv(GL_COMPILE_STATUS) / qglDetachShader / qglDeleteShader / qglAttachShader →
	// MTLLibrary newLibraryWithSource:options:error: + MTLRenderPipelineDescriptor vertex/fragmentFunction.
	// Entire function body is seamed; returns 1 (success) as placeholder so CPU permutation
	// loops in GLSL_InitGPUShaders continue to execute.
	(void)program; (void)prevShader; (void)buffer; (void)size; (void)shaderType;
	return 1;
}

static int GLSL_LoadGPUShaderText( const char *name, const char *fallback,
								   GLenum shaderType, char *dest, int destSize ) {
	char filename[MAX_QPATH];
	GLchar      *buffer = NULL;
	const GLchar *shaderText = NULL;
	int size;
	int result;

	if ( shaderType == GL_VERTEX_SHADER ) {
		Com_sprintf( filename, sizeof( filename ), "glsl/%s_vp.glsl", name );
	} else
	{
		Com_sprintf( filename, sizeof( filename ), "glsl/%s_fp.glsl", name );
	}

	if ( r_externalGLSL->integer ) {
		size = ri.FS_ReadFile( filename, (void **)&buffer );
	} else {
		size = 0;
		buffer = NULL;
	}

	if ( !buffer ) {
		if ( fallback ) {
			ri.Printf( PRINT_DEVELOPER, "...loading built-in '%s'\n", filename );
			shaderText = fallback;
			size = (int)strlen( shaderText );
		} else
		{
			// PHASE5-M5.1: fallback shaders are NULL (no GLSL stringification in Metal
			// build). Provide empty placeholder so GLSL_InitGPUShaders succeeds —
			// actual MSL shaders are wired in M5.2+.
			ri.Printf( PRINT_DEVELOPER, "...using placeholder for '%s' (Metal)\n", filename );
			shaderText = "// placeholder";
			size = (int)strlen( shaderText );
		}
	} else
	{
		ri.Printf( PRINT_DEVELOPER, "...loading '%s'\n", filename );
		shaderText = buffer;
	}

	if ( size > destSize ) {
		result = 0;
	} else
	{
		Q_strncpyz( dest, shaderText, size + 1 );
		result = 1;
	}

	if ( buffer ) {
		ri.FS_FreeFile( buffer );
	}

	return result;
}

static void GLSL_LinkProgram( GLuint program ) {
	// PARITY-SEAM(Metal-device): qglLinkProgram / qglGetProgramiv(GL_LINK_STATUS) →
	// MTLDevice newRenderPipelineStateWithDescriptor:error: builds the linked pipeline.
	(void)program;
}

static void GLSL_ShowProgramUniforms( GLuint program ) {
	// PARITY-SEAM(Metal-device): qglGetProgramiv(GL_ACTIVE_UNIFORMS) / qglGetActiveUniform →
	// Metal uses argument buffers / function constants; uniform reflection is not a runtime GL query.
	// Under Metal this debug utility is a documented no-op; names are visible in shader source.
	(void)program;
}

// PHASE5-M5.2: Build a Metal vertex descriptor for the given attrib flags.
// Each active attribute gets its own vertex buffer slot (non-interleaved layout
// matching the separate tess.xyz/texCoords/normal/color CPU arrays).
// Slot assignment (avoids slot 1 which is reserved for vertex uniforms):
//   ATTR_INDEX_POSITION  (0) → Metal buffer slot 2  (vec4_t = float4, stride 16)
//   ATTR_INDEX_TEXCOORD  (1) → Metal buffer slot 3  (vec2_t = float2, stride 8)
//   ATTR_INDEX_LIGHTCOORD (2) → Metal buffer slot 6  (vec2_t = float2, stride 8)
//   ATTR_INDEX_TANGENT   (3) → Metal buffer slot 7  (int16_t[4] = short4n, stride 8)
//   ATTR_INDEX_NORMAL    (4) → Metal buffer slot 4  (int16_t[4] = short4n, stride 8)
//   ATTR_INDEX_COLOR     (5) → Metal buffer slot 5  (uint16_t[4] = ushort4n, stride 8)
// GL2 ref: tr_glsl.c:570-616 (qglBindAttribLocation calls)
static MTL::VertexDescriptor *GLSL_BuildMetalVertexDescriptor( uint32_t attribs ) {
	MTL::VertexDescriptor *vtxDesc = MTL::VertexDescriptor::alloc()->init();
	if ( !vtxDesc ) {
		return NULL;
	}

	if ( attribs & ATTR_POSITION ) {
		auto *attr = vtxDesc->attributes()->object( ATTR_INDEX_POSITION );
		attr->setFormat( MTL::VertexFormatFloat4 ); // vec4_t xyz
		attr->setOffset( 0 );
		attr->setBufferIndex( 2 );
		auto *layout = vtxDesc->layouts()->object( 2 );
		layout->setStride( sizeof( float ) * 4 );
		layout->setStepFunction( MTL::VertexStepFunctionPerVertex );
		layout->setStepRate( 1 );
	}

	if ( attribs & ATTR_TEXCOORD ) {
		auto *attr = vtxDesc->attributes()->object( ATTR_INDEX_TEXCOORD );
		attr->setFormat( MTL::VertexFormatFloat2 ); // vec2_t texCoords
		attr->setOffset( 0 );
		attr->setBufferIndex( 3 );
		auto *layout = vtxDesc->layouts()->object( 3 );
		layout->setStride( sizeof( float ) * 2 );
		layout->setStepFunction( MTL::VertexStepFunctionPerVertex );
		layout->setStepRate( 1 );
	}

	if ( attribs & ATTR_LIGHTCOORD ) {
		auto *attr = vtxDesc->attributes()->object( ATTR_INDEX_LIGHTCOORD );
		attr->setFormat( MTL::VertexFormatFloat2 ); // vec2_t lightCoords
		attr->setOffset( 0 );
		attr->setBufferIndex( 6 );
		auto *layout = vtxDesc->layouts()->object( 6 );
		layout->setStride( sizeof( float ) * 2 );
		layout->setStepFunction( MTL::VertexStepFunctionPerVertex );
		layout->setStepRate( 1 );
	}

	if ( attribs & ATTR_NORMAL ) {
		auto *attr = vtxDesc->attributes()->object( ATTR_INDEX_NORMAL );
		attr->setFormat( MTL::VertexFormatShort4Normalized ); // int16_t[4] normal
		attr->setOffset( 0 );
		attr->setBufferIndex( 4 );
		auto *layout = vtxDesc->layouts()->object( 4 );
		layout->setStride( sizeof( int16_t ) * 4 );
		layout->setStepFunction( MTL::VertexStepFunctionPerVertex );
		layout->setStepRate( 1 );
	}

	if ( attribs & ATTR_COLOR ) {
		auto *attr = vtxDesc->attributes()->object( ATTR_INDEX_COLOR );
		attr->setFormat( MTL::VertexFormatUShort4Normalized ); // uint16_t[4] color
		attr->setOffset( 0 );
		attr->setBufferIndex( 5 );
		auto *layout = vtxDesc->layouts()->object( 5 );
		layout->setStride( sizeof( uint16_t ) * 4 );
		layout->setStepFunction( MTL::VertexStepFunctionPerVertex );
		layout->setStepRate( 1 );
	}

	if ( attribs & ATTR_TANGENT ) {
		// GL2 ref: tr_glsl.c:588 (ATTR_INDEX_TANGENT qglBindAttribLocation)
		// int16_t[4] tangent packed as short4n — same format as normal.
		// Metal buffer slot 7 (avoids slots 2-6 used by other attribs).
		auto *attr = vtxDesc->attributes()->object( ATTR_INDEX_TANGENT );
		attr->setFormat( MTL::VertexFormatShort4Normalized ); // int16_t[4] tangent
		attr->setOffset( 0 );
		attr->setBufferIndex( 7 );
		auto *layout = vtxDesc->layouts()->object( 7 );
		layout->setStride( sizeof( int16_t ) * 4 );
		layout->setStepFunction( MTL::VertexStepFunctionPerVertex );
		layout->setStepRate( 1 );
	}

	if ( attribs & ATTR_LIGHTDIRECTION ) {
		// GL2 ref: tr_vbo.c:223-247 (ATTR_INDEX_LIGHTDIRECTION, count 4, GL_SHORT, normalized)
		// int16_t[4] lightdir packed as short4n — same format as normal/tangent.
		// Metal buffer slot 8 (slots 2-7 used by position/texcoord/normal/color/lightcoord/tangent).
		auto *attr = vtxDesc->attributes()->object( ATTR_INDEX_LIGHTDIRECTION );
		attr->setFormat( MTL::VertexFormatShort4Normalized ); // int16_t[4] lightdir
		attr->setOffset( 0 );
		attr->setBufferIndex( 8 );
		auto *layout = vtxDesc->layouts()->object( 8 );
		layout->setStride( sizeof( int16_t ) * 4 );
		layout->setStepFunction( MTL::VertexStepFunctionPerVertex );
		layout->setStepRate( 1 );
	}

	// Vertex animation (USE_VERTEX_ANIMATION): the old frame of position / normal / tangent,
	// same formats as the current frame, in buffer slots 9 / 10 / 11.
	// GL2 ref: tr_glsl.c:603-611 (attr_Position2 / attr_Normal2 / attr_Tangent2)
	static const struct { uint32_t bit; int attrIndex; int slot; bool position; } kOldFrame[] = {
		{ ATTR_POSITION2, ATTR_INDEX_POSITION2,  9, true  },
		{ ATTR_NORMAL2,   ATTR_INDEX_NORMAL2,   10, false },
		{ ATTR_TANGENT2,  ATTR_INDEX_TANGENT2,  11, false },
	};
	for (const auto &old : kOldFrame)
	{
		if ( !( attribs & old.bit ) ) {
			continue;
		}
		auto *attr = vtxDesc->attributes()->object( old.attrIndex );
		attr->setFormat( old.position ? MTL::VertexFormatFloat4 : MTL::VertexFormatShort4Normalized );
		attr->setOffset( 0 );
		attr->setBufferIndex( old.slot );
		auto *layout = vtxDesc->layouts()->object( old.slot );
		layout->setStride( old.position ? sizeof( float ) * 4 : sizeof( int16_t ) * 4 );
		layout->setStepFunction( MTL::VertexStepFunctionPerVertex );
		layout->setStepRate( 1 );
	}

	return vtxDesc;
}

// FNV-1a 32-bit hash — used to produce per-permutation program keys.
// Identical tokens are used here and in GLSL_InitGPUShaders' extradefines strings,
// so the hash is stable across runs as long as define order is deterministic.
static unsigned int GLSL_FNV1a32( const char *s ) {
	unsigned int h = 2166136261u;
	while (*s )
	{
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

// PARITY-SEAM(Metal-device): GLSL #extension/#version directives are GLSL-only and invalid in MSL;
// stripped here so GL2's extradefines logic (which mirrors tr_glsl.c verbatim) stays unchanged.
static void GLSL_StripGLSLOnlyDirectives( const char *in, char *out, size_t outSize ) {
	const char *src = in;
	char *dst = out;
	const char *dstEnd = out + outSize - 1;

	while (*src && dst < dstEnd )
	{
		const char *lineStart = src;
		while (*src && *src != '\n' )
			src++;
		bool hadNewline = (*src == '\n' );
		const char *lineEnd = src;
		if ( hadNewline ) {
			src++;
		}

		// Find first non-whitespace token on this line
		const char *tok = lineStart;
		while ( tok < lineEnd && (*tok == ' ' || *tok == '\t' ) )
			tok++;

		bool isGLSLOnly = false;
		if ( tok < lineEnd && *tok == '#' ) {
			const char *p = tok + 1;
			while ( p < lineEnd && (*p == ' ' || *p == '\t' ) )
				p++;
			size_t rem = (size_t)( lineEnd - p );
			if ( rem >= 9 && strncmp( p, "extension", 9 ) == 0 &&
				 ( rem == 9 || p[9] == ' ' || p[9] == '\t' ) ) {
				isGLSLOnly = true;
			} else if ( rem >= 7 && strncmp( p, "version", 7 ) == 0 &&
						( rem == 7 || p[7] == ' ' || p[7] == '\t' ) ) {
				isGLSLOnly = true;
			}
		}

		if ( !isGLSLOnly ) {
			size_t lineLen = (size_t)( lineEnd - lineStart );
			size_t needed = lineLen + ( hadNewline ? 1 : 0 );
			if ( dst + needed > dstEnd ) {
				break;
			}
			memcpy( dst, lineStart, lineLen );
			dst += lineLen;
			if ( hadNewline ) {
				*dst++ = '\n';
			}
		}
	}
	*dst = '\0';
}

// Uniform block layout expected by each program family's MSL (see R_DrawElements).
static int GLSL_MetalUniformLayoutForName( const char *name ) {
	if ( !strcmp( name, "texturecolor" ) ) {
		return METAL_UNIFORMS_TEXTURECOLOR;
	}
	if ( !strncmp( name, "lightall", 8 ) ) {
		return METAL_UNIFORMS_LIGHTALL;
	}
	if ( !strncmp( name, "fogpass", 7 ) ) {
		return METAL_UNIFORMS_FOGPASS;
	}
	if ( !strncmp( name, "dlight", 6 ) ) {
		return METAL_UNIFORMS_DLIGHT;
	}
	if ( !strncmp( name, "pshadow", 7 ) ) {
		return METAL_UNIFORMS_PSHADOW;
	}
	if ( !strncmp( name, "shadowfill", 10 ) ) {
		return METAL_UNIFORMS_SHADOWFILL;
	}
	return METAL_UNIFORMS_DRAW;
}

// Shader permutations of the current R_Init, keyed by permutation key plus vertex layout.
// Different GL2 permutation indices can produce identical extradefines (same progKey);
// those share one entry, so each distinct MSL source is compiled once.
// GLSL_InitGPUShader2 only issues the library compiles (asynchronously, so all
// permutations build in parallel on Metal's compiler threads);
// GLSL_FinishPendingMetalPrograms then waits for them and does function lookup,
// registration, pipeline creation and prewarm on the render thread.
// Cleared in GLSL_InitGPUShaders / GLSL_ShutdownGPUShaders.
struct MetalPendingProgram {
	char name[MAX_QPATH];
	char progKey[MAX_QPATH];
	int attribs;
	MetalShaderCompileJob *job;
	std::vector<shaderProgram_t *> programs;
};
static std::vector<MetalPendingProgram> s_pendingPrograms;
static std::map<std::pair<std::string, uint64_t>, size_t> s_pendingProgramIndex;

// Second phase of a permutation's Metal setup: wait for its library compile, then do
// function lookup, registration, pipeline creation and prewarm. Runs on the render thread.
// Returns qfalse when the MSL failed to compile or lacks its entry points.
static qboolean GLSL_FinishPendingMetalProgram( MetalPendingProgram &pending ) {
	const char *name = pending.name;
	const char *progKey = pending.progKey;
	const int attribs = pending.attribs;
	const int uniformLayout = GLSL_MetalUniformLayoutForName( name );

	MTL::Library *vertLib = NULL;
	MTL::Library *fragLib = NULL;
	const bool compiled = GLMetal_FinishShaderLibraryCompile( pending.job, progKey, &vertLib, &fragLib );
	pending.job = NULL;
	if ( !compiled ) {
		ri.Printf( PRINT_ALL, "[Metal] GLSL_FinishPendingMetalPrograms: failed to compile MSL for '%s'\n", progKey );
		return qfalse;
	}

	// Entry-point lookup uses the BASE name (e.g. "generic_vert") — each library
	// exposes these names regardless of which permutation was compiled.
	// Vasquez contract: entry points are always "<baseName>_vert"/"<baseName>_frag".
	char entryVert[MAX_QPATH], entryFrag[MAX_QPATH];
	Com_sprintf( entryVert, sizeof( entryVert ), "%s_vert", name );
	Com_sprintf( entryFrag, sizeof( entryFrag ), "%s_frag", name );

	NS::String *nsVert = NS::String::string( entryVert, NS::StringEncoding::UTF8StringEncoding );
	MTL::Function *vertFn = vertLib->newFunction( nsVert );
	if ( !vertFn ) {
		ri.Printf( PRINT_ALL, "[Metal] GLSL_FinishPendingMetalPrograms: entry '%s' not found in library\n", entryVert );
		if ( fragLib ) {
			fragLib->release();
		}
		vertLib->release();
		return qfalse;
	}

	MTL::Function *fragFn = NULL;
	if ( fragLib ) {
		NS::String *nsFrag = NS::String::string( entryFrag, NS::StringEncoding::UTF8StringEncoding );
		fragFn = fragLib->newFunction( nsFrag );
		if ( !fragFn ) {
			ri.Printf( PRINT_ALL, "[Metal] GLSL_FinishPendingMetalPrograms: entry '%s' not found in library\n", entryFrag );
			vertFn->release();
			fragLib->release();
			vertLib->release();
			return qfalse;
		}
	}

	// Build a vertex descriptor for the enabled attribute set.
	// Uses fixed separate-buffer-slot layout: slot 2=pos, 3=tc, 4=norm, 5=color.
	MTL::VertexDescriptor *vtxDesc = GLSL_BuildMetalVertexDescriptor( (uint32_t)attribs );

	uint64_t vtxDescHash = GLMetal_VertexDescHashForAttribs( (uint32_t)attribs );

	// Register program-fns under progKey; the returned id is the draw-time PSO handle.
	const int programId = GLMetal_RegisterProgramPipeline( progKey, vertFn, fragFn, vtxDesc, vtxDescHash );

	// PSOs must match the attachment formats of the pass they draw into. Almost all
	// program draws target the scene FBO (tr.renderFbo: tr.renderImage + tr.renderDepthImage);
	// shadowfill draws into depth-only shadow maps. Building against whatever target
	// happens to be bound at init (the drawable, no depth yet) produced PSOs that no draw
	// could use, so build/prewarm explicitly for the real target formats.
	MTL::PixelFormat colorFormat = GLMetal_ColorPixelFormat();
	MTL::PixelFormat depthFormat = MTL::PixelFormatInvalid;
	if ( uniformLayout == METAL_UNIFORMS_SHADOWFILL ) {
		colorFormat = MTL::PixelFormatInvalid;
		depthFormat = MTL::PixelFormatDepth32Float;
	} else if ( tr.renderImage && tr.renderDepthImage )     {
		MTL::Texture *colorTex = GLMetal_GetTexture( (uint32_t)tr.renderImage->texnum );
		MTL::Texture *depthTex = GLMetal_GetTexture( (uint32_t)tr.renderDepthImage->texnum );
		if ( colorTex ) {
			colorFormat = colorTex->pixelFormat();
		}
		if ( depthTex ) {
			depthFormat = depthTex->pixelFormat();
		}
	}

	// GLS_DEFAULT (no blend) pipeline built synchronously: it validates the permutation
	// and gates draws (metalPipeline != NULL).
	MTL::RenderPipelineState *pso = GLMetal_GetOrCreatePipelineWithFormats(
		progKey, vertFn, fragFn, vtxDesc, vtxDescHash, 0 /*GLS_DEFAULT*/,
		MTL::ColorWriteMaskAll, colorFormat, depthFormat );

	if ( pso ) {
		ri.Printf( PRINT_DEVELOPER, "[Metal] pipeline ready for '%s'\n", progKey );
	} else
	{
		ri.Printf( PRINT_ALL, "[Metal] GLSL_FinishPendingMetalPrograms: pipeline creation failed for '%s'\n", progKey );
		// Non-fatal: leave metalPipeline=NULL, draw calls will skip.
	}

	// Pre-warm the common blend permutations asynchronously so first use of additive /
	// alpha / multiplicative stages does not hitch, without blocking startup on the
	// compiles. Only blend bits affect a PSO (depth/alpha-test bits are masked out of the
	// key), so these are the distinct blend modes Q3 shaders use most.
	// A draw that needs one before its compile finishes builds it synchronously.
	if ( pso && uniformLayout != METAL_UNIFORMS_SHADOWFILL ) {
		static const uint32_t kPrewarmStateBits[] = {
			GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE,                                 // additive (teleporters, jump-pads)
			GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA,          // alpha blend (particles, HUD)
			GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA,                // premultiplied alpha
			GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO,                          // lightmap/filter multiply
			GLS_SRCBLEND_ZERO | GLS_DSTBLEND_SRC_COLOR,                          // filter multiply (alt form)
			GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE,                           // dlight add
		};
		GLMetal_PrewarmProgramPipelines( programId, kPrewarmStateBits,
										 (int)( sizeof( kPrewarmStateBits ) / sizeof( kPrewarmStateBits[0] ) ), colorFormat, depthFormat );
	}

	for (shaderProgram_t *program : pending.programs)
	{
		program->metalProgramId = programId;
		program->metalPipeline  = (void *)pso; // NULL gates draws off
	}

	if ( vtxDesc ) {
		vtxDesc->release();
	}
	if ( fragFn ) {
		fragFn->release();
	}
	vertFn->release();
	if ( fragLib ) {
		fragLib->release();
	}
	vertLib->release();
	return qtrue;
}

// Collect every compile issued by GLSL_InitGPUShader2 during this R_Init.
// A permutation whose MSL does not compile is fatal, as it was when compiles were
// synchronous (GLSL_InitGPUShader returned 0 -> "Could not load <name> shader!").
static void GLSL_FinishPendingMetalPrograms( void ) {
	char failedName[MAX_QPATH];
	failedName[0] = '\0';

	// Finish every job even after a failure so no compile is left running.
	for (MetalPendingProgram &pending : s_pendingPrograms)
	{
		if ( !GLSL_FinishPendingMetalProgram( pending ) && !failedName[0] ) {
			Q_strncpyz( failedName, pending.name, sizeof( failedName ) );
		}
	}
	s_pendingPrograms.clear();
	s_pendingProgramIndex.clear();

	if ( failedName[0] ) {
		ri.Error( ERR_FATAL, "Could not load %s shader!", failedName );
	}
}

// Metal extension: accepts two extra parameters not present in GL2:
//   extra    — the GL2 extradefines string (e.g. "#define USE_TCGEN\n#define USE_TCMOD\n").
//              Prepended VERBATIM to the MSL source so the Metal C-preprocessor sees the
//              same #define tokens that GL2's GLSL driver saw.  Valid MSL preprocessor
//              directives — no translation needed.
//   progKey  — permutation-qualified cache key (format: "<name>#<8-hex-digits>").
//              Passed to GLMetal_RegisterProgramPipeline / GLMetal_GetOrCreatePipeline so
//              the PSO and program-fns caches discriminate between permutations.
//              program->name retains the bare base name for GL2 parity; progKey lives in
//              program->metalProgramKey (Metal-only field).
static int GLSL_InitGPUShader2( shaderProgram_t * program, const char *name,
								int attribs, const char *vpCode, const char *fpCode,
								const char *extra, const char *progKey ) {
	ri.Printf( PRINT_DEVELOPER, "------- GPU shader -------\n" );

	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Error( ERR_DROP, "GLSL_InitGPUShader2: \"%s\" is too long", name );
	}

	Q_strncpyz( program->name, name, sizeof( program->name ) );
	Q_strncpyz( program->metalProgramKey, progKey, sizeof( program->metalProgramKey ) );

	// PARITY-SEAM(Metal-device): qglCreateProgram → MTLRenderPipelineDescriptor allocation.
	program->program = 0;
	program->attribs = attribs;
	program->metalPipeline = NULL; // default: no Metal pipeline (PHASE5)
	program->metalProgramId = -1;
	program->metalUniformLayout = GLSL_MetalUniformLayoutForName( name );

	// ── PHASE5-M5.2: Metal pipeline path ────────────────────────────────────
	// Check if MSL source is available for this program name.
	// If yes: prepend extradefines, compile libraries, create the Metal pipeline.
	// If no:  log non-fatal, leave metalPipeline=NULL, and continue — returning
	//         success anyway (see PHASE5-DEVIATION comment in GLSL_InitGPUShader).
	// GL2 ref: tr_glsl.c:556-616 (compile + link path)
	const char *mslVPBase = GLMetal_GetShaderSource( name, qfalse );
	const char *mslFPBase = fpCode ? GLMetal_GetShaderSource( name, qtrue ) : NULL;

	const std::pair<std::string, uint64_t> pendingKey( progKey, GLMetal_VertexDescHashForAttribs( (uint32_t)attribs ) );
	const auto pendingIt = mslVPBase ? s_pendingProgramIndex.find( pendingKey ) : s_pendingProgramIndex.end();
	if ( pendingIt != s_pendingProgramIndex.end() ) {
		// Same MSL source and vertex layout as an earlier permutation: share its compile,
		// program and pipeline.
		s_pendingPrograms[pendingIt->second].programs.push_back( program );
		ri.Printf( PRINT_DEVELOPER, "[Metal] GLSL_InitGPUShader2: reusing permutation '%s'\n", progKey );
	} else if ( mslVPBase )     {
		// Prepend extradefines (the GL2 extra string) to each MSL source so the
		// Metal C-preprocessor activates the same #ifdef USE_* blocks that the GL
		// GLSL driver activated.  MSL buffer sized to 40000 to accommodate ~32KB
		// shader sources plus up to ~1024 bytes of defines.
		static const int kMSLBufSz = 40000;
		char mslVPBuf[40000];
		char mslFPBuf[40000];

		// PARITY-SEAM(Metal-device): GLSL #extension/#version directives are GLSL-only and invalid in MSL;
		// stripped here so GL2's extradefines logic (which mirrors tr_glsl.c verbatim) stays unchanged.
		char filteredExtra[2048];
		const char *extraForMSL = extra;
		if ( extra && extra[0] ) {
			GLSL_StripGLSLOnlyDirectives( extra, filteredExtra, sizeof( filteredExtra ) );
			extraForMSL = filteredExtra;
		}

		if ( extraForMSL && extraForMSL[0] ) {
			int extraLen = (int)strlen( extraForMSL );
			int vpBaseLen = (int)strlen( mslVPBase );
			if ( extraLen + vpBaseLen + 1 > kMSLBufSz ) {
				ri.Printf( PRINT_ALL, "[Metal] GLSL_InitGPUShader2: MSL vert buffer overflow for '%s'\n", name );
				return 0;
			}
			memcpy( mslVPBuf, extraForMSL, (size_t)extraLen );
			memcpy( mslVPBuf + extraLen, mslVPBase, (size_t)vpBaseLen + 1 );
		} else
		{
			Q_strncpyz( mslVPBuf, mslVPBase, kMSLBufSz );
		}

		const char *mslVP = mslVPBuf;

		const char *mslFP = NULL;
		if ( mslFPBase ) {
			if ( extraForMSL && extraForMSL[0] ) {
				int extraLen = (int)strlen( extraForMSL );
				int fpBaseLen = (int)strlen( mslFPBase );
				if ( extraLen + fpBaseLen + 1 > kMSLBufSz ) {
					ri.Printf( PRINT_ALL, "[Metal] GLSL_InitGPUShader2: MSL frag buffer overflow for '%s'\n", name );
					return 0;
				}
				memcpy( mslFPBuf, extraForMSL, (size_t)extraLen );
				memcpy( mslFPBuf + extraLen, mslFPBase, (size_t)fpBaseLen + 1 );
			} else
			{
				Q_strncpyz( mslFPBuf, mslFPBase, kMSLBufSz );
			}
			mslFP = mslFPBuf;
		}

		ri.Printf( PRINT_DEVELOPER, "[Metal] GLSL_InitGPUShader2: compiling permutation '%s'\n", progKey );

		// Issue the compile of a UNIQUE MTL::Library pair per permutation (libraries are
		// never shared across permutations so #ifdef blocks resolve independently) and
		// carry on; GLSL_FinishPendingMetalPrograms collects the result.
		MetalPendingProgram pending;
		Q_strncpyz( pending.name, name, sizeof( pending.name ) );
		Q_strncpyz( pending.progKey, progKey, sizeof( pending.progKey ) );
		pending.attribs = attribs;
		pending.job = GLMetal_BeginShaderLibraryCompile( mslVP, mslFP );
		if ( !pending.job ) {
			ri.Printf( PRINT_ALL, "[Metal] GLSL_InitGPUShader2: failed to start MSL compile for '%s'\n", progKey );
			return 0;
		}
		pending.programs.push_back( program );
		s_pendingProgramIndex[pendingKey] = s_pendingPrograms.size();
		s_pendingPrograms.push_back( pending );
	}
	// If mslVPBase == NULL the shader has no MSL yet; metalPipeline stays NULL.
	// The non-fatal deviation is enforced in GLSL_InitGPUShader.
	// ── end PHASE5-M5.2 Metal pipeline path ─────────────────────────────────

	// GL-side stubs (PARITY-SEAMs) — keep so the CPU permutation bookkeeping
	// (GLSL_InitUniforms, uniform buffer) still runs correctly.
	if ( !( GLSL_CompileGPUShader( program->program, &program->vertexShader, vpCode, (int)strlen( vpCode ), GL_VERTEX_SHADER ) ) ) {
		ri.Printf( PRINT_ALL, "GLSL_InitGPUShader2: Unable to load \"%s\" as GL_VERTEX_SHADER\n", name );
		// PARITY-SEAM(Metal-device): qglDeleteProgram → MTLRenderPipelineState release.
		return 0;
	}

	if ( fpCode ) {
		if ( !( GLSL_CompileGPUShader( program->program, &program->fragmentShader, fpCode, (int)strlen( fpCode ), GL_FRAGMENT_SHADER ) ) ) {
			ri.Printf( PRINT_ALL, "GLSL_InitGPUShader2: Unable to load \"%s\" as GL_FRAGMENT_SHADER\n", name );
			// PARITY-SEAM(Metal-device): qglDeleteProgram → MTLRenderPipelineState release.
			return 0;
		}
	}

	// PARITY-SEAM(Metal-device): qglBindAttribLocation → MTLVertexDescriptor attribute layout;
	// attrib-index bindings map to Metal vertex buffer attribute slots in the pipeline descriptor.
	(void)attribs; // attribs stored in program->attribs above; layout applied at pipeline build time.

	GLSL_LinkProgram( program->program );

	return 1;
}

static int GLSL_InitGPUShader( shaderProgram_t * program, const char *name,
							   int attribs, qboolean fragmentShader, const GLchar *extra, qboolean addHeader,
							   const char *fallback_vp, const char *fallback_fp ) {
	char vpCode[32000];
	char fpCode[32000];
	char *postHeader;
	int size;
	int result;

	// PHASE5-DEVIATION: GL2 returns failure→ERR_FATAL when there is no shader
	// source (tr_glsl.c:638-641, tr_glsl.c:656-660).  During Metal bring-up we
	// check for MSL availability FIRST: if no MSL is translated yet, we log a
	// non-fatal notice, initialise the program struct with metalPipeline=NULL, and
	// return SUCCESS.  Draw calls gate on (metalPipeline != NULL) and skip quietly.
	// Restore strict behavior once all MSL is ported (remove this block).
	// GL2 original fatal path: GLSL_InitGPUShader → GLSL_LoadGPUShaderText returns 0
	//   → GLSL_InitGPUShader returns 0 → ERR_FATAL "Could not load <name> shader!"
	if ( GLMetal_GetShaderSource( name, qfalse ) == NULL ) {
		// No MSL for this shader — initialise bookkeeping fields and return success.
		Q_strncpyz( program->name, name, sizeof( program->name ) );
		program->metalProgramKey[0] = '\0'; // no permutation key — inactive program
		program->program  = 0;
		program->attribs  = attribs;
		program->metalPipeline = NULL;
		program->metalProgramId = -1;
		program->metalUniformLayout = METAL_UNIFORMS_DRAW;
		ri.Printf( PRINT_DEVELOPER, "[Metal] shader '%s' has no MSL yet (Phase 5 deferred) — inactive\n", name );
		return 1; // non-fatal
	}

	size = sizeof( vpCode );
	if ( addHeader ) {
		GLSL_GetShaderHeader( GL_VERTEX_SHADER, extra, vpCode, size );
		postHeader = &vpCode[strlen( vpCode )];
		size -= (int)strlen( vpCode );
	} else
	{
		postHeader = &vpCode[0];
	}

	if ( !GLSL_LoadGPUShaderText( name, fallback_vp, GL_VERTEX_SHADER, postHeader, size ) ) {
		return 0;
	}

	if ( fragmentShader ) {
		size = sizeof( fpCode );
		if ( addHeader ) {
			GLSL_GetShaderHeader( GL_FRAGMENT_SHADER, extra, fpCode, size );
			postHeader = &fpCode[strlen( fpCode )];
			size -= (int)strlen( fpCode );
		} else
		{
			postHeader = &fpCode[0];
		}

		if ( !GLSL_LoadGPUShaderText( name, fallback_fp, GL_FRAGMENT_SHADER, postHeader, size ) ) {
			return 0;
		}
	}

	// Compute permutation-qualified Metal program key: "<name>#<8-hex hash of extra>".
	// The hash is computed over the extradefines string so each distinct permutation
	// gets a unique cache key even if the base name is the same.
	// GL2 did not need this — the GL driver sees the compiled GLSL source as distinct
	// program objects.  Metal caches by string key, so we must differentiate here.
	char progKey[MAX_QPATH];
	{
		unsigned int keyhash = GLSL_FNV1a32( extra ? extra : "" );
		Com_sprintf( progKey, sizeof( progKey ), "%s#%08x", name, keyhash );
	}

	result = GLSL_InitGPUShader2( program, name, attribs, vpCode, fragmentShader ? fpCode : NULL,
								  extra, progKey );

	return result;
}

void GLSL_InitUniforms( shaderProgram_t *program ) {
	int i, size;

	GLint *uniforms = program->uniforms;

	size = 0;
	for (i = 0; i < UNIFORM_COUNT; i++)
	{
		// PARITY-SEAM(Metal-device): GL uniform location → Metal arg-buffer binding;
		// using index sentinel so CPU uniform cache stays live.
		uniforms[i] = i;

		program->uniformBufferOffsets[i] = size;

		switch ( uniformsInfo[i].type )
		{
		case GLSL_INT:
			size += sizeof( GLint );
			break;
		case GLSL_FLOAT:
			size += sizeof( GLfloat );
			break;
		case GLSL_FLOAT5:
			size += sizeof( vec_t ) * 5;
			break;
		case GLSL_VEC2:
			size += sizeof( vec_t ) * 2;
			break;
		case GLSL_VEC3:
			size += sizeof( vec_t ) * 3;
			break;
		case GLSL_VEC4:
			size += sizeof( vec_t ) * 4;
			break;
		case GLSL_MAT16:
			size += sizeof( vec_t ) * 16;
			break;
		case GLSL_MAT16_BONEMATRIX:
			size += sizeof( vec_t ) * 16 * glRefConfig.glslMaxAnimatedBones;
			break;
		default:
			break;
		}
	}

	program->uniformBuffer = (char *)ri.Z_Malloc( size );
}

void GLSL_FinishGPUShader( shaderProgram_t *program ) {
	GLSL_ShowProgramUniforms( program->program );
	GL_CheckErrors();
}

void GLSL_SetUniformInt( shaderProgram_t *program, int uniformNum, GLint value ) {
	GLint *uniforms = program->uniforms;
	GLint *compare = (GLint *)( program->uniformBuffer + program->uniformBufferOffsets[uniformNum] );

	if ( uniforms[uniformNum] == -1 ) {
		return;
	}

	if ( uniformsInfo[uniformNum].type != GLSL_INT ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformInt: wrong type for uniform %i in program %s\n", uniformNum, program->name );
		return;
	}

	if ( value == *compare ) {
		return;
	}

	*compare = value;

	// PARITY-SEAM(Metal-device): qglProgramUniform1iEXT → Metal argument-buffer integer slot write;
	// needs MTLDevice / argument encoder (Phase: device bring-up).
}

void GLSL_SetUniformFloat( shaderProgram_t *program, int uniformNum, GLfloat value ) {
	GLint *uniforms = program->uniforms;
	GLfloat *compare = (GLfloat *)( program->uniformBuffer + program->uniformBufferOffsets[uniformNum] );

	if ( uniforms[uniformNum] == -1 ) {
		return;
	}

	if ( uniformsInfo[uniformNum].type != GLSL_FLOAT ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformFloat: wrong type for uniform %i in program %s\n", uniformNum, program->name );
		return;
	}

	if ( value == *compare ) {
		return;
	}

	*compare = value;

	// PARITY-SEAM(Metal-device): qglProgramUniform1fEXT → Metal argument-buffer float slot write;
	// needs MTLDevice / argument encoder (Phase: device bring-up).
}

void GLSL_SetUniformVec2( shaderProgram_t *program, int uniformNum, const vec2_t v ) {
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)( program->uniformBuffer + program->uniformBufferOffsets[uniformNum] );

	if ( uniforms[uniformNum] == -1 ) {
		return;
	}

	if ( uniformsInfo[uniformNum].type != GLSL_VEC2 ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec2: wrong type for uniform %i in program %s\n", uniformNum, program->name );
		return;
	}

	if ( v[0] == compare[0] && v[1] == compare[1] ) {
		return;
	}

	compare[0] = v[0];
	compare[1] = v[1];

	// PARITY-SEAM(Metal-device): qglProgramUniform2fEXT → Metal argument-buffer vec2 slot write;
	// needs MTLDevice / argument encoder (Phase: device bring-up).
}

void GLSL_SetUniformVec3( shaderProgram_t *program, int uniformNum, const vec3_t v ) {
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)( program->uniformBuffer + program->uniformBufferOffsets[uniformNum] );

	if ( uniforms[uniformNum] == -1 ) {
		return;
	}

	if ( uniformsInfo[uniformNum].type != GLSL_VEC3 ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec3: wrong type for uniform %i in program %s\n", uniformNum, program->name );
		return;
	}

	if ( VectorCompare( v, compare ) ) {
		return;
	}

	VectorCopy( v, compare );

	// PARITY-SEAM(Metal-device): qglProgramUniform3fEXT → Metal argument-buffer vec3 slot write;
	// needs MTLDevice / argument encoder (Phase: device bring-up).
}

void GLSL_SetUniformVec4( shaderProgram_t *program, int uniformNum, const vec4_t v ) {
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)( program->uniformBuffer + program->uniformBufferOffsets[uniformNum] );

	if ( uniforms[uniformNum] == -1 ) {
		return;
	}

	if ( uniformsInfo[uniformNum].type != GLSL_VEC4 ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformVec4: wrong type for uniform %i in program %s\n", uniformNum, program->name );
		return;
	}

	if ( VectorCompare4( v, compare ) ) {
		return;
	}

	VectorCopy4( v, compare );

	// PARITY-SEAM(Metal-device): qglProgramUniform4fEXT → Metal argument-buffer vec4 slot write;
	// needs MTLDevice / argument encoder (Phase: device bring-up).
}

void GLSL_SetUniformFloat5( shaderProgram_t *program, int uniformNum, const vec5_t v ) {
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)( program->uniformBuffer + program->uniformBufferOffsets[uniformNum] );

	if ( uniforms[uniformNum] == -1 ) {
		return;
	}

	if ( uniformsInfo[uniformNum].type != GLSL_FLOAT5 ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformFloat5: wrong type for uniform %i in program %s\n", uniformNum, program->name );
		return;
	}

	if ( VectorCompare5( v, compare ) ) {
		return;
	}

	VectorCopy5( v, compare );

	// PARITY-SEAM(Metal-device): qglProgramUniform1fvEXT(5 floats) → Metal argument-buffer
	// float[5] slot write; needs MTLDevice / argument encoder (Phase: device bring-up).
}

void GLSL_SetUniformMat4( shaderProgram_t *program, int uniformNum, const mat4_t matrix ) {
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)( program->uniformBuffer + program->uniformBufferOffsets[uniformNum] );

	if ( uniforms[uniformNum] == -1 ) {
		return;
	}

	if ( uniformsInfo[uniformNum].type != GLSL_MAT16 ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4: wrong type for uniform %i in program %s\n", uniformNum, program->name );
		return;
	}

	if ( Mat4Compare( matrix, compare ) ) {
		return;
	}

	Mat4Copy( matrix, compare );

	// PARITY-SEAM(Metal-device): qglProgramUniformMatrix4fvEXT → Metal argument-buffer
	// mat4 slot write (float4x4); needs MTLDevice / argument encoder (Phase: device bring-up).
}

void GLSL_SetUniformMat4BoneMatrix( shaderProgram_t *program, int uniformNum, /*const*/ mat4_t *matrix, int numMatricies ) {
	GLint *uniforms = program->uniforms;
	vec_t *compare = (float *)( program->uniformBuffer + program->uniformBufferOffsets[uniformNum] );

	if ( uniforms[uniformNum] == -1 ) {
		return;
	}

	if ( uniformsInfo[uniformNum].type != GLSL_MAT16_BONEMATRIX ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4BoneMatrix: wrong type for uniform %i in program %s\n", uniformNum, program->name );
		return;
	}

	if ( numMatricies > glRefConfig.glslMaxAnimatedBones ) {
		ri.Printf( PRINT_WARNING, "GLSL_SetUniformMat4BoneMatrix: too many matricies (%d/%d) for uniform %i in program %s\n",
				   numMatricies, glRefConfig.glslMaxAnimatedBones, uniformNum, program->name );
		return;
	}

	if ( !memcmp( matrix, compare, numMatricies * sizeof( mat4_t ) ) ) {
		return;
	}

	Com_Memcpy( compare, matrix, numMatricies * sizeof( mat4_t ) );

	// PARITY-SEAM(Metal-device): qglProgramUniformMatrix4fvEXT(numMatricies) → Metal argument-buffer
	// bone-matrix array write (float4x4[N]); needs MTLDevice / argument encoder (Phase: device bring-up).
}

void GLSL_DeleteGPUShader( shaderProgram_t *program ) {
	if ( program->program ) {
		// PARITY-SEAM(Metal-device): qglDetachShader / qglDeleteShader / qglDeleteProgram →
		// MTLRenderPipelineState release (ARC/autorelease under metal-cpp).
		(void)program; // GPU object teardown deferred to Metal device bring-up phase.
	}

	if ( program->uniformBuffer ) {
		ri.Free( program->uniformBuffer );
	}

	Com_Memset( program, 0, sizeof(*program ) );
}

void GLSL_InitGPUShaders( void ) {
	int startTime, endTime;
	int i;
	char extradefines[1024];
	int attribs;
	int numGenShaders = 0, numLightShaders = 0, numEtcShaders = 0;

	ri.Printf( PRINT_ALL, "------- GLSL_InitGPUShaders -------\n" );

	s_pendingPrograms.clear();
	s_pendingProgramIndex.clear();

	R_IssuePendingRenderCommands();

	startTime = ri.Milliseconds();

	// PARITY-SEAM(Metal-device): maxVertexAttribs reflects GL capability; under Metal all
	// vertex attributes are always available via MTLVertexDescriptor.  CPU branch kept REAL.
	if ( glRefConfig.maxVertexAttribs > ATTR_INDEX_NORMAL2 ) {
		ri.Printf( PRINT_ALL, "Using GPU vertex animation\n" );
		glRefConfig.gpuVertexAnimation = qtrue;
	} else {
		ri.Printf( PRINT_ALL, "Using CPU vertex animation\n" );
		glRefConfig.gpuVertexAnimation = qfalse;
	}

	for (i = 0; i < GENERICDEF_COUNT; i++)
	{
		if ( ( i & GENERICDEF_USE_VERTEX_ANIMATION ) && ( i & GENERICDEF_USE_BONE_ANIMATION ) ) {
			continue;
		}

		if ( ( i & GENERICDEF_USE_BONE_ANIMATION ) && !glRefConfig.glslMaxAnimatedBones ) {
			continue;
		}

		attribs = ATTR_POSITION | ATTR_TEXCOORD | ATTR_LIGHTCOORD | ATTR_NORMAL | ATTR_COLOR;
		extradefines[0] = '\0';

		if ( i & GENERICDEF_USE_DEFORM_VERTEXES ) {
			Q_strcat( extradefines, 1024, "#define USE_DEFORM_VERTEXES\n" );
		}

		if ( i & GENERICDEF_USE_TCGEN_AND_TCMOD ) {
			Q_strcat( extradefines, 1024, "#define USE_TCGEN\n" );
			Q_strcat( extradefines, 1024, "#define USE_TCMOD\n" );
		}

		if ( i & GENERICDEF_USE_VERTEX_ANIMATION ) {
			if ( !glRefConfig.gpuVertexAnimation ) {
				continue;
			}

			Q_strcat( extradefines, 1024, "#define USE_VERTEX_ANIMATION\n" );
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		} else if ( i & GENERICDEF_USE_BONE_ANIMATION )     {
			Q_strcat( extradefines, 1024, va( "#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones ) );
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		if ( i & GENERICDEF_USE_FOG ) {
			Q_strcat( extradefines, 1024, "#define USE_FOG\n" );
		}

		if ( i & GENERICDEF_USE_RGBAGEN ) {
			Q_strcat( extradefines, 1024, "#define USE_RGBAGEN\n" );
		}

		if ( i & GENERICDEF_USE_WOLF_FOG_LINEAR ) {
			Q_strcat( extradefines, 1024, "#define USE_WOLF_FOG_LINEAR\n" );
		}

		if ( i & GENERICDEF_USE_WOLF_FOG_EXPONENTIAL ) {
			Q_strcat( extradefines, 1024, "#define USE_WOLF_FOG_EXPONENTIAL\n" );
		}

		if ( !GLSL_InitGPUShader( &tr.genericShader[i], "generic", attribs, qtrue, extradefines, qtrue, fallbackShader_generic_vp, fallbackShader_generic_fp ) ) {
			ri.Error( ERR_FATAL, "Could not load generic shader!" );
		}

		GLSL_InitUniforms( &tr.genericShader[i] );

		GLSL_SetUniformInt( &tr.genericShader[i], UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP );
		GLSL_SetUniformInt( &tr.genericShader[i], UNIFORM_LIGHTMAP,   TB_LIGHTMAP );

		GLSL_FinishGPUShader( &tr.genericShader[i] );

		numGenShaders++;
	}


	attribs = ATTR_POSITION | ATTR_TEXCOORD;

	if ( !GLSL_InitGPUShader( &tr.textureColorShader, "texturecolor", attribs, qtrue, extradefines, qtrue, fallbackShader_texturecolor_vp, fallbackShader_texturecolor_fp ) ) {
		ri.Error( ERR_FATAL, "Could not load texturecolor shader!" );
	}

	GLSL_InitUniforms( &tr.textureColorShader );

	GLSL_SetUniformInt( &tr.textureColorShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP );

	GLSL_FinishGPUShader( &tr.textureColorShader );

	numEtcShaders++;

	for (i = 0; i < FOGDEF_COUNT; i++)
	{
		if ( ( i & FOGDEF_USE_VERTEX_ANIMATION ) && ( i & FOGDEF_USE_BONE_ANIMATION ) ) {
			continue;
		}

		if ( ( i & FOGDEF_USE_VERTEX_ANIMATION ) && !glRefConfig.gpuVertexAnimation ) {
			continue;
		}

		if ( ( i & FOGDEF_USE_BONE_ANIMATION ) && !glRefConfig.glslMaxAnimatedBones ) {
			continue;
		}

		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if ( i & FOGDEF_USE_DEFORM_VERTEXES ) {
			Q_strcat( extradefines, 1024, "#define USE_DEFORM_VERTEXES\n" );
		}

		if ( i & FOGDEF_USE_VERTEX_ANIMATION ) {
			Q_strcat( extradefines, 1024, "#define USE_VERTEX_ANIMATION\n" );
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		} else if ( i & FOGDEF_USE_BONE_ANIMATION )     {
			Q_strcat( extradefines, 1024, va( "#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones ) );
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		if ( i & FOGDEF_USE_WOLF_FOG_LINEAR ) {
			Q_strcat( extradefines, 1024, "#define USE_WOLF_FOG_LINEAR\n" );
		}

		if ( i & FOGDEF_USE_WOLF_FOG_EXPONENTIAL ) {
			Q_strcat( extradefines, 1024, "#define USE_WOLF_FOG_EXPONENTIAL\n" );
		}

		if ( !GLSL_InitGPUShader( &tr.fogShader[i], "fogpass", attribs, qtrue, extradefines, qtrue, fallbackShader_fogpass_vp, fallbackShader_fogpass_fp ) ) {
			ri.Error( ERR_FATAL, "Could not load fogpass shader!" );
		}

		GLSL_InitUniforms( &tr.fogShader[i] );
		GLSL_FinishGPUShader( &tr.fogShader[i] );

		numEtcShaders++;
	}


	for (i = 0; i < DLIGHTDEF_COUNT; i++)
	{
		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if ( i & DLIGHTDEF_USE_DEFORM_VERTEXES ) {
			Q_strcat( extradefines, 1024, "#define USE_DEFORM_VERTEXES\n" );
		}

		if ( !GLSL_InitGPUShader( &tr.dlightShader[i], "dlight", attribs, qtrue, extradefines, qtrue, fallbackShader_dlight_vp, fallbackShader_dlight_fp ) ) {
			ri.Error( ERR_FATAL, "Could not load dlight shader!" );
		}

		GLSL_InitUniforms( &tr.dlightShader[i] );

		GLSL_SetUniformInt( &tr.dlightShader[i], UNIFORM_DIFFUSEMAP, TB_DIFFUSEMAP );

		GLSL_FinishGPUShader( &tr.dlightShader[i] );

		numEtcShaders++;
	}


	for (i = 0; i < LIGHTDEF_COUNT; i++)
	{
		int lightType = i & LIGHTDEF_LIGHTTYPE_MASK;
		qboolean fastLight = ( qboolean ) !( r_normalMapping->integer || r_specularMapping->integer );

		if ( ( i & LIGHTDEF_USE_PARALLAXMAP ) && !r_parallaxMapping->integer ) {
			continue;
		}

		if ( ( i & LIGHTDEF_USE_SHADOWMAP ) && ( !lightType || !r_sunlightMode->integer ) ) {
			continue;
		}

		if ( ( i & LIGHTDEF_ENTITY_VERTEX_ANIMATION ) && ( i & LIGHTDEF_ENTITY_BONE_ANIMATION ) ) {
			continue;
		}

		if ( ( i & LIGHTDEF_ENTITY_BONE_ANIMATION ) && !glRefConfig.glslMaxAnimatedBones ) {
			continue;
		}

		attribs = ATTR_POSITION | ATTR_TEXCOORD | ATTR_COLOR | ATTR_NORMAL;
		// Metal: the single shared "lightall" MSL always declares attr_LightCoord [[attribute(2)]].
		// Force ATTR_LIGHTCOORD on every permutation so each PSO's vertex descriptor matches the MSL
		// (otherwise non-lightmap permutations, incl. the sky's lightallShader[0], fail PSO creation).
		attribs |= ATTR_LIGHTCOORD;

		extradefines[0] = '\0';

		if ( r_dlightMode->integer >= 2 ) {
			Q_strcat( extradefines, 1024, "#define USE_SHADOWMAP\n" );
		}

		if ( glRefConfig.swizzleNormalmap ) {
			Q_strcat( extradefines, 1024, "#define SWIZZLE_NORMALMAP\n" );
		}

		if ( lightType ) {
			Q_strcat( extradefines, 1024, "#define USE_LIGHT\n" );

			if ( fastLight ) {
				Q_strcat( extradefines, 1024, "#define USE_FAST_LIGHT\n" );
			}

			switch ( lightType )
			{
			case LIGHTDEF_USE_LIGHTMAP:
				Q_strcat( extradefines, 1024, "#define USE_LIGHTMAP\n" );
				if ( r_deluxeMapping->integer && !fastLight ) {
					Q_strcat( extradefines, 1024, "#define USE_DELUXEMAP\n" );
				}
				attribs |= ATTR_LIGHTCOORD | ATTR_LIGHTDIRECTION;
				break;
			case LIGHTDEF_USE_LIGHT_VECTOR:
				Q_strcat( extradefines, 1024, "#define USE_LIGHT_VECTOR\n" );
				break;
			case LIGHTDEF_USE_LIGHT_VERTEX:
				Q_strcat( extradefines, 1024, "#define USE_LIGHT_VERTEX\n" );
				attribs |= ATTR_LIGHTDIRECTION;
				break;
			default:
				break;
			}

			if ( r_normalMapping->integer ) {
				Q_strcat( extradefines, 1024, "#define USE_NORMALMAP\n" );

				attribs |= ATTR_TANGENT;

				if ( ( i & LIGHTDEF_USE_PARALLAXMAP ) && !( i & LIGHTDEF_ENTITY_VERTEX_ANIMATION ) && !( i & LIGHTDEF_ENTITY_BONE_ANIMATION ) && r_parallaxMapping->integer ) {
					Q_strcat( extradefines, 1024, "#define USE_PARALLAXMAP\n" );
					if ( r_parallaxMapping->integer > 1 ) {
						Q_strcat( extradefines, 1024, "#define USE_RELIEFMAP\n" );
					}

					if ( r_parallaxMapShadows->integer ) {
						Q_strcat( extradefines, 1024, "#define USE_PARALLAXMAP_SHADOWS\n" );
					}

					Q_strcat( extradefines, 1024, va( "#define r_parallaxMapOffset %f\n", r_parallaxMapOffset->value ) );
				}
			}

			if ( r_specularMapping->integer ) {
				Q_strcat( extradefines, 1024, "#define USE_SPECULARMAP\n" );
			}

			if ( r_cubeMapping->integer ) {
				Q_strcat( extradefines, 1024, "#define USE_CUBEMAP\n" );
				if ( r_cubeMapping->integer == 2 ) {
					Q_strcat( extradefines, 1024, "#define USE_BOX_CUBEMAP_PARALLAX\n" );
				}
			} else if ( r_deluxeSpecular->value > 0.000001f )     {
				Q_strcat( extradefines, 1024, va( "#define r_deluxeSpecular %f\n", r_deluxeSpecular->value ) );
			}

			switch ( r_glossType->integer )
			{
			case 0:
			default:
				Q_strcat( extradefines, 1024, "#define GLOSS_IS_GLOSS\n" );
				break;
			case 1:
				Q_strcat( extradefines, 1024, "#define GLOSS_IS_SMOOTHNESS\n" );
				break;
			case 2:
				Q_strcat( extradefines, 1024, "#define GLOSS_IS_ROUGHNESS\n" );
				break;
			case 3:
				Q_strcat( extradefines, 1024, "#define GLOSS_IS_SHININESS\n" );
				break;
			}
		}

		if ( i & LIGHTDEF_USE_SHADOWMAP ) {
			Q_strcat( extradefines, 1024, "#define USE_SHADOWMAP\n" );

			if ( r_sunlightMode->integer == 1 ) {
				Q_strcat( extradefines, 1024, "#define SHADOWMAP_MODULATE\n" );
			} else if ( r_sunlightMode->integer == 2 ) {
				Q_strcat( extradefines, 1024, "#define USE_PRIMARY_LIGHT\n" );
			}
		}

		if ( i & LIGHTDEF_USE_TCGEN_AND_TCMOD ) {
			Q_strcat( extradefines, 1024, "#define USE_TCGEN\n" );
			Q_strcat( extradefines, 1024, "#define USE_TCMOD\n" );
		}

		if ( i & LIGHTDEF_ENTITY_VERTEX_ANIMATION ) {
			Q_strcat( extradefines, 1024, "#define USE_MODELMATRIX\n" );

			if ( glRefConfig.gpuVertexAnimation ) {
				Q_strcat( extradefines, 1024, "#define USE_VERTEX_ANIMATION\n" );
				attribs |= ATTR_POSITION2 | ATTR_NORMAL2;

				if ( r_normalMapping->integer ) {
					attribs |= ATTR_TANGENT2;
				}
			}
		} else if ( i & LIGHTDEF_ENTITY_BONE_ANIMATION )     {
			Q_strcat( extradefines, 1024, "#define USE_MODELMATRIX\n" );
			Q_strcat( extradefines, 1024, va( "#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones ) );
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		// D3 fix: inject r_FBufScale and ROUGHNESS_MIPS into every lightall permutation.
		// GL2 injects these in GLSL_GetShaderHeader (tr_glsl.c:385-404) which runs for every
		// GLSL compile; the Metal path bypasses GetShaderHeader so we inject here instead.
		// MSL float2 syntax used (not GLSL vec2) — GLSL_StripGLSLOnlyDirectives only strips
		// #extension/#version, so these #defines pass through unmodified.
		// The MSL has #ifndef guards (tr_metal_shadersource.cpp:886-894) so these injected
		// #defines override the hardcoded 1920×1080 / 7-mip fallbacks.
		{
			float fbufWidthScale  = 1.0f / (float)glConfig.vidWidth;
			float fbufHeightScale = 1.0f / (float)glConfig.vidHeight;
			Q_strcat( extradefines, 1024,
					  va( "#define r_FBufScale float2(%f, %f)\n", fbufWidthScale, fbufHeightScale ) );

			if ( r_cubeMapping->integer ) {
				int cubeMipSize = r_cubemapSize->integer;
				int numRoughnessMips = 0;
				while ( cubeMipSize )
				{
					cubeMipSize >>= 1;
					numRoughnessMips++;
				}
				numRoughnessMips = MAX( 1, numRoughnessMips - 2 );
				Q_strcat( extradefines, 1024, va( "#define ROUGHNESS_MIPS float(%d)\n", numRoughnessMips ) );
			}
		}

		if ( !GLSL_InitGPUShader( &tr.lightallShader[i], "lightall", attribs, qtrue, extradefines, qtrue, fallbackShader_lightall_vp, fallbackShader_lightall_fp ) ) {
			ri.Error( ERR_FATAL, "Could not load lightall shader!" );
		}

		GLSL_InitUniforms( &tr.lightallShader[i] );

		GLSL_SetUniformInt( &tr.lightallShader[i], UNIFORM_DIFFUSEMAP,  TB_DIFFUSEMAP );
		GLSL_SetUniformInt( &tr.lightallShader[i], UNIFORM_LIGHTMAP,    TB_LIGHTMAP );
		GLSL_SetUniformInt( &tr.lightallShader[i], UNIFORM_NORMALMAP,   TB_NORMALMAP );
		GLSL_SetUniformInt( &tr.lightallShader[i], UNIFORM_DELUXEMAP,   TB_DELUXEMAP );
		GLSL_SetUniformInt( &tr.lightallShader[i], UNIFORM_SPECULARMAP, TB_SPECULARMAP );
		GLSL_SetUniformInt( &tr.lightallShader[i], UNIFORM_SHADOWMAP,   TB_SHADOWMAP );
		GLSL_SetUniformInt( &tr.lightallShader[i], UNIFORM_CUBEMAP,     TB_CUBEMAP );

		GLSL_FinishGPUShader( &tr.lightallShader[i] );

		numLightShaders++;
	}

	for (i = 0; i < SHADOWMAPDEF_COUNT; i++)
	{
		if ( ( i & SHADOWMAPDEF_USE_VERTEX_ANIMATION ) && ( i & SHADOWMAPDEF_USE_BONE_ANIMATION ) ) {
			continue;
		}

		if ( ( i & SHADOWMAPDEF_USE_VERTEX_ANIMATION ) && !glRefConfig.gpuVertexAnimation ) {
			continue;
		}

		if ( ( i & SHADOWMAPDEF_USE_BONE_ANIMATION ) && !glRefConfig.glslMaxAnimatedBones ) {
			continue;
		}

		attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD;

		extradefines[0] = '\0';

		if ( i & SHADOWMAPDEF_USE_VERTEX_ANIMATION ) {
			Q_strcat( extradefines, 1024, "#define USE_VERTEX_ANIMATION\n" );
			attribs |= ATTR_POSITION2 | ATTR_NORMAL2;
		}

		if ( i & SHADOWMAPDEF_USE_BONE_ANIMATION ) {
			Q_strcat( extradefines, 1024, va( "#define USE_BONE_ANIMATION\n#define MAX_GLSL_BONES %d\n", glRefConfig.glslMaxAnimatedBones ) );
			attribs |= ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS;
		}

		if ( !GLSL_InitGPUShader( &tr.shadowmapShader[i], "shadowfill", attribs, qtrue, extradefines, qtrue, fallbackShader_shadowfill_vp, fallbackShader_shadowfill_fp ) ) {
			ri.Error( ERR_FATAL, "Could not load shadowfill shader!" );
		}

		GLSL_InitUniforms( &tr.shadowmapShader[i] );
		GLSL_FinishGPUShader( &tr.shadowmapShader[i] );

		numEtcShaders++;
	}

	attribs = ATTR_POSITION | ATTR_NORMAL;
	extradefines[0] = '\0';

	Q_strcat( extradefines, 1024, "#define USE_PCF\n#define USE_DISCARD\n" );

	if ( !GLSL_InitGPUShader( &tr.pshadowShader, "pshadow", attribs, qtrue, extradefines, qtrue, fallbackShader_pshadow_vp, fallbackShader_pshadow_fp ) ) {
		ri.Error( ERR_FATAL, "Could not load pshadow shader!" );
	}

	GLSL_InitUniforms( &tr.pshadowShader );

	GLSL_SetUniformInt( &tr.pshadowShader, UNIFORM_SHADOWMAP, TB_DIFFUSEMAP );

	GLSL_FinishGPUShader( &tr.pshadowShader );

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if ( !GLSL_InitGPUShader( &tr.down4xShader, "down4x", attribs, qtrue, extradefines, qtrue, fallbackShader_down4x_vp, fallbackShader_down4x_fp ) ) {
		ri.Error( ERR_FATAL, "Could not load down4x shader!" );
	}

	GLSL_InitUniforms( &tr.down4xShader );

	GLSL_SetUniformInt( &tr.down4xShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP );

	GLSL_FinishGPUShader( &tr.down4xShader );

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if ( !GLSL_InitGPUShader( &tr.bokehShader, "bokeh", attribs, qtrue, extradefines, qtrue, fallbackShader_bokeh_vp, fallbackShader_bokeh_fp ) ) {
		ri.Error( ERR_FATAL, "Could not load bokeh shader!" );
	}

	GLSL_InitUniforms( &tr.bokehShader );

	GLSL_SetUniformInt( &tr.bokehShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP );

	GLSL_FinishGPUShader( &tr.bokehShader );

	numEtcShaders++;


	attribs = ATTR_POSITION | ATTR_TEXCOORD;
	extradefines[0] = '\0';

	if ( !GLSL_InitGPUShader( &tr.tonemapShader, "tonemap", attribs, qtrue, extradefines, qtrue, fallbackShader_tonemap_vp, fallbackShader_tonemap_fp ) ) {
		ri.Error( ERR_FATAL, "Could not load tonemap shader!" );
	}

	GLSL_InitUniforms( &tr.tonemapShader );

	GLSL_SetUniformInt( &tr.tonemapShader, UNIFORM_TEXTUREMAP, TB_COLORMAP );
	GLSL_SetUniformInt( &tr.tonemapShader, UNIFORM_LEVELSMAP,  TB_LEVELSMAP );

	GLSL_FinishGPUShader( &tr.tonemapShader );

	numEtcShaders++;


	for (i = 0; i < 2; i++)
	{
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if ( !i ) {
			Q_strcat( extradefines, 1024, "#define FIRST_PASS\n" );
		}

		if ( !GLSL_InitGPUShader( &tr.calclevels4xShader[i], "calclevels4x", attribs, qtrue, extradefines, qtrue, fallbackShader_calclevels4x_vp, fallbackShader_calclevels4x_fp ) ) {
			ri.Error( ERR_FATAL, "Could not load calclevels4x shader!" );
		}

		GLSL_InitUniforms( &tr.calclevels4xShader[i] );

		GLSL_SetUniformInt( &tr.calclevels4xShader[i], UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP );

		GLSL_FinishGPUShader( &tr.calclevels4xShader[i] );

		numEtcShaders++;
	}


	if ( glRefConfig.glslMajorVersion > 1 || ( glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 10 )
		 || glRefConfig.shadowSamplers ) {
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if ( qglesMajorVersion < 3 && glRefConfig.shadowSamplers ) {
			Q_strcat( extradefines, 1024, "#extension GL_EXT_shadow_samplers : enable\n" );
		}

		if ( r_shadowFilter->integer >= 1 ) {
			Q_strcat( extradefines, 1024, "#define USE_SHADOW_FILTER\n" );
		}

		if ( r_shadowFilter->integer >= 2 ) {
			Q_strcat( extradefines, 1024, "#define USE_SHADOW_FILTER2\n" );
		}

		if ( r_shadowCascadeZFar->integer != 0 ) {
			Q_strcat( extradefines, 1024, "#define USE_SHADOW_CASCADE\n" );
		}

		Q_strcat( extradefines, 1024, va( "#define r_shadowMapSize %f\n", r_shadowMapSize->value ) );
		Q_strcat( extradefines, 1024, va( "#define r_shadowCascadeZFar %f\n", r_shadowCascadeZFar->value ) );

		if ( !GLSL_InitGPUShader( &tr.shadowmaskShader, "shadowmask", attribs, qtrue, extradefines, qtrue, fallbackShader_shadowmask_vp, fallbackShader_shadowmask_fp ) ) {
			ri.Error( ERR_FATAL, "Could not load shadowmask shader!" );
		}

		GLSL_InitUniforms( &tr.shadowmaskShader );

		GLSL_SetUniformInt( &tr.shadowmaskShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP );
		GLSL_SetUniformInt( &tr.shadowmaskShader, UNIFORM_SHADOWMAP,  TB_SHADOWMAP );
		GLSL_SetUniformInt( &tr.shadowmaskShader, UNIFORM_SHADOWMAP2, TB_SHADOWMAP2 );
		GLSL_SetUniformInt( &tr.shadowmaskShader, UNIFORM_SHADOWMAP3, TB_SHADOWMAP3 );
		GLSL_SetUniformInt( &tr.shadowmaskShader, UNIFORM_SHADOWMAP4, TB_SHADOWMAP4 );

		GLSL_FinishGPUShader( &tr.shadowmaskShader );

		numEtcShaders++;
	}


	// D4 verdict: tr.greyscaleShader is parity-retained for GL2 structural fidelity but is NEVER
	// bound/drawn under Metal. The screen desaturation pass is handled entirely by the Metal-native
	// present pipeline (metalPresentGreyscale) via RB_DrawGreyscale → GLMetal_GreyscalePass.
	// No GLSL_BindProgram(&tr.greyscaleShader) call exists in the Metal renderer.
	// GL2 ref: tr_backend.c:1369 (RB_DrawGreyscale binds tr.greyscaleShader + u_Greyscale uniform).
	if ( !GLSL_InitGPUShader( &tr.greyscaleShader, "greyscale", attribs, qtrue, extradefines, qtrue,
							  fallbackShader_greyscale_vp, fallbackShader_greyscale_fp ) ) {
		ri.Error( ERR_FATAL, "Unable to load greyscale shader" );
	}

	GLSL_InitUniforms( &tr.greyscaleShader );
	GLSL_SetUniformInt( &tr.greyscaleShader, UNIFORM_TEXTUREMAP, TB_DIFFUSEMAP );
	GLSL_FinishGPUShader( &tr.greyscaleShader );

	numEtcShaders++;

	if ( glRefConfig.glslMajorVersion > 1 || ( glRefConfig.glslMajorVersion == 1 && glRefConfig.glslMinorVersion >= 10 )
		 || glRefConfig.standardDerivatives ) {
		attribs = ATTR_POSITION | ATTR_TEXCOORD;
		extradefines[0] = '\0';

		if ( qglesMajorVersion < 3 && glRefConfig.standardDerivatives ) {
			Q_strcat( extradefines, 1024, "#extension GL_OES_standard_derivatives : enable\n" );
		}

		if ( !GLSL_InitGPUShader( &tr.ssaoShader, "ssao", attribs, qtrue, extradefines, qtrue, fallbackShader_ssao_vp, fallbackShader_ssao_fp ) ) {
			ri.Error( ERR_FATAL, "Could not load ssao shader!" );
		}

		GLSL_InitUniforms( &tr.ssaoShader );

		GLSL_SetUniformInt( &tr.ssaoShader, UNIFORM_SCREENDEPTHMAP, TB_COLORMAP );

		GLSL_FinishGPUShader( &tr.ssaoShader );

		numEtcShaders++;


		for (i = 0; i < 4; i++)
		{
			attribs = ATTR_POSITION | ATTR_TEXCOORD;
			extradefines[0] = '\0';

			if ( qglesMajorVersion < 3 && glRefConfig.standardDerivatives ) {
				Q_strcat( extradefines, 1024, "#extension GL_OES_standard_derivatives : enable\n" );
			}

			if ( i & 1 ) {
				Q_strcat( extradefines, 1024, "#define USE_VERTICAL_BLUR\n" );
			} else {
				Q_strcat( extradefines, 1024, "#define USE_HORIZONTAL_BLUR\n" );
			}

			if ( !( i & 2 ) ) {
				Q_strcat( extradefines, 1024, "#define USE_DEPTH\n" );
			}


			if ( !GLSL_InitGPUShader( &tr.depthBlurShader[i], "depthBlur", attribs, qtrue, extradefines, qtrue, fallbackShader_depthblur_vp, fallbackShader_depthblur_fp ) ) {
				ri.Error( ERR_FATAL, "Could not load depthBlur shader!" );
			}

			GLSL_InitUniforms( &tr.depthBlurShader[i] );

			GLSL_SetUniformInt( &tr.depthBlurShader[i], UNIFORM_SCREENIMAGEMAP, TB_COLORMAP );
			GLSL_SetUniformInt( &tr.depthBlurShader[i], UNIFORM_SCREENDEPTHMAP, TB_LIGHTMAP );

			GLSL_FinishGPUShader( &tr.depthBlurShader[i] );

			numEtcShaders++;
		}
	}

	GLSL_FinishPendingMetalPrograms();

	endTime = ri.Milliseconds();

	ri.Printf( PRINT_ALL, "loaded %i GLSL shaders (%i gen %i light %i etc) in %5.2f seconds\n",
			   numGenShaders + numLightShaders + numEtcShaders, numGenShaders, numLightShaders,
			   numEtcShaders, ( endTime - startTime ) / 1000.0 );
}

void GLSL_ShutdownGPUShaders( void ) {
	int i;

	ri.Printf( PRINT_ALL, "------- GLSL_ShutdownGPUShaders -------\n" );

	s_pendingPrograms.clear();
	s_pendingProgramIndex.clear();

	// PARITY-SEAM(Metal-device): qglDisableVertexAttribArray loop → MTLVertexDescriptor teardown;
	// under Metal vertex attribute enable/disable is part of pipeline state, not runtime GL state.

	GL_BindNullProgram();

	for ( i = 0; i < GENERICDEF_COUNT; i++)
		GLSL_DeleteGPUShader( &tr.genericShader[i] );

	GLSL_DeleteGPUShader( &tr.textureColorShader );

	for ( i = 0; i < FOGDEF_COUNT; i++)
		GLSL_DeleteGPUShader( &tr.fogShader[i] );

	for ( i = 0; i < DLIGHTDEF_COUNT; i++)
		GLSL_DeleteGPUShader( &tr.dlightShader[i] );

	for ( i = 0; i < LIGHTDEF_COUNT; i++)
		GLSL_DeleteGPUShader( &tr.lightallShader[i] );

	for ( i = 0; i < SHADOWMAPDEF_COUNT; i++)
		GLSL_DeleteGPUShader( &tr.shadowmapShader[i] );

	GLSL_DeleteGPUShader( &tr.pshadowShader );
	GLSL_DeleteGPUShader( &tr.down4xShader );
	GLSL_DeleteGPUShader( &tr.bokehShader );
	GLSL_DeleteGPUShader( &tr.tonemapShader );

	for ( i = 0; i < 2; i++)
		GLSL_DeleteGPUShader( &tr.calclevels4xShader[i] );

	GLSL_DeleteGPUShader( &tr.shadowmaskShader );
	GLSL_DeleteGPUShader( &tr.ssaoShader );

	for ( i = 0; i < 4; i++)
		GLSL_DeleteGPUShader( &tr.depthBlurShader[i] );
}

void GLSL_BindProgram( shaderProgram_t * program ) {
	GLuint programObject = program ? program->program : 0;
	char *name = program ? program->name : (char *)"NULL";

	if ( r_logFile->integer ) {
		// PARITY-SEAM(Metal-device): GLimp_LogComment → no-op under Metal;
		// logging provided via ri.Printf or Metal GPU capture tools.
		(void)name;
	}

	// PHASE5-M5.2: Track the active program for R_DrawElements Metal dispatch.
	// GL2 equivalent: GL_UseProgram sets the current program; here we record it.
	glMetal_activeProgram = program;

	// PARITY-SEAM(Metal-device): GL_UseProgram(programObject) → MTLRenderCommandEncoder
	// setRenderPipelineState: call; needs MTLRenderCommandEncoder (Phase: device bring-up).
	if ( GL_UseProgram( programObject ) ) {
		backEnd.pc.c_glslShaderBinds++;
	}
}

// GL2 ref: (no direct GL2 equivalent — PHASE5-M5.2 Metal bring-up accessor)
// Returns the program last bound via GLSL_BindProgram, for use in R_DrawElements.
shaderProgram_t *GLSL_GetActiveMetalProgram( void ) {
	return glMetal_activeProgram;
}

shaderProgram_t *GLSL_GetGenericShaderProgram( int stage, glfog_t *glFog ) {
	shaderStage_t *pStage = tess.xstages[stage];
	int shaderAttribs = 0;

	if ( tess.fogNum && pStage->adjustColorsForFog ) {
		shaderAttribs |= GENERICDEF_USE_FOG;
	}

	//
	// RTCW fog
	//
	if ( glFog ) {
		shaderAttribs |= GENERICDEF_USE_FOG;
		if ( glFog->mode == GL_LINEAR ) {
			shaderAttribs |= GENERICDEF_USE_WOLF_FOG_LINEAR;
		} else { // if (glFog->mode == GL_EXP)
			shaderAttribs |= GENERICDEF_USE_WOLF_FOG_EXPONENTIAL;
		}
	}

	switch ( pStage->rgbGen )
	{
	case CGEN_LIGHTING_DIFFUSE:
		shaderAttribs |= GENERICDEF_USE_RGBAGEN;
		break;
	default:
		break;
	}

	switch ( pStage->alphaGen )
	{
	case AGEN_LIGHTING_SPECULAR:
	case AGEN_PORTAL:
	case AGEN_NORMALZFADE:
		shaderAttribs |= GENERICDEF_USE_RGBAGEN;
		break;
	default:
		break;
	}

	if ( pStage->bundle[0].tcGen != TCGEN_TEXTURE ) {
		shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
	}

	if ( tess.shader->numDeforms && !ShaderRequiresCPUDeforms( tess.shader ) ) {
		shaderAttribs |= GENERICDEF_USE_DEFORM_VERTEXES;
	}

	if ( glState.vertexAnimation ) {
		shaderAttribs |= GENERICDEF_USE_VERTEX_ANIMATION;
	} else if ( glState.boneAnimation )     {
		shaderAttribs |= GENERICDEF_USE_BONE_ANIMATION;
	}

	if ( pStage->bundle[0].numTexMods ) {
		shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
	}

	return &tr.genericShader[shaderAttribs];
}

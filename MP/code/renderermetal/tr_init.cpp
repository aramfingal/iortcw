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
// tr_init.cpp — Metal renderer lifecycle / initialisation
//
// Phase-1 FULL PORT: all 28 functions from tr_init.c are present.
//   15 REAL      — GL-agnostic bookkeeping; ported directly.
//    9 PARITY-SEAM — real parts preserved; GL/GLimp calls documented and
//                   replaced with seams pending Metal device/swapchain in Phase 4.
//    4 PARITY-STUB — GL-only (error queries, memory extensions, occlusion
//                   queries); no Metal equivalent; documented no-ops.
//
// KNOWN HARD SEAM — Metal device / window / swapchain
//   GL2 calls GLimp_Init (SDL + GL context), reads GL strings/extensions,
//   and uses qgl* function pointers throughout.  Under Metal there is no GL
//   context.  Functions that touch that layer are marked:
//     PARITY-SEAM(Metal-device): <reason>. GL2 ref: tr_init.c:NNN
//   Real Metal device + CAMetalLayer bring-up is Phase 4 work (Hudson).
//   sdl_glimp.c is EXCLUDED from the Metal build; GLimp_* stubs are
//   provided below so the object file links cleanly.

/*
 * metal-cpp SINGLE-IMPLEMENTATION TU
 *
 * metal-cpp is a header-only C++ wrapper around the Metal Objective-C API.
 * It requires exactly ONE translation unit to define the concrete
 * implementations of NS/MTL/CA private symbols by setting these macros
 * BEFORE the metal-cpp headers are included.  If they appear in multiple TUs
 * the linker produces duplicate-symbol errors; if they appear in none the
 * linker produces undefined-symbol errors.
 *
 * They live here — folded into tr_init.cpp — to preserve the strict 1:1
 * filename parity rule (every renderermetal/ file maps to an identically-
 * named renderergl2/ counterpart; no extra files are permitted).  There is
 * no GL2 counterpart concept for these defines — they are a metal-cpp build
 * requirement, not renderer logic.
 *
 * q_shared.h MUST come first: it defines qboolean, qtrue, qfalse, etc.
 * before the Metal/Foundation headers pull in 'Boolean' and other macOS
 * types that are distinct from our q* types.  Wrapped in extern "C" to
 * prevent C++ name-mangling of the C declarations.
 */
extern "C" {
#include "../qcommon/q_shared.h"
}

// metal-cpp single-implementation defines — MUST precede the metal-cpp includes.
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

// Pull the metal-cpp headers.  Include path for-copilot/metal-cpp/ is added
// to the target in cmake/renderer_metal.cmake.
#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <QuartzCore/QuartzCore.hpp>

/*
 * INCLUDE STRATEGY — mirrors GL2 tr_init.c:24-26
 *   tr_local.h (Phase-0, ported 2026-06-04): all renderer types, externs,
 *     function declarations.  GL entry-point block (qgl.h/QGL_*_PROCS) is
 *     commented-out in the Metal copy; GL scalar types are plain typedefs.
 *   tr_dsa.h  (Phase-0, ported 2026-06-04): DSA helper prototypes.
 *   C headers wrapped in extern "C" to prevent C++ name-mangling.
 *   GL2 ref: tr_init.c:24-26
 */
extern "C" {
#include "tr_local.h"
#include "tr_dsa.h"
}

// Metal device API. Header wraps its C declarations in extern "C" internally.
#include "tr_metal_device.h"

// GLimp_Init/Shutdown/Minimize implemented in tr_metal_glimp.cpp (Phase 5 device bring-up)
// GLimp_InitExtraExtensions is implemented in tr_extensions.cpp (Phase 4).
// GL2 ref: sdl_glimp.c, tr_extensions.c:33

// ============================================================================
// Globals
// ============================================================================

glconfig_t glConfig;
glRefConfig_t glRefConfig;
// renderer-import table moved to tr_main.cpp with trGlobals_t tr.
// GL2 ref: tr_main.c:28,40
qboolean textureFilterAnisotropic = qfalse;
int maxAnisotropy            = 0;
float displayAspect            = 0.0f;
qboolean haveClampToEdge          = qfalse;
glstate_t glState;

// GL2 ref: tr_init.c:41 (USE_RENDERER_DLOPEN guard)
#ifdef USE_RENDERER_DLOPEN
cvar_t  *com_altivec;
#endif

// GL2 ref: tr_init.c:44-239 — cvar pointer definitions
cvar_t  *r_flareSize;
cvar_t  *r_flareFade;
cvar_t  *r_flareCoeff;

cvar_t  *r_railWidth;
cvar_t  *r_railCoreWidth;
cvar_t  *r_railSegmentLength;

cvar_t  *r_verbose;
cvar_t  *r_ignore;

cvar_t  *r_displayRefresh;

cvar_t  *r_detailTextures;

cvar_t  *r_znear;
cvar_t  *r_zproj;
cvar_t  *r_stereoSeparation;
cvar_t  *r_zfar;

cvar_t  *r_skipBackEnd;

cvar_t  *r_stereoEnabled;
cvar_t  *r_anaglyphMode;

cvar_t  *r_greyscale;

cvar_t  *r_ignorehwgamma;
cvar_t  *r_measureOverdraw;

cvar_t  *r_inGameVideo;
cvar_t  *r_fastsky;
cvar_t  *r_drawSun;
cvar_t  *r_dynamiclight;
cvar_t  *r_dlightBacks;

cvar_t  *r_lodbias;
cvar_t  *r_lodscale;

cvar_t  *r_norefresh;
cvar_t  *r_drawentities;
cvar_t  *r_drawworld;
cvar_t  *r_speeds;
cvar_t  *r_fullbright; // JPW NERVE removed per atvi request
cvar_t  *r_novis;
cvar_t  *r_nocull;
cvar_t  *r_facePlaneCull;
cvar_t  *r_showcluster;
cvar_t  *r_nocurves;

cvar_t  *r_allowExtensions;

cvar_t  *r_ext_compressed_textures;
cvar_t  *r_ext_multitexture;
cvar_t  *r_ext_compiled_vertex_array;
cvar_t  *r_ext_texture_env_add;
cvar_t  *r_ext_texture_filter_anisotropic;
cvar_t  *r_ext_max_anisotropy;

//----(SA)	added
cvar_t  *r_ext_NV_fog_dist;
cvar_t  *r_nv_fogdist_mode;

cvar_t  *r_ext_ATI_pntriangles;
cvar_t  *r_ati_truform_tess;        //
cvar_t  *r_ati_truform_normalmode;  // linear/quadratic
cvar_t  *r_ati_truform_pointmode;   // linear/cubic
cvar_t  *r_ati_fsaa_samples;        //DAJ valids are 1, 2, 4
//----(SA)	end

cvar_t  *r_ext_framebuffer_object;
cvar_t  *r_ext_texture_float;
cvar_t  *r_ext_framebuffer_multisample;
cvar_t  *r_arb_seamless_cube_map;
cvar_t  *r_arb_vertex_array_object;
cvar_t  *r_ext_direct_state_access;

cvar_t  *r_cameraExposure;

cvar_t  *r_externalGLSL;

cvar_t  *r_hdr;
cvar_t  *r_floatLightmap;
cvar_t  *r_postProcess;
// SLICE-B ACCEPTANCE HOOK cvar (temporary).
cvar_t  *r_metalBlitTest;

cvar_t  *r_toneMap;
cvar_t  *r_forceToneMap;
cvar_t  *r_forceToneMapMin;
cvar_t  *r_forceToneMapAvg;
cvar_t  *r_forceToneMapMax;

cvar_t  *r_autoExposure;
cvar_t  *r_forceAutoExposure;
cvar_t  *r_forceAutoExposureMin;
cvar_t  *r_forceAutoExposureMax;
cvar_t  *r_debugAutoExposure; // Metal-only debug; no GL2 equivalent. GL2 ref: tr_postprocess.c (no cvar).

cvar_t  *r_depthPrepass;
cvar_t  *r_ssao;

cvar_t  *r_normalMapping;
cvar_t  *r_specularMapping;
cvar_t  *r_deluxeMapping;
cvar_t  *r_parallaxMapping;
cvar_t  *r_parallaxMapOffset;
cvar_t  *r_parallaxMapShadows;
cvar_t  *r_cubeMapping;
cvar_t  *r_cubemapSize;
cvar_t  *r_deluxeSpecular;
cvar_t  *r_pbr;
cvar_t  *r_baseNormalX;
cvar_t  *r_baseNormalY;
cvar_t  *r_baseParallax;
cvar_t  *r_baseSpecular;
cvar_t  *r_baseGloss;
cvar_t  *r_glossType;
cvar_t  *r_mergeLightmaps;
cvar_t  *r_dlightMode;
cvar_t  *r_pshadowDist;
cvar_t  *r_imageUpsample;
cvar_t  *r_imageUpsampleMaxSize;
cvar_t  *r_imageUpsampleType;
cvar_t  *r_genNormalMaps;
cvar_t  *r_forceSun;
cvar_t  *r_forceSunLightScale;
cvar_t  *r_forceSunAmbientScale;
cvar_t  *r_sunlightMode;
cvar_t  *r_drawSunRays;
cvar_t  *r_sunShadows;
cvar_t  *r_shadowFilter;
cvar_t  *r_shadowBlur;
cvar_t  *r_shadowMapSize;
cvar_t  *r_shadowCascadeZNear;
cvar_t  *r_shadowCascadeZFar;
cvar_t  *r_shadowCascadeZBias;
cvar_t  *r_ignoreDstAlpha;

cvar_t  *r_ignoreGLErrors;
cvar_t  *r_logFile;

cvar_t  *r_stencilbits;
cvar_t  *r_depthbits;
cvar_t  *r_colorbits;
cvar_t  *r_texturebits;
cvar_t  *r_ext_multisample;

cvar_t  *r_drawBuffer;
cvar_t  *r_glIgnoreWicked3D;
cvar_t  *r_lightmap;
cvar_t  *r_vertexLight;
cvar_t  *r_uiFullScreen;
cvar_t  *r_shadows;
cvar_t  *r_portalsky;   //----(SA)	added
cvar_t  *r_flares;
cvar_t  *r_mode;
cvar_t  *r_nobind;
cvar_t  *r_singleShader;
cvar_t  *r_roundImagesDown;
cvar_t  *r_colorMipLevels;
cvar_t  *r_picmip;
cvar_t  *r_showtris;
cvar_t  *r_showsky;
cvar_t  *r_shownormals;
cvar_t  *r_finish;
cvar_t  *r_clear;
cvar_t  *r_swapInterval;
cvar_t  *r_textureMode;
cvar_t  *r_offsetFactor;
cvar_t  *r_offsetUnits;
cvar_t  *r_gamma;
cvar_t  *r_intensity;
cvar_t  *r_lockpvs;
cvar_t  *r_noportals;
cvar_t  *r_portalOnly;

cvar_t  *r_subdivisions;
cvar_t  *r_lodCurveError;

cvar_t  *r_fullscreen;
cvar_t  *r_noborder;

cvar_t  *r_customwidth;
cvar_t  *r_customheight;
cvar_t  *r_customPixelAspect;

cvar_t  *r_overBrightBits;
cvar_t  *r_mapOverBrightBits;

cvar_t  *r_debugSurface;
cvar_t  *r_simpleMipMaps;

cvar_t  *r_showImages;

cvar_t  *r_ambientScale;
cvar_t  *r_directedScale;
cvar_t  *r_debugLight;
cvar_t  *r_debugSort;
cvar_t  *r_printShaders;
cvar_t  *r_saveFontData;

cvar_t  *r_marksOnTriangleMeshes;

cvar_t  *r_vaoCache;
cvar_t  *r_metalGpuMd3;

cvar_t  *r_aviMotionJpegQuality;
cvar_t  *r_screenshotJpegQuality;

// Ridah
cvar_t  *r_compressModels;
cvar_t  *r_exportCompressedModels;

cvar_t  *r_buildScript;

cvar_t  *r_bonesDebug;
// done.

// Rafael - wolf fog
cvar_t  *r_wolffog;
// done

cvar_t  *r_highQualityVideo;
cvar_t  *r_rmse;

cvar_t  *r_maxpolys;
int max_polys;
cvar_t  *r_maxpolyverts;
int max_polyverts;

// ============================================================================
// Static forward declarations — GL2 ref: tr_init.c:37-38
// ============================================================================
static void GfxInfo_f( void );
static void GfxMemInfo_f( void );
static void InitOpenGL( void );
static void R_ModeList_f( void );

// ============================================================================
// Video mode table — GL2 ref: tr_init.c:357-379 (COPY-THEN-ANNOTATE)
// ============================================================================
typedef struct vidmode_s
{
	const char *description;
	int width, height;
	float pixelAspect;              // pixel width / height
} vidmode_t;

// Note: Also add these modes to ui/ui_shared.c
vidmode_t r_vidModes[] =
{
	{ "Mode  0:   320x240   (4:3)",     320,     240,    1 },
	{ "Mode  1:   400x300   (4:3)",     400,     300,    1 },
	{ "Mode  2:   512x384   (4:3)",     512,     384,    1 },
	{ "Mode  3:   640x480   (4:3)",     640,     480,    1 },
	{ "Mode  4:   800x600   (4:3)",     800,     600,    1 },
	{ "Mode  5:   960x720   (4:3)",     960,     720,    1 },
	{ "Mode  6:  1024x768   (4:3)",    1024,     768,    1 },
	{ "Mode  7:  1152x864   (4:3)",    1152,     864,    1 },
	{ "Mode  8: 1280x1024   (5:4)",    1280,    1024,    1 },
	{ "Mode  9: 1600x1200   (4:3)",    1600,    1200,    1 },
	{ "Mode 10: 2048x1536   (4:3)",    2048,    1536,    1 },
	{ "Mode 11:   856x480  (16:9)",     856,     480,    1 },
	{ "Mode 12:   640x360  (16:9)",     640,     360,    1 },
	{ "Mode 13:   640x400 (16:10)",     640,     400,    1 },
	{ "Mode 14:   800x450  (16:9)",     800,     450,    1 },
	{ "Mode 15:   800x500 (16:10)",     800,     500,    1 },
	{ "Mode 16:  1024x640 (16:10)",    1024,     640,    1 },
	{ "Mode 17:  1024x576  (16:9)",    1024,     576,    1 },
	{ "Mode 18:  1280x720  (16:9)",    1280,     720,    1 },
	{ "Mode 19:  1280x768 (16:10)",    1280,     768,    1 },
	{ "Mode 20:  1280x800 (16:10)",    1280,     800,    1 },
	{ "Mode 21:  1280x960   (4:3)",    1280,     960,    1 },
	{ "Mode 22:  1440x900 (16:10)",    1440,     900,    1 },
	{ "Mode 23:  1600x900  (16:9)",    1600,     900,    1 },
	{ "Mode 24: 1600x1000 (16:10)",    1600,    1000,    1 },
	{ "Mode 25: 1680x1050 (16:10)",    1680,    1050,    1 },
	{ "Mode 26: 1920x1080  (16:9)",    1920,    1080,    1 },
	{ "Mode 27: 1920x1200 (16:10)",    1920,    1200,    1 },
	{ "Mode 28: 1920x1440   (4:3)",    1920,    1440,    1 },
	{ "Mode 29: 2560x1600 (16:10)",    2560,    1600,    1 }
};
static int s_numVidModes = ARRAY_LEN( r_vidModes );

// ============================================================================
// FUNCTION 1/28: InitOpenGL (static)
// PARITY-SEAM(Metal-device): GLimp_Init now creates the Metal window/drawable
//   and populates window/device glConfig fields. InitOpenGL only fills Metal
//   capability fields that GLimp_Init does not set.
// ============================================================================
static void InitOpenGL( void ) {
	if ( glConfig.vidWidth == 0 ) {
		// GLimp_Init owns window/drawable + device glConfig fields under Metal.
		GLimp_Init( qfalse );
		GLimp_InitExtraExtensions();

		// Populate remaining Metal capability defaults not owned by GLimp_Init.
		glConfig.textureEnvAddAvailable = qtrue;
		glConfig.maxTextureSize         = 16384;  // conservative; all Apple Silicon
		glConfig.numTextureUnits        = 16;
		glRefConfig.maxVertexAttribs    = 31;     // Metal supports 31 vertex attributes
		glRefConfig.glslMaxAnimatedBones = 64;    // conservative safe value

		glConfig.displayFrequency = 60;
		glConfig.isFullscreen     = ( r_fullscreen && r_fullscreen->integer ) ? qtrue : qfalse;
		glConfig.stereoEnabled    = qfalse;
		glConfig.smpActive        = qfalse;

		// PARITY-SEAM(Metal-device): r_cubeMapping check requires QGL_VERSION_ATLEAST;
		// under Metal cubemap support is always available. GL2 ref: tr_init.c:301-304
		// (no-op seam — r_cubeMapping works under Metal unconditionally)
	}

	// PARITY-SEAM(Metal-device): GL_SetDefaultState sets GL pipeline state and
	// glState struct fields.  Metal pipeline state is per-PSO; the glState struct
	// population inside GL_SetDefaultState is preserved. GL2 ref: tr_init.c:307
	GL_SetDefaultState();
}

// ============================================================================
// FUNCTION 2/28: GL_CheckErrs
// PARITY-STUB: no-op under Metal — Metal has no global GL error code.
//   Metal command errors surface via MTLCommandBuffer.error (Phase 4).
// ============================================================================
void GL_CheckErrs( const char *file, int line ) {
	// PARITY-STUB: no-op under Metal — GL error polling not applicable.
	(void)file; (void)line;
}

// ============================================================================
// FUNCTION 3/28: R_GetModeInfo — REAL
// Pure math / cvar lookup; no GL.  GL2 ref: tr_init.c:381
// ============================================================================
qboolean R_GetModeInfo( int *width, int *height, float *windowAspect, int mode ) {
	vidmode_t   *vm;
	float pixelAspect;

	if ( mode < -1 ) {
		return qfalse;
	}
	if ( mode >= s_numVidModes ) {
		return qfalse;
	}

	if ( mode == -1 ) {
		*width = r_customwidth->integer;
		*height = r_customheight->integer;
		pixelAspect = r_customPixelAspect->value;
	} else {
		vm = &r_vidModes[mode];

		*width  = vm->width;
		*height = vm->height;
		pixelAspect = vm->pixelAspect;
	}

	*windowAspect = (float)*width / ( *height * pixelAspect );

	return qtrue;
}

// ============================================================================
// FUNCTION 4/28: R_ModeList_f (static) — REAL
// ============================================================================
static void R_ModeList_f( void ) {
	int i;

	ri.Printf( PRINT_ALL, "\n" );
	for ( i = 0; i < s_numVidModes; i++ )
	{
		ri.Printf( PRINT_ALL, "%s\n", r_vidModes[i].description );
	}
	ri.Printf( PRINT_ALL, "\n" );
}

// ============================================================================
// FUNCTION 5/28: RB_ReadPixels
// Reads RGBA8 top-down pixels from GLMetal_ReadPixels (one presented frame of
// latency) and converts them to the GL2 screenshot contract: RGB, bottom-up,
// with rows padded to GL_PACK_ALIGNMENT.
// ============================================================================
byte *RB_ReadPixels( int x, int y, int width, int height, size_t *offset, int *padlen ) {
	byte *buffer, *bufstart, *scratch;
	int packAlign = 4; // GL default GL_PACK_ALIGNMENT; Metal has no pack state.
	int linelen, padwidth;
	int row, col;

	linelen = width * 3;
	padwidth = PAD( linelen, packAlign );

	// Allocate a few more bytes so that we can choose an alignment we like.
	buffer = (byte *)ri.Hunk_AllocateTempMemory( padwidth * height + *offset + packAlign - 1 );
	bufstart = (byte *)PADP( (intptr_t)buffer + *offset, packAlign );

	scratch = (byte *)ri.Hunk_AllocateTempMemory( width * height * 4 );
	if ( !GLMetal_ReadPixels( x, y, width, height, scratch ) ) {
		ri.Hunk_FreeTempMemory( scratch );
		ri.Hunk_FreeTempMemory( buffer );
		return NULL;
	}

	for ( row = 0; row < height; row++ ) {
		const byte *src = scratch + ( height - 1 - row ) * width * 4;
		byte *dst = bufstart + row * padwidth;

		for ( col = 0; col < width; col++ ) {
			dst[col * 3 + 0] = src[col * 4 + 0];
			dst[col * 3 + 1] = src[col * 4 + 1];
			dst[col * 3 + 2] = src[col * 4 + 2];
		}
	}

	ri.Hunk_FreeTempMemory( scratch );

	*offset = bufstart - buffer;
	*padlen = padwidth - linelen;

	return buffer;
}

// ============================================================================
// FUNCTION 6/28: RB_TakeScreenshot
// ============================================================================
void RB_TakeScreenshot( int x, int y, int width, int height, char *fileName ) {
	byte *allbuf, *buffer;
	byte *srcptr, *destptr;
	byte *endline, *endmem;
	byte temp;

	int linelen, padlen;
	size_t offset = 18, memcount;

	allbuf = RB_ReadPixels( x, y, width, height, &offset, &padlen );
	if ( !allbuf ) {
		return;
	}
	buffer = allbuf + offset - 18;

	Com_Memset( buffer, 0, 18 );
	buffer[2] = 2;      // uncompressed type
	buffer[12] = width & 255;
	buffer[13] = width >> 8;
	buffer[14] = height & 255;
	buffer[15] = height >> 8;
	buffer[16] = 24;    // pixel size

	linelen = width * 3;

	srcptr = destptr = allbuf + offset;
	endmem = srcptr + ( linelen + padlen ) * height;

	while ( srcptr < endmem )
	{
		endline = srcptr + linelen;
		while ( srcptr < endline )
		{
			temp = srcptr[0];
			*destptr++ = srcptr[2];
			*destptr++ = srcptr[1];
			*destptr++ = temp;
			srcptr += 3;
		}
		srcptr += padlen;
	}

	memcount = linelen * height;

	if ( glConfig.deviceSupportsGamma ) {
		R_GammaCorrect( allbuf + offset, memcount );
	}

	ri.FS_WriteFile( fileName, buffer, memcount + 18 );

	ri.Hunk_FreeTempMemory( allbuf );
}

// ============================================================================
// FUNCTION 7/28: RB_TakeScreenshotJPEG
// ============================================================================
void RB_TakeScreenshotJPEG( int x, int y, int width, int height, char *fileName ) {
	byte *buffer;
	size_t offset = 0, memcount;
	int padlen;

	buffer = RB_ReadPixels( x, y, width, height, &offset, &padlen );
	if ( !buffer ) {
		return;
	}
	memcount = ( width * 3 + padlen ) * height;

	if ( glConfig.deviceSupportsGamma ) {
		R_GammaCorrect( buffer + offset, memcount );
	}

	RE_SaveJPG( fileName, r_screenshotJpegQuality->integer, width, height,
				buffer + offset, padlen );
	ri.Hunk_FreeTempMemory( buffer );
}

// ============================================================================
// FUNCTION 8/28: RB_TakeScreenshotCmd
// ============================================================================
// PARITY-SEAM(Metal-device): the Metal device only copies the presented drawable for
// readback on demand (a full-frame copy every frame is pure overhead otherwise). A screenshot
// requested while no fresh capture exists is therefore deferred: capture is armed for the
// frame being built, and the screenshot is written at the start of the next backend frame,
// from that captured image. Levelshots (front-end, R_LevelShot) are deferred the same way.
#define METAL_CAPTURE_MAX_ATTEMPTS 8
static struct {
	qboolean pending;
	qboolean jpeg;
	int x, y, width, height;
	char fileName[MAX_OSPATH];
	int attempts;
} s_pendingScreenshot;
static qboolean s_pendingLevelShot;
static int s_pendingLevelShotAttempts;

void R_LevelShot( void );

static void RB_TakeScreenshotNow( int x, int y, int width, int height, char *fileName, qboolean jpeg ) {
	if ( jpeg ) {
		RB_TakeScreenshotJPEG( x, y, width, height, fileName );
	} else {
		RB_TakeScreenshot( x, y, width, height, fileName );
	}
}

// Called at the start of every backend frame (RB_ExecuteRenderCommands).
void R_MetalProcessPendingCaptures( void ) {
	if ( s_pendingScreenshot.pending ) {
		if ( GLMetal_CaptureReady() ) {
			s_pendingScreenshot.pending = qfalse;
			RB_TakeScreenshotNow( s_pendingScreenshot.x, s_pendingScreenshot.y,
								  s_pendingScreenshot.width, s_pendingScreenshot.height,
								  s_pendingScreenshot.fileName, s_pendingScreenshot.jpeg );
		} else if ( ++s_pendingScreenshot.attempts > METAL_CAPTURE_MAX_ATTEMPTS )   {
			s_pendingScreenshot.pending = qfalse;
			ri.Printf( PRINT_WARNING, "Screenshot %s failed: no frame capture available\n", s_pendingScreenshot.fileName );
		} else
		{
			GLMetal_RequestCapture();
		}
	}

	if ( s_pendingLevelShot ) {
		if ( GLMetal_CaptureReady() ) {
			s_pendingLevelShot = qfalse;
			R_LevelShot();
		} else if ( ++s_pendingLevelShotAttempts > METAL_CAPTURE_MAX_ATTEMPTS )   {
			s_pendingLevelShot = qfalse;
			ri.Printf( PRINT_WARNING, "Levelshot failed: no frame capture available\n" );
		} else
		{
			GLMetal_RequestCapture();
		}
	}
}

const void *RB_TakeScreenshotCmd( const void *data ) {
	const screenshotCommand_t *cmd;

	cmd = (const screenshotCommand_t *)data;

	// finish any 2D drawing if needed — sibling dep (tr_surface.cpp / Phase 1)
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	if ( !GLMetal_CaptureReady() ) {
		GLMetal_RequestCapture();
		s_pendingScreenshot.pending  = qtrue;
		s_pendingScreenshot.jpeg     = cmd->jpeg;
		s_pendingScreenshot.x        = cmd->x;
		s_pendingScreenshot.y        = cmd->y;
		s_pendingScreenshot.width    = cmd->width;
		s_pendingScreenshot.height   = cmd->height;
		s_pendingScreenshot.attempts = 0;
		Q_strncpyz( s_pendingScreenshot.fileName, cmd->fileName, sizeof( s_pendingScreenshot.fileName ) );
		return (const void *)( cmd + 1 );
	}

	if ( cmd->jpeg ) {
		RB_TakeScreenshotJPEG( cmd->x, cmd->y, cmd->width, cmd->height, cmd->fileName );
	} else {
		RB_TakeScreenshot( cmd->x, cmd->y, cmd->width, cmd->height, cmd->fileName );
	}

	return (const void *)( cmd + 1 );
}

// ============================================================================
// FUNCTION 9/28: R_TakeScreenshot — REAL
// Uses R_GetCommandBuffer (sibling dep) — no GL.  GL2 ref: tr_init.c:618
// ============================================================================
void R_TakeScreenshot( int x, int y, int width, int height, char *name, qboolean jpeg ) {
	static char fileName[MAX_OSPATH];
	screenshotCommand_t *cmd;

	cmd = (screenshotCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SCREENSHOT;

	cmd->x = x;
	cmd->y = y;
	cmd->width = width;
	cmd->height = height;
	Q_strncpyz( fileName, name, sizeof( fileName ) );
	cmd->fileName = fileName;
	cmd->jpeg = jpeg;
}

// ============================================================================
// FUNCTION 10/28: R_ScreenshotFilename — REAL
// Pure string formatting; no GL.  GL2 ref: tr_init.c:642
// ============================================================================
void R_ScreenshotFilename( int lastNumber, char *fileName ) {
	int a,b,c,d;

	if ( lastNumber < 0 || lastNumber > 9999 ) {
		Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot9999.tga" );
		return;
	}

	a = lastNumber / 1000;
	lastNumber -= a * 1000;
	b = lastNumber / 100;
	lastNumber -= b * 100;
	c = lastNumber / 10;
	lastNumber -= c * 10;
	d = lastNumber;

	Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot%i%i%i%i.tga"
				 , a, b, c, d );
}

// ============================================================================
// FUNCTION 11/28: R_ScreenshotFilenameJPEG — REAL
// Pure string formatting; no GL.  GL2 ref: tr_init.c:667
// ============================================================================
void R_ScreenshotFilenameJPEG( int lastNumber, char *fileName ) {
	int a,b,c,d;

	if ( lastNumber < 0 || lastNumber > 9999 ) {
		Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot9999.jpg" );
		return;
	}

	a = lastNumber / 1000;
	lastNumber -= a * 1000;
	b = lastNumber / 100;
	lastNumber -= b * 100;
	c = lastNumber / 10;
	lastNumber -= c * 10;
	d = lastNumber;

	Com_sprintf( fileName, MAX_OSPATH, "screenshots/shot%i%i%i%i.jpg"
				 , a, b, c, d );
}

// ============================================================================
// FUNCTION 12/28: R_LevelShot
// ============================================================================
void R_LevelShot( void ) {
	// Metal: see s_pendingScreenshot — defer until a captured frame exists.
	if ( !GLMetal_CaptureReady() ) {
		GLMetal_RequestCapture();
		if ( !s_pendingLevelShot ) {
			s_pendingLevelShotAttempts = 0;
		}
		s_pendingLevelShot = qtrue;
		return;
	}

	char checkname[MAX_OSPATH];
	byte        *buffer;
	byte        *source, *allsource;
	byte        *src, *dst;
	size_t offset = 0;
	int padlen;
	int x, y;
	int r, g, b;
	float xScale, yScale;
	int xx, yy;

	Com_sprintf( checkname, sizeof( checkname ), "levelshots/%s.tga", tr.world->baseName );

	allsource = RB_ReadPixels( 0, 0, glConfig.vidWidth, glConfig.vidHeight,
							   &offset, &padlen );
	if ( !allsource ) {
		return;
	}
	source = allsource + offset;

	buffer = (byte *)ri.Hunk_AllocateTempMemory( 128 * 128 * 3 + 18 );
	Com_Memset( buffer, 0, 18 );
	buffer[2] = 2;      // uncompressed type
	buffer[12] = 128;
	buffer[14] = 128;
	buffer[16] = 24;    // pixel size

	xScale = glConfig.vidWidth / 512.0f;
	yScale = glConfig.vidHeight / 384.0f;
	for ( y = 0 ; y < 128 ; y++ ) {
		for ( x = 0 ; x < 128 ; x++ ) {
			r = g = b = 0;
			for ( yy = 0 ; yy < 3 ; yy++ ) {
				for ( xx = 0 ; xx < 4 ; xx++ ) {
					src = source + ( 3 * glConfig.vidWidth + padlen ) *
						  (int)( ( y * 3 + yy ) * yScale ) +
						  3 * (int)( ( x * 4 + xx ) * xScale );
					r += src[0];
					g += src[1];
					b += src[2];
				}
			}
			dst = buffer + 18 + 3 * ( y * 128 + x );
			dst[0] = b / 12;
			dst[1] = g / 12;
			dst[2] = r / 12;
		}
	}

	if ( glConfig.deviceSupportsGamma ) {
		R_GammaCorrect( buffer + 18, 128 * 128 * 3 );
	}

	ri.FS_WriteFile( checkname, buffer, 128 * 128 * 3 + 18 );

	ri.Hunk_FreeTempMemory( buffer );
	ri.Hunk_FreeTempMemory( allsource );

	ri.Printf( PRINT_ALL, "Wrote %s\n", checkname );
}

// ============================================================================
// FUNCTION 13/28: R_ScreenShot_f — REAL
// Dispatch logic only; pixel work via R_TakeScreenshot / R_LevelShot.
// ============================================================================
void R_ScreenShot_f( void ) {
	char checkname[MAX_OSPATH];
	static int lastNumber = -1;
	qboolean silent;

	if ( !strcmp( ri.Cmd_Argv( 1 ), "levelshot" ) ) {
		R_LevelShot();
		return;
	}

	if ( !strcmp( ri.Cmd_Argv( 1 ), "silent" ) ) {
		silent = qtrue;
	} else {
		silent = qfalse;
	}

	if ( ri.Cmd_Argc() == 2 && !silent ) {
		Com_sprintf( checkname, MAX_OSPATH, "screenshots/%s.tga", ri.Cmd_Argv( 1 ) );
	} else {
		if ( lastNumber == -1 ) {
			lastNumber = 0;
		}
		for ( ; lastNumber <= 9999 ; lastNumber++ ) {
			R_ScreenshotFilename( lastNumber, checkname );
			if ( !ri.FS_FileExists( checkname ) ) {
				break;
			}
		}
		if ( lastNumber >= 9999 ) {
			ri.Printf( PRINT_ALL, "ScreenShot: Couldn't create a file\n" );
			return;
		}
		lastNumber++;
	}

	R_TakeScreenshot( 0, 0, glConfig.vidWidth, glConfig.vidHeight, checkname, qfalse );

	if ( !silent ) {
		ri.Printf( PRINT_ALL, "Wrote %s\n", checkname );
	}
}

// ============================================================================
// FUNCTION 14/28: R_ScreenShotJPEG_f — REAL
// ============================================================================
void R_ScreenShotJPEG_f( void ) {
	char checkname[MAX_OSPATH];
	static int lastNumber = -1;
	qboolean silent;

	if ( !strcmp( ri.Cmd_Argv( 1 ), "levelshot" ) ) {
		R_LevelShot();
		return;
	}

	if ( !strcmp( ri.Cmd_Argv( 1 ), "silent" ) ) {
		silent = qtrue;
	} else {
		silent = qfalse;
	}

	if ( ri.Cmd_Argc() == 2 && !silent ) {
		Com_sprintf( checkname, MAX_OSPATH, "screenshots/%s.jpg", ri.Cmd_Argv( 1 ) );
	} else {
		if ( lastNumber == -1 ) {
			lastNumber = 0;
		}
		for ( ; lastNumber <= 9999 ; lastNumber++ ) {
			R_ScreenshotFilenameJPEG( lastNumber, checkname );
			if ( !ri.FS_FileExists( checkname ) ) {
				break;
			}
		}
		if ( lastNumber == 10000 ) {
			ri.Printf( PRINT_ALL, "ScreenShot: Couldn't create a file\n" );
			return;
		}
		lastNumber++;
	}

	R_TakeScreenshot( 0, 0, glConfig.vidWidth, glConfig.vidHeight, checkname, qtrue );

	if ( !silent ) {
		ri.Printf( PRINT_ALL, "Wrote %s\n", checkname );
	}
}

// ============================================================================
// FUNCTION 15/28: R_ExportCubemaps — REAL
// Uses R_GetCommandBuffer (sibling dep); no GL.  GL2 ref: tr_init.c:879
// ============================================================================
void R_ExportCubemaps( void ) {
	exportCubemapsCommand_t *cmd;

	cmd = (exportCubemapsCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_EXPORT_CUBEMAPS;
}

// ============================================================================
// FUNCTION 16/28: R_ExportCubemaps_f — REAL
// ============================================================================
void R_ExportCubemaps_f( void ) {
	R_ExportCubemaps();
}

// ============================================================================
// FUNCTION 17/28: RB_TakeVideoFrameCmd
// ============================================================================
const void *RB_TakeVideoFrameCmd( const void *data ) {
	const videoFrameCommand_t *cmd;
	byte *allbuf, *cBuf;
	size_t memcount, avilinelen;
	int avipadwidth, avipadlen, padlen;
	size_t offset = 0;
	int linelen;

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	cmd = (const videoFrameCommand_t *)data;

	allbuf = RB_ReadPixels( 0, 0, cmd->width, cmd->height, &offset, &padlen );
	if ( !allbuf ) {
		return (const void *)( cmd + 1 );
	}
	cBuf = allbuf + offset;

	linelen = cmd->width * 3;
	memcount = ( linelen + padlen ) * cmd->height;
	avilinelen = cmd->width * 3;

	// AVI line padding
	avipadwidth = PAD( avilinelen, AVI_LINE_PADDING );
	avipadlen = avipadwidth - avilinelen;

	// gamma correct
	if ( glConfig.deviceSupportsGamma ) {
		R_GammaCorrect( cBuf, memcount );
	}

	if ( cmd->motionJpeg ) {
		memcount = RE_SaveJPGToBuffer( cmd->encodeBuffer, avilinelen * cmd->height,
									   r_aviMotionJpegQuality->integer,
									   cmd->width, cmd->height, cBuf, padlen );
		ri.CL_WriteAVIVideoFrame( cmd->encodeBuffer, memcount );
	} else
	{
		byte *lineend, *memend;
		byte *srcptr, *destptr;

		srcptr = cBuf;
		destptr = cmd->encodeBuffer;
		memend = srcptr + memcount;

		// swap R and B and remove line paddings
		while ( srcptr < memend )
		{
			lineend = srcptr + linelen;
			while ( srcptr < lineend )
			{
				*destptr++ = srcptr[2];
				*destptr++ = srcptr[1];
				*destptr++ = srcptr[0];
				srcptr += 3;
			}

			Com_Memset( destptr, '\0', avipadlen );
			destptr += avipadlen;

			srcptr += padlen;
		}

		ri.CL_WriteAVIVideoFrame( cmd->encodeBuffer, avipadwidth * cmd->height );
	}

	ri.Hunk_FreeTempMemory( allbuf );

	return (const void *)( cmd + 1 );
}

// ============================================================================
// FUNCTION 18/28: GL_SetDefaultState
// PARITY-SEAM(Metal-device): real MTL pipeline state (MTLRenderPipelineDescriptor,
//   MTLDepthStencilDescriptor, etc.) lands in Phase 3/4.  For now: populate
//   glState struct fields that are queried by the rest of the renderer — these
//   assignments are real.  All qgl* calls are omitted (no GL context).
// ============================================================================
void GL_SetDefaultState( void ) {
	// Populate glState — these fields are read by sibling renderer code.
	glState.glStateBits         = GLS_DEPTHTEST_DISABLE | GLS_DEPTHMASK_TRUE;
	glState.storedGlState        = 0;
	glState.faceCulling          = CT_TWO_SIDED;
	glState.faceCullFront        = qtrue;
	glState.currentVao           = NULL;
	glState.vertexAttribsEnabled = 0;

	// PARITY-SEAM(Metal-device): qgl* state calls omitted — no GL context.
	// Equivalents land when Metal PSO descriptors are built (Phase 3/4):
	//   qglClearDepth(1.0f)          → MTLRenderPassDescriptor.depthAttachment.clearDepth
	//   qglCullFace(GL_FRONT)        → MTLRenderPipelineDescriptor.cullMode
	//   GL_BindNullTextures()        → Metal: no global texture binding
	//   GL_BindNullFramebuffers()    → Metal: no FBO concept (MTLRenderPassDescriptor)
	// GL2 ref: tr_init.c:1027 — sets gl_filter_min/gl_filter_max from r_textureMode cvar at startup.
	GL_TextureMode( r_textureMode->string );
	//   qglDepthFunc(GL_LEQUAL)      → MTLDepthStencilDescriptor.depthCompareFunction
	//   GL_BindNullProgram()         → Metal: no global shader binding
	//   qglPolygonMode(GL_FILL)      → Metal: always fill (no wireframe GL_LINE)
	//   qglDepthMask(GL_TRUE)        → MTLDepthStencilDescriptor.depthWriteEnabled
	//   qglDisable/Enable(...)       → per-PSO flags in Phase 3
	//   qglPolygonOffset(...)        → MTLRenderCommandEncoder.setDepthBias
	//   qglClearColor(...)           → MTLRenderPassDescriptor.colorAttachments[0].clearColor
}

// ============================================================================
// FUNCTION 19/28: R_PrintLongString — REAL
// Workaround for ri.Printf 1024-char limit; no GL.  GL2 ref: tr_init.c:1073
// ============================================================================
void R_PrintLongString( const char *string ) {
	char buffer[1024];
	const char *p;
	int size = strlen( string );

	p = string;
	while ( size > 0 )
	{
		Q_strncpyz( buffer, p, sizeof( buffer ) );
		ri.Printf( PRINT_ALL, "%s", buffer );
		p += 1023;
		size -= 1023;
	}
}

// ============================================================================
// FUNCTION 20/28: GfxInfo_f
// PARITY-SEAM: glConfig fields printed (real); GL extension listing replaced
//   with Metal note; qglLockArraysEXT ref replaced with "n/a (Metal)".
// ============================================================================
static void GfxInfo_f( void ) {
	const char *enablestrings[] =
	{
		"disabled",
		"enabled"
	};
	const char *fsstrings[] =
	{
		"windowed",
		"fullscreen"
	};

	ri.Printf( PRINT_ALL, "\nGL_VENDOR: %s\n", glConfig.vendor_string );
	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", glConfig.renderer_string );
	ri.Printf( PRINT_ALL, "GL_VERSION: %s\n", glConfig.version_string );
	// PARITY-SEAM(Metal-device): GL extension listing via qglGetStringi /
	// qglGetString not available under Metal — capabilities are queried via
	// MTLDevice feature sets (Phase 4).  GL2 ref: tr_init.c:1111-1125
	ri.Printf( PRINT_ALL, "GL_EXTENSIONS: (Metal renderer — see MTLDevice featureSet)\n" );
	ri.Printf( PRINT_ALL, "GL_MAX_TEXTURE_SIZE: %d\n", glConfig.maxTextureSize );
	ri.Printf( PRINT_ALL, "GL_MAX_TEXTURE_IMAGE_UNITS: %d\n", glConfig.numTextureUnits );
	ri.Printf( PRINT_ALL, "\nPIXELFORMAT: color(%d-bits) Z(%d-bit) stencil(%d-bits)\n", glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits );
	ri.Printf( PRINT_ALL, "MODE: %d, %d x %d %s hz:", r_mode->integer, glConfig.vidWidth, glConfig.vidHeight, fsstrings[r_fullscreen->integer == 1] );
	if ( glConfig.displayFrequency ) {
		ri.Printf( PRINT_ALL, "%d\n", glConfig.displayFrequency );
	} else
	{
		ri.Printf( PRINT_ALL, "N/A\n" );
	}
	if ( glConfig.deviceSupportsGamma ) {
		ri.Printf( PRINT_ALL, "GAMMA: hardware w/ %d overbright bits\n", tr.overbrightBits );
	} else
	{
		ri.Printf( PRINT_ALL, "GAMMA: software w/ %d overbright bits\n", tr.overbrightBits );
	}


	ri.Printf( PRINT_ALL, "texturemode: %s\n", r_textureMode->string );
	ri.Printf( PRINT_ALL, "picmip: %d\n", r_picmip->integer );
	ri.Printf( PRINT_ALL, "texture bits: %d\n", r_texturebits->integer );
	// PARITY-SEAM(Metal-device): qglLockArraysEXT not available under Metal.
	ri.Printf( PRINT_ALL, "compiled vertex arrays: n/a (Metal)\n" );
	ri.Printf( PRINT_ALL, "texenv add: %s\n", enablestrings[glConfig.textureEnvAddAvailable != 0] );
	ri.Printf( PRINT_ALL, "compressed textures: %s\n", enablestrings[glConfig.textureCompression != TC_NONE] );

	ri.Printf( PRINT_ALL, "NV distance fog: %s\n", enablestrings[glConfig.NVFogAvailable != 0] );
	if ( glConfig.NVFogAvailable ) {
		ri.Printf( PRINT_ALL, "Fog Mode: %s\n", r_nv_fogdist_mode->string );
	}

	if ( r_vertexLight->integer || glConfig.hardwareType == GLHW_PERMEDIA2 ) {
		ri.Printf( PRINT_ALL, "HACK: using vertex lightmap approximation\n" );
	}
	if ( glConfig.hardwareType == GLHW_RAGEPRO ) {
		ri.Printf( PRINT_ALL, "HACK: ragePro approximations\n" );
	}
	if ( glConfig.hardwareType == GLHW_RIVA128 ) {
		ri.Printf( PRINT_ALL, "HACK: riva128 approximations\n" );
	}
	if ( r_finish->integer ) {
		ri.Printf( PRINT_ALL, "Forcing glFinish\n" );
	}
}

// ============================================================================
// FUNCTION 21/28: GfxMemInfo_f
// PARITY-STUB: no-op under Metal — GL_NVX_gpu_memory_info and GL_ATI_meminfo
//   are OpenGL extensions.  Metal memory info is via MTLDevice.currentAllocatedSize
//   etc. (Phase 4).
// ============================================================================
static void GfxMemInfo_f( void ) {
	// PARITY-STUB: no-op under Metal — GL memory extension queries (MI_NVX,
	// MI_ATI) not applicable.  GL2 ref: tr_init.c:1176
	ri.Printf( PRINT_ALL, "GfxMemInfo: Metal renderer — GL memory extensions not applicable.\n" );
}

// ============================================================================
// FUNCTION 22/28: R_Register — REAL
// Pure cvar registration + command binding; no GL.
// GLimp_Minimize command binding replaced with seam comment (GLimp_Minimize
// stub provided above; the command is still registered so "minimize" works
// once the real Phase-4 implementation arrives).
// ============================================================================
void R_Register( void ) {
	#ifdef USE_RENDERER_DLOPEN
	com_altivec = ri.Cvar_Get( "com_altivec", "1", CVAR_ARCHIVE );
	#endif

	//
	// latched and archived variables
	//
	r_allowExtensions = ri.Cvar_Get( "r_allowExtensions", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_compressed_textures = ri.Cvar_Get( "r_ext_compressed_textures", "0", CVAR_ARCHIVE | CVAR_LATCH );   // (SA) ew, a spelling change I missed from the missionpack
	r_ext_multitexture = ri.Cvar_Get( "r_ext_multitexture", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_compiled_vertex_array = ri.Cvar_Get( "r_ext_compiled_vertex_array", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_glIgnoreWicked3D = ri.Cvar_Get( "r_glIgnoreWicked3D", "0", CVAR_ARCHIVE | CVAR_LATCH );

//----(SA)	added
	r_ext_ATI_pntriangles           = ri.Cvar_Get( "r_ext_ATI_pntriangles", "0", CVAR_ARCHIVE | CVAR_LATCH );   //----(SA)	default to '0'
	r_ati_truform_tess              = ri.Cvar_Get( "r_ati_truform_tess", "1", CVAR_ARCHIVE );
	r_ati_truform_normalmode        = ri.Cvar_Get( "r_ati_truform_normalmode", "GL_PN_TRIANGLES_NORMAL_MODE_LINEAR", CVAR_ARCHIVE );
	r_ati_truform_pointmode         = ri.Cvar_Get( "r_ati_truform_pointmode", "GL_PN_TRIANGLES_POINT_MODE_LINEAR", CVAR_ARCHIVE );

	r_ati_fsaa_samples              = ri.Cvar_Get( "r_ati_fsaa_samples", "1", CVAR_ARCHIVE );       //DAJ valids are 1, 2, 4

	r_ext_NV_fog_dist                   = ri.Cvar_Get( "r_ext_NV_fog_dist", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_nv_fogdist_mode                   = ri.Cvar_Get( "r_nv_fogdist_mode", "GL_EYE_RADIAL_NV", CVAR_ARCHIVE );    // default to 'looking good'
//----(SA)	end

	r_ext_texture_env_add = ri.Cvar_Get( "r_ext_texture_env_add", "1", CVAR_ARCHIVE | CVAR_LATCH );

	r_ext_framebuffer_object = ri.Cvar_Get( "r_ext_framebuffer_object", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_texture_float = ri.Cvar_Get( "r_ext_texture_float", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_framebuffer_multisample = ri.Cvar_Get( "r_ext_framebuffer_multisample", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_arb_seamless_cube_map = ri.Cvar_Get( "r_arb_seamless_cube_map", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_arb_vertex_array_object = ri.Cvar_Get( "r_arb_vertex_array_object", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_direct_state_access = ri.Cvar_Get( "r_ext_direct_state_access", "1", CVAR_ARCHIVE | CVAR_LATCH );

	r_ext_texture_filter_anisotropic = ri.Cvar_Get( "r_ext_texture_filter_anisotropic", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_max_anisotropy = ri.Cvar_Get( "r_ext_max_anisotropy", "2", CVAR_ARCHIVE | CVAR_LATCH );

	r_picmip = ri.Cvar_Get( "r_picmip", "1", CVAR_ARCHIVE | CVAR_LATCH ); //----(SA)	mod for DM and DK for id build.  was "1" // JPW NERVE pushed back to 1
	r_roundImagesDown = ri.Cvar_Get( "r_roundImagesDown", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_rmse = ri.Cvar_Get( "r_rmse", "0.0", CVAR_ARCHIVE | CVAR_LATCH );
	r_colorMipLevels = ri.Cvar_Get( "r_colorMipLevels", "0", CVAR_LATCH );
	ri.Cvar_CheckRange( r_picmip, 0, 16, qtrue );
	r_detailTextures = ri.Cvar_Get( "r_detailtextures", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_texturebits = ri.Cvar_Get( "r_texturebits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_colorbits = ri.Cvar_Get( "r_colorbits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_stencilbits = ri.Cvar_Get( "r_stencilbits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_depthbits = ri.Cvar_Get( "r_depthbits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ext_multisample = ri.Cvar_Get( "r_ext_multisample", "0", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_CheckRange( r_ext_multisample, 0, 4, qtrue );
	r_overBrightBits = ri.Cvar_Get( "r_overBrightBits", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ignorehwgamma = ri.Cvar_Get( "r_ignorehwgamma", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_mode = ri.Cvar_Get( "r_mode", "3", CVAR_ARCHIVE | CVAR_LATCH );
	r_fullscreen = ri.Cvar_Get( "r_fullscreen", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_noborder = ri.Cvar_Get( "r_noborder", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_customwidth = ri.Cvar_Get( "r_customwidth", "1600", CVAR_ARCHIVE | CVAR_LATCH );
	r_customheight = ri.Cvar_Get( "r_customheight", "1024", CVAR_ARCHIVE | CVAR_LATCH );
	r_customPixelAspect = ri.Cvar_Get( "r_customPixelAspect", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_simpleMipMaps = ri.Cvar_Get( "r_simpleMipMaps", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_vertexLight = ri.Cvar_Get( "r_vertexLight", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_uiFullScreen = ri.Cvar_Get( "r_uifullscreen", "0", 0 );
	r_subdivisions = ri.Cvar_Get( "r_subdivisions", "4", CVAR_ARCHIVE | CVAR_LATCH );
	r_stereoEnabled = ri.Cvar_Get( "r_stereoEnabled", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_greyscale = ri.Cvar_Get( "r_greyscale", "0", CVAR_ARCHIVE | CVAR_LATCH );
	ri.Cvar_CheckRange( r_greyscale, 0, 1, qfalse );

	r_externalGLSL = ri.Cvar_Get( "r_externalGLSL", "0", CVAR_LATCH );

	r_hdr = ri.Cvar_Get( "r_hdr", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_floatLightmap = ri.Cvar_Get( "r_floatLightmap", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_postProcess = ri.Cvar_Get( "r_postProcess", "1", CVAR_ARCHIVE );
	// SLICE-B ACCEPTANCE HOOK cvar (temporary).
	r_metalBlitTest = ri.Cvar_Get( "r_metalBlitTest", "0", CVAR_TEMP );

	r_toneMap = ri.Cvar_Get( "r_toneMap", "1", CVAR_ARCHIVE );
	r_forceToneMap = ri.Cvar_Get( "r_forceToneMap", "0", CVAR_CHEAT );
	r_forceToneMapMin = ri.Cvar_Get( "r_forceToneMapMin", "-8.0", CVAR_CHEAT );
	r_forceToneMapAvg = ri.Cvar_Get( "r_forceToneMapAvg", "-2.0", CVAR_CHEAT );
	r_forceToneMapMax = ri.Cvar_Get( "r_forceToneMapMax", "0.0", CVAR_CHEAT );

	r_autoExposure = ri.Cvar_Get( "r_autoExposure", "1", CVAR_ARCHIVE );
	r_forceAutoExposure = ri.Cvar_Get( "r_forceAutoExposure", "0", CVAR_CHEAT );
	r_forceAutoExposureMin = ri.Cvar_Get( "r_forceAutoExposureMin", "-2.0", CVAR_CHEAT );
	r_forceAutoExposureMax = ri.Cvar_Get( "r_forceAutoExposureMax", "2.0", CVAR_CHEAT );
	r_debugAutoExposure = ri.Cvar_Get( "r_debugAutoExposure", "0", CVAR_CHEAT ); // Metal-only debug cvar; no GL2 equivalent. GL2 ref: tr_init.c:1288-1296 (CVAR_CHEAT convention for r_force* debug cvars).

	r_cameraExposure = ri.Cvar_Get( "r_cameraExposure", "1", CVAR_CHEAT );

	r_depthPrepass = ri.Cvar_Get( "r_depthPrepass", "1", CVAR_ARCHIVE );
	r_ssao = ri.Cvar_Get( "r_ssao", "0", CVAR_LATCH | CVAR_ARCHIVE );

	r_normalMapping = ri.Cvar_Get( "r_normalMapping", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_specularMapping = ri.Cvar_Get( "r_specularMapping", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_deluxeMapping = ri.Cvar_Get( "r_deluxeMapping", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_parallaxMapping = ri.Cvar_Get( "r_parallaxMapping", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_parallaxMapOffset = ri.Cvar_Get( "r_parallaxMapOffset", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_parallaxMapShadows = ri.Cvar_Get( "r_parallaxMapShadows", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_cubeMapping = ri.Cvar_Get( "r_cubeMapping", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_cubemapSize = ri.Cvar_Get( "r_cubemapSize", "128", CVAR_ARCHIVE | CVAR_LATCH );
	r_deluxeSpecular = ri.Cvar_Get( "r_deluxeSpecular", "0.1", CVAR_ARCHIVE | CVAR_LATCH );
	r_pbr = ri.Cvar_Get( "r_pbr", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_baseNormalX = ri.Cvar_Get( "r_baseNormalX", "1.0", CVAR_ARCHIVE | CVAR_LATCH );
	r_baseNormalY = ri.Cvar_Get( "r_baseNormalY", "1.0", CVAR_ARCHIVE | CVAR_LATCH );
	r_baseParallax = ri.Cvar_Get( "r_baseParallax", "0.05", CVAR_ARCHIVE | CVAR_LATCH );
	r_baseSpecular = ri.Cvar_Get( "r_baseSpecular", "0.04", CVAR_ARCHIVE | CVAR_LATCH );
	r_baseGloss = ri.Cvar_Get( "r_baseGloss", "0.1", CVAR_ARCHIVE | CVAR_LATCH );
	r_glossType = ri.Cvar_Get( "r_glossType", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_dlightMode = ri.Cvar_Get( "r_dlightMode", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_pshadowDist = ri.Cvar_Get( "r_pshadowDist", "128", CVAR_ARCHIVE );
	r_mergeLightmaps = ri.Cvar_Get( "r_mergeLightmaps", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_imageUpsample = ri.Cvar_Get( "r_imageUpsample", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_imageUpsampleMaxSize = ri.Cvar_Get( "r_imageUpsampleMaxSize", "1024", CVAR_ARCHIVE | CVAR_LATCH );
	r_imageUpsampleType = ri.Cvar_Get( "r_imageUpsampleType", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_genNormalMaps = ri.Cvar_Get( "r_genNormalMaps", "0", CVAR_ARCHIVE | CVAR_LATCH );

	r_forceSun = ri.Cvar_Get( "r_forceSun", "0", CVAR_ARCHIVE );
	r_forceSunLightScale = ri.Cvar_Get( "r_forceSunLightScale", "1.0", CVAR_CHEAT );
	r_forceSunAmbientScale = ri.Cvar_Get( "r_forceSunAmbientScale", "0.5", CVAR_CHEAT );
	r_drawSunRays = ri.Cvar_Get( "r_drawSunRays", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_sunlightMode = ri.Cvar_Get( "r_sunlightMode", "1", CVAR_ARCHIVE | CVAR_LATCH );

	r_sunShadows = ri.Cvar_Get( "r_sunShadows", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowFilter = ri.Cvar_Get( "r_shadowFilter", "1", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowBlur = ri.Cvar_Get( "r_shadowBlur", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowMapSize = ri.Cvar_Get( "r_shadowMapSize", "1024", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowCascadeZNear = ri.Cvar_Get( "r_shadowCascadeZNear", "8", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowCascadeZFar = ri.Cvar_Get( "r_shadowCascadeZFar", "1024", CVAR_ARCHIVE | CVAR_LATCH );
	r_shadowCascadeZBias = ri.Cvar_Get( "r_shadowCascadeZBias", "0", CVAR_ARCHIVE | CVAR_LATCH );
	r_ignoreDstAlpha = ri.Cvar_Get( "r_ignoreDstAlpha", "1", CVAR_ARCHIVE | CVAR_LATCH );

	//
	// temporary latched variables that can only change over a restart
	//
	r_displayRefresh = ri.Cvar_Get( "r_displayRefresh", "0", CVAR_LATCH );
	ri.Cvar_CheckRange( r_displayRefresh, 0, 200, qtrue );
	r_fullbright = ri.Cvar_Get( "r_fullbright", "0", CVAR_LATCH ); // JPW NERVE removed per atvi request
	r_mapOverBrightBits = ri.Cvar_Get( "r_mapOverBrightBits", "2", CVAR_LATCH );
	r_intensity = ri.Cvar_Get( "r_intensity", "1", CVAR_LATCH );
	r_singleShader = ri.Cvar_Get( "r_singleShader", "0", CVAR_CHEAT | CVAR_LATCH );

	//
	// archived variables that can change at any time
	//
	r_lodCurveError = ri.Cvar_Get( "r_lodCurveError", "250", CVAR_ARCHIVE );
	r_lodbias = ri.Cvar_Get( "r_lodbias", "0", CVAR_ARCHIVE );
	r_flares = ri.Cvar_Get( "r_flares", "1", CVAR_ARCHIVE );
	r_znear = ri.Cvar_Get( "r_znear", "4", CVAR_CHEAT );
	ri.Cvar_CheckRange( r_znear, 0.001f, 200, qfalse );
	r_zproj = ri.Cvar_Get( "r_zproj", "64", CVAR_ARCHIVE );
	r_stereoSeparation = ri.Cvar_Get( "r_stereoSeparation", "64", CVAR_ARCHIVE );
//----(SA)	added
	r_zfar = ri.Cvar_Get( "r_zfar", "0", CVAR_CHEAT );
//----(SA)	end
	r_ignoreGLErrors = ri.Cvar_Get( "r_ignoreGLErrors", "1", CVAR_ARCHIVE );
	r_fastsky = ri.Cvar_Get( "r_fastsky", "0", CVAR_ARCHIVE );
	r_inGameVideo = ri.Cvar_Get( "r_inGameVideo", "1", CVAR_ARCHIVE );
	r_drawSun = ri.Cvar_Get( "r_drawSun", "1", CVAR_ARCHIVE );
	r_dynamiclight = ri.Cvar_Get( "r_dynamiclight", "1", CVAR_ARCHIVE );
	r_dlightBacks = ri.Cvar_Get( "r_dlightBacks", "1", CVAR_ARCHIVE );
	r_finish = ri.Cvar_Get( "r_finish", "0", CVAR_ARCHIVE );
	r_textureMode = ri.Cvar_Get( "r_textureMode", "GL_LINEAR_MIPMAP_LINEAR", CVAR_ARCHIVE );
#ifdef __EMSCRIPTEN__
	r_swapInterval = ri.Cvar_Get( "r_swapInterval", "1",
								  CVAR_ARCHIVE | CVAR_LATCH );
#else
	r_swapInterval = ri.Cvar_Get( "r_swapInterval", "0",
								  CVAR_ARCHIVE | CVAR_LATCH );
#endif
	r_gamma = ri.Cvar_Get( "r_gamma", "1", CVAR_ARCHIVE );
	r_facePlaneCull = ri.Cvar_Get( "r_facePlaneCull", "1", CVAR_ARCHIVE );

	r_railWidth = ri.Cvar_Get( "r_railWidth", "16", CVAR_ARCHIVE );
	r_railCoreWidth = ri.Cvar_Get( "r_railCoreWidth", "1", CVAR_ARCHIVE );
	r_railSegmentLength = ri.Cvar_Get( "r_railSegmentLength", "32", CVAR_ARCHIVE );

	r_ambientScale = ri.Cvar_Get( "r_ambientScale", "0.5", CVAR_CHEAT );
	r_directedScale = ri.Cvar_Get( "r_directedScale", "1", CVAR_CHEAT );

	r_anaglyphMode = ri.Cvar_Get( "r_anaglyphMode", "0", CVAR_ARCHIVE );

	//
	// temporary variables that can change at any time
	//
	r_showImages = ri.Cvar_Get( "r_showImages", "0", CVAR_TEMP );

	r_debugLight = ri.Cvar_Get( "r_debuglight", "0", CVAR_TEMP );
	r_debugSort = ri.Cvar_Get( "r_debugSort", "0", CVAR_CHEAT );
	r_printShaders = ri.Cvar_Get( "r_printShaders", "0", 0 );
	r_saveFontData = ri.Cvar_Get( "r_saveFontData", "0", 0 );

	// Ridah
	r_compressModels = ri.Cvar_Get( "r_compressModels", "0", 0 );     // converts MD3 -> MDC at run-time
	r_exportCompressedModels = ri.Cvar_Get( "r_exportCompressedModels", "0", 0 ); // saves compressed models
	r_buildScript = ri.Cvar_Get( "com_buildscript", "0", 0 );
	r_bonesDebug = ri.Cvar_Get( "r_bonesDebug", "0", CVAR_CHEAT );
	// done.

	// Rafael - wolf fog
	r_wolffog = ri.Cvar_Get( "r_wolffog", "1", CVAR_CHEAT ); // JPW NERVE cheat protected per id request
	// done

	r_nocurves = ri.Cvar_Get( "r_nocurves", "0", CVAR_CHEAT );
	r_drawworld = ri.Cvar_Get( "r_drawworld", "1", CVAR_CHEAT );
	r_lightmap = ri.Cvar_Get( "r_lightmap", "0", CVAR_CHEAT ); // DHM - NERVE :: cheat protect
	r_portalOnly = ri.Cvar_Get( "r_portalOnly", "0", CVAR_CHEAT );

	r_flareSize = ri.Cvar_Get( "r_flareSize", "40", CVAR_CHEAT );
	ri.Cvar_Set( "r_flareFade", "5" ); // to force this when people already have "7" in their config
	r_flareFade = ri.Cvar_Get( "r_flareFade", "5", CVAR_CHEAT );
	r_flareCoeff = ri.Cvar_Get( "r_flareCoeff", FLARE_STDCOEFF, CVAR_CHEAT );

	r_skipBackEnd = ri.Cvar_Get( "r_skipBackEnd", "0", CVAR_CHEAT );

	r_measureOverdraw = ri.Cvar_Get( "r_measureOverdraw", "0", CVAR_CHEAT );
	r_lodscale = ri.Cvar_Get( "r_lodscale", "5", CVAR_CHEAT );
	r_norefresh = ri.Cvar_Get( "r_norefresh", "0", CVAR_CHEAT );
	r_drawentities = ri.Cvar_Get( "r_drawentities", "1", CVAR_CHEAT );
	r_ignore = ri.Cvar_Get( "r_ignore", "1", CVAR_CHEAT );
	r_nocull = ri.Cvar_Get( "r_nocull", "0", CVAR_CHEAT );
	r_novis = ri.Cvar_Get( "r_novis", "0", CVAR_CHEAT );
	r_showcluster = ri.Cvar_Get( "r_showcluster", "0", CVAR_CHEAT );
	r_speeds = ri.Cvar_Get( "r_speeds", "0", CVAR_CHEAT );
	r_verbose = ri.Cvar_Get( "r_verbose", "0", CVAR_CHEAT );
	r_logFile = ri.Cvar_Get( "r_logFile", "0", CVAR_CHEAT );
	r_debugSurface = ri.Cvar_Get( "r_debugSurface", "0", CVAR_CHEAT );
	r_nobind = ri.Cvar_Get( "r_nobind", "0", CVAR_CHEAT );
	r_showtris = ri.Cvar_Get( "r_showtris", "0", CVAR_CHEAT );
	r_showsky = ri.Cvar_Get( "r_showsky", "0", CVAR_CHEAT );
	r_shownormals = ri.Cvar_Get( "r_shownormals", "0", CVAR_CHEAT );
	r_clear = ri.Cvar_Get( "r_clear", "0", CVAR_CHEAT );
	r_offsetFactor = ri.Cvar_Get( "r_offsetfactor", "-1", CVAR_CHEAT );
	r_offsetUnits = ri.Cvar_Get( "r_offsetunits", "-2", CVAR_CHEAT );
	r_drawBuffer = ri.Cvar_Get( "r_drawBuffer", "GL_BACK", CVAR_CHEAT );
	r_lockpvs = ri.Cvar_Get( "r_lockpvs", "0", CVAR_CHEAT );
	r_noportals = ri.Cvar_Get( "r_noportals", "0", CVAR_CHEAT );
	r_shadows = ri.Cvar_Get( "cg_shadows", "1", 0 );
	r_portalsky = ri.Cvar_Get( "cg_skybox", "1", 0 );

	r_marksOnTriangleMeshes = ri.Cvar_Get( "r_marksOnTriangleMeshes", "0", CVAR_ARCHIVE );

	r_vaoCache = ri.Cvar_Get( "r_vaoCache", "0", CVAR_ARCHIVE );
	// GPU vertex animation of MD3 models; 0 falls back to the CPU lerp into tess
	// (kept as a comparison / fallback switch, deliberately not archived).
	r_metalGpuMd3 = ri.Cvar_Get( "r_metalGpuMd3", "1", 0 );

	r_aviMotionJpegQuality = ri.Cvar_Get( "r_aviMotionJpegQuality", "90", CVAR_ARCHIVE );
	r_screenshotJpegQuality = ri.Cvar_Get( "r_screenshotJpegQuality", "90", CVAR_ARCHIVE );

	r_maxpolys = ri.Cvar_Get( "r_maxpolys", va( "%d", MAX_POLYS ), 0 );
	r_maxpolyverts = ri.Cvar_Get( "r_maxpolyverts", va( "%d", MAX_POLYVERTS ), 0 );

	r_highQualityVideo = ri.Cvar_Get( "r_highQualityVideo", "1", CVAR_ARCHIVE );
	// GL2 ref: tr_init.c:1448-1458 — commands registered here must be removed in RE_Shutdown
	ri.Cmd_AddCommand( "imagelist",        R_ImageList_f );
	ri.Cmd_AddCommand( "shaderlist",       R_ShaderList_f );
	ri.Cmd_AddCommand( "skinlist",         R_SkinList_f );
	ri.Cmd_AddCommand( "modellist",        R_Modellist_f );
	ri.Cmd_AddCommand( "modelist",         R_ModeList_f );
	ri.Cmd_AddCommand( "screenshot",       R_ScreenShot_f );
	ri.Cmd_AddCommand( "screenshotJPEG",   R_ScreenShotJPEG_f );
	ri.Cmd_AddCommand( "gfxinfo",          GfxInfo_f );
	// PARITY-SEAM(Metal-device): GLimp_Minimize stub provided above; command
	// registered so "minimize" is valid once Phase-4 implements it.
	ri.Cmd_AddCommand( "minimize",         GLimp_Minimize );
	ri.Cmd_AddCommand( "gfxmeminfo",       GfxMemInfo_f );
	ri.Cmd_AddCommand( "exportCubemaps",   R_ExportCubemaps_f );
	ri.Cmd_AddCommand( "taginfo",          R_TagInfo_f );

	// Ridah
	{
		void R_CropImages_f( void );
		ri.Cmd_AddCommand( "cropimages", R_CropImages_f );
	}
	// done.
}

// ============================================================================
// FUNCTION 23/28: R_InitQueries
// PARITY-STUB: no-op under Metal — GL occlusion queries (qglGenQueries) have
//   no Metal equivalent.  Metal occlusion testing uses visibility result buffers
//   (MTLRenderCommandEncoder setVisibilityResultMode) — Phase 4.
// ============================================================================
void R_InitQueries( void ) {
	// PARITY-STUB: no-op under Metal — GL occlusion queries not applicable.
}

// ============================================================================
// FUNCTION 24/28: R_ShutDownQueries
// PARITY-STUB: no-op under Metal — GL occlusion queries not applicable.
// ============================================================================
void R_ShutDownQueries( void ) {
	// PARITY-STUB: no-op under Metal — GL occlusion queries not applicable.
}

// ============================================================================
// FUNCTION 25/28: R_Init — REAL (with seams for GL-specific calls)
// Function table init, cvar registration, hunk allocation: REAL.
// InitOpenGL call: PARITY-SEAM (Metal device bring-up seam).
// R_InitImages / FBO_Init / GLSL_InitGPUShaders / etc.: sibling Phase-1 deps.
// qglGetError at end: PARITY-SEAM (no GL error query under Metal).
// ============================================================================
void R_Init( void ) {
	int i;
	byte *ptr;

	ri.Printf( PRINT_ALL, "----- R_Init -----\n" );

	// clear all our internal state
	memset( &tr, 0, sizeof( tr ) );
	memset( &backEnd, 0, sizeof( backEnd ) );
	memset( &tess, 0, sizeof( tess ) );

	if ( sizeof( glconfig_t ) != 7268 ) {
		ri.Error( ERR_FATAL, "Mod ABI incompatible: sizeof(glconfig_t) == %u != 7268",
				  (unsigned int) sizeof( glconfig_t ) );
	}

	if ( (intptr_t)tess.xyz & 15 ) {
		ri.Printf( PRINT_WARNING, "tess.xyz not 16 byte aligned\n" );
	}

	// init function tables — GL2 ref: tr_init.c:1509-1531
	for ( i = 0; i < FUNCTABLE_SIZE; i++ )
	{
		tr.sinTable[i]      = sin( DEG2RAD( i * 360.0f / ( ( float ) ( FUNCTABLE_SIZE - 1 ) ) ) );
		tr.squareTable[i]   = ( i < FUNCTABLE_SIZE / 2 ) ? 1.0f : -1.0f;
		tr.sawToothTable[i] = (float)i / FUNCTABLE_SIZE;
		tr.inverseSawToothTable[i] = 1.0f - tr.sawToothTable[i];

		if ( i < FUNCTABLE_SIZE / 2 ) {
			if ( i < FUNCTABLE_SIZE / 4 ) {
				tr.triangleTable[i] = ( float ) i / ( FUNCTABLE_SIZE / 4 );
			} else
			{
				tr.triangleTable[i] = 1.0f - tr.triangleTable[i - FUNCTABLE_SIZE / 4];
			}
		} else
		{
			tr.triangleTable[i] = -tr.triangleTable[i - FUNCTABLE_SIZE / 2];
		}
	}

	// Ridah, init the virtual memory
	R_Hunk_Begin();

	R_InitFogTable();

	R_NoiseInit();

	R_Register();

	// M5.4 A1 scope limiter — unimplemented features suppressed at startup.
	// r_toneMap limiter removed: tonemap chain is wired (Lambert Wave 2 + Ripley Wave 2 reconcile).
	// GL2 ref: tr_init.c:1387 (no forced-zero overrides in GL2 init path)
	// r_ssao un-gated: T5 wires GLMetal_DispatchSAO generation in tr_backend.cpp.
	// Apple-SAO compute path replaces GL2's ssao_fp.glsl fragment-shader pass.
	// Parity deviation logged: .squad/decisions/inbox/dallas-ssao-deviation-decision.md.
	// GL2 ref: tr_init.c:1387 — no forced-zero for r_ssao in GL2.
	if ( r_drawSunRays->integer ) {
		ri.Cvar_Set( "r_drawSunRays", "0" );
	}
	if ( r_cubeMapping->integer ) {
		ri.Cvar_Set( "r_cubeMapping", "0" );
	}
	if ( r_shadowBlur->integer ) {
		ri.Cvar_Set( "r_shadowBlur", "0" );
	}
	if ( r_ext_framebuffer_multisample->integer ) {
		ri.Cvar_Set( "r_ext_framebuffer_multisample", "0" );
	}

	max_polys = r_maxpolys->integer;
	if ( max_polys < MAX_POLYS ) {
		max_polys = MAX_POLYS;
	}

	max_polyverts = r_maxpolyverts->integer;
	if ( max_polyverts < MAX_POLYVERTS ) {
		max_polyverts = MAX_POLYVERTS;
	}

	ptr = (byte *)ri.Hunk_Alloc( sizeof( *backEndData ) + sizeof( srfPoly_t ) * max_polys + sizeof( polyVert_t ) * max_polyverts, h_low );
	backEndData = (backEndData_t *) ptr;
	backEndData->polys = (srfPoly_t *) ( (char *) ptr + sizeof( *backEndData ) );
	backEndData->polyVerts = (polyVert_t *) ( (char *) ptr + sizeof( *backEndData ) + sizeof( srfPoly_t ) * max_polys );

	R_InitNextFrame();

	// PARITY-SEAM(Metal-device): InitOpenGL triggers GLimp_Init (seam) and
	// populates glConfig with Metal defaults.  GL2 ref: tr_init.c:1553
	InitOpenGL();

	// Sibling Phase-1 deps — unresolved until Phase 1 completes.
	R_InitImages();

	if ( glRefConfig.framebufferObject ) {
		FBO_Init();
	}

	GLSL_InitGPUShaders();

	R_InitVaos();

	R_InitShaders();

	R_InitSkins();

	R_ModelInit();

	R_InitFreeType();

	R_InitQueries();

	// PARITY-SEAM(Metal-device): qglGetError() not available under Metal.
	// Metal command buffer errors are surfaced via MTLCommandBuffer.error (Phase 4).
	GfxInfo_f();
	ri.Printf( PRINT_ALL, "----- finished R_Init -----\n" );
}

// ============================================================================
// FUNCTION 26/28: RE_Shutdown — REAL (with seam for GLimp_Shutdown)
// Command removal and state zeroing: REAL.
// GLimp_Shutdown: PARITY-SEAM.
// ============================================================================
void RE_Shutdown( qboolean destroyWindow ) {

	ri.Printf( PRINT_ALL, "RE_Shutdown( %i )\n", destroyWindow );

	// GL2 ref: tr_init.c:1593-1603 — must mirror R_Register's Cmd_AddCommand list
	ri.Cmd_RemoveCommand( "imagelist" );
	ri.Cmd_RemoveCommand( "shaderlist" );
	ri.Cmd_RemoveCommand( "skinlist" );
	ri.Cmd_RemoveCommand( "modellist" );
	ri.Cmd_RemoveCommand( "modelist" );
	ri.Cmd_RemoveCommand( "screenshot" );
	ri.Cmd_RemoveCommand( "screenshotJPEG" );
	ri.Cmd_RemoveCommand( "gfxinfo" );
	ri.Cmd_RemoveCommand( "minimize" );
	ri.Cmd_RemoveCommand( "gfxmeminfo" );
	ri.Cmd_RemoveCommand( "exportCubemaps" );
	ri.Cmd_RemoveCommand( "taginfo" );

	// Ridah
	ri.Cmd_RemoveCommand( "cropimages" );
	// done.

	if ( tr.registered ) {
		R_IssuePendingRenderCommands();
		R_ShutDownQueries();
		if ( glRefConfig.framebufferObject ) {
			FBO_Shutdown();
		}
		R_DeleteTextures();
		R_ShutdownVaos();
		GLSL_ShutdownGPUShaders();
	}

	R_DoneFreeType();

	if ( destroyWindow ) {
		// PARITY-SEAM(Metal-device): mirrors GL2's GLimp_Shutdown() call.
		// GLimp_Shutdown (tr_metal_glimp.cpp) performs the REAL teardown:
		// GLMetal_Shutdown (device/queue/semaphore) + IN_Shutdown +
		// SDL_Metal_DestroyView + SDL_DestroyWindow + SDL_QuitSubSystem.
		// This is required for vid_restart correctness: without destroying the
		// old window/Metal view, vid_restart's GLimp_Init creates a duplicate
		// orphaned window and leaks the CAMetalLayer/NSView, which throws an
		// uncaught NSException during later Cocoa teardown (crash on /quit).
		// (The old MTL_Shutdown() stub only released the never-initialized
		// vestigial MTL_Init device/queue — a no-op that leaked the real ones.)
		GLimp_Shutdown();

		Com_Memset( &glConfig,    0, sizeof( glConfig ) );
		Com_Memset( &glRefConfig, 0, sizeof( glRefConfig ) );
		textureFilterAnisotropic = qfalse;
		maxAnisotropy = 0;
		displayAspect = 0.0f;
		haveClampToEdge = qfalse;

		Com_Memset( &glState, 0, sizeof( glState ) );
	}

	tr.registered = qfalse;
}

// ============================================================================
// FUNCTION 27/28: RE_EndRegistration — REAL
// Calls R_IssuePendingRenderCommands (stub in this TU) and RB_ShowImages
// (sibling dep).  GL2 ref: tr_init.c:1643
// ============================================================================
void RE_EndRegistration( void ) {
	R_IssuePendingRenderCommands();
	if ( !ri.Sys_LowPhysicalMemory() ) {
		RB_ShowImages();
	}
}

// ============================================================================
// FUNCTION 28/28: GetRefAPI — REAL
// extern "C" so dlopen symbol is unmangled.  Mirrors GL2 structure exactly.
// ============================================================================
extern "C" Q_EXPORT refexport_t * QDECL GetRefAPI( int apiVersion, refimport_t *rimp ) {
	static refexport_t re;

	ri = *rimp;

	memset( &re, 0, sizeof( re ) );

	if ( apiVersion != REF_API_VERSION ) {
		ri.Printf( PRINT_ALL,
				   "Mismatched REF_API_VERSION: expected %i, got %i\n",
				   REF_API_VERSION, apiVersion );
		return NULL;
	}

	// Wire all refexport_t function pointers.

	re.Shutdown              = RE_Shutdown;

	re.BeginRegistration = RE_BeginRegistration;
	re.RegisterModel    = RE_RegisterModel;
	re.RegisterSkin     = RE_RegisterSkin;
//----(SA) added
	re.GetSkinModel         = RE_GetSkinModel;
	re.GetShaderFromModel   = RE_GetShaderFromModel;
//----(SA) end
	re.RegisterShader   = RE_RegisterShader;
	re.RegisterShaderNoMip = RE_RegisterShaderNoMip;
	re.LoadWorld        = RE_LoadWorldMap;
	re.SetWorldVisData  = RE_SetWorldVisData;
	re.EndRegistration  = RE_EndRegistration;

	re.BeginFrame            = RE_BeginFrame;
	re.EndFrame              = RE_EndFrame;

	re.MarkFragments         = R_MarkFragments;
	re.LerpTag               = R_LerpTag;
	re.ModelBounds           = R_ModelBounds;

	re.ClearScene       = RE_ClearScene;
	re.AddRefEntityToScene = RE_AddRefEntityToScene;
	re.AddPolyToScene   = RE_AddPolyToScene;
	// Ridah
	re.AddPolysToScene  = RE_AddPolysToScene;
	// done.
	re.AddLightToScene  = RE_AddLightToScene;
//----(SA)
	re.AddCoronaToScene = RE_AddCoronaToScene;
	re.SetFog           = R_SetFog;
//----(SA)
	re.RenderScene      = RE_RenderScene;

	re.SetColor         = RE_SetColor;
	re.DrawStretchPic   = RE_StretchPic;
	re.DrawRotatedPic   = RE_RotatedPic;        // NERVE - SMF
	re.DrawStretchPicGradient   = RE_StretchPicGradient;
	re.DrawStretchRaw   = RE_StretchRaw;
	re.UploadCinematic  = RE_UploadCinematic;
	re.RegisterFont     = RE_RegisterFont;
	re.RemapShader      = R_RemapShader;
	re.GetEntityToken   = R_GetEntityToken;

	re.TakeVideoFrame = RE_TakeVideoFrame;

	return &re;
}

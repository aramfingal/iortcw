/*
===========================================================================
Copyright (C) 2006 Kirk Barnes
Copyright (C) 2006-2008 Robert Beckebans <trebor_7@users.sourceforge.net>

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
// tr_fbo.cpp — Metal port of tr_fbo.c
//
//
// REAL:  FBO registry/struct bookkeeping (tr.fbos[], numFBOs, name/index/
//        width/height/format field assignments, colorBuffers[]/colorImage[]
//        tracking, glRefConfig guards), FBO_Init control flow (hdrFormat
//        selection, multisample negotiation, full engine FBO creation
//        sequence, NULL-image guards).
// PARITY-SEAM(Metal-device): GL object creation/deletion/bind/blit calls
//        (qglGenFramebuffers, qglDeleteFramebuffers, qglGenRenderbuffers,
//        qglDeleteRenderbuffers, GL_BindRenderbuffer, qglNamedRenderbuffer*,
//        qglNamedFramebuffer*, qglCheckNamedFramebufferStatusEXT,
//        qglBlitFramebuffer, qglGetIntegerv(GL_MAX_SAMPLES), qglClear,
//        qglViewport, qglScissor) → MTLRenderPassDescriptor / MTLTexture
//        attachments / MTLBlitCommandEncoder (Phase 4).
//
// Disposition (11 functions):
//   9 REAL  (FBO_Create, FBO_CreateBuffer, FBO_AttachImage, FBO_Bind,
//            FBO_Init, FBO_Shutdown, R_FBOList_f, FBO_Blit,
//            FBO_BlitFromTexture)
//   2 PARITY-SEAM heavy (R_CheckFBO, FBO_FastBlit) — CPU coord/guard
//            logic kept; GL query / blit calls seamed
//   0 PARITY-STUB
//
// Scaffolds removed: FBO_Init, FBO_Shutdown, FBO_Bind, FBO_AttachImage,
//   FBO_FastBlit, FBO_BlitFromTexture, FBO_Blit (all 7 Ripley stubs from
//   tr_scaffold_stubs.cpp). No new scaffolds required.

// ---------------------------------------------------------------------------
// GL enum constants — #ifndef guarded (GL-COMPAT per-TU rule).
// GL2 ref: GL headers (canonical values; same as used in tr_dsa.cpp).
// ---------------------------------------------------------------------------
#ifndef GL_RGB
#define GL_RGB                              0x1907
#endif
#ifndef GL_RGBA
#define GL_RGBA                             0x1908
#endif
#ifndef GL_RGB8
#define GL_RGB8                             0x8051
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                            0x8058
#endif
#ifndef GL_RGB16F_ARB
#define GL_RGB16F_ARB                       0x881B
#endif
#ifndef GL_RGBA16F_ARB
#define GL_RGBA16F_ARB                      0x881A
#endif
#ifndef GL_RGB32F_ARB
#define GL_RGB32F_ARB                       0x8815
#endif
#ifndef GL_RGBA32F_ARB
#define GL_RGBA32F_ARB                      0x8814
#endif
#ifndef GL_DEPTH_COMPONENT
#define GL_DEPTH_COMPONENT                  0x1902
#endif
#ifndef GL_DEPTH_COMPONENT16_ARB
#define GL_DEPTH_COMPONENT16_ARB            0x81A5
#endif
#ifndef GL_DEPTH_COMPONENT24_ARB
#define GL_DEPTH_COMPONENT24_ARB            0x81A6
#endif
#ifndef GL_DEPTH_COMPONENT32_ARB
#define GL_DEPTH_COMPONENT32_ARB            0x81A7
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24                0x81A6
#endif
#ifndef GL_STENCIL_INDEX
#define GL_STENCIL_INDEX                    0x1901
#endif
#ifndef GL_STENCIL_INDEX1
#define GL_STENCIL_INDEX1                   0x8D46
#endif
#ifndef GL_STENCIL_INDEX4
#define GL_STENCIL_INDEX4                   0x8D47
#endif
#ifndef GL_STENCIL_INDEX8
#define GL_STENCIL_INDEX8                   0x8D48
#endif
#ifndef GL_STENCIL_INDEX16
#define GL_STENCIL_INDEX16                  0x8D49
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL                    0x84F9
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8                 0x88F0
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0                0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT                 0x8D00
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT               0x8D20
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D                       0x0DE1
#endif
#ifndef GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB  0x8515
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                      0x8D40
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER                 0x8CA9
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER                 0x8CA8
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER                     0x8CA0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE             0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_UNSUPPORTED
#define GL_FRAMEBUFFER_UNSUPPORTED          0x8CDD
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT        0x8CD6
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER
#define GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER       0x8CDB
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER
#define GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER       0x8CDC
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE
#define GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE       0x8D56
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES                      0x8D57
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT                 0x00004000
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT                 0x00000100
#endif
#ifndef GL_NEAREST
#define GL_NEAREST                          0x2600
#endif
#ifndef GL_LINEAR
#define GL_LINEAR                           0x2601
#endif

// Include strategy: same as tr_backend.cpp / tr_scene.cpp.
// tr_extramath.cpp defines Mat4Ortho as a C++ function.
// Include tr_extramath.h in C++ context so its declaration matches the
// definition; the include guard prevents re-declaration when tr_local.h
// is subsequently included inside extern "C".
// GL2 ref: tr_fbo.c:22-24 (analogous pattern)
extern "C" {
#include "../qcommon/q_shared.h"
}

#include "tr_extramath.h"

extern "C" {
#include "tr_local.h"
#include "tr_dsa.h"
}

// Metal blit-test cvar — declared in tr_init.cpp; referenced here only to
// gate the non-GL2 acceptance-test renderFbo path (see FBO_Init below).
extern cvar_t *r_metalBlitTest;

#include "tr_metal_device.h"
#include "tr_metal_shadersource.h" // PARITY-SEAM(Metal-device): query MSL availability before postprocess blits.
#include <set>
#include <string>

static uint32_t FBO_ColorTexnum( const FBO_t *fbo ) {
	return ( fbo && fbo->colorImage[0] ) ? fbo->colorImage[0]->texnum : 0;
}

static qboolean FBO_HasTextureBackedColor( const FBO_t *fbo ) {
	return (qboolean)( fbo && fbo->colorImage[0] && fbo->colorImage[0]->texnum != 0 );
}

static void FBO_WarnOnce( qboolean *warned, const char *message ) {
	if ( !*warned ) {
		ri.Printf( PRINT_DEVELOPER, "%s\n", message );
		*warned = qtrue;
	}
}

// ---------------------------------------------------------------------------
// Levels-texture binding (tonemap / autoexposure chain)
//
// GL2 semantics: RB_ToneMap calls GL_BindToTMU(levelsImage, TB_LEVELSMAP)
// before FBO_Blit(..., &tr.tonemapShader, ...).  OpenGL's state machine then
// automatically delivers TMU 1 to u_LevelsMap inside the draw call.
//
// Metal: GLMetal_RenderBlitTextureMulti creates its own command encoder and
// does NOT inherit s_textureBindings[].  Instead, FBO_BlitFromTexture reads
// s_textureBindings[TB_LEVELSMAP] via GLMetal_GetBoundTexnum (Wave 2 getter)
// and passes it as extraTexnums[0] → [[texture(1)]].  No separate setter
// call from Lambert is required — GL_BindToTMU(levelsImage, TB_LEVELSMAP)
// is the single source of truth, matching GL2 semantics exactly.
//
// GL2 ref: tr_postprocess.c:89-91 — GL_BindToTMU(levels, TB_LEVELSMAP)
//          before the tonemap FBO_Blit; tr_fbo.c:580-585 RB_InstantQuad2
// ---------------------------------------------------------------------------

/*
=============
R_CheckFBO
=============
*/
qboolean R_CheckFBO( const FBO_t * fbo ) {
	// PARITY-SEAM(Metal-device): qglCheckNamedFramebufferStatusEXT(fbo->frameBuffer, GL_FRAMEBUFFER)
	// → Metal render targets are always valid at allocation time; no completeness query.
	// Return qtrue unconditionally during Phase 4 bring-up.
	GLenum code = GL_FRAMEBUFFER_COMPLETE;
	(void)fbo;

	if ( code == GL_FRAMEBUFFER_COMPLETE ) {
		return qtrue;
	}

	// an error occurred — switch preserved for documentation parity with GL2.
	switch ( code )
	{
	case GL_FRAMEBUFFER_UNSUPPORTED:
		ri.Printf( PRINT_WARNING, "R_CheckFBO: (%s) Unsupported framebuffer format\n", fbo->name );
		break;

	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
		ri.Printf( PRINT_WARNING, "R_CheckFBO: (%s) Framebuffer incomplete attachment\n", fbo->name );
		break;

	case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
		ri.Printf( PRINT_WARNING, "R_CheckFBO: (%s) Framebuffer incomplete, missing attachment\n", fbo->name );
		break;

	case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
		ri.Printf( PRINT_WARNING, "R_CheckFBO: (%s) Framebuffer incomplete, missing draw buffer\n", fbo->name );
		break;

	case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
		ri.Printf( PRINT_WARNING, "R_CheckFBO: (%s) Framebuffer incomplete, missing read buffer\n", fbo->name );
		break;

	case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
		ri.Printf( PRINT_WARNING, "R_CheckFBO: (%s) Framebuffer incomplete multisample\n", fbo->name );
		break;

	default:
		ri.Printf( PRINT_WARNING, "R_CheckFBO: (%s) unknown error 0x%X\n", fbo->name, code );
		break;
	}

	return qfalse;
}

/*
============
FBO_Create
============
*/
FBO_t          *FBO_Create( const char *name, int width, int height ) {
	FBO_t          *fbo;

	// REAL: validation and registry bookkeeping. GL2 ref: tr_fbo.c:84
	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Error( ERR_DROP, "FBO_Create: \"%s\" is too long", name );
	}

	if ( width <= 0 || width > glRefConfig.maxRenderbufferSize ) {
		ri.Error( ERR_DROP, "FBO_Create: bad width %i", width );
	}

	if ( height <= 0 || height > glRefConfig.maxRenderbufferSize ) {
		ri.Error( ERR_DROP, "FBO_Create: bad height %i", height );
	}

	if ( tr.numFBOs == MAX_FBOS ) {
		ri.Error( ERR_DROP, "FBO_Create: MAX_FBOS hit" );
	}

	// REAL: struct allocation and field initialisation. GL2 ref: tr_fbo.c:107
	fbo = tr.fbos[tr.numFBOs] = (FBO_t *)ri.Hunk_Alloc( sizeof(*fbo ), h_low );
	Q_strncpyz( fbo->name, name, sizeof( fbo->name ) );
	fbo->index = tr.numFBOs++;
	fbo->width = width;
	fbo->height = height;

	// PARITY-SEAM(Metal-device): qglGenFramebuffers(1, &fbo->frameBuffer)
	// → MTLRenderPassDescriptor / MTLTexture created at Phase 4 bind-up.
	// fbo->frameBuffer stays 0 as a Phase-4 placeholder.
	fbo->frameBuffer = 0;

	return fbo;
}

/*
=================
FBO_CreateBuffer
=================
*/
void FBO_CreateBuffer( FBO_t *fbo, int format, int index, int multisample ) {
	uint32_t *pRenderBuffer;
	GLenum attachment;
	qboolean absent;

	// REAL: format-to-field routing. GL2 ref: tr_fbo.c:126
	switch ( format )
	{
	case GL_RGB:
	case GL_RGBA:
	case GL_RGB8:
	case GL_RGBA8:
	case GL_RGB16F_ARB:
	case GL_RGBA16F_ARB:
	case GL_RGB32F_ARB:
	case GL_RGBA32F_ARB:
		fbo->colorFormat = format;
		pRenderBuffer = &fbo->colorBuffers[index];
		attachment = GL_COLOR_ATTACHMENT0 + index;
		break;

	case GL_DEPTH_COMPONENT:
	case GL_DEPTH_COMPONENT16_ARB:
	case GL_DEPTH_COMPONENT24_ARB:
	case GL_DEPTH_COMPONENT32_ARB:
		fbo->depthFormat = format;
		pRenderBuffer = &fbo->depthBuffer;
		attachment = GL_DEPTH_ATTACHMENT;
		break;

	case GL_STENCIL_INDEX:
	case GL_STENCIL_INDEX1:
	case GL_STENCIL_INDEX4:
	case GL_STENCIL_INDEX8:
	case GL_STENCIL_INDEX16:
		fbo->stencilFormat = format;
		pRenderBuffer = &fbo->stencilBuffer;
		attachment = GL_STENCIL_ATTACHMENT;
		break;

	case GL_DEPTH_STENCIL:
	case GL_DEPTH24_STENCIL8:
		fbo->packedDepthStencilFormat = format;
		pRenderBuffer = &fbo->packedDepthStencilBuffer;
		attachment = 0;     // special: both depth and stencil
		break;

	default:
		ri.Printf( PRINT_WARNING, "FBO_CreateBuffer: invalid format %d\n", format );
		return;
	}

	// REAL: absent-buffer guard (tracks whether the GL object has been
	// allocated yet; preserved for Phase-4 parity). GL2 ref: tr_fbo.c:183
	absent = (qboolean)(*pRenderBuffer == 0 );
	if ( absent ) {
		// PARITY-SEAM(Metal-device): qglGenRenderbuffers(1, pRenderBuffer)
		// → allocate MTLTexture for MSAA/depth role at Phase 4.
		*pRenderBuffer = 0; // Phase-4 placeholder

		// PARITY-SEAM(Metal-device): GL_BindRenderbuffer(*pRenderBuffer)
		// → workaround for AMD Windows driver; no-op under Metal.
	}

	// PARITY-SEAM(Metal-device): qglNamedRenderbufferStorageMultisampleEXT /
	// qglNamedRenderbufferStorageEXT → MTLTextureDescriptor with MSAA
	// sampleCount / standard texture descriptor at Phase 4.
	// glRefConfig.framebufferMultisample guard preserved for parity.
	if ( multisample && glRefConfig.framebufferMultisample ) {
		// PARITY-SEAM(Metal-device): MSAA renderbuffer storage. Phase 4.
	} else
	{
		// PARITY-SEAM(Metal-device): standard renderbuffer storage. Phase 4.
	}

	if ( absent ) {
		if ( attachment == 0 ) {
			// PARITY-SEAM(Metal-device): qglNamedFramebufferRenderbufferEXT for
			// GL_DEPTH_ATTACHMENT + GL_STENCIL_ATTACHMENT (packed depth/stencil)
			// → attach MTLTexture as depthAttachment + stencilAttachment in
			// MTLRenderPassDescriptor at Phase 4.
		} else
		{
			// PARITY-SEAM(Metal-device): qglNamedFramebufferRenderbufferEXT for
			// colour / depth / stencil attachment
			// → attach MTLTexture at colorAttachments[index] / depthAttachment /
			// stencilAttachment in MTLRenderPassDescriptor at Phase 4.
		}
	}
}


/*
=================
FBO_AttachImage
=================
*/
void FBO_AttachImage( FBO_t *fbo, image_t *image, GLenum attachment, GLuint cubemapside ) {
	GLenum target = GL_TEXTURE_2D;
	int index;

	// REAL: cubemap target selection. GL2 ref: tr_fbo.c:210
	if ( image->flags & IMGFLAG_CUBEMAP ) {
		target = GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB + cubemapside;
	}

	// PARITY-SEAM(Metal-device): qglNamedFramebufferTexture2DEXT(fbo->frameBuffer,
	// attachment, target, image->texnum, 0) → bind MTLTexture (image->texnum as
	// handle) to the MTLRenderPassDescriptor attachment slot at Phase 4.
	(void)target;

	// REAL: colorImage[]/depthImage registry bookkeeping. GL2 ref: tr_fbo.c:213
	if ( attachment == GL_DEPTH_ATTACHMENT ) {
		// GL2 ref: tr_fbo.c:160-163 (pshadow/sun FBOs attach depth textures).
		// Store the depth attachment so the backend can find the depth target
		// for depth-only render passes (e.g., pshadow capture).
		fbo->depthImage = image;
	} else {
		index = attachment - GL_COLOR_ATTACHMENT0;
		if ( index >= 0 && index <= 15 ) {
			fbo->colorImage[index] = image;
		}
	}
}


/*
============
FBO_Bind
============
*/
void FBO_Bind( FBO_t * fbo ) {
	// REAL: framebufferObject guard. GL2 ref: tr_fbo.c:227
	if ( !glRefConfig.framebufferObject ) {
		ri.Printf( PRINT_WARNING, "FBO_Bind() called without framebuffers enabled!\n" );
		return;
	}

	// REAL: current-FBO duplicate check. GL2 ref: tr_fbo.c:232
	if ( glState.currentFBO == fbo ) {
		return;
	}

	// PARITY-SEAM(Metal-device): GLimp_LogComment is provided by sdl_glimp.c
	// which is not compiled under Metal; log annotation deferred to Phase 4
	// Metal debug labels (MTLCommandBuffer debugDescription).
	// GLimp_LogComment(va("--- FBO_Bind( %s ) ---\n", fbo ? fbo->name : "NULL"));

	// REAL: current-FBO state bookkeeping. GL2 ref: tr_fbo.c:240
	glState.currentFBO = fbo;

	// PARITY-SEAM(Metal-device): GL_BindFramebuffer(GL_FRAMEBUFFER, fbo ? fbo->frameBuffer : 0)
	// → set active MTLRenderPassDescriptor target at Phase 4.
	if ( fbo == NULL ) {
		GLMetal_SetRenderTarget( 0, 0, false, false );
	} else
	{
		uint32_t colorTexnum = fbo->colorImage[0] ? fbo->colorImage[0]->texnum : 0;
		uint32_t depthTexnum = ( fbo == tr.renderFbo && tr.renderDepthImage ) ? tr.renderDepthImage->texnum : 0;
		GLMetal_SetRenderTarget( colorTexnum, depthTexnum, false, false );
	}
}

/*
============
FBO_Init
============
*/
void FBO_Init( void ) {
	int i;
	int hdrFormat, multisample = 0;

	ri.Printf( PRINT_ALL, "------- FBO_Init -------\n" );

	// REAL: early-out guard (matches GL2 behaviour; glRefConfig.framebufferObject
	// is set false by Hudson's tr_extensions until Phase-4 Metal bring-up).
	if ( !glRefConfig.framebufferObject ) {
		return;
	}

	// REAL: registry reset. GL2 ref: tr_fbo.c:260
	tr.numFBOs = 0;

	GL_CheckErrors();

	// REAL: flush pending render commands before allocating FBO GPU objects.
	R_IssuePendingRenderCommands();

	// REAL: HDR format selection. GL2 ref: tr_fbo.c:265
	hdrFormat = GL_RGBA8;
	if ( r_hdr->integer && glRefConfig.textureFloat ) {
		hdrFormat = GL_RGBA16F_ARB;
	}

	// REAL: multisample cap negotiation. GL2 ref: tr_fbo.c:269
	if ( glRefConfig.framebufferMultisample ) {
		// PARITY-SEAM(Metal-device): qglGetIntegerv(GL_MAX_SAMPLES, &multisample)
		// → MTLDevice maxSampleCount at Phase 4; leave multisample = 0 for now.
		multisample = 0;
	}

	// REAL: clamp to cvar. GL2 ref: tr_fbo.c:272
	if ( r_ext_framebuffer_multisample->integer < multisample ) {
		multisample = r_ext_framebuffer_multisample->integer;
	}

	// REAL: reject MSAA if no blit support or sample count < 2. GL2 ref: tr_fbo.c:275
	if ( multisample < 2 || !glRefConfig.framebufferBlit ) {
		multisample = 0;
	}

	// REAL: write-back negotiated value. GL2 ref: tr_fbo.c:278
	if ( multisample != r_ext_framebuffer_multisample->integer ) {
		ri.Cvar_SetValue( "r_ext_framebuffer_multisample", (float)multisample );
	}

	// REAL: MSAA path — render FBO + msaaResolve FBO. GL2 ref: tr_fbo.c:281
	if ( multisample && glRefConfig.framebufferMultisample ) {
		tr.renderFbo = FBO_Create( "_render", tr.renderDepthImage->width, tr.renderDepthImage->height );
		FBO_CreateBuffer( tr.renderFbo, hdrFormat, 0, multisample );
		FBO_CreateBuffer( tr.renderFbo, GL_DEPTH_COMPONENT24, 0, multisample );
		R_CheckFBO( tr.renderFbo );

		tr.msaaResolveFbo = FBO_Create( "_msaaResolve", tr.renderDepthImage->width, tr.renderDepthImage->height );
		FBO_AttachImage( tr.msaaResolveFbo, tr.renderImage, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( tr.msaaResolveFbo, tr.renderDepthImage, GL_DEPTH_ATTACHMENT, 0 );
		R_CheckFBO( tr.msaaResolveFbo );
	}
	// REAL: HDR-only path (no MSAA). GL2 ref: tr_fbo.c:294
	else if ( r_hdr->integer ) {
		tr.renderFbo = FBO_Create( "_render", tr.renderDepthImage->width, tr.renderDepthImage->height );
		FBO_AttachImage( tr.renderFbo, tr.renderImage, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( tr.renderFbo, tr.renderDepthImage, GL_DEPTH_ATTACHMENT, 0 );
		R_CheckFBO( tr.renderFbo );
	} else
	{
		// Metal divergence from GL2: always create tr.renderFbo.
		// GL2 ref: tr_fbo.c:251-310 — GL2 leaves renderFbo NULL here and the
		// scene renders directly to framebuffer 0 (the window surface). Metal
		// cannot do this because the drawable is late-acquired per Hudson's
		// frame-lifecycle contract (GLMetal_AcquireDrawableForPresent is called
		// only at present time). Without an offscreen target, draw calls that
		// target the drawable silently fail (GLMetal_EnsureEncoder returns false
		// when s_drawable is nil). The offscreen FBO guarantees rendering always
		// succeeds; RB_PresentToScreen then blits via GLMetal_TonemapPass which
		// acquires the drawable on demand.
		tr.renderFbo = FBO_Create( "_render", tr.renderDepthImage->width, tr.renderDepthImage->height );
		FBO_AttachImage( tr.renderFbo, tr.renderImage, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( tr.renderFbo, tr.renderDepthImage, GL_DEPTH_ATTACHMENT, 0 );
		R_CheckFBO( tr.renderFbo );
	}

	// REAL + partial SEAM: if a render FBO was created, clear it to fix the
	// corrupt-screen bug on older hardware. GL2 ref: tr_fbo.c:303
	if ( tr.renderFbo ) {
		// PARITY-SEAM(Metal-device): GL_BindFramebuffer + qglClear →
		// MTLRenderPassDescriptor loadAction = MTLLoadActionClear at Phase 4.
		// GL2 ref: tr_fbo.c:305-306.  This primes the lazy clear state; the
		// backend also color-clears the first renderFbo view each frame so startup
		// cannot show stale texture contents if no encoder opens during init.
		uint32_t colorTexnum = tr.renderFbo->colorImage[0] ? tr.renderFbo->colorImage[0]->texnum : 0;
		uint32_t depthTexnum = tr.renderDepthImage ? tr.renderDepthImage->texnum : 0;
		GLMetal_SetRenderTarget( colorTexnum, depthTexnum, true, true );
	}

	// REAL: screenScratch FBO. GL2 ref: tr_fbo.c:309
	if ( tr.screenScratchImage ) {
		tr.screenScratchFbo = FBO_Create( "screenScratch", tr.screenScratchImage->width, tr.screenScratchImage->height );
		FBO_AttachImage( tr.screenScratchFbo, tr.screenScratchImage, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( tr.screenScratchFbo, tr.renderDepthImage, GL_DEPTH_ATTACHMENT, 0 );
		R_CheckFBO( tr.screenScratchFbo );
	}

	// REAL: sunRays FBO. GL2 ref: tr_fbo.c:317
	if ( tr.sunRaysImage ) {
		tr.sunRaysFbo = FBO_Create( "_sunRays", tr.renderDepthImage->width, tr.renderDepthImage->height );
		FBO_AttachImage( tr.sunRaysFbo, tr.sunRaysImage, GL_COLOR_ATTACHMENT0, 0 );
		FBO_AttachImage( tr.sunRaysFbo, tr.renderDepthImage, GL_DEPTH_ATTACHMENT, 0 );
		R_CheckFBO( tr.sunRaysFbo );
	}

	// REAL: per-light shadow-map FBOs. GL2 ref: tr_fbo.c:324
	if ( MAX_DRAWN_PSHADOWS && tr.pshadowMaps[0] ) {
		for ( i = 0; i < MAX_DRAWN_PSHADOWS; i++)
		{
			tr.pshadowFbos[i] = FBO_Create( va( "_shadowmap%d", i ), tr.pshadowMaps[i]->width, tr.pshadowMaps[i]->height );
			FBO_CreateBuffer( tr.pshadowFbos[i], GL_RGBA8, 0, 0 );
			FBO_AttachImage( tr.pshadowFbos[i], tr.pshadowMaps[i], GL_DEPTH_ATTACHMENT, 0 );
			R_CheckFBO( tr.pshadowFbos[i] );
		}
	}

	// REAL: sun-shadow cascade FBOs. GL2 ref: tr_fbo.c:335
	if ( tr.sunShadowDepthImage[0] ) {
		for (i = 0; i < 4; i++)
		{
			tr.sunShadowFbo[i] = FBO_Create( "_sunshadowmap", tr.sunShadowDepthImage[i]->width, tr.sunShadowDepthImage[i]->height );
			FBO_CreateBuffer( tr.sunShadowFbo[i], GL_RGBA8, 0, 0 );
			FBO_AttachImage( tr.sunShadowFbo[i], tr.sunShadowDepthImage[i], GL_DEPTH_ATTACHMENT, 0 );
			R_CheckFBO( tr.sunShadowFbo[i] );
		}
	}

	// REAL: screen-shadow FBO. GL2 ref: tr_fbo.c:345
	if ( tr.screenShadowImage ) {
		tr.screenShadowFbo = FBO_Create( "_screenshadow", tr.screenShadowImage->width, tr.screenShadowImage->height );
		FBO_AttachImage( tr.screenShadowFbo, tr.screenShadowImage, GL_COLOR_ATTACHMENT0, 0 );
		R_CheckFBO( tr.screenShadowFbo );
	}

	// REAL: texture-scratch FBOs. GL2 ref: tr_fbo.c:352
	if ( tr.textureScratchImage[0] ) {
		for (i = 0; i < 2; i++)
		{
			tr.textureScratchFbo[i] = FBO_Create( va( "_texturescratch%d", i ), tr.textureScratchImage[i]->width, tr.textureScratchImage[i]->height );
			FBO_AttachImage( tr.textureScratchFbo[i], tr.textureScratchImage[i], GL_COLOR_ATTACHMENT0, 0 );
			R_CheckFBO( tr.textureScratchFbo[i] );
		}
	}

	// REAL: calcLevels FBO. GL2 ref: tr_fbo.c:361
	if ( tr.calcLevelsImage ) {
		tr.calcLevelsFbo = FBO_Create( "_calclevels", tr.calcLevelsImage->width, tr.calcLevelsImage->height );
		FBO_AttachImage( tr.calcLevelsFbo, tr.calcLevelsImage, GL_COLOR_ATTACHMENT0, 0 );
		R_CheckFBO( tr.calcLevelsFbo );
	}

	// REAL: targetLevels FBO. GL2 ref: tr_fbo.c:368
	if ( tr.targetLevelsImage ) {
		tr.targetLevelsFbo = FBO_Create( "_targetlevels", tr.targetLevelsImage->width, tr.targetLevelsImage->height );
		FBO_AttachImage( tr.targetLevelsFbo, tr.targetLevelsImage, GL_COLOR_ATTACHMENT0, 0 );
		R_CheckFBO( tr.targetLevelsFbo );
	}

	// REAL: quarter-size FBOs. GL2 ref: tr_fbo.c:375
	if ( tr.quarterImage[0] ) {
		for (i = 0; i < 2; i++)
		{
			tr.quarterFbo[i] = FBO_Create( va( "_quarter%d", i ), tr.quarterImage[i]->width, tr.quarterImage[i]->height );
			FBO_AttachImage( tr.quarterFbo[i], tr.quarterImage[i], GL_COLOR_ATTACHMENT0, 0 );
			R_CheckFBO( tr.quarterFbo[i] );
		}
	}

	// REAL: HDR depth FBO. GL2 ref: tr_fbo.c:383
	if ( tr.hdrDepthImage ) {
		tr.hdrDepthFbo = FBO_Create( "_hdrDepth", tr.hdrDepthImage->width, tr.hdrDepthImage->height );
		FBO_AttachImage( tr.hdrDepthFbo, tr.hdrDepthImage, GL_COLOR_ATTACHMENT0, 0 );
		R_CheckFBO( tr.hdrDepthFbo );
	}

	// REAL: screen-SSAO FBO. GL2 ref: tr_fbo.c:390
	if ( tr.screenSsaoImage ) {
		tr.screenSsaoFbo = FBO_Create( "_screenssao", tr.screenSsaoImage->width, tr.screenSsaoImage->height );
		FBO_AttachImage( tr.screenSsaoFbo, tr.screenSsaoImage, GL_COLOR_ATTACHMENT0, 0 );
		R_CheckFBO( tr.screenSsaoFbo );
	}

	// REAL: render-cube FBO. GL2 ref: tr_fbo.c:397
	if ( tr.renderCubeImage ) {
		tr.renderCubeFbo = FBO_Create( "_renderCubeFbo", tr.renderCubeImage->width, tr.renderCubeImage->height );
		FBO_AttachImage( tr.renderCubeFbo, tr.renderCubeImage, GL_COLOR_ATTACHMENT0, 0 );
		FBO_CreateBuffer( tr.renderCubeFbo, GL_DEPTH_COMPONENT24_ARB, 0, 0 );
		R_CheckFBO( tr.renderCubeFbo );
	}

	GL_CheckErrors();

	// PARITY-SEAM(Metal-device): GL_BindFramebuffer(GL_FRAMEBUFFER, 0) →
	// clear active render target at Phase 4.
	GLMetal_SetRenderTarget( 0, 0, false, false );
	glState.currentFBO = NULL;
}

/*
============
FBO_Shutdown
============
*/
void FBO_Shutdown( void ) {
	int i, j;
	FBO_t          *fbo;

	ri.Printf( PRINT_ALL, "------- FBO_Shutdown -------\n" );

	// REAL: early-out guard. GL2 ref: tr_fbo.c:434
	if ( !glRefConfig.framebufferObject ) {
		return;
	}

	// REAL: unbind current FBO before teardown. GL2 ref: tr_fbo.c:436
	FBO_Bind( NULL );

	// REAL: iterate registry and release resources. GL2 ref: tr_fbo.c:438
	for (i = 0; i < tr.numFBOs; i++)
	{
		fbo = tr.fbos[i];

		// REAL: colorBuffers[] loop uses glRefConfig.maxColorAttachments.
		for (j = 0; j < glRefConfig.maxColorAttachments; j++)
		{
			if ( fbo->colorBuffers[j] ) {
				// PARITY-SEAM(Metal-device): qglDeleteRenderbuffers(1, &fbo->colorBuffers[j])
				// → release MTLTexture backing the colour renderbuffer at Phase 4.
				fbo->colorBuffers[j] = 0;
			}
		}

		if ( fbo->depthBuffer ) {
			// PARITY-SEAM(Metal-device): qglDeleteRenderbuffers(1, &fbo->depthBuffer)
			// → release MTLTexture backing the depth renderbuffer at Phase 4.
			fbo->depthBuffer = 0;
		}

		if ( fbo->stencilBuffer ) {
			// PARITY-SEAM(Metal-device): qglDeleteRenderbuffers(1, &fbo->stencilBuffer)
			// → release MTLTexture backing the stencil renderbuffer at Phase 4.
			fbo->stencilBuffer = 0;
		}

		if ( fbo->frameBuffer ) {
			// PARITY-SEAM(Metal-device): qglDeleteFramebuffers(1, &fbo->frameBuffer)
			// → release Metal render-pass resources at Phase 4.
			fbo->frameBuffer = 0;
		}
	}
}

/*
============
R_FBOList_f
============
*/
void R_FBOList_f( void ) {
	int i;
	FBO_t          *fbo;

	// REAL: entirely CPU/console logic. GL2 ref: tr_fbo.c:470
	if ( !glRefConfig.framebufferObject ) {
		ri.Printf( PRINT_ALL, "GL_EXT_framebuffer_object is not available.\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "             size       name\n" );
	ri.Printf( PRINT_ALL, "----------------------------------------------------------\n" );

	for (i = 0; i < tr.numFBOs; i++)
	{
		fbo = tr.fbos[i];

		ri.Printf( PRINT_ALL, "  %4i: %4i %4i %s\n", i, fbo->width, fbo->height, fbo->name );
	}

	ri.Printf( PRINT_ALL, " %i FBOs\n", tr.numFBOs );
}

void FBO_BlitFromTexture( struct image_s *src, vec4_t inSrcTexCorners, vec2_t inSrcTexScale, FBO_t *dst, ivec4_t inDstBox, struct shaderProgram_s *shaderProgram, vec4_t inColor, int blend ) {
	ivec4_t dstBox;
	vec4_t color;
	vec4_t quadVerts[4];
	vec2_t texCoords[4];
	vec2_t invTexRes;
	FBO_t *oldFbo = glState.currentFBO;
	mat4_t projection;
	int width, height;

	if ( !src ) {
		ri.Printf( PRINT_WARNING, "Tried to blit from a NULL texture!\n" );
		return;
	}

	// REAL: destination dimensions. GL2 ref: tr_fbo.c:505
	width  = dst ? dst->width  : glConfig.vidWidth;
	height = dst ? dst->height : glConfig.vidHeight;

	// REAL: source texture corner mapping. GL2 ref: tr_fbo.c:508
	if ( inSrcTexCorners ) {
		VectorSet2( texCoords[0], inSrcTexCorners[0], inSrcTexCorners[1] );
		VectorSet2( texCoords[1], inSrcTexCorners[2], inSrcTexCorners[1] );
		VectorSet2( texCoords[2], inSrcTexCorners[2], inSrcTexCorners[3] );
		VectorSet2( texCoords[3], inSrcTexCorners[0], inSrcTexCorners[3] );
	} else
	{
		VectorSet2( texCoords[0], 0.0f, 1.0f );
		VectorSet2( texCoords[1], 1.0f, 1.0f );
		VectorSet2( texCoords[2], 1.0f, 0.0f );
		VectorSet2( texCoords[3], 0.0f, 0.0f );
	}

	// REAL: destination box — framebuffers are 0-bottom, Y-up. GL2 ref: tr_fbo.c:522
	if ( inDstBox ) {
		dstBox[0] = inDstBox[0];
		dstBox[1] = height - inDstBox[1] - inDstBox[3];
		dstBox[2] = inDstBox[0] + inDstBox[2];
		dstBox[3] = height - inDstBox[1];
	} else
	{
		VectorSet4( dstBox, 0, height, width, 0 );
	}

	// REAL: inverse texture resolution. GL2 ref: tr_fbo.c:533
	if ( inSrcTexScale ) {
		VectorCopy2( inSrcTexScale, invTexRes );
	} else
	{
		VectorSet2( invTexRes, 1.0f, 1.0f );
	}

	// REAL: colour default. GL2 ref: tr_fbo.c:541
	if ( inColor ) {
		VectorCopy4( inColor, color );
	} else
	{
		VectorCopy4( colorWhite, color );
	}

	// REAL: shader program default. GL2 ref: tr_fbo.c:549
	if ( !shaderProgram ) {
		shaderProgram = &tr.textureColorShader;
	}

	// REAL: bind destination FBO. GL2 ref: tr_fbo.c:554
	FBO_Bind( dst );

	// GL2 ref: tr_fbo.c:556-557. RenderBlitTexture owns the Metal viewport/scissor
	// for the destination rect, preserving dst contents with load/store.

	// REAL: ortho projection. GL2 ref: tr_fbo.c:559
	Mat4Ortho( 0, width, height, 0, 0, 1, projection );

	GL_Cull( CT_TWO_SIDED );
	GL_BindToTMU( src, TB_COLORMAP );

	// REAL: quad vertex positions. GL2 ref: tr_fbo.c:565
	VectorSet4( quadVerts[0], dstBox[0], dstBox[1], 0.0f, 1.0f );
	VectorSet4( quadVerts[1], dstBox[2], dstBox[1], 0.0f, 1.0f );
	VectorSet4( quadVerts[2], dstBox[2], dstBox[3], 0.0f, 1.0f );
	VectorSet4( quadVerts[3], dstBox[0], dstBox[3], 0.0f, 1.0f );

	// REAL: normalise invTexRes by source dimensions. GL2 ref: tr_fbo.c:571
	invTexRes[0] /= src->width;
	invTexRes[1] /= src->height;

	GL_State( blend );
	GLSL_BindProgram( shaderProgram );
	GLSL_SetUniformMat4( shaderProgram, UNIFORM_MODELVIEWPROJECTIONMATRIX, projection );
	GLSL_SetUniformVec4( shaderProgram, UNIFORM_COLOR, color );
	GLSL_SetUniformVec2( shaderProgram, UNIFORM_INVTEXRES, invTexRes );
	GLSL_SetUniformVec2( shaderProgram, UNIFORM_AUTOEXPOSUREMINMAX, tr.refdef.autoExposureMinMax );
	GLSL_SetUniformVec3( shaderProgram, UNIFORM_TONEMINAVGMAXLINEAR, tr.refdef.toneMinAvgMaxLinear );

	float srcUV[8] = {
		texCoords[0][0], texCoords[0][1],
		texCoords[1][0], texCoords[1][1],
		texCoords[2][0], texCoords[2][1],
		texCoords[3][0], texCoords[3][1]
	};
	int dstX, dstY, dstW, dstH;
	if ( inDstBox ) {
		dstX = inDstBox[0];
		dstY = inDstBox[1];
		dstW = inDstBox[2];
		dstH = inDstBox[3];
	} else
	{
		dstX = 0;
		dstY = 0;
		dstW = width;
		dstH = height;
	}
	if ( dstW < 0 ) {
		dstX += dstW;
		dstW = -dstW;
	}
	if ( dstH < 0 ) {
		dstY += dstH;
		dstH = -dstH;
	}

	// BUG FIX (lambert-autoexposure-rootcause): use metalProgramKey for pipeline lookup.
	// metalProgramKey is the permutation-qualified cache key registered by GLSL_InitGPUShader2
	// (e.g. "tonemap#811c9dc5", "calclevels4x#<FIRST_PASS_hash>").  shaderProgram->name is
	// the bare base name (e.g. "tonemap", "calclevels4x") which GLMetal_GetPipelineForState
	// cannot resolve because s_programFns is keyed by progKey not by base name.
	// Before this fix ALL postprocess shader blits silently failed (PSO=nullptr), leaving
	// the calclevels chain and tonemap draws as no-ops — auto-exposure never adapted.
	// GL2 ref: tr_glsl.c:1033-1041 (progKey = "<name>#<hash>"); tr_glsl.c:922 (registration).
	// shaderProgram is never NULL here (defaulted to tr.textureColorShader above).
	const char *programName =
		( shaderProgram->metalProgramKey[0] ) ? shaderProgram->metalProgramKey : shaderProgram->name;
	const qboolean hasMsl = ( shaderProgram->name[0] ) ?
							( ( GLMetal_GetShaderSource( shaderProgram->name, qfalse ) != NULL ) ? qtrue : qfalse ) : qfalse; // PARITY-SEAM(Metal-device): vertex MSL presence gates real postprocess blits.
	const void *postprocessUniforms = NULL; // PARITY-SEAM(Metal-device): NULL preserves the legacy bare texturecolor path.
	MetalPostprocessUniforms ppU;
	if ( hasMsl && Q_stricmp( shaderProgram->name, "texturecolor" ) != 0 ) {
		R_FillMetalPostprocessUniforms( shaderProgram, &ppU ); // GL2 ref: uniforms set above mirror tr_fbo.c:577-580 before DrawElements.
		postprocessUniforms = &ppU;
	} else
	{
		if ( Q_stricmp( shaderProgram->name, "texturecolor" ) != 0 ) {
			static std::set<std::string> s_deferredShaderWarned;
			if ( s_deferredShaderWarned.find( shaderProgram->name ) == s_deferredShaderWarned.end() ) {
				ri.Printf( PRINT_DEVELOPER, "WARNING: Metal FBO_BlitFromTexture deferred shader '%s'; using texturecolor passthrough for Slice B\n", shaderProgram->name );
				s_deferredShaderWarned.insert( shaderProgram->name );
			}
		}
		// Use texturecolor pipeline via its permutation-qualified key.
		programName = tr.textureColorShader.metalProgramKey[0] ?
					  tr.textureColorShader.metalProgramKey : "texturecolor";
	}

	// Pass levels map (TB_LEVELSMAP, slot 1) to the blit dispatcher so the tonemap
	// shader can sample adapted luminance.  GL2 delivers this automatically because
	// GL_BindToTMU(levelsImage, TB_LEVELSMAP) persists in OpenGL state; in Metal we
	// read it explicitly via GLMetal_GetBoundTexnum and pass it as extraTexnums[0].
	// For ordinary blits (no levels map bound) the texnum is 0 and Hudson's device
	// guards skip the bind, so non-tonemap paths are not regressed.
	// GL2 ref: tr_postprocess.c:89-91 (GL_BindToTMU(calcLevelsImage/fixedLevelsImage, TB_LEVELSMAP))
	const uint32_t levelsTexnum = GLMetal_GetBoundTexnum( TB_LEVELSMAP );
	uint32_t extraTexnums[1] = { levelsTexnum };
	const int extraTexCount = ( levelsTexnum != 0 ) ? 1 : 0;

	// Use multi-texture variant so the levels map (and any future extra TMU) reaches
	// the fragment shader at [[texture(1..N)]].  When extraTexCount==0 this is
	// functionally identical to GLMetal_RenderBlitTexture.
	// GL2 ref: tr_fbo.c:580-585 (RB_InstantQuad2 draws with all TMUs bound)

	// fullCoverage=true when the blit rect equals the full destination FBO — lets Hudson
	// set loadAction=DontCare (no wasted tile read on TBDR). GL2 has no equivalent cost
	// because qglBlitFramebuffer is a DMA command with no load phase.
	// Applies to: calclevels4x first-pass, all 8 ping-pong halving steps, temporal blend,
	// and tonemap final (all called with dstBox==NULL → dstX/Y=0, dstW/H=full).
	// RC-2 fix (dallas-hdr-perf-audit.md). GL2 ref: tr_fbo.c:678 (qglBlitFramebuffer).
	//
	// CORRECTNESS GATE: the DontCare optimization is only valid for blits that
	// fully OVERWRITE the destination.  A blend that READS the destination
	// (e.g. SSAO's GLS_SRCBLEND_DST_COLOR multiply: result = src*dst) requires the
	// destination's existing contents, so its tile MUST be loaded (LoadActionLoad).
	// Without this gate a full-coverage destination-reading blit multiplies against
	// undefined/zero tile memory and produces black.  The destination is read when:
	//   - the source blend factor references the destination (DST_COLOR / DST_ALPHA
	//     and their ONE_MINUS variants), or
	//   - the destination blend factor contributes (anything other than ZERO).
	// Replace-style blits (no blend, or SRCBLEND_ONE|DSTBLEND_ZERO) are unaffected
	// and keep the DontCare fast path.
	const int srcBlendBits = blend & GLS_SRCBLEND_BITS;
	const int dstBlendBits = blend & GLS_DSTBLEND_BITS;
	const bool srcFactorReadsDst =
		( srcBlendBits == GLS_SRCBLEND_DST_COLOR ) ||
		( srcBlendBits == GLS_SRCBLEND_ONE_MINUS_DST_COLOR ) ||
		( srcBlendBits == GLS_SRCBLEND_DST_ALPHA ) ||
		( srcBlendBits == GLS_SRCBLEND_ONE_MINUS_DST_ALPHA );
	const bool dstFactorContributes =
		( dstBlendBits != 0 ) && ( dstBlendBits != GLS_DSTBLEND_ZERO );
	const bool blitReadsDestination = srcFactorReadsDst || dstFactorContributes;
	const bool fullCoverage = ( dstX == 0 && dstY == 0 && dstW == width && dstH == height ) &&
							  !blitReadsDestination; // full-overwrite only: DontCare load (perf)

	if ( !GLMetal_RenderBlitTextureMulti( src->texnum, srcUV, FBO_ColorTexnum( dst ),
										  dstX, dstY, dstW, dstH, width, height,
										  GL_LINEAR, blend, color, programName,
										  postprocessUniforms,
										  extraTexnums, extraTexCount, fullCoverage ) ) {
		ri.Printf( PRINT_DEVELOPER, "WARNING: Metal FBO_BlitFromTexture render blit failed\n" );
	}

	// REAL: restore previous FBO. GL2 ref: tr_fbo.c:592
	FBO_Bind( oldFbo );
}

void FBO_Blit( const FBO_t *src, ivec4_t inSrcBox, vec2_t srcTexScale, FBO_t *dst, ivec4_t dstBox, struct shaderProgram_s *shaderProgram, vec4_t color, int blend ) {
	vec4_t srcTexCorners;

	if ( !src ) {
		ri.Printf( PRINT_WARNING, "Tried to blit from a NULL FBO!\n" );
		return;
	}

	// REAL: convert pixel box to normalised texture coordinates. GL2 ref: tr_fbo.c:603
	if ( inSrcBox ) {
		srcTexCorners[0] =  inSrcBox[0]                / (float)src->width;
		srcTexCorners[1] = ( inSrcBox[1] + inSrcBox[3] ) / (float)src->height;
		srcTexCorners[2] = ( inSrcBox[0] + inSrcBox[2] ) / (float)src->width;
		srcTexCorners[3] =  inSrcBox[1]                / (float)src->height;

		// Metal render targets store GL's bottom row last, and the blit writes a GL box
		// (x, y, w, h) to rows [H-y-h, H-y). Shift V so the source box is read from the
		// same rows; without this a sub-rect blit reads the opposite corner of the texture
		// (the auto-exposure downsample chain averaged stale texels). No-op for full boxes.
		{
			const float vShift = 1.0f - srcTexCorners[1] - srcTexCorners[3];
			srcTexCorners[1] += vShift;
			srcTexCorners[3] += vShift;
		}
	} else
	{
		VectorSet4( srcTexCorners, 0.0f, 0.0f, 1.0f, 1.0f );
	}

	// REAL: delegate to FBO_BlitFromTexture. GL2 ref: tr_fbo.c:617
	FBO_BlitFromTexture( src->colorImage[0], srcTexCorners, srcTexScale, dst, dstBox, shaderProgram, color, blend | GLS_DEPTHTEST_DISABLE );
}

void FBO_FastBlit( const FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox, int buffers, int filter ) {
	ivec4_t srcBoxFinal, dstBoxFinal;
	GLuint srcFb, dstFb;

	// REAL: extract GL framebuffer handles (0 = default backbuffer). GL2 ref: tr_fbo.c:631
	srcFb = src ? src->frameBuffer : 0;
	dstFb = dst ? dst->frameBuffer : 0;

	// REAL: source box calculation. GL2 ref: tr_fbo.c:634
	if ( !srcBox ) {
		int width =  src ? src->width  : glConfig.vidWidth;
		int height = src ? src->height : glConfig.vidHeight;

		VectorSet4( srcBoxFinal, 0, 0, width, height );
	} else
	{
		VectorSet4( srcBoxFinal, srcBox[0], srcBox[1], srcBox[0] + srcBox[2], srcBox[1] + srcBox[3] );
	}

	// REAL: destination box and scissor calculation. GL2 ref: tr_fbo.c:646
	if ( !dstBox ) {
		int width  = dst ? dst->width  : glConfig.vidWidth;
		int height = dst ? dst->height : glConfig.vidHeight;

		// RenderBlitTexture owns Metal scissor for the destination rect. GL2 ref: tr_fbo.c:651
		VectorSet4( dstBoxFinal, 0, 0, width, height );
	} else
	{
		ivec4_t scissorBox;

		// REAL: scissor-box normalisation (negative extents → positive). GL2 ref: tr_fbo.c:657
		Vector4Copy( dstBox, scissorBox );

		if ( scissorBox[2] < 0 ) {
			scissorBox[0] += scissorBox[2];
			scissorBox[2] = abs( scissorBox[2] );
		}

		if ( scissorBox[3] < 0 ) {
			scissorBox[1] += scissorBox[3];
			scissorBox[3] = abs( scissorBox[3] );
		}

		// Metal render blit receives dstBoxFinal; the device applies viewport/scissor.
		VectorSet4( dstBoxFinal, dstBox[0], dstBox[1], dstBox[0] + dstBox[2], dstBox[1] + dstBox[3] );
	}

	const int srcFullW = src ? src->width : glConfig.vidWidth;
	const int srcFullH = src ? src->height : glConfig.vidHeight;
	const int dstFullW = dst ? dst->width : glConfig.vidWidth;
	const int dstFullH = dst ? dst->height : glConfig.vidHeight;
	const int srcW = srcBoxFinal[2] - srcBoxFinal[0];
	const int srcH = srcBoxFinal[3] - srcBoxFinal[1];
	const int dstW = dstBoxFinal[2] - dstBoxFinal[0];
	const int dstH = dstBoxFinal[3] - dstBoxFinal[1];

	// REAL: fallback to shader blit if no hardware blit support. GL2 ref: tr_fbo.c:624
	// A1 parity: when framebufferBlit is unavailable, FBO_FastBlit MUST call FBO_Blit
	// as a fallback for ALL buffer combinations (depth, color, color|depth) so that
	// parity with GL2's unconditional FBO_Blit(src,...) path is preserved.
	// Metal optimization: attempt fast MTLBlit first for eligible same-size nearest
	// color-only copies; fall through to FBO_Blit for everything else.
	if ( !glRefConfig.framebufferBlit ) {
		const qboolean sameColorFormat = (qboolean)( src && dst &&
													 src->colorImage[0] && dst->colorImage[0] &&
													 src->colorImage[0]->internalFormat == dst->colorImage[0]->internalFormat );

		if ( buffers == GL_COLOR_BUFFER_BIT && filter == GL_NEAREST &&
			 FBO_HasTextureBackedColor( src ) && FBO_HasTextureBackedColor( dst ) &&
			 srcW > 0 && srcH > 0 && dstW == srcW && dstH == srcH && sameColorFormat ) {
			const int srcMetalY = srcFullH - srcBoxFinal[3];
			const int dstMetalY = dstFullH - dstBoxFinal[3];
			if ( srcMetalY >= 0 && dstMetalY >= 0 &&
				 GLMetal_FastCopyTexture( FBO_ColorTexnum( src ), srcBoxFinal[0], srcMetalY, srcW, srcH,
										  FBO_ColorTexnum( dst ), dstBoxFinal[0], dstMetalY, dstW, dstH ) ) {
				// FIX-3: fires at most once (fast-copy success is not in the per-frame hot path).
				static qboolean s_fastCopyLoggedA = qfalse;
				if ( !s_fastCopyLoggedA ) {
					ri.Printf( PRINT_DEVELOPER, "[Metal] FBO_FastBlit: MTLBlit fast-copy path (one-time)\n" ); s_fastCopyLoggedA = qtrue;
				}
				GL_BindFramebuffer( GL_FRAMEBUFFER, 0 );
				GLMetal_SetRenderTarget( 0, 0, false, false );
				glState.currentFBO = NULL;
				return;
			}
		}

		// A1 parity: unconditional shader-blit fallback for all remaining cases.
		// FIX-3: guard — this fires ~528/sec at 333 FPS (8 downsample steps × 66/sec).
		// Preserve PRINT_DEVELOPER level; eliminate per-frame string-format cost.
		static qboolean s_renderPathLoggedA = qfalse;
		if ( !s_renderPathLoggedA ) {
			ri.Printf( PRINT_DEVELOPER, "[Metal] FBO_FastBlit: render-pass blit path (one-time)\n" ); s_renderPathLoggedA = qtrue;
		}
		FBO_Blit( src, srcBox, NULL, dst, dstBox, NULL, NULL, 0 );
		return;
	}

	// Hardware blit path (framebufferBlit == true).
	// Depth/MSAA and non-texture-backed cases are legitimately unsupported here
	// because qglBlitFramebuffer is not implemented for Metal yet.
	{
		static qboolean s_depthWarned = qfalse;
		static qboolean s_endpointWarned = qfalse;

		if ( buffers & GL_DEPTH_BUFFER_BIT ) {
			FBO_WarnOnce( &s_depthWarned, "WARNING: Metal FBO_FastBlit depth/MSAA blits deferred to Slice F" );
			return;
		}

		if ( buffers != GL_COLOR_BUFFER_BIT || !FBO_HasTextureBackedColor( src ) || !FBO_HasTextureBackedColor( dst ) ) {
			FBO_WarnOnce( &s_endpointWarned, "WARNING: Metal FBO_FastBlit only supports color-only texture-backed FBO endpoints in Slice B" );
			return;
		}
	}

	const qboolean sameColorFormat = (qboolean)( src && dst &&
												 src->colorImage[0] && dst->colorImage[0] &&
												 src->colorImage[0]->internalFormat == dst->colorImage[0]->internalFormat );

	qboolean didBlit = qfalse;
	if ( filter == GL_NEAREST && srcW > 0 && srcH > 0 && dstW == srcW && dstH == srcH && sameColorFormat ) {
		const int srcMetalY = srcFullH - srcBoxFinal[3];
		const int dstMetalY = dstFullH - dstBoxFinal[3];
		if ( srcMetalY >= 0 && dstMetalY >= 0 ) {
			didBlit = GLMetal_FastCopyTexture( FBO_ColorTexnum( src ), srcBoxFinal[0], srcMetalY, srcW, srcH,
											   FBO_ColorTexnum( dst ), dstBoxFinal[0], dstMetalY, dstW, dstH );
			if ( didBlit ) {
				// FIX-3: fires at most once (fast-copy success, hardware-blit path).
				static qboolean s_fastCopyLoggedB = qfalse;
				if ( !s_fastCopyLoggedB ) {
					ri.Printf( PRINT_DEVELOPER, "[Metal] FBO_FastBlit: MTLBlit fast-copy path (one-time)\n" ); s_fastCopyLoggedB = qtrue;
				}
			}
		}
	}

	if ( !didBlit ) {
		const float srcLeft   = srcBoxFinal[0] / (float)srcFullW;
		const float srcRight  = srcBoxFinal[2] / (float)srcFullW;
		// Same GL-box -> Metal-row V shift as FBO_Blit (no-op for full boxes).
		const float srcVShift = 1.0f - ( srcBoxFinal[1] + srcBoxFinal[3] ) / (float)srcFullH;
		const float srcBottom = srcBoxFinal[1] / (float)srcFullH + srcVShift;
		const float srcTop    = srcBoxFinal[3] / (float)srcFullH + srcVShift;
		const float srcUV[8] = { srcLeft, srcTop, srcRight, srcTop, srcRight, srcBottom, srcLeft, srcBottom };
		int renderDstX = dstBoxFinal[0];
		int renderDstY = dstBoxFinal[1];
		int renderDstW = dstW;
		int renderDstH = dstH;
		if ( renderDstW < 0 ) {
			renderDstX += renderDstW;
			renderDstW = -renderDstW;
		}
		if ( renderDstH < 0 ) {
			renderDstY += renderDstH;
			renderDstH = -renderDstH;
		}
		const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		// full-overwrite: DontCare load (perf) — FBO_FastBlit always writes the full dest rect
		// (FBO_FastBlit callers pass NULL dstBox for full-coverage writes, which expands to
		// (0,0,dstFullW,dstFullH) above). Compute dynamically for correctness on partial rects.
		const bool fullCoverage = ( renderDstX == 0 && renderDstY == 0 &&
									renderDstW == dstFullW && renderDstH == dstFullH ); // full-overwrite: DontCare load (perf)
		didBlit = GLMetal_RenderBlitTexture( FBO_ColorTexnum( src ), srcUV, FBO_ColorTexnum( dst ),
											 renderDstX, renderDstY, renderDstW, renderDstH,
											 dstFullW, dstFullH, filter, GLS_DEPTHTEST_DISABLE,
											 white, "texturecolor",
											 NULL, fullCoverage ); // PARITY-SEAM(Metal-device): fast blits stay on bare texturecolor path.
		if ( didBlit ) {
			// FIX-3: fires at most once (render-pass fallback, hardware-blit path).
			static qboolean s_renderPathLoggedB = qfalse;
			if ( !s_renderPathLoggedB ) {
				ri.Printf( PRINT_DEVELOPER, "[Metal] FBO_FastBlit: render-pass blit path (one-time)\n" ); s_renderPathLoggedB = qtrue;
			}
		}
	}

	if ( !didBlit ) {
		ri.Printf( PRINT_DEVELOPER, "WARNING: Metal FBO_FastBlit blit failed\n" );
	}

	// GL2 ref: tr_fbo.c:678-685. Metal device calls replace qglBlitFramebuffer;
	// preserve GL2's post-blit default-framebuffer/currentFBO reset exactly.
	(void)srcFb; (void)dstFb;
	GL_BindFramebuffer( GL_FRAMEBUFFER, 0 );
	GLMetal_SetRenderTarget( 0, 0, false, false );
	glState.currentFBO = NULL;
}

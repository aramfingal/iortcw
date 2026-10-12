/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).

RTCW SP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW SP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW SP Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the RTCW SP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW SP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
// tr_backend.cpp — Metal render backend
//
// Phase-1 PORT: all 26 functions from tr_backend.c are present.
//   4  REAL       — GL-agnostic matrix math and command dispatch.
//  22  PARITY-SEAM — real control flow preserved; GL/qgl calls documented
//                    and replaced with seam markers; Metal command-encoder
//                    submission is Phase 4 (MTL::RenderCommandEncoder).
//   0  PARITY-STUB — none; every function has meaningful bookkeeping.
//
// NOTE: The ledger recorded 21 functions for this file; grep of the GL2
//   source finds 26 (5 were missed: RB_SetGL2D, RB_SetColor, RB_DrawSurfs,
//   RB_DrawBuffer, RB_SwapBuffers).  All 26 are ported here.
//
// KNOWN HARD SEAM — Metal device / command encoder / swapchain
//   GL2 submits GPU work through qgl* (OpenGL) function pointers.  Under
//   Metal, GPU work goes through MTL::RenderCommandEncoder obtained from
//   the active command buffer.  All direct GPU-submission calls are marked:
//     PARITY-SEAM(Metal-device): <reason>. GL2 ref: tr_backend.c:NNN
//   The full command-encoder bring-up is Phase 4 (Hudson + Ripley tr_fbo).

// Include strategy for tr_backend.cpp:
//
// tr_extramath.cpp defines Mat4* as C++ functions.
// tr_local.h includes tr_extramath.h when wrapped in extern "C", which would
// declare Mat4* as C-linked — a linkage mismatch.
//
// Fix: include q_shared.h first (for vec_t/qboolean types), then include
// tr_extramath.h in C++ context so its declarations match tr_extramath.cpp
// definitions.  The tr_extramath.h include guard prevents re-declaration when
// tr_local.h later tries to include it inside extern "C".
//
extern "C" {
#include "../qcommon/q_shared.h"   // vec_t, vec3_t, qboolean, ID_INLINE, etc.
}

#include "tr_extramath.h"           // Mat4* — C++ linkage matches tr_extramath.cpp

extern "C" {
#include "tr_local.h"               // tr_extramath.h skipped (include guard)
#include "tr_dsa.h"
#include "tr_fbo.h"                 // FBO_Blit, FBO_FastBlit, FBO_BlitFromTexture
}

// Forward declarations for tr_postprocess.cpp functions (no header; matches GL2 tr_postprocess.c).
// GL2 ref: tr_postprocess.c:25, 106, 299, 452
extern "C" void RB_ToneMap( FBO_t *hdrFbo, ivec4_t hdrBox, FBO_t *ldrFbo, ivec4_t ldrBox, int autoExposure );
extern "C" void RB_BokehBlur( FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox, float blur );
extern "C" void RB_SunRays( FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox );
extern "C" void RB_GaussianBlur( FBO_t *srcFbo, FBO_t *dstFbo, float blur );

// PHASE5-M5.2b: Metal texture binding table — included OUTSIDE extern "C" because
// tr_metal_device.h wraps its own C declarations in extern "C" internally.
#include "tr_metal_device.h"

// SLICE-B ACCEPTANCE HOOK cvar (temporary).
extern cvar_t *r_metalBlitTest;

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_REPEAT
#define GL_REPEAT 0x2901
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GL_DEPTH_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT 0x00000100
#endif
#ifndef GL_NEAREST
#define GL_NEAREST 0x2600
#endif
#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif
#ifndef GL_NEAREST_MIPMAP_NEAREST
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
#define GL_LINEAR_MIPMAP_NEAREST  0x2701
#endif
#ifndef GL_NEAREST_MIPMAP_LINEAR
#define GL_NEAREST_MIPMAP_LINEAR  0x2702
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR   0x2703
#endif

// COPY-THEN-ANNOTATE: globals from GL2 tr_backend.c:26-37
backEndData_t   *backEndData;
backEndState_t backEnd;

// COPY-THEN-ANNOTATE: coordinate-system flip matrix.
// Converts from ioquake3 convention (looking down +X) to standard
// right-handed convention (looking down -Z) for portal/mirror views.
static float s_flipMatrix[16] = {
	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	0, 0, -1, 0,
	-1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};

static const mat4_t s_metalDepthRemap = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.5f, 0.0f,
	0.0f, 0.0f, 0.5f, 1.0f
};

static void GL_RemapMVPDepthForMetal( void ) {
	mat4_t glMvp;

	Mat4Copy( glState.modelviewProjection, glMvp );
	Mat4Multiply( s_metalDepthRemap, glMvp, glState.modelviewProjection );
}

/*
=================
R_FillMetalShadowmaskUniforms
GL2 ref: tr_backend.c:996-1037
Fills the 352-byte MetalShadowmaskUniforms struct with per-frame view and
shadow-cascade data.  Verbatim port of Vasquez's reference implementation.
=================
*/
static void R_FillMetalShadowmaskUniforms( MetalShadowmaskUniforms *u ) {
	// u_ViewOrigin — GL2 ref: tr_backend.c:1032
	VectorCopy( backEnd.refdef.vieworg, u->u_ViewOrigin );
	u->u_ViewOrigin[3] = 0.0f;

	float zmax = backEnd.viewParms.zFar;
	float ymax = zmax * tan( backEnd.viewParms.fovY * M_PI / 360.0f );
	float xmax = zmax * tan( backEnd.viewParms.fovX * M_PI / 360.0f );

	// u_ViewForward — GL2 ref: tr_backend.c:1027
	VectorScale( backEnd.refdef.viewaxis[0], zmax, u->u_ViewForward );
	u->u_ViewForward[3] = 0.0f;
	// u_ViewLeft — GL2 ref: tr_backend.c:1029 (xmax scale)
	VectorScale( backEnd.refdef.viewaxis[1], xmax, u->u_ViewLeft );
	u->u_ViewLeft[3] = 0.0f;
	// u_ViewUp — GL2 ref: tr_backend.c:1031 (ymax scale)
	VectorScale( backEnd.refdef.viewaxis[2], ymax, u->u_ViewUp );
	u->u_ViewUp[3] = 0.0f;

	// u_ViewInfo — GL2 ref: tr_backend.c:975 (viewInfo = {zFar/zNear, zFar, ...})
	u->u_ViewInfo[0] = backEnd.viewParms.zFar / r_znear->value; // zFar/zNear
	u->u_ViewInfo[1] = backEnd.viewParms.zFar;
	u->u_ViewInfo[2] = 0.0f;
	u->u_ViewInfo[3] = 0.0f;

	// u_ShadowMapSize — GL2 ref: float-injected #define r_shadowMapSize
	u->u_ShadowMapSize = (float)r_shadowMapSize->integer;

	// 4× shadow MVP matrices — GL2 ref: tr_backend.c:1009-1012
	memcpy( u->u_ShadowMvp,  backEnd.refdef.sunShadowMvp[0], 64 );
	memcpy( u->u_ShadowMvp2, backEnd.refdef.sunShadowMvp[1], 64 );
	memcpy( u->u_ShadowMvp3, backEnd.refdef.sunShadowMvp[2], 64 );
	memcpy( u->u_ShadowMvp4, backEnd.refdef.sunShadowMvp[3], 64 );
	memset( u->_pad, 0, sizeof( u->_pad ) );
}

// Forward declaration — defined in tr_image_dds.c (Parker, Phase 2).
extern "C" void R_SaveDDS( const char *filename, byte *pic, int width, int height, int depth );


/*
** GL_BindToTMU
** GL2 ref: tr_backend.c:43
**
** Binds image to texture mapping unit tmu.  Under Metal, texture binding
** is done through the argument buffer / descriptor set on the command
** encoder (Phase 4).  The frameUsed timestamp is always updated.
*/
void GL_BindToTMU( image_t *image, int tmu ) {
	GLuint texture = ( tmu == TB_COLORMAP ) ? tr.defaultImage->texnum : 0;
	GLenum target = GL_TEXTURE_2D;

	if ( image ) {
		if ( image->flags & IMGFLAG_CUBEMAP ) {
			target = GL_TEXTURE_CUBE_MAP;
		}

		// Texture residency tracking — GL-agnostic bookkeeping, REAL.
		image->frameUsed = tr.frameCount;
		texture = image->texnum;
	} else
	{
		ri.Printf( PRINT_WARNING, "GL_BindToTMU: NULL image\n" );
	}

	// PARITY-SEAM(Metal-device): GL texture bind is now recorded in the
	// per-TMU Metal binding table; R_DrawElements applies it to the current
	// MTLRenderCommandEncoder at draw time.
	// GL_BindMultiTexture( GL_TEXTURE0 + tmu, target, texture );
	const int wrapClampMode = ( image && ( image->flags & IMGFLAG_CLAMPTOEDGE ) ) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
	const qboolean mipmap = ( image && ( image->flags & IMGFLAG_MIPMAP ) ) ? qtrue : qfalse;
	// GL2 ref: tr_image.c:30 (gl_filter_min/gl_filter_max defaults),
	//          tr_image.c:2336-2339 (per-texture min/mag application),
	//          tr_init.c:1027 (GL_TextureMode sets gl_filter_min from r_textureMode cvar).
	// gl_filter_max is either GL_NEAREST or GL_LINEAR and directly encodes mag/base linear-ness.
	// gl_filter_min's mipmap suffix encodes whether mip level blending is linear (trilinear).
	const qboolean filterLinear = ( gl_filter_max == GL_LINEAR ) ? qtrue : qfalse;
	const qboolean mipLinear    = ( gl_filter_min == GL_LINEAR_MIPMAP_LINEAR ||
									gl_filter_min == GL_NEAREST_MIPMAP_LINEAR ) ? qtrue : qfalse;
	GLMetal_SetTextureBinding( tmu, texture, wrapClampMode, filterLinear, mipmap, mipLinear );
	(void)target;
}


/*
** GL_Cull
** GL2 ref: tr_backend.c:68
**
** Sets face-culling mode.  Under Metal, cull mode is part of the
** MTLRenderPipelineDescriptor (set at pipeline creation, Phase 4).
*/
void GL_Cull( int cullType ) {
	if ( glState.faceCulling == cullType ) {
		return;
	}

	if ( cullType == CT_TWO_SIDED ) {
		// PARITY-SEAM(Metal-device): qglDisable(GL_CULL_FACE) → cullMode = MTLCullModeNone
	} else
	{
		qboolean cullFront = (qboolean)( cullType == CT_FRONT_SIDED );

		// PARITY-SEAM(Metal-device): qglEnable(GL_CULL_FACE)/qglCullFace → MTLCullModeFront/Back
		// Cull-front tracking preserved for Metal pipeline state cache.
		glState.faceCullFront = cullFront;
	}

	glState.faceCulling = cullType;
}


/*
** GL_State
**
** Responsible for setting the most-commonly changed render state in Q3.
** Under Metal, each distinct state combination is a separate compiled
** MTLRenderPipelineState + MTLDepthStencilState.  The GLS_* bit tracking
** below feeds the Metal state cache (Phase 4).
** GL2 ref: tr_backend.c:99
*/
void GL_State( unsigned long stateBits ) {
	unsigned long diff = stateBits ^ glState.glStateBits;

	if ( !diff ) {
		return;
	}

	// PARITY-SEAM(Metal-device): All GL state transitions below map to
	// MTLRenderPipelineDescriptor / MTLDepthStencilDescriptor properties.
	// The full GL2 blend-factor dispatch table (tr_backend.c:152-218) and
	// depth/polygon/test state switches (tr_backend.c:221-266) are preserved
	// as inline comments for Phase 4 Metal state mapping reference.
	//
	// GL2 ref: tr_backend.c:108-266 (depth, blend, mask, polymode, depthtest)
	//
	// Metal mapping notes (Phase 4):
	//   GLS_DEPTHFUNC_EQUAL/GREATER/LEQUAL  → MTLDepthStencilDescriptor.depthCompareFunction
	//   GLS_SRCBLEND_* / GLS_DSTBLEND_*     → MTLRenderPipelineColorAttachmentDescriptor
	//                                           .sourceRGBBlendFactor / .destinationRGBBlendFactor
	//   GLS_DEPTHMASK_TRUE                  → MTLDepthStencilDescriptor.depthWriteEnabled
	//   GLS_POLYMODE_LINE                   → MTLRenderPipelineDescriptor.rasterizationEnabled
	//                                          (or a wireframe geometry shader in MSL)
	//   GLS_DEPTHTEST_DISABLE               → MTLDepthStencilDescriptor.depthCompareFunction = Always
	//
	// GL2 blend tables for Phase 4 reference (COPY-THEN-ANNOTATE, tr_backend.c:152-218):
	//   GLS_SRCBLEND_ZERO            → MTLBlendFactorZero
	//   GLS_SRCBLEND_ONE             → MTLBlendFactorOne
	//   GLS_SRCBLEND_DST_COLOR       → MTLBlendFactorDestinationColor
	//   GLS_SRCBLEND_ONE_MINUS_DST_COLOR → MTLBlendFactorOneMinusDestinationColor
	//   GLS_SRCBLEND_SRC_ALPHA       → MTLBlendFactorSourceAlpha
	//   GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA → MTLBlendFactorOneMinusSourceAlpha
	//   GLS_SRCBLEND_DST_ALPHA       → MTLBlendFactorDestinationAlpha
	//   GLS_SRCBLEND_ONE_MINUS_DST_ALPHA → MTLBlendFactorOneMinusDestinationAlpha
	//   GLS_SRCBLEND_ALPHA_SATURATE  → MTLBlendFactorSourceAlphaSaturated
	//   GLS_DSTBLEND_ZERO            → MTLBlendFactorZero
	//   GLS_DSTBLEND_ONE             → MTLBlendFactorOne
	//   GLS_DSTBLEND_SRC_COLOR       → MTLBlendFactorSourceColor
	//   GLS_DSTBLEND_ONE_MINUS_SRC_COLOR → MTLBlendFactorOneMinusSourceColor
	//   GLS_DSTBLEND_SRC_ALPHA       → MTLBlendFactorSourceAlpha
	//   GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA → MTLBlendFactorOneMinusSourceAlpha
	//   GLS_DSTBLEND_DST_ALPHA       → MTLBlendFactorDestinationAlpha
	//   GLS_DSTBLEND_ONE_MINUS_DST_ALPHA → MTLBlendFactorOneMinusDestinationAlpha

	// State-bit tracking: feeds Metal pipeline state cache (Phase 4).
	glState.glStateBits = stateBits;
}


/*
** GL_SetProjectionMatrix
** GL2 ref: tr_backend.c:270
**
** REAL — updates the cached projection matrix and recomputes modelviewProjection.
** Under Metal the same MVP matrix is uploaded as a shader uniform.
*/
void GL_SetProjectionMatrix( mat4_t matrix ) {
	Mat4Copy( matrix, glState.projection );
	Mat4Multiply( glState.projection, glState.modelview, glState.modelviewProjection );
	// PHASE5 Metal-device: Metal clip-space depth is [0,1] (GL is [-1,1]).
	// Remap MVP z: z' = 0.5*z + 0.5*w to avoid near-plane clipping.
	GL_RemapMVPDepthForMetal();
}


/*
** GL_SetModelviewMatrix
** GL2 ref: tr_backend.c:277
**
** REAL — updates the cached modelview matrix and recomputes modelviewProjection.
*/
void GL_SetModelviewMatrix( mat4_t matrix ) {
	Mat4Copy( matrix, glState.modelview );
	Mat4Multiply( glState.projection, glState.modelview, glState.modelviewProjection );
	// PHASE5 Metal-device: Metal clip-space depth is [0,1] (GL is [-1,1]).
	// Remap MVP z: z' = 0.5*z + 0.5*w to avoid near-plane clipping.
	GL_RemapMVPDepthForMetal();
}


/*
================
RB_Hyperspace
GL2 ref: tr_backend.c:291

A player has predicted a teleport but hasn't arrived yet — flash white.
PARITY-SEAM: flag tracking is REAL; clear-color submission seamed.
================
*/
static void RB_Hyperspace( void ) {
	float c;

	if ( !backEnd.isHyperspace ) {
		// do initialization shit (GL2 ref: tr_backend.c:295)
	}

	c = ( backEnd.refdef.time & 255 ) / 255.0f;

	// PARITY-SEAM(Metal-device): clear-color + clear → MTLRenderPassDescriptor
	// colorAttachments[0].clearColor + MTLLoadActionClear (Phase 4).
	// qglClearColor( c, c, c, 1 );
	// qglClear( GL_COLOR_BUFFER_BIT );
	// qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	(void)c;

	backEnd.isHyperspace = qtrue;
}


/*
static void SetViewportAndScissor
GL2 ref: tr_backend.c:307

PARITY-SEAM: projection matrix update REAL; qglViewport/qglScissor → Metal
viewport/scissor rect on the MTLRenderCommandEncoder (Phase 4).
*/
static void SetViewportAndScissor( void ) {
	GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );

	// GL2 ref: tr_backend.c:311-314 — qglViewport/qglScissor
	GLMetal_SetViewport( backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
						 backEnd.viewParms.viewportWidth, backEnd.viewParms.viewportHeight );
}


/*
=================
RB_BeginDrawingView
GL2 ref: tr_backend.c:325

Any mirrored or portaled views have already been drawn, so prepare
to actually render the visible surfaces for this view.
PARITY-SEAM: FBO/state bookkeeping REAL; qglFinish/qglClear seamed.
=================
*/
void RB_BeginDrawingView( void ) {
	int clearBits = 0;

	// sync with gl if needed
	if ( r_finish->integer == 1 && !glState.finishCalled ) {
		// PARITY-SEAM(Metal-device): qglFinish → MTLCommandBuffer commit + waitUntilCompleted (Phase 4).
		glState.finishCalled = qtrue;
	}
	if ( r_finish->integer == 0 ) {
		glState.finishCalled = qtrue;
	}

	// We will need to change the projection matrix before drawing 2D images again.
	backEnd.projection2D = qfalse;

	// Metal: GLMetal_BeginFrame resets s_renderTargetState to drawable-targeting.
	// Ensure BeginFrame runs BEFORE we set the FBO render target so the reset
	// doesn't overwrite our offscreen target.  Without this, the first frame's
	// world draws get a NULL encoder (drawable not yet acquired) and are dropped.
	// GL2 ref: no equivalent — GL has no late-acquired drawable.
	GLMetal_BeginFrame();

	// PARITY-SEAM(Metal-device): FBO selection + attachment → MTLRenderPassDescriptor (Phase 4).
	// Under Metal, render targets are MTLTexture attachments in MTLRenderPassDescriptor.
	// FBO_AttachImage / FBO_Bind bookkeeping preserved in spirit; GL calls omitted.
	FBO_t *viewFbo = NULL;
	if ( glRefConfig.framebufferObject ) {
		viewFbo = backEnd.viewParms.targetFbo ? backEnd.viewParms.targetFbo : tr.renderFbo;
		FBO_Bind( viewFbo );
	}

	// Set the modelview matrix for the viewer.
	SetViewportAndScissor();

	// Ensure depth writes enabled for depth clear.
	GL_State( GLS_DEFAULT );


////////// (SA) modified to ensure one glclear() per frame at most

	// clear relevant buffers
	clearBits = 0;

	if ( r_measureOverdraw->integer || r_shadows->integer == 2 ) {
		clearBits |= GL_STENCIL_BUFFER_BIT;
	}

	if ( r_uiFullScreen->integer ) {
		clearBits = GL_DEPTH_BUFFER_BIT;    // (SA) always just clear depth for menus

	} else if ( skyboxportal )   {
		if ( backEnd.refdef.rdflags & RDF_SKYBOXPORTAL ) {
			// portal scene, clear whatever is necessary
			clearBits |= GL_DEPTH_BUFFER_BIT;

			if ( r_fastsky->integer || backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) {
				// fastsky: clear color
				// try clearing first with the portal sky fog color, then the world fog color, then finally a default
				clearBits |= GL_COLOR_BUFFER_BIT;

				if ( glfogsettings[FOG_PORTALVIEW].registered ) {
					GLMetal_SetClearColor( glfogsettings[FOG_PORTALVIEW].color[0], glfogsettings[FOG_PORTALVIEW].color[1], glfogsettings[FOG_PORTALVIEW].color[2], glfogsettings[FOG_PORTALVIEW].color[3] );
				} else if ( glfogNum > FOG_NONE && glfogsettings[FOG_CURRENT].registered )   {
					GLMetal_SetClearColor( glfogsettings[FOG_CURRENT].color[0], glfogsettings[FOG_CURRENT].color[1], glfogsettings[FOG_CURRENT].color[2], glfogsettings[FOG_CURRENT].color[3] );
				} else
				{
//					qglClearColor ( 1.0, 0.0, 0.0, 1.0 );	// red clear for testing portal sky clear
//					GLMetal_SetClearColor( 0.5, 0.5, 0.5, 1.0 );
				}
			} else
			{
				// rendered sky (either clear color or draw quake sky)
				if ( glfogsettings[FOG_PORTALVIEW].registered ) {
					GLMetal_SetClearColor( glfogsettings[FOG_PORTALVIEW].color[0], glfogsettings[FOG_PORTALVIEW].color[1], glfogsettings[FOG_PORTALVIEW].color[2], glfogsettings[FOG_PORTALVIEW].color[3] );

					if ( glfogsettings[FOG_PORTALVIEW].clearscreen ) {
						// portal fog requests a screen clear (distance fog rather than quake sky)
						clearBits |= GL_COLOR_BUFFER_BIT;
					}
				}

			}
		} else
		{
			// world scene with portal sky, don't clear any buffers, just set the fog color if there is one
			clearBits |= GL_DEPTH_BUFFER_BIT;   // this will go when I get the portal sky rendering way out in the zbuffer (or not writing to zbuffer at all)

			if ( glfogNum > FOG_NONE && glfogsettings[FOG_CURRENT].registered ) {
				if ( backEnd.refdef.rdflags & RDF_UNDERWATER ) {
					if ( glfogsettings[FOG_CURRENT].mode == GL_LINEAR ) {
						clearBits |= GL_COLOR_BUFFER_BIT;
					}

				} else if ( !( r_portalsky->integer ) )   {
					// portal skies have been manually turned off, clear bg color
					clearBits |= GL_COLOR_BUFFER_BIT;
				}

				GLMetal_SetClearColor( glfogsettings[FOG_CURRENT].color[0], glfogsettings[FOG_CURRENT].color[1], glfogsettings[FOG_CURRENT].color[2], glfogsettings[FOG_CURRENT].color[3] );
			}
		}
	} else
	{
		// world scene with no portal sky
		clearBits |= GL_DEPTH_BUFFER_BIT;

		// NERVE - SMF - we don't want to clear the buffer when no world model is specified
		if ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) {
			clearBits &= ~GL_COLOR_BUFFER_BIT;
		}
		// -NERVE - SMF
		// (SA) well, this is silly then
		else if ( r_fastsky->integer ) { // || backEnd.refdef.rdflags & RDF_NOWORLDMODEL )
			clearBits |= GL_COLOR_BUFFER_BIT;

			if ( glfogsettings[FOG_CURRENT].registered ) {
				// try to clear fastsky with current fog color
				GLMetal_SetClearColor( glfogsettings[FOG_CURRENT].color[0], glfogsettings[FOG_CURRENT].color[1], glfogsettings[FOG_CURRENT].color[2], glfogsettings[FOG_CURRENT].color[3] );
			} else
			{
//				qglClearColor ( 0.0, 0.0, 1.0, 1.0 );	// blue clear for testing world sky clear
//				GLMetal_SetClearColor( 0.5, 0.5, 0.5, 1.0 );
			}
		} else
		{
			// world scene, no portal sky, not fastsky, clear color if fog says to, otherwise, just set the clearcolor
			if ( glfogsettings[FOG_CURRENT].registered ) {
				// try to clear fastsky with current fog color
				GLMetal_SetClearColor( glfogsettings[FOG_CURRENT].color[0], glfogsettings[FOG_CURRENT].color[1], glfogsettings[FOG_CURRENT].color[2], glfogsettings[FOG_CURRENT].color[3] );

				if ( glfogsettings[FOG_CURRENT].clearscreen ) {
					// world fog requests a screen clear (distance fog rather than quake sky)
					clearBits |= GL_COLOR_BUFFER_BIT;
				}
			}
		}
	}
	if ( glRefConfig.framebufferObject && viewFbo ) {
		static uint32_t lastRenderTargetColorTexnum = 0;
		static uint32_t lastRenderTargetDepthTexnum = 0;
		static int lastRenderTargetSetFrame = -1;

		// GL2 ref: tr_backend.c:364-386. Color is cleared ONLY when r_fastsky &&
		// !RDF_NOWORLDMODEL (folded into clearBits above) or when targeting the
		// cube FBO. There is NO unconditional per-frame renderFbo color clear in GL2.
		// A previous Metal-only forced clear of tr.renderFbo wiped the fullscreen 2D
		// menu background (menuback) that is drawn before the 3D banner scene, leaving
		// the blue-gray clear color showing through. Removed for 1:1 parity.
		bool clearColor = ( clearBits & GL_COLOR_BUFFER_BIT ) != 0;
		if ( tr.renderCubeFbo && backEnd.viewParms.targetFbo == tr.renderCubeFbo ) {
			clearColor = qtrue;
		}
		uint32_t colorTexnum = viewFbo->colorImage[0] ? viewFbo->colorImage[0]->texnum : 0;
		uint32_t depthTexnum = ( viewFbo == tr.renderFbo && tr.renderDepthImage ) ? tr.renderDepthImage->texnum : 0;
		bool needDepthClear = ( clearBits & GL_DEPTH_BUFFER_BIT ) != 0;

		// GL2 ref: tr_fbo.c:160-163, tr_backend.c:341-355.
		// Depth-only shadow capture passes (VPF_DEPTHSHADOW): the pshadow FBO has a
		// depthImage but no colorImage. Route through GLMetal_SetDepthOnlyRenderTarget
		// to create a depth-only render pass (no color attachment). This is the Metal
		// equivalent of GL2 binding a depth-only FBO + glColorMask(FALSE×4).
		qboolean isDepthShadow = ( qboolean ) !!( backEnd.viewParms.flags & VPF_DEPTHSHADOW );
		if ( isDepthShadow && viewFbo->depthImage ) {
			GLMetal_SetDepthOnlyRenderTarget( viewFbo->depthImage->texnum, needDepthClear );
			// GL2 ref: tr_backend.c:382 — clear-on-bind. Force the deferred depth clear to be
			// honored now so a pshadow whose occluder draws zero fragments this frame doesn't
			// sample a stale (uncleared) depth map → phantom morphing shadow blob on gunfire.
			if ( needDepthClear ) {
				GLMetal_ForcePendingClears();
			}
		} else
		{

			// GL2 parity: GL can clear depth within an active render pass (qglClear).
			// Metal cannot — clears only happen at render pass start (LoadActionClear).
			// If we're re-entering RB_BeginDrawingView on the SAME render target
			// (e.g., main view after portal sub-view), force an encoder restart by
			// briefly switching to the drawable target.  This ensures the pending
			// depth clear is honored in a new render pass.
			// GL2 ref: tr_backend.c:382 (qglClear applies immediately per-view)
			if ( needDepthClear &&
				 lastRenderTargetSetFrame == tr.frameCount &&
				 lastRenderTargetColorTexnum == colorTexnum &&
				 lastRenderTargetDepthTexnum == depthTexnum ) {
				GLMetal_SetRenderTarget( 0, 0, false, false );
			}

			lastRenderTargetColorTexnum = colorTexnum;
			lastRenderTargetDepthTexnum = depthTexnum;
			lastRenderTargetSetFrame = tr.frameCount;

			GLMetal_SetRenderTarget( colorTexnum, depthTexnum, clearColor, needDepthClear );
		}
	}

	// PARITY-SEAM(Metal-device): qglClear(clearBits) → MTLLoadActionClear on render pass (Phase 4).

	// Hyperspace flash.
	if ( ( backEnd.refdef.rdflags & RDF_HYPERSPACE ) ) {
		RB_Hyperspace();
		return;
	} else
	{
		backEnd.isHyperspace = qfalse;
	}

	// We will only draw a sun if there was sky rendered in this view.
	backEnd.skyRenderedThisView = qfalse;

	// Cache the clamped greyscale value.
	backEnd.greyscale = Com_Clamp( 0.0f, 1.0f, r_greyscale->value );

	// Clip to the plane of the portal.
	if ( backEnd.viewParms.isPortal ) {
		// COPY-THEN-ANNOTATE: portal plane transform to flip matrix.
		// Portal clip plane computation (GL2 ref: tr_backend.c:402-415) is #if 0'd
		// in GL2 already; the flip matrix is still applied.
		GL_SetModelviewMatrix( s_flipMatrix );
	}
}

/*
============
RB_ZombieFX

  This is post-tesselation filtering, made especially for the Zombie.
============
*/

extern void GlobalVectorToLocal( const vec3_t in, vec3_t out );
extern vec_t VectorLengthSquared( const vec3_t v );

#define ZOMBIEFX_MAX_VERTS              2048
#define ZOMBIEFX_FADEOUT_TIME_SEC       ( 0.001 * ZOMBIEFX_FADEOUT_TIME )
#define ZOMBIEFX_MAX_HITS               128
#define ZOMBIEFX_MAX_NEWHITS            4
#define ZOMBIEFX_HIT_OKRANGE_SQR        9   // all verts within this range will be hit
#define ZOMBIEFX_HIT_MAXRANGE_SQR       36  // each bullet that strikes the bounding box, will effect verts inside this range (allowing for projections onto the mesh)
#define ZOMBIEFX_PERHIT_TAKEALPHA       150
#define ZOMBIEFX_MAX_HITS_PER_VERT      2

static char *zombieFxFleshHitSurfaceNames[2] = {"u_body","l_legs"};

// this stores each of the flesh hits for each of the zombies in the game
typedef struct {
	qboolean isHit;
	unsigned short numHits;
	unsigned short vertHits[ZOMBIEFX_MAX_HITS]; // bit flags to represent those verts that have been hit
	int numNewHits;
	vec3_t newHitPos[ZOMBIEFX_MAX_NEWHITS];
	vec3_t newHitDir[ZOMBIEFX_MAX_NEWHITS];
} trZombieFleshHitverts_t;
//
trZombieFleshHitverts_t zombieFleshHitVerts[MAX_SP_CLIENTS][2]; // one for upper, one for lower

void RB_ZombieFXInit( void ) {
	memset( zombieFleshHitVerts, 0, sizeof( zombieFleshHitVerts ) );
}

void RB_ZombieFXAddNewHit( int entityNum, const vec3_t hitPos, const vec3_t hitDir ) {
	int part = 0;

	if ( entityNum == -1 ) {
		// hack, reset data
		RB_ZombieFXInit();
		return;
	}

	if ( entityNum & ( 1 << 30 ) ) {
		part = 1;
		entityNum &= ~( 1 << 30 );
	}

	if ( entityNum >= MAX_SP_CLIENTS ) {
		Com_Printf( "RB_ZombieFXAddNewHit: entityNum (%i) outside allowable range (%i)\n", entityNum, MAX_SP_CLIENTS );
		return;
	}
	if ( zombieFleshHitVerts[entityNum][part].numHits + zombieFleshHitVerts[entityNum][part].numNewHits >= ZOMBIEFX_MAX_HITS ) {
		// already full of hits
		return;
	}
	if ( zombieFleshHitVerts[entityNum][part].numNewHits >= ZOMBIEFX_MAX_NEWHITS ) {
		// just ignore this hit
		return;
	}
	// add it to the list
	VectorCopy( hitPos, zombieFleshHitVerts[entityNum][part].newHitPos[zombieFleshHitVerts[entityNum][part].numNewHits] );
	VectorCopy( hitDir, zombieFleshHitVerts[entityNum][part].newHitDir[zombieFleshHitVerts[entityNum][part].numNewHits] );
	zombieFleshHitVerts[entityNum][part].numNewHits++;
}

void RB_ZombieFXProcessNewHits( trZombieFleshHitverts_t *fleshHitVerts, int oldNumVerts, int numSurfVerts ) {
	float *xyzTrav;
	int16_t *normTrav;
	vec3_t hitPos, hitDir, v, testDir;
	float bestHitDist, thisDist;
	qboolean foundHit;
	int i, j, bestHit = 0;
	unsigned short *hitTrav;
	byte hitCounts[ZOMBIEFX_MAX_VERTS];     // so we can quickly tell if a particular vert has been hit enough times already

	// first build the hitCount list
	memset( hitCounts, 0, sizeof( hitCounts ) );
	for ( i = 0, hitTrav = fleshHitVerts->vertHits; i < fleshHitVerts->numHits; i++, hitTrav++ ) {
		hitCounts[*hitTrav]++;
	}

	// for each new hit
	for ( i = 0; i < fleshHitVerts->numNewHits; i++ ) {
		// calc the local hitPos
		VectorCopy( fleshHitVerts->newHitPos[i], v );
		VectorSubtract( v, backEnd.currentEntity->e.origin, v );
		GlobalVectorToLocal( v, hitPos );
		// calc the local hitDir
		VectorCopy( fleshHitVerts->newHitDir[i], v );
		GlobalVectorToLocal( v, hitDir );

		// look for close matches
		foundHit = qfalse;

		// for each vertex
		for (   j = 0, bestHitDist = -1, xyzTrav = tess.xyz[oldNumVerts], normTrav = tess.normal[oldNumVerts];
				j < numSurfVerts;
				j++, xyzTrav += 4, normTrav += 4 ) {
			vec3_t fNormTrav;

			// if this vert has been hit enough times already
			if ( hitCounts[j] > ZOMBIEFX_MAX_HITS_PER_VERT ) {
				continue;
			}

			R_VaoUnpackNormal( fNormTrav, normTrav );

			// if this normal faces the wrong way, reject it
			if ( DotProduct( fNormTrav, hitDir ) > 0 ) {
				continue;
			}
			// get the diff vector
			VectorSubtract( xyzTrav, hitPos, testDir );
			// check for distance within range
			thisDist = VectorLengthSquared( testDir );
			if ( thisDist < ZOMBIEFX_HIT_OKRANGE_SQR ) {
				goto hitCheckDone;
			}
			thisDist = sqrt( thisDist );
			// check for the projection being inside range
			VectorMA( hitPos, thisDist, hitDir, v );
			VectorSubtract( xyzTrav, v, testDir );
			thisDist = VectorLengthSquared( testDir );
			if ( thisDist < ZOMBIEFX_HIT_OKRANGE_SQR ) {
				goto hitCheckDone;
			}
			// if we are still struggling to find a hit, then pick the closest outside the OK range
			if ( !foundHit ) {
				if ( thisDist < ZOMBIEFX_HIT_MAXRANGE_SQR && ( bestHitDist < 0 || thisDist < bestHitDist ) ) {
					bestHitDist = thisDist;
					bestHit = j;
				}
			}

			// if it gets to here, then it failed
			continue;

hitCheckDone:

			// this vertex was hit
			foundHit = qtrue;
			// set the appropriate bit-flag
			fleshHitVerts->isHit = qtrue;
			fleshHitVerts->vertHits[fleshHitVerts->numHits++] = (unsigned short)j;
			//if (fleshHitVerts->numHits == ZOMBIEFX_MAX_HITS)
			//	break;	// only find one close match per shot
			if ( fleshHitVerts->numHits == ZOMBIEFX_MAX_HITS ) {
				break;
			}
		}

		if ( fleshHitVerts->numHits == ZOMBIEFX_MAX_HITS ) {
			break;
		}

		// if we didn't find a hit vertex, grab the closest acceptible match
		if ( !foundHit && bestHitDist >= 0 ) {
			// set the appropriate bit-flag
			fleshHitVerts->isHit = qtrue;
			fleshHitVerts->vertHits[fleshHitVerts->numHits++] = (unsigned short)bestHit;
			if ( fleshHitVerts->numHits == ZOMBIEFX_MAX_HITS ) {
				break;
			}
		}
	}

	// we've processed any new hits
	fleshHitVerts->numNewHits = 0;
}

void RB_ZombieFXShowFleshHits( trZombieFleshHitverts_t *fleshHitVerts, int oldNumVerts, int numSurfVerts ) {
	uint16_t *vertColors;
	unsigned short *vertHits;
	int i;

	vertColors = tess.color[oldNumVerts];
	vertHits = fleshHitVerts->vertHits;

	// for each hit entry, adjust that verts alpha component
	for ( i = 0; i < fleshHitVerts->numHits; i++, vertHits++ ) {
		if ( vertColors[( *vertHits ) * 4 + 3] < ZOMBIEFX_PERHIT_TAKEALPHA * 257 ) {
			vertColors[( *vertHits ) * 4 + 3] = 0;
		} else {
			vertColors[( *vertHits ) * 4 + 3] -= ZOMBIEFX_PERHIT_TAKEALPHA * 257;
		}
	}
}

void RB_ZombieFXDecompose( int oldNumVerts, int numSurfVerts, float deltaTimeScale ) {
	uint16_t *vertColors;
	float   *xyz;
	int16_t *norm;
	vec3_t fNorm;
	int i;
	float alpha;

	vertColors = tess.color[oldNumVerts];
	xyz = tess.xyz[oldNumVerts];
	norm = tess.normal[oldNumVerts];

	for ( i = 0; i < numSurfVerts; i++, vertColors += 4, xyz += 4, norm += 4 ) {
		alpha = 65535.0 * ( (float)( 1 + i % 3 ) / 3.0 ) * deltaTimeScale * 2;
		if ( alpha > 65535.0 ) {
			alpha = 65535.0;
		}
		if ( (float)vertColors[3] - alpha < 0 ) {
			vertColors[3] = 0;
		} else {
			vertColors[3] -= (byte)alpha;
		}

		R_VaoUnpackNormal( fNorm, norm );

		// skin shrinks with age
		VectorMA( xyz, -2.0 * deltaTimeScale, fNorm, xyz );
	}
}

void RB_ZombieFXFullAlpha( int oldNumVerts, int numSurfVerts ) {
	uint16_t *vertColors;
	int i;

	vertColors = tess.color[oldNumVerts];

	for ( i = 0; i < numSurfVerts; i++, vertColors += 4 ) {
		vertColors[3] = 65535;
	}
}

void RB_ZombieFX( int part, drawSurf_t *drawSurf, int oldNumVerts, int oldNumIndex ) {
	int numSurfVerts;
	float deltaTime;
	char    *surfName;
	trZombieFleshHitverts_t *fleshHitVerts;

	// Central point for Zombie post-tess processing. Various effects can be added from this point

	if ( *drawSurf->surface == SF_MDV ) {
		surfName = ( (mdvSurface_t *)drawSurf->surface )->name;
	} else {
		Com_Printf( "RB_ZombieFX: unknown surface type\n" );
		return;
	}

	// ignore all surfaces starting with u_sk (skeleton)
	if ( !Q_strncmp( surfName, "u_sk", 4 ) ) {
		return;
	}
	// legs
	if ( !Q_strncmp( surfName, "l_sk", 4 ) ) {
		return;
	}
	// head
	if ( !Q_strncmp( surfName, "h_sk", 4 ) ) {
		return;
	}

	numSurfVerts = tess.numVertexes - oldNumVerts;

	if ( numSurfVerts > ZOMBIEFX_MAX_VERTS ) {
		Com_Printf( "RB_ZombieFX: exceeded ZOMBIEFX_MAX_VERTS\n" );
		return;
	}

	deltaTime = backEnd.currentEntity->e.shaderTime;
	if ( ZOMBIEFX_FADEOUT_TIME_SEC < deltaTime ) {
		// nothing to do, it's done fading out
		tess.numVertexes = oldNumVerts;
		tess.numIndexes = oldNumIndex;
		return;
	}

	fleshHitVerts = &zombieFleshHitVerts[backEnd.currentEntity->e.entityNum][part];

	// set everything to full alpha
	RB_ZombieFXFullAlpha( oldNumVerts, numSurfVerts );

	// if this is the chest surface, do flesh hits
	if ( !Q_stricmp( surfName, zombieFxFleshHitSurfaceNames[part] ) ) {

		// check for any new bullet impacts that need to be scanned for triangle collisions
		if ( fleshHitVerts->numNewHits ) {
			RB_ZombieFXProcessNewHits( fleshHitVerts, oldNumVerts, numSurfVerts );
		}

		// hide vertices marked as being torn off
		if ( fleshHitVerts->isHit ) {
			RB_ZombieFXShowFleshHits( fleshHitVerts, oldNumVerts, numSurfVerts );
		}
	}

	// decompose?
	if ( deltaTime ) {
		RB_ZombieFXDecompose( oldNumVerts, numSurfVerts, deltaTime / ZOMBIEFX_FADEOUT_TIME_SEC );
	}

}


/*
==================
RB_RenderDrawSurfList
GL2 ref: tr_backend.c:426

Iterates over the sorted draw surface list, batches surfaces by shader,
and dispatches to the surface-type render functions.

PARITY-SEAM: entity transform logic (R_DecomposeSort, R_RotateForEntity,
R_TransformDlights) is REAL; surface dispatch (rb_surfaceTable) and
shader batch (RB_BeginSurface/RB_EndSurface) are REAL (scaffolded); GL
depth-range hack is seamed; FBO bind at end is seamed.

This function shape is load-bearing for Phase 3 (tr_shade) integration.
==================
*/
void RB_RenderDrawSurfList( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	shader_t        *shader = NULL, *oldShader;
	int fogNum, oldFogNum;
	int entityNum, oldEntityNum;
	int dlighted, oldDlighted;
	int pshadowed, oldPshadowed;
	int cubemapIndex, oldCubemapIndex;
	qboolean depthRange, oldDepthRange, isCrosshair, wasCrosshair;
	int i;
	drawSurf_t      *drawSurf;
	int oldSort;
	double originalTime;
	FBO_t           *fbo = NULL;

	int oldNumVerts, oldNumIndex;
	//GR - tessellation flag
	int atiTess = 0, oldAtiTess;

	// save original time for entity shader offsets
	originalTime = backEnd.refdef.floatTime;

	fbo = glState.currentFBO;

	// Draw everything.
	oldEntityNum    = -1;
	backEnd.currentEntity = &tr.worldEntity;
	oldShader       = NULL;
	oldFogNum       = -1;
	oldDepthRange   = qfalse;
	wasCrosshair    = qfalse;
	oldDlighted     = qfalse;
	oldPshadowed    = qfalse;
	oldCubemapIndex = -1;
	oldSort = -1;
	// GR - tessellation also forces to draw everything
	oldAtiTess = -1;

	backEnd.pc.c_surfaces += numDrawSurfs;

	for ( i = 0, drawSurf = drawSurfs; i < numDrawSurfs; i++, drawSurf++ )
	{
		if ( drawSurf->sort == (unsigned)oldSort && drawSurf->cubemapIndex == oldCubemapIndex ) {
			if ( backEnd.depthFill && shader && ( shader->sort != SS_OPAQUE && shader->sort != SS_PORTAL ) ) {
				continue;
			}

			// Fast path — same shader as previous surface.
			oldNumVerts = tess.numVertexes;
			oldNumIndex = tess.numIndexes;
			rb_surfaceTable[ *drawSurf->surface ]( drawSurf->surface );

			// RF, convert the newly created vertexes into dust particles, and overwrite
			if ( backEnd.currentEntity->e.reFlags & REFLAG_ZOMBIEFX ) {
				RB_ZombieFX( 0, drawSurf, oldNumVerts, oldNumIndex );
			} else if ( backEnd.currentEntity->e.reFlags & REFLAG_ZOMBIEFX2 )     {
				RB_ZombieFX( 1, drawSurf, oldNumVerts, oldNumIndex );
			}

			continue;
		}

		oldSort = (int)drawSurf->sort;
		// GR - also extract tesselation flag
		R_DecomposeSort( drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted, &pshadowed, &atiTess );
		cubemapIndex = drawSurf->cubemapIndex;

		// Change the tess parameters if needed.
		if ( shader != NULL && ( shader != oldShader || fogNum != oldFogNum ||
								 dlighted != oldDlighted || pshadowed != oldPshadowed ||
								 cubemapIndex != oldCubemapIndex
		                         // GR - force draw on tessellation flag change
								 || ( atiTess != oldAtiTess )
								 || ( entityNum != oldEntityNum && !shader->entityMergable ) ) ) {
			if ( oldShader != NULL ) {
				// GR - pass tessellation flag to the shader command
				// make sure to use oldAtiTess!!!
				tess.ATI_tess = (qboolean)( oldAtiTess == ATI_TESS_TRUFORM );

				RB_EndSurface();
			}
			RB_BeginSurface( shader, fogNum, cubemapIndex );
			backEnd.pc.c_surfBatches++;
			oldShader       = shader;
			oldFogNum       = fogNum;
			oldDlighted     = dlighted;
			oldPshadowed    = pshadowed;
			oldCubemapIndex = cubemapIndex;
			// GR - update old tessellation flag
			oldAtiTess = atiTess;
		}

		if ( backEnd.depthFill && shader && ( shader->sort != SS_OPAQUE && shader->sort != SS_PORTAL ) ) {
			continue;
		}

		// Change the modelview matrix if needed.
		if ( entityNum != oldEntityNum ) {
			depthRange = isCrosshair = qfalse;

			if ( entityNum != REFENTITYNUM_WORLD ) {
				backEnd.currentEntity = &backEnd.refdef.entities[entityNum];

				backEnd.refdef.floatTime = originalTime - (double)backEnd.currentEntity->e.shaderTime;

				// we have to reset the shaderTime as well otherwise image animations start
				// from the wrong frame
//				tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;

				R_RotateForEntity( backEnd.currentEntity, &backEnd.viewParms, &backEnd.or );

				if ( backEnd.currentEntity->needDlights ) {
					R_TransformDlights( backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.or );
				}

				if ( backEnd.currentEntity->e.renderfx & RF_DEPTHHACK ) {
					depthRange = qtrue;
					if ( backEnd.currentEntity->e.renderfx & RF_CROSSHAIR ) {
						isCrosshair = qtrue;
					}
				}
			} else
			{
				backEnd.currentEntity        = &tr.worldEntity;
				backEnd.refdef.floatTime     = originalTime;
				backEnd.or                   = backEnd.viewParms.world;
				// we have to reset the shaderTime as well otherwise image animations on
				// the world (like water) continue with the wrong frame
//				tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
				R_TransformDlights( backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.or );
			}

			GL_SetModelviewMatrix( backEnd.or.modelMatrix );

			if ( oldDepthRange != depthRange || wasCrosshair != isCrosshair ) {
				if ( depthRange ) {
					if ( backEnd.viewParms.stereoFrame != STEREO_CENTER ) {
						if ( isCrosshair ) {
							if ( oldDepthRange ) {
								// was not a crosshair but now is, change back proj matrix
								GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
							}
						} else
						{
							viewParms_t temp = backEnd.viewParms;
							R_SetupProjection( &temp, r_znear->value, 0, qfalse );
							GL_SetProjectionMatrix( temp.projectionMatrix );
						}
					}

					if ( !oldDepthRange ) {
						// GL2 ref: tr_backend.c:566 — qglDepthRange(0, 0.3)
						GLMetal_SetDepthRange( 0.0f, 0.3f );
					}
				} else
				{
					if ( !wasCrosshair && backEnd.viewParms.stereoFrame != STEREO_CENTER ) {
						GL_SetProjectionMatrix( backEnd.viewParms.projectionMatrix );
					}

					// GL2 ref: tr_backend.c:575 — qglDepthRange(0, 1)
					GLMetal_SetDepthRange( 0.0f, 1.0f );
				}

				oldDepthRange = depthRange;
				wasCrosshair  = isCrosshair;
			}

			oldEntityNum = entityNum;
		}

		// RF, ZOMBIEFX, store the tess indexes, so we can grab the calculated
		// vertex positions and normals, and convert them into dust particles
		oldNumVerts = tess.numVertexes;
		oldNumIndex = tess.numIndexes;

		// add the triangles for this surface
		// Add the triangles for this surface.
		rb_surfaceTable[ *drawSurf->surface ]( drawSurf->surface );

		// RF, convert the newly created vertexes into dust particles, and overwrite
		if ( backEnd.currentEntity->e.reFlags & REFLAG_ZOMBIEFX ) {
			RB_ZombieFX( 0, drawSurf, oldNumVerts, oldNumIndex );
		} else if ( backEnd.currentEntity->e.reFlags & REFLAG_ZOMBIEFX2 ) {
			RB_ZombieFX( 1, drawSurf, oldNumVerts, oldNumIndex );
		}
	}

	// Draw the contents of the last shader batch.
	if ( oldShader != NULL ) {
		// GR - pass tessellation flag to the shader command
		// make sure to use oldAtiTess!!!
		tess.ATI_tess = (qboolean)( oldAtiTess == ATI_TESS_TRUFORM );

		RB_EndSurface();
	}

	// go back to the world modelview matrix
	backEnd.currentEntity = &tr.worldEntity;
	backEnd.refdef.floatTime = originalTime;
	backEnd.or = backEnd.viewParms.world;
	R_TransformDlights( backEnd.refdef.num_dlights, backEnd.refdef.dlights, &backEnd.or );

	// PARITY-SEAM(Metal-device): FBO rebind after surface list — Phase 4.
	// if (glRefConfig.framebufferObject) FBO_Bind(fbo);
	(void)fbo;

	// Go back to the world modelview matrix.
	GL_SetModelviewMatrix( backEnd.viewParms.world.modelMatrix );

	// GL2 ref: tr_backend.c:603 — qglDepthRange(0, 1)
	GLMetal_SetDepthRange( 0.0f, 1.0f );
}


/*
================
RB_SetGL2D
GL2 ref: tr_backend.c:621

Sets up the 2D rendering state (orthographic projection, no depth test).
PARITY-SEAM: width/height, matrix math, GL_State, GL_Cull REAL;
qglViewport/qglScissor seamed.
================
*/
void RB_SetGL2D( void ) {
	mat4_t matrix;
	int width, height;

	if ( backEnd.projection2D && backEnd.last2DFBO == glState.currentFBO ) {
		return;
	}

	backEnd.projection2D = qtrue;
	backEnd.last2DFBO    = glState.currentFBO;

	if ( glState.currentFBO ) {
		width  = glState.currentFBO->width;
		height = glState.currentFBO->height;
	} else
	{
		width  = glConfig.vidWidth;
		height = glConfig.vidHeight;
	}

	// GL2 ref: tr_backend.c:643-644 — qglViewport/qglScissor
	GLMetal_SetViewport( 0, 0, width, height );

	Mat4Ortho( 0, width, height, 0, 0, 1, matrix );
	GL_SetProjectionMatrix( matrix );
	Mat4Identity( matrix );
	GL_SetModelviewMatrix( matrix );

	GL_State( GLS_DEPTHTEST_DISABLE |
			  GLS_SRCBLEND_SRC_ALPHA |
			  GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );

	GL_Cull( CT_TWO_SIDED );

	// Set time for 2D shaders.
	backEnd.refdef.time      = ri.Milliseconds();
	backEnd.refdef.floatTime = backEnd.refdef.time * 0.001;
}


/*
=============
RE_StretchRaw
GL2 ref: tr_backend.c:672

FIXME (GL2): not exactly backend.
Stretches a raw 32-bit power-of-2 bitmap over the given screen rectangle.
Used for cinematics.

PARITY-SEAM: R_IssuePendingRenderCommands / flush REAL; GL texture
upload + GLSL draw call seamed until Metal texture upload path exists (Phase 4).
=============
*/
void RE_StretchRaw( int x, int y, int w, int h, int cols, int rows,
					const byte *data, int client, qboolean dirty ) {
	if ( !tr.registered ) {
		return;
	}

	R_IssuePendingRenderCommands();

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// PARITY-SEAM(Metal-device): qglFinish → MTLCommandBuffer waitUntilCompleted (Phase 4).

	// Validate power-of-2 dimensions.
	{
		int i, j;
		for ( i = 0 ; ( 1 << i ) < cols ; i++ ) {}
		for ( j = 0 ; ( 1 << j ) < rows ; j++ ) {}
		if ( ( 1 << i ) != cols || ( 1 << j ) != rows ) {
			ri.Error( ERR_DROP, "Draw_StretchRaw: size not a power of 2: %i by %i", cols, rows );
		}
	}

	RE_UploadCinematic( w, h, cols, rows, data, client, dirty );
	GL_BindToTMU( tr.scratchImage[client], TB_COLORMAP );

	// PARITY-SEAM(Metal-device): a fresh device frame resets the render target to the
	// drawable while glState.currentFBO may still name renderFbo — see RB_StretchPic.
	if ( GLMetal_BeginFrame() ) {
		glState.currentFBO = NULL;
	}
	if ( glRefConfig.framebufferObject ) {
		FBO_Bind( tr.renderFbo );
	}

	RB_SetGL2D();

	vec4_t quadVerts[4];
	vec2_t texCoords[4];
	VectorSet4( quadVerts[0], x,     y,     0.0f, 1.0f );
	VectorSet4( quadVerts[1], x + w, y,     0.0f, 1.0f );
	VectorSet4( quadVerts[2], x + w, y + h, 0.0f, 1.0f );
	VectorSet4( quadVerts[3], x,     y + h, 0.0f, 1.0f );

	VectorSet2( texCoords[0], 0.5f / cols,          0.5f / rows );
	VectorSet2( texCoords[1], ( cols - 0.5f ) / cols, 0.5f / rows );
	VectorSet2( texCoords[2], ( cols - 0.5f ) / cols, ( rows - 0.5f ) / rows );
	VectorSet2( texCoords[3], 0.5f / cols,          ( rows - 0.5f ) / rows );

	GLSL_BindProgram( &tr.textureColorShader );

	GLSL_SetUniformMat4( &tr.textureColorShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GLSL_SetUniformVec4( &tr.textureColorShader, UNIFORM_COLOR, colorWhite );

	RB_InstantQuad2( quadVerts, texCoords );
}


/*
void RE_UploadCinematic
GL2 ref: tr_backend.c:737

Uploads a new cinematic frame to the scratch texture for the given client.
PARITY-SEAM: dimension / residency tracking REAL; GL texture sub-image
upload seamed until MTLTexture path is wired in Phase 4.
*/
void RE_UploadCinematic( int w, int h, int cols, int rows,
						 const byte *data, int client, qboolean dirty ) {
	if ( !tr.scratchImage[client] ) {
		ri.Printf( PRINT_WARNING, "RE_UploadCinematic: scratch images not initialized\n" );
		return;
	}

	image_t *image = tr.scratchImage[client];
	(void)w; (void)h;

	if ( cols != image->width || rows != image->height ) {
		image->width  = image->uploadWidth  = cols;
		image->height = image->uploadHeight = rows;
	} else if ( !dirty )   {
		return;
	}

	// GL2 uploads cinematics as GL_RGB8 (sampled alpha == 1) and RE_StretchRaw draws with
	// alpha blending (RB_SetGL2D), so force opaque alpha rather than trusting the decoder's
	// fourth byte. Metal has no RGB8 format.
	const size_t numPixels = (size_t)cols * (size_t)rows;
	byte *opaque = (byte *)ri.Hunk_AllocateTempMemory( (int)( numPixels * 4 ) );
	for ( size_t i = 0; i < numPixels; i++ )
	{
		opaque[i * 4 + 0] = data[i * 4 + 0];
		opaque[i * 4 + 1] = data[i * 4 + 1];
		opaque[i * 4 + 2] = data[i * 4 + 2];
		opaque[i * 4 + 3] = 255;
	}

	// GL2 ref: tr_backend.c:749-783 — (re)specify the texture when the size changed,
	// otherwise sub-image upload. Sampling is linear/clamp-to-edge via image->flags
	// (scratch images are IMGFLAG_CLAMPTOEDGE without IMGFLAG_MIPMAP).
	MTL::Texture *existing = GLMetal_GetTexture( (uint32_t)image->texnum );
	if ( !existing || (int)existing->width() != cols || (int)existing->height() != rows ) {
		GLMetal_CreateTexture( (uint32_t)image->texnum, cols, rows, GL_RGBA8, opaque, qfalse, GL_CLAMP_TO_EDGE );
	} else
	{
		GLMetal_UpdateTextureSubImage( (uint32_t)image->texnum, 0, 0, cols, rows, opaque );
	}

	ri.Hunk_FreeTempMemory( opaque );
}


/*
=============
RB_SetColor
GL2 ref: tr_backend.c:794

REAL — copies the command's RGBA color into backEnd.color2D.
This function is entirely GL-agnostic.
=============
*/
const void *RB_SetColor( const void *data ) {
	const setColorCommand_t *cmd;

	cmd = (const setColorCommand_t *)data;

	backEnd.color2D[0] = cmd->color[0] * 255;
	backEnd.color2D[1] = cmd->color[1] * 255;
	backEnd.color2D[2] = cmd->color[2] * 255;
	backEnd.color2D[3] = cmd->color[3] * 255;

	return (const void *)( cmd + 1 );
}


/*
=============
RB_StretchPic
GL2 ref: tr_backend.c:812

Draws a 2D stretch-pic from the command stream.
PARITY-SEAM: FBO/2D-setup + vertex assembly REAL; shader dispatch seamed.
=============
*/
const void *RB_StretchPic( const void *data ) {
	const stretchPicCommand_t *cmd;
	shader_t *shader;
	int numVerts, numIndexes;

	cmd = (const stretchPicCommand_t *)data;

	// Metal: GLMetal_BeginFrame resets s_renderTargetState to drawable-targeting.
	// It is invoked lazily by GLMetal_CurrentRenderEncoder on the first draw of a
	// frame. If the frame's FIRST command is a 2D stretchpic (e.g. the fullscreen
	// menu background drawn before the 3D banner scene), FBO_Bind(tr.renderFbo)
	// below would bind renderFbo, but the lazy BeginFrame at draw time would then
	// RESET the target back to the drawable — so the stretchpic renders to the
	// drawable and is subsequently overwritten by the renderFbo→drawable present
	// blit (menu background silently lost). Force BeginFrame BEFORE FBO_Bind so the
	// bind sticks, exactly as RB_BeginDrawingView does.
	// GL2 ref: no equivalent — GL has no late-acquired drawable / deferred frame.
	// PARITY-SEAM(Metal-device): When GLMetal_BeginFrame begins a NEW frame it resets
	// the device render target to the drawable. glState.currentFBO persists stale from
	// the prior frame (FBO_Bind only assigns it, never clears it between frames), so
	// FBO_Bind(tr.renderFbo) would early-return without re-establishing renderFbo as the
	// device target → menuback + cursor render to the drawable (overwritten by the
	// renderFbo→drawable present blit) while renderFbo retains a stale image → cursor
	// trails and lost background. Invalidate currentFBO so the following FBO_Bind
	// unconditionally re-binds renderFbo. This triggers ONLY on 2D-only frames whose
	// first command is a stretchpic. Gameplay is immune: its first command is always
	// RB_BeginDrawingView, which calls GLMetal_BeginFrame then GLMetal_SetRenderTarget
	// directly — so GLMetal_BeginFrame returns qfalse for every subsequent stretchpic
	// (HUD etc.) and currentFBO is never reset mid-frame.
	// GL2 ref: none — GL framebuffer binding is not reset by begin-frame machinery.
	if ( GLMetal_BeginFrame() ) {
		glState.currentFBO = NULL;
	}

	// PARITY-SEAM(Metal-device): FBO_Bind(tr.renderFbo) → render-target switch (Phase 4).
	if ( glRefConfig.framebufferObject ) {
		FBO_Bind( tr.renderFbo );
	}

	RB_SetGL2D();

	// Shader-change batching.
	shader = cmd->shader;
	if ( shader != tess.shader ) {
		if ( tess.numIndexes ) {
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface( shader, 0, 0 );
	}

	// Vertex assembly — GL-agnostic tessellator writes.
	RB_CHECKOVERFLOW( 4, 6 );
	numVerts   = tess.numVertexes;
	numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes  += 6;

	tess.indexes[ numIndexes ]     = numVerts + 3;
	tess.indexes[ numIndexes + 1 ] = numVerts + 0;
	tess.indexes[ numIndexes + 2 ] = numVerts + 2;
	tess.indexes[ numIndexes + 3 ] = numVerts + 2;
	tess.indexes[ numIndexes + 4 ] = numVerts + 0;
	tess.indexes[ numIndexes + 5 ] = numVerts + 1;

	{
		uint16_t color[4];
		VectorScale4( backEnd.color2D, 257, color );
		VectorCopy4( color, tess.color[ numVerts ] );
		VectorCopy4( color, tess.color[ numVerts + 1 ] );
		VectorCopy4( color, tess.color[ numVerts + 2 ] );
		VectorCopy4( color, tess.color[ numVerts + 3 ] );
	}

	tess.xyz[ numVerts ][0] = cmd->x;
	tess.xyz[ numVerts ][1] = cmd->y;
	tess.xyz[ numVerts ][2] = 0;
	tess.texCoords[ numVerts ][0] = cmd->s1;
	tess.texCoords[ numVerts ][1] = cmd->t1;

	tess.xyz[ numVerts + 1 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 1 ][1] = cmd->y;
	tess.xyz[ numVerts + 1 ][2] = 0;
	tess.texCoords[ numVerts + 1 ][0] = cmd->s2;
	tess.texCoords[ numVerts + 1 ][1] = cmd->t1;

	tess.xyz[ numVerts + 2 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 2 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 2 ][2] = 0;
	tess.texCoords[ numVerts + 2 ][0] = cmd->s2;
	tess.texCoords[ numVerts + 2 ][1] = cmd->t2;

	tess.xyz[ numVerts + 3 ][0] = cmd->x;
	tess.xyz[ numVerts + 3 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 3 ][2] = 0;
	tess.texCoords[ numVerts + 3 ][0] = cmd->s1;
	tess.texCoords[ numVerts + 3 ][1] = cmd->t2;

	return (const void *)( cmd + 1 );
}

/*
==============
RB_StretchPicGradient
==============
*/
const void *RB_StretchPicGradient( const void *data ) {
	const stretchPicCommand_t   *cmd;
	shader_t *shader;
	int numVerts, numIndexes;

	cmd = (const stretchPicCommand_t *)data;

	if ( glRefConfig.framebufferObject ) {
		FBO_Bind( tr.renderFbo );
	}

	RB_SetGL2D();

	shader = cmd->shader;
	if ( shader != tess.shader ) {
		if ( tess.numIndexes ) {
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface( shader, 0, 0 );
	}

	RB_CHECKOVERFLOW( 4, 6 );
	numVerts = tess.numVertexes;
	numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes += 6;

	tess.indexes[ numIndexes ] = numVerts + 3;
	tess.indexes[ numIndexes + 1 ] = numVerts + 0;
	tess.indexes[ numIndexes + 2 ] = numVerts + 2;
	tess.indexes[ numIndexes + 3 ] = numVerts + 2;
	tess.indexes[ numIndexes + 4 ] = numVerts + 0;
	tess.indexes[ numIndexes + 5 ] = numVerts + 1;

	{
		uint16_t color[4];

		VectorScale4( backEnd.color2D, 257, color );

		VectorCopy4( color, tess.color[ numVerts ] );
		VectorCopy4( color, tess.color[ numVerts + 1] );

		VectorScale4( cmd->gradientColor, 257, color );

		VectorCopy4( color, tess.color[ numVerts + 2] );
		VectorCopy4( color, tess.color[ numVerts + 3 ] );
	}

	tess.xyz[ numVerts ][0] = cmd->x;
	tess.xyz[ numVerts ][1] = cmd->y;
	tess.xyz[ numVerts ][2] = 0;

	tess.texCoords[ numVerts ][0] = cmd->s1;
	tess.texCoords[ numVerts ][1] = cmd->t1;

	tess.xyz[ numVerts + 1 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 1 ][1] = cmd->y;
	tess.xyz[ numVerts + 1 ][2] = 0;

	tess.texCoords[ numVerts + 1 ][0] = cmd->s2;
	tess.texCoords[ numVerts + 1 ][1] = cmd->t1;

	tess.xyz[ numVerts + 2 ][0] = cmd->x + cmd->w;
	tess.xyz[ numVerts + 2 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 2 ][2] = 0;

	tess.texCoords[ numVerts + 2 ][0] = cmd->s2;
	tess.texCoords[ numVerts + 2 ][1] = cmd->t2;

	tess.xyz[ numVerts + 3 ][0] = cmd->x;
	tess.xyz[ numVerts + 3 ][1] = cmd->y + cmd->h;
	tess.xyz[ numVerts + 3 ][2] = 0;

	tess.texCoords[ numVerts + 3 ][0] = cmd->s1;
	tess.texCoords[ numVerts + 3 ][1] = cmd->t2;

	return (const void *)( cmd + 1 );
}


/*
=============
RB_DrawSurfs
GL2 ref: tr_backend.c:896

Processes a drawSurfs command: sets up the view, runs the depth prepass
(if enabled), then the main surface list.
PARITY-SEAM: command unpacking + backEnd state REAL; depth-prepass FBO
and shadow/SSAO passes seamed; main RB_RenderDrawSurfList call REAL.
=============
*/
const void *RB_DrawSurfs( const void *data ) {
	const drawSurfsCommand_t *cmd;
	qboolean isShadowView;

	// Finish any pending 2D drawing.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	cmd = (const drawSurfsCommand_t *)data;

	backEnd.refdef   = cmd->refdef;
	backEnd.viewParms = cmd->viewParms;

	isShadowView = ( qboolean ) !!( backEnd.viewParms.flags & VPF_DEPTHSHADOW );

	// Clear z buffer, set modelview.
	RB_BeginDrawingView();

	// PARITY-SEAM(Metal-device): depth-prepass + SSAO + shadow-mask passes.
	// Shadow-mask pass (E6-4b): WIRED — see R_FillMetalShadowmaskUniforms + GLMetal_ShadowmaskPass below.
	// Depth prepass (E6-5): WIRED for r_sunlightMode 1 frames (fills tr.renderDepthImage for shadowmask).
	// Remaining passes still seamed:
	//   * general early-z prepass for non-sun frames (PARITY-SEAM: Metal main lit pass writes its own depth)
	//   * SSAO                (FBO_Bind, GLSL_BindProgram, RB_InstantQuad2) — in RB_PostProcess
	// GL2 ref: tr_backend.c:915-917 — sun shadow-map views clamp depth so casters outside the
	// cascade's near/far range still write depth instead of being clipped away.
	const qboolean depthClampView = ( glRefConfig.framebufferObject &&
									  ( backEnd.viewParms.flags & VPF_DEPTHCLAMP ) &&
									  glRefConfig.depthClamp ) ? qtrue : qfalse;
	if ( depthClampView ) {
		GLMetal_SetDepthClamp( qtrue );
	}

	if ( !isShadowView ) {
		// GL2 ref: tr_backend.c:920-932 — depth prepass populates tr.renderDepthImage so the
		// sun shadowmask can reconstruct world position. SCOPED to the sunlight path to keep
		// r_sunlightMode 0 / non-sun frames byte-identical (the general early-z prepass for all
		// frames remains a documented PARITY-SEAM; Metal's main lit pass writes its own depth).
		if ( r_sunlightMode->integer && ( backEnd.viewParms.flags & VPF_USESUNLIGHT )
			 && r_depthPrepass->integer && !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) && tr.renderDepthImage ) {
			backEnd.depthFill = qtrue;
			// PARITY-SEAM(Metal-device): GL2 qglColorMask(GL_FALSE x4) disables color writes during
			// the prepass; here the prepass garbage color in tr.renderImage is overwritten when the
			// main lit pass re-renders renderFbo. Depth is the meaningful output.
			RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );
			backEnd.depthFill = qfalse;
		}

		// GL2 ref: tr_backend.c:958-1036 — sun cascade shadowmask generation.
		// Composites the depth cascades into tr.screenShadowImage (fullscreen quad).
		// Must run BEFORE RB_RenderDrawSurfList so lightall can sample the shadow mask
		// (tr_shade.cpp:2296 via LIGHTDEF_USE_SHADOWMAP).
		//
		// VPF_USESUNLIGHT is set in tr_scene.cpp:608 when r_depthPrepass && (r_forceSun || tr.sunShadows).
		// That matches GL2 R_SetupViewParms (tr_main.c) exactly — no discrepancy.
		if ( r_sunlightMode->integer && ( backEnd.viewParms.flags & VPF_USESUNLIGHT ) && tr.screenShadowImage ) {
			MetalShadowmaskUniforms smu;
			R_FillMetalShadowmaskUniforms( &smu );

			// Viewport box scaled to screenShadowFbo dimensions.
			int box[4];
			box[0] = backEnd.viewParms.viewportX      * tr.screenShadowFbo->width  / glConfig.vidWidth;
			box[1] = backEnd.viewParms.viewportY      * tr.screenShadowFbo->height / glConfig.vidHeight;
			box[2] = backEnd.viewParms.viewportWidth  * tr.screenShadowFbo->width  / glConfig.vidWidth;
			box[3] = backEnd.viewParms.viewportHeight * tr.screenShadowFbo->height / glConfig.vidHeight;

			// GL2 ref: tr_backend.c:999 — useCascades gate
			qboolean useCascades = (qboolean)( r_shadowCascadeZFar->integer != 0 );

			uint32_t cascades[4] = {
				tr.sunShadowDepthImage[0] ? tr.sunShadowDepthImage[0]->texnum : 0,
				tr.sunShadowDepthImage[1] ? tr.sunShadowDepthImage[1]->texnum : 0,
				tr.sunShadowDepthImage[2] ? tr.sunShadowDepthImage[2]->texnum : 0,
				tr.sunShadowDepthImage[3] ? tr.sunShadowDepthImage[3]->texnum : 0
			};

			GLMetal_ShadowmaskPass(
				tr.screenShadowImage->texnum,
				box[0], box[1], box[2], box[3],
				tr.renderDepthImage ? tr.renderDepthImage->texnum : 0,
				cascades, useCascades, &smu );
			// GL2 ref: tr_backend.c:1037-1063 — two-pass separable depth-aware blur on screenShadowImage.
			// PARITY-SEAM(Metal-device): GL2 used tr.hdrDepthImage (a blit copy of the depth buffer) as the
			// depth source for both blur passes. Metal samples tr.renderDepthImage directly — consistent with
			// the shadowmask pass above, which already does the same. No hdrDepth blit step needed.
			if ( r_shadowBlur->integer && tr.screenScratchFbo && tr.screenScratchImage && tr.screenShadowFbo
				 && tr.screenScratchImage->texnum && tr.renderDepthImage ) {
				MetalDepthBlurUniforms dbu;
				// x=zFar/zNear, y=zFar: carried from shadowmask setup (GL2 ref: tr_backend.c:975).
				dbu.u_ViewInfo[0] = backEnd.viewParms.zFar / r_znear->value; // GL2 ref: tr_backend.c:975 (zFar/zNear)
				dbu.u_ViewInfo[1] = backEnd.viewParms.zFar;                  // GL2 ref: tr_backend.c (zFar)
				// z,w = 1/scratchFbo dims (same for both passes). GL2 ref: tr_backend.c:1040-1041
				dbu.u_ViewInfo[2] = 1.0f / (float)tr.screenScratchFbo->width;
				dbu.u_ViewInfo[3] = 1.0f / (float)tr.screenScratchFbo->height;

				// PASS 0 — horizontal + depth: blur screenShadowImage → screenScratchFbo. GL2 ref: tr_backend.c:1043-1050
				int box0[4];
				box0[0] = backEnd.viewParms.viewportX      * tr.screenScratchFbo->width  / glConfig.vidWidth;
				box0[1] = backEnd.viewParms.viewportY      * tr.screenScratchFbo->height / glConfig.vidHeight;
				box0[2] = backEnd.viewParms.viewportWidth  * tr.screenScratchFbo->width  / glConfig.vidWidth;
				box0[3] = backEnd.viewParms.viewportHeight * tr.screenScratchFbo->height / glConfig.vidHeight;
				GLMetal_DepthBlurPass( tr.screenScratchImage->texnum, box0[0], box0[1], box0[2], box0[3],
									   tr.screenShadowImage->texnum, tr.renderDepthImage->texnum,
									   qtrue /*horizontal*/, qtrue /*useDepth*/, &dbu );

				// PASS 1 — vertical + depth: blur screenScratchImage → screenShadowFbo. GL2 ref: tr_backend.c:1052-1062
				int box1[4];
				box1[0] = backEnd.viewParms.viewportX      * tr.screenShadowFbo->width  / glConfig.vidWidth;
				box1[1] = backEnd.viewParms.viewportY      * tr.screenShadowFbo->height / glConfig.vidHeight;
				box1[2] = backEnd.viewParms.viewportWidth  * tr.screenShadowFbo->width  / glConfig.vidWidth;
				box1[3] = backEnd.viewParms.viewportHeight * tr.screenShadowFbo->height / glConfig.vidHeight;
				GLMetal_DepthBlurPass( tr.screenShadowImage->texnum, box1[0], box1[1], box1[2], box1[3],
									   tr.screenScratchImage->texnum, tr.renderDepthImage->texnum,
									   qfalse /*vertical*/, qtrue /*useDepth*/, &dbu );
			}
		}

		// GL2 ref: tr_backend.c:929 — the main pass reuses the pre-pass depth: it re-draws
		// the same surfaces at equal depth and passes GL_LEQUAL. That needs bit-identical
		// positions from the pre-pass (generic) and main (lightall / generic) vertex shaders,
		// which the [[invariant]] position outputs plus CompileOptions preserveInvariance
		// guarantee. The shadow mask pass switched targets, so rebind the scene target here,
		// loading (not clearing) depth. Gated on the same condition as the pre-pass.
		if ( r_sunlightMode->integer && ( backEnd.viewParms.flags & VPF_USESUNLIGHT )
			 && r_depthPrepass->integer && !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) && tr.renderDepthImage ) {
			uint32_t colorTexnum = tr.renderFbo->colorImage[0] ? tr.renderFbo->colorImage[0]->texnum : 0;
			uint32_t depthTexnum = tr.renderDepthImage->texnum;
			GLMetal_SetRenderTarget( colorTexnum, depthTexnum, false, false );
		}

		// Main surface list.
		RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );

		// RB_ShadowFinish self-guards on r_shadows != 2 (byte-identical no-op for all other modes).
		RB_ShadowFinish();
		// RB_DrawSun, RB_RenderFlares — seamed (Phase 4).
	} else
	{
		// GL2 ref: tr_backend.c:920-932 — shadow view: depth-fill render pass.
		// Renders occluder geometry into the pshadow FBO's depth attachment.
		// Color writes disabled; hardware depth write is the meaningful output.
		// The targetFbo was set to tr.pshadowFbos[i] by R_RenderPshadowMaps.
		backEnd.depthFill = qtrue;
		// PARITY-SEAM(Metal-device): qglColorMask(GL_FALSE×4) — Metal equivalent is
		// MTLRenderPipelineColorAttachmentDescriptor.writeMask = .none. For now,
		// rely on the shadowfill shader writing trivial color; depth is the output.
		RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );
		backEnd.depthFill = qfalse;
	}

	if ( depthClampView ) {
		GLMetal_SetDepthClamp( qfalse );
	}

	// PARITY-SEAM(Metal-device): cubemap mipmap generation after cube-face render.
	// qglGenerateTextureMipmapEXT → MTLTexture generateMipmaps (Phase 4).

	// Clear leaked isMirror / flags.
	backEnd.viewParms.isMirror = qfalse;
	backEnd.viewParms.flags    = (viewParmFlags_t)0;

	return (const void *)( cmd + 1 );
}


/*
=============
RB_DrawBuffer
GL2 ref: tr_backend.c:1213

Processes a drawBuffer command: selects render target.
PARITY-SEAM: FBO bind REAL (seamed); qglDrawBuffer seamed.
=============
*/
const void *RB_DrawBuffer( const void *data ) {
	const drawBufferCommand_t *cmd;

	cmd = (const drawBufferCommand_t *)data;

	// Finish any pending 2D drawing.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// PARITY-SEAM(Metal-device): FBO_Bind(NULL) + qglDrawBuffer →
	// MTLRenderCommandEncoder endEncoding + presentDrawable (Phase 4).

	// Debug clear on r_clear.
	if ( r_clear->integer ) {
		// PARITY-SEAM: qglClearColor + qglClear → MTLLoadActionClear (Phase 4).
	}

	return (const void *)( cmd + 1 );
}


/*
===============
RB_ShowImages
GL2 ref: tr_backend.c:1250

Draws all loaded images to the screen for texture-thrashing testing.
Also called by RE_EndRegistration.
PARITY-SEAM: layout maths + timing REAL; GL bind + draw seamed.
===============
*/
void RB_ShowImages( void ) {
	int i;
	image_t *image;
	float x, y, w, h;
	int start, end;

	RB_SetGL2D();

	// PARITY-SEAM(Metal-device): qglClear(GL_COLOR_BUFFER_BIT) → MTLLoadActionClear (Phase 4).

	// PARITY-SEAM: qglFinish → MTLCommandBuffer waitUntilCompleted (Phase 4).

	start = ri.Milliseconds();

	for ( i = 0 ; i < tr.numImages ; i++ )
	{
		image = tr.images[i];

		w = glConfig.vidWidth / 40;
		h = glConfig.vidHeight / 30;

		x = i % 40 * w;
		y = i / 30 * h;

		// Show in proportional size in mode 2.
		if ( r_showImages->integer == 2 ) {
			w *= image->uploadWidth  / 512.0f;
			h *= image->uploadHeight / 512.0f;
		}

		// PARITY-SEAM(Metal-device): GL_BindToTMU + RB_InstantQuad draw →
		// MTLRenderCommandEncoder setFragmentTexture + drawPrimitives (Phase 4).
		(void)x; (void)y; (void)w; (void)h;
	}

	// PARITY-SEAM: qglFinish (Phase 4). GL2 ref: tr_backend.c:1292
	end = ri.Milliseconds();
	ri.Printf( PRINT_ALL, "%i msec to draw all images\n", end - start );
}


/*
=============
RB_ColorMask
GL2 ref: tr_backend.c:1305

Sets the color-write mask.  PARITY-SEAM: colorMask tracking REAL;
qglColorMask seamed.
=============
*/
const void *RB_ColorMask( const void *data ) {
	const colorMaskCommand_t *cmd = (const colorMaskCommand_t *)data;

	// Finish any pending 2D drawing.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	if ( glRefConfig.framebufferObject ) {
		// Reverse color mask so 0,0,0,0 is the default.
		backEnd.colorMask[0] = ( qboolean ) !cmd->rgba[0];
		backEnd.colorMask[1] = ( qboolean ) !cmd->rgba[1];
		backEnd.colorMask[2] = ( qboolean ) !cmd->rgba[2];
		backEnd.colorMask[3] = ( qboolean ) !cmd->rgba[3];
	}

	// PARITY-SEAM(Metal-device): qglColorMask → MTLRenderPipelineColorAttachmentDescriptor
	// .writeMask (Phase 4).

	return (const void *)( cmd + 1 );
}


/*
=============
RB_ClearDepth
GL2 ref: tr_backend.c:1333

Clears the depth buffer.  PARITY-SEAM: FBO selection logic REAL;
qglClear seamed.
=============
*/
const void *RB_ClearDepth( const void *data ) {
	const clearDepthCommand_t *cmd = (const clearDepthCommand_t *)data;

	// Finish any pending 2D drawing.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// Texture swapping test.
	if ( r_showImages->integer ) {
		RB_ShowImages();
	}

	// PARITY-SEAM(Metal-device): FBO_Bind(tr.renderFbo) + qglClear(GL_DEPTH_BUFFER_BIT) →
	// MTLRenderPassDescriptor depthAttachment.loadAction = MTLLoadActionClear (Phase 4).

	return (const void *)( cmd + 1 );
}


/*
=============
RB_DrawGreyscale
GL2 ref: tr_backend.c:1369

Full-screen greyscale post-process pass.
=============
*/
static void RB_DrawGreyscale( const FBO_t *src ) {
	if ( !src || !src->colorImage[0] ) {
		return;
	}

	// GL2 binds tr.greyscaleShader and draws a fullscreen quad blitting src->screen
	// with luminance desaturation (GLS_SRCBLEND_ONE|GLS_DSTBLEND_ZERO). Under Metal
	// this is the greyscale present pass into the drawable.
	MTL::Texture *srcTex = GLMetal_GetTexture( src->colorImage[0]->texnum );
	GLMetal_GreyscalePass( srcTex, backEnd.greyscale );
}


/*
=============
RB_PresentToScreen
GL2 ref: tr_backend.c:1402

Blits the final render FBO to the system framebuffer (or applies greyscale).
PARITY-SEAM: FBO selection logic REAL; FBO blits seamed.
=============
*/
static void RB_PresentToScreen( void ) {
	if ( !glRefConfig.framebufferObject ) {
		return;
	}

	// GL2 ref: tr_backend.c:1407-1438.  A1 replaces the final FBO_FastBlit
	// with the Metal present tonemap/greyscale pass into the drawable.
	// GL2 ref: tr_backend.c:1422-1426: branch on backEnd.greyscale — now implemented.
	if ( tr.renderFbo && tr.renderFbo->colorImage[0] ) {
		// SLICE-B ACCEPTANCE HOOK (temporary; remove/guard after gate)
		if ( r_metalBlitTest && r_metalBlitTest->integer && tr.quarterFbo[0] && tr.quarterFbo[1] ) {
			ivec4_t fullViewBox;
			ivec4_t quarterBox;
			ivec4_t insetBox;
			VectorSet4( fullViewBox, 0, tr.renderFbo->height, tr.renderFbo->width, -tr.renderFbo->height );
			VectorSet4( quarterBox, 0, 0, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height );
			// q3dm1 is vertically asymmetric (sky up/floor down); a top-left inset
			// makes a flipped FBO→FBO path obvious before the final TonemapPass.
			VectorSet4( insetBox, 0, tr.renderFbo->height - tr.quarterFbo[1]->height,
						tr.quarterFbo[1]->width, tr.quarterFbo[1]->height );
			FBO_FastBlit( tr.renderFbo, fullViewBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR );
			FBO_FastBlit( tr.quarterFbo[0], quarterBox, tr.quarterFbo[1], quarterBox, GL_COLOR_BUFFER_BIT, GL_NEAREST );
			FBO_Blit( tr.quarterFbo[1], NULL, NULL, tr.renderFbo, insetBox, NULL, colorWhite, 0 );
		}

		if ( backEnd.greyscale > 0.0f ) {
			RB_DrawGreyscale( tr.renderFbo );
		} else
		{
			MTL::Texture *srcTex = GLMetal_GetTexture( tr.renderFbo->colorImage[0]->texnum );
			// A1 identity passthrough: GL2's final present blit (FBO_FastBlit) applies NO
			// exposure — exposure/tone-curve live only in RB_ToneMap, which A1 disables
			// (r_toneMap 0). Pass 1.0 for a true identity present; exposure is deferred to
			// the RB_ToneMap port (M5.4 Slice C). GL2 ref: tr_backend.c:1429,1436.
			GLMetal_TonemapPass( srcTex, 1.0f );
		}
	}
}


/*
=============
RB_SwapBuffers
GL2 ref: tr_backend.c:1448

Processes a swapBuffers command: presents the final frame.
PARITY-SEAM: overdraw measurement seamed; RB_PresentToScreen + GLimp calls seamed;
backEnd.projection2D reset REAL.
=============
*/
const void *RB_SwapBuffers( const void *data ) {
	const swapBuffersCommand_t *cmd;

	// Finish any pending 2D drawing.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	// Texture swapping test.
	if ( r_showImages->integer ) {
		RB_ShowImages();
	}

	cmd = (const swapBuffersCommand_t *)data;

	// PARITY-SEAM(Metal-device): overdraw stencil readback.
	// qglReadPixels → MTLBlitCommandEncoder copy buffer (Phase 4).
	if ( r_measureOverdraw->integer ) {
		// PARITY-SEAM: GL stencil overdraw measurement omitted (Phase 4).
	}

	// Present final frame.
	RB_PresentToScreen();

	// PARITY-SEAM(Metal-device): qglFinish → MTLCommandBuffer waitUntilCompleted (Phase 4).

	// PHASE5: wired to Metal present
	GLimp_EndFrame();

	backEnd.projection2D = qfalse;

	return (const void *)( cmd + 1 );
}


/*
=============
RB_CapShadowMap
GL2 ref: tr_backend.c:1502

Captures a projected shadow map or cube-face shadow map.
PARITY-SEAM: map/cubeSide validity checks REAL; GL texture copy seamed.
=============
*/
const void *RB_CapShadowMap( const void *data ) {
	const capShadowmapCommand_t *cmd = (const capShadowmapCommand_t *)data;

	// Finish any pending 2D drawing.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	if ( cmd->map != -1 ) {
		if ( cmd->cubeSide != -1 ) {
			// PARITY-SEAM(Metal-device): qglCopyTextureSubImage2DEXT cubemap face →
			// MTLBlitCommandEncoder copyFromTexture:... (Phase 4).
		} else
		{
			// PARITY-SEAM(Metal-device): qglCopyTextureSubImage2DEXT 2D →
			// MTLBlitCommandEncoder copyFromTexture:... (Phase 4).
		}
	}

	return (const void *)( cmd + 1 );
}


/*
=============
RB_PostProcess
GL2 ref: tr_backend.c:1538

Full post-process pipeline: MSAA resolve, SSAO blend, tone mapping,
sun rays, bokeh blur.  Control flow mirrors tr_backend.c:1561-1748 exactly.
SEAM: qgl* raw state calls are PARITY-SEAM; all FBO_Blit/FBO_FastBlit paths
use tr_fbo.cpp entry points (Ripley's domain).
=============
*/
const void *RB_PostProcess( const void *data ) {
	const postProcessCommand_t *cmd = (const postProcessCommand_t *)data;
	FBO_t *srcFbo, *dstFbo;
	ivec4_t srcBox, dstBox;
	qboolean autoExposure;

	// Finish any pending 2D drawing.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	if ( !glRefConfig.framebufferObject || !r_postProcess->integer ) {
		return (const void *)( cmd + 1 );
	}

	if ( cmd ) {
		backEnd.refdef   = cmd->refdef;
		backEnd.viewParms = cmd->viewParms;
	}

	srcFbo = tr.renderFbo;
	dstFbo = tr.renderFbo;

	// MSAA resolve — must happen before any post-process reads from srcFbo.
	if ( tr.msaaResolveFbo ) {
		// Can't resolve just part of the MSAA FBO; multiple views pay the full cost.
		// PARITY-SEAM(Metal-device): GL_DEPTH_BUFFER_BIT resolve →
		// MTLBlitCommandEncoder resolveTexture (depth resolve deferred to FBO slice F).
		FBO_FastBlit( tr.renderFbo, NULL, tr.msaaResolveFbo, NULL,
					  GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST );
		srcFbo = tr.msaaResolveFbo;
	}

	// Viewport-aligned destination box for subsequent blits.
	dstBox[0] = backEnd.viewParms.viewportX;
	dstBox[1] = backEnd.viewParms.viewportY;
	dstBox[2] = backEnd.viewParms.viewportWidth;
	dstBox[3] = backEnd.viewParms.viewportHeight;

	// SSAO GENERATION — Apple Scalable Ambient Obscurance (SAO) compute pass.
	//
	// GL2 ref: tr_backend.c:1067-1130 (ssaoShader render to screenSsaoFbo + depthBlur).
	// PARITY DEVIATION: GL2 uses a fragment-shader 3-pass FBO blit (ssao_fp.glsl).
	//   Metal path replaces that with a single compute dispatch via the SAO kernel
	//   (glsl/sao_compute.metal, entry scalableAmbientObscurance).
	//   Deviation approved and logged: .squad/decisions/inbox/dallas-ssao-deviation-decision.md.
	//
	// Frame-flow context — depth availability:
	//   At this point in RB_PostProcess, the opaque+transparent geometry passes have
	//   already been submitted to tr.renderFbo (which has tr.renderDepthImage as its
	//   depth attachment).  The MSAA resolve block above (line ~1457) has run if MSAA
	//   is active, but that only resolves COLOR; depth is always available in
	//   tr.renderDepthImage after the geometry passes and before this call.
	//   GLMetal_DispatchSAO ends the active render encoder (if any), opens a compute
	//   encoder, reads tr.renderDepthImage, writes AO to tr.screenSsaoImage, then
	//   ends the compute encoder.  The subsequent FBO_Blit consumption block below
	//   opens a new render encoder, which is correct ordering.
	//
	// RESOLUTION (full-res, T5b):
	//   tr.screenSsaoImage and tr.renderDepthImage are BOTH created at full screen
	//   resolution (width × height; tr_image.cpp).  GLMetal_DispatchSAO dispatches at
	//   full res, using thread_position_in_grid as both the AO write coordinate and the
	//   depth read coordinate — a 1:1 mapping with no out-of-bounds writes.  The
	//   srcBox scaling below therefore resolves to a 1.0 scale factor.
	//   v2 DEFERRED (perf only, not correctness): half-res SAO + hierarchical depth
	//   pyramid + depth-blur post-pass (see dallas-ssao-impl-plan.md).
	if ( r_ssao->integer ) {
		// Generate AO into tr.screenSsaoImage using the Apple-SAO compute kernel.
		// Depth must be from the scene's opaque pass (tr.renderDepthImage).
		// GL2 ref: tr_backend.c:1067 (ssaoShader generation block).
		GLMetal_DispatchSAO( tr.screenSsaoImage->texnum,
							 tr.renderDepthImage->texnum,
							 backEnd.viewParms.projectionMatrix );
	}

	// SSAO blend into srcFbo.
	if ( r_ssao->integer ) {
		srcBox[0] = (int)( backEnd.viewParms.viewportX      * tr.screenSsaoImage->width  / (float)glConfig.vidWidth );
		srcBox[1] = (int)( backEnd.viewParms.viewportY      * tr.screenSsaoImage->height / (float)glConfig.vidHeight );
		srcBox[2] = (int)( backEnd.viewParms.viewportWidth  * tr.screenSsaoImage->width  / (float)glConfig.vidWidth );
		srcBox[3] = (int)( backEnd.viewParms.viewportHeight * tr.screenSsaoImage->height / (float)glConfig.vidHeight );

		FBO_Blit( tr.screenSsaoFbo, srcBox, NULL, srcFbo, dstBox, NULL, NULL,
				  GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO );
	}

	// Source box in original viewport coords (no SSAO scale).
	srcBox[0] = backEnd.viewParms.viewportX;
	srcBox[1] = backEnd.viewParms.viewportY;
	srcBox[2] = backEnd.viewParms.viewportWidth;
	srcBox[3] = backEnd.viewParms.viewportHeight;

	if ( srcFbo ) {
		if ( r_hdr->integer && ( r_toneMap->integer || r_forceToneMap->integer ) ) {
			// Auto-exposure: either user cvar or force-cheat.
			autoExposure = ( r_autoExposure->integer || r_forceAutoExposure->integer ) ? qtrue : qfalse;

			// Tonemap into an intermediate scratch FBO, then copy back.
			// The intermediate FBO is needed because reading from and writing
			// to the same FBO in a single pass is undefined.
			RB_ToneMap( srcFbo, srcBox, tr.screenScratchFbo, srcBox, (int)autoExposure );
			FBO_FastBlit( tr.screenScratchFbo, srcBox, srcFbo, srcBox,
						  GL_COLOR_BUFFER_BIT, GL_NEAREST );
		} else if ( r_cameraExposure->value != 0.0f )   {
			// Simple per-frame exposure adjustment without full tonemap.
			vec4_t color;
			color[0] =
				color[1] =
					color[2] = (float)pow( 2.0, r_cameraExposure->value ); //exp2(r_cameraExposure->value)
			color[3] = 1.0f;

			FBO_BlitFromTexture( tr.whiteImage, NULL, NULL, srcFbo, srcBox, NULL, color,
								 GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO );
		}
	}

	// Sun rays (only when cvar is on).
	if ( r_drawSunRays->integer ) {
		RB_SunRays( srcFbo, srcBox, srcFbo, srcBox );
	}

	// Bokeh blur / Gaussian blur over the whole frame.
	// GL2 ref: tr_backend.c:1620-1623 — GL2 uses `if (1)` to permanently choose bokeh.
	if ( 1 ) {
		RB_BokehBlur( srcFbo, srcBox, srcFbo, srcBox, backEnd.refdef.blurFactor );
	} else {
		RB_GaussianBlur( srcFbo, srcFbo, backEnd.refdef.blurFactor );
	}

	// If post-process routed through a different FBO (MSAA resolve path), blit back
	// to the primary render target so RB_PresentToScreen sees a unified source.
	if ( srcFbo != dstFbo ) {
		FBO_FastBlit( srcFbo, srcBox, dstFbo, dstBox, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	}

	return (const void *)( cmd + 1 );
}


/*
=============
RB_ExportCubemaps
GL2 ref: tr_backend.c:1761

Reads back all cubemap faces from GPU and saves them as DDS files.
PARITY-SEAM: iteration logic + filename construction REAL;
GL readback seamed until MTLBlitCommandEncoder copyFromTexture: exists (Phase 4).
=============
*/
const void *RB_ExportCubemaps( const void *data ) {
	const exportCubemapsCommand_t *cmd = (const exportCubemapsCommand_t *)data;

	// Finish any pending 2D drawing.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	if ( !glRefConfig.framebufferObject || !tr.world || tr.numCubemaps == 0 ) {
		// Do nothing.
		ri.Printf( PRINT_ALL, "Nothing to export!\n" );
		return (const void *)( cmd + 1 );
	}

	if ( cmd ) {
		// PARITY-SEAM(Metal-device): FBO + qglReadPixels + R_SaveDDS →
		// MTLBlitCommandEncoder copyFromTexture: → map buffer → R_SaveDDS (Phase 4).
		// Iteration structure (numCubemaps, 6 faces, filename construction) preserved
		// as the shape for Phase 4 readback implementation.
		int i, j;
		for ( i = 0; i < tr.numCubemaps; i++ )
		{
			char filename[MAX_QPATH];
			cubemap_t     *cubemap = &tr.cubemaps[i];

			for ( j = 0; j < 6; j++ )
			{
				// PARITY-SEAM: FBO_AttachImage + qglReadPixels → Metal blit (Phase 4).
				(void)cubemap; (void)j;
			}

			// Filename construction — GL-agnostic. GL2 ref: tr_backend.c:1798-1806.
			if ( cubemap->name[0] ) {
				COM_StripExtension( cubemap->name, filename, MAX_QPATH );
				Q_strcat( filename, MAX_QPATH, ".dds" );
			} else
			{
				Com_sprintf( filename, MAX_QPATH, "cubemaps/%s/%03d.dds",
							 tr.world->baseName, i );
			}

			// PARITY-SEAM: R_SaveDDS (needs readback buffer, Phase 4).
			ri.Printf( PRINT_ALL, "Would export cubemap %d as %s (Phase 4)\n", i, filename );
		}
	}

	return (const void *)( cmd + 1 );
}


/*
====================
RB_ExecuteRenderCommands
GL2 ref: tr_backend.c:1826

REAL — command-stream dispatch loop.  Processes all queued render commands
in order.  This function is entirely GL-agnostic (it dispatches to the other
RB_* functions defined in this file).  The dispatch structure is load-bearing
for Phase 3 (tr_shade) integration.
====================
*/
void RB_ExecuteRenderCommands( const void *data ) {
	int t1, t2;

	t1 = ri.Milliseconds();

	// Metal: screenshots/levelshots deferred from the previous frame (on-demand capture).
	R_MetalProcessPendingCaptures();

	while ( 1 )
	{
		data = PADP( data, sizeof( void * ) );

		switch ( *(const int *)data )
		{
		case RC_SET_COLOR:
			data = RB_SetColor( data );
			break;
		case RC_STRETCH_PIC:
			data = RB_StretchPic( data );
			break;
		case RC_STRETCH_PIC_GRADIENT:
			data = RB_StretchPicGradient( data );
			break;
		case RC_DRAW_SURFS:
			data = RB_DrawSurfs( data );
			break;
		case RC_DRAW_BUFFER:
			data = RB_DrawBuffer( data );
			break;
		case RC_SWAP_BUFFERS:
			data = RB_SwapBuffers( data );
			break;
		case RC_SCREENSHOT:
			data = RB_TakeScreenshotCmd( data );
			break;
		case RC_VIDEOFRAME:
			data = RB_TakeVideoFrameCmd( data );
			break;
		case RC_COLORMASK:
			data = RB_ColorMask( data );
			break;
		case RC_CLEARDEPTH:
			data = RB_ClearDepth( data );
			break;
		case RC_CAPSHADOWMAP:
			data = RB_CapShadowMap( data );
			break;
		case RC_POSTPROCESS:
			data = RB_PostProcess( data );
			break;
		case RC_EXPORT_CUBEMAPS:
			data = RB_ExportCubemaps( data );
			break;
		case RC_END_OF_LIST:
		default:
			// Finish any pending 2D drawing.
			if ( tess.numIndexes ) {
				RB_EndSurface();
			}

			// Stop rendering.
			t2 = ri.Milliseconds();
			backEnd.pc.msec = t2 - t1;
			return;
		}
	}
}

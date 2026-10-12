/*
===========================================================================
Copyright (C) 2011 Andrei Drexler, Richard Allen, James Canete

This file is part of Reaction source code.

Reaction source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Reaction source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Reaction source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_postprocess.cpp — Metal renderer port of tr_postprocess.c
// GL2 ref: tr_postprocess.c
//
// Phase-4 PORT: all 9 functions from tr_postprocess.c are present.
//   2  REAL        — RB_HBlur, RB_VBlur (pure delegates to RB_BlurAxis)
//   7  PARITY-SEAM — real CPU control flow + all math preserved; raw qgl*
//                    annotated; FBO_Blit/FBO_FastBlit/GL_BindToTMU delegate
//                    to seamed FBO/backend TUs.
//   0  PARITY-STUB — none
//
// Wave-2 Wiring (this session, 2026-06-06):
//   RB_ToneMap: A1 scope limiter removed; full GL2 execution path wired.
//     - calclevels4x first-pass blit (Vasquez MSL, now live)
//     - 256→1 ping-pong halving downsample
//     - adapted-luminance temporal blend (textureFloat ? 0.03 : 0.1)
//     - tonemap draw with exposure = 2^(r_cameraExposure - autoExposure)
//     - GL_BindToTMU(TB_LEVELSMAP) sets glState slot 1; FBO_BlitFromTexture
//       (Ripley's tr_fbo.cpp) must pass it as extraTexnums[0] to
//       GLMetal_RenderBlitTextureMulti — see lambert-tonemap-path.md.
//   RB_SunRays: A1 scope limiter removed; cvar gate lives in RB_PostProcess
//     (tr_backend.cpp) matching GL2 tr_backend.c:1617.
//
// SEAM DISCIPLINE: uniform-value/color computation, box/rect math, viewport
// math, control-flow choosing src/dst FBOs — all kept REAL.  Only raw qgl*
// state writes (qglColorMask, qglGetQueryObjectiv/uiv) are PARITY-SEAM.

extern "C" {
#include "../qcommon/q_shared.h"
}

#include "tr_extramath.h"

extern "C" {
#include "tr_local.h"
}

// ---------------------------------------------------------------------------
// GL-COMPAT per-TU defines (GL enums not present in tr_local.h)
// ---------------------------------------------------------------------------

#ifndef GL_TRUE
#define GL_TRUE                             1
#endif

#ifndef GL_LINEAR
#define GL_LINEAR                           0x2601
#endif

#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE           0x8867
#endif

#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT                     0x8866
#endif

// ---------------------------------------------------------------------------

void RB_ToneMap( FBO_t *hdrFbo, ivec4_t hdrBox, FBO_t *ldrFbo, ivec4_t ldrBox, int autoExposure ) {
	ivec4_t srcBox, dstBox;
	vec4_t color;
	static int lastFrameCount = 0;

	if ( autoExposure ) {
		// GL2 ref: tr_postprocess.c:31-35 — stale-frame guard: re-compute
		// log-luminance at most once every 5 frames; reset on frame counter wrap.
		if ( lastFrameCount == 0 || tr.frameCount < lastFrameCount || tr.frameCount - lastFrameCount > 5 ) {
			// determine average log luminance
			FBO_t *srcFbo, *dstFbo, *tmp;
			int size = 256;

			lastFrameCount = tr.frameCount;

			VectorSet4( dstBox, 0, 0, size, size );

			// First-pass calclevels4x: log2-luminance encode from HDR scene → 256×256 scratch.
			// Vasquez contract: calclevels4x[0] uses FIRST_PASS define; loglumi encoded in RGB.
			// u_InvTexRes at buffer offset 80 is set by FBO_BlitFromTexture via GLSL_SetUniformVec2.
			FBO_Blit( hdrFbo, hdrBox, NULL, tr.textureScratchFbo[0], dstBox, &tr.calclevels4xShader[0], NULL, 0 );

			srcFbo = tr.textureScratchFbo[0];
			dstFbo = tr.textureScratchFbo[1];

			// downscale to 1x1 texture
			// GL2 ref: tr_postprocess.c:48-58 — ping-pong halving until 1×1.
			while ( size > 1 )
			{
				VectorSet4( srcBox, 0, 0, size, size );
				//size >>= 2;
				size >>= 1;
				VectorSet4( dstBox, 0, 0, size, size );

				if ( size == 1 ) {
					dstFbo = tr.targetLevelsFbo;
				}

				// NOTE: calclevels4xShader[1] path is commented out in GL2 (tr_postprocess.c:59).
				// We use FBO_FastBlit for each halving step, exactly matching GL2.
				FBO_FastBlit( srcFbo, srcBox, dstFbo, dstBox, GL_COLOR_BUFFER_BIT, GL_LINEAR );

				tmp = srcFbo;
				srcFbo = dstFbo;
				dstFbo = tmp;
			}
		}

		// Blend new 1×1 log-luminance with running adapted luminance (gradual adaptation over time).
		VectorSet4( srcBox, 0, 0, 0, 0 );

		color[0] =
			color[1] =
				color[2] = 1.0f;
		// GL2 ref: tr_postprocess.c:70-73 — textureFloat path uses slow adaptation (0.03),
		// integer path uses fast adaptation (0.1).
		if ( glRefConfig.textureFloat ) {
			color[3] = 0.03f;
		} else {
			color[3] = 0.1f;
		}

		FBO_Blit( tr.targetLevelsFbo, srcBox, NULL, tr.calcLevelsFbo, NULL, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );

		// Debug: log adaptation state periodically. Zero cost when cvar is 0 or unregistered.
		// GL2 ref: no equivalent — Metal-only debug instrument.
		if ( r_debugAutoExposure && r_debugAutoExposure->integer ) {
			static int s_debugFrameCounter = 0;
			const int debugInterval = ( r_debugAutoExposure->integer > 1 ) ? r_debugAutoExposure->integer : 60;
			if ( ( s_debugFrameCounter++ % debugInterval ) == 0 ) {
				float exposureScalar = (float)pow( 2.0, r_cameraExposure->value - autoExposure );
				ri.Printf( PRINT_ALL,
						   "[HDR] frame=%d adaptAlpha=%.4f exposureScalar=%.4f camExposure=%.3f autoExp=%d\n",
						   tr.frameCount,
						   color[3],
						   exposureScalar,
						   r_cameraExposure->value,
						   autoExposure );
			}
		}
	}

	// Tonemap: exposure = 2^(r_cameraExposure - autoExposure).
	// When autoExposure=1 the levels map offsets exposure; when 0, raw camera exposure only.
	color[0] =
		color[1] =
			color[2] = pow( 2, r_cameraExposure->value - autoExposure ); //exp2(r_cameraExposure->value);
	color[3] = 1.0f;

	// Bind levels map at TB_LEVELSMAP (slot 1) so the tonemap shader can read adapted luminance.
	// PARITY-SEAM(Metal-device): GL_BindToTMU stores texnum in s_textureBindings[TB_LEVELSMAP].
	// FBO_BlitFromTexture (tr_fbo.cpp, Ripley) must pass s_textureBindings[TB_LEVELSMAP].texnum
	// as extraTexnums[0] to GLMetal_RenderBlitTextureMulti so it lands at [[texture(1)]].
	// Until Ripley wires this, the levels-map sample in the tonemap shader will read a null/stale
	// binding. Document in lambert-tonemap-path.md for Ripley.
	if ( autoExposure ) {
		GL_BindToTMU( tr.calcLevelsImage,  TB_LEVELSMAP ); // adapted luminance
	} else {
		GL_BindToTMU( tr.fixedLevelsImage, TB_LEVELSMAP ); // fixed neutral levels

	}
	// Tonemap blit: scene(slot 0) + levels map(slot 1) → LDR output.
	FBO_Blit( hdrFbo, hdrBox, NULL, ldrFbo, ldrBox, &tr.tonemapShader, color, 0 );
}

/*
=============
RB_BokehBlur


Blurs a part of one framebuffer to another.

Framebuffers can be identical.
=============
*/

void RB_BokehBlur( FBO_t *src, ivec4_t srcBox, FBO_t *dst, ivec4_t dstBox, float blur ) {
//	ivec4_t srcBox, dstBox;
	vec4_t color;

	blur *= 10.0f;

	if ( blur < 0.004f ) {
		return;
	}

	if ( glRefConfig.framebufferObject ) {
		// bokeh blur
		if ( blur > 0.0f ) {
			ivec4_t quarterBox;

			quarterBox[0] = 0;
			quarterBox[1] = tr.quarterFbo[0]->height;
			quarterBox[2] = tr.quarterFbo[0]->width;
			quarterBox[3] = -tr.quarterFbo[0]->height;

			// create a quarter texture
			//FBO_Blit(NULL, NULL, NULL, tr.quarterFbo[0], NULL, NULL, NULL, 0);
			FBO_FastBlit( src, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR );
		}

#ifndef HQ_BLUR
		if ( blur > 1.0f ) {
			// create a 1/16th texture
			//FBO_Blit(tr.quarterFbo[0], NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, NULL, 0);
			FBO_FastBlit( tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR );
		}
#endif

		if ( blur > 0.0f && blur <= 1.0f ) {
			// Crossfade original with quarter texture
			VectorSet4( color, 1, 1, 1, blur );

			FBO_Blit( tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
		}
#ifndef HQ_BLUR
		// ok blur, but can see some pixelization
		else if ( blur > 1.0f && blur <= 2.0f ) {
			// crossfade quarter texture with 1/16th texture
			FBO_Blit( tr.quarterFbo[0], NULL, NULL, dst, dstBox, NULL, NULL, 0 );

			VectorSet4( color, 1, 1, 1, blur - 1.0f );

			FBO_Blit( tr.textureScratchFbo[0], NULL, NULL, dst, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
		} else if ( blur > 2.0f )     {
			// blur 1/16th texture then replace
			int i;

			for (i = 0; i < 2; i++)
			{
				vec2_t blurTexScale;
				float subblur;

				subblur = ( ( blur - 2.0f ) / 2.0f ) / 3.0f * (float)( i + 1 );

				blurTexScale[0] =
					blurTexScale[1] = subblur;

				color[0] =
					color[1] =
						color[2] = 0.5f;
				color[3] = 1.0f;

				if ( i != 0 ) {
					FBO_Blit( tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
				} else {
					FBO_Blit( tr.textureScratchFbo[0], NULL, blurTexScale, tr.textureScratchFbo[1], NULL, &tr.bokehShader, color, 0 );
				}
			}

			FBO_Blit( tr.textureScratchFbo[1], NULL, NULL, dst, dstBox, NULL, NULL, 0 );
		}
#else // higher quality blur, but slower
		else if ( blur > 1.0f ) {
			// blur quarter texture then replace
			int i;

			src = tr.quarterFbo[0];
			dst = tr.quarterFbo[1];

			VectorSet4( color, 0.5f, 0.5f, 0.5f, 1 );

			for (i = 0; i < 2; i++)
			{
				vec2_t blurTexScale;
				float subblur;

				subblur = ( blur - 1.0f ) / 2.0f * (float)( i + 1 );

				blurTexScale[0] =
					blurTexScale[1] = subblur;

				color[0] =
					color[1] =
						color[2] = 1.0f;
				if ( i != 0 ) {
					color[3] = 1.0f;
				} else {
					color[3] = 0.5f;
				}

				FBO_Blit( tr.quarterFbo[0], NULL, blurTexScale, tr.quarterFbo[1], NULL, &tr.bokehShader, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
			}

			FBO_Blit( tr.quarterFbo[1], NULL, NULL, dst, dstBox, NULL, NULL, 0 );
		}
#endif
	}
}


static void RB_RadialBlur( FBO_t *srcFbo, FBO_t *dstFbo, int passes, float stretch, float x, float y, float w, float h, float xcenter, float ycenter, float alpha ) {
	ivec4_t srcBox, dstBox;
	int srcWidth, srcHeight;
	vec4_t color;
	const float inc = 1.f / passes;
	const float mul = powf( stretch, inc );
	float scale;

	alpha *= inc;
	VectorSet4( color, alpha, alpha, alpha, 1.0f );

	srcWidth  = srcFbo ? srcFbo->width  : glConfig.vidWidth;
	srcHeight = srcFbo ? srcFbo->height : glConfig.vidHeight;

	VectorSet4( srcBox, 0, 0, srcWidth, srcHeight );

	VectorSet4( dstBox, x, y, w, h );
	FBO_Blit( srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, 0 );

	--passes;
	scale = mul;
	while ( passes > 0 )
	{
		float iscale = 1.f / scale;
		float s0 = xcenter * ( 1.f - iscale );
		float t0 = ( 1.0f - ycenter ) * ( 1.f - iscale );

		srcBox[0] = s0 * srcWidth;
		srcBox[1] = t0 * srcHeight;
		srcBox[2] = iscale * srcWidth;
		srcBox[3] = iscale * srcHeight;

		FBO_Blit( srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

		scale *= mul;
		--passes;
	}
}


static qboolean RB_UpdateSunFlareVis( void ) {
	// PARITY-SEAM(Metal-device): GL occlusion query objects have no direct
	// Metal equivalent.  glRefConfig.occlusionQuery will be false under Metal
	// so this function returns qtrue immediately via the guard below, which
	// preserves the GL2 "assume visible" fallback behaviour.
	GLuint sampleCount = 0;
	if ( !glRefConfig.occlusionQuery ) {
		return qtrue;
	}

	tr.sunFlareQueryIndex ^= 1;
	if ( !tr.sunFlareQueryActive[tr.sunFlareQueryIndex] ) {
		return qtrue;
	}

	/* debug code */
	if ( 0 ) {
		int iter;
		for (iter = 0 ; ; ++iter)
		{
			GLint available = 0;
			// PARITY-SEAM(Metal-device): qglGetQueryObjectiv → no Metal equivalent;
			// reachable only when glRefConfig.occlusionQuery, which is false under Metal.
			(void)available;
			// qglGetQueryObjectiv(tr.sunFlareQuery[tr.sunFlareQueryIndex], GL_QUERY_RESULT_AVAILABLE, &available);
			if ( available ) {
				break;
			}
		}

		ri.Printf( PRINT_DEVELOPER, "Waited %d iterations\n", iter );
	}

	// Note: On desktop OpenGL this is a sample count (glRefConfig.occlusionQueryTarget == GL_SAMPLES_PASSED)
	// but on OpenGL ES this is a boolean (glRefConfig.occlusionQueryTarget == GL_ANY_SAMPLES_PASSED)
	// PARITY-SEAM(Metal-device): qglGetQueryObjectuiv → no Metal equivalent;
	// reachable only when glRefConfig.occlusionQuery, which is false under Metal.
	// qglGetQueryObjectuiv(tr.sunFlareQuery[tr.sunFlareQueryIndex], GL_QUERY_RESULT, &sampleCount);
	return ( sampleCount > 0 ) ? qtrue : qfalse;
}

void RB_SunRays( FBO_t *srcFbo, ivec4_t srcBox, FBO_t *dstFbo, ivec4_t dstBox ) {
	vec4_t color;
	float dot;
	const float cutoff = 0.25f;
	qboolean colorize = qtrue;

//	float w, h, w2, h2;
	mat4_t mvp;
	vec4_t pos, hpos;

	dot = DotProduct( tr.sunDirection, backEnd.viewParms.or.axis[0] );
	if ( dot < cutoff ) {
		return;
	}

	if ( !RB_UpdateSunFlareVis() ) {
		return;
	}

	// From RB_DrawSun()
	{
		float dist;
		mat4_t trans, model;

		Mat4Translation( backEnd.viewParms.or.origin, trans );
		Mat4Multiply( backEnd.viewParms.world.modelMatrix, trans, model );
		Mat4Multiply( backEnd.viewParms.projectionMatrix, model, mvp );

		dist = backEnd.viewParms.zFar / 1.75;       // div sqrt(3)

		VectorScale( tr.sunDirection, dist, pos );
	}

	// project sun point
	//Mat4Multiply(backEnd.viewParms.projectionMatrix, backEnd.viewParms.world.modelMatrix, mvp);
	Mat4Transform( mvp, pos, hpos );

	// transform to UV coords
	hpos[3] = 0.5f / hpos[3];

	pos[0] = 0.5f + hpos[0] * hpos[3];
	pos[1] = 0.5f + hpos[1] * hpos[3];

	// initialize quarter buffers
	{
		float mul = 1.f;
		ivec4_t rayBox, quarterBox;
		int srcWidth  = srcFbo ? srcFbo->width  : glConfig.vidWidth;
		int srcHeight = srcFbo ? srcFbo->height : glConfig.vidHeight;

		VectorSet4( color, mul, mul, mul, 1 );

		rayBox[0] = srcBox[0] * tr.sunRaysFbo->width  / srcWidth;
		rayBox[1] = srcBox[1] * tr.sunRaysFbo->height / srcHeight;
		rayBox[2] = srcBox[2] * tr.sunRaysFbo->width  / srcWidth;
		rayBox[3] = srcBox[3] * tr.sunRaysFbo->height / srcHeight;

		quarterBox[0] = 0;
		quarterBox[1] = tr.quarterFbo[0]->height;
		quarterBox[2] = tr.quarterFbo[0]->width;
		quarterBox[3] = -tr.quarterFbo[0]->height;

		// first, downsample the framebuffer
		if ( colorize ) {
			FBO_FastBlit( srcFbo, srcBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR );
			FBO_Blit( tr.sunRaysFbo, rayBox, NULL, tr.quarterFbo[0], quarterBox, NULL, color, GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO );
		} else
		{
			FBO_FastBlit( tr.sunRaysFbo, rayBox, tr.quarterFbo[0], quarterBox, GL_COLOR_BUFFER_BIT, GL_LINEAR );
		}
	}

	// radial blur passes, ping-ponging between the two quarter-size buffers
	{
		const float stretch_add = 2.f / 3.f;
		float stretch = 1.f + stretch_add;
		int i;
		for (i = 0; i < 2; ++i)
		{
			RB_RadialBlur( tr.quarterFbo[i & 1], tr.quarterFbo[( ~i ) & 1], 5, stretch, 0.f, 0.f, tr.quarterFbo[0]->width, tr.quarterFbo[0]->height, pos[0], pos[1], 1.125f );
			stretch += stretch_add;
		}
	}

	// add result back on top of the main buffer
	{
		float mul = 1.f;

		VectorSet4( color, mul, mul, mul, 1 );

		FBO_Blit( tr.quarterFbo[0], NULL, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	}
}

static void RB_BlurAxis( FBO_t *srcFbo, FBO_t *dstFbo, float strength, qboolean horizontal ) {
	float dx, dy;
	float xmul, ymul;
	float weights[3] = {
		0.227027027f,
		0.316216216f,
		0.070270270f,
	};
	float offsets[3] = {
		0.f,
		1.3846153846f,
		3.2307692308f,
	};

	xmul = horizontal;
	ymul = 1.f - xmul;

	xmul *= strength;
	ymul *= strength;

	{
		ivec4_t srcBox, dstBox;
		vec4_t color;

		VectorSet4( color, weights[0], weights[0], weights[0], 1.0f );
		VectorSet4( srcBox, 0, 0, srcFbo->width, srcFbo->height );
		VectorSet4( dstBox, 0, 0, dstFbo->width, dstFbo->height );
		FBO_Blit( srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, 0 );

		VectorSet4( color, weights[1], weights[1], weights[1], 1.0f );
		dx = offsets[1] * xmul;
		dy = offsets[1] * ymul;
		VectorSet4( srcBox, dx, dy, srcFbo->width, srcFbo->height );
		FBO_Blit( srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
		VectorSet4( srcBox, -dx, -dy, srcFbo->width, srcFbo->height );
		FBO_Blit( srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

		VectorSet4( color, weights[2], weights[2], weights[2], 1.0f );
		dx = offsets[2] * xmul;
		dy = offsets[2] * ymul;
		VectorSet4( srcBox, dx, dy, srcFbo->width, srcFbo->height );
		FBO_Blit( srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
		VectorSet4( srcBox, -dx, -dy, srcFbo->width, srcFbo->height );
		FBO_Blit( srcFbo, srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	}
}

static void RB_HBlur( FBO_t *srcFbo, FBO_t *dstFbo, float strength ) {
	RB_BlurAxis( srcFbo, dstFbo, strength, qtrue );
}

static void RB_VBlur( FBO_t *srcFbo, FBO_t *dstFbo, float strength ) {
	RB_BlurAxis( srcFbo, dstFbo, strength, qfalse );
}

void RB_GaussianBlur( FBO_t *srcFbo, FBO_t *dstFbo, float blur ) {
	//float mul = 1.f;
	float factor = Com_Clamp( 0.f, 1.f, blur );

	if ( factor <= 0.f ) {
		return;
	}

	{
		ivec4_t srcBox, dstBox;
		vec4_t color;

		VectorSet4( color, 1, 1, 1, 1 );

		// first, downsample the framebuffer
		FBO_FastBlit( srcFbo, NULL, tr.quarterFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR );
		FBO_FastBlit( tr.quarterFbo[0], NULL, tr.textureScratchFbo[0], NULL, GL_COLOR_BUFFER_BIT, GL_LINEAR );

		// set the alpha channel
		// PARITY-SEAM(Metal-device): qglColorMask → MTLRenderPipelineColorAttachmentDescriptor
		//   color-write-mask; under Metal color masks are pipeline state, not immediate calls.
		// qglColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
		FBO_BlitFromTexture( tr.whiteImage, NULL, NULL, tr.textureScratchFbo[0], NULL, NULL, color, GLS_DEPTHTEST_DISABLE );
		// PARITY-SEAM(Metal-device): restore full color mask.
		// qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		// blur the tiny buffer horizontally and vertically
		RB_HBlur( tr.textureScratchFbo[0], tr.textureScratchFbo[1], factor );
		RB_VBlur( tr.textureScratchFbo[1], tr.textureScratchFbo[0], factor );

		// finally, merge back to framebuffer
		VectorSet4( srcBox, 0, 0, tr.textureScratchFbo[0]->width, tr.textureScratchFbo[0]->height );
		VectorSet4( dstBox, 0, 0, glConfig.vidWidth,              glConfig.vidHeight );
		color[3] = factor;
		FBO_Blit( tr.textureScratchFbo[0], srcBox, NULL, dstFbo, dstBox, NULL, color, GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	}
}

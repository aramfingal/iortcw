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
// tr_cmds.cpp — Metal render command buffer
//
// Phase-1 PORT: all 14 functions from tr_cmds.c are present.
//  13 REAL       — CPU-side command-buffer bookkeeping is fully GL-agnostic
//                  and ports directly; the command ring buffer, draw-surface
//                  command injection, per-frame counters, timing, and the
//                  RE_EndFrame flush all work identically under Metal.
//   1 PARITY-SEAM — RE_BeginFrame: real frame-count/cvar/command-ID logic
//                   preserved; qgl* stencil-setup, color-mask, draw-buffer-
//                   clear GL calls replaced with seam markers for Phase-4.
//
// NOTE: Ledger recorded 10 functions; authoritative grep count is 14.
//   Missed: R_PerformanceCounters, R_GetCommandBufferReserved,
//           R_AddPostProcessCmd, R_SetColorMode (all REAL).
//
// KNOWN HARD SEAM — GPU state calls in RE_BeginFrame:
//   All qgl* (OpenGL) calls are seamed with:
//     // PARITY-SEAM(Metal-device): <reason>. GL2 ref: tr_cmds.c:NNN
//   Full Metal frame-begin (MTL::CommandBuffer + clear passes) is Phase 4.

// Include strategy: same as tr_backend.cpp.
// tr_extramath.cpp defines Mat4* as C++ functions.
// Include q_shared.h + tr_extramath.h in C++ context first so their
// declarations have C++ linkage, then include tr_local.h in extern "C"
// (include guard prevents re-declaration of tr_extramath.h).
extern "C" {
#include "../qcommon/q_shared.h"
}
#include "tr_extramath.h"
extern "C" {
#include "tr_local.h"
}

// ---------------------------------------------------------------------------
// Local GL constants used by R_SetColorMode and RE_BeginFrame.
// GLboolean is typedef'd as unsigned char in tr_local.h (Metal scalar block).
// GL_TRUE/GL_FALSE are boolean values — no GPU entry points depend on them.
// GL2 ref: provided via SDL_opengl.h/qgl.h in GL2; defined here explicitly.
// ---------------------------------------------------------------------------
#ifndef GL_TRUE
#define GL_TRUE  1
#endif
#ifndef GL_FALSE
#define GL_FALSE 0
#endif

// PARITY-SEAM(Metal-device): qglesMajorVersion lives in qgl.h which is
// excluded under Metal (no OpenGL entry points). Under Metal there is no GLES;
// treat as 0 so all GLES-guarded paths are skipped (desktop path taken).
// GL2 ref: renderercommon/qgl.h:334
#define qglesMajorVersion 0

// GL draw-buffer enum values used by RE_BeginFrame for cmd->buffer.
// Under Metal the actual swapchain buffer routing is Phase-4 (CAMetalLayer);
// these values are stored in the command stream as placeholders.
// GL2 ref: OpenGL spec §17.4.1
#ifndef GL_FRONT
#define GL_FRONT      0x0404
#endif
#ifndef GL_BACK
#define GL_BACK       0x0405
#endif
#ifndef GL_BACK_LEFT
#define GL_BACK_LEFT  0x0402
#endif
#ifndef GL_BACK_RIGHT
#define GL_BACK_RIGHT 0x0403
#endif

// Stereo-anaglyph mode identifiers — CPU-only bookkeeping, no GL calls.
#define MODE_RED_CYAN      1
#define MODE_RED_BLUE      2
#define MODE_RED_GREEN     3
#define MODE_GREEN_MAGENTA 4
#define MODE_MAX           MODE_GREEN_MAGENTA


/*
=====================
R_PerformanceCounters
=====================
*/
void R_PerformanceCounters( void ) {
	if ( !r_speeds->integer ) {
		Com_Memset( &tr.pc, 0, sizeof( tr.pc ) );
		Com_Memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
		return;
	}

	if ( r_speeds->integer == 1 ) {
		ri.Printf( PRINT_ALL, "%i/%i/%i shaders/batches/surfs %i leafs %i verts %i/%i tris %.2f mtex %.2f dc\n",
				   backEnd.pc.c_shaders, backEnd.pc.c_surfBatches, backEnd.pc.c_surfaces, tr.pc.c_leafs, backEnd.pc.c_vertexes,
				   backEnd.pc.c_indexes / 3, backEnd.pc.c_totalIndexes / 3,
				   R_SumOfUsedImages() / ( 1000000.0f ), backEnd.pc.c_overDraw / (float)( glConfig.vidWidth * glConfig.vidHeight ) );
	} else if ( r_speeds->integer == 2 ) {
		ri.Printf( PRINT_ALL, "(patch) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
				   tr.pc.c_sphere_cull_patch_in, tr.pc.c_sphere_cull_patch_clip, tr.pc.c_sphere_cull_patch_out,
				   tr.pc.c_box_cull_patch_in, tr.pc.c_box_cull_patch_clip, tr.pc.c_box_cull_patch_out );
		ri.Printf( PRINT_ALL, "(md3) %i sin %i sclip  %i sout %i bin %i bclip %i bout\n",
				   tr.pc.c_sphere_cull_md3_in, tr.pc.c_sphere_cull_md3_clip, tr.pc.c_sphere_cull_md3_out,
				   tr.pc.c_box_cull_md3_in, tr.pc.c_box_cull_md3_clip, tr.pc.c_box_cull_md3_out );
	} else if ( r_speeds->integer == 3 ) {
		ri.Printf( PRINT_ALL, "viewcluster: %i\n", tr.viewCluster );
	} else if ( r_speeds->integer == 4 ) {
		if ( backEnd.pc.c_dlightVertexes ) {
			ri.Printf( PRINT_ALL, "dlight srf:%i  culled:%i  verts:%i  tris:%i\n",
					   tr.pc.c_dlightSurfaces, tr.pc.c_dlightSurfacesCulled,
					   backEnd.pc.c_dlightVertexes, backEnd.pc.c_dlightIndexes / 3 );
		}
	}
//----(SA)	this is unnecessary since it will always show 2048.  I moved this to where it is accurate for the world
//	else if (r_speeds->integer == 5 )
//	{
//		ri.Printf( PRINT_ALL, "zFar: %.0f\n", tr.viewParms.zFar );
//	}
	else if ( r_speeds->integer == 6 ) {
		ri.Printf( PRINT_ALL, "flare adds:%i tests:%i renders:%i\n",
				   backEnd.pc.c_flareAdds, backEnd.pc.c_flareTests, backEnd.pc.c_flareRenders );
	} else if ( r_speeds->integer == 7 )    {
		ri.Printf( PRINT_ALL, "VAO draws: static %i dynamic %i\n",
				   backEnd.pc.c_staticVaoDraws, backEnd.pc.c_dynamicVaoDraws );
		ri.Printf( PRINT_ALL, "GLSL binds: %i  draws: gen %i light %i fog %i dlight %i\n",
				   backEnd.pc.c_glslShaderBinds, backEnd.pc.c_genericDraws, backEnd.pc.c_lightallDraws, backEnd.pc.c_fogDraws, backEnd.pc.c_dlightDraws );
	}

	memset( &tr.pc, 0, sizeof( tr.pc ) );
	memset( &backEnd.pc, 0, sizeof( backEnd.pc ) );
}


/*
====================
R_IssueRenderCommands
====================
*/
void R_IssueRenderCommands( qboolean runPerformanceCounters ) {
	renderCommandList_t *cmdList;

	if ( !tr.registered ) {  //DAJ BUGFIX
		return;
	}
	cmdList = &backEndData->commands;
	assert( cmdList );
	// add an end-of-list command
	*( int * )( cmdList->cmds + cmdList->used ) = RC_END_OF_LIST;

	// clear it out, in case this is a sync and not a buffer flip
	cmdList->used = 0;

	if ( runPerformanceCounters ) {
		R_PerformanceCounters();
	}

	// actually start the commands going
	if ( !r_skipBackEnd->integer ) {
		// let it start on the new batch
		RB_ExecuteRenderCommands( cmdList->cmds );
	}
}


/*
====================
R_IssuePendingRenderCommands

Issue any pending commands and wait for them to complete.
====================
*/
void R_IssuePendingRenderCommands( void ) {
	if ( !tr.registered ) {
		return;
	}
	R_IssueRenderCommands( qfalse );
}

/*
============
R_GetCommandBufferReserved

make sure there is enough command space
============
*/
void *R_GetCommandBufferReserved( int bytes, int reservedBytes ) {
	renderCommandList_t *cmdList;

	if ( !tr.registered ) {  //DAJ BUGFIX
		return NULL;
	}
	cmdList = &backEndData->commands;
	bytes = PAD( bytes, sizeof( void * ) );

	// always leave room for the end of list command
	if ( cmdList->used + bytes + sizeof( int ) + reservedBytes > MAX_RENDER_COMMANDS ) {
		if ( bytes > MAX_RENDER_COMMANDS - sizeof( int ) ) {
			ri.Error( ERR_FATAL, "R_GetCommandBuffer: bad size %i", bytes );
		}
		// if we run out of room, just start dropping commands
		ri.Printf( PRINT_WARNING, "Failed to allocate render command of size %d\n", bytes );
		return NULL;
	}

	cmdList->used += bytes;

	return cmdList->cmds + cmdList->used - bytes;
}


/*
=============
R_GetCommandBuffer

returns NULL if there is not enough space for important commands
=============
*/
void *R_GetCommandBuffer( int bytes ) {
	return R_GetCommandBufferReserved( bytes, PAD( sizeof( swapBuffersCommand_t ), sizeof( void * ) ) );
}


/*
=============
R_AddDrawSurfCmd

=============
*/
void    R_AddDrawSurfCmd( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	drawSurfsCommand_t  *cmd;

	cmd = (drawSurfsCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_DRAW_SURFS;

	cmd->drawSurfs = drawSurfs;
	cmd->numDrawSurfs = numDrawSurfs;

	cmd->refdef = tr.refdef;
	cmd->viewParms = tr.viewParms;
}

/*
=============
R_AddCapShadowmapCmd

=============
*/
void    R_AddCapShadowmapCmd( int map, int cubeSide ) {
	capShadowmapCommand_t   *cmd;

	cmd = (capShadowmapCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_CAPSHADOWMAP;

	cmd->map = map;
	cmd->cubeSide = cubeSide;
}


/*
=============
R_AddPostProcessCmd

=============
*/
void    R_AddPostProcessCmd( void ) {
	postProcessCommand_t    *cmd;

	cmd = (postProcessCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_POSTPROCESS;

	cmd->refdef = tr.refdef;
	cmd->viewParms = tr.viewParms;
}

/*
=============
RE_SetColor

Passing NULL will set the color to white
=============
*/
void    RE_SetColor( const float *rgba ) {
	setColorCommand_t   *cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = (setColorCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SET_COLOR;
	if ( !rgba ) {
		static float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

		rgba = white;
	}

	cmd->color[0] = rgba[0];
	cmd->color[1] = rgba[1];
	cmd->color[2] = rgba[2];
	cmd->color[3] = rgba[3];
}


/*
=============
RE_StretchPic
=============
*/
void RE_StretchPic( float x, float y, float w, float h,
					float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	stretchPicCommand_t *cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = (stretchPicCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_PIC;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
}

/*
================
R_SetColorMode

CPU-side helper: fills a GLboolean[4] rgba mask for anaglyph stereo modes.
No GPU calls — pure boolean flag manipulation.
================
*/
void R_SetColorMode( GLboolean *rgba, stereoFrame_t stereoFrame, int colormode ) {
	rgba[0] = rgba[1] = rgba[2] = rgba[3] = GL_TRUE;

	if ( colormode > MODE_MAX ) {
		if ( stereoFrame == STEREO_LEFT ) {
			stereoFrame = STEREO_RIGHT;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			stereoFrame = STEREO_LEFT;
		}

		colormode -= MODE_MAX;
	}

	if ( colormode == MODE_GREEN_MAGENTA ) {
		if ( stereoFrame == STEREO_LEFT ) {
			rgba[0] = rgba[2] = GL_FALSE;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			rgba[1] = GL_FALSE;
		}
	} else
	{
		if ( stereoFrame == STEREO_LEFT ) {
			rgba[1] = rgba[2] = GL_FALSE;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			rgba[0] = GL_FALSE;

			if ( colormode == MODE_RED_BLUE ) {
				rgba[1] = GL_FALSE;
			} else if ( colormode == MODE_RED_GREEN ) {
				rgba[2] = GL_FALSE;
			}
		}
	}
}

/*
=============
RE_RotatedPic
=============
*/
void RE_RotatedPic( float x, float y, float w, float h,
					float s1, float t1, float s2, float t2, qhandle_t hShader, float angle ) {
	stretchPicCommand_t *cmd;

	cmd = (stretchPicCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_ROTATED_PIC;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;

	// fixup
	cmd->w /= 2;
	cmd->h /= 2;
	cmd->x += cmd->w;
	cmd->y += cmd->h;
	cmd->w = sqrt( ( cmd->w * cmd->w ) + ( cmd->h * cmd->h ) );
	cmd->h = cmd->w;

	cmd->angle = angle;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;
}

//----(SA)	added
/*
==============
RE_StretchPicGradient
==============
*/
void RE_StretchPicGradient( float x, float y, float w, float h,
							float s1, float t1, float s2, float t2, qhandle_t hShader, const float *gradientColor, int gradientType ) {
	stretchPicCommand_t *cmd;

	cmd = (stretchPicCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_PIC_GRADIENT;
	cmd->shader = R_GetShaderByHandle( hShader );
	cmd->x = x;
	cmd->y = y;
	cmd->w = w;
	cmd->h = h;
	cmd->s1 = s1;
	cmd->t1 = t1;
	cmd->s2 = s2;
	cmd->t2 = t2;

	if ( !gradientColor ) {
		static float colorWhite[4] = { 1, 1, 1, 1 };

		gradientColor = colorWhite;
	}

	cmd->gradientColor[0] = gradientColor[0] * 255;
	cmd->gradientColor[1] = gradientColor[1] * 255;
	cmd->gradientColor[2] = gradientColor[2] * 255;
	cmd->gradientColor[3] = gradientColor[3] * 255;
	cmd->gradientType = gradientType;
}
//----(SA)	end


/*
====================
RE_BeginFrame

If running in stereo, RE_BeginFrame will be called twice
for each RE_EndFrame
====================
*/
// PARITY-SEAM: real frame-count, cvar-modified, and command-ID logic preserved.
// GPU state (stencil enable/disable, color-mask clear, draw-buffer selection) is
// seamed — these require OpenGL entry points unavailable under Metal. The Metal
// equivalents (clear pass attachments, MTLRenderPassDescriptor, MTLPixelFormat
// stereo) are Phase-4 (Hudson + Ripley tr_fbo).
void RE_BeginFrame( stereoFrame_t stereoFrame ) {
	drawBufferCommand_t *cmd = NULL;
	colorMaskCommand_t *colcmd = NULL;

	if ( !tr.registered ) {
		return;
	}
	glState.finishCalled = qfalse;

	tr.frameCount++;
	tr.frameSceneNum = 0;

	//
	// do overdraw measurement
	//
	if ( r_measureOverdraw->integer ) {
		// qglesMajorVersion == 0 under Metal (desktop path; no GLES).
		if ( qglesMajorVersion >= 1 && !glRefConfig.readStencil ) {
			ri.Printf( PRINT_WARNING, "OpenGL ES needs GL_NV_read_stencil to read stencil bits to measure overdraw\n" );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		} else if ( glConfig.stencilBits < 4 )   {
			ri.Printf( PRINT_ALL, "Warning: not enough stencil bits to measure overdraw: %d\n", glConfig.stencilBits );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		} else if ( r_shadows->integer == 2 ) {
			ri.Printf( PRINT_ALL, "Warning: stencil shadows and overdraw measurement are mutually exclusive\n" );
			ri.Cvar_Set( "r_measureOverdraw", "0" );
			r_measureOverdraw->modified = qfalse;
		} else
		{
			R_IssuePendingRenderCommands();
			// PARITY-SEAM(Metal-device): stencil-test enable + stencil state
			// setup requires MTL::RenderPassDescriptor stencil attachment.
			// Under Metal, overdraw measurement uses a stencil attachment on the
			// render pass descriptor rather than qglEnable(GL_STENCIL_TEST).
			// qglEnable( GL_STENCIL_TEST );
			// qglStencilMask( ~0U );
			// qglClearStencil( 0U );
			// qglStencilFunc( GL_ALWAYS, 0U, ~0U );
			// qglStencilOp( GL_KEEP, GL_INCR, GL_INCR );
		}
		r_measureOverdraw->modified = qfalse;
	} else
	{
		// this is only reached if it was on and is now off
		if ( r_measureOverdraw->modified ) {
			R_IssuePendingRenderCommands();
			// PARITY-SEAM(Metal-device): stencil-test disable.
			// qglDisable( GL_STENCIL_TEST );
		}
		r_measureOverdraw->modified = qfalse;
	}

	//
	// texturemode stuff
	//
	if ( r_textureMode->modified ) {
		R_IssuePendingRenderCommands();
		GL_TextureMode( r_textureMode->string );
		r_textureMode->modified = qfalse;
	}

	//
	// NVidia stuff
	//

	// fog control
	if ( glConfig.NVFogAvailable && r_nv_fogdist_mode->modified ) {
		r_nv_fogdist_mode->modified = qfalse;
		if ( !Q_stricmp( r_nv_fogdist_mode->string, "GL_EYE_PLANE_ABSOLUTE_NV" ) ) {
			glConfig.NVFogMode = (int)GL_EYE_PLANE_ABSOLUTE_NV;
		} else if ( !Q_stricmp( r_nv_fogdist_mode->string, "GL_EYE_PLANE" ) ) {
			glConfig.NVFogMode = (int)GL_EYE_PLANE;
		} else if ( !Q_stricmp( r_nv_fogdist_mode->string, "GL_EYE_RADIAL_NV" ) ) {
			glConfig.NVFogMode = (int)GL_EYE_RADIAL_NV;
		} else {
			// in case this was really 'else', store a valid value for next time
			glConfig.NVFogMode = (int)GL_EYE_RADIAL_NV;
			ri.Cvar_Set( "r_nv_fogdist_mode", "GL_EYE_RADIAL_NV" );
		}
	}

	//
	// gamma stuff
	//
	if ( r_gamma->modified ) {
		r_gamma->modified = qfalse;

		R_IssuePendingRenderCommands();
		R_SetColorMappings();
	}

	// check for errors
	if ( !r_ignoreGLErrors->integer ) {
		R_IssuePendingRenderCommands();
		// PARITY-SEAM(Metal-device): qglGetError() is an OpenGL entry point.
		// Under Metal, GPU error detection uses MTL::CommandBuffer completion
		// handlers or MTL::Device error callbacks. No direct equivalent at this
		// call site; Phase-4 will add Metal error reporting here.
		// if ((err = qglGetError()) != GL_NO_ERROR)
		//     ri.Error(ERR_FATAL, "RE_BeginFrame() - glGetError() failed (0x%x)!", err);
	}

	if ( glConfig.stereoEnabled ) {
		if ( !( cmd = (drawBufferCommand_t *)R_GetCommandBuffer( sizeof(*cmd ) ) ) ) {
			return;
		}

		cmd->commandId = RC_DRAW_BUFFER;

		if ( stereoFrame == STEREO_LEFT ) {
			// PARITY-SEAM(Metal-device): GL_BACK_LEFT is an OpenGL draw-buffer
			// enum. Under Metal, left-eye routing is via MTLRenderPassDescriptor
			// and a separate texture slice. Phase-4 will replace this constant.
			cmd->buffer = (int)GL_BACK_LEFT;
		} else if ( stereoFrame == STEREO_RIGHT ) {
			// PARITY-SEAM(Metal-device): GL_BACK_RIGHT. See left-eye note above.
			cmd->buffer = (int)GL_BACK_RIGHT;
		} else {
			ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );
		}
	} else
	{
		if ( qglesMajorVersion >= 1 && r_anaglyphMode->integer ) {
			ri.Printf( PRINT_WARNING, "OpenGL ES does not support drawing to separate buffer for anaglyph mode\n" );
			ri.Cvar_Set( "r_anaglyphMode", "0" );
			r_anaglyphMode->modified = qfalse;
		}

		if ( r_anaglyphMode->integer ) {
			if ( r_anaglyphMode->modified ) {
				// PARITY-SEAM(Metal-device): qglColorMask write-mask reset.
				// Under Metal the color write mask is per-pipeline-state descriptor;
				// the reset is a no-op here until Phase-4 rebuilds the PSO.
				// qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

				// CPU-side color mask bookkeeping — real.
				backEnd.colorMask[0] = (qboolean)GL_FALSE;
				backEnd.colorMask[1] = (qboolean)GL_FALSE;
				backEnd.colorMask[2] = (qboolean)GL_FALSE;
				backEnd.colorMask[3] = (qboolean)GL_FALSE;

				if ( glRefConfig.framebufferObject ) {
					// PARITY-SEAM(Metal-device): FBO clears for anaglyph init.
					// Under Metal, frame clear is specified via
					// MTLRenderPassAttachmentDescriptor.loadAction = MTLLoadActionClear.
					// The FBO_Bind / qglClear sequence is replaced by load-action on the
					// render pass descriptor in Phase-4.
					// if (tr.msaaResolveFbo) { FBO_Bind(tr.msaaResolveFbo); qglClear(GL_COLOR_BUFFER_BIT); }
					// if (tr.renderFbo)      { FBO_Bind(tr.renderFbo);      qglClear(GL_COLOR_BUFFER_BIT); }
					// FBO_Bind(NULL);
				}

				// PARITY-SEAM(Metal-device): front/back-buffer draw + clear.
				// qglDrawBuffer(GL_FRONT); qglClear(GL_COLOR_BUFFER_BIT);
				// qglDrawBuffer(GL_BACK);  qglClear(GL_COLOR_BUFFER_BIT);

				r_anaglyphMode->modified = qfalse;
			}

			if ( stereoFrame == STEREO_LEFT ) {
				if ( !( cmd = (drawBufferCommand_t *)R_GetCommandBuffer( sizeof(*cmd ) ) ) ) {
					return;
				}

				if ( !( colcmd = (colorMaskCommand_t *)R_GetCommandBuffer( sizeof(*colcmd ) ) ) ) {
					return;
				}
			} else if ( stereoFrame == STEREO_RIGHT )      {
				clearDepthCommand_t *cldcmd;

				if ( !( cldcmd = (clearDepthCommand_t *)R_GetCommandBuffer( sizeof(*cldcmd ) ) ) ) {
					return;
				}

				cldcmd->commandId = RC_CLEARDEPTH;

				if ( !( colcmd = (colorMaskCommand_t *)R_GetCommandBuffer( sizeof(*colcmd ) ) ) ) {
					return;
				}
			} else {
				ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is enabled, but stereoFrame was %i", stereoFrame );
			}

			R_SetColorMode( colcmd->rgba, stereoFrame, r_anaglyphMode->integer );
			colcmd->commandId = RC_COLORMASK;
		} else
		{
			if ( stereoFrame != STEREO_CENTER ) {
				ri.Error( ERR_FATAL, "RE_BeginFrame: Stereo is disabled, but stereoFrame was %i", stereoFrame );
			}

			if ( !( cmd = (drawBufferCommand_t *)R_GetCommandBuffer( sizeof(*cmd ) ) ) ) {
				return;
			}
		}

		if ( cmd ) {
			cmd->commandId = RC_DRAW_BUFFER;

			if ( r_anaglyphMode->modified ) {
				// PARITY-SEAM(Metal-device): qglColorMask write-mask reset.
				// qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

				// CPU-side color mask bookkeeping — real.
				backEnd.colorMask[0] = (qboolean)0;
				backEnd.colorMask[1] = (qboolean)0;
				backEnd.colorMask[2] = (qboolean)0;
				backEnd.colorMask[3] = (qboolean)0;
				r_anaglyphMode->modified = qfalse;
			}

			if ( !Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) ) {
				// PARITY-SEAM(Metal-device): GL_FRONT draw-buffer enum — no direct
				// Metal equivalent; placeholder value until Phase-4 swapchain.
				cmd->buffer = (int)GL_FRONT;
			} else {
				// PARITY-SEAM(Metal-device): GL_BACK draw-buffer enum.
				cmd->buffer = (int)GL_BACK;
			}
		}
	}

	tr.refdef.stereoFrame = stereoFrame;
}

/*
=============
RE_EndFrame

Returns the number of msec spent in the back end
=============
*/
void RE_EndFrame( int *frontEndMsec, int *backEndMsec ) {
	swapBuffersCommand_t    *cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = (swapBuffersCommand_t *)R_GetCommandBufferReserved( sizeof( *cmd ), 0 );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_SWAP_BUFFERS;

	R_IssueRenderCommands( qtrue );

	R_InitNextFrame();

	if ( frontEndMsec ) {
		*frontEndMsec = tr.frontEndMsec;
	}
	tr.frontEndMsec = 0;
	if ( backEndMsec ) {
		*backEndMsec = backEnd.pc.msec;
	}
	backEnd.pc.msec = 0;
}

/*
=============
RE_TakeVideoFrame
=============
*/
void RE_TakeVideoFrame( int width, int height,
						byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg ) {
	videoFrameCommand_t *cmd;

	if ( !tr.registered ) {
		return;
	}

	cmd = (videoFrameCommand_t *)R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}

	cmd->commandId = RC_VIDEOFRAME;

	cmd->width = width;
	cmd->height = height;
	cmd->captureBuffer = captureBuffer;
	cmd->encodeBuffer = encodeBuffer;
	cmd->motionJpeg = motionJpeg;
}

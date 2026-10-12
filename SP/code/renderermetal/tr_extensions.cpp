/*
===========================================================================
Copyright (C) 2011 James Canete (use.less01@gmail.com)

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_extensions.cpp - Metal counterpart to tr_extensions.c (GL extension loader)
//
// PARITY CONTRACT: 1 function — GLimp_InitExtraExtensions (tr_extensions.c:33).
// The entire function body is a documented PARITY-SEAM: GL extension probing
// and qgl* function-pointer binding have no Metal equivalent.  glRefConfig.*
// capability flags are instead set to static Metal-appropriate constant values
// that the already-ported renderer files expect.
//

extern "C" {
#include "tr_local.h"
}

// ============================================================================
// FUNCTION 1/1: GLimp_InitExtraExtensions
//
// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
//   GL2 probes the driver via QGL_VERSION_ATLEAST, SDL_GL_ExtensionSupported,
//   and SDL_GL_GetProcAddress, then binds qgl* function pointers and sets
//   glRefConfig.* capability flags.  Under Metal there is no GL driver, no
//   extension strings, and no qgl* function pointers.  Every qgl* pointer
//   binding is intentionally OMITTED (documented inline).  All glRefConfig.*
//   fields are set to the static constant values the ported renderer expects.
//
// ============================================================================
void GLimp_InitExtraExtensions( void ) {
	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2 calls qglGetString(GL_RENDERER) to detect Intel workaround.  No Intel
	// GL workaround needed under Metal — the Metal driver is not the Intel GL path.
	// -------------------------------------------------------------------------
	glRefConfig.intelGraphics = qfalse;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2 branches on qglesMajorVersion to select 16-bit vs 32-bit index type.
	// Under Metal qglesMajorVersion = 0 (no GLES, see GL-COMPAT in tr_local.h).
	// Metal supports MTLIndexTypeUInt32 natively; always use 32-bit indices.
	// -------------------------------------------------------------------------
	glRefConfig.vaoCacheGlIndexType = GL_UNSIGNED_INT;
	glRefConfig.vaoCacheGlIndexSize = sizeof( unsigned int );

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2: #define GLE(ret, name, ...) qgl##name = GLDSA_##name;
	//      QGL_EXT_direct_state_access_PROCS; // set DSA fallbacks
	// qgl* function pointers do not exist under Metal.  tr_dsa.cpp provides
	// Metal-side seams for DSA wrapper logic; no pointer-binding needed here.
	// -------------------------------------------------------------------------

	// -------------------------------------------------------------------------
	// PARITY-STUB: OpenGL ES-specific extension path.
	// The entire `if (qglesMajorVersion)` block (lines 62–161) is unreachable
	// under Metal (qglesMajorVersion is 0 per GL-COMPAT) and is intentionally
	// skipped.  The individual GLES caps below (readDepth, readStencil, etc.)
	// are set to their Metal-appropriate static values further down.
	// -------------------------------------------------------------------------

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2: OpenGL 1.5 — GL_ARB_occlusion_query
	//   glRefConfig.occlusionQuery = qtrue;
	//   glRefConfig.occlusionQueryTarget = GL_SAMPLES_PASSED;
	//   QGL_ARB_occlusion_query_PROCS; // binds qglGenQueries / qglBeginQuery / etc.
	// qgl* bindings OMITTED — no qgl* under Metal.  Metal occlusion testing uses
	// MTLVisibilityResultBuffer (MTLRenderCommandEncoder setVisibilityResultMode),
	// wired at Phase 4 device bring-up.
	// occlusionQueryTarget: no GL target enum; sentinel 0 (unused until Phase 4).
	// -------------------------------------------------------------------------
	glRefConfig.occlusionQuery       = qtrue;
	glRefConfig.occlusionQueryTarget = 0;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2: OpenGL 3.0 — GL_ARB_framebuffer_object
	//   qglGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, ...) / GL_MAX_COLOR_ATTACHMENTS
	//   QGL_ARB_framebuffer_object_PROCS; // binds qglBindFramebuffer / etc.
	// qgl* bindings OMITTED.
	// Metal A1 supports texture-backed render targets.  Keep GL-only blit/MSAA
	// capability flags disabled so unsupported multisample/resolve paths stay off.
	// -------------------------------------------------------------------------
	glRefConfig.framebufferObject      = qtrue;
	glRefConfig.framebufferBlit        = qfalse;
	glRefConfig.framebufferMultisample = qfalse;
	glRefConfig.maxRenderbufferSize    = 16384;
	glRefConfig.maxColorAttachments    = 1;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2: OpenGL 3.0 — GL_ARB_vertex_array_object
	//   QGL_ARB_vertex_array_object_PROCS; // binds qglGenVertexArrays / etc.
	// qgl* bindings OMITTED.
	// Metal always has VAO-equivalent buffer binding via MTLRenderCommandEncoder
	// setVertexBuffer.  Set qtrue so tr_vbo.cpp takes the VAO allocation and
	// binding paths (all qgl* calls within those paths are seamed to no-ops).
	// -------------------------------------------------------------------------
	glRefConfig.vertexArrayObject = qtrue;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2: OpenGL 3.0 — GL_ARB_texture_float
	//   glRefConfig.textureFloat = !!r_ext_texture_float->integer;
	// Metal always supports float textures (MTLPixelFormatRGBA16Float,
	// MTLPixelFormatRGBA32Float).  Always qtrue.
	// -------------------------------------------------------------------------
	glRefConfig.textureFloat = qtrue;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2: OpenGL 3.2 — GL_ARB_depth_clamp
	// Metal always performs depth clamping natively via MTLDepthStencilDescriptor.
	// -------------------------------------------------------------------------
	glRefConfig.depthClamp = qtrue;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2: OpenGL 3.2 — GL_ARB_seamless_cube_map
	//   glRefConfig.seamlessCubeMap = !!r_arb_seamless_cube_map->integer;
	// Metal always performs seamless cube-map filtering with no per-face seams.
	// -------------------------------------------------------------------------
	glRefConfig.seamlessCubeMap = qtrue;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL_NVX_gpu_memory_info / GL_ATI_meminfo — no Metal equivalent.
	// MTLDevice.recommendedMaxWorkingSetSize is the Metal analogue (Phase 4).
	// -------------------------------------------------------------------------
	glRefConfig.memInfo = MI_NONE;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL_ARB_texture_compression_rgtc / GL_ARB_texture_compression_bptc
	// BCx formats are available on macOS via MTLPixelFormatBC4_RUnorm (RGTC) and
	// MTLPixelFormatBC7_RGBAUnorm (BPTC).  Setting TCR_NONE for seam consistency
	// until the BC upload path in tr_image.cpp is wired to MTLPixelFormat.
	// -------------------------------------------------------------------------
	glRefConfig.textureCompression = TCR_NONE;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2: swizzleNormalmap = r_ext_compressed_textures->integer && !(TCR_RGTC)
	// With textureCompression = TCR_NONE, swizzle is not needed.  qfalse.
	// -------------------------------------------------------------------------
	glRefConfig.swizzleNormalmap = qfalse;

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL_EXT_direct_state_access — GL-only.  tr_dsa.cpp provides Metal-side seams
	// for the DSA wrapper functions; no GL DSA pointer-binding or flag needed.
	// QGL_EXT_direct_state_access_PROCS bindings OMITTED.
	// -------------------------------------------------------------------------
	glRefConfig.directStateAccess = qfalse;

	// -------------------------------------------------------------------------
	// PARITY-STUB: OpenGL ES capability flags (no GLES under Metal).
	// GL2 ref: tr_extensions.c:98 (readDepth), :111 (readStencil),
	//          :123 (shadowSamplers), :135 (standardDerivatives)
	// GL_NV_read_depth / GL_NV_read_stencil: Metal uses depth/stencil texture
	// reads via MTLRenderPassDescriptor storeAction (Phase 4).
	// GL_EXT_shadow_samplers / GL_OES_standard_derivatives: Metal MSL always
	// supports shadow texture sampling and dFdx/dFdy derivatives.
	// -------------------------------------------------------------------------
	glRefConfig.readDepth          = qfalse;  // Metal: texture read (Phase 4)
	glRefConfig.readStencil        = qfalse;  // Metal: texture read (Phase 4)
	glRefConfig.shadowSamplers     = qtrue;   // Metal MSL: always available
	glRefConfig.standardDerivatives = qtrue;  // Metal MSL: always available

	// -------------------------------------------------------------------------
	// PARITY-SEAM(Metal-device): GL extension probing → static Metal capabilities.
	// GL2 ref: tr_extensions.c:350 (done:), tr_extensions.c:352
	// GL2: queries GL_SHADING_LANGUAGE_VERSION string and parses it into
	//   glRefConfig.glslMajorVersion / glRefConfig.glslMinorVersion.
	// Under Metal the shading language is MSL.  Set the equivalent GLSL version
	// to 4.50 (desktop GLSL) so tr_glsl.cpp line 324 takes the "version 150"
	// header branch (glslMajorVersion > 1 path) and not the GLES fallback.
	// NOTE: glslMaxAnimatedBones and maxVertexAttribs are already populated in
	// InitOpenGL (tr_init.cpp:445-446); do not overwrite them here.
	// -------------------------------------------------------------------------
	glRefConfig.glslMajorVersion = 4;
	glRefConfig.glslMinorVersion = 50;

	// -------------------------------------------------------------------------
	// F1: ANISOTROPIC FILTERING — Metal always supports anisotropy.
	// Mirror GL2 sdl_glimp.c:1031-1043 semantics exactly:
	//   if (r_ext_texture_filter_anisotropic && extension supported) {
	//       textureFilterAnisotropic = qtrue;
	//       maxAnisotropy = (int)glConfig.maxTextureFilterAnisotropy; }
	// Under Metal the hardware cap is 16 (Apple GPU MTLSamplerDescriptor maxAnisotropy).
	// r_ext_max_anisotropy->value replaces the GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT query.
	// -------------------------------------------------------------------------
	if ( r_ext_texture_filter_anisotropic->integer ) {
		textureFilterAnisotropic = qtrue;
		// Clamp user-requested level to [1, 16] — 16 is the Metal hardware cap.
		// GL2 uses glConfig.maxTextureFilterAnisotropy (queried from driver) as upper bound.
		maxAnisotropy = (int)Com_Clamp( 1.0f, 16.0f, r_ext_max_anisotropy->value );
		ri.Printf( PRINT_ALL, "...anisotropic filtering: qtrue (maxAnisotropy = %d)\n", maxAnisotropy );
	} else {
		textureFilterAnisotropic = qfalse;
		maxAnisotropy = 1;
		ri.Printf( PRINT_ALL, "...anisotropic filtering: disabled (r_ext_texture_filter_anisotropic = 0)\n" );
	}

	ri.Printf( PRINT_ALL, "...Metal: static capabilities (no GL extension probing)\n" );
}

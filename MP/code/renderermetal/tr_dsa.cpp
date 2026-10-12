/*
===========================================================================
Copyright (C) 2016 James Canete

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
===========================================================================
*/

// GL2 ref: tr_dsa.c (1:1 parity)
//
// PURPOSE: GL Direct State Access emulation / shadow-copy state-cache layer.
//   Under Metal the qgl* GPU-dispatch calls are PARITY-SEAM; all CPU-side
//   shadow-copy compare-and-store (glDsaState.*) logic is kept REAL so that
//   callers can query / update cached state even without a GPU round-trip.
//
// SEAM DISCIPLINE:
//   REAL  — glDsaState compare-and-store bookkeeping (preserved verbatim).
//   SEAM  — every qgl* / GLDSA_* function-pointer GPU call replaced with
//            // PARITY-SEAM(Metal-device): <Metal equivalent> comment.
//   STUB  — never-reached GLDSA_* fallback bodies whose only work is GPU I/O.

// GL-COMPAT: GL enum constants needed by this TU only.  #ifndef-guarded so
// they coexist with any future consolidated header.  Values from OpenGL 3.x spec.
#ifndef GL_TEXTURE0
#define GL_TEXTURE0                         0x84C0
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D                       0x0DE1
#endif
#ifndef GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP                 0x8513
#endif
#ifndef GL_TEXTURE_CUBE_MAP_POSITIVE_X
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X      0x8515
#endif
#ifndef GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z      0x851F
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

extern "C" {
#include "tr_local.h"
#include "tr_dsa.h"
}

// GL2 ref: tr_dsa.c:25-36 — glDsaState shadow-copy struct.
// REAL: kept verbatim; fields are updated by the compare-and-store wrappers below.
static struct
{
	GLuint textures[NUM_TEXTURE_BUNDLES];
	GLenum texunit;

	GLuint program;

	GLuint drawFramebuffer;
	GLuint readFramebuffer;
	GLuint renderbuffer;
}
glDsaState;

void GL_BindNullTextures( void ) {
	int i;

	if ( glRefConfig.directStateAccess ) {
		for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
		{
			// PARITY-SEAM(Metal-device): qglBindMultiTextureEXT(GL_TEXTURE0+i, GL_TEXTURE_2D, 0)
			// → MTLRenderCommandEncoder setFragmentTexture:nil atIndex:i.
			glDsaState.textures[i] = 0;
		}
	} else
	{
		for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
		{
			// PARITY-SEAM(Metal-device): qglActiveTexture(GL_TEXTURE0+i) → no explicit TMU
			// selection under Metal; texture index passed directly to encoder.
			// PARITY-SEAM(Metal-device): qglBindTexture(GL_TEXTURE_2D, 0)
			// → MTLRenderCommandEncoder setFragmentTexture:nil atIndex:i.
			glDsaState.textures[i] = 0;
		}

		// PARITY-SEAM(Metal-device): qglActiveTexture(GL_TEXTURE0) → no-op under Metal.
		glDsaState.texunit = GL_TEXTURE0;
	}
}

int GL_BindMultiTexture( GLenum texunit, GLenum target, GLuint texture ) {
	GLuint tmu = texunit - GL_TEXTURE0;

	// REAL: shadow-copy early-out. GL2 ref: tr_dsa.c:68
	if ( glDsaState.textures[tmu] == texture ) {
		return 0;
	}

	// REAL: cube-map face → cube-map target normalisation. GL2 ref: tr_dsa.c:71
	if ( target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ) {
		target = GL_TEXTURE_CUBE_MAP;
	}

	// PARITY-SEAM(Metal-device): qglBindMultiTextureEXT(texunit, target, texture)
	// → MTLRenderCommandEncoder setFragmentTexture:atIndex: / setVertexTexture:atIndex:.
	glDsaState.textures[tmu] = texture;
	return 1;
}

GLvoid APIENTRY GLDSA_BindMultiTextureEXT( GLenum texunit, GLenum target, GLuint texture ) {
	// REAL: texunit shadow-copy guard. GL2 ref: tr_dsa.c:81
	if ( glDsaState.texunit != texunit ) {
		// PARITY-SEAM(Metal-device): qglActiveTexture(texunit) → no explicit TMU
		// selection under Metal; index used directly by encoder.
		glDsaState.texunit = texunit;
	}

	// PARITY-SEAM(Metal-device): qglBindTexture(target, texture)
	// → MTLRenderCommandEncoder setFragmentTexture:atIndex:.
	(void)target; (void)texture;
}

GLvoid APIENTRY GLDSA_TextureParameterfEXT( GLuint texture, GLenum target, GLenum pname, GLfloat param ) {
	// REAL: bind path updates glDsaState.textures cache. GL2 ref: tr_dsa.c:92
	GL_BindMultiTexture( glDsaState.texunit, target, texture );
	// PARITY-SEAM(Metal-device): qglTexParameterf(target, pname, param)
	// → MTLSamplerDescriptor property set (e.g. minFilter/magFilter/addressMode).
	(void)pname; (void)param;
}

GLvoid APIENTRY GLDSA_TextureParameteriEXT( GLuint texture, GLenum target, GLenum pname, GLint param ) {
	// REAL: bind path updates glDsaState.textures cache. GL2 ref: tr_dsa.c:98
	GL_BindMultiTexture( glDsaState.texunit, target, texture );
	// PARITY-SEAM(Metal-device): qglTexParameteri(target, pname, param)
	// → MTLSamplerDescriptor property set.
	(void)pname; (void)param;
}

GLvoid APIENTRY GLDSA_TextureImage2DEXT( GLuint texture, GLenum target, GLint level, GLint internalformat,
										 GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels ) {
	// REAL: bind path updates glDsaState.textures cache. GL2 ref: tr_dsa.c:105
	GL_BindMultiTexture( glDsaState.texunit, target, texture );
	// PARITY-SEAM(Metal-device): qglTexImage2D(target, level, internalformat, width, height,
	// border, format, type, pixels) → MTLTexture newTextureWithDescriptor: / replaceRegion:.
	(void)level; (void)internalformat; (void)width; (void)height;
	(void)border; (void)format; (void)type; (void)pixels;
}

GLvoid APIENTRY GLDSA_TextureSubImage2DEXT( GLuint texture, GLenum target, GLint level, GLint xoffset, GLint yoffset,
											GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels ) {
	// REAL: bind path updates glDsaState.textures cache. GL2 ref: tr_dsa.c:112
	GL_BindMultiTexture( glDsaState.texunit, target, texture );
	// PARITY-SEAM(Metal-device): qglTexSubImage2D(target, level, xoffset, yoffset,
	// width, height, format, type, pixels) → MTLTexture replaceRegion:mipmapLevel:withBytes:.
	(void)level; (void)xoffset; (void)yoffset; (void)width; (void)height;
	(void)format; (void)type; (void)pixels;
}

GLvoid APIENTRY GLDSA_CopyTextureSubImage2DEXT( GLuint texture, GLenum target, GLint level, GLint xoffset, GLint yoffset,
												GLint x, GLint y, GLsizei width, GLsizei height ) {
	// REAL: bind path updates glDsaState.textures cache. GL2 ref: tr_dsa.c:119
	GL_BindMultiTexture( glDsaState.texunit, target, texture );
	// PARITY-SEAM(Metal-device): qglCopyTexSubImage2D(target, level, xoffset, yoffset, x, y,
	// width, height) → MTLBlitCommandEncoder copyFromTexture:toTexture: region.
	(void)level; (void)xoffset; (void)yoffset; (void)x; (void)y; (void)width; (void)height;
}

GLvoid APIENTRY  GLDSA_CompressedTextureImage2DEXT( GLuint texture, GLenum target, GLint level, GLenum internalformat,
													GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data ) {
	// REAL: bind path updates glDsaState.textures cache. GL2 ref: tr_dsa.c:126
	GL_BindMultiTexture( glDsaState.texunit, target, texture );
	// PARITY-SEAM(Metal-device): qglCompressedTexImage2D(target, level, internalformat,
	// width, height, border, imageSize, data) → MTLTexture replaceRegion: with compressed data.
	(void)level; (void)internalformat; (void)width; (void)height;
	(void)border; (void)imageSize; (void)data;
}

GLvoid APIENTRY GLDSA_CompressedTextureSubImage2DEXT( GLuint texture, GLenum target, GLint level,
													  GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format,
													  GLsizei imageSize, const GLvoid *data ) {
	// REAL: bind path updates glDsaState.textures cache. GL2 ref: tr_dsa.c:134
	GL_BindMultiTexture( glDsaState.texunit, target, texture );
	// PARITY-SEAM(Metal-device): qglCompressedTexSubImage2D(target, level, xoffset, yoffset,
	// width, height, format, imageSize, data) → MTLTexture replaceRegion: compressed sub-region.
	(void)level; (void)xoffset; (void)yoffset; (void)width; (void)height;
	(void)format; (void)imageSize; (void)data;
}

GLvoid APIENTRY GLDSA_GenerateTextureMipmapEXT( GLuint texture, GLenum target ) {
	// REAL: bind path updates glDsaState.textures cache. GL2 ref: tr_dsa.c:140
	GL_BindMultiTexture( glDsaState.texunit, target, texture );
	// PARITY-SEAM(Metal-device): qglGenerateMipmap(target)
	// → MTLBlitCommandEncoder generateMipmapsForTexture:.
}

void GL_BindNullProgram( void ) {
	// PARITY-SEAM(Metal-device): qglUseProgram(0)
	// → MTLRenderCommandEncoder setRenderPipelineState:nil (pipeline reset).
	glDsaState.program = 0;
}

int GL_UseProgram( GLuint program ) {
	// REAL: shadow-copy early-out. GL2 ref: tr_dsa.c:152
	if ( glDsaState.program == program ) {
		return 0;
	}

	// PARITY-SEAM(Metal-device): qglUseProgram(program)
	// → MTLRenderCommandEncoder setRenderPipelineState: (pipeline bind).
	glDsaState.program = program;
	return 1;
}

GLvoid APIENTRY GLDSA_ProgramUniform1iEXT( GLuint program, GLint location, GLint v0 ) {
	// REAL: program bind updates glDsaState.program cache. GL2 ref: tr_dsa.c:162
	GL_UseProgram( program );
	// PARITY-SEAM(Metal-device): qglUniform1i(location, v0)
	// → MTLRenderCommandEncoder setFragmentBytes:length:atIndex: (integer uniform).
	(void)location; (void)v0;
}

GLvoid APIENTRY GLDSA_ProgramUniform1fEXT( GLuint program, GLint location, GLfloat v0 ) {
	// REAL: program bind updates glDsaState.program cache. GL2 ref: tr_dsa.c:168
	GL_UseProgram( program );
	// PARITY-SEAM(Metal-device): qglUniform1f(location, v0)
	// → MTLRenderCommandEncoder setFragmentBytes:length:atIndex: (float uniform).
	(void)location; (void)v0;
}

GLvoid APIENTRY GLDSA_ProgramUniform2fEXT( GLuint program, GLint location,
										   GLfloat v0, GLfloat v1 ) {
	// REAL: program bind updates glDsaState.program cache. GL2 ref: tr_dsa.c:175
	GL_UseProgram( program );
	// PARITY-SEAM(Metal-device): qglUniform2f(location, v0, v1)
	// → MTLRenderCommandEncoder setFragmentBytes:length:atIndex: (vec2 uniform).
	(void)location; (void)v0; (void)v1;
}

GLvoid APIENTRY GLDSA_ProgramUniform3fEXT( GLuint program, GLint location,
										   GLfloat v0, GLfloat v1, GLfloat v2 ) {
	// REAL: program bind updates glDsaState.program cache. GL2 ref: tr_dsa.c:182
	GL_UseProgram( program );
	// PARITY-SEAM(Metal-device): qglUniform3f(location, v0, v1, v2)
	// → MTLRenderCommandEncoder setFragmentBytes:length:atIndex: (vec3 uniform).
	(void)location; (void)v0; (void)v1; (void)v2;
}

GLvoid APIENTRY GLDSA_ProgramUniform4fEXT( GLuint program, GLint location,
										   GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3 ) {
	// REAL: program bind updates glDsaState.program cache. GL2 ref: tr_dsa.c:189
	GL_UseProgram( program );
	// PARITY-SEAM(Metal-device): qglUniform4f(location, v0, v1, v2, v3)
	// → MTLRenderCommandEncoder setFragmentBytes:length:atIndex: (vec4 uniform).
	(void)location; (void)v0; (void)v1; (void)v2; (void)v3;
}

GLvoid APIENTRY GLDSA_ProgramUniform1fvEXT( GLuint program, GLint location,
											GLsizei count, const GLfloat *value ) {
	// REAL: program bind updates glDsaState.program cache. GL2 ref: tr_dsa.c:196
	GL_UseProgram( program );
	// PARITY-SEAM(Metal-device): qglUniform1fv(location, count, value)
	// → MTLRenderCommandEncoder setFragmentBytes:length:atIndex: (float array uniform).
	(void)location; (void)count; (void)value;
}

GLvoid APIENTRY GLDSA_ProgramUniformMatrix4fvEXT( GLuint program, GLint location,
												  GLsizei count, GLboolean transpose,
												  const GLfloat *value ) {
	// REAL: program bind updates glDsaState.program cache. GL2 ref: tr_dsa.c:204
	GL_UseProgram( program );
	// PARITY-SEAM(Metal-device): qglUniformMatrix4fv(location, count, transpose, value)
	// → MTLRenderCommandEncoder setVertexBytes:length:atIndex: (mat4 uniform).
	(void)location; (void)count; (void)transpose; (void)value;
}

void GL_BindNullFramebuffers( void ) {
	// PARITY-SEAM(Metal-device): qglBindFramebuffer(GL_FRAMEBUFFER, 0)
	// → MTLRenderPassDescriptor with nil color/depth attachments (pass reset).
	glDsaState.drawFramebuffer = glDsaState.readFramebuffer = 0;
	// PARITY-SEAM(Metal-device): qglBindRenderbuffer(GL_RENDERBUFFER, 0)
	// → no equivalent; Metal does not use renderbuffer objects.
	glDsaState.renderbuffer = 0;
}

void GL_BindFramebuffer( GLenum target, GLuint framebuffer ) {
	switch ( target )
	{
	case GL_FRAMEBUFFER:
		// REAL: shadow-copy guard for both draw and read. GL2 ref: tr_dsa.c:221
		if ( framebuffer != glDsaState.drawFramebuffer || framebuffer != glDsaState.readFramebuffer ) {
			// PARITY-SEAM(Metal-device): qglBindFramebuffer(GL_FRAMEBUFFER, framebuffer)
			// → MTLRenderPassDescriptor colorAttachments[0].texture set for both read/draw.
			glDsaState.drawFramebuffer = glDsaState.readFramebuffer = framebuffer;
		}
		break;

	case GL_DRAW_FRAMEBUFFER:
		// REAL: shadow-copy guard for draw only. GL2 ref: tr_dsa.c:229
		if ( framebuffer != glDsaState.drawFramebuffer ) {
			// PARITY-SEAM(Metal-device): qglBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer)
			// → MTLRenderPassDescriptor colorAttachments[0].texture (draw target).
			glDsaState.drawFramebuffer = framebuffer;
		}
		break;

	case GL_READ_FRAMEBUFFER:
		// REAL: shadow-copy guard for read only. GL2 ref: tr_dsa.c:236
		if ( framebuffer != glDsaState.readFramebuffer ) {
			// PARITY-SEAM(Metal-device): qglBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer)
			// → MTLBlitCommandEncoder source texture (read source).
			glDsaState.readFramebuffer = framebuffer;
		}
		break;
	}
}

void GL_BindRenderbuffer( GLuint renderbuffer ) {
	// REAL: shadow-copy guard. GL2 ref: tr_dsa.c:248
	if ( renderbuffer != glDsaState.renderbuffer ) {
		// PARITY-SEAM(Metal-device): qglBindRenderbuffer(GL_RENDERBUFFER, renderbuffer)
		// → no direct equivalent; Metal uses MTLTexture for MSAA/depth renderbuffer roles.
		glDsaState.renderbuffer = renderbuffer;
	}
}

GLvoid APIENTRY GLDSA_NamedRenderbufferStorageEXT( GLuint renderbuffer,
												   GLenum internalformat, GLsizei width, GLsizei height ) {
	// REAL: bind path updates glDsaState.renderbuffer cache. GL2 ref: tr_dsa.c:258
	GL_BindRenderbuffer( renderbuffer );
	// PARITY-SEAM(Metal-device): qglRenderbufferStorage(GL_RENDERBUFFER, internalformat, width, height)
	// → MTLTextureDescriptor with pixelFormat/width/height for renderbuffer-role MTLTexture.
	(void)internalformat; (void)width; (void)height;
}

GLvoid APIENTRY GLDSA_NamedRenderbufferStorageMultisampleEXT( GLuint renderbuffer,
															  GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height ) {
	// REAL: bind path updates glDsaState.renderbuffer cache. GL2 ref: tr_dsa.c:265
	GL_BindRenderbuffer( renderbuffer );
	// PARITY-SEAM(Metal-device): qglRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
	// internalformat, width, height) → MTLTextureDescriptor with sampleCount/pixelFormat/size
	// for MSAA MTLTexture.
	(void)samples; (void)internalformat; (void)width; (void)height;
}

GLenum APIENTRY GLDSA_CheckNamedFramebufferStatusEXT( GLuint framebuffer, GLenum target ) {
	// REAL: bind path updates glDsaState.drawFramebuffer/readFramebuffer cache. GL2 ref: tr_dsa.c:271
	GL_BindFramebuffer( target, framebuffer );
	// PARITY-SEAM(Metal-device): qglCheckFramebufferStatus(target) → Metal render passes
	// are implicitly complete; no separate completeness query exists.  Return
	// GL_FRAMEBUFFER_COMPLETE so callers proceed without error.
	return GL_FRAMEBUFFER_COMPLETE;
}

GLvoid APIENTRY GLDSA_NamedFramebufferTexture2DEXT( GLuint framebuffer,
													GLenum attachment, GLenum textarget, GLuint texture, GLint level ) {
	// REAL: bind path updates glDsaState framebuffer cache. GL2 ref: tr_dsa.c:278
	GL_BindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	// PARITY-SEAM(Metal-device): qglFramebufferTexture2D(GL_FRAMEBUFFER, attachment,
	// textarget, texture, level) → MTLRenderPassDescriptor colorAttachments[slot].texture =
	// (MTLTexture)texture at mip level.
	(void)attachment; (void)textarget; (void)texture; (void)level;
}

GLvoid APIENTRY GLDSA_NamedFramebufferRenderbufferEXT( GLuint framebuffer,
													   GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer ) {
	// REAL: bind path updates glDsaState framebuffer cache. GL2 ref: tr_dsa.c:285
	GL_BindFramebuffer( GL_FRAMEBUFFER, framebuffer );
	// PARITY-SEAM(Metal-device): qglFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment,
	// renderbuffertarget, renderbuffer) → MTLRenderPassDescriptor depth/stencil/color
	// attachment texture set from MSAA MTLTexture.
	(void)attachment; (void)renderbuffertarget; (void)renderbuffer;
}

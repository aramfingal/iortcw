/*
===========================================================================
Copyright (C) 2007-2009 Robert Beckebans <trebor_7@users.sourceforge.net>

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
// tr_vbo.cpp — Metal port of code/renderergl2/tr_vbo.c
//
// FUNCTION COUNT: 23 (authoritative grep, verified 2026-06-04)
// REAL (CPU-only):
//   R_VaoPackTangent, R_VaoPackNormal, R_VaoPackColor,
//   R_VaoUnpackTangent, R_VaoUnpackNormal,
//   R_VaoList_f, VaoCache_CheckAdd, VaoCache_InitQueue,
//   VaoCache_AddSurface, VaoCache_BindVao
// REAL + PARITY-SEAM(Metal-device):
//   Vao_SetVertexPointers, R_CreateVao, R_CreateVao2, R_BindVao,
//   R_BindNullVao, R_InitVaos, R_ShutdownVaos, RB_UpdateTessVao,
//   VaoCache_Commit, VaoCache_DrawElements, VaoCache_Init,
//   VaoCache_RecycleVertexBuffer, VaoCache_RecycleIndexBuffer
//
// Attribute packing (snorm/unorm math) and VAO layout bookkeeping are REAL.
// Actual GPU buffer operations (qglGenBuffers, qglBindBuffer, qglBufferData,
// qglBufferSubData, qglVertexAttribPointer, qglGenVertexArrays,
// qglBindVertexArray, qglDeleteBuffers, qglDeleteVertexArrays,
// qglEnableVertexAttribArray, qglDisableVertexAttribArray, qglDrawElements)
// become PARITY-SEAM(Metal-device) no-ops.  Under Metal these will be
// replaced with MTLBuffer create/bind/draw on the device (Phase 3/4).
//
// NOTE: R_DrawElements lives in tr_shade.c (owner: Lambert); its scaffold
// stub remains in tr_scaffold_stubs.cpp until tr_shade.cpp is ported.

extern "C" {
#include "tr_local.h"
void GLMetal_DestroyStaticBuffer( void *buffer );
}

// ---------------------------------------------------------------------------
// GL constants needed for attribute type/usage bookkeeping.
// The numeric values are the standard OpenGL enumerants; they are stored in
// vaoAttrib_t.type and used by Vao_SetVertexPointers / RB_UpdateTessVao to
// identify data widths.  Under Metal the actual vertex-format selection
// happens in the MTLVertexDescriptor seam (Phase 3/4).
// GL2 ref: GL/gl.h (standard)
// ---------------------------------------------------------------------------
#ifndef GL_FALSE
#define GL_FALSE            0
#endif
#ifndef GL_TRUE
#define GL_TRUE             1
#endif
#ifndef GL_FLOAT
#define GL_FLOAT            0x1406
#endif
#ifndef GL_SHORT
#define GL_SHORT            0x1402
#endif
// GL_UNSIGNED_SHORT (0x1403) already defined in tr_local.h
#ifndef GL_TRIANGLES
#define GL_TRIANGLES        0x0004
#endif
// Buffer usage hints (stored in glUsage locals; seamed on buffer-create path)
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW      0x88B4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW     0x88E8
#endif
// Buffer binding targets (used in seamed qglBind/BufferData calls)
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER         0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif

// ---------------------------------------------------------------------------
// VaoCache private types and limits — exact copy from tr_vbo.c:656-713
// (COPY-THEN-ANNOTATE: sizes and struct layout are parity-critical)
// ---------------------------------------------------------------------------

// FIXME: This sets a limit of 65536 verts/262144 indexes per static surface
// This is higher than the old vq3 limits but is worth noting
#define VAOCACHE_QUEUE_MAX_SURFACES ( 1 << 10 )
#define VAOCACHE_QUEUE_MAX_VERTEXES ( 1 << 16 )
#define VAOCACHE_QUEUE_MAX_INDEXES  ( VAOCACHE_QUEUE_MAX_VERTEXES * 4 )

typedef struct queuedSurface_s
{
	srfVert_t *vertexes;
	int numVerts;
	glIndex_t *indexes;
	int numIndexes;
}
queuedSurface_t;

static struct
{
	queuedSurface_t surfaces[VAOCACHE_QUEUE_MAX_SURFACES];
	int numSurfaces;

	srfVert_t vertexes[VAOCACHE_QUEUE_MAX_VERTEXES];
	int vertexCommitSize;

	vaoCacheGlIndex_t indexes[VAOCACHE_QUEUE_MAX_INDEXES];
	int indexCommitSize;
}
vcq;

#define VAOCACHE_MAX_SURFACES ( 1 << 16 )
#define VAOCACHE_MAX_BATCHES  ( 1 << 10 )

// srfVert_t is 60 bytes
// assuming each vert is referenced 4 times, need 16 bytes (4 glIndex_t) per vert
// -> need about 4/15ths the space for indexes as vertexes
#define VAOCACHE_VERTEX_BUFFER_SIZE ( 16 * 1024 * 1024 )
#define VAOCACHE_INDEX_BUFFER_SIZE  ( 5 * 1024 * 1024 )

typedef struct buffered_s
{
	glIndex_t *indexes;
	int numIndexes;
	int bufferOffset;
}
buffered_t;

static struct
{
	vao_t *vao;
	buffered_t surfaceIndexSets[VAOCACHE_MAX_SURFACES];
	int numSurfaces;

	int batchLengths[VAOCACHE_MAX_BATCHES];
	int numBatches;

	int vertexOffset;
	int indexOffset;
}
vc;

// ===========================================================================
// Attribute packing / unpacking — REAL CPU math, no GPU involvement
// ===========================================================================

void R_VaoPackTangent( int16_t *out, vec4_t v ) {
	out[0] = (int16_t)( v[0] * 32767.0f + ( v[0] > 0.0f ? 0.5f : -0.5f ) );
	out[1] = (int16_t)( v[1] * 32767.0f + ( v[1] > 0.0f ? 0.5f : -0.5f ) );
	out[2] = (int16_t)( v[2] * 32767.0f + ( v[2] > 0.0f ? 0.5f : -0.5f ) );
	out[3] = (int16_t)( v[3] * 32767.0f + ( v[3] > 0.0f ? 0.5f : -0.5f ) );
}

void R_VaoPackNormal( int16_t *out, vec3_t v ) {
	out[0] = (int16_t)( v[0] * 32767.0f + ( v[0] > 0.0f ? 0.5f : -0.5f ) );
	out[1] = (int16_t)( v[1] * 32767.0f + ( v[1] > 0.0f ? 0.5f : -0.5f ) );
	out[2] = (int16_t)( v[2] * 32767.0f + ( v[2] > 0.0f ? 0.5f : -0.5f ) );
	out[3] = 0;
}

void R_VaoPackColor( uint16_t *out, vec4_t c ) {
	out[0] = (uint16_t)( c[0] * 65535.0f + 0.5f );
	out[1] = (uint16_t)( c[1] * 65535.0f + 0.5f );
	out[2] = (uint16_t)( c[2] * 65535.0f + 0.5f );
	out[3] = (uint16_t)( c[3] * 65535.0f + 0.5f );
}

void R_VaoUnpackTangent( vec4_t v, int16_t *pack ) {
	v[0] = pack[0] / 32767.0f;
	v[1] = pack[1] / 32767.0f;
	v[2] = pack[2] / 32767.0f;
	v[3] = pack[3] / 32767.0f;
}

void R_VaoUnpackNormal( vec3_t v, int16_t *pack ) {
	v[0] = pack[0] / 32767.0f;
	v[1] = pack[1] / 32767.0f;
	v[2] = pack[2] / 32767.0f;
}

// ===========================================================================
// Vao_SetVertexPointers — vertex attribute pointer setup
// Attribute loop is REAL; qglVertexAttribPointer / Enable / Disable are
// PARITY-SEAM(Metal-device): vertex layout goes into MTLVertexDescriptor.
// ===========================================================================
void Vao_SetVertexPointers( vao_t *vao ) {
	int attribIndex;

	for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++)
	{
		uint32_t attribBit = 1u << attribIndex;
		vaoAttrib_t *vAtb = &vao->attribs[attribIndex];

		if ( vAtb->enabled ) {
			// PARITY-SEAM(Metal-device): qglVertexAttribPointer → MTLVertexDescriptor
			// qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type,
			//     vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset));
			if ( glRefConfig.vertexArrayObject || !( glState.vertexAttribsEnabled & attribBit ) ) {
				// PARITY-SEAM(Metal-device): qglEnableVertexAttribArray(attribIndex)
				(void)attribBit;
			}

			if ( !glRefConfig.vertexArrayObject || vao == tess.vao ) {
				glState.vertexAttribsEnabled |= attribBit;
			}
		} else
		{
			// don't disable vertex attribs when using vertex array objects
			// Vao_SetVertexPointers is only called during init when using VAOs
			if ( !glRefConfig.vertexArrayObject && ( glState.vertexAttribsEnabled & attribBit ) ) {
				// PARITY-SEAM(Metal-device): qglDisableVertexAttribArray(attribIndex)
				(void)attribBit;
			}

			if ( !glRefConfig.vertexArrayObject || vao == tess.vao ) {
				glState.vertexAttribsEnabled &= ~attribBit;
			}
		}
	}
}

// ===========================================================================
// R_CreateVao — allocate and register a vao_t; GPU buffer create is seamed
// Bookkeeping (name, numVaos, vertexesSize, indexesSize) is REAL.
// ===========================================================================
vao_t *R_CreateVao( const char *name, byte *vertexes, int vertexesSize, byte *indexes, int indexesSize, vaoUsage_t usage ) {
	vao_t *vao;
	int glUsage;

	switch ( usage )
	{
	case VAO_USAGE_STATIC:
		glUsage = GL_STATIC_DRAW;
		break;

	case VAO_USAGE_DYNAMIC:
		glUsage = GL_DYNAMIC_DRAW;
		break;

	default:
		ri.Error( ERR_FATAL, "bad vaoUsage_t given: %i", usage );
		return NULL;
	}

	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Error( ERR_DROP, "R_CreateVao: \"%s\" is too long", name );
	}

	if ( tr.numVaos == MAX_VAOS ) {
		ri.Error( ERR_DROP, "R_CreateVao: MAX_VAOS hit" );
	}

	R_IssuePendingRenderCommands();

	vao = tr.vaos[tr.numVaos] = (vao_t *)ri.Hunk_Alloc( sizeof(*vao ), h_low );
	tr.numVaos++;

	memset( vao, 0, sizeof(*vao ) );

	Q_strncpyz( vao->name, name, sizeof( vao->name ) );

	if ( glRefConfig.vertexArrayObject ) {
		// PARITY-SEAM(Metal-device): qglGenVertexArrays(1, &vao->vao)
		// PARITY-SEAM(Metal-device): qglBindVertexArray(vao->vao)
		(void)vao->vao;
	}

	vao->vertexesSize = vertexesSize;

	// PARITY-SEAM(Metal-device): qglGenBuffers(1, &vao->vertexesVBO)
	// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO)
	// PARITY-SEAM(Metal-device): qglBufferData(GL_ARRAY_BUFFER, vertexesSize, vertexes, glUsage)
	(void)vertexes;
	(void)glUsage;

	vao->indexesSize = indexesSize;

	// PARITY-SEAM(Metal-device): qglGenBuffers(1, &vao->indexesIBO)
	// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO)
	// PARITY-SEAM(Metal-device): qglBufferData(GL_ELEMENT_ARRAY_BUFFER, indexesSize, indexes, glUsage)
	(void)indexes;

	glState.currentVao = vao;

	GL_CheckErrors();

	return vao;
}

// ===========================================================================
// R_CreateVao2 — interleaved srfVert_t layout; packs verts CPU-side; seams GPU upload
// Attribute layout tables are REAL (COPY-THEN-ANNOTATE from tr_vbo.c:209-255).
// Data packing loop is REAL.  GPU buffer create/upload is seamed.
// ===========================================================================
vao_t *R_CreateVao2( const char *name, int numVertexes, srfVert_t *verts, int numIndexes, glIndex_t *indexes ) {
	vao_t *vao;
	int i;

	byte  *data;
	int dataSize;
	int dataOfs;

	int glUsage = GL_STATIC_DRAW;

	if ( !numVertexes || !numIndexes ) {
		return NULL;
	}

	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Error( ERR_DROP, "R_CreateVao2: \"%s\" is too long", name );
	}

	if ( tr.numVaos == MAX_VAOS ) {
		ri.Error( ERR_DROP, "R_CreateVao2: MAX_VAOS hit" );
	}

	R_IssuePendingRenderCommands();

	vao = tr.vaos[tr.numVaos] = (vao_t *)ri.Hunk_Alloc( sizeof(*vao ), h_low );
	tr.numVaos++;

	memset( vao, 0, sizeof(*vao ) );

	Q_strncpyz( vao->name, name, sizeof( vao->name ) );

	// since these vertex attributes are never altered, interleave them
	// COPY-THEN-ANNOTATE: attribute layout table — exact copy from tr_vbo.c:209-255
	vao->attribs[ATTR_INDEX_POSITION      ].enabled = 1;
	vao->attribs[ATTR_INDEX_NORMAL        ].enabled = 1;
	vao->attribs[ATTR_INDEX_TANGENT       ].enabled = 1;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].enabled = 1;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].enabled = 1;
	vao->attribs[ATTR_INDEX_COLOR         ].enabled = 1;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;

	vao->attribs[ATTR_INDEX_POSITION      ].count = 3;
	vao->attribs[ATTR_INDEX_NORMAL        ].count = 4;
	vao->attribs[ATTR_INDEX_TANGENT       ].count = 4;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].count = 2;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].count = 2;
	vao->attribs[ATTR_INDEX_COLOR         ].count = 4;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;

	vao->attribs[ATTR_INDEX_POSITION      ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_NORMAL        ].type = GL_SHORT;
	vao->attribs[ATTR_INDEX_TANGENT       ].type = GL_SHORT;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_COLOR         ].type = GL_UNSIGNED_SHORT;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type = GL_SHORT;

	vao->attribs[ATTR_INDEX_POSITION      ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_NORMAL        ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_TANGENT       ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_COLOR         ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;

	// offsets and strides — interleaved layout, stride == total struct size
	vao->attribs[ATTR_INDEX_POSITION      ].offset = 0;        dataSize  = (int)sizeof( verts[0].xyz );
	vao->attribs[ATTR_INDEX_NORMAL        ].offset = dataSize; dataSize += (int)sizeof( verts[0].normal );
	vao->attribs[ATTR_INDEX_TANGENT       ].offset = dataSize; dataSize += (int)sizeof( verts[0].tangent );
	vao->attribs[ATTR_INDEX_TEXCOORD      ].offset = dataSize; dataSize += (int)sizeof( verts[0].st );
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].offset = dataSize; dataSize += (int)sizeof( verts[0].lightmap );
	vao->attribs[ATTR_INDEX_COLOR         ].offset = dataSize; dataSize += (int)sizeof( verts[0].color );
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = dataSize; dataSize += (int)sizeof( verts[0].lightdir );

	vao->attribs[ATTR_INDEX_POSITION      ].stride = dataSize;
	vao->attribs[ATTR_INDEX_NORMAL        ].stride = dataSize;
	vao->attribs[ATTR_INDEX_TANGENT       ].stride = dataSize;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].stride = dataSize;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].stride = dataSize;
	vao->attribs[ATTR_INDEX_COLOR         ].stride = dataSize;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = dataSize;

	if ( glRefConfig.vertexArrayObject ) {
		// PARITY-SEAM(Metal-device): qglGenVertexArrays(1, &vao->vao)
		// PARITY-SEAM(Metal-device): qglBindVertexArray(vao->vao)
		(void)vao->vao;
	}

	// create VBO — pack interleaved vertex data CPU-side (REAL)
	dataSize *= numVertexes;
	data = (byte *)ri.Hunk_AllocateTempMemory( dataSize );
	dataOfs = 0;

	for (i = 0; i < numVertexes; i++)
	{
		// xyz
		memcpy( data + dataOfs, &verts[i].xyz,      sizeof( verts[i].xyz ) );
		dataOfs += (int)sizeof( verts[i].xyz );

		// normal
		memcpy( data + dataOfs, &verts[i].normal,   sizeof( verts[i].normal ) );
		dataOfs += (int)sizeof( verts[i].normal );

		// tangent
		memcpy( data + dataOfs, &verts[i].tangent,  sizeof( verts[i].tangent ) );
		dataOfs += (int)sizeof( verts[i].tangent );

		// texcoords
		memcpy( data + dataOfs, &verts[i].st,       sizeof( verts[i].st ) );
		dataOfs += (int)sizeof( verts[i].st );

		// lightmap texcoords
		memcpy( data + dataOfs, &verts[i].lightmap, sizeof( verts[i].lightmap ) );
		dataOfs += (int)sizeof( verts[i].lightmap );

		// colors
		memcpy( data + dataOfs, &verts[i].color,    sizeof( verts[i].color ) );
		dataOfs += (int)sizeof( verts[i].color );

		// light directions
		memcpy( data + dataOfs, &verts[i].lightdir, sizeof( verts[i].lightdir ) );
		dataOfs += (int)sizeof( verts[i].lightdir );
	}

	vao->vertexesSize = dataSize;

	// PARITY-SEAM(Metal-device): qglGenBuffers(1, &vao->vertexesVBO)
	// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO)
	// PARITY-SEAM(Metal-device): qglBufferData(GL_ARRAY_BUFFER, vao->vertexesSize, data, glUsage)
	(void)glUsage;

	// create IBO
	vao->indexesSize = numIndexes * (int)sizeof( glIndex_t );

	// PARITY-SEAM(Metal-device): qglGenBuffers(1, &vao->indexesIBO)
	// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO)
	// PARITY-SEAM(Metal-device): qglBufferData(GL_ELEMENT_ARRAY_BUFFER, vao->indexesSize, indexes, glUsage)
	(void)indexes;

	Vao_SetVertexPointers( vao );

	glState.currentVao = vao;

	GL_CheckErrors();

	ri.Hunk_FreeTempMemory( data );

	return vao;
}

// ===========================================================================
// R_BindVao — make vao the current VAO; track state + perf counter
// State tracking (glState.currentVao, vertexAttribsInterpolation, pc) is REAL.
// qglBindVertexArray / qglBindBuffer are PARITY-SEAM(Metal-device).
// ===========================================================================
void R_BindVao( vao_t *vao ) {
	if ( !vao ) {
		ri.Error( ERR_DROP, "R_BindVao: NULL vao" );
		return;
	}

	// PARITY-SEAM(Metal-device): GLimp_LogComment → no-op under Metal
	// (sdl_glimp.c not linked into renderer_metal)
	// GL2 ref: tr_vbo.c:348 — GLimp_LogComment(va("--- R_BindVao( %s ) ---\n", vao->name))

	if ( glState.currentVao != vao ) {
		glState.currentVao = vao;

		glState.vertexAttribsInterpolation = 0;
		glState.vertexAnimation = qfalse;
		backEnd.pc.c_vaoBinds++;

		if ( glRefConfig.vertexArrayObject ) {
			// PARITY-SEAM(Metal-device): qglBindVertexArray(vao->vao)
			(void)vao->vao;

			// Intel Graphics doesn't save GL_ELEMENT_ARRAY_BUFFER binding with VAO binding.
			if ( glRefConfig.intelGraphics || vao == tess.vao ) {
				// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO)
				(void)vao->indexesIBO;
			}

			// tess VAO always has buffers bound
			if ( vao == tess.vao ) {
				// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO)
				(void)vao->vertexesVBO;
			}
		} else
		{
			// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO)
			(void)vao->vertexesVBO;
			// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO)
			(void)vao->indexesIBO;

			// tess VAO doesn't have vertex pointers set until data is uploaded
			if ( vao != tess.vao ) {
				Vao_SetVertexPointers( vao );
			}
		}
	}
}

// ===========================================================================
// R_BindNullVao — unbind current VAO; clears glState.currentVao
// State tracking is REAL.  qglBindVertexArray(0) / qglBindBuffer(0) are seamed.
// ===========================================================================
void R_BindNullVao( void ) {
	// PARITY-SEAM(Metal-device): GLimp_LogComment("--- R_BindNullVao ---\n")

	if ( glState.currentVao ) {
		if ( glRefConfig.vertexArrayObject ) {
			// PARITY-SEAM(Metal-device): qglBindVertexArray(0)
			// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)
			(void)0;
		} else
		{
			// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ARRAY_BUFFER, 0)
			// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)
			(void)0;
		}
		glState.currentVao = NULL;
	}

	GL_CheckErrors();
}

// ===========================================================================
// R_InitVaos — create tess VAO, set attribute layout, init VaoCache
// Attribute layout tables are REAL (COPY-THEN-ANNOTATE from tr_vbo.c:442-496).
// GPU VAO/VBO creation is seamed through R_CreateVao.
// ===========================================================================
void R_InitVaos( void ) {
	int vertexesSize, indexesSize;
	int offset;

	ri.Printf( PRINT_ALL, "------- R_InitVaos -------\n" );

	tr.numVaos = 0;

	vertexesSize  = (int)sizeof( tess.xyz[0] );
	vertexesSize += (int)sizeof( tess.normal[0] );
	vertexesSize += (int)sizeof( tess.tangent[0] );
	vertexesSize += (int)sizeof( tess.color[0] );
	vertexesSize += (int)sizeof( tess.texCoords[0] );
	vertexesSize += (int)sizeof( tess.lightCoords[0] );
	vertexesSize += (int)sizeof( tess.lightdir[0] );
	vertexesSize *= SHADER_MAX_VERTEXES;

	indexesSize = (int)sizeof( tess.indexes[0] ) * SHADER_MAX_INDEXES;

	tess.vao = R_CreateVao( "tessVertexArray_VAO", NULL, vertexesSize, NULL, indexesSize, VAO_USAGE_DYNAMIC );

	offset = 0;

	// COPY-THEN-ANNOTATE: tess VAO attribute layout — exact copy from tr_vbo.c:442-496
	tess.vao->attribs[ATTR_INDEX_POSITION      ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].count = 3;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].count = 4;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].count = 4;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].count = 2;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].count = 2;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].count = 4;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].type = GL_SHORT;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].type = GL_SHORT;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].type = GL_UNSIGNED_SHORT;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type = GL_SHORT;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;

	// non-interleaved layout: each attribute array is stride == sizeof(element)
	tess.vao->attribs[ATTR_INDEX_POSITION      ].offset = offset; offset += (int)sizeof( tess.xyz[0] )         * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].offset = offset; offset += (int)sizeof( tess.normal[0] )      * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].offset = offset; offset += (int)sizeof( tess.tangent[0] )     * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].offset = offset; offset += (int)sizeof( tess.texCoords[0] )   * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].offset = offset; offset += (int)sizeof( tess.lightCoords[0] ) * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].offset = offset; offset += (int)sizeof( tess.color[0] )       * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = offset;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].stride = (int)sizeof( tess.xyz[0] );
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].stride = (int)sizeof( tess.normal[0] );
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].stride = (int)sizeof( tess.tangent[0] );
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].stride = (int)sizeof( tess.texCoords[0] );
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].stride = (int)sizeof( tess.lightCoords[0] );
	tess.vao->attribs[ATTR_INDEX_COLOR         ].stride = (int)sizeof( tess.color[0] );
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = (int)sizeof( tess.lightdir[0] );

	tess.attribPointers[ATTR_INDEX_POSITION]       = tess.xyz;
	tess.attribPointers[ATTR_INDEX_NORMAL]         = tess.normal;
	tess.attribPointers[ATTR_INDEX_TANGENT]        = tess.tangent;
	tess.attribPointers[ATTR_INDEX_TEXCOORD]       = tess.texCoords;
	tess.attribPointers[ATTR_INDEX_LIGHTCOORD]     = tess.lightCoords;
	tess.attribPointers[ATTR_INDEX_COLOR]          = tess.color;
	tess.attribPointers[ATTR_INDEX_LIGHTDIRECTION] = tess.lightdir;

	Vao_SetVertexPointers( tess.vao );

	R_BindNullVao();

	VaoCache_Init();

	GL_CheckErrors();
}

// ===========================================================================
// R_ShutdownVaos — delete all VAOs and GPU buffers; reset numVaos
// Array iteration is REAL.  qglDeleteVertexArrays / qglDeleteBuffers are seamed.
// ===========================================================================
void R_ShutdownVaos( void ) {
	int i;
	vao_t *vao;

	ri.Printf( PRINT_ALL, "------- R_ShutdownVaos -------\n" );

	R_BindNullVao();

	for (i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		if ( vao->vao ) {
			// PARITY-SEAM(Metal-device): qglDeleteVertexArrays(1, &vao->vao)
			(void)vao->vao;
		}

		if ( vao->vertexesVBO ) {
			// PARITY-SEAM(Metal-device): qglDeleteBuffers(1, &vao->vertexesVBO)
			(void)vao->vertexesVBO;
		}

		if ( vao->indexesIBO ) {
			// PARITY-SEAM(Metal-device): qglDeleteBuffers(1, &vao->indexesIBO)
			(void)vao->indexesIBO;
		}

		if ( vao->metalBuffer ) {
			GLMetal_DestroyStaticBuffer( vao->metalBuffer );
			vao->metalBuffer = NULL;
		}
	}

	tr.numVaos = 0;
}

// ===========================================================================
// R_VaoList_f — console command: print VAO memory usage
// Pure ri.Printf reporting — REAL, no GPU involvement.
// ===========================================================================
void R_VaoList_f( void ) {
	int i;
	vao_t *vao;
	int vertexesSize = 0;
	int indexesSize  = 0;

	ri.Printf( PRINT_ALL, " size          name\n" );
	ri.Printf( PRINT_ALL, "----------------------------------------------------------\n" );

	for (i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		ri.Printf( PRINT_ALL, "%d.%02d MB %s\n", vao->vertexesSize / ( 1024 * 1024 ),
				   ( vao->vertexesSize % ( 1024 * 1024 ) ) * 100 / ( 1024 * 1024 ), vao->name );

		vertexesSize += vao->vertexesSize;
	}

	for (i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		ri.Printf( PRINT_ALL, "%d.%02d MB %s\n", vao->indexesSize / ( 1024 * 1024 ),
				   ( vao->indexesSize % ( 1024 * 1024 ) ) * 100 / ( 1024 * 1024 ), vao->name );

		indexesSize += vao->indexesSize;
	}

	ri.Printf( PRINT_ALL, " %i total VAOs\n", tr.numVaos );
	ri.Printf( PRINT_ALL, " %d.%02d MB total vertices memory\n", vertexesSize / ( 1024 * 1024 ),
			   ( vertexesSize % ( 1024 * 1024 ) ) * 100 / ( 1024 * 1024 ) );
	ri.Printf( PRINT_ALL, " %d.%02d MB total triangle indices memory\n", indexesSize / ( 1024 * 1024 ),
			   ( indexesSize % ( 1024 * 1024 ) ) * 100 / ( 1024 * 1024 ) );
}

// ===========================================================================
// RB_UpdateTessVao — upload CPU tess arrays into bound GPU VAO/VBO
// Adapted from Tess_UpdateVBOs from xreal.
// Attrib-enable tracking loop is REAL.
// qglBufferData / qglBufferSubData / qglVertexAttribPointer / Enable / Disable
// are PARITY-SEAM(Metal-device).
// ===========================================================================
void RB_UpdateTessVao( unsigned int attribBits ) {
	// PARITY-SEAM(Metal-device): GLimp_LogComment("--- RB_UpdateTessVao ---\n")

	backEnd.pc.c_dynamicVaoDraws++;

	// update the default VAO
	if ( tess.numVertexes > 0 && tess.numVertexes <= SHADER_MAX_VERTEXES
		 && tess.numIndexes > 0 && tess.numIndexes <= SHADER_MAX_INDEXES ) {
		int attribIndex;
		int attribUpload;

		R_BindVao( tess.vao );

		// orphan old vertex buffer so we don't stall on it
		// PARITY-SEAM(Metal-device): qglBufferData(GL_ARRAY_BUFFER, tess.vao->vertexesSize, NULL, GL_DYNAMIC_DRAW)
		(void)0;

		// if nothing to set, set everything
		if ( !( attribBits & ATTR_BITS ) ) {
			attribBits = ATTR_BITS;
		}

		attribUpload = attribBits;

		for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++)
		{
			uint32_t attribBit = 1u << attribIndex;
			vaoAttrib_t *vAtb = &tess.vao->attribs[attribIndex];

			if ( attribUpload & attribBit ) {
				// note: tess has a VBO where stride == size
				// PARITY-SEAM(Metal-device): qglBufferSubData(GL_ARRAY_BUFFER,
				//     vAtb->offset, tess.numVertexes * vAtb->stride,
				//     tess.attribPointers[attribIndex])
				(void)vAtb;
			}

			if ( attribBits & attribBit ) {
				if ( !glRefConfig.vertexArrayObject ) {
					// PARITY-SEAM(Metal-device): qglVertexAttribPointer(attribIndex,
					//     vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride,
					//     BUFFER_OFFSET(vAtb->offset))
					(void)0;
				}

				if ( !( glState.vertexAttribsEnabled & attribBit ) ) {
					// PARITY-SEAM(Metal-device): qglEnableVertexAttribArray(attribIndex)
					glState.vertexAttribsEnabled |= attribBit;
				}
			} else
			{
				if ( glState.vertexAttribsEnabled & attribBit ) {
					// PARITY-SEAM(Metal-device): qglDisableVertexAttribArray(attribIndex)
					glState.vertexAttribsEnabled &= ~attribBit;
				}
			}
		}

		// orphan old index buffer so we don't stall on it
		// PARITY-SEAM(Metal-device): qglBufferData(GL_ELEMENT_ARRAY_BUFFER, tess.vao->indexesSize, NULL, GL_DYNAMIC_DRAW)
		// PARITY-SEAM(Metal-device): qglBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0,
		//     tess.numIndexes * sizeof(tess.indexes[0]), tess.indexes)
		(void)0;
	}
}

// ===========================================================================
// VaoCache_Commit — flush queued surfaces: look up or build a new GPU batch
// Batch-search loop and index remapping are REAL.
// qglBufferSubData (vertex and index upload) is PARITY-SEAM(Metal-device).
// ===========================================================================
void VaoCache_Commit( void ) {
	buffered_t      *indexSet;
	int             *batchLength;
	queuedSurface_t *surf, *end = vcq.surfaces + vcq.numSurfaces;

	R_BindVao( vc.vao );

	// Search for a matching batch
	// FIXME: Use faster search
	indexSet    = vc.surfaceIndexSets;
	batchLength = vc.batchLengths;
	for (; batchLength < vc.batchLengths + vc.numBatches; batchLength++)
	{
		if (*batchLength == vcq.numSurfaces ) {
			buffered_t *indexSet2 = indexSet;
			for (surf = vcq.surfaces; surf < end; surf++, indexSet2++)
			{
				if ( surf->indexes != indexSet2->indexes || surf->numIndexes != indexSet2->numIndexes ) {
					break;
				}
			}

			if ( surf == end ) {
				break;
			}
		}

		indexSet += *batchLength;
	}

	// If found, use it
	if ( indexSet < vc.surfaceIndexSets + vc.numSurfaces ) {
		tess.firstIndex = indexSet->bufferOffset / (int)glRefConfig.vaoCacheGlIndexSize;
	}
	// If not, rebuffer the batch
	// FIXME: keep track of the vertexes so we don't have to reupload every time
	else
	{
		srfVert_t         *dstVertex    = vcq.vertexes;
		vaoCacheGlIndex_t *dstIndex     = vcq.indexes;
		unsigned short    *dstIndexUshort = (unsigned short *)vcq.indexes;

		batchLength  = vc.batchLengths + vc.numBatches;
		*batchLength = vcq.numSurfaces;
		vc.numBatches++;

		tess.firstIndex      = vc.indexOffset / (int)glRefConfig.vaoCacheGlIndexSize;
		vcq.vertexCommitSize = 0;
		vcq.indexCommitSize  = 0;

		for (surf = vcq.surfaces; surf < end; surf++)
		{
			glIndex_t *srcIndex   = surf->indexes;
			int vertexesSize      = surf->numVerts * (int)sizeof( srfVert_t );
			int indexesSize       = surf->numIndexes * (int)glRefConfig.vaoCacheGlIndexSize;
			int i;
			int indexOffset       = ( vc.vertexOffset + vcq.vertexCommitSize ) / (int)sizeof( srfVert_t );

			Com_Memcpy( dstVertex, surf->vertexes, vertexesSize );
			dstVertex += surf->numVerts;

			vcq.vertexCommitSize += vertexesSize;

			indexSet            = vc.surfaceIndexSets + vc.numSurfaces;
			indexSet->indexes   = surf->indexes;
			indexSet->numIndexes = surf->numIndexes;
			indexSet->bufferOffset = vc.indexOffset + vcq.indexCommitSize;
			vc.numSurfaces++;

			if ( glRefConfig.vaoCacheGlIndexType == GL_UNSIGNED_SHORT ) {
				for (i = 0; i < surf->numIndexes; i++)
					*dstIndexUshort++ = (unsigned short)(*srcIndex++ + indexOffset );
			} else
			{
				for (i = 0; i < surf->numIndexes; i++)
					*dstIndex++ = (vaoCacheGlIndex_t)(*srcIndex++ + indexOffset );
			}

			vcq.indexCommitSize += indexesSize;
		}

		if ( vcq.vertexCommitSize ) {
			// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ARRAY_BUFFER, vc.vao->vertexesVBO)
			// PARITY-SEAM(Metal-device): qglBufferSubData(GL_ARRAY_BUFFER,
			//     vc.vertexOffset, vcq.vertexCommitSize, vcq.vertexes)
			(void)0;
			vc.vertexOffset += vcq.vertexCommitSize;
		}

		if ( vcq.indexCommitSize ) {
			// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesIBO)
			// PARITY-SEAM(Metal-device): qglBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
			//     vc.indexOffset, vcq.indexCommitSize, vcq.indexes)
			(void)0;
			vc.indexOffset += vcq.indexCommitSize;
		}
	}
}

// ===========================================================================
// VaoCache_DrawElements — issue indexed draw for the VaoCache batch
// assert (current VAO == vc.vao) is REAL.
// qglDrawElements is PARITY-SEAM(Metal-device): MTLRenderCommandEncoder
// drawIndexedPrimitives in Phase 3/4.
// ===========================================================================
void VaoCache_DrawElements( int numIndexes, int firstIndex ) {
	assert( glState.currentVao == vc.vao );

	// PARITY-SEAM(Metal-device): qglDrawElements(GL_TRIANGLES, numIndexes,
	//     glRefConfig.vaoCacheGlIndexType,
	//     BUFFER_OFFSET(firstIndex * glRefConfig.vaoCacheGlIndexSize))
	(void)numIndexes;
	(void)firstIndex;
}

// ===========================================================================
// VaoCache_Init — allocate VaoCache VAO; set attribute layout; zero counters
// Attribute layout tables are REAL (COPY-THEN-ANNOTATE from tr_vbo.c:842-898).
// GPU allocation seamed through R_CreateVao / Vao_SetVertexPointers.
// ===========================================================================
void VaoCache_Init( void ) {
	int vertexBufferSize;
	int indexBufferSize;

	if ( glRefConfig.vaoCacheGlIndexType == GL_UNSIGNED_SHORT ) {
		vertexBufferSize = (int)sizeof( srfVert_t ) * USHRT_MAX;
		indexBufferSize  = (int)sizeof( unsigned short ) * USHRT_MAX * 4;
	} else
	{
		vertexBufferSize = VAOCACHE_VERTEX_BUFFER_SIZE;
		indexBufferSize  = VAOCACHE_INDEX_BUFFER_SIZE;
	}

	vc.vao = R_CreateVao( "VaoCache", NULL, vertexBufferSize, NULL, indexBufferSize, VAO_USAGE_DYNAMIC );

	// COPY-THEN-ANNOTATE: VaoCache VAO attribute layout — exact copy from tr_vbo.c:842-898
	vc.vao->attribs[ATTR_INDEX_POSITION].enabled       = 1;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].enabled       = 1;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].enabled     = 1;
	vc.vao->attribs[ATTR_INDEX_NORMAL].enabled         = 1;
	vc.vao->attribs[ATTR_INDEX_TANGENT].enabled        = 1;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;
	vc.vao->attribs[ATTR_INDEX_COLOR].enabled          = 1;

	vc.vao->attribs[ATTR_INDEX_POSITION].count       = 3;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].count       = 2;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].count     = 2;
	vc.vao->attribs[ATTR_INDEX_NORMAL].count         = 4;
	vc.vao->attribs[ATTR_INDEX_TANGENT].count        = 4;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;
	vc.vao->attribs[ATTR_INDEX_COLOR].count          = 4;

	vc.vao->attribs[ATTR_INDEX_POSITION].type             = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].type             = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].type           = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_NORMAL].type               = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_TANGENT].type              = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type       = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_COLOR].type                = GL_UNSIGNED_SHORT;

	vc.vao->attribs[ATTR_INDEX_POSITION].normalized       = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].normalized       = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].normalized     = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_NORMAL].normalized         = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_TANGENT].normalized        = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_COLOR].normalized          = GL_TRUE;

	vc.vao->attribs[ATTR_INDEX_POSITION].offset       = (int)offsetof( srfVert_t, xyz );
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].offset       = (int)offsetof( srfVert_t, st );
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].offset     = (int)offsetof( srfVert_t, lightmap );
	vc.vao->attribs[ATTR_INDEX_NORMAL].offset         = (int)offsetof( srfVert_t, normal );
	vc.vao->attribs[ATTR_INDEX_TANGENT].offset        = (int)offsetof( srfVert_t, tangent );
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = (int)offsetof( srfVert_t, lightdir );
	vc.vao->attribs[ATTR_INDEX_COLOR].offset          = (int)offsetof( srfVert_t, color );

	vc.vao->attribs[ATTR_INDEX_POSITION].stride       = (int)sizeof( srfVert_t );
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].stride       = (int)sizeof( srfVert_t );
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].stride     = (int)sizeof( srfVert_t );
	vc.vao->attribs[ATTR_INDEX_NORMAL].stride         = (int)sizeof( srfVert_t );
	vc.vao->attribs[ATTR_INDEX_TANGENT].stride        = (int)sizeof( srfVert_t );
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = (int)sizeof( srfVert_t );
	vc.vao->attribs[ATTR_INDEX_COLOR].stride          = (int)sizeof( srfVert_t );

	Vao_SetVertexPointers( vc.vao );

	vc.numSurfaces   = 0;
	vc.numBatches    = 0;
	vc.vertexOffset  = 0;
	vc.indexOffset   = 0;
	vcq.vertexCommitSize = 0;
	vcq.indexCommitSize  = 0;
	vcq.numSurfaces  = 0;
}

// ===========================================================================
// VaoCache_BindVao — bind the VaoCache's persistent VAO as current
// Delegates to R_BindVao — REAL.
// ===========================================================================
void VaoCache_BindVao( void ) {
	R_BindVao( vc.vao );
}

// ===========================================================================
// VaoCache_CheckAdd — boundary-check before queuing a surface
// All bounds arithmetic is REAL CPU math; sets out flags if limits exceeded.
// ===========================================================================
void VaoCache_CheckAdd( qboolean *endSurface, qboolean *recycleVertexBuffer, qboolean *recycleIndexBuffer, int numVerts, int numIndexes ) {
	int vertexesSize = (int)sizeof( srfVert_t ) * numVerts;
	int indexesSize  = (int)glRefConfig.vaoCacheGlIndexSize * numIndexes;

	if ( vc.vao->vertexesSize < vc.vertexOffset + vcq.vertexCommitSize + vertexesSize ) {
		*recycleVertexBuffer = qtrue;
		*recycleIndexBuffer  = qtrue;
		*endSurface          = qtrue;
	}

	if ( vc.vao->indexesSize < vc.indexOffset + vcq.indexCommitSize + indexesSize ) {
		*recycleIndexBuffer = qtrue;
		*endSurface         = qtrue;
	}

	if ( vc.numSurfaces + vcq.numSurfaces >= VAOCACHE_MAX_SURFACES ) {
		*recycleIndexBuffer = qtrue;
		*endSurface         = qtrue;
	}

	if ( vc.numBatches >= VAOCACHE_MAX_BATCHES ) {
		*recycleIndexBuffer = qtrue;
		*endSurface         = qtrue;
	}

	if ( vcq.numSurfaces >= VAOCACHE_QUEUE_MAX_SURFACES ) {
		*endSurface = qtrue;
	}

	if ( VAOCACHE_QUEUE_MAX_VERTEXES * (int)sizeof( srfVert_t ) < vcq.vertexCommitSize + vertexesSize ) {
		*endSurface = qtrue;
	}

	if ( VAOCACHE_QUEUE_MAX_INDEXES * (int)glRefConfig.vaoCacheGlIndexSize < vcq.indexCommitSize + indexesSize ) {
		*endSurface = qtrue;
	}
}

// ===========================================================================
// VaoCache_RecycleVertexBuffer — reset vertex ring-buffer write pointer
// Counter reset is REAL.  qglBindBuffer / qglBufferData are seamed.
// ===========================================================================
void VaoCache_RecycleVertexBuffer( void ) {
	// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ARRAY_BUFFER, vc.vao->vertexesVBO)
	// PARITY-SEAM(Metal-device): qglBufferData(GL_ARRAY_BUFFER, vc.vao->vertexesSize, NULL, GL_DYNAMIC_DRAW)
	(void)0;
	vc.vertexOffset = 0;
}

// ===========================================================================
// VaoCache_RecycleIndexBuffer — reset index ring-buffer + batch/surface counters
// Counter resets are REAL.  qglBindBuffer / qglBufferData are seamed.
// ===========================================================================
void VaoCache_RecycleIndexBuffer( void ) {
	// PARITY-SEAM(Metal-device): qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesIBO)
	// PARITY-SEAM(Metal-device): qglBufferData(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesSize, NULL, GL_DYNAMIC_DRAW)
	(void)0;
	vc.indexOffset  = 0;
	vc.numSurfaces  = 0;
	vc.numBatches   = 0;
}

// ===========================================================================
// VaoCache_InitQueue — reset per-frame surface batch queue counters
// REAL — pure counter zeroing, no GPU involvement.
// ===========================================================================
void VaoCache_InitQueue( void ) {
	vcq.vertexCommitSize = 0;
	vcq.indexCommitSize  = 0;
	vcq.numSurfaces      = 0;
}

// ===========================================================================
// VaoCache_AddSurface — enqueue one surface into the per-frame batch queue
// REAL — pointer bookkeeping + running size accumulators, no GPU involvement.
// ===========================================================================
void VaoCache_AddSurface( srfVert_t *verts, int numVerts, glIndex_t *indexes, int numIndexes ) {
	queuedSurface_t *queueEntry = vcq.surfaces + vcq.numSurfaces;
	queueEntry->vertexes   = verts;
	queueEntry->numVerts   = numVerts;
	queueEntry->indexes    = indexes;
	queueEntry->numIndexes = numIndexes;
	vcq.numSurfaces++;

	vcq.vertexCommitSize += (int)sizeof( srfVert_t ) * numVerts;
	vcq.indexCommitSize  += (int)glRefConfig.vaoCacheGlIndexSize * numIndexes;
}

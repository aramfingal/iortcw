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
// tr_surf.c — Metal port of code/renderergl2/tr_surface.c
// GL2 ref: tr_surface.c (file)
//
// THIS ENTIRE FILE IS BACK END
//
// Geometry builder: writes verts/indexes/normals/texcoords/colors into
// the tess (shaderCommands_t) CPU-side arrays.  All CPU math is REAL.
// GPU buffer operations (R_BindVao, RB_UpdateTessVao, R_DrawElements,
// VaoCache_*, qglBindBuffer, qglVertexAttribPointer) are
// PARITY-SEAM(Metal-device) — see tr_vbo.cpp (owner: Parker) when ported.

extern "C" {
#include "tr_local.h"
}

//============================================================================

/*
==============
RB_CheckOverflow
==============
*/
void RB_CheckOverflow( int verts, int indexes ) {
	if ( tess.numVertexes + verts < SHADER_MAX_VERTEXES
		 && tess.numIndexes + indexes < SHADER_MAX_INDEXES ) {
		return;
	}

	RB_EndSurface();

	if ( verts >= SHADER_MAX_VERTEXES ) {
		ri.Error( ERR_DROP, "RB_CheckOverflow: verts > MAX (%d > %d)", verts, SHADER_MAX_VERTEXES );
	}
	if ( indexes >= SHADER_MAX_INDEXES ) {
		ri.Error( ERR_DROP, "RB_CheckOverflow: indices > MAX (%d > %d)", indexes, SHADER_MAX_INDEXES );
	}

	RB_BeginSurface( tess.shader, tess.fogNum, tess.cubemapIndex );
}

/*
==============
RB_CheckVao
==============
*/
void RB_CheckVao( vao_t *vao ) {
	if ( vao != glState.currentVao ) {
		RB_EndSurface();
		RB_BeginSurface( tess.shader, tess.fogNum, tess.cubemapIndex );

		// PARITY-SEAM(Metal-device): R_BindVao binds a GPU VAO/VBO.
		// Under Metal the equivalent is selecting a vertex buffer/descriptor.
		// Stub until tr_vbo.cpp is ported (owner: Parker).
		R_BindVao( vao );
	}

	if ( vao != tess.vao ) {
		tess.useInternalVao = qfalse;
	}
}

/*
==============
RB_AddQuadStampExt
==============
*/
void RB_AddQuadStampExt( vec3_t origin, vec3_t left, vec3_t up, float color[4], float s1, float t1, float s2, float t2 ) {
	vec3_t normal;
	int16_t iNormal[4];
	uint16_t iColor[4];
	int ndx;

	RB_CheckVao( tess.vao );

	RB_CHECKOVERFLOW( 4, 6 );

	ndx = tess.numVertexes;

	// triangle indexes for a simple quad
	tess.indexes[ tess.numIndexes ]     = ndx;
	tess.indexes[ tess.numIndexes + 1 ] = ndx + 1;
	tess.indexes[ tess.numIndexes + 2 ] = ndx + 3;

	tess.indexes[ tess.numIndexes + 3 ] = ndx + 3;
	tess.indexes[ tess.numIndexes + 4 ] = ndx + 1;
	tess.indexes[ tess.numIndexes + 5 ] = ndx + 2;

	tess.xyz[ndx][0] = origin[0] + left[0] + up[0];
	tess.xyz[ndx][1] = origin[1] + left[1] + up[1];
	tess.xyz[ndx][2] = origin[2] + left[2] + up[2];

	tess.xyz[ndx + 1][0] = origin[0] - left[0] + up[0];
	tess.xyz[ndx + 1][1] = origin[1] - left[1] + up[1];
	tess.xyz[ndx + 1][2] = origin[2] - left[2] + up[2];

	tess.xyz[ndx + 2][0] = origin[0] - left[0] - up[0];
	tess.xyz[ndx + 2][1] = origin[1] - left[1] - up[1];
	tess.xyz[ndx + 2][2] = origin[2] - left[2] - up[2];

	tess.xyz[ndx + 3][0] = origin[0] + left[0] - up[0];
	tess.xyz[ndx + 3][1] = origin[1] + left[1] - up[1];
	tess.xyz[ndx + 3][2] = origin[2] + left[2] - up[2];

	// constant normal all the way around
	VectorSubtract( vec3_origin, backEnd.viewParms.or.axis[0], normal );

	R_VaoPackNormal( iNormal, normal );

	VectorCopy4( iNormal, tess.normal[ndx] );
	VectorCopy4( iNormal, tess.normal[ndx + 1] );
	VectorCopy4( iNormal, tess.normal[ndx + 2] );
	VectorCopy4( iNormal, tess.normal[ndx + 3] );

	// standard square texture coordinates
	VectorSet2( tess.texCoords[ndx],   s1, t1 );
	VectorSet2( tess.lightCoords[ndx], s1, t1 );

	VectorSet2( tess.texCoords[ndx + 1],   s2, t1 );
	VectorSet2( tess.lightCoords[ndx + 1], s2, t1 );

	VectorSet2( tess.texCoords[ndx + 2],   s2, t2 );
	VectorSet2( tess.lightCoords[ndx + 2], s2, t2 );

	VectorSet2( tess.texCoords[ndx + 3],   s1, t2 );
	VectorSet2( tess.lightCoords[ndx + 3], s1, t2 );

	// constant color all the way around
	R_VaoPackColor( iColor, color );

	VectorCopy4( iColor, tess.color[ndx] );
	VectorCopy4( iColor, tess.color[ndx + 1] );
	VectorCopy4( iColor, tess.color[ndx + 2] );
	VectorCopy4( iColor, tess.color[ndx + 3] );

	tess.numVertexes += 4;
	tess.numIndexes  += 6;
}

/*
==============
RB_AddQuadStamp
==============
*/
void RB_AddQuadStamp( vec3_t origin, vec3_t left, vec3_t up, float color[4] ) {
	RB_AddQuadStampExt( origin, left, up, color, 0, 0, 1, 1 );
}

/*
==============
RB_InstantQuad2
==============
*/
void RB_InstantQuad2( vec4_t quadVerts[4], vec2_t texCoords[4] ) {
	// PARITY-SEAM(Metal-device): GLimp_LogComment → no-op under Metal
	// (provided by sdl_glimp.c which is not linked into renderer_metal).

	tess.numVertexes = 0;
	tess.numIndexes  = 0;
	tess.firstIndex  = 0;

	VectorCopy4( quadVerts[0], tess.xyz[tess.numVertexes] );
	VectorCopy2( texCoords[0], tess.texCoords[tess.numVertexes] );
	tess.numVertexes++;

	VectorCopy4( quadVerts[1], tess.xyz[tess.numVertexes] );
	VectorCopy2( texCoords[1], tess.texCoords[tess.numVertexes] );
	tess.numVertexes++;

	VectorCopy4( quadVerts[2], tess.xyz[tess.numVertexes] );
	VectorCopy2( texCoords[2], tess.texCoords[tess.numVertexes] );
	tess.numVertexes++;

	VectorCopy4( quadVerts[3], tess.xyz[tess.numVertexes] );
	VectorCopy2( texCoords[3], tess.texCoords[tess.numVertexes] );
	tess.numVertexes++;

	tess.indexes[tess.numIndexes++] = 0;
	tess.indexes[tess.numIndexes++] = 1;
	tess.indexes[tess.numIndexes++] = 2;
	tess.indexes[tess.numIndexes++] = 0;
	tess.indexes[tess.numIndexes++] = 2;
	tess.indexes[tess.numIndexes++] = 3;

	// PARITY-SEAM(Metal-device): RB_UpdateTessVao uploads CPU arrays to GPU VAO.
	// R_DrawElements issues the GPU draw call.
	// Both are stubs until tr_vbo.cpp + Metal draw path are wired (owner: Parker/Lambert).
	RB_UpdateTessVao( ATTR_POSITION | ATTR_TEXCOORD );
	R_DrawElements( tess.numIndexes, tess.firstIndex );

	tess.numIndexes  = 0;
	tess.numVertexes = 0;
	tess.firstIndex  = 0;
}

/*
==============
RB_InstantQuad
==============
*/
void RB_InstantQuad( vec4_t quadVerts[4] ) {
	vec2_t texCoords[4];

	VectorSet2( texCoords[0], 0.0f, 0.0f );
	VectorSet2( texCoords[1], 1.0f, 0.0f );
	VectorSet2( texCoords[2], 1.0f, 1.0f );
	VectorSet2( texCoords[3], 0.0f, 1.0f );

	// PARITY-SEAM(Metal-device): GLSL_BindProgram / GLSL_SetUniform* are GL2
	// shader operations.  Under Metal they are no-op scaffold stubs until
	// tr_glsl.cpp is ported (owner: Lambert).
	GLSL_BindProgram( &tr.textureColorShader );
	GLSL_SetUniformMat4( &tr.textureColorShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GLSL_SetUniformVec4( &tr.textureColorShader, UNIFORM_COLOR, colorWhite );

	RB_InstantQuad2( quadVerts, texCoords );
}

/*
==============
RB_SurfaceSplash
==============
*/
static void RB_SurfaceSplash( void ) {
	vec3_t left, up;
	float radius;
	float colors[4];
	trRefEntity_t   *ent = backEnd.currentEntity;

	// calculate the xyz locations for the four corners
	radius = ent->e.radius;

	VectorSet( left, -radius, 0, 0 );
	VectorSet( up, 0, radius, 0 );
	if ( backEnd.viewParms.isMirror ) {
		VectorSubtract( vec3_origin, left, left );
	}

	VectorScale4( ent->e.shaderRGBA, 1.0f / 255.0f, colors );

	RB_AddQuadStamp( ent->e.origin, left, up, colors );
}

/*
==============
RB_SurfaceSprite
==============
*/
static void RB_SurfaceSprite( void ) {
	vec3_t left, up;
	float radius;
	float colors[4];
	trRefEntity_t   *ent = backEnd.currentEntity;

	radius = ent->e.radius;
	if ( ent->e.rotation == 0 ) {
		VectorScale( backEnd.viewParms.or.axis[1], radius, left );
		VectorScale( backEnd.viewParms.or.axis[2], radius, up );
	} else {
		float s, c;
		float ang;

		ang = M_PI * ent->e.rotation / 180;
		s = sin( ang );
		c = cos( ang );

		VectorScale( backEnd.viewParms.or.axis[1],  c * radius, left );
		VectorMA(    left, -s * radius, backEnd.viewParms.or.axis[2], left );

		VectorScale( backEnd.viewParms.or.axis[2], c * radius, up );
		VectorMA(    up,    s * radius, backEnd.viewParms.or.axis[1], up );
	}
	if ( backEnd.viewParms.isMirror ) {
		VectorSubtract( vec3_origin, left, left );
	}

	VectorScale4( ent->e.shaderRGBA, 1.0f / 255.0f, colors );

	RB_AddQuadStamp( ent->e.origin, left, up, colors );
}

/*
=============
RB_SurfacePolychain
=============
*/
static void RB_SurfacePolychain( srfPoly_t *p ) {
	int i;
	int numv;

	RB_CheckVao( tess.vao );

	RB_CHECKOVERFLOW( p->numVerts, 3 * ( p->numVerts - 2 ) );

	// fan triangles into the tess array
	numv = tess.numVertexes;
	for ( i = 0; i < p->numVerts; i++ ) {
		VectorCopy( p->verts[i].xyz, tess.xyz[numv] );
		tess.texCoords[numv][0] = p->verts[i].st[0];
		tess.texCoords[numv][1] = p->verts[i].st[1];
		tess.color[numv][0] = (int)p->verts[i].modulate[0] * 257;
		tess.color[numv][1] = (int)p->verts[i].modulate[1] * 257;
		tess.color[numv][2] = (int)p->verts[i].modulate[2] * 257;
		tess.color[numv][3] = (int)p->verts[i].modulate[3] * 257;
		numv++;
	}

	// generate fan indexes into the tess array
	for ( i = 0; i < p->numVerts - 2; i++ ) {
		tess.indexes[tess.numIndexes + 0] = tess.numVertexes;
		tess.indexes[tess.numIndexes + 1] = tess.numVertexes + i + 1;
		tess.indexes[tess.numIndexes + 2] = tess.numVertexes + i + 2;
		tess.numIndexes += 3;
	}

	tess.numVertexes = numv;
}

/*
==============
RB_SurfaceVertsAndIndexes
==============
*/
static void RB_SurfaceVertsAndIndexes( int numVerts, srfVert_t *verts, int numIndexes, glIndex_t *indexes, int dlightBits, int pshadowBits ) {
	int i;
	glIndex_t      *inIndex;
	srfVert_t      *dv;
	float          *xyz, *texCoords, *lightCoords;
	int16_t        *lightdir;
	int16_t        *normal;
	int16_t        *tangent;
	glIndex_t      *outIndex;
	uint16_t       *color;

	RB_CheckVao( tess.vao );

	RB_CHECKOVERFLOW( numVerts, numIndexes );

	inIndex  = indexes;
	outIndex = &tess.indexes[ tess.numIndexes ];
	for ( i = 0 ; i < numIndexes ; i++ ) {
		*outIndex++ = tess.numVertexes + *inIndex++;
	}
	tess.numIndexes += numIndexes;

	if ( tess.shader->vertexAttribs & ATTR_POSITION ) {
		dv  = verts;
		xyz = tess.xyz[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, xyz += 4 )
			VectorCopy( dv->xyz, xyz );
	}

	if ( tess.shader->vertexAttribs & ATTR_NORMAL ) {
		dv     = verts;
		normal = tess.normal[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, normal += 4 )
			VectorCopy4( dv->normal, normal );
	}

	if ( tess.shader->vertexAttribs & ATTR_TANGENT ) {
		dv      = verts;
		tangent = tess.tangent[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, tangent += 4 )
			VectorCopy4( dv->tangent, tangent );
	}

	if ( tess.shader->vertexAttribs & ATTR_TEXCOORD ) {
		dv        = verts;
		texCoords = tess.texCoords[tess.numVertexes];
		for ( i = 0 ; i < numVerts ; i++, dv++, texCoords += 2 )
			VectorCopy2( dv->st, texCoords );
	}

	if ( tess.shader->vertexAttribs & ATTR_LIGHTCOORD ) {
		dv          = verts;
		lightCoords = tess.lightCoords[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, lightCoords += 2 )
			VectorCopy2( dv->lightmap, lightCoords );
	}

	if ( tess.shader->vertexAttribs & ATTR_COLOR ) {
		dv    = verts;
		color = tess.color[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, color += 4 )
			VectorCopy4( dv->color, color );
	}

	if ( tess.shader->vertexAttribs & ATTR_LIGHTDIRECTION ) {
		dv       = verts;
		lightdir = tess.lightdir[ tess.numVertexes ];
		for ( i = 0 ; i < numVerts ; i++, dv++, lightdir += 4 )
			VectorCopy4( dv->lightdir, lightdir );
	}

#if 0  // nothing even uses vertex dlightbits
	for ( i = 0 ; i < numVerts ; i++ ) {
		tess.vertexDlightBits[ tess.numVertexes + i ] = dlightBits;
	}
#endif

	tess.dlightBits  |= dlightBits;
	tess.pshadowBits |= pshadowBits;

	tess.numVertexes += numVerts;
}

/*
==============
RB_SurfaceVaoCached
==============
*/
static qboolean RB_SurfaceVaoCached( int numVerts, srfVert_t *verts, int numIndexes, glIndex_t *indexes, int dlightBits, int pshadowBits ) {
	// PARITY-SEAM(Metal-device): VaoCache_* calls below manage a GPU-side
	// dynamic VAO/VBO cache.  Under Metal the equivalent is a ring-buffer of
	// MTLBuffer objects; that layer (tr_vbo.cpp, owner: Parker) is not yet
	// ported.  Return qfalse so callers fall back to RB_SurfaceVertsAndIndexes.
	qboolean recycleVertexBuffer = qfalse;
	qboolean recycleIndexBuffer  = qfalse;
	qboolean endSurface          = qfalse;

	// VaoCache_DrawElements has no Metal implementation, so a surface claimed here would
	// never be drawn (r_vaoCache 1 made world surfaces vanish). Always take the tess path,
	// which streams through per-frame memory with one upload per surface.
	if ( 1 ) {
		return qfalse;
	}

	if ( !r_vaoCache->integer ) {
		return qfalse;
	}

	if ( !( !ShaderRequiresCPUDeforms( tess.shader ) && !tess.shader->isSky && !tess.shader->isPortal ) ) {
		return qfalse;
	}

	if ( !numIndexes || !numVerts ) {
		return qfalse;
	}

	VaoCache_BindVao();

	tess.dlightBits  |= dlightBits;
	tess.pshadowBits |= pshadowBits;

	VaoCache_CheckAdd( &endSurface, &recycleVertexBuffer, &recycleIndexBuffer, numVerts, numIndexes );

	if ( endSurface ) {
		RB_EndSurface();
		RB_BeginSurface( tess.shader, tess.fogNum, tess.cubemapIndex );
	}

	if ( recycleVertexBuffer ) {
		VaoCache_RecycleVertexBuffer();
	}

	if ( recycleIndexBuffer ) {
		VaoCache_RecycleIndexBuffer();
	}

	if ( !tess.numVertexes ) {
		VaoCache_InitQueue();
	}

	VaoCache_AddSurface( verts, numVerts, indexes, numIndexes );

	tess.numIndexes      += numIndexes;
	tess.numVertexes     += numVerts;
	tess.useInternalVao   = qfalse;
	tess.useCacheVao      = qtrue;

	return qtrue;
}

/*
=============
RB_SurfaceTriangles
=============
*/
static void RB_SurfaceTriangles( srfBspSurface_t *srf ) {
	if ( RB_SurfaceVaoCached( srf->numVerts, srf->verts, srf->numIndexes,
							  srf->indexes, srf->dlightBits, srf->pshadowBits ) ) {
		return;
	}

	RB_SurfaceVertsAndIndexes( srf->numVerts, srf->verts, srf->numIndexes,
							   srf->indexes, srf->dlightBits, srf->pshadowBits );
}

/*
==============
RB_SurfaceBeam
==============
*/
static void RB_SurfaceBeam( void ) {
#define NUM_BEAM_SEGS 6
	refEntity_t *e;
	shaderProgram_t *sp = &tr.textureColorShader;
	int i;
	vec3_t perpvec;
	vec3_t direction, normalized_direction;
	vec3_t start_points[NUM_BEAM_SEGS], end_points[NUM_BEAM_SEGS];
	vec3_t oldorigin, origin;

	e = &backEnd.currentEntity->e;

	oldorigin[0] = e->oldorigin[0];
	oldorigin[1] = e->oldorigin[1];
	oldorigin[2] = e->oldorigin[2];

	origin[0] = e->origin[0];
	origin[1] = e->origin[1];
	origin[2] = e->origin[2];

	normalized_direction[0] = direction[0] = oldorigin[0] - origin[0];
	normalized_direction[1] = direction[1] = oldorigin[1] - origin[1];
	normalized_direction[2] = direction[2] = oldorigin[2] - origin[2];

	if ( VectorNormalize( normalized_direction ) == 0 ) {
		return;
	}

	PerpendicularVector( perpvec, normalized_direction );
	VectorScale( perpvec, 4, perpvec );

	for ( i = 0; i < NUM_BEAM_SEGS; i++ )
	{
		RotatePointAroundVector( start_points[i], normalized_direction, perpvec, ( 360.0 / NUM_BEAM_SEGS ) * i );
		VectorAdd( start_points[i], direction, end_points[i] );
	}

	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

	tess.numVertexes = 0;
	tess.numIndexes  = 0;
	tess.firstIndex  = 0;

	for ( i = 0; i <= NUM_BEAM_SEGS; i++ ) {
		VectorCopy( start_points[ i % NUM_BEAM_SEGS ], tess.xyz[tess.numVertexes++] );
		VectorCopy( end_points  [ i % NUM_BEAM_SEGS ], tess.xyz[tess.numVertexes++] );
	}

	for ( i = 0; i < NUM_BEAM_SEGS; i++ ) {
		tess.indexes[tess.numIndexes++] =       i      * 2;
		tess.indexes[tess.numIndexes++] =      ( i + 1 ) * 2;
		tess.indexes[tess.numIndexes++] = 1  +  i      * 2;

		tess.indexes[tess.numIndexes++] = 1  +  i      * 2;
		tess.indexes[tess.numIndexes++] =      ( i + 1 ) * 2;
		tess.indexes[tess.numIndexes++] = 1  + ( i + 1 ) * 2;
	}

	// PARITY-SEAM(Metal-device): RB_UpdateTessVao uploads CPU arrays to GPU;
	// R_DrawElements issues the GPU draw call.
	// Stubs until tr_vbo.cpp is ported (owner: Parker).
	RB_UpdateTessVao( ATTR_POSITION );

	GLSL_BindProgram( sp );
	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	GLSL_SetUniformVec4( sp, UNIFORM_COLOR, colorRed );
	GLSL_SetUniformInt( sp, UNIFORM_ALPHATEST, 0 );

	R_DrawElements( tess.numIndexes, tess.firstIndex );

	tess.numIndexes  = 0;
	tess.numVertexes = 0;
	tess.firstIndex  = 0;
}

//================================================================================

/*
==============
DoRailCore
==============
*/
static void DoRailCore( const vec3_t start, const vec3_t end, const vec3_t up, float len, float spanWidth ) {
	float spanWidth2;
	int vbase;
	float t = len / 256.0f;

	RB_CheckVao( tess.vao );

	RB_CHECKOVERFLOW( 4, 6 );

	vbase = tess.numVertexes;

	spanWidth2 = -spanWidth;

	VectorMA( start, spanWidth, up, tess.xyz[tess.numVertexes] );
	tess.texCoords[tess.numVertexes][0] = 0;
	tess.texCoords[tess.numVertexes][1] = 0;
	tess.color[tess.numVertexes][0] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[0] * 0.25f * 257.0f );
	tess.color[tess.numVertexes][1] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[1] * 0.25f * 257.0f );
	tess.color[tess.numVertexes][2] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[2] * 0.25f * 257.0f );
	tess.numVertexes++;

	VectorMA( start, spanWidth2, up, tess.xyz[tess.numVertexes] );
	tess.texCoords[tess.numVertexes][0] = 0;
	tess.texCoords[tess.numVertexes][1] = 1;
	tess.color[tess.numVertexes][0] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[0] * 257 );
	tess.color[tess.numVertexes][1] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[1] * 257 );
	tess.color[tess.numVertexes][2] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[2] * 257 );
	tess.numVertexes++;

	VectorMA( end, spanWidth, up, tess.xyz[tess.numVertexes] );
	tess.texCoords[tess.numVertexes][0] = t;
	tess.texCoords[tess.numVertexes][1] = 0;
	tess.color[tess.numVertexes][0] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[0] * 257 );
	tess.color[tess.numVertexes][1] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[1] * 257 );
	tess.color[tess.numVertexes][2] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[2] * 257 );
	tess.numVertexes++;

	VectorMA( end, spanWidth2, up, tess.xyz[tess.numVertexes] );
	tess.texCoords[tess.numVertexes][0] = t;
	tess.texCoords[tess.numVertexes][1] = 1;
	tess.color[tess.numVertexes][0] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[0] * 257 );
	tess.color[tess.numVertexes][1] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[1] * 257 );
	tess.color[tess.numVertexes][2] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[2] * 257 );
	tess.numVertexes++;

	tess.indexes[tess.numIndexes++] = vbase;
	tess.indexes[tess.numIndexes++] = vbase + 1;
	tess.indexes[tess.numIndexes++] = vbase + 2;

	tess.indexes[tess.numIndexes++] = vbase + 2;
	tess.indexes[tess.numIndexes++] = vbase + 1;
	tess.indexes[tess.numIndexes++] = vbase + 3;
}

/*
==============
DoRailDiscs
==============
*/
static void DoRailDiscs( int numSegs, const vec3_t start, const vec3_t dir, const vec3_t right, const vec3_t up ) {
	int i;
	vec3_t pos[4];
	vec3_t v;
	int spanWidth = r_railWidth->integer;
	float c, s;
	float scale;

	if ( numSegs > 1 ) {
		numSegs--;
	}
	if ( !numSegs ) {
		return;
	}

	scale = 0.25;

	for ( i = 0; i < 4; i++ )
	{
		c = cos( DEG2RAD( 45 + i * 90 ) );
		s = sin( DEG2RAD( 45 + i * 90 ) );
		v[0] = ( right[0] * c + up[0] * s ) * scale * spanWidth;
		v[1] = ( right[1] * c + up[1] * s ) * scale * spanWidth;
		v[2] = ( right[2] * c + up[2] * s ) * scale * spanWidth;
		VectorAdd( start, v, pos[i] );

		if ( numSegs > 1 ) {
			// offset by 1 segment if we're doing a long distance shot
			VectorAdd( pos[i], dir, pos[i] );
		}
	}

	RB_CheckVao( tess.vao );

	for ( i = 0; i < numSegs; i++ )
	{
		int j;

		RB_CHECKOVERFLOW( 4, 6 );

		for ( j = 0; j < 4; j++ )
		{
			VectorCopy( pos[j], tess.xyz[tess.numVertexes] );
			tess.texCoords[tess.numVertexes][0] = (float)( j < 2 );
			tess.texCoords[tess.numVertexes][1] = (float)( j && j != 3 );
			tess.color[tess.numVertexes][0] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[0] * 257 );
			tess.color[tess.numVertexes][1] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[1] * 257 );
			tess.color[tess.numVertexes][2] = (uint16_t)( backEnd.currentEntity->e.shaderRGBA[2] * 257 );
			tess.numVertexes++;

			VectorAdd( pos[j], dir, pos[j] );
		}

		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 0;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 1;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 3;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 3;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 1;
		tess.indexes[tess.numIndexes++] = tess.numVertexes - 4 + 2;
	}
}

/*
** RB_SurfaceRailRings
*/
static void RB_SurfaceRailRings( void ) {
	refEntity_t *e;
	int numSegs;
	int len;
	vec3_t vec;
	vec3_t right, up;
	vec3_t start, end;

	e = &backEnd.currentEntity->e;

	VectorCopy( e->oldorigin, start );
	VectorCopy( e->origin,    end );

	VectorSubtract( end, start, vec );
	len = VectorNormalize( vec );
	MakeNormalVectors( vec, right, up );
	numSegs = (int)( len / r_railSegmentLength->value );
	if ( numSegs <= 0 ) {
		numSegs = 1;
	}

	VectorScale( vec, r_railSegmentLength->value, vec );

	DoRailDiscs( numSegs, start, vec, right, up );
}

/*
** RB_SurfaceRailCore
*/
static void RB_SurfaceRailCore( void ) {
	refEntity_t *e;
	int len;
	vec3_t right;
	vec3_t vec;
	vec3_t start, end;
	vec3_t v1, v2;

	e = &backEnd.currentEntity->e;

	VectorCopy( e->oldorigin, start );
	VectorCopy( e->origin,    end );

	VectorSubtract( end, start, vec );
	len = VectorNormalize( vec );

	VectorSubtract( start, backEnd.viewParms.or.origin, v1 );
	VectorNormalize( v1 );
	VectorSubtract( end, backEnd.viewParms.or.origin, v2 );
	VectorNormalize( v2 );
	CrossProduct( v1, v2, right );
	VectorNormalize( right );

	DoRailCore( start, end, right, (float)len, (float)r_railCoreWidth->integer );
}

/*
** RB_SurfaceLightningBolt
*/
static void RB_SurfaceLightningBolt( void ) {
	refEntity_t *e;
	int len;
	vec3_t right;
	vec3_t vec;
	vec3_t start, end;
	vec3_t v1, v2;
	int i;

	e = &backEnd.currentEntity->e;

	VectorCopy( e->oldorigin, end );
	VectorCopy( e->origin,    start );

	VectorSubtract( end, start, vec );
	len = VectorNormalize( vec );

	VectorSubtract( start, backEnd.viewParms.or.origin, v1 );
	VectorNormalize( v1 );
	VectorSubtract( end, backEnd.viewParms.or.origin, v2 );
	VectorNormalize( v2 );
	CrossProduct( v1, v2, right );
	VectorNormalize( right );

	for ( i = 0; i < 4; i++ ) {
		vec3_t temp;

		DoRailCore( start, end, right, (float)len, 8 );
		RotatePointAroundVector( temp, vec, right, 45 );
		VectorCopy( temp, right );
	}
}

/*
==============
LerpMeshVertexes
==============
*/
static void LerpMeshVertexes( mdvSurface_t *surf, float backlerp ) {
	float   *outXyz;
	int16_t *outNormal, *outTangent;
	mdvVertex_t *newVerts;
	int vertNum;

	newVerts = surf->verts + backEnd.currentEntity->e.frame * surf->numVerts;

	outXyz     = tess.xyz[tess.numVertexes];
	outNormal  = tess.normal[tess.numVertexes];
	outTangent = tess.tangent[tess.numVertexes];

	if ( backlerp == 0 ) {
		for (vertNum = 0; vertNum < surf->numVerts; vertNum++)
		{
			VectorCopy( newVerts->xyz,     outXyz );
			VectorCopy4( newVerts->normal,  outNormal );
			VectorCopy4( newVerts->tangent, outTangent );

			newVerts++;
			outXyz     += 4;
			outNormal  += 4;
			outTangent += 4;
		}
	} else
	{
		mdvVertex_t *oldVerts;

		oldVerts = surf->verts + backEnd.currentEntity->e.oldframe * surf->numVerts;

		for (vertNum = 0; vertNum < surf->numVerts; vertNum++)
		{
			/* VectorLerp inline — avoids extern-"C" linkage conflict with tr_extramath.cpp */
			outXyz[0] = newVerts->xyz[0] * ( 1.0f - backlerp ) + oldVerts->xyz[0] * backlerp;
			outXyz[1] = newVerts->xyz[1] * ( 1.0f - backlerp ) + oldVerts->xyz[1] * backlerp;
			outXyz[2] = newVerts->xyz[2] * ( 1.0f - backlerp ) + oldVerts->xyz[2] * backlerp;

			outNormal[0] = (int16_t)( newVerts->normal[0] * ( 1.0f - backlerp ) + oldVerts->normal[0] * backlerp );
			outNormal[1] = (int16_t)( newVerts->normal[1] * ( 1.0f - backlerp ) + oldVerts->normal[1] * backlerp );
			outNormal[2] = (int16_t)( newVerts->normal[2] * ( 1.0f - backlerp ) + oldVerts->normal[2] * backlerp );
			outNormal[3] = 0;

			outTangent[0] = (int16_t)( newVerts->tangent[0] * ( 1.0f - backlerp ) + oldVerts->tangent[0] * backlerp );
			outTangent[1] = (int16_t)( newVerts->tangent[1] * ( 1.0f - backlerp ) + oldVerts->tangent[1] * backlerp );
			outTangent[2] = (int16_t)( newVerts->tangent[2] * ( 1.0f - backlerp ) + oldVerts->tangent[2] * backlerp );
			outTangent[3] = newVerts->tangent[3];

			newVerts++;
			oldVerts++;
			outXyz     += 4;
			outNormal  += 4;
			outTangent += 4;
		}
	}
}

/*
=============
RB_SurfaceMesh
=============
*/
static void RB_SurfaceMesh( mdvSurface_t *surface ) {
	int j;
	float backlerp;
	mdvSt_t     *texCoords;
	int Bob, Doug;
	int numVerts;

	// RF, check for REFLAG_HANDONLY
	if ( backEnd.currentEntity->e.reFlags & REFLAG_ONLYHAND ) {
		if ( !strstr( surface->name, "hand" ) ) {
			return;
		}
	}

	if (  backEnd.currentEntity->e.oldframe == backEnd.currentEntity->e.frame ) {
		backlerp = 0;
	} else {
		backlerp = backEnd.currentEntity->e.backlerp;
	}

	RB_CheckVao( tess.vao );

	RB_CHECKOVERFLOW( surface->numVerts, surface->numIndexes );

	LerpMeshVertexes( surface, backlerp );

	Bob  = tess.numIndexes;
	Doug = tess.numVertexes;
	for (j = 0; j < surface->numIndexes; j++) {
		tess.indexes[Bob + j] = Doug + surface->indexes[j];
	}
	tess.numIndexes += surface->numIndexes;

	texCoords = surface->st;
	numVerts  = surface->numVerts;
	for ( j = 0; j < numVerts; j++ ) {
		tess.texCoords[Doug + j][0] = texCoords[j].st[0];
		tess.texCoords[Doug + j][1] = texCoords[j].st[1];
	}

	tess.numVertexes += surface->numVerts;
}

/*
==============
RB_SurfaceFace
==============
*/
static void RB_SurfaceFace( srfBspSurface_t *srf ) {
	if ( RB_SurfaceVaoCached( srf->numVerts, srf->verts, srf->numIndexes,
							  srf->indexes, srf->dlightBits, srf->pshadowBits ) ) {
		return;
	}

	RB_SurfaceVertsAndIndexes( srf->numVerts, srf->verts, srf->numIndexes,
							   srf->indexes, srf->dlightBits, srf->pshadowBits );
}

/*
==============
LodErrorForVolume
==============
*/
static float LodErrorForVolume( vec3_t local, float radius ) {
	vec3_t world;
	float d;

	if ( r_lodCurveError->value < 0 ) {
		return 0;
	}

	world[0] = local[0] * backEnd.or.axis[0][0] + local[1] * backEnd.or.axis[1][0] +
			   local[2] * backEnd.or.axis[2][0] + backEnd.or.origin[0];
	world[1] = local[0] * backEnd.or.axis[0][1] + local[1] * backEnd.or.axis[1][1] +
			   local[2] * backEnd.or.axis[2][1] + backEnd.or.origin[1];
	world[2] = local[0] * backEnd.or.axis[0][2] + local[1] * backEnd.or.axis[1][2] +
			   local[2] * backEnd.or.axis[2][2] + backEnd.or.origin[2];

	VectorSubtract( world, backEnd.viewParms.or.origin, world );
	d = DotProduct( world, backEnd.viewParms.or.axis[0] );

	if ( d < 0 ) {
		d = -d;
	}
	d -= radius;
	if ( d < 1 ) {
		d = 1;
	}

	return r_lodCurveError->value / d;
}

/*
=============
RB_SurfaceGrid
=============
*/
static void RB_SurfaceGrid( srfBspSurface_t *srf ) {
	int i, j;
	float   *xyz;
	float   *texCoords, *lightCoords;
	int16_t *normal;
	int16_t *tangent;
	uint16_t *color;
	int16_t *lightdir;
	srfVert_t   *dv;
	int rows, irows, vrows;
	int used;
	int widthTable[MAX_GRID_SIZE];
	int heightTable[MAX_GRID_SIZE];
	float lodError;
	int lodWidth, lodHeight;
	int numVertexes;
	int dlightBits;
	int pshadowBits;

	if ( RB_SurfaceVaoCached( srf->numVerts, srf->verts, srf->numIndexes,
							  srf->indexes, srf->dlightBits, srf->pshadowBits ) ) {
		return;
	}

	RB_CheckVao( tess.vao );

	dlightBits = srf->dlightBits;
	tess.dlightBits |= dlightBits;

	pshadowBits = srf->pshadowBits;
	tess.pshadowBits |= pshadowBits;

	lodError = LodErrorForVolume( srf->lodOrigin, srf->lodRadius );

	widthTable[0] = 0;
	lodWidth = 1;
	for ( i = 1; i < srf->width - 1; i++ ) {
		if ( srf->widthLodError[i] <= lodError ) {
			widthTable[lodWidth] = i;
			lodWidth++;
		}
	}
	widthTable[lodWidth] = srf->width - 1;
	lodWidth++;

	heightTable[0] = 0;
	lodHeight = 1;
	for ( i = 1; i < srf->height - 1; i++ ) {
		if ( srf->heightLodError[i] <= lodError ) {
			heightTable[lodHeight] = i;
			lodHeight++;
		}
	}
	heightTable[lodHeight] = srf->height - 1;
	lodHeight++;

	// very large grids may need multiple passes
	used = 0;
	while ( used < lodHeight - 1 ) {
		do {
			vrows = ( SHADER_MAX_VERTEXES - tess.numVertexes ) / lodWidth;
			irows = ( SHADER_MAX_INDEXES  - tess.numIndexes  ) / ( lodWidth * 6 );

			if ( vrows < 2 || irows < 1 ) {
				RB_EndSurface();
				RB_BeginSurface( tess.shader, tess.fogNum, tess.cubemapIndex );
			} else {
				break;
			}
		} while ( 1 );

		rows = irows;
		if ( vrows < irows + 1 ) {
			rows = vrows - 1;
		}
		if ( used + rows > lodHeight ) {
			rows = lodHeight - used;
		}

		numVertexes = tess.numVertexes;

		xyz        = tess.xyz[numVertexes];
		normal     = tess.normal[numVertexes];
		tangent    = tess.tangent[numVertexes];
		texCoords  = tess.texCoords[numVertexes];
		lightCoords = tess.lightCoords[numVertexes];
		color      = tess.color[numVertexes];
		lightdir   = tess.lightdir[numVertexes];

		for ( i = 0; i < rows; i++ ) {
			for ( j = 0; j < lodWidth; j++ ) {
				dv = srf->verts + heightTable[ used + i ] * srf->width
					 + widthTable[ j ];

				if ( tess.shader->vertexAttribs & ATTR_POSITION ) {
					VectorCopy( dv->xyz, xyz );
					xyz += 4;
				}

				if ( tess.shader->vertexAttribs & ATTR_NORMAL ) {
					VectorCopy4( dv->normal, normal );
					normal += 4;
				}

				if ( tess.shader->vertexAttribs & ATTR_TANGENT ) {
					VectorCopy4( dv->tangent, tangent );
					tangent += 4;
				}

				if ( tess.shader->vertexAttribs & ATTR_TEXCOORD ) {
					VectorCopy2( dv->st, texCoords );
					texCoords += 2;
				}

				if ( tess.shader->vertexAttribs & ATTR_LIGHTCOORD ) {
					VectorCopy2( dv->lightmap, lightCoords );
					lightCoords += 2;
				}

				if ( tess.shader->vertexAttribs & ATTR_COLOR ) {
					VectorCopy4( dv->color, color );
					color += 4;
				}

				if ( tess.shader->vertexAttribs & ATTR_LIGHTDIRECTION ) {
					VectorCopy4( dv->lightdir, lightdir );
					lightdir += 4;
				}
			}
		}

		// add the indexes
		{
			int numIndexes;
			int w, h;

			h = rows - 1;
			w = lodWidth - 1;
			numIndexes = tess.numIndexes;
			for (i = 0; i < h; i++) {
				for (j = 0; j < w; j++) {
					int v1, v2, v3, v4;

					v1 = numVertexes + i * lodWidth + j + 1;
					v2 = v1 - 1;
					v3 = v2 + lodWidth;
					v4 = v3 + 1;

					tess.indexes[numIndexes]   = v2;
					tess.indexes[numIndexes + 1] = v3;
					tess.indexes[numIndexes + 2] = v1;

					tess.indexes[numIndexes + 3] = v1;
					tess.indexes[numIndexes + 4] = v3;
					tess.indexes[numIndexes + 5] = v4;
					numIndexes += 6;
				}
			}

			tess.numIndexes = numIndexes;
		}

		tess.numVertexes += rows * lodWidth;

		used += rows - 1;
	}
}

//===========================================================================

/*
===================
RB_SurfaceAxis
===================
*/
static void RB_SurfaceAxis( void ) {
	// PARITY-STUB: no-op under Metal — the GL2 body is entirely inside
	// #if 0 in the source (uses deprecated immediate-mode qglBegin/qglEnd
	// which have no Metal equivalent and were never shipped).
}

//===========================================================================

/*
====================
RB_SurfaceEntity
====================
*/
static void RB_SurfaceEntity( surfaceType_t *surfType ) {
	(void)surfType;
	switch ( backEnd.currentEntity->e.reType ) {
	case RT_SPLASH:
		RB_SurfaceSplash();
		break;
	case RT_SPRITE:
		RB_SurfaceSprite();
		break;
	case RT_BEAM:
		RB_SurfaceBeam();
		break;
	case RT_RAIL_CORE:
		RB_SurfaceRailCore();
		break;
	case RT_RAIL_RINGS:
		RB_SurfaceRailRings();
		break;
	case RT_LIGHTNING:
		RB_SurfaceLightningBolt();
		break;
	default:
		RB_SurfaceAxis();
		break;
	}
}

/*
==============
RB_SurfaceBad
==============
*/
static void RB_SurfaceBad( surfaceType_t *surfType ) {
	(void)surfType;
	ri.Printf( PRINT_ALL, "Bad surface tesselated.\n" );
}

/*
==============
RB_SurfaceFlare
==============
*/
static void RB_SurfaceFlare( srfFlare_t *surf ) {
	if ( r_flares->integer ) {
		RB_AddFlare( surf, tess.fogNum, surf->origin, surf->color, 1.0f, surf->normal, 0, qtrue );
	}
}

/*
==============
RB_SurfaceVaoMdvMesh
==============
*/
void RB_SurfaceVaoMdvMesh( srfVaoMdvMesh_t * surface ) {
	refEntity_t    *refEnt;

	// CPU path (RB_SurfaceMesh -> LerpMeshVertexes fills tess) when:
	//  - the shader needs CPU deforms (as GL2),
	//  - the surface is a stencil shadow volume (RB_EndSurface hands tr.shadowShader to
	//    RB_ShadowTessEnd, which builds the volume from tess.xyz) or is not drawn by the
	//    generic stage iterator,
	//  - there is no GPU buffer, or r_metalGpuMd3 is off.
	if ( ShaderRequiresCPUDeforms( tess.shader )
		 || tess.shader == tr.shadowShader
		 || tess.shader->optimalStageIteratorFunc != RB_StageIteratorGeneric
		 || !surface->vao || !surface->vao->metalBuffer
		 || !r_metalGpuMd3->integer ) {
		RB_SurfaceMesh( surface->mdvSurface );
		return;
	}

	//RB_CheckVao(surface->vao);
	RB_EndSurface();
	RB_BeginSurface( tess.shader, tess.fogNum, tess.cubemapIndex );

	R_BindVao( surface->vao );

	tess.useInternalVao = qfalse;
	tess.metalStaticVao = surface->vao;

	tess.numIndexes = surface->numIndexes;
	tess.numVertexes = surface->numVerts;

	refEnt = &backEnd.currentEntity->e;

	glState.vertexAttribsInterpolation = ( refEnt->oldframe == refEnt->frame ) ? 0.0f : refEnt->backlerp;

	if ( surface->mdvModel->numFrames > 1 ) {
		// GL2 sets the frame / oldframe attribute pointers here (tr_surface.c:1252-1290);
		// Metal binds them per draw in R_DrawElements (R_MetalBindVaoStreams).
		glState.vertexAnimation = qtrue;
	}

	RB_EndSurface();

	// So we don't lerp surfaces that shouldn't be lerped
	glState.vertexAnimation = qfalse;
}

/*
==============
RB_SurfaceSkip
==============
*/
static void RB_SurfaceSkip( void *surf ) {
	(void)surf;
}


// rb_surfaceTable[] — function-pointer dispatch table.
// Order MUST match the surfaceType_t enum in tr_local.h exactly.
void ( *rb_surfaceTable[SF_NUM_SURFACE_TYPES] ) ( void * ) = {
	( void ( * ) ( void* ) ) RB_SurfaceBad,          // SF_BAD,
	( void ( * ) ( void* ) ) RB_SurfaceSkip,         // SF_SKIP,
	( void ( * ) ( void* ) ) RB_SurfaceFace,         // SF_FACE,
	( void ( * ) ( void* ) ) RB_SurfaceGrid,         // SF_GRID,
	( void ( * ) ( void* ) ) RB_SurfaceTriangles,    // SF_TRIANGLES,
	( void ( * ) ( void* ) ) RB_SurfacePolychain,    // SF_POLY,
	( void ( * ) ( void* ) ) RB_SurfaceMesh,         // SF_MDV,
	( void ( * ) ( void* ) ) RB_SurfaceAnim,         // SF_MDS,
	( void ( * ) ( void* ) ) RB_MDRSurfaceAnim,   // SF_MDR,
	( void ( * ) ( void* ) ) RB_IQMSurfaceAnim,   // SF_IQM,
	( void ( * ) ( void* ) ) RB_SurfaceFlare,     // SF_FLARE,
	( void ( * ) ( void* ) ) RB_SurfaceEntity,    // SF_ENTITY
	( void ( * ) ( void* ) ) RB_SurfaceVaoMdvMesh,    // SF_VAO_MDVMESH
	( void ( * ) ( void* ) ) RB_IQMSurfaceAnimVao,    // SF_VAO_IQM
};

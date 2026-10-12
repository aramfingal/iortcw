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
// tr_shadows.cpp — Metal renderer counterpart to renderergl2/tr_shadows.c
// GL2 parity: 5 functions (R_AddEdgeDef, R_RenderShadowEdges,
//   RB_ShadowTessEnd, RB_ShadowFinish, RB_ProjectionShadowDeform).
//
// Seam discipline:
//   REAL — silhouette-edge detection loop (R_RenderShadowEdges),
//          projected-vertex math + facing determination + edge-table build
//          (RB_ShadowTessEnd), projection-shadow deform (RB_ProjectionShadowDeform).
//   PARITY-SEAM(Metal-device) — stencil GL state (qglEnable/StencilFunc/StencilOp),
//          qglColorMask, qglBegin/qglVertex3fv/qglEnd edge triangle-strip draws
//          → MTLDepthStencilState + MTLRenderCommandEncoder draw at device bring-up.
//   PARITY-SEAM(Metal-device) — RB_ShadowFinish full-screen darkening quad
//          → MTLDepthStencilState stencil compare + full-screen quad draw.

extern "C" {
#include "../qcommon/q_shared.h"
}
#include "tr_extramath.h"
extern "C" {
#include "tr_local.h"
}
// tr_metal_device.h includes metal-cpp C++ headers and wraps its own C decls in extern "C".
// Must be included at C++ file scope, NOT inside an extern "C" block.
#include "tr_metal_device.h"

// GL enum values expected by GLMetal_SetStencilState — match device-side constants exactly.
// GL2 ref: tr_shadows.c:222-231 (qglStencilFunc / qglStencilOp enum arguments)
#ifndef GL_NOTEQUAL
#define GL_NOTEQUAL 0x0205
#endif
#ifndef GL_ALWAYS
#define GL_ALWAYS 0x0207
#endif
#ifndef GL_KEEP
#define GL_KEEP 0x1E00
#endif
#ifndef GL_INCR
#define GL_INCR 0x1E02
#endif
#ifndef GL_DECR
#define GL_DECR 0x1E03
#endif

/*

  for a projection shadow:

  point[x] += light vector * ( z - shadow plane )
  point[y] +=
  point[z] = shadow plane

  1 0 light[x] / light[z]

*/

typedef struct {
	int i2;
	int facing;
} edgeDef_t;

#define MAX_EDGE_DEFS   32

static edgeDef_t edgeDefs[SHADER_MAX_VERTEXES][MAX_EDGE_DEFS];
static int numEdgeDefs[SHADER_MAX_VERTEXES];
// GL2 note: facing[] and shadowXyz[] were commented out in tr_shadows.c (lines 46-47)
// alongside the #if 0 blocks; both are activated here to support the REAL CPU paths.
static int shadowFacing[SHADER_MAX_INDEXES / 3];
static vec3_t shadowXyz[SHADER_MAX_VERTEXES];

// Shadow-volume triangle-list buffer: 6 verts (2 triangles) per silhouette edge.
// GL2 ref: tr_shadows.c:62 (qglBegin/qglVertex3fv strip per edge → triangle-list here)
static vec4_t shadowVolumeVerts[SHADER_MAX_INDEXES * 6];
static int numShadowVolumeVerts;

void R_AddEdgeDef( int i1, int i2, int isFacing ) {
	int c;

	c = numEdgeDefs[ i1 ];
	if ( c == MAX_EDGE_DEFS ) {
		return;     // overflow
	}
	edgeDefs[ i1 ][ c ].i2 = i2;
	edgeDefs[ i1 ][ c ].facing = isFacing;

	numEdgeDefs[ i1 ]++;
}

void R_RenderShadowEdges( void ) {
	int i;
	int c, c2;
	int j, k;
	int i2;
	int c_edges, c_rejected;
	int hit[2];

	// GL2 note: both the per-triangle "dumb" path and this silhouette-edge path
	// were inside a FIXME #if 0 in tr_shadows.c:62.  The edge-detection loop
	// (hit[] logic) is REAL CPU; only the per-edge triangle-strip draw is seamed.

	numShadowVolumeVerts = 0;
	c_edges = 0;
	c_rejected = 0;

	// An edge is NOT a silhouette edge if its face doesn't face the light,
	// or if it has a reverse paired edge that also faces the light.
	// A well-behaved polyhedron has exactly two faces per edge, but many
	// models have dangling or overfanned edges.
	for ( i = 0 ; i < tess.numVertexes ; i++ ) {
		c = numEdgeDefs[ i ];
		for ( j = 0 ; j < c ; j++ ) {
			if ( !edgeDefs[ i ][ j ].facing ) {
				continue;
			}

			hit[0] = 0;
			hit[1] = 0;

			i2 = edgeDefs[ i ][ j ].i2;
			c2 = numEdgeDefs[ i2 ];
			for ( k = 0 ; k < c2 ; k++ ) {
				if ( edgeDefs[ i2 ][ k ].i2 == i ) {
					hit[ edgeDefs[ i2 ][ k ].facing ]++;
				}
			}

			// if it doesn't share the edge with another front-facing
			// triangle, it is a silhouette edge
			if ( hit[ 1 ] == 0 ) {
				// Overflow guard — mirrors R_AddEdgeDef discipline.
				// GL2 ref: tr_shadows.c:49 (MAX_EDGE_DEFS overflow check)
				if ( numShadowVolumeVerts + 6 > SHADER_MAX_INDEXES * 6 ) {
					c_rejected++;
					continue;
				}

				// Emit two triangles (A,B,C) + (C,B,D) in triangle-list order.
				// A = tess.xyz[i],   B = shadowXyz[i]
				// C = tess.xyz[i2],  D = shadowXyz[i2]
				// GL2 ref: tr_shadows.c:84-90 (qglBegin GL_TRIANGLE_STRIP + qglVertex3fv ×4)
				const float *A = tess.xyz[i];
				const float *B = shadowXyz[i];
				const float *C = tess.xyz[i2];
				const float *D = shadowXyz[i2];

				// Triangle 0: A B C
				shadowVolumeVerts[numShadowVolumeVerts][0] = A[0];
				shadowVolumeVerts[numShadowVolumeVerts][1] = A[1];
				shadowVolumeVerts[numShadowVolumeVerts][2] = A[2];
				shadowVolumeVerts[numShadowVolumeVerts][3] = 1.0f;
				numShadowVolumeVerts++;

				shadowVolumeVerts[numShadowVolumeVerts][0] = B[0];
				shadowVolumeVerts[numShadowVolumeVerts][1] = B[1];
				shadowVolumeVerts[numShadowVolumeVerts][2] = B[2];
				shadowVolumeVerts[numShadowVolumeVerts][3] = 1.0f;
				numShadowVolumeVerts++;

				shadowVolumeVerts[numShadowVolumeVerts][0] = C[0];
				shadowVolumeVerts[numShadowVolumeVerts][1] = C[1];
				shadowVolumeVerts[numShadowVolumeVerts][2] = C[2];
				shadowVolumeVerts[numShadowVolumeVerts][3] = 1.0f;
				numShadowVolumeVerts++;

				// Triangle 1: C B D
				shadowVolumeVerts[numShadowVolumeVerts][0] = C[0];
				shadowVolumeVerts[numShadowVolumeVerts][1] = C[1];
				shadowVolumeVerts[numShadowVolumeVerts][2] = C[2];
				shadowVolumeVerts[numShadowVolumeVerts][3] = 1.0f;
				numShadowVolumeVerts++;

				shadowVolumeVerts[numShadowVolumeVerts][0] = B[0];
				shadowVolumeVerts[numShadowVolumeVerts][1] = B[1];
				shadowVolumeVerts[numShadowVolumeVerts][2] = B[2];
				shadowVolumeVerts[numShadowVolumeVerts][3] = 1.0f;
				numShadowVolumeVerts++;

				shadowVolumeVerts[numShadowVolumeVerts][0] = D[0];
				shadowVolumeVerts[numShadowVolumeVerts][1] = D[1];
				shadowVolumeVerts[numShadowVolumeVerts][2] = D[2];
				shadowVolumeVerts[numShadowVolumeVerts][3] = 1.0f;
				numShadowVolumeVerts++;

				c_edges++;
			} else {
				c_rejected++;
			}
		}
	}

	(void)c_edges;
	(void)c_rejected;
}

/*
=================
RB_ShadowTessEnd

triangleFromEdge[ v1 ][ v2 ]


  set triangle from edge( v1, v2, tri )
  if ( facing[ triangleFromEdge[ v1 ][ v2 ] ] && !facing[ triangleFromEdge[ v2 ][ v1 ] ) {
  }
=================
*/

void RB_ShadowTessEnd( void ) {
	int i;
	int numTris;
	vec3_t lightDir;

	// GL2 note: entire body was inside #if 0 in tr_shadows.c:158.
	// CPU math (vertex projection, facing determination, edge table build) is REAL;
	// stencil GL state + two-sided shadow-edge draw passes are PARITY-SEAM.

	if ( glConfig.stencilBits < 4 ) {
		return;
	}

	VectorCopy( backEnd.currentEntity->lightDir, lightDir );

	// project vertexes away from light direction — REAL CPU
	for ( i = 0 ; i < tess.numVertexes ; i++ ) {
		VectorMA( tess.xyz[i], -512, lightDir, shadowXyz[i] );
	}

	// decide which triangles face the light — REAL CPU
	Com_Memset( numEdgeDefs, 0, 4 * tess.numVertexes );

	numTris = tess.numIndexes / 3;
	for ( i = 0 ; i < numTris ; i++ ) {
		int i1, i2, i3;
		vec3_t d1, d2, normal;
		float   *v1, *v2, *v3;
		float d;

		i1 = tess.indexes[ i * 3 + 0 ];
		i2 = tess.indexes[ i * 3 + 1 ];
		i3 = tess.indexes[ i * 3 + 2 ];

		v1 = tess.xyz[ i1 ];
		v2 = tess.xyz[ i2 ];
		v3 = tess.xyz[ i3 ];

		VectorSubtract( v2, v1, d1 );
		VectorSubtract( v3, v1, d2 );
		CrossProduct( d1, d2, normal );

		d = DotProduct( normal, lightDir );
		if ( d > 0 ) {
			shadowFacing[ i ] = 1;
		} else {
			shadowFacing[ i ] = 0;
		}

		// build per-vertex edge table — REAL CPU
		R_AddEdgeDef( i1, i2, shadowFacing[ i ] );
		R_AddEdgeDef( i2, i3, shadowFacing[ i ] );
		R_AddEdgeDef( i3, i1, shadowFacing[ i ] );
	}

	// Two-pass stencil shadow volume draw.
	// Pass 1: GL_Cull(CT_BACK_SIDED); qglStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	// Pass 2: GL_Cull(CT_FRONT_SIDED); qglStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
	// GL2 regenerates the identical edge geometry for each pass; here it is built once and
	// both passes draw from a single upload. The passes stay separate (rather than one
	// two-sided-stencil draw) because clamped INCR/DECR results depend on pass order.
	// The device restores stencil-disabled state afterwards so subsequent draws are
	// byte-identical to pre-shadow.
	R_RenderShadowEdges();
	GLMetal_DrawShadowVolumeStencilPasses( &shadowVolumeVerts[0][0], numShadowVolumeVerts,
										   glState.modelviewProjection );
}


/*
=================
RB_ShadowFinish

Darken everything that is in a shadow volume.
We have to delay this until everything has been shadowed,
because otherwise shadows from different body parts would
overlap and double darken.
=================
*/

void RB_ShadowFinish( void ) {
	// guard checks are REAL CPU
	if ( r_shadows->integer != 2 ) {
		return;
	}
	if ( glConfig.stencilBits < 4 ) {
		return;
	}

	// Full-screen darkening quad: pixels inside a shadow volume (stencil != 0) are darkened.
	// GL2: qglEnable(GL_STENCIL_TEST); qglStencilFunc(GL_NOTEQUAL, 0, 255);
	//      qglColor3f(0.6f, 0.6f, 0.6f);
	//      GL_State(GLS_DEPTHMASK_TRUE|GLS_SRCBLEND_DST_COLOR|GLS_DSTBLEND_ZERO);
	//      qglBegin(GL_QUADS); ... qglEnd();
	//      qglDisable(GL_STENCIL_TEST);
	static const float darken[4] = { 0.6f, 0.6f, 0.6f, 1.0f };
	GLMetal_SetStencilState( qtrue, GL_NOTEQUAL, GL_KEEP, GL_KEEP, GL_KEEP, 0 );
	GLMetal_DrawShadowFinish( darken );
	// Restore: stencil disabled so subsequent draws are byte-identical to pre-shadow.
	GLMetal_SetStencilState( qfalse, GL_ALWAYS, GL_KEEP, GL_KEEP, GL_KEEP, 0 );
}


/*
=================
RB_ProjectionShadowDeform

=================
*/

void RB_ProjectionShadowDeform( void ) {
	float   *xyz;
	int i;
	float h;
	vec3_t ground;
	vec3_t light;
	float groundDist;
	float d;
	vec3_t lightDir;

	xyz = ( float * ) tess.xyz;

	ground[0] = backEnd.or.axis[0][2];
	ground[1] = backEnd.or.axis[1][2];
	ground[2] = backEnd.or.axis[2][2];

	groundDist = backEnd.or.origin[2] - backEnd.currentEntity->e.shadowPlane;

	VectorCopy( backEnd.currentEntity->lightDir, lightDir );
	d = DotProduct( lightDir, ground );
	// don't let the shadows get too long or go negative
	if ( d < 0.5 ) {
		VectorMA( lightDir, ( 0.5 - d ), ground, lightDir );
		d = DotProduct( lightDir, ground );
	}
	d = 1.0 / d;

	light[0] = lightDir[0] * d;
	light[1] = lightDir[1] * d;
	light[2] = lightDir[2] * d;

	for ( i = 0; i < tess.numVertexes; i++, xyz += 4 ) {
		h = DotProduct( xyz, ground ) + groundDist;

		xyz[0] -= light[0] * h;
		xyz[1] -= light[1] * h;
		xyz[2] -= light[2] * h;
	}
}

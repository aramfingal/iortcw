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
// tr_image.cpp -- Metal renderer image / texture management + skin subsystem
//
// FUNCTION COUNT: 49 (authoritative grep, verified 2026-06-04)
// REAL (CPU-only): generateHashValue, GL_TextureMode, R_SumOfUsedImages,
//   R_ImageList_f, ResampleTexture, RGBAtoYCoCgA, YCoCgAtoRGBA,
//   RGBAtoNormal, DoFCBI, DoFCBIQuick, DoLinear, ExpandHalfTextureToGrid,
//   FillInNormalizedZ, FCBIByBlock, R_LightScaleTexture, R_MipMapsRGB,
//   R_MipMapNormalHeight, R_BlendOverTexture, R_ConvertTextureFormat,
//   RawImage_SwizzleRA, RawImage_ScaleToPower2, RawImage_HasAlpha,
//   RawImage_GetFormat, CompressMonoBlock, CalculateMipSize,
//   PixelDataFormatFromInternalFormat, R_CreateImage, R_FindImageFile,
//   R_CreateDlightImage, R_InitFogTable, R_FogFactor, R_CreateFogImage,
//   R_CreateDefaultImage, R_CreateBuiltinImages, R_SetColorMappings,
//   R_InitImages, R_DeleteTextures, CommaParse, RE_RegisterSkin,
//   R_InitSkins, R_GetSkinByHandle, R_SkinList_f, R_LoadImage,
//   R_GammaCorrect
// PARITY-SEAM(Metal-device): RawImage_UploadToRgtc2Texture,
//   RawImage_UploadTexture mip-loop qgl calls, GL_TextureMode (sampler param calls),
//   R_SetColorMappings (GLimp_SetGamma)
// METAL TEXTURE LIFECYCLE: R_CreateImage2, R_UpdateSubImage, R_DeleteTextures
//
// image_t.texnum stays GLuint per parity contract.  It keys the Metal
// MTL::Texture side-table owned by tr_metal_device.

extern "C" {
#include "../qcommon/q_shared.h"
}
#include "tr_extramath.h"
extern "C" {
#include "tr_local.h"
#include "tr_dsa.h"
}

// Phase 5 texture upload seam: C texture lifecycle API, C++ draw accessors.
#include "tr_metal_device.h"

// GL-COMPAT: qglesMajorVersion = 0 (no GLES under Metal)
// GL2 ref: renderercommon/qgl.h:334
#ifndef qglesMajorVersion
#define qglesMajorVersion 0
#endif

// GL-COMPAT: QGL_VERSION_ATLEAST always false under Metal
#ifndef QGL_VERSION_ATLEAST
#define QGL_VERSION_ATLEAST( maj,min ) 0
#endif

// ---------------------------------------------------------------------------
// GL constants (standard OpenGL numeric values, used as integer tokens only).
// No GL driver present; these drive internal state tracking and seamed code.
// ---------------------------------------------------------------------------
#ifndef GL_NEAREST
#define GL_NEAREST                          0x2600
#endif
#ifndef GL_LINEAR
#define GL_LINEAR                           0x2601
#endif
#ifndef GL_NEAREST_MIPMAP_NEAREST
#define GL_NEAREST_MIPMAP_NEAREST           0x2700
#endif
#ifndef GL_LINEAR_MIPMAP_NEAREST
#define GL_LINEAR_MIPMAP_NEAREST            0x2701
#endif
#ifndef GL_NEAREST_MIPMAP_LINEAR
#define GL_NEAREST_MIPMAP_LINEAR            0x2702
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR             0x2703
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER               0x2800
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER               0x2801
#endif
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S                   0x2802
#endif
#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T                   0x2803
#endif
#ifndef GL_TEXTURE_WRAP_R
#define GL_TEXTURE_WRAP_R                   0x8072
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT       0x84FE
#endif
#ifndef GL_DEPTH_TEXTURE_MODE
#define GL_DEPTH_TEXTURE_MODE               0x884B
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE             0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC             0x884D
#endif
#ifndef GL_COMPARE_R_TO_TEXTURE
#define GL_COMPARE_R_TO_TEXTURE             0x884E
#endif
#ifndef GL_LEQUAL
#define GL_LEQUAL                           0x0203
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                    0x812F
#endif
#ifndef GL_REPEAT
#define GL_REPEAT                           0x2901
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
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE                    0x1401
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4
#define GL_UNSIGNED_SHORT_4_4_4_4           0x8033
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5             0x8363
#endif
#ifndef GL_DEPTH_COMPONENT
#define GL_DEPTH_COMPONENT                  0x1902
#endif
#ifndef GL_RGB
#define GL_RGB                              0x1907
#endif
#ifndef GL_RGBA
#define GL_RGBA                             0x1908
#endif
#ifndef GL_LUMINANCE
#define GL_LUMINANCE                        0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA                  0x190A
#endif
#ifndef GL_RGB5
#define GL_RGB5                             0x8050
#endif
#ifndef GL_RGB8
#define GL_RGB8                             0x8051
#endif
#ifndef GL_RGBA4
#define GL_RGBA4                            0x8056
#endif
#ifndef GL_RGBA8
#define GL_RGBA8                            0x8058
#endif
#ifndef GL_RGBA16
#define GL_RGBA16                           0x805B
#endif
#ifndef GL_LUMINANCE8
#define GL_LUMINANCE8                       0x8040
#endif
#ifndef GL_LUMINANCE8_ALPHA8
#define GL_LUMINANCE8_ALPHA8                0x8045
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16                0x81A5
#endif
#ifndef GL_DEPTH_COMPONENT16_ARB
#define GL_DEPTH_COMPONENT16_ARB            0x81A5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24                0x81A6
#endif
#ifndef GL_DEPTH_COMPONENT24_ARB
#define GL_DEPTH_COMPONENT24_ARB            0x81A6
#endif
#ifndef GL_DEPTH_COMPONENT32
#define GL_DEPTH_COMPONENT32                0x81A7
#endif
#ifndef GL_DEPTH_COMPONENT32_ARB
#define GL_DEPTH_COMPONENT32_ARB            0x81A7
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F                          0x881A
#endif
#ifndef GL_RGBA16F_ARB
#define GL_RGBA16F_ARB                      0x881A
#endif
#ifndef GL_R32F
#define GL_R32F                             0x822E
#endif
#ifndef GL_SRGB_EXT
#define GL_SRGB_EXT                         0x8C40
#endif
#ifndef GL_SRGB8_EXT
#define GL_SRGB8_EXT                        0x8C41
#endif
#ifndef GL_SRGB_ALPHA_EXT
#define GL_SRGB_ALPHA_EXT                   0x8C42
#endif
#ifndef GL_SRGB8_ALPHA8_EXT
#define GL_SRGB8_ALPHA8_EXT                 0x8C43
#endif
#ifndef GL_SLUMINANCE_ALPHA_EXT
#define GL_SLUMINANCE_ALPHA_EXT             0x8C44
#endif
#ifndef GL_SLUMINANCE8_ALPHA8_EXT
#define GL_SLUMINANCE8_ALPHA8_EXT           0x8C45
#endif
#ifndef GL_SLUMINANCE_EXT
#define GL_SLUMINANCE_EXT                   0x8C46
#endif
#ifndef GL_SLUMINANCE8_EXT
#define GL_SLUMINANCE8_EXT                  0x8C47
#endif
#ifndef GL_RGB4_S3TC
#define GL_RGB4_S3TC                        0x83A1
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT     0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT    0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT    0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT    0x83F3
#endif
#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT    0x8C4C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif
#ifndef GL_COMPRESSED_RED_RGTC1
#define GL_COMPRESSED_RED_RGTC1             0x8DBB
#endif
#ifndef GL_COMPRESSED_SIGNED_RED_RGTC1
#define GL_COMPRESSED_SIGNED_RED_RGTC1      0x8DBC
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2              0x8DBD
#endif
#ifndef GL_COMPRESSED_SIGNED_RG_RGTC2
#define GL_COMPRESSED_SIGNED_RG_RGTC2       0x8DBE
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM_ARB
#define GL_COMPRESSED_RGBA_BPTC_UNORM_ARB           0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB     0x8E8D
#endif
#ifndef GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB
#define GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB     0x8E8E
#endif
#ifndef GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB
#define GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB   0x8E8F
#endif

// Extern declarations for globals defined in tr_init.cpp
// GL2 ref: tr_init.c (definitions); tr_image.c (uses)
extern "C" {
extern cvar_t  *r_texturebits;
extern cvar_t  *r_ext_max_anisotropy;
extern cvar_t  *r_ext_compressed_textures;
extern qboolean textureFilterAnisotropic;
extern int maxAnisotropy;
}

// R_LoadDDS implemented in tr_image_dds.cpp
extern "C" void R_LoadDDS( const char *filename, byte **pic, int *width, int *height,
						   GLenum *picFormat, int *numMips );

#if defined( __APPLE__ )
extern "C" void GLMetal_CreateTextureWithFormatAndUsage( uint32_t texnum, int width, int height,
														 int internalFormat, const void *pixels, qboolean mipmap, int wrapClampMode,
														 int metalPixelFormat, uint32_t metalUsage ) __attribute__( ( weak_import ) );
#endif

// File-scope globals (GL2 ref: tr_image.c:27-34)
static byte s_intensitytable[256];
static unsigned char s_gammatable[256];

// gl_filter_min/gl_filter_max: integer tokens for current filter mode.
// Under Metal these will feed future MTLSamplerDescriptor construction.
int gl_filter_min = GL_LINEAR_MIPMAP_NEAREST;
int gl_filter_max = GL_LINEAR;

#define FILE_HASH_SIZE      4096
static image_t*        hashTable[FILE_HASH_SIZE];

// Incrementing counter for image_t.texnum placeholder handle.
// PARITY-SEAM(Metal-device): qglGenTextures not available without device.
// Monotonic counter keeps hash-table functional. GLuint->MTL::Texture side-table
// is Phase-4. GL2 ref: tr_image.c:2190
static GLuint s_texnumCounter = 1;

// Texture filter mode table (GL2 ref: tr_image.c:47-59)
typedef struct {
	const char *name;
	int minimize, maximize;
} textureMode_t;

textureMode_t modes[] = {
	{"GL_NEAREST",                GL_NEAREST,                GL_NEAREST},
	{"GL_LINEAR",                 GL_LINEAR,                 GL_LINEAR},
	{"GL_NEAREST_MIPMAP_NEAREST", GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST},
	{"GL_LINEAR_MIPMAP_NEAREST",  GL_LINEAR_MIPMAP_NEAREST,  GL_LINEAR},
	{"GL_NEAREST_MIPMAP_LINEAR",  GL_NEAREST_MIPMAP_LINEAR,  GL_NEAREST},
	{"GL_LINEAR_MIPMAP_LINEAR",   GL_LINEAR_MIPMAP_LINEAR,   GL_LINEAR}
};

// mipBlendColors (GL2 ref: tr_image.c:1439)
byte mipBlendColors[16][4] = {
	{0,0,0,0},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
};

// ===========================================================================
// FUNCTION 1/49: R_GammaCorrect -- REAL
// ===========================================================================
void R_GammaCorrect( byte *buffer, int bufSize ) {
	int i;
	for ( i = 0; i < bufSize; i++ ) {
		buffer[i] = s_gammatable[buffer[i]];
	}
}

// ===========================================================================
// FUNCTION 2/49: generateHashValue -- REAL (static)
// ===========================================================================
static long generateHashValue( const char *fname ) {
	int i;
	long hash;
	char letter;

	hash = 0;
	i = 0;
	while ( fname[i] != '\0' ) {
		letter = tolower( fname[i] );
		if ( letter == '.' ) {
			break;                       // don't include extension
		}
		if ( letter == '\\' ) {
			letter = '/';                  // damn path names
		}
		hash += (long)( letter ) * ( i + 119 );
		i++;
	}
	hash &= ( FILE_HASH_SIZE - 1 );
	return hash;
}

// ===========================================================================
// FUNCTION 3/49: GL_TextureMode -- REAL filter-global update;
//   per-image GPU sampler param calls PARITY-SEAM'd (Metal uses MTLSamplerDescriptor)
// ===========================================================================
void GL_TextureMode( const char *string ) {
	int i;
	image_t *glt;

	for ( i = 0 ; i < 6 ; i++ ) {
		if ( !Q_stricmp( modes[i].name, string ) ) {
			break;
		}
	}

	// hack to prevent trilinear from being set on voodoo,
	// because their driver freaks...
	if ( i == 5 && glConfig.hardwareType == GLHW_3DFX_2D3D ) {
		ri.Printf( PRINT_ALL, "Refusing to set trilinear on a voodoo.\n" );
		i = 3;
	}

	if ( i == 6 ) {
		ri.Printf( PRINT_ALL, "bad filter name\n" );
		return;
	}

	gl_filter_min = modes[i].minimize;
	gl_filter_max = modes[i].maximize;

	// PARITY: GL2 (tr_image.c:116-122) iterates all mipmap images and calls
	// qglTextureParameterfEXT to re-apply GL_TEXTURE_MIN/MAG_FILTER on each.
	// Under Metal there are no per-texture GL filter state records; samplers are
	// cached objects keyed on (wrap, filterLinear, mipmap) and resolved at draw
	// time.  Iterating images to set image-level bookkeeping is preserved for
	// structural parity, but the per-GPU-texture re-bind is replaced by a single
	// GLMetal_FlushSamplerCache() call so the sampler cache rebuilds with the
	// new mip-filter mode on the next draw.
	for ( i = 0 ; i < tr.numImages ; i++ ) {
		glt = tr.images[ i ];
		(void)glt; // per-image filter bookkeeping: no GL per-texture state under Metal
	}
	// Metal replacement for per-texture qglTextureParameterfEXT calls:
	// flush sampler cache so next draw creates samplers with updated gl_filter_min.
	// GL2 ref: tr_image.c:119-121 (qglTextureParameterfEXT GL_TEXTURE_MIN/MAG_FILTER)
	GLMetal_FlushSamplerCache();
}

// ===========================================================================
// FUNCTION 4/49: R_SumOfUsedImages -- REAL
// ===========================================================================
int R_SumOfUsedImages( void ) {
	int total;
	int i, fc = ( tr.frameCount - 1 );

	total = 0;
	for ( i = 0; i < tr.numImages; i++ ) {
		if ( tr.images[i]->frameUsed == fc ) {
			total += tr.images[i]->uploadWidth * tr.images[i]->uploadHeight;
		}
	}
	return total;
}

// ===========================================================================
// FUNCTION 5/49: R_ImageList_f -- REAL
// ===========================================================================
void R_ImageList_f( void ) {
	int i;
	int estTotalSize = 0;

	ri.Printf( PRINT_ALL, "\n      -w-- -h-- -type-- -size- --name-------\n" );

	for ( i = 0 ; i < tr.numImages ; i++ ) {
		image_t *image = tr.images[i];
		const char    *format = "????   ";
		const char    *sizeSuffix;
		int estSize;
		int displaySize;

		estSize = image->uploadHeight * image->uploadWidth;

		switch ( image->internalFormat ) {
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
			format = "sDXT1  ";
			estSize /= 2; break;
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
			format = "sDXT5  "; break;
		case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB:
			format = "sBPTC  "; break;
		case GL_COMPRESSED_RG_RGTC2:
			format = "RGTC2  "; break;
		case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
			format = "DXT1   ";
			estSize /= 2; break;
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
			format = "DXT1a  ";
			estSize /= 2; break;
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
			format = "DXT5   "; break;
		case GL_COMPRESSED_RGBA_BPTC_UNORM_ARB:
			format = "BPTC   "; break;
		case GL_RGB4_S3TC:
			format = "S3TC   ";
			estSize /= 2; break;
		case GL_RGBA16F:
			format = "RGBA16F";
			estSize *= 8; break;
		case GL_RGBA16:
			format = "RGBA16 ";
			estSize *= 8; break;
		case GL_RGBA4:
		case GL_RGBA8:
		case GL_RGBA:
			format = "RGBA   ";
			estSize *= 4; break;
		case GL_LUMINANCE8:
		case GL_LUMINANCE:
			format = "L      "; break;
		case GL_RGB5:
		case GL_RGB8:
		case GL_RGB:
			format = "RGB    ";
			estSize *= 3; break;
		case GL_LUMINANCE8_ALPHA8:
		case GL_LUMINANCE_ALPHA:
			format = "LA     ";
			estSize *= 2; break;
		case GL_SRGB_EXT:
		case GL_SRGB8_EXT:
			format = "sRGB   ";
			estSize *= 3; break;
		case GL_SRGB_ALPHA_EXT:
		case GL_SRGB8_ALPHA8_EXT:
			format = "sRGBA  ";
			estSize *= 4; break;
		case GL_SLUMINANCE_EXT:
		case GL_SLUMINANCE8_EXT:
			format = "sL     "; break;
		case GL_SLUMINANCE_ALPHA_EXT:
		case GL_SLUMINANCE8_ALPHA8_EXT:
			format = "sLA    ";
			estSize *= 2; break;
		case GL_DEPTH_COMPONENT16:
			format = "Depth16";
			estSize *= 2; break;
		case GL_DEPTH_COMPONENT24:
			format = "Depth24";
			estSize *= 3; break;
		case GL_DEPTH_COMPONENT:
		case GL_DEPTH_COMPONENT32:
			format = "Depth32";
			estSize *= 4; break;
		}

		if ( image->flags & IMGFLAG_MIPMAP ) {
			estSize += estSize / 2;
		}

		sizeSuffix = "b ";
		displaySize = estSize;
		if ( displaySize > 1024 ) {
			displaySize /= 1024; sizeSuffix = "kb";
		}
		if ( displaySize > 1024 ) {
			displaySize /= 1024; sizeSuffix = "Mb";
		}
		if ( displaySize > 1024 ) {
			displaySize /= 1024; sizeSuffix = "Gb";
		}

		ri.Printf( PRINT_ALL, "%4i: %4ix%4i %s %4i%s %s\n",
				   i, image->uploadWidth, image->uploadHeight, format, displaySize, sizeSuffix, image->imgName );
		estTotalSize += estSize;
	}

	ri.Printf( PRINT_ALL, " ---------\n" );
	ri.Printf( PRINT_ALL, " approx %i bytes\n", estTotalSize );
	ri.Printf( PRINT_ALL, " %i total images\n\n", tr.numImages );
}

// ===========================================================================
// FUNCTION 6/49: ResampleTexture -- REAL (static)
// ===========================================================================
static void ResampleTexture( byte *in, int inwidth, int inheight, byte *out,
							 int outwidth, int outheight ) {
	int i, j;
	byte *inrow, *inrow2;
	int frac, fracstep;
	int p1[2048], p2[2048];
	byte *pix1, *pix2, *pix3, *pix4;

	if ( outwidth > 2048 ) {
		ri.Error( ERR_DROP, "ResampleTexture: max width" );
	}

	fracstep = inwidth * 0x10000 / outwidth;
	frac = fracstep >> 2;
	for ( i = 0 ; i < outwidth ; i++ ) { p1[i] = 4 * ( frac >> 16 ); frac += fracstep; }
	frac = 3 * ( fracstep >> 2 );
	for ( i = 0 ; i < outwidth ; i++ ) { p2[i] = 4 * ( frac >> 16 ); frac += fracstep; }

	for (i = 0 ; i < outheight ; i++) {
		inrow  = in + 4 * inwidth * (int)( ( i + 0.25 ) * inheight / outheight );
		inrow2 = in + 4 * inwidth * (int)( ( i + 0.75 ) * inheight / outheight );
		for (j = 0 ; j < outwidth ; j++) {
			pix1 = inrow  + p1[j]; pix2 = inrow  + p2[j];
			pix3 = inrow2 + p1[j]; pix4 = inrow2 + p2[j];
			*out++ = ( pix1[0] + pix2[0] + pix3[0] + pix4[0] ) >> 2;
			*out++ = ( pix1[1] + pix2[1] + pix3[1] + pix4[1] ) >> 2;
			*out++ = ( pix1[2] + pix2[2] + pix3[2] + pix4[2] ) >> 2;
			*out++ = ( pix1[3] + pix2[3] + pix3[3] + pix4[3] ) >> 2;
		}
	}
}

// ===========================================================================
// FUNCTION 7/49: RGBAtoYCoCgA -- REAL (static)
// ===========================================================================
static void RGBAtoYCoCgA( const byte *in, byte *out, int width, int height ) {
	int x, y;
	for (y = 0; y < height; y++) {
		const byte *inbyte  = in  + y * width * 4;
		byte       *outbyte = out + y * width * 4;
		for (x = 0; x < width; x++) {
			byte r, g, b, a, rb;
			r = *inbyte++; g = *inbyte++; b = *inbyte++; a = *inbyte++;
			rb = ( r + b ) >> 1;
			outbyte[0] = ( g + rb ) >> 1;
			outbyte[1] = 128 + ( (int)r - (int)b ) / 2;
			outbyte[2] = 128 + ( (int)g - (int)rb );
			outbyte[3] = a;
			outbyte += 4;
		}
	}
}

// ===========================================================================
// FUNCTION 8/49: YCoCgAtoRGBA -- REAL (static)
// ===========================================================================
static void YCoCgAtoRGBA( const byte *in, byte *out, int width, int height ) {
	int x, y;
	for (y = 0; y < height; y++) {
		const byte *inbyte  = in  + y * width * 4;
		byte       *outbyte = out + y * width * 4;
		for (x = 0; x < width; x++) {
			byte Y, Co, Cg, a;
			int tmp, r, g, b;
			Y = *inbyte++; Co = *inbyte++; Cg = *inbyte++; a = *inbyte++;
			tmp = (int)Y - ( ( (int)Cg - 128 ) >> 1 );
			g   = CLAMP( tmp + ( (int)Cg - 128 ),         0, 255 );
			b   = CLAMP( tmp - ( ( (int)Co - 128 ) >> 1 ), 0, 255 );
			r   = CLAMP( b   + ( (int)Co - 128 ),          0, 255 );
			*outbyte++ = (byte)r; *outbyte++ = (byte)g;
			*outbyte++ = (byte)b; *outbyte++ = a;
		}
	}
}

// ===========================================================================
// FUNCTION 9/49: RGBAtoNormal -- REAL (static)
// ===========================================================================
static void RGBAtoNormal( const byte *in, byte *out, int width, int height, qboolean clampToEdge ) {
	int x, y, max;

	max = 1;
	for (y = 0; y < height; y++) {
		const byte *inbyte  = in  + y * width * 4;
		byte       *outbyte = out + y * width * 4 + 3;
		for (x = 0; x < width; x++) {
			byte result = ( inbyte[0] >> 2 ) + ( inbyte[1] >> 1 ) + ( inbyte[2] >> 2 );
			result = result * result / 255;
			*outbyte = result;
			max = MAX( max, *outbyte );
			outbyte += 4; inbyte += 4;
		}
	}

	if ( max < 255 ) {
		for (y = 0; y < height; y++) {
			byte *outbyte = out + y * width * 4 + 3;
			for (x = 0; x < width; x++) {
				*outbyte = *outbyte + ( 255 - max ); outbyte += 4;
			}
		}
	}

	for (y = 0; y < height; y++) {
		byte *outbyte = out + y * width * 4;
		for (x = 0; x < width; x++) {
			byte s[9];
			int x2, y2, i;
			vec3_t normal;
			i = 0;
			for (y2 = -1; y2 <= 1; y2++) {
				int src_y = y + y2;
				src_y = clampToEdge ? CLAMP( src_y, 0, height - 1 ) : ( src_y + height ) % height;
				for (x2 = -1; x2 <= 1; x2++) {
					int src_x = x + x2;
					src_x = clampToEdge ? CLAMP( src_x, 0, width - 1 ) : ( src_x + width ) % width;
					s[i++] = *( out + ( src_y * width + src_x ) * 4 + 3 );
				}
			}
			normal[0] =   s[0]        - s[2] + 2 * s[3] - 2 * s[5] + s[6] - s[8];
			normal[1] =   s[0] + 2 * s[1] + s[2]          - s[6] - 2 * s[7]   - s[8];
			normal[2] = s[4] * 4;
			if ( !VectorNormalize2( normal, normal ) ) {
				VectorSet( normal, 0, 0, 1 );
			}
			*outbyte++ = FloatToOffsetByte( normal[0] );
			*outbyte++ = FloatToOffsetByte( normal[1] );
			*outbyte++ = FloatToOffsetByte( normal[2] );
			outbyte++;
		}
	}
}

#define COPYSAMPLE( a,b ) *(unsigned int *)( a ) = *(unsigned int *)( b )

// ===========================================================================
// FUNCTION 10/49: DoFCBI -- REAL (static)
// Fast Curve Based Interpolation
// ===========================================================================
static void DoFCBI( byte *in, byte *out, int width, int height, int component ) {
	int x, y;
	byte *outbyte, *inbyte;

	for (y = 2; y < height - 2; y += 2) {
		inbyte  = in  + ( y * width + 2 ) * 4 + component;
		outbyte = out + ( y * width + 2 ) * 4 + component;
		for (x = 2; x < width - 2; x += 2) { *outbyte = *inbyte; outbyte += 8; inbyte += 8; }
	}

	for (y = 3; y < height - 3; y += 2) {
		byte sa, sb, sc, sd, se, sf, sg, sh, si, sj, sk, sl;
		byte *line1, *line2, *line3, *line4;
		x = 3;
		line1 = in + ( ( y - 3 ) * width + ( x - 1 ) ) * 4 + component;
		line2 = in + ( ( y - 1 ) * width + ( x - 3 ) ) * 4 + component;
		line3 = in + ( ( y + 1 ) * width + ( x - 3 ) ) * 4 + component;
		line4 = in + ( ( y + 3 ) * width + ( x - 1 ) ) * 4 + component;
		sa = *line1; line1 += 8;
		sc = *line2; line2 += 8; sd = *line2; line2 += 8; se = *line2; line2 += 8;
		sg = *line3; line3 += 8; sh = *line3; line3 += 8; si = *line3; line3 += 8;
		sk = *line4; line4 += 8;
		outbyte = out + ( y * width + x ) * 4 + component;
		for ( ; x < width - 3; x += 2) {
			int NWd, NEd, NWp, NEp;
			sb = *line1; line1 += 8;
			sf = *line2; line2 += 8;
			sj = *line3; line3 += 8;
			sl = *line4; line4 += 8;
			NWp = sd + si; NEp = se + sh;
			NWd = abs( sd - si ); NEd = abs( se - sh );
			if ( NWd > 100 || NEd > 100 || abs( NWp - NEp ) > 200 ) {
				*outbyte = ( NWd < NEd ) ? NWp >> 1 : NEp >> 1;
			} else {
				int NWdd = abs( sa + sj - 3 * NWp + sc + sl + NEp );
				int NEdd = abs( sg + sb - 3 * NEp + sk + sf + NWp );
				*outbyte = ( NWdd > NEdd ) ? NWp >> 1 : NEp >> 1;
			}
			outbyte += 8;
			sa = sb; sc = sd; sd = se; se = sf; sg = sh; sh = si; si = sj; sk = sl;
		}
	}

	for (y = 3; y < height - 3; y += 2) {
		inbyte  = out + ( y * width + 3 ) * 4 + component;
		outbyte = in  + ( y * width + 3 ) * 4 + component;
		for (x = 3; x < width - 3; x += 2) { *outbyte = *inbyte; outbyte += 8; inbyte += 8; }
	}

	for (y = 2; y < height - 3; y++) {
		byte sa, sb, sc, sd, se, sf, sg, sh, si, sj, sk, sl;
		byte *line1, *line2, *line3, *line4, *line5;
		x = ( y + 1 ) % 2 + 2;
		line1 = in + ( ( y - 2 ) * width + ( x - 1 ) ) * 4 + component;
		line2 = in + ( ( y - 1 ) * width + ( x - 2 ) ) * 4 + component;
		line3 = in + ( ( y  ) * width + ( x - 1 ) ) * 4 + component;
		line4 = in + ( ( y + 1 ) * width + ( x - 2 ) ) * 4 + component;
		line5 = in + ( ( y + 2 ) * width + ( x - 1 ) ) * 4 + component;
		sa = *line1; line1 += 8;
		sc = *line2; line2 += 8; sd = *line2; line2 += 8;
		sf = *line3; line3 += 8;
		sh = *line4; line4 += 8; si = *line4; line4 += 8;
		sk = *line5; line5 += 8;
		outbyte = out + ( y * width + x ) * 4 + component;
		for ( ; x < width - 3; x += 2) {
			int hd, vd, hp, vp;
			sb = *line1; line1 += 8;
			se = *line2; line2 += 8;
			sg = *line3; line3 += 8;
			sj = *line4; line4 += 8;
			sl = *line5; line5 += 8;
			hp = sf + sg; vp = sd + si;
			hd = abs( sf - sg ); vd = abs( sd - si );
			if ( hd > 100 || vd > 100 || abs( hp - vp ) > 200 ) {
				*outbyte = ( hd < vd ) ? hp >> 1 : vp >> 1;
			} else {
				int hdd = abs( sc + se - 3 * hp + sh + sj + vp );
				int vdd = abs( sa + sk - 3 * vp + sb + sl + hp );
				*outbyte = ( hdd > vdd ) ? hp >> 1 : vp >> 1;
			}
			outbyte += 8;
			sa = sb; sc = sd; sd = se; sf = sg; sh = si; si = sj; sk = sl;
		}
	}
}

// ===========================================================================
// FUNCTION 11/49: DoFCBIQuick -- REAL (static)
// ===========================================================================
static void DoFCBIQuick( byte *in, byte *out, int width, int height, int component ) {
	int x, y;
	byte *outbyte, *inbyte;

	for (y = 2; y < height - 2; y += 2) {
		inbyte  = in  + ( y * width + 2 ) * 4 + component;
		outbyte = out + ( y * width + 2 ) * 4 + component;
		for (x = 2; x < width - 2; x += 2) { *outbyte = *inbyte; outbyte += 8; inbyte += 8; }
	}

	for (y = 3; y < height - 4; y += 2) {
		byte sd, se, sh, si;
		byte *line2, *line3;
		x = 3;
		line2 = in + ( ( y - 1 ) * width + ( x - 1 ) ) * 4 + component;
		line3 = in + ( ( y + 1 ) * width + ( x - 1 ) ) * 4 + component;
		sd = *line2; line2 += 8;
		sh = *line3; line3 += 8;
		outbyte = out + ( y * width + x ) * 4 + component;
		for ( ; x < width - 4; x += 2) {
			int NWd, NEd, NWp, NEp;
			se = *line2; line2 += 8;
			si = *line3; line3 += 8;
			NWp = sd + si; NEp = se + sh;
			NWd = abs( sd - si ); NEd = abs( se - sh );
			*outbyte = ( NWd < NEd ) ? NWp >> 1 : NEp >> 1;
			outbyte += 8;
			sd = se; sh = si;
		}
	}

	for (y = 3; y < height - 3; y += 2) {
		inbyte  = out + ( y * width + 3 ) * 4 + component;
		outbyte = in  + ( y * width + 3 ) * 4 + component;
		for (x = 3; x < width - 3; x += 2) { *outbyte = *inbyte; outbyte += 8; inbyte += 8; }
	}

	for (y = 2; y < height - 3; y++) {
		byte sd, sf, sg, si;
		byte *line2, *line3, *line4;
		x = ( y + 1 ) % 2 + 2;
		line2 = in + ( ( y - 1 ) * width + ( x  ) ) * 4 + component;
		line3 = in + ( ( y  ) * width + ( x - 1 ) ) * 4 + component;
		line4 = in + ( ( y + 1 ) * width + ( x  ) ) * 4 + component;
		outbyte = out + ( y * width + x ) * 4 + component;
		sf = *line3; line3 += 8;
		for ( ; x < width - 3; x += 2) {
			int hd, vd, hp, vp;
			sd = *line2; line2 += 8;
			sg = *line3; line3 += 8;
			si = *line4; line4 += 8;
			hp = sf + sg; vp = sd + si;
			hd = abs( sf - sg ); vd = abs( sd - si );
			*outbyte = ( hd < vd ) ? hp >> 1 : vp >> 1;
			outbyte += 8;
			sf = sg;
		}
	}
}

// ===========================================================================
// FUNCTION 12/49: DoLinear -- REAL (static)
// ===========================================================================
static void DoLinear( byte *in, byte *out, int width, int height ) {
	int x, y, i;
	byte *outbyte, *inbyte;

	for (y = 2; y < height - 2; y += 2) {
		x = 2;
		inbyte  = in  + ( y * width + x ) * 4;
		outbyte = out + ( y * width + x ) * 4;
		for ( ; x < width - 2; x += 2) { COPYSAMPLE( outbyte, inbyte ); outbyte += 8; inbyte += 8; }
	}

	for (y = 1; y < height - 1; y += 2) {
		byte sd[4] = {0}, se[4] = {0}, sh[4] = {0}, si[4] = {0};
		byte *line2, *line3;
		x = 1;
		line2 = in + ( ( y - 1 ) * width + ( x - 1 ) ) * 4;
		line3 = in + ( ( y + 1 ) * width + ( x - 1 ) ) * 4;
		COPYSAMPLE( sd, line2 ); line2 += 8;
		COPYSAMPLE( sh, line3 ); line3 += 8;
		outbyte = out + ( y * width + x ) * 4;
		for ( ; x < width - 1; x += 2) {
			COPYSAMPLE( se, line2 ); line2 += 8;
			COPYSAMPLE( si, line3 ); line3 += 8;
			for (i = 0; i < 4; i++) *outbyte++ = ( sd[i] + si[i] + se[i] + sh[i] ) >> 2;
			outbyte += 4;
			COPYSAMPLE( sd, se ); COPYSAMPLE( sh, si );
		}
	}

	for (y = 1; y < height - 1; y += 2) {
		x = 1;
		inbyte  = out + ( y * width + x ) * 4;
		outbyte = in  + ( y * width + x ) * 4;
		for ( ; x < width - 1; x += 2) { COPYSAMPLE( outbyte, inbyte ); outbyte += 8; inbyte += 8; }
	}

	for (y = 1; y < height - 1; y++) {
		byte sd[4], sf[4], sg[4], si[4];
		byte *line2, *line3, *line4;
		x = y % 2 + 1;
		line2 = in + ( ( y - 1 ) * width + ( x  ) ) * 4;
		line3 = in + ( ( y  ) * width + ( x - 1 ) ) * 4;
		line4 = in + ( ( y + 1 ) * width + ( x  ) ) * 4;
		COPYSAMPLE( sf, line3 ); line3 += 8;
		outbyte = out + ( y * width + x ) * 4;
		for ( ; x < width - 1; x += 2) {
			COPYSAMPLE( sd, line2 ); line2 += 8;
			COPYSAMPLE( sg, line3 ); line3 += 8;
			COPYSAMPLE( si, line4 ); line4 += 8;
			for (i = 0; i < 4; i++) *outbyte++ = ( sf[i] + sg[i] + sd[i] + si[i] ) >> 2;
			outbyte += 4;
			COPYSAMPLE( sf, sg );
		}
	}
}

// ===========================================================================
// FUNCTION 13/49: ExpandHalfTextureToGrid -- REAL (static)
// ===========================================================================
static void ExpandHalfTextureToGrid( byte *data, int width, int height ) {
	int x, y;
	for (y = height / 2; y > 0; y--) {
		byte *outbyte = data + ( ( y * 2 - 1 ) * ( width    ) - 2 ) * 4;
		byte *inbyte  = data + ( y      * ( width / 2 ) - 1 ) * 4;
		for (x = width / 2; x > 0; x--) {
			COPYSAMPLE( outbyte, inbyte );
			outbyte -= 8; inbyte -= 4;
		}
	}
}

// ===========================================================================
// FUNCTION 14/49: FillInNormalizedZ -- REAL (static)
// ===========================================================================
static void FillInNormalizedZ( const byte *in, byte *out, int width, int height ) {
	int x, y;
	for (y = 0; y < height; y++) {
		const byte *inbyte  = in  + y * width * 4;
		byte       *outbyte = out + y * width * 4;
		for (x = 0; x < width; x++) {
			byte nx, ny, nz, h;
			float fnx, fny, fll, fnz;
			nx = *inbyte++; ny = *inbyte++; inbyte++; h = *inbyte++;
			fnx = OffsetByteToFloat( nx );
			fny = OffsetByteToFloat( ny );
			fll = 1.0f - fnx * fnx - fny * fny;
			fnz = ( fll >= 0.0f ) ? (float)sqrt( fll ) : 0.0f;
			nz = FloatToOffsetByte( fnz );
			*outbyte++ = nx; *outbyte++ = ny; *outbyte++ = nz; *outbyte++ = h;
		}
	}
}

#define WORKBLOCK_SIZE     128
#define WORKBLOCK_BORDER   4
#define WORKBLOCK_REALSIZE ( WORKBLOCK_SIZE + WORKBLOCK_BORDER * 2 )

// ===========================================================================
// FUNCTION 15/49: FCBIByBlock -- REAL (static)
// ===========================================================================
static void FCBIByBlock( byte *data, int width, int height, qboolean clampToEdge, qboolean normalized ) {
	byte workdata[WORKBLOCK_REALSIZE * WORKBLOCK_REALSIZE * 4];
	byte outdata[WORKBLOCK_REALSIZE * WORKBLOCK_REALSIZE * 4];
	byte *inbyte, *outbyte;
	int x, y, srcx, srcy;

	ExpandHalfTextureToGrid( data, width, height );

	for (y = 0; y < height; y += WORKBLOCK_SIZE) {
		for (x = 0; x < width; x += WORKBLOCK_SIZE) {
			int x2, y2;
			int workwidth  = MIN( WORKBLOCK_SIZE, width  - x );
			int workheight = MIN( WORKBLOCK_SIZE, height - y );
			int fullworkwidth  = workwidth  + WORKBLOCK_BORDER * 2;
			int fullworkheight = workheight + WORKBLOCK_BORDER * 2;

			for (y2 = 0; y2 < fullworkheight; y2 += 2) {
				srcy = y + y2 - WORKBLOCK_BORDER;
				srcy = clampToEdge ? CLAMP( srcy,0,height - 2 ) : ( srcy + height ) % height;
				outbyte = workdata + y2   * fullworkwidth * 4;
				inbyte  = data     + srcy * width         * 4;
				for (x2 = 0; x2 < fullworkwidth; x2 += 2) {
					srcx = x + x2 - WORKBLOCK_BORDER;
					srcx = clampToEdge ? CLAMP( srcx,0,width - 2 ) : ( srcx + width ) % width;
					COPYSAMPLE( outbyte, inbyte + srcx * 4 );
					outbyte += 8;
				}
			}

			DoLinear( workdata, outdata, fullworkwidth, fullworkheight );

			if ( !normalized ) {
				switch ( r_imageUpsampleType->integer ) {
				case 1: DoFCBIQuick( workdata,outdata,fullworkwidth,fullworkheight,0 ); break;
				case 2: default: DoFCBI( workdata,outdata,fullworkwidth,fullworkheight,0 ); break;
				case 0: break;
				}
			} else {
				switch ( r_imageUpsampleType->integer ) {
				case 1:
					DoFCBIQuick( workdata,outdata,fullworkwidth,fullworkheight,0 );
					DoFCBIQuick( workdata,outdata,fullworkwidth,fullworkheight,1 ); break;
				case 2: default:
					DoFCBI( workdata,outdata,fullworkwidth,fullworkheight,0 );
					DoFCBI( workdata,outdata,fullworkwidth,fullworkheight,1 ); break;
				case 0: break;
				}
			}

			for (y2 = 0; y2 < workheight; y2++) {
				inbyte  = outdata + ( ( y2 + WORKBLOCK_BORDER ) * fullworkwidth + WORKBLOCK_BORDER ) * 4;
				outbyte = data    + ( ( y + y2 ) * width + x ) * 4;
				for (x2 = 0; x2 < workwidth; x2++) {
					COPYSAMPLE( outbyte, inbyte ); outbyte += 4; inbyte += 4;
				}
			}
		}
	}
}
#undef COPYSAMPLE

// ===========================================================================
// FUNCTION 16/49: R_LightScaleTexture -- REAL
// ===========================================================================
void R_LightScaleTexture( byte *in, int inwidth, int inheight, qboolean only_gamma ) {
	if ( only_gamma ) {
		if ( !glConfig.deviceSupportsGamma ) {
			int i, c;
			byte *p = in;
			c = inwidth * inheight;
			for (i = 0; i < c; i++, p += 4) {
				p[0] = s_gammatable[p[0]];
				p[1] = s_gammatable[p[1]];
				p[2] = s_gammatable[p[2]];
			}
		}
	} else {
		int i, c;
		byte *p = in;
		c = inwidth * inheight;
		if ( glConfig.deviceSupportsGamma ) {
			for (i = 0; i < c; i++, p += 4) {
				p[0] = s_intensitytable[p[0]];
				p[1] = s_intensitytable[p[1]];
				p[2] = s_intensitytable[p[2]];
			}
		} else {
			for (i = 0; i < c; i++, p += 4) {
				p[0] = s_gammatable[s_intensitytable[p[0]]];
				p[1] = s_gammatable[s_intensitytable[p[1]]];
				p[2] = s_gammatable[s_intensitytable[p[2]]];
			}
		}
	}
}

// ===========================================================================
// FUNCTION 17/49: R_MipMapsRGB -- REAL (static)
// ===========================================================================
static void R_MipMapsRGB( byte *in, int inWidth, int inHeight ) {
	int x, y, c, stride;
	const byte *in2;
	float total;
	static float downmipSrgbLookup[256];
	static int downmipSrgbLookupSet = 0;
	byte  *out = in;

	if ( !downmipSrgbLookupSet ) {
		for (x = 0; x < 256; x++)
			downmipSrgbLookup[x] = powf( x / 255.0f, 2.2f ) * 0.25f;
		downmipSrgbLookupSet = 1;
	}

	if ( inWidth == 1 && inHeight == 1 ) {
		return;
	}

	if ( inWidth == 1 || inHeight == 1 ) {
		for (x = ( inWidth * inHeight ) >> 1; x; x--) {
			for (c = 3; c; c--, in++) {
				total = ( downmipSrgbLookup[*( in )] + downmipSrgbLookup[*( in + 4 )] ) * 2.0f;
				*out++ = (byte)( powf( total, 1.0f / 2.2f ) * 255.0f );
			}
			*out++ = (*( in ) + *( in + 4 ) ) >> 1; in += 5;
		}
		return;
	}

	stride = inWidth * 4;
	inWidth >>= 1; inHeight >>= 1;
	in2 = in + stride;
	for (y = inHeight; y; y--, in += stride, in2 += stride) {
		for (x = inWidth; x; x--) {
			for (c = 3; c; c--, in++, in2++) {
				total = downmipSrgbLookup[*( in )]   + downmipSrgbLookup[*( in + 4 )]
						+ downmipSrgbLookup[*( in2 )]  + downmipSrgbLookup[*( in2 + 4 )];
				*out++ = (byte)( powf( total, 1.0f / 2.2f ) * 255.0f );
			}
			*out++ = (*( in ) + *( in + 4 ) + *( in2 ) + *( in2 + 4 ) ) >> 2; in += 5; in2 += 5;
		}
	}
}

// ===========================================================================
// FUNCTION 18/49: R_MipMapNormalHeight -- REAL (static)
// ===========================================================================
static void R_MipMapNormalHeight( const byte *in, byte *out, int width, int height, qboolean swizzle ) {
	int i, j, row;
	int sx = swizzle ? 3 : 0;
	int sa = swizzle ? 0 : 3;

	if ( width == 1 && height == 1 ) {
		return;
	}

	row = width * 4;
	width >>= 1; height >>= 1;

	for (i = 0; i < height; i++, in += row) {
		for (j = 0; j < width; j++, out += 4, in += 8) {
			vec3_t v;
			v[0] = OffsetByteToFloat( in[sx    ] ) + OffsetByteToFloat( in[sx  + 4] )
				   + OffsetByteToFloat( in[sx + row] ) + OffsetByteToFloat( in[sx + row + 4] );
			v[1] = OffsetByteToFloat( in[     1] ) + OffsetByteToFloat( in[     5] )
				   + OffsetByteToFloat( in[ row + 1] ) + OffsetByteToFloat( in[ row + 5] );
			v[2] = OffsetByteToFloat( in[     2] ) + OffsetByteToFloat( in[     6] )
				   + OffsetByteToFloat( in[ row + 2] ) + OffsetByteToFloat( in[ row + 6] );
			VectorNormalizeFast( v );
			out[sx] = FloatToOffsetByte( v[0] );
			out[1 ] = FloatToOffsetByte( v[1] );
			out[2 ] = FloatToOffsetByte( v[2] );
			out[sa] = MAX( MAX( in[sa], in[sa + 4] ), MAX( in[sa + row], in[sa + row + 4] ) );
		}
	}
}

// ===========================================================================
// FUNCTION 19/49: R_BlendOverTexture -- REAL (static)
// ===========================================================================
static void R_BlendOverTexture( byte *data, int pixelCount, byte blend[4] ) {
	int i;
	int inverseAlpha = 255 - blend[3];
	int premult[3];
	premult[0] = blend[0] * blend[3];
	premult[1] = blend[1] * blend[3];
	premult[2] = blend[2] * blend[3];
	for (i = 0; i < pixelCount; i++, data += 4) {
		data[0] = ( data[0] * inverseAlpha + premult[0] ) >> 9;
		data[1] = ( data[1] * inverseAlpha + premult[1] ) >> 9;
		data[2] = ( data[2] * inverseAlpha + premult[2] ) >> 9;
	}
}

// Type mapping: GLenum = unsigned int (GL-compat typedef in tr_local.h)
#define ROW_PADDING( width,bpp,alignment ) ( PAD( ( width ) * ( bpp ),( alignment ) ) - ( width ) * ( bpp ) )

// ===========================================================================
// FUNCTION 20/49: R_ConvertTextureFormat -- REAL
// ===========================================================================
void R_ConvertTextureFormat( const byte *in, int width, int height, GLenum format, GLenum type, byte *out ) {
	int x, y, rowPadding;
	int unpackAlign = 4; // matches GL_UNPACK_ALIGNMENT default

	if ( format == GL_RGB && type == GL_UNSIGNED_BYTE ) {
		rowPadding = ROW_PADDING( width, 3, unpackAlign );
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				*out++ = *in++; *out++ = *in++; *out++ = *in++; in++;
			}
			out += rowPadding;
		}
	} else if ( format == GL_LUMINANCE && type == GL_UNSIGNED_BYTE ) {
		rowPadding = ROW_PADDING( width, 1, unpackAlign );
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) { *out++ = *in++; in += 3; }
			out += rowPadding;
		}
	} else if ( format == GL_LUMINANCE_ALPHA && type == GL_UNSIGNED_BYTE ) {
		rowPadding = ROW_PADDING( width, 2, unpackAlign );
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++) {
				*out++ = *in++; in += 2; *out++ = *in++;
			}
			out += rowPadding;
		}
	} else if ( format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5 ) {
		rowPadding = ROW_PADDING( width, 2, unpackAlign );
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++, in += 4, out += 2) {
				*( (unsigned short*)out ) = ( (unsigned short)( in[0] >> 3 ) << 11 )
											| ( (unsigned short)( in[1] >> 2 ) << 5 )
											| ( (unsigned short)( in[2] >> 3 ) << 0 );
			}
			out += rowPadding;
		}
	} else if ( format == GL_RGBA && type == GL_UNSIGNED_SHORT_4_4_4_4 ) {
		rowPadding = ROW_PADDING( width, 2, unpackAlign );
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++, in += 4, out += 2) {
				*( (unsigned short*)out ) = ( (unsigned short)( in[0] >> 4 ) << 12 )
											| ( (unsigned short)( in[1] >> 4 ) << 8 )
											| ( (unsigned short)( in[2] >> 4 ) << 4 )
											| ( (unsigned short)( in[3] >> 4 ) << 0 );
			}
			out += rowPadding;
		}
	} else {
		ri.Error( ERR_DROP, "Unable to convert RGBA image to OpenGL format 0x%X and type 0x%X", format, type );
	}
}

// ===========================================================================
// FUNCTION 21/49: RawImage_SwizzleRA -- REAL (static)
// ===========================================================================
static void RawImage_SwizzleRA( byte *data, int width, int height ) {
	int i;
	byte *ptr = data, swap;
	for (i = 0; i < width * height; i++, ptr += 4) {
		swap = ptr[0]; ptr[0] = ptr[3]; ptr[3] = swap;
	}
}

// ===========================================================================
// FUNCTION 22/49: RawImage_ScaleToPower2 -- REAL (static)
// ===========================================================================
static qboolean RawImage_ScaleToPower2( byte **data, int *inout_width, int *inout_height,
										imgType_t type, imgFlags_t flags, byte **resampledBuffer ) {
	int width  = *inout_width;
	int height = *inout_height;
	int scaled_width, scaled_height;
	qboolean picmip      = ( qboolean ) !!( flags & IMGFLAG_PICMIP );
	qboolean mipmap      = ( qboolean ) !!( flags & IMGFLAG_MIPMAP );
	qboolean clampToEdge = ( qboolean ) !!( flags & IMGFLAG_CLAMPTOEDGE );
	qboolean scaled;
#if 0
	static int rmse_saved = 0;

	// do the root mean square error stuff first
	if ( r_rmse->value ) {
		while ( R_RMSE( *data, width, height ) < r_rmse->value ) {
			rmse_saved += ( height * width * 4 ) - ( ( width >> 1 ) * ( height >> 1 ) * 4 );
			*resampledBuffer = ri.Hunk_AllocateTempMemory( ( width >> 1 ) * ( height >> 1 ) * 4 );
			ResampleTexture( *data, width, height, *resampledBuffer, width >> 1, height >> 1 );
			*data = *resampledBuffer;
			width = width >> 1;
			height = height >> 1;
			ri.Printf( PRINT_ALL, "r_rmse of %f has saved %dkb\n", r_rmse->value, ( rmse_saved / 1024 ) );
		}
	}
#endif

	if ( !mipmap ) {
		scaled_width  = width;
		scaled_height = height;
	} else {
		scaled_width  = NextPowerOfTwo( width );
		scaled_height = NextPowerOfTwo( height );
	}

	if ( r_roundImagesDown->integer && scaled_width  > width ) {
		scaled_width  >>= 1;
	}
	if ( r_roundImagesDown->integer && scaled_height > height ) {
		scaled_height >>= 1;
	}

	if ( picmip && data && resampledBuffer && r_imageUpsample->integer &&
		 scaled_width  < r_imageUpsampleMaxSize->integer &&
		 scaled_height < r_imageUpsampleMaxSize->integer ) {
		int finalwidth  = scaled_width << r_imageUpsample->integer;
		int finalheight = scaled_height << r_imageUpsample->integer;
		while ( finalwidth  > r_imageUpsampleMaxSize->integer ||
				finalheight > r_imageUpsampleMaxSize->integer ) { finalwidth >>= 1; finalheight >>= 1; }
		while ( finalwidth  > glConfig.maxTextureSize ||
				finalheight > glConfig.maxTextureSize ) { finalwidth >>= 1; finalheight >>= 1; }

		*resampledBuffer = (byte*)ri.Hunk_AllocateTempMemory( finalwidth * finalheight * 4 );

		if ( scaled_width != width || scaled_height != height ) {
			ResampleTexture(*data, width, height, *resampledBuffer, scaled_width, scaled_height );
		} else {
			Com_Memcpy(*resampledBuffer, *data, width * height * 4 );
		}

		if ( type == IMGTYPE_COLORALPHA ) {
			RGBAtoYCoCgA(*resampledBuffer, *resampledBuffer, scaled_width, scaled_height );
		}

		while ( scaled_width < finalwidth || scaled_height < finalheight ) {
			scaled_width <<= 1; scaled_height <<= 1;
			FCBIByBlock(*resampledBuffer, scaled_width, scaled_height, clampToEdge,
						(qboolean)( type == IMGTYPE_NORMAL || type == IMGTYPE_NORMALHEIGHT ) );
		}

		if ( type == IMGTYPE_COLORALPHA ) {
			YCoCgAtoRGBA(*resampledBuffer, *resampledBuffer, scaled_width, scaled_height );
		} else if ( type == IMGTYPE_NORMAL || type == IMGTYPE_NORMALHEIGHT ) {
			FillInNormalizedZ(*resampledBuffer, *resampledBuffer, scaled_width, scaled_height );
		}

		*data = *resampledBuffer;
	} else if ( scaled_width != width || scaled_height != height )     {
		if ( data && resampledBuffer ) {
			*resampledBuffer = (byte*)ri.Hunk_AllocateTempMemory( scaled_width * scaled_height * 4 );
			ResampleTexture(*data, width, height, *resampledBuffer, scaled_width, scaled_height );
			*data = *resampledBuffer;
		}
	}

	width = scaled_width; height = scaled_height;

	if ( picmip ) {
		scaled_width >>= r_picmip->integer; scaled_height >>= r_picmip->integer;
	}

	while ( scaled_width > glConfig.maxTextureSize || scaled_height > glConfig.maxTextureSize ) {
		scaled_width >>= 1; scaled_height >>= 1;
	}

	scaled_width  = MAX( 1, scaled_width );
	scaled_height = MAX( 1, scaled_height );
	scaled = (qboolean)( ( width != scaled_width ) || ( height != scaled_height ) ); // D2: bool->qboolean

	if ( data ) {
		while ( width > scaled_width || height > scaled_height ) {
			if ( type == IMGTYPE_NORMAL || type == IMGTYPE_NORMALHEIGHT ) {
				R_MipMapNormalHeight(*data, *data, width, height, qfalse );
			} else {
				R_MipMapsRGB(*data, width, height );
			}
			width  = MAX( 1, width  >> 1 );
			height = MAX( 1, height >> 1 );
		}
	}

	*inout_width  = width;
	*inout_height = height;
	return scaled;
}

// ===========================================================================
// FUNCTION 23/49: RawImage_HasAlpha -- REAL (static)
// ===========================================================================
static qboolean RawImage_HasAlpha( const byte *scan, int numPixels ) {
	int i;
	if ( !scan ) {
		return qtrue;
	}
	for (i = 0; i < numPixels; i++) {
		if ( scan[i * 4 + 3] != 255 ) {
			return qtrue;
		}
	}
	return qfalse;
}

// ===========================================================================
// FUNCTION 24/49: RawImage_GetFormat -- REAL (static)
// ===========================================================================
static GLenum RawImage_GetFormat( const byte *data, int numPixels, GLenum picFormat,
								  qboolean lightMap, imgType_t type, imgFlags_t flags ) {
	int samples = 3;
	GLenum internalFormat = GL_RGB;
	qboolean forceNoCompression = ( qboolean ) !!( flags & IMGFLAG_NO_COMPRESSION );
	qboolean normalmap = (qboolean)( ( type == IMGTYPE_NORMAL || type == IMGTYPE_NORMALHEIGHT ) );

	if ( picFormat != GL_RGBA8 ) {
		return picFormat;
	}

	if ( normalmap ) {
		if ( ( type == IMGTYPE_NORMALHEIGHT ) && RawImage_HasAlpha( data, numPixels ) && r_parallaxMapping->integer ) {
			if ( !forceNoCompression && ( glRefConfig.textureCompression & TCR_BPTC ) ) {
				internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
			} else if ( !forceNoCompression && glConfig.textureCompression == TC_S3TC_ARB ) {
				internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			} else if ( r_texturebits->integer == 16 ) {
				internalFormat = GL_RGBA4;
			} else if ( r_texturebits->integer == 32 )                                                               {
				internalFormat = GL_RGBA8;
			} else { internalFormat = GL_RGBA;}
		} else {
			if ( !forceNoCompression && ( glRefConfig.textureCompression & TCR_RGTC ) ) {
				internalFormat = GL_COMPRESSED_RG_RGTC2;
			} else if ( !forceNoCompression && ( glRefConfig.textureCompression & TCR_BPTC ) ) {
				internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
			} else if ( !forceNoCompression && glConfig.textureCompression == TC_S3TC_ARB ) {
				internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			} else if ( r_texturebits->integer == 16 ) {
				internalFormat = GL_RGB5;
			} else if ( r_texturebits->integer == 32 )                                                              {
				internalFormat = GL_RGB8;
			} else { internalFormat = GL_RGB;}
		}
	} else if ( lightMap ) {
		internalFormat = GL_RGBA;
	} else {
		if ( RawImage_HasAlpha( data, numPixels ) ) {
			samples = 4;
		}
		if ( samples == 3 ) {
			if ( !forceNoCompression && ( glRefConfig.textureCompression & TCR_BPTC ) ) {
				internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
			} else if ( !forceNoCompression && glConfig.textureCompression == TC_S3TC_ARB ) {
				internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
			} else if ( !forceNoCompression && glConfig.textureCompression == TC_S3TC ) {
				internalFormat = GL_RGB4_S3TC;
			} else if ( r_texturebits->integer == 16 ) {
				internalFormat = GL_RGB5;
			} else if ( r_texturebits->integer == 32 )                                                              {
				internalFormat = GL_RGB8;
			} else { internalFormat = GL_RGB;}
		} else {
			if ( !forceNoCompression && ( glRefConfig.textureCompression & TCR_BPTC ) ) {
				internalFormat = GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
			} else if ( !forceNoCompression && glConfig.textureCompression == TC_S3TC_ARB ) {
				internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			} else if ( r_texturebits->integer == 16 ) {
				internalFormat = GL_RGBA4;
			} else if ( r_texturebits->integer == 32 )                                                               {
				internalFormat = GL_RGBA8;
			} else { internalFormat = GL_RGBA;}
		}
	}
	return internalFormat;
}

// ===========================================================================
// FUNCTION 25/49: CompressMonoBlock -- REAL (static)
// ===========================================================================
static void CompressMonoBlock( byte outdata[8], const byte indata[16] ) {
	int hi, lo, diff, bias, outbyte, shift, i;
	byte *p = outdata;

	hi = lo = indata[0];
	for (i = 1; i < 16; i++) { hi = MAX( indata[i], hi ); lo = MIN( indata[i], lo ); }
	*p++ = hi; *p++ = lo;
	diff = hi - lo;
	if ( diff == 0 ) {
		outbyte = ( hi == 255 ) ? 255 : 0;
		for (i = 0; i < 6; i++) *p++ = outbyte;
		return;
	}
	bias = diff / 2 - lo * 7;
	outbyte = shift = 0;
	for (i = 0; i < 16; i++) {
		const byte fixIndex[8] = { 1, 7, 6, 5, 4, 3, 2, 0 };
		byte index = fixIndex[( indata[i] * 7 + bias ) / diff];
		outbyte |= index << shift;
		shift += 3;
		if ( shift >= 8 ) {
			*p++ = outbyte & 0xff; shift -= 8; outbyte >>= 8;
		}
	}
}

// ===========================================================================
// FUNCTION 26/49: RawImage_UploadToRgtc2Texture
//   CPU RGTC2 block compression: REAL
//   GPU upload: PARITY-SEAM(Metal-device)
// ===========================================================================
static void RawImage_UploadToRgtc2Texture( GLuint texture, int miplevel, int x, int y,
										   int width, int height, byte *data ) {
	int wBlocks, hBlocks, iy, ix, size;
	byte *compressedData, *p;

	wBlocks = ( width + 3 ) / 4; hBlocks = ( height + 3 ) / 4;
	size = wBlocks * hBlocks * 16;

	// CPU RGTC2 compression -- REAL
	p = compressedData = (byte*)ri.Hunk_AllocateTempMemory( size );
	for (iy = 0; iy < height; iy += 4) {
		int oh = MIN( 4, height - iy );
		for (ix = 0; ix < width; ix += 4) {
			byte workingData[16];
			int component, ox, oy;
			int ow = MIN( 4, width - ix );
			for (component = 0; component < 2; component++) {
				for (oy = 0; oy < oh; oy++)
					for (ox = 0; ox < ow; ox++)
						workingData[oy * 4 + ox] = data[( ( iy + oy ) * width + ix + ox ) * 4 + component];
				for (oy = 0; oy < 4; oy++)
					for (ox = ( oy < oh ) ? ow : 0; ox < 4; ox++)
						workingData[oy * 4 + ox] = workingData[( oy % oh ) * 4 + ox % ow];
				CompressMonoBlock( p, workingData );
				p += 8;
			}
		}
	}

	// PARITY-SEAM(Metal-device): qglCompressedTextureSubImage2DEXT would upload
	// RGTC2-compressed blocks to GPU texture `texture` mip `miplevel`.
	// Under Metal: MTLTexture replaceRegion:... with GL_COMPRESSED_RG_RGTC2 layout.
	// Pending Metal device bring-up. CPU-compressed data available in compressedData.
	(void)texture; (void)miplevel; (void)x; (void)y; (void)size; (void)compressedData;

	ri.Hunk_FreeTempMemory( compressedData );
}

// ===========================================================================
// FUNCTION 27/49: CalculateMipSize -- REAL (static)
// ===========================================================================
static int CalculateMipSize( int width, int height, GLenum picFormat ) {
	int numBlocks = ( ( width + 3 ) / 4 ) * ( ( height + 3 ) / 4 );
	int numPixels = width * height;
	switch ( picFormat ) {
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
	case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
	case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RED_RGTC1:
	case GL_COMPRESSED_SIGNED_RED_RGTC1:
		return numBlocks * 8;
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
	case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
	case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
	case GL_COMPRESSED_RG_RGTC2:
	case GL_COMPRESSED_SIGNED_RG_RGTC2:
	case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB:
	case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB:
	case GL_COMPRESSED_RGBA_BPTC_UNORM_ARB:
	case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB:
		return numBlocks * 16;
	case GL_RGBA8:
	case GL_SRGB8_ALPHA8_EXT:
		return numPixels * 4;
	case GL_RGBA16:
		return numPixels * 8;
	default:
		ri.Printf( PRINT_ALL, "Unsupported texture format %08x\n", picFormat );
		return 0;
	}
	return 0;
}

// ===========================================================================
// FUNCTION 28/49: PixelDataFormatFromInternalFormat -- REAL (static)
// ===========================================================================
static GLenum PixelDataFormatFromInternalFormat( GLenum internalFormat ) {
	switch ( internalFormat ) {
	case GL_DEPTH_COMPONENT:
	case GL_DEPTH_COMPONENT16_ARB:
	case GL_DEPTH_COMPONENT24_ARB:
	case GL_DEPTH_COMPONENT32_ARB:
		return GL_DEPTH_COMPONENT;
	default:
		return GL_RGBA;
	}
}

static qboolean Metal_IsDepthInternalFormat( GLenum internalFormat ) {
	return (qboolean)( internalFormat == GL_DEPTH_COMPONENT ||
					   internalFormat == GL_DEPTH_COMPONENT16 || internalFormat == GL_DEPTH_COMPONENT16_ARB ||
					   internalFormat == GL_DEPTH_COMPONENT24 || internalFormat == GL_DEPTH_COMPONENT24_ARB ||
					   internalFormat == GL_DEPTH_COMPONENT32 || internalFormat == GL_DEPTH_COMPONENT32_ARB );
}

static MTL::PixelFormat Metal_PixelFormatFromInternalFormat( GLenum internalFormat ) {
	if ( Metal_IsDepthInternalFormat( internalFormat ) ) {
		return MTL::PixelFormatDepth32Float;
	}
	if ( internalFormat == GL_RGBA16F || internalFormat == GL_RGBA16F_ARB ) {
		return MTL::PixelFormatRGBA16Float;
	}
	if ( internalFormat == GL_R32F ) {
		return MTL::PixelFormatR32Float;
	}

	switch ( internalFormat ) {
	case GL_SRGB_ALPHA_EXT:
	case GL_SRGB8_ALPHA8_EXT:
		return MTL::PixelFormatRGBA8Unorm_sRGB;
	case GL_LUMINANCE:
	case GL_LUMINANCE8:
		return MTL::PixelFormatR8Unorm;
	case GL_LUMINANCE_ALPHA:
	case GL_LUMINANCE8_ALPHA8:
		return MTL::PixelFormatRG8Unorm;
	// GL2 ref: tr_image.c:1966, tr_image_dds.c:216 — DDS/GL2 BCx tokens
	// become Metal block-compressed formats; tr_metal_device.mm computes
	// bytesPerRow as ceil(width/4) * blockBytes for each uploaded mip.
	case GL_RGB4_S3TC:
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		return MTL::PixelFormatBC1_RGBA;
	case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
	case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
		return MTL::PixelFormatBC1_RGBA_sRGB;
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		return MTL::PixelFormatBC2_RGBA;
	case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
		return MTL::PixelFormatBC2_RGBA_sRGB;
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		return MTL::PixelFormatBC3_RGBA;
	case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
		return MTL::PixelFormatBC3_RGBA_sRGB;
	case GL_COMPRESSED_RED_RGTC1:
		return MTL::PixelFormatBC4_RUnorm;
	case GL_COMPRESSED_SIGNED_RED_RGTC1:
		return MTL::PixelFormatBC4_RSnorm;
	case GL_COMPRESSED_RG_RGTC2:
		return MTL::PixelFormatBC5_RGUnorm;
	case GL_COMPRESSED_SIGNED_RG_RGTC2:
		return MTL::PixelFormatBC5_RGSnorm;
	case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB:
		return MTL::PixelFormatBC6H_RGBUfloat;
	case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB:
		return MTL::PixelFormatBC6H_RGBFloat;
	case GL_COMPRESSED_RGBA_BPTC_UNORM_ARB:
		return MTL::PixelFormatBC7_RGBAUnorm;
	case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB:
		return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
	default:
		return MTL::PixelFormatRGBA8Unorm;
	}
}

static qboolean Metal_IsRenderImage( const char *name, const byte *pic ) {
	if ( !name ) {
		return qfalse;
	}
	if ( pic == NULL && !strcmp( name, "_render" ) ) {
		return qtrue;
	}

	return (qboolean)( !strcmp( name, "screenScratch" ) ||
					   !strcmp( name, "*hdrDepth" ) ||
					   !strcmp( name, "*sunRays" ) ||
					   !strncmp( name, "*textureScratch", 15 ) ||
					   !strcmp( name, "*calcLevels" ) ||
					   !strcmp( name, "*targetLevels" ) ||
					   !strncmp( name, "*quarter", 8 ) ||
					   !strcmp( name, "*screenShadow" ) ||
					   !strcmp( name, "*screenSsao" ) );
}

static qboolean Metal_IsRenderDepthImage( const char *name, const byte *pic ) {
	return (qboolean)( pic == NULL && name && !strcmp( name, "*renderdepth" ) );
}

static qboolean Metal_IsSsaoImage( const char *name, const byte *pic ) {
	return (qboolean)( pic == NULL && name && !strcmp( name, "*screenSsao" ) );
}

// GL2 ref: tr_image.c:2944 (pshadow maps), tr_image.c:2953 (sun shadow maps).
// Shadow depth maps are rendered into as depth-only render targets during the
// shadow capture pass, then sampled as depth2d textures by the consumer shader.
// They require both RenderTarget (for depth-only FBO attachment) and ShaderRead.
static qboolean Metal_IsShadowDepthImage( const char *name ) {
	if ( !name ) {
		return qfalse;
	}
	// "*shadowmap%i" — per-object pshadow maps (tr_image.cpp:2327)
	if ( !strncmp( name, "*shadowmap", 10 ) ) {
		return qtrue;
	}
	// "*sunshadowdepth%i" — sun cascade shadow maps (tr_image.cpp:2335, E6)
	if ( !strncmp( name, "*sunshadowdepth", 15 ) ) {
		return qtrue;
	}
	return qfalse;
}

static MTL::TextureUsage Metal_TextureUsageForImage( const char *name, const byte *pic ) {
	// SSAO output: compute kernel writes it, then it is sampled during lighting.
	// ShaderWrite required for compute store; ShaderRead required for subsequent sample.
	// Parity deviation: Apple SAO compute kernel (Dallas SSAO deviation decision, 2026-06-07).
	if ( Metal_IsSsaoImage( name, pic ) ) {
		return MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
	}
	if ( Metal_IsRenderImage( name, pic ) ) {
		return MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
	}
	if ( Metal_IsRenderDepthImage( name, pic ) ) {
		// SAO compute kernel reads scene depth; add ShaderRead alongside RenderTarget.
		// Parity deviation: Apple SAO compute kernel (Dallas SSAO deviation decision, 2026-06-07).
		return MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
	}
	// GL2 ref: tr_fbo.c:160-163 (pshadow/sun FBOs attach depth textures as GL_DEPTH_ATTACHMENT).
	// Shadow depth maps need RenderTarget for the depth-only capture pass.
	if ( Metal_IsShadowDepthImage( name ) ) {
		return MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
	}
	return MTL::TextureUsageShaderRead;
}

// ===========================================================================
// FUNCTION 29/49: RawImage_UploadTexture
//   CPU mip loop: REAL; GPU uploads: PARITY-SEAM(Metal-device)
// ===========================================================================
static void RawImage_UploadTexture( GLuint texture, byte *data, int x, int y,
									int width, int height, GLenum target,
									GLenum picFormat, GLenum dataFormat, GLenum dataType,
									int numMips, GLenum internalFormat,
									imgType_t type, imgFlags_t flags, qboolean subtexture ) {
	qboolean rgtc  = (qboolean)( internalFormat == GL_COMPRESSED_RG_RGTC2 );
	qboolean rgba8 = (qboolean)( ( picFormat == GL_RGBA8 || picFormat == GL_SRGB8_ALPHA8_EXT ) );
	qboolean rgba  = (qboolean)( rgba8 || ( picFormat == GL_RGBA16 ) );
	qboolean mipmap = ( qboolean ) !!( flags & IMGFLAG_MIPMAP );
	int size, miplevel;
	qboolean lastMip = qfalse;
	byte    *formatBuffer = NULL;
	(void)subtexture;

	// qglesMajorVersion == 0 under Metal; this block never entered
	if ( qglesMajorVersion && rgba8 && ( dataFormat != GL_RGBA || dataType != GL_UNSIGNED_BYTE ) ) {
		formatBuffer = (byte*)ri.Hunk_AllocateTempMemory( 4 * width * height );
	}

	miplevel = 0;
	do {
		lastMip = (qboolean)( ( width == 1 && height == 1 ) || !mipmap );
		size    = CalculateMipSize( width, height, picFormat );

		if ( !rgba ) {
			// PARITY-SEAM(Metal-device): qglCompressedTextureSubImage2DEXT seamed.
			(void)target; (void)size; (void)texture;
		} else {
			if ( rgba8 && miplevel != 0 && r_colorMipLevels->integer ) {
				R_BlendOverTexture( data, width * height, mipBlendColors[miplevel] );
			}
			if ( rgba8 && rgtc ) {
				// CPU compression REAL; GPU upload seamed inside
				RawImage_UploadToRgtc2Texture( texture, miplevel, x, y, width, height, data );
			} else if ( formatBuffer ) {
				R_ConvertTextureFormat( data, width, height, dataFormat, dataType, formatBuffer );
				// PARITY-SEAM(Metal-device): qglTextureSubImage2DEXT (converted) seamed.
				(void)formatBuffer;
			} else {
				// PARITY-SEAM(Metal-device): qglTextureSubImage2DEXT (RGBA) seamed.
				// CPU data in `data`; dims width x height; mip `miplevel`.
				(void)dataFormat; (void)dataType;
			}
		}

		if ( !lastMip && numMips < 2 ) {
			if ( glRefConfig.framebufferObject ) {
				// PARITY-SEAM(Metal-device): qglGenerateTextureMipmapEXT seamed.
				// Metal: MTLTexture generateMipmapsForTexture. GL2 ref: tr_image.c:2069
				break;
			} else if ( rgba8 ) {
				if ( type == IMGTYPE_NORMAL || type == IMGTYPE_NORMALHEIGHT ) {
					R_MipMapNormalHeight( data, data, width, height, glRefConfig.swizzleNormalmap );
				} else {
					R_MipMapsRGB( data, width, height );
				}
			}
		}

		x >>= 1; y >>= 1;
		width  = MAX( 1, width  >> 1 );
		height = MAX( 1, height >> 1 );
		miplevel++;
		if ( numMips > 1 ) {
			data += size; numMips--;
		}
	} while ( !lastMip );

	if ( formatBuffer ) {
		ri.Hunk_FreeTempMemory( formatBuffer );
	}
}

// ===========================================================================
// FUNCTION 30/49: Upload32
//   CPU pre-processing: REAL; GPU upload: PARITY-SEAM via RawImage_UploadTexture
// ===========================================================================
static void Upload32( byte *data, int x, int y, int width, int height,
					  GLenum picFormat, GLenum dataFormat, GLenum dataType,
					  int numMips, image_t *image, qboolean scaled ) {
	int i, c;
	imgType_t type          = image->type;
	imgFlags_t flags         = image->flags;
	GLenum internalFormat = (GLenum)image->internalFormat;
	qboolean rgba8  = (qboolean)( ( picFormat == GL_RGBA8 || picFormat == GL_SRGB8_ALPHA8_EXT ) );
	qboolean mipmap = (qboolean)( !!( flags & IMGFLAG_MIPMAP ) && ( rgba8 || numMips > 1 ) );
	qboolean cubemap = ( qboolean ) !!( flags & IMGFLAG_CUBEMAP );
	(void)c;

	if ( rgba8 && !cubemap ) {
		if ( type == IMGTYPE_COLORALPHA ) {
			if ( !( flags & IMGFLAG_NOLIGHTSCALE ) && ( scaled || mipmap ) ) {
				R_LightScaleTexture( data, width, height, (qboolean)( !mipmap ) );
			}
		}
		if ( glRefConfig.swizzleNormalmap && ( type == IMGTYPE_NORMAL || type == IMGTYPE_NORMALHEIGHT ) ) {
			RawImage_SwizzleRA( data, width, height );
		}
	}

	if ( cubemap ) {
		for (i = 0; i < 6; i++) {
			int w2 = width, h2 = height;
			RawImage_UploadTexture( image->texnum, data, x, y, width, height,
									GL_TEXTURE_CUBE_MAP_POSITIVE_X + i,
									picFormat, dataFormat, dataType, numMips, internalFormat, type, flags, qfalse );
			for (c = numMips; c; c--) {
				data += CalculateMipSize( w2, h2, picFormat );
				w2 = MAX( 1, w2 >> 1 ); h2 = MAX( 1, h2 >> 1 );
			}
		}
	} else {
		RawImage_UploadTexture( image->texnum, data, x, y, width, height, GL_TEXTURE_2D,
								picFormat, dataFormat, dataType, numMips, internalFormat, type, flags, qfalse );
	}
	GL_CheckErrors();
}

// ===========================================================================
// R_CreateImage2 -- REAL bookkeeping + Metal texture create
// ===========================================================================
image_t *R_CreateImage2( const char *name, byte *pic, int width, int height,
						 GLenum picFormat, int numMips, imgType_t type, imgFlags_t flags, int internalFormat ) {
	byte     *resampledBuffer = NULL;
	image_t  *image;
	qboolean isLightmap = qfalse, scaled = qfalse;
	long hash;
	int glWrapClampMode, mipWidth, mipHeight, miplevel;
	qboolean rgba8   = (qboolean)( (qboolean)( picFormat == GL_RGBA8 || picFormat == GL_SRGB8_ALPHA8_EXT ) );
	qboolean mipmap  = ( qboolean ) !!( flags & IMGFLAG_MIPMAP );
	qboolean cubemap = ( qboolean ) !!( flags & IMGFLAG_CUBEMAP );
	qboolean picmip  = ( qboolean ) !!( flags & IMGFLAG_PICMIP );
	qboolean lastMip;
	GLenum textureTarget = cubemap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
	GLenum dataFormat, dataType;

	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Error( ERR_DROP, "R_CreateImage: \"%s\" is too long", name );
	}
	if ( !strncmp( name, "*lightmap", 9 ) ) {
		isLightmap = qtrue;
	}
	if ( tr.numImages == MAX_DRAWIMAGES ) {
		ri.Error( ERR_DROP, "R_CreateImage: MAX_DRAWIMAGES hit" );
	}

	image = tr.images[tr.numImages] = (image_t*)ri.Hunk_Alloc( sizeof( image_t ), h_low );

	// GL2 ref: tr_image.c:2190 — Metal device side-table replaces qglGenTextures.
	image->texnum = s_texnumCounter++;

	tr.numImages++;
	image->type  = type;
	image->flags = flags;
	strcpy( image->imgName, name );
	image->width  = width;
	image->height = height;

	glWrapClampMode = ( flags & IMGFLAG_CLAMPTOEDGE ) ? GL_CLAMP_TO_EDGE : GL_REPEAT;

	if ( !internalFormat ) {
		internalFormat = (int)RawImage_GetFormat( pic, width * height, picFormat, isLightmap, type, flags );
	}

	dataFormat = PixelDataFormatFromInternalFormat( (GLenum)internalFormat );
	dataType   = ( picFormat == GL_RGBA16 ) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

	// GLES path skipped (qglesMajorVersion == 0 under Metal). GL2 ref: tr_image.c:2212
	if ( qglesMajorVersion ) {
		switch ( internalFormat ) {
		case GL_LUMINANCE: case GL_LUMINANCE8:
			internalFormat = GL_LUMINANCE; dataFormat = GL_LUMINANCE; dataType = GL_UNSIGNED_BYTE; break;
		case GL_LUMINANCE_ALPHA: case GL_LUMINANCE8_ALPHA8:
			internalFormat = GL_LUMINANCE_ALPHA; dataFormat = GL_LUMINANCE_ALPHA; dataType = GL_UNSIGNED_BYTE; break;
		case GL_RGB: case GL_RGB8:
			internalFormat = GL_RGB; dataFormat = GL_RGB; dataType = GL_UNSIGNED_BYTE; break;
		case GL_RGB5:
			internalFormat = GL_RGB; dataFormat = GL_RGB; dataType = GL_UNSIGNED_SHORT_5_6_5; break;
		case GL_RGBA: case GL_RGBA8:
			internalFormat = GL_RGBA; dataFormat = GL_RGBA; dataType = GL_UNSIGNED_BYTE; break;
		case GL_RGBA4:
			internalFormat = GL_RGBA; dataFormat = GL_RGBA; dataType = GL_UNSIGNED_SHORT_4_4_4_4; break;
		default:
			ri.Error( ERR_DROP, "Missing GLES support for image '%s' format 0x%X\n", name, internalFormat );
		}
	}

	image->internalFormat = internalFormat;

	if ( !cubemap ) {
		if ( rgba8 ) {
			scaled = RawImage_ScaleToPower2( &pic, &width, &height, type, flags, &resampledBuffer );
		} else if ( pic && picmip ) {
			for (miplevel = r_picmip->integer; miplevel > 0 && numMips > 1; miplevel--, numMips--) {
				int sz = CalculateMipSize( width, height, picFormat );
				width = MAX( 1, width >> 1 ); height = MAX( 1, height >> 1 );
				pic += sz;
			}
		}
	}

	image->uploadWidth  = width;
	image->uploadHeight = height;

	// GL2 ref: tr_image.c:2282-2301 — GLMetal_CreateTexture below replaces
	// the seamed qglTextureImage2DEXT storage loop after level-0 pixels are final.
	mipWidth = width; mipHeight = height; miplevel = 0;
	do {
		lastMip = (qboolean)( !mipmap || ( mipWidth == 1 && mipHeight == 1 ) );
		(void)textureTarget; (void)miplevel; (void)dataFormat; (void)dataType;
		mipWidth  = MAX( 1, mipWidth  >> 1 );
		mipHeight = MAX( 1, mipHeight >> 1 );
		miplevel++;
	} while ( !lastMip );

	if ( pic ) {
		Upload32( pic, 0, 0, width, height, picFormat, dataFormat, dataType, numMips, image, scaled );
	}

	// GL2 ref: tr_image.c:2282-2305 — GLMetal_CreateTexture replaces the seamed
	// qglTextureImage2DEXT allocation plus level-0 qglTextureSubImage2DEXT upload.
	{
		MTL::PixelFormat metalPixelFormat = Metal_PixelFormatFromInternalFormat( (GLenum)image->internalFormat );
		// The scene depth buffer carries stencil: r_shadows 2 stencil shadow volumes draw into
		// tr.renderFbo, whose depth attachment had no stencil plane (the stencil test was a
		// no-op). Depth32Float_Stencil8 still samples as depth2d<float> for SSAO/shadowmask.
		if ( Metal_IsRenderDepthImage( name, pic ) ) {
			metalPixelFormat = MTL::PixelFormatDepth32Float_Stencil8;
		}
		const MTL::TextureUsage metalUsage = Metal_TextureUsageForImage( name, pic );
#if defined( __APPLE__ )
		if ( GLMetal_CreateTextureWithFormatAndUsage ) {
			// GL2 ref: tr_image.c:2282-2305 — DDS BCx images carry all
			// precomputed mip bytes in `pic`; Metal cannot auto-generate BC mips.
			GLMetal_CreateTextureWithFormatUsageAndMips( (uint32_t)image->texnum, image->uploadWidth, image->uploadHeight,
														 image->internalFormat, pic,
														 (qboolean)( ( flags & IMGFLAG_MIPMAP ) ? qtrue : qfalse ),
														 glWrapClampMode, (int)metalPixelFormat, (uint32_t)metalUsage, (int)picFormat, numMips );
		} else
#endif
		{
			if ( Metal_IsRenderImage( name, pic ) || Metal_IsRenderDepthImage( name, pic ) ) {
				ri.Printf( PRINT_WARNING,
						   "[Metal] %s needs GLMetal_CreateTextureWithFormatAndUsage for pixel format %d usage 0x%X; using legacy texture create\n",
						   name, (int)metalPixelFormat, (unsigned int)metalUsage );
			}
			GLMetal_CreateTexture( (uint32_t)image->texnum, image->uploadWidth, image->uploadHeight,
								   image->internalFormat, pic,
								   (qboolean)( ( flags & IMGFLAG_MIPMAP ) ? qtrue : qfalse ),
								   glWrapClampMode );
		}
	}

	if ( resampledBuffer ) {
		ri.Hunk_FreeTempMemory( resampledBuffer );
	}

	// GL2 ref: tr_image.c:2311-2339 — sampler qglTextureParameterfEXT/i state
	// is consumed device-side from flags/wrap when textures/samplers are resolved.
	// F2: textureFilterAnisotropic and maxAnisotropy are consumed device-side by
	// GLMetal_GetOrCreateSampler (tr_metal_device).  The (void) suppression casts
	// have been removed so the compiler can see the globals are live.
	// GL2 ref: tr_image.c:2317-2319 (anisotropy applied per-texture at upload)
	(void)textureTarget;

	GL_CheckErrors();

	hash = generateHashValue( name );
	image->next = hashTable[hash];
	hashTable[hash] = image;

	// Ridah
	image->hash = hash;

	return image;
}

// ===========================================================================
// R_CreateImage -- REAL (wrapper for R_CreateImage2, for the old parameters)
// ===========================================================================
image_t *R_CreateImage( const char *name, byte *pic, int width, int height,
						imgType_t type, imgFlags_t flags, int internalFormat ) {
	return R_CreateImage2( name, pic, width, height, GL_RGBA8, 0, type, flags, internalFormat );
}


// ===========================================================================
// FUNCTION 33/49: R_UpdateSubImage -- REAL format selection + Metal sub-update
// ===========================================================================
void R_UpdateSubImage( image_t *image, byte *pic, int x, int y, int width, int height, GLenum picFormat ) {
	GLenum dataFormat = PixelDataFormatFromInternalFormat( (GLenum)image->internalFormat );
	GLenum dataType   = ( picFormat == GL_RGBA16 ) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
	Upload32( pic, x, y, width, height, picFormat, dataFormat, dataType, 0, image, qfalse );

	// GL2 ref: tr_image.c:2372 — GLMetal_UpdateTextureSubImage replaces the
	// seamed qglTextureSubImage2DEXT path for dynamic level-0 region updates.
	GLMetal_UpdateTextureSubImage( (uint32_t)image->texnum, x, y, width, height, pic );
}

// Image loader table (GL2 ref: tr_image.c:2380-2399)
typedef struct {
	const char *ext;
	void (*ImageLoader)( const char *, unsigned char **, int *, int * );
} imageExtToLoaderMap_t;

static imageExtToLoaderMap_t imageLoaders[] = {
	{ "png",  R_LoadPNG },
	{ "tga",  R_LoadTGA },
	{ "jpg",  R_LoadJPG },
	{ "jpeg", R_LoadJPG },
	{ "pcx",  R_LoadPCX },
	{ "bmp",  R_LoadBMP }
};
static int numImageLoaders = ARRAY_LEN( imageLoaders );

// ===========================================================================
// FUNCTION 34/49: R_LoadImage -- REAL
// ===========================================================================
void R_LoadImage( const char *name, byte **pic, int *width, int *height,
				  GLenum *picFormat, int *numMips ) {
	qboolean orgNameFailed = qfalse;
	int orgLoader = -1;
	int i;
	char base[MAX_QPATH], localName[MAX_QPATH], ddsName[MAX_QPATH];
	const char *ext;
	char      *altName;

	*pic = NULL; *width = 0; *height = 0; *picFormat = GL_RGBA8; *numMips = 0;

	Q_strncpyz( localName, name, sizeof( localName ) );
	ext = COM_GetExtension( localName );
	COM_StripExtension( name, base, sizeof( base ) );
	Q_strncpyz( ddsName, base, sizeof( ddsName ) );
	Q_strcat( ddsName, sizeof( ddsName ), ".dds" );

	if ( r_ext_compressed_textures->integer ) {
		R_LoadDDS( ddsName, pic, width, height, picFormat, numMips );
		if (*pic ) {
			return;
		}

		if ( ext && *ext && Q_stricmp( ext, "dds" ) ) {
			for (i = 0; i < numImageLoaders; i++) {
				if ( !Q_stricmp( ext, imageLoaders[i].ext ) ) {
					imageLoaders[i].ImageLoader( localName, pic, width, height );
					if (*pic ) {
						return;
					}
					orgNameFailed = qtrue; orgLoader = i; break;
				}
			}
			COM_StripExtension( name, localName, sizeof( localName ) );
		}

		for (i = 0; i < numImageLoaders; i++) {
			if ( i == orgLoader ) {
				continue;
			}
			altName = va( "%s.%s", localName, imageLoaders[i].ext );
			imageLoaders[i].ImageLoader( altName, pic, width, height );
			if (*pic ) {
				if ( orgNameFailed ) {
					ri.Printf( PRINT_DEVELOPER, "WARNING: %s not present, using %s instead\n", name, altName );
				}
				return;
			}
		}
		return;
	}

	if ( ext && *ext && Q_stricmp( ext, "dds" ) ) {
		for (i = 0; i < numImageLoaders; i++) {
			if ( !Q_stricmp( ext, imageLoaders[i].ext ) ) {
				imageLoaders[i].ImageLoader( localName, pic, width, height );
				if (*pic ) {
					return;
				}
				orgNameFailed = qtrue; orgLoader = i; break;
			}
		}
		COM_StripExtension( name, localName, sizeof( localName ) );
	}

	for (i = 0; i < numImageLoaders; i++) {
		if ( i == orgLoader ) {
			continue;
		}
		if ( !Q_stricmp( imageLoaders[i].ext, "dds" ) ) {
			continue;
		}
		altName = va( "%s.%s", localName, imageLoaders[i].ext );
		imageLoaders[i].ImageLoader( altName, pic, width, height );
		if (*pic ) {
			if ( orgNameFailed ) {
				ri.Printf( PRINT_DEVELOPER, "WARNING: %s not present, using %s instead\n", name, altName );
			}
			return;
		}
	}

	if ( !*pic ) {
		R_LoadDDS( ddsName, pic, width, height, picFormat, numMips );
		if (*pic ) {
			ri.Printf( PRINT_DEVELOPER, "WARNING: falling back to DDS %s\n", ddsName );
		}
	}
}

// ===========================================================================
// R_FindImageFile -- REAL
// ===========================================================================
image_t *R_FindImageFile( const char *name, imgType_t type, imgFlags_t flags ) {
	image_t    *image;
	int width, height;
	byte       *pic;
	GLenum picFormat;
	int picNumMips;
	long hash;
	imgFlags_t checkFlagsTrue, checkFlagsFalse;

	if ( !name ) {
		return NULL;
	}

	hash = generateHashValue( name );

	for (image = hashTable[hash]; image; image = image->next) {
		if ( !strcmp( name, image->imgName ) ) {
			// the white image can be used with any set of parms, but other mismatches are errors
			if ( strcmp( name, "*white" ) ) {
				if ( image->flags != flags ) {
					ri.Printf( PRINT_DEVELOPER, "WARNING: reused image %s with mixed flags (%i vs %i)\n",
							   name, image->flags, flags );
				}
			}
			return image;
		}
	}

	R_LoadImage( name, &pic, &width, &height, &picFormat, &picNumMips );
	if ( !pic ) {
		return NULL;
	}

	checkFlagsTrue  = (imgFlags_t)( IMGFLAG_PICMIP | IMGFLAG_MIPMAP | IMGFLAG_GENNORMALMAP );
	checkFlagsFalse = IMGFLAG_CUBEMAP;
	if ( r_normalMapping->integer && ( picFormat == GL_RGBA8 ) && ( type == IMGTYPE_COLORALPHA ) &&
		 ( ( flags & checkFlagsTrue ) == checkFlagsTrue ) && !( flags & checkFlagsFalse ) ) {
		char normalName[MAX_QPATH];
		image_t   *normalImage;
		int normalWidth, normalHeight;
		imgFlags_t normalFlags = (imgFlags_t)( ( flags & ~IMGFLAG_GENNORMALMAP ) | IMGFLAG_NOLIGHTSCALE );

		COM_StripExtension( name, normalName, MAX_QPATH );
		Q_strcat( normalName, MAX_QPATH, "_n" );

		normalImage = R_FindImageFile( normalName, IMGTYPE_NORMAL, normalFlags );
		if ( !normalImage ) {
			byte *normalPic;
			int x, y;
			normalWidth = width; normalHeight = height;
			normalPic = (byte*)ri.Z_Malloc( width * height * 4 );
			RGBAtoNormal( pic, normalPic, width, height, (qboolean)( flags & IMGFLAG_CLAMPTOEDGE ) );

			RGBAtoYCoCgA( pic, pic, width, height );
			for (y = 0; y < height; y++) {
				byte *picbyte  = pic       + y * width * 4;
				byte *normbyte = normalPic + y * width * 4;
				for (x = 0; x < width; x++) {
					int div = MAX( normbyte[2] - 127, 16 );
					picbyte[0] = CLAMP( picbyte[0] * 128 / div, 0, 255 );
					picbyte += 4; normbyte += 4;
				}
			}
			YCoCgAtoRGBA( pic, pic, width, height );

			R_CreateImage( normalName, normalPic, normalWidth, normalHeight,
						   IMGTYPE_NORMAL, normalFlags, 0 );
			ri.Free( normalPic );
		}
	}

	if ( ( flags & IMGFLAG_MIPMAP ) && picFormat != GL_RGBA8 && picFormat != GL_SRGB8_ALPHA8_EXT ) {
		int wh = MAX( width, height );
		int neededMips = 0;
		while ( wh ) { neededMips++; wh >>= 1; }
		if ( neededMips > picNumMips ) {
			flags = (imgFlags_t)( flags & ~IMGFLAG_MIPMAP );
		}
	}

	image = R_CreateImage2( ( char * ) name, pic, width, height, picFormat, picNumMips, type, flags, 0 );
	ri.Free( pic );
	return image;
}


/*
================
R_CreateDlightImage
================
*/
// ===========================================================================
// FUNCTION 36/49: R_CreateDlightImage -- REAL (static)
// ===========================================================================
#define DLIGHT_SIZE 16
static void R_CreateDlightImage( void ) {
	int x, y;
	byte data[DLIGHT_SIZE][DLIGHT_SIZE][4];
	int b;

	for (x = 0; x < DLIGHT_SIZE; x++) {
		for (y = 0; y < DLIGHT_SIZE; y++) {
			float d = ( DLIGHT_SIZE / 2 - 0.5f - x ) * ( DLIGHT_SIZE / 2 - 0.5f - x )
					  + ( DLIGHT_SIZE / 2 - 0.5f - y ) * ( DLIGHT_SIZE / 2 - 0.5f - y );
			b = 4000 / (int)d;
			if ( b > 255 ) {
				b = 255;
			} else if ( b < 75 ) {
				b = 0;
			}
			data[y][x][0] = data[y][x][1] = data[y][x][2] = b;
			data[y][x][3] = 255;
		}
	}
	tr.dlightImage = R_CreateImage( "*dlight", (byte*)data, DLIGHT_SIZE, DLIGHT_SIZE,
									IMGTYPE_COLORALPHA, IMGFLAG_CLAMPTOEDGE, 0 );
}

// ===========================================================================
// FUNCTION 37/49: R_InitFogTable -- REAL
// ===========================================================================
void R_InitFogTable( void ) {
	int i;
	float d, exp = 0.5f;
	for (i = 0; i < FOG_TABLE_SIZE; i++) {
		d = pow( (float)i / ( FOG_TABLE_SIZE - 1 ), exp );
		tr.fogTable[i] = d;
	}
}

// ===========================================================================
// FUNCTION 38/49: R_FogFactor -- REAL
// Returns a 0.0 to 1.0 fog density value
// ===========================================================================
float R_FogFactor( float s, float t ) {
	float d;
	s -= 1.0f / 512;
	if ( s < 0 ) {
		return 0;
	}
	if ( t < 1.0f / 32 ) {
		return 0;
	}
	if ( t < 31.0f / 32 ) {
		s *= ( t - 1.0f / 32.0f ) / ( 30.0f / 32.0f );
	}
	s *= 8;
	if ( s > 1.0f ) {
		s = 1.0f;
	}
	d = tr.fogTable[(int)( s * ( FOG_TABLE_SIZE - 1 ) )];
	return d;
}

// ===========================================================================
// FUNCTION 39/49: R_CreateFogImage -- REAL (static)
// ===========================================================================
#define FOG_S 256
#define FOG_T 32
static void R_CreateFogImage( void ) {
	int x, y;
	byte *data;
	float d;

	data = (byte*)ri.Hunk_AllocateTempMemory( FOG_S * FOG_T * 4 );
	for (x = 0; x < FOG_S; x++) {
		for (y = 0; y < FOG_T; y++) {
			d = R_FogFactor( ( x + 0.5f ) / FOG_S, ( y + 0.5f ) / FOG_T );
			data[( y * FOG_S + x ) * 4 + 0] = data[( y * FOG_S + x ) * 4 + 1] = data[( y * FOG_S + x ) * 4 + 2] = 255;
			data[( y * FOG_S + x ) * 4 + 3] = (byte)( 255 * d );
		}
	}
	tr.fogImage = R_CreateImage( "*fog", data, FOG_S, FOG_T, IMGTYPE_COLORALPHA, IMGFLAG_CLAMPTOEDGE, 0 );
	ri.Hunk_FreeTempMemory( data );
}

// ===========================================================================
// FUNCTION 40/49: R_CreateDefaultImage -- REAL (static)
// ===========================================================================
#define DEFAULT_SIZE 16
static void R_CreateDefaultImage( void ) {
	int x;
	byte data[DEFAULT_SIZE][DEFAULT_SIZE][4];

	Com_Memset( data, 32, sizeof( data ) );
	for (x = 0; x < DEFAULT_SIZE; x++) {
		//----(SA) to make the default grid noticable but not blinding
		data[0][x][0] = data[0][x][1] = 255; data[0][x][2] = 0; data[0][x][3] = 255;
		data[x][0][0] = data[x][0][1] = 255; data[x][0][2] = 0; data[x][0][3] = 255;
		data[DEFAULT_SIZE - 1][x][0] = data[DEFAULT_SIZE - 1][x][1] = 255;
		data[DEFAULT_SIZE - 1][x][2] = 0; data[DEFAULT_SIZE - 1][x][3] = 255;
		data[x][DEFAULT_SIZE - 1][0] = data[x][DEFAULT_SIZE - 1][1] = 255;
		data[x][DEFAULT_SIZE - 1][2] = 0; data[x][DEFAULT_SIZE - 1][3] = 255;
	}
	tr.defaultImage = R_CreateImage( "*default", (byte*)data, DEFAULT_SIZE, DEFAULT_SIZE,
									 IMGTYPE_COLORALPHA, IMGFLAG_MIPMAP, 0 );
}

// ===========================================================================
// FUNCTION 41/49: R_CreateBuiltinImages -- REAL
// ===========================================================================
void R_CreateBuiltinImages( void ) {
	int x, y;
	byte data[DEFAULT_SIZE][DEFAULT_SIZE][4];

	R_CreateDefaultImage();

	Com_Memset( data, 255, sizeof( data ) );
	tr.whiteImage = R_CreateImage( "*white", (byte*)data, 8, 8, IMGTYPE_COLORALPHA, IMGFLAG_NONE, 0 );

	if ( r_dlightMode->integer >= 2 ) {
		for (x = 0; x < MAX_DLIGHTS; x++) {
			tr.shadowCubemaps[x] = R_CreateImage( va( "*shadowcubemap%i", x ), NULL,
												  PSHADOW_MAP_SIZE, PSHADOW_MAP_SIZE, IMGTYPE_COLORALPHA,
												  (imgFlags_t)( IMGFLAG_CLAMPTOEDGE | IMGFLAG_CUBEMAP ), 0 );
		}
	}

	for (x = 0; x < DEFAULT_SIZE; x++) {
		for (y = 0; y < DEFAULT_SIZE; y++) {
			data[y][x][0] = data[y][x][1] = data[y][x][2] = tr.identityLightByte;
			data[y][x][3] = 255;
		}
	}
	tr.identityLightImage = R_CreateImage( "*identityLight", (byte*)data, 8, 8,
										   IMGTYPE_COLORALPHA, IMGFLAG_NONE, 0 );

	for (x = 0; x < 32; x++) {
		tr.scratchImage[x] = R_CreateImage( "*scratch", (byte*)data, DEFAULT_SIZE, DEFAULT_SIZE,
											IMGTYPE_COLORALPHA, (imgFlags_t)( IMGFLAG_PICMIP | IMGFLAG_CLAMPTOEDGE ), 0 );
	}

	R_CreateDlightImage();
	R_CreateFogImage();

	if ( glRefConfig.framebufferObject ) {
		int width = glConfig.vidWidth, height = glConfig.vidHeight;
		int hdrFormat = GL_RGBA8;
		if ( r_hdr->integer && glRefConfig.textureFloat ) {
			hdrFormat = GL_RGBA16F_ARB;
		}
		int rgbFormat = GL_RGBA8;

		tr.renderImage = R_CreateImage( "_render", NULL, width, height, IMGTYPE_COLORALPHA,
										(imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), hdrFormat );

		if ( r_shadowBlur->integer || r_hdr->integer ) {
			tr.screenScratchImage = R_CreateImage( "screenScratch", NULL, width, height,
												   IMGTYPE_COLORALPHA, (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), rgbFormat );
		}

		if ( r_shadowBlur->integer || r_ssao->integer ) {
			tr.hdrDepthImage = R_CreateImage( "*hdrDepth", NULL, width, height, IMGTYPE_COLORALPHA,
											  (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_R32F );
		}

		if ( r_drawSunRays->integer ) {
			tr.sunRaysImage = R_CreateImage( "*sunRays", NULL, width, height, IMGTYPE_COLORALPHA,
											 (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), rgbFormat );
		}

		tr.renderDepthImage  = R_CreateImage( "*renderdepth", NULL, width, height, IMGTYPE_COLORALPHA,
											  (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_DEPTH_COMPONENT24 );
		tr.textureDepthImage = R_CreateImage( "*texturedepth", NULL, PSHADOW_MAP_SIZE, PSHADOW_MAP_SIZE,
											  IMGTYPE_COLORALPHA, (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_DEPTH_COMPONENT24 );

		{
			void *p;
			data[0][0][0] = 0; data[0][0][1] = (byte)( 0.45f * 255 );
			data[0][0][2] = 255; data[0][0][3] = 255;
			p = data;
			tr.calcLevelsImage   = R_CreateImage( "*calcLevels",   (byte*)p, 1, 1, IMGTYPE_COLORALPHA,
												  (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), hdrFormat );
			tr.targetLevelsImage = R_CreateImage( "*targetLevels", (byte*)p, 1, 1, IMGTYPE_COLORALPHA,
												  (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), hdrFormat );
			tr.fixedLevelsImage  = R_CreateImage( "*fixedLevels",  (byte*)p, 1, 1, IMGTYPE_COLORALPHA,
												  (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), hdrFormat );
		}
		for (x = 0; x < 2; x++)
			tr.textureScratchImage[x] = R_CreateImage( va( "*textureScratch%d", x ), NULL, 256, 256,
													   IMGTYPE_COLORALPHA, (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_RGBA8 );
		for (x = 0; x < 2; x++)
			tr.quarterImage[x] = R_CreateImage( va( "*quarter%d", x ), NULL, width / 2, height / 2,
												IMGTYPE_COLORALPHA, (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_RGBA8 );
		if ( r_ssao->integer ) {
			// Full render resolution: the Apple SAO compute kernel (GLMetal_DispatchSAO, T4b)
			// uses thread_position_in_grid as both the AO write coordinate and the depth read
			// coordinate, requiring the AO output and depth input textures to share the same
			// resolution.  Half-res SAO + hierarchical-depth pyramid + depth-blur is a deferred
			// v2 performance optimisation; see SSAO parity deviation decision:
			//   .squad/decisions/inbox/dallas-ssao-deviation-decision.md  (T5b).
			tr.screenSsaoImage = R_CreateImage( "*screenSsao", NULL, width, height, IMGTYPE_COLORALPHA,
												(imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_RGBA8 );
		}

		for (x = 0; x < MAX_DRAWN_PSHADOWS; x++) {
			tr.pshadowMaps[x] = R_CreateImage( va( "*shadowmap%i", x ), NULL, PSHADOW_MAP_SIZE, PSHADOW_MAP_SIZE,
											   IMGTYPE_COLORALPHA, (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_DEPTH_COMPONENT24 );
			// PARITY-SEAM(Metal-device): GL_TEXTURE_COMPARE_MODE/FUNC seamed.
			// Metal: MTLSamplerDescriptor.compareFunction. GL2 ref: tr_image.c:2944
		}

		if ( r_sunlightMode->integer ) {
			for (x = 0; x < 4; x++) {
				tr.sunShadowDepthImage[x] = R_CreateImage( va( "*sunshadowdepth%i", x ), NULL,
														   r_shadowMapSize->integer, r_shadowMapSize->integer, IMGTYPE_COLORALPHA,
														   (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_DEPTH_COMPONENT24 );
				// PARITY-SEAM(Metal-device): depth compare seamed. GL2 ref: tr_image.c:2944
			}
			tr.screenShadowImage = R_CreateImage( "*screenShadow", NULL, width, height,
												  IMGTYPE_COLORALPHA, (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE ), GL_RGBA8 );
		}

		if ( r_cubeMapping->integer ) {
			tr.renderCubeImage = R_CreateImage( "*renderCube", NULL, r_cubemapSize->integer, r_cubemapSize->integer,
												IMGTYPE_COLORALPHA, (imgFlags_t)( IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE | IMGFLAG_MIPMAP | IMGFLAG_CUBEMAP ), rgbFormat );
		}
	}
}

// ===========================================================================
// FUNCTION 42/49: R_SetColorMappings
//   Gamma/intensity table build: REAL; GLimp_SetGamma: PARITY-SEAM
// ===========================================================================
void R_SetColorMappings( void ) {
	int i, j;
	float g;
	int inf;

	tr.overbrightBits = r_overBrightBits->integer;
	if ( tr.overbrightBits > 2 ) {
		tr.overbrightBits = 2;
	} else if ( tr.overbrightBits < 0 ) {
		tr.overbrightBits = 0;
	}
	if ( tr.overbrightBits > r_mapOverBrightBits->integer ) {
		tr.overbrightBits = r_mapOverBrightBits->integer;
	}

	tr.identityLight     = 1.0f / ( 1 << tr.overbrightBits );
	tr.identityLightByte = (int)( 255 * tr.identityLight );

	if ( r_intensity->value <= 1 ) {
		ri.Cvar_Set( "r_intensity", "1" );
	}
	if ( r_gamma->value < 0.5f ) {
		ri.Cvar_Set( "r_gamma", "0.5" );
	} else if ( r_gamma->value > 3.0f ) {
		ri.Cvar_Set( "r_gamma", "3.0" );
	}

	g = r_gamma->value;
	for (i = 0; i < 256; i++) {
		inf = ( g == 1 ) ? i : (int)( 255 * pow( i / 255.0f, 1.0f / g ) + 0.5f );
		if ( inf < 0 ) {
			inf = 0;
		}
		if ( inf > 255 ) {
			inf = 255;
		}
		s_gammatable[i] = inf;
	}
	for (i = 0; i < 256; i++) {
		j = (int)( i * r_intensity->value );
		if ( j > 255 ) {
			j = 255;
		}
		s_intensitytable[i] = j;
	}

	if ( glConfig.deviceSupportsGamma ) {
		// PARITY-SEAM(Metal-device): GLimp_SetGamma uploads hardware gamma ramp via
		// SDL/OpenGL. Under Metal no direct HW gamma ramp; display gamma is CoreDisplay.
		// s_gammatable computed above for software-path usage in R_LightScaleTexture.
	}
}

// ===========================================================================
// FUNCTION 43/49: R_InitImages -- REAL
// ===========================================================================
void R_InitImages( void ) {
	Com_Memset( hashTable, 0, sizeof( hashTable ) );
	R_SetColorMappings();
	R_CreateBuiltinImages();
}

// ===========================================================================
// FUNCTION 44/49: R_DeleteTextures
//   Array clear: REAL; texture deletion via Metal device side-table
// ===========================================================================
void R_DeleteTextures( void ) {
	int i;
	for (i = 0; i < tr.numImages; i++) {
		// GL2 ref: tr_image.c:3053 — GLMetal_DeleteTexture replaces the seamed
		// qglDeleteTextures call and releases the texnum-keyed MTL::Texture.
		GLMetal_DeleteTexture( (uint32_t)tr.images[i]->texnum );
	}
	Com_Memset( tr.images, 0, sizeof( tr.images ) );
	tr.numImages = 0;
	s_texnumCounter = 1; // reset so handles are reused after full shutdown
	GL_BindNullTextures();
}

// ===========================================================================
// FUNCTION 45/49: CommaParse -- REAL (static)
// Parses comma-separated skin file tokens (not compatible with normal parser)
// ===========================================================================
static char *CommaParse( char **data_p ) {
	int c = 0, len;
	char *data;
	static char com_token[MAX_TOKEN_CHARS];

	data = *data_p;
	len  = 0;
	com_token[0] = 0;

	if ( !data ) {
		*data_p = NULL; return com_token;
	}

	while ( 1 ) {
		while ( ( c = *data ) <= ' ' ) { if ( !c ) {
											 break;
										 }
										 data++; }
		c = *data;
		if ( c == '/' && data[1] == '/' ) {
			data += 2;
			while (*data && *data != '\n' ) data++;
		} else if ( c == '/' && data[1] == '*' ) {
			data += 2;
			while (*data && (*data != '*' || data[1] != '/' ) ) data++;
			if (*data ) {
				data += 2;
			}
		} else {break;}
	}

	if ( c == 0 ) {
		return (char*)"";
	}

	if ( c == '"' ) {
		data++;
		while ( 1 ) {
			c = *data++;
			if ( c == '"' || !c ) {
				com_token[len] = 0; *data_p = data; return com_token;
			}
			if ( len < MAX_TOKEN_CHARS - 1 ) {
				com_token[len] = c; len++;
			}
		}
	}

	do {
		if ( len < MAX_TOKEN_CHARS - 1 ) {
			com_token[len] = c; len++;
		}
		data++; c = *data;
	} while ( c > 32 && c != ',' );

	com_token[len] = 0;
	*data_p = data;
	return com_token;
}


//----(SA) added so client can see what model or scale for the model was specified in a skin
/*
==============
RE_GetSkinModel
==============
*/
qboolean RE_GetSkinModel( qhandle_t skinid, const char *type, char *name ) {
	int i;
	skin_t      *bar;

	if ( skinid < 1 || skinid >= tr.numSkins ) {
		return qfalse;
	}

	bar = tr.skins[skinid];

	if ( !Q_stricmp( type, "playerscale" ) ) {    // client is requesting scale from the skin rather than a model
		Com_sprintf( name, MAX_QPATH, "%.2f %.2f %.2f", bar->scale[0], bar->scale[1], bar->scale[2] );
		return qtrue;
	}

	for ( i = 0; i < bar->numModels; i++ )
	{
		if ( !Q_stricmp( bar->models[i]->type, type ) ) { // (SA) whoops, should've been this way
			Q_strncpyz( name, bar->models[i]->model, sizeof( bar->models[i]->model ) );
			return qtrue;
		}
	}
	return qfalse;
}

/*
==============
RE_GetShaderFromModel
	return a shader index for a given model's surface
	'withlightmap' set to '0' will create a new shader that is a copy of the one found
	on the model, without the lighmap stage, if the shader has a lightmap stage

	NOTE: only works for bmodels right now.  Could modify for other models (md3's etc.)
==============
*/
qhandle_t RE_GetShaderFromModel( qhandle_t modelid, int surfnum, int withlightmap ) {
//	model_t     *model;
//	bmodel_t    *bmodel;
//	msurface_t  *surf;
//	shader_t    *shd;

//	if ( surfnum < 0 ) {
//		surfnum = 0;
//	}

//	model = R_GetModelByHandle( modelid );  // (SA) should be correct now
/* FIXME - Rend2
	if ( model ) {
		bmodel  = model->bmodel;
		if ( bmodel && bmodel->firstSurface ) {
			if ( surfnum >= bmodel->numSurfaces ) { // if it's out of range, return the first surface
				surfnum = 0;
			}

			surf = bmodel->firstSurface + surfnum;
//			if(surf->shader->lightmapIndex != LIGHTMAP_NONE) {
			if ( surf->shader->lightmapIndex > LIGHTMAP_NONE ) {
				image_t *image;
				long hash;
				qboolean mip = qtrue;   // mip generation on by default

				// get mipmap info for original texture
				hash = generateHashValue( surf->shader->name );
				for ( image = hashTable[hash]; image; image = image->next ) {
					if ( !strcmp( surf->shader->name, image->imgName ) ) {
						mip = image->flags & IMGFLAG_MIPMAP;
						break;
					}
				}
				shd = R_FindShader( surf->shader->name, LIGHTMAP_NONE, mip );
				shd->stages[0]->rgbGen = CGEN_LIGHTING_DIFFUSE; // (SA) new
			} else {
				shd = surf->shader;
			}

			return shd->index;
		}
	}
*/
	return 0;
}

//----(SA) end

/*
===============
RE_RegisterSkin

===============
*/
// ===========================================================================
// FUNCTION 46/49: RE_RegisterSkin -- REAL
// ===========================================================================
qhandle_t RE_RegisterSkin( const char *name ) {
	skinSurface_t parseSurfaces[MAX_SKIN_SURFACES];
	qhandle_t hSkin;
	skin_t       *skin;
	skinSurface_t *surf;
	skinModel_t *model;     //----(SA) added
	union { char *c; void *v; } text;
	char *text_p, *token;
	char surfName[MAX_QPATH];
	int totalSurfaces;

	if ( !name || !name[0] ) {
		ri.Printf( PRINT_DEVELOPER,"Empty name passed to RE_RegisterSkin\n" ); return 0;
	}
	if ( strlen( name ) >= MAX_QPATH ) {
		ri.Printf( PRINT_DEVELOPER,"Skin name exceeds MAX_QPATH\n" ); return 0;
	}

	for (hSkin = 1; hSkin < tr.numSkins; hSkin++) {
		skin = tr.skins[hSkin];
		if ( !Q_stricmp( skin->name, name ) ) {
			if ( skin->numSurfaces == 0 ) {
				return 0;
			}
			return hSkin;
		}
	}

	if ( tr.numSkins == MAX_SKINS ) {
		ri.Printf( PRINT_WARNING, "WARNING: RE_RegisterSkin('%s') MAX_SKINS hit\n", name );
		return 0;
	}

//----(SA)	moved things around slightly to fix the problem where you restart
//			a map that has ai characters who had invalid skin names entered
//			in thier "skin" or "head" field

	R_IssuePendingRenderCommands();

	if ( strcmp( name + strlen( name ) - 5, ".skin" ) ) {
		tr.numSkins++;
		skin = (skin_t *)ri.Hunk_Alloc( sizeof( skin_t ), h_low );
		tr.skins[hSkin] = skin;
		Q_strncpyz( skin->name, name, sizeof( skin->name ) );
		skin->numSurfaces   = 0;
		skin->numModels     = 0;        //----(SA) added
		skin->numSurfaces = 1;
		skin->surfaces = (skinSurface_t*)ri.Hunk_Alloc( sizeof( skinSurface_t ), h_low );
		skin->surfaces[0].shader = R_FindShader( name, LIGHTMAP_NONE, qtrue );
		return hSkin;
	}

	ri.FS_ReadFile( name, &text.v );
	if ( !text.c ) {
		return 0;
	}

	tr.numSkins++;
	skin = (skin_t *)ri.Hunk_Alloc( sizeof( skin_t ), h_low );
	tr.skins[hSkin] = skin;
	Q_strncpyz( skin->name, name, sizeof( skin->name ) );
	skin->numSurfaces   = 0;
	skin->numModels     = 0;        //----(SA) added

//----(SA)	end

	totalSurfaces = 0;
	text_p = text.c;
	while ( text_p && *text_p ) {
		token = CommaParse( &text_p );
		Q_strncpyz( surfName, token, sizeof( surfName ) );
		if ( !token[0] ) {
			break;
		}
		Q_strlwr( surfName );
		if (*text_p == ',' ) {
			text_p++;
		}
		if ( strstr( token, "tag_" ) ) {
			continue;
		}

		if ( strstr( token, "md3_" ) ) {  // this is specifying a model
			if ( skin->numModels >= MAX_PART_MODELS ) {
				ri.Printf( PRINT_WARNING, "WARNING: Ignoring models in '%s', the max is %d!\n", name, MAX_PART_MODELS );
				break;
			}

			model = skin->models[ skin->numModels ] = (skinModel_t *)ri.Hunk_Alloc( sizeof( *skin->models[0] ), h_low );
			Q_strncpyz( model->type, token, sizeof( model->type ) );

			// get the model name
			token = CommaParse( &text_p );

			Q_strncpyz( model->model, token, sizeof( model->model ) );

			skin->numModels++;
			continue;
		}

//----(SA)	added
		if ( strstr( token, "playerscale" ) ) {
			token = CommaParse( &text_p );
			skin->scale[0] = atof( token );   // uniform scaling for now
			skin->scale[1] = atof( token );
			skin->scale[2] = atof( token );
			continue;
		}
//----(SA) end

		// parse the shader name
		token = CommaParse( &text_p );
		if ( skin->numSurfaces < MAX_SKIN_SURFACES ) {
			surf = &parseSurfaces[skin->numSurfaces];
			Q_strncpyz( surf->name, surfName, sizeof( surf->name ) );
			surf->shader = R_FindShader( token, LIGHTMAP_NONE, qtrue );
			skin->numSurfaces++;
		}
		totalSurfaces++;
	}
	ri.FS_FreeFile( text.v );

	if ( totalSurfaces > MAX_SKIN_SURFACES ) {
		ri.Printf( PRINT_WARNING, "WARNING: Ignoring excess surfaces (found %d, max is %d) in skin '%s'!\n",
				   totalSurfaces, MAX_SKIN_SURFACES, name );
	}

	// never let a skin have 0 shaders
	//----(SA)	allow this for the (current) special case of the loper's upper body
	//			(it's upper body has no surfaces, only tags)
	if ( skin->numSurfaces == 0 ) {
		if ( !( strstr( name, "loper" ) && strstr( name, "upper" ) ) ) {
			return 0;       // use default skin
		}
	}

	skin->surfaces = (skinSurface_t*)ri.Hunk_Alloc( skin->numSurfaces * sizeof( skinSurface_t ), h_low );
	memcpy( skin->surfaces, parseSurfaces, skin->numSurfaces * sizeof( skinSurface_t ) );
	return hSkin;
}

// ===========================================================================
// FUNCTION 47/49: R_InitSkins -- REAL
// ===========================================================================
void R_InitSkins( void ) {
	skin_t *skin;
	tr.numSkins = 1;
	skin = tr.skins[0] = (skin_t*)ri.Hunk_Alloc( sizeof( skin_t ), h_low );
	Q_strncpyz( skin->name, "<default skin>", sizeof( skin->name ) );
	skin->numSurfaces = 1;
	skin->surfaces = (skinSurface_t*)ri.Hunk_Alloc( sizeof( skinSurface_t ), h_low );
	skin->surfaces[0].shader = tr.defaultShader;
}

// ===========================================================================
// FUNCTION 48/49: R_GetSkinByHandle -- REAL
// ===========================================================================
skin_t *R_GetSkinByHandle( qhandle_t hSkin ) {
	if ( hSkin < 1 || hSkin >= tr.numSkins ) {
		return tr.skins[0];
	}
	return tr.skins[hSkin];
}

// ===========================================================================
// FUNCTION 49/49: R_SkinList_f -- REAL
// ===========================================================================
void R_SkinList_f( void ) {
	int i, j;
	skin_t *skin;

	ri.Printf( PRINT_ALL, "------------------\n" );
	for (i = 0; i < tr.numSkins; i++) {
		skin = tr.skins[i];
		ri.Printf( PRINT_ALL, "%3i:%s (%d surfaces)\n", i, skin->name, skin->numSurfaces );
		for (j = 0; j < skin->numSurfaces; j++) {
			ri.Printf( PRINT_ALL, "       %s = %s\n",
					   skin->surfaces[j].name, skin->surfaces[j].shader->name );
		}
	}
	ri.Printf( PRINT_ALL, "------------------\n" );
}

// Ridah, utility for automatically cropping and numbering a bunch of images in a directory
/*
=============
SaveTGA

  saves out to 24 bit uncompressed format (no alpha)
=============
*/
void SaveTGA( char *name, byte **pic, int width, int height ) {
	byte    *inpixel, *outpixel;
	byte    *outbuf, *b;

	outbuf = (byte *)ri.Hunk_AllocateTempMemory( width * height * 4 + 18 );
	b = outbuf;

	memset( b, 0, 18 );
	b[2] = 2;       // uncompressed type
	b[12] = width & 255;
	b[13] = width >> 8;
	b[14] = height & 255;
	b[15] = height >> 8;
	b[16] = 24; // pixel size

	{
		int row, col;
		int rows, cols;

		rows = ( height );
		cols = ( width );

		outpixel = b + 18;

		for ( row = ( rows - 1 ); row >= 0; row-- )
		{
			inpixel = ( ( *pic ) + ( row * cols ) * 4 );

			for ( col = 0; col < cols; col++ )
			{
				*outpixel++ = *( inpixel + 2 );   // blue
				*outpixel++ = *( inpixel + 1 );   // green
				*outpixel++ = *( inpixel + 0 );   // red
				//*outpixel++ = *(inpixel + 3);	// alpha

				inpixel += 4;
			}
		}
	}

	ri.FS_WriteFile( name, outbuf, (int)( outpixel - outbuf ) );

	ri.Hunk_FreeTempMemory( outbuf );

}

/*
=============
SaveTGAAlpha

  saves out to 32 bit uncompressed format (with alpha)
=============
*/
void SaveTGAAlpha( char *name, byte **pic, int width, int height ) {
	byte    *inpixel, *outpixel;
	byte    *outbuf, *b;

	outbuf = (byte *)ri.Hunk_AllocateTempMemory( width * height * 4 + 18 );
	b = outbuf;

	memset( b, 0, 18 );
	b[2] = 2;       // uncompressed type
	b[12] = width & 255;
	b[13] = width >> 8;
	b[14] = height & 255;
	b[15] = height >> 8;
	b[16] = 32; // pixel size

	{
		int row, col;
		int rows, cols;

		rows = ( height );
		cols = ( width );

		outpixel = b + 18;

		for ( row = ( rows - 1 ); row >= 0; row-- )
		{
			inpixel = ( ( *pic ) + ( row * cols ) * 4 );

			for ( col = 0; col < cols; col++ )
			{
				*outpixel++ = *( inpixel + 2 );   // blue
				*outpixel++ = *( inpixel + 1 );   // green
				*outpixel++ = *( inpixel + 0 );   // red
				*outpixel++ = *( inpixel + 3 );   // alpha

				inpixel += 4;
			}
		}
	}

	ri.FS_WriteFile( name, outbuf, (int)( outpixel - outbuf ) );

	ri.Hunk_FreeTempMemory( outbuf );

}

/*
==============
R_CropImage
==============
*/
#define CROPIMAGES_ENABLED
//#define FUNNEL_HACK
#define RESIZE
//#define QUICKTIME_BANNER
#define TWILTB2_HACK

qboolean R_CropImage( char *name, byte **pic, int border, int *width, int *height, int lastBox[2] ) {
#ifdef CROPIMAGES_ENABLED
	int row, col;
	int rows, cols;
	byte    *inpixel, *temppic, *outpixel;
	int mins[2], maxs[2];
	int diff[2];
	//int	newWidth;
	int /*a, max,*/ i;
	int alpha;
	//int	*center;
	qboolean /*invalid,*/ skip;
	vec3_t fCol, fScale;
	qboolean filterColors = qfalse;
	int fCount;
	float f,c;
#define FADE_BORDER_RANGE   ( ( *width ) / 40 )

	rows = ( *height );
	cols = ( *width );

	mins[0] = 99999;
	mins[1] = 99999;
	maxs[0] = -99999;
	maxs[1] = -99999;

#ifdef TWILTB2_HACK
	// find the filter color (use first few pixels)
	filterColors = qtrue;
	inpixel = ( *pic );
	VectorClear( fCol );
	for ( fCount = 0; fCount < 8; fCount++, inpixel += 4 ) {
		for ( i = 0; i < 3; i++ ) {
			if ( *( inpixel + i ) > 70 ) {
				continue;   // too bright, cant be noise
			}
			if ( fCol[i] < *( inpixel + i ) ) {
				fCol[i] = *( inpixel + i );
			}
		}
	}
	//VectorScale( fCol, 1.0/fCount, fCol );
	for ( i = 0; i < 3; i++ ) {
		fCol[i] += 4;
		fScale[i] = 255.0 / ( 255.0 - fCol[i] );
	}
#endif

	for ( row = 0; row < rows; row++ )
	{
		inpixel = ( ( *pic ) + ( row * cols ) * 4 );

		for ( col = 0; col < cols; col++, inpixel += 4 )
		{
			if ( filterColors ) {
				// special code for filtering the twiltb series
				for ( i = 0; i < 3; i++ ) {
					f = *( inpixel + i );
					f -= fCol[i];
					if ( f <= 0 ) {
						f = 0;
					} else { f *= fScale[i];}
					if ( f > 255 ) {
						f = 255;
					}
					*( inpixel + i ) = f;
				}
				// if this pixel is near the edge, then fade it out (just do a brightness fade)
				if ( (int *)inpixel ) {
					// calc the fade scale
					f = (float)row / FADE_BORDER_RANGE;
					if ( f > (float)( rows - row - 1 ) / FADE_BORDER_RANGE ) {
						f = (float)( rows - row - 1 ) / FADE_BORDER_RANGE;
					}
					if ( f > (float)( cols - col - 1 ) / FADE_BORDER_RANGE ) {
						f = (float)( cols - col - 1 ) / FADE_BORDER_RANGE;
					}
					if ( f > (float)( col ) / FADE_BORDER_RANGE ) {
						f = (float)( col ) / FADE_BORDER_RANGE;
					}
					if ( f < 1.0 ) {
						if ( f <= 0 ) {
							*( (int *)inpixel ) = 0;
						} else {
							f += 0.2 * crandom();
							if ( f < 1.0 ) {
								if ( f <= 0 ) {
									*( (int *)inpixel ) = 0;
								} else {
									f = 1.0 - f;
									for ( i = 0; i < 3; i++ ) {
										c = *( inpixel + i );
										c -= f * c;
										if ( c < 0 ) {
											c = 0;
										}
										*( inpixel + i ) = c;
									}
								}
							}
						}
					}
				}
				continue;
			}

			skip = qfalse;
#ifdef QUICKTIME_BANNER
			// hack for quicktime ripped images
			if ( ( col > cols - 3 || row > rows - 36 ) ) {
				// filter this pixel out
				*( inpixel + 0 ) = 0;
				*( inpixel + 1 ) = 0;
				*( inpixel + 2 ) = 0;
				skip = qtrue;
			}
#endif

			if ( !skip ) {
				if (    ( *( inpixel + 0 ) > 20 )       // blue
						||  ( *( inpixel + 1 ) > 20 ) // green
						||  ( *( inpixel + 2 ) > 20 ) ) { // red

					if ( col < mins[0] ) {
						mins[0] = col;
					}
					if ( col > maxs[0] ) {
						maxs[0] = col;
					}
					if ( row < mins[1] ) {
						mins[1] = row;
					}
					if ( row > maxs[1] ) {
						maxs[1] = row;
					}

				} else {
					// filter this pixel out
					*( inpixel + 0 ) = 0;
					*( inpixel + 1 ) = 0;
					*( inpixel + 2 ) = 0;
				}
			}

#ifdef FUNNEL_HACK  // scale brightness down
			for ( i = 0; i < 3; i++ ) {
				alpha = *( inpixel + i );
				if ( ( alpha -= 20 ) < 0 ) {
					alpha = 0;
				}
				*( inpixel + i ) = alpha;
			}
#endif

			// set the alpha component
			alpha = *( inpixel + 0 ); // + *(inpixel + 1) + *(inpixel + 2);
			if ( *( inpixel + 1 ) > alpha ) {
				alpha = *( inpixel + 1 );
			}
			if ( *( inpixel + 2 ) > alpha ) {
				alpha = *( inpixel + 2 );
			}
			//alpha = (int)((float)alpha / 3.0);
			//alpha /= 3;
			if ( alpha > 255 ) {
				alpha = 255;
			}
			*( inpixel + 3 ) = (byte)alpha;
		}
	}

#ifdef RESIZE
	return qtrue;
#endif

	// convert it so that the center is the center of the image
	// this is used for some explosions
	/*
	for (i=0; i<2; i++) {
		if (i==0)	center = width;
		else		center = height;

		if ((*center/2 - mins[i]) > (maxs[i] - *center/2)) {
			maxs[i] = *center/2 + (*center/2 - mins[i]);
		} else {
			mins[i] = *center/2 - (maxs[i] - *center/2);
		}
	}
	*/

	// HACK for funnel
#ifdef FUNNEL_HACK
	mins[0] = 210;
	maxs[0] = 430;
	mins[1] = 0;
	maxs[1] = *height - 1;

	for ( i = 0; i < 2; i++ ) {
		diff[i] = maxs[i] - mins[i];
	}
#else

#ifndef RESIZE
	// apply the border
	for ( i = 0; i < 2; i++ ) {
		mins[i] -= border;
		if ( mins[i] < 0 ) {
			mins[i] = 0;
		}
		maxs[i] += border;
		if ( i == 0 ) {
			if ( maxs[i] > *width - 1 ) {
				maxs[i] = *width - 1;
			}
		} else {
			if ( maxs[i] > *height - 1 ) {
				maxs[i] = *height - 1;
			}
		}
	}

	// we have the mins/maxs, so work out the best square to crop to
	for ( i = 0; i < 2; i++ ) {
		diff[i] = maxs[i] - mins[i];
	}

	// widen the axis that has the smallest diff
	a = -1;
	if ( diff[1] > diff[0] ) {
		a = 0;
		max = *width - 1;
	} else if ( diff[0] > diff[1] ) {
		a = 1;
		max = *height - 1;
	}
	if ( a >= 0 ) {
		invalid = qfalse;
		while ( diff[a] < diff[!a] ) {
			if ( invalid ) {
				Com_Printf( "unable to find a good crop size\n" );
				return qfalse;
			}
			invalid = qtrue;
			if ( mins[a] > 0 ) {
				mins[a] -= 1;
				diff[a] = maxs[a] - mins[a];
				invalid = qfalse;
			}
			if ( ( diff[a] < diff[!a] ) && ( maxs[a] < max ) ) {
				maxs[a] += 1;
				diff[a] = maxs[a] - mins[a];
				invalid = qfalse;
			}
		}
	}

	// make sure it's bigger or equal to the last one
	for ( i = 0; i < 2; i++ ) {
		if ( ( maxs[i] - mins[i] ) < lastBox[i] ) {
			if ( i == 0 ) {
				center = width;
			} else { center = height;}

			maxs[i] = *center / 2 + ( lastBox[i] / 2 );
			mins[i] = maxs[i] - lastBox[i];
			diff[i] = lastBox[i];
		}
	}
#else
	for ( i = 0; i < 2; i++ ) {
		diff[i] = maxs[i] - mins[i];
		lastBox[i] = diff[i];
	}
#endif  // RESIZE
#endif  // FUNNEL_HACK

	temppic = (byte *)ri.Z_Malloc( sizeof( unsigned int ) * diff[0] * diff[1] );
	outpixel = (byte *)temppic;

	for ( row = mins[1]; row < maxs[1]; row++ )
	{
		inpixel = ( ( *pic ) + ( row * cols ) * 4 );
		inpixel += mins[0] * 4;

		for ( col = mins[0]; col < maxs[0]; col++ )
		{
			*outpixel++ = *( inpixel + 0 );   // blue
			*outpixel++ = *( inpixel + 1 );   // green
			*outpixel++ = *( inpixel + 2 );   // red
			*outpixel++ = *( inpixel + 3 );   // alpha

			inpixel += 4;
		}
	}

	// for some reason this causes memory drop, not worth investigating (dev command only)
	//ri.Free( *pic );

	*pic = temppic;

	*width = diff[0];
	*height = diff[1];

	return qtrue;
#else
	return qtrue;   // shutup the compiler
#endif
}


/*
===============
R_CropAndNumberImagesInDirectory
===============
*/
void    R_CropAndNumberImagesInDirectory( char *dir, char *ext, int maxWidth, int maxHeight, int withAlpha ) {
#ifdef CROPIMAGES_ENABLED
	char    **fileList;
	int numFiles, j;
#ifndef RESIZE
	int i;
#endif
	byte    *pic, *temppic;
	int width, height, newWidth, newHeight;
	GLenum picFormat;
	int picNumMips;
	char    *pch;
	int b,c,d,lastNumber;
	int lastBox[2] = {0,0};

	fileList = ri.FS_ListFiles( dir, ext, &numFiles );

	if ( !numFiles ) {
		ri.Printf( PRINT_ALL, "no '%s' files in directory '%s'\n", ext, dir );
		return;
	}

	ri.Printf( PRINT_ALL, "%i files found, beginning processing..\n", numFiles );

	for ( j = 0; j < numFiles; j++ ) {
		char filename[MAX_QPATH], outfilename[MAX_QPATH];

		if ( !Q_strncmp( fileList[j], "spr", 3 ) ) {
			continue;
		}

		Com_sprintf( filename, sizeof( filename ), "%s/%s", dir, fileList[j] );
		ri.Printf( PRINT_ALL, "...cropping '%s'.. ", filename );

		R_LoadImage( filename, &pic, &width, &height, &picFormat, &picNumMips );
		if ( !pic ) {
			ri.Printf( PRINT_ALL, "error reading file, ignoring.\n" );
			continue;
		}

		// file has been read, crop it, resize it down to a power of 2, then save
		if ( !R_CropImage( filename, &pic, 6, &width, &height, lastBox ) ) {
			ri.Printf( PRINT_ALL, "unable to crop image.\n" );
			//ri.Free( pic );
			break;
		}
#ifndef RESIZE
		for ( i = 2; ( 1 << i ) <= maxWidth; i++ ) {
			if ( ( width < ( 1 << i ) ) && ( width > ( 1 << ( i - 1 ) ) ) ) {
				newWidth = ( 1 << ( i - 1 ) );
				if ( newWidth > maxWidth ) {
					newWidth = maxWidth;
				}
				newHeight = newWidth;
				temppic = ri.Z_Malloc( sizeof( unsigned int ) * newWidth * newHeight );
				ResampleTexture( (unsigned int *)pic, width, height, (unsigned int *)temppic, newWidth, newHeight );
				memcpy( pic, temppic, sizeof( unsigned int ) * newWidth * newHeight );
				ri.Free( temppic );
				width = height = newWidth;
				break;
			}
		}
		if ( width > maxWidth ) {
			// we need to force the scale downwards
			newWidth = maxWidth;
			newHeight = maxWidth;
			temppic = ri.Z_Malloc( sizeof( unsigned int ) * newWidth * newHeight );
			ResampleTexture( (unsigned int *)pic, width, height, (unsigned int *)temppic, newWidth, newHeight );
			memcpy( pic, temppic, sizeof( unsigned int ) * newWidth * newHeight );
			ri.Free( temppic );
			width = newWidth;
			height = newHeight;
		}
#else
		newWidth = maxWidth;
		newHeight = maxHeight;
		temppic = (byte *)ri.Z_Malloc( sizeof( unsigned int ) * newWidth * newHeight );
		ResampleTexture( pic, width, height, temppic, newWidth, newHeight );
		memcpy( pic, temppic, sizeof( unsigned int ) * newWidth * newHeight );
		ri.Free( temppic );
		width = newWidth;
		height = newHeight;
#endif

		// set the new filename
		pch = strrchr( filename, '/' );
		*pch = '\0';
		lastNumber = j;
		b = lastNumber / 100;
		lastNumber -= b * 100;
		c = lastNumber / 10;
		lastNumber -= c * 10;
		d = lastNumber;
		Com_sprintf( outfilename, sizeof( outfilename ), "%s/spr%i%i%i.tga", filename, b, c, d );

		if ( withAlpha ) {
			SaveTGAAlpha( outfilename, &pic, width, height );
		} else {
			SaveTGA( outfilename, &pic, width, height );
		}

		// free the pixel data
		//ri.Free( pic );

		ri.Printf( PRINT_ALL, "done.\n" );
	}
#endif
}

/*
==============
R_CropImages_f
==============
*/
void R_CropImages_f( void ) {
#ifdef CROPIMAGES_ENABLED
	if ( ri.Cmd_Argc() < 5 ) {
		ri.Printf( PRINT_ALL, "syntax: cropimages <dir> <extension> <maxWidth> <maxHeight> <alpha 0/1>\neg: 'cropimages sprites/fire1 .tga 64 64 0'\n" );
		return;
	}
	R_CropAndNumberImagesInDirectory( ri.Cmd_Argv( 1 ), ri.Cmd_Argv( 2 ), atoi( ri.Cmd_Argv( 3 ) ), atoi( ri.Cmd_Argv( 4 ) ), atoi( ri.Cmd_Argv( 5 ) ) );
#else
	ri.Printf( PRINT_ALL, "This command has been disabled.\n" );
#endif
}
// done.

// tr_metal_device.mm — Metal device context singleton + frame lifecycle (Phase 5 bootstrap)
// NOT a 1:1 parity file — documented Metal bootstrap module.

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <string.h>
#include <vector>
#include <chrono>
#include <limits.h>

#include <dispatch/dispatch.h>
#include <TargetConditionals.h>
#include <objc/message.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGDirectDisplay.h>

#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <QuartzCore/CAMetalLayer.hpp>
#include <Foundation/NSAutoreleasePool.hpp>

extern "C" {
#include "../qcommon/q_shared.h"
#include "../renderer/tr_public.h"
extern refimport_t ri;
// Anisotropic filtering globals — defined in tr_init.cpp, declared extern in tr_common.h.
// GL2 ref: tr_init.c:30-31 (definitions), sdl_glimp.c:1031-1043 (assignment)
extern qboolean textureFilterAnisotropic;
extern int      maxAnisotropy;
}

#include "tr_metal_device.h"
#include "tr_metal_shadersource.h"
#include "tr_ssao.h"

// Mirror only the constants/layout needed for device-side pipeline setup.
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif
#ifndef GL_SHORT
#define GL_SHORT 0x1402
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT 0x1403
#endif
#ifndef GL_RGB
#define GL_RGB 0x1907
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_LUMINANCE
#define GL_LUMINANCE 0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA 0x190A
#endif
#ifndef GL_RGB5
#define GL_RGB5 0x8050
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
#ifndef GL_RGBA4
#define GL_RGBA4 0x8056
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#ifndef GL_LUMINANCE8
#define GL_LUMINANCE8 0x8040
#endif
#ifndef GL_LUMINANCE8_ALPHA8
#define GL_LUMINANCE8_ALPHA8 0x8045
#endif
#ifndef GL_SRGB_EXT
#define GL_SRGB_EXT 0x8C40
#endif
#ifndef GL_SRGB8_EXT
#define GL_SRGB8_EXT 0x8C41
#endif
#ifndef GL_SRGB_ALPHA_EXT
#define GL_SRGB_ALPHA_EXT 0x8C42
#endif
#ifndef GL_SRGB8_ALPHA8_EXT
#define GL_SRGB8_ALPHA8_EXT 0x8C43
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGBA16F_ARB
#define GL_RGBA16F_ARB 0x881A
#endif
#ifndef GL_R32F
#define GL_R32F 0x822E
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_REPEAT
#define GL_REPEAT 0x2901
#endif
#ifndef GL_NEVER
#define GL_NEVER 0x0200
#endif
#ifndef GL_LESS
#define GL_LESS 0x0201
#endif
#ifndef GL_EQUAL
#define GL_EQUAL 0x0202
#endif
#ifndef GL_LEQUAL
#define GL_LEQUAL 0x0203
#endif
#ifndef GL_GREATER
#define GL_GREATER 0x0204
#endif
#ifndef GL_NOTEQUAL
#define GL_NOTEQUAL 0x0205
#endif
#ifndef GL_GEQUAL
#define GL_GEQUAL 0x0206
#endif
#ifndef GL_ALWAYS
#define GL_ALWAYS 0x0207
#endif
#ifndef GL_ZERO
#define GL_ZERO 0
#endif
#ifndef GL_KEEP
#define GL_KEEP 0x1E00
#endif
#ifndef GL_REPLACE
#define GL_REPLACE 0x1E01
#endif
#ifndef GL_INCR
#define GL_INCR 0x1E02
#endif
#ifndef GL_DECR
#define GL_DECR 0x1E03
#endif
#ifndef GL_INVERT
#define GL_INVERT 0x150A
#endif
#ifndef GL_INCR_WRAP
#define GL_INCR_WRAP 0x8507
#endif
#ifndef GL_DECR_WRAP
#define GL_DECR_WRAP 0x8508
#endif
#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
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
#define GL_COMPRESSED_RED_RGTC1 0x8DBB
#endif
#ifndef GL_COMPRESSED_SIGNED_RED_RGTC1
#define GL_COMPRESSED_SIGNED_RED_RGTC1 0x8DBC
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif
#ifndef GL_COMPRESSED_SIGNED_RG_RGTC2
#define GL_COMPRESSED_SIGNED_RG_RGTC2 0x8DBE
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM_ARB
#define GL_COMPRESSED_RGBA_BPTC_UNORM_ARB 0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB 0x8E8D
#endif
#ifndef GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB
#define GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_ARB 0x8E8E
#endif
#ifndef GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB
#define GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_ARB 0x8E8F
#endif
#ifndef GL_RGB4_S3TC
#define GL_RGB4_S3TC 0x83A1
#endif

#define VAO_MAX_ATTRIBS 16
#define ATTR_INDEX_POSITION 0
#define ATTR_INDEX_TEXCOORD 1
#define ATTR_INDEX_NORMAL 4
#define ATTR_INDEX_COLOR 5
#define ATTR_POSITION (1 << ATTR_INDEX_POSITION)
#define ATTR_TEXCOORD (1 << ATTR_INDEX_TEXCOORD)

#define GLS_SRCBLEND_ZERO                  0x00000001
#define GLS_SRCBLEND_ONE                   0x00000002
#define GLS_SRCBLEND_DST_COLOR             0x00000003
#define GLS_SRCBLEND_ONE_MINUS_DST_COLOR   0x00000004
#define GLS_SRCBLEND_SRC_ALPHA             0x00000005
#define GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA   0x00000006
#define GLS_SRCBLEND_DST_ALPHA             0x00000007
#define GLS_SRCBLEND_ONE_MINUS_DST_ALPHA   0x00000008
#define GLS_SRCBLEND_ALPHA_SATURATE        0x00000009
#define GLS_SRCBLEND_BITS                  0x0000000f

#define GLS_DSTBLEND_ZERO                  0x00000010
#define GLS_DSTBLEND_ONE                   0x00000020
#define GLS_DSTBLEND_SRC_COLOR             0x00000030
#define GLS_DSTBLEND_ONE_MINUS_SRC_COLOR   0x00000040
#define GLS_DSTBLEND_SRC_ALPHA             0x00000050
#define GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA   0x00000060
#define GLS_DSTBLEND_DST_ALPHA             0x00000070
#define GLS_DSTBLEND_ONE_MINUS_DST_ALPHA   0x00000080
#define GLS_DSTBLEND_BITS                  0x000000f0

// PSO_AFFECTING_STATE_BITS: the subset of glStateBits that actually influences
// the MTLRenderPipelineState.  Only blend bits are read during pipeline creation:
//   GLS_SRCBLEND_BITS (0x0f) — ConfigureBlendState:1585 (srcBlendBits mask)
//   GLS_DSTBLEND_BITS (0xf0) — ConfigureBlendState:1586 (dstBlendBits mask)
// Depth/alpha-test bits (GLS_DEPTHMASK_TRUE, GLS_DEPTHTEST_DISABLE,
// GLS_DEPTHFUNC_BITS, GLS_ATEST_*) are NOT read here; they go to
// MTLDepthStencilState (s_depthStencilCache) or shader permutation selection.
// Masking the key to these bits prevents spurious cache misses when only
// non-PSO state changes between draw calls (F1 fix — dallas-perf-audit.md).
static const uint32_t PSO_AFFECTING_STATE_BITS = GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS;

#define GLS_DEPTHMASK_TRUE                 0x00000100
#define GLS_DEPTHTEST_DISABLE             0x00010000
#define GLS_DEPTHFUNC_EQUAL                0x00020000
#define GLS_DEPTHFUNC_GREATER              0x00040000
#define GLS_DEPTHFUNC_BITS                 0x00060000
#define GLS_DEPTH_BITS                     (GLS_DEPTHMASK_TRUE | GLS_DEPTHTEST_DISABLE | GLS_DEPTHFUNC_BITS)

// Compact stencil state encoding packed into s_stencilStateBits.
// Lower 32 bits remain depth-only state; packed stencil bits are folded into the
// upper 32 bits of the depth/stencil cache key when enabled.
static const uint32_t STENCIL_TEST_ENABLE_BIT      = 0x00000001u;
static const uint32_t STENCIL_COMPARE_SHIFT        = 1u;
static const uint32_t STENCIL_COMPARE_MASK         = (0x7u << STENCIL_COMPARE_SHIFT);
static const uint32_t STENCIL_PASS_SHIFT           = 4u;
static const uint32_t STENCIL_PASS_MASK            = (0x7u << STENCIL_PASS_SHIFT);
static const uint32_t STENCIL_DEPTH_FAIL_SHIFT     = 7u;
static const uint32_t STENCIL_DEPTH_FAIL_MASK      = (0x7u << STENCIL_DEPTH_FAIL_SHIFT);
static const uint32_t STENCIL_FAIL_SHIFT           = 10u;
static const uint32_t STENCIL_FAIL_MASK            = (0x7u << STENCIL_FAIL_SHIFT);
static const uint32_t STENCIL_REFERENCE_SHIFT      = 13u;
static const uint32_t STENCIL_REFERENCE_MASK       = (0xFFu << STENCIL_REFERENCE_SHIFT);

enum GLMetalStencilCompareBits : uint32_t {
	GLMetalStencilCompareAlways = 0,
	GLMetalStencilCompareNotEqual = 1,
	GLMetalStencilCompareEqual = 2,
	GLMetalStencilCompareLess = 3,
	GLMetalStencilCompareLessEqual = 4,
	GLMetalStencilCompareGreater = 5,
	GLMetalStencilCompareGreaterEqual = 6,
	GLMetalStencilCompareNever = 7
};

enum GLMetalStencilOpBits : uint32_t {
	GLMetalStencilOpKeep = 0,
	GLMetalStencilOpZero = 1,
	GLMetalStencilOpReplace = 2,
	GLMetalStencilOpIncrementClamp = 3,
	GLMetalStencilOpDecrementClamp = 4,
	GLMetalStencilOpInvert = 5,
	GLMetalStencilOpIncrementWrap = 6,
	GLMetalStencilOpDecrementWrap = 7
};

typedef struct vaoAttrib_s
{
	uint32_t enabled;
	uint32_t count;
	uint32_t type;
	uint32_t normalized;
	uint32_t stride;
	uint32_t offset;
} vaoAttrib_t;

struct vao_s
{
	char name[MAX_QPATH];
	uint32_t vao;
	uint32_t vertexesVBO;
	int vertexesSize;
	vaoAttrib_t attribs[VAO_MAX_ATTRIBS];
	uint32_t frameSize;
	uint32_t indexesIBO;
	int indexesSize;
};

// ============================================================================
// Device context singleton
// ============================================================================

static MTL::Device                *s_device = nullptr;
static MTL::CommandQueue          *s_queue = nullptr;
static CA::MetalLayer             *s_layer = nullptr;
static MTL::PixelFormat            s_colorPixelFormat = MTL::PixelFormatBGRA8Unorm;

// Per-frame state
static CA::MetalDrawable          *s_drawable = nullptr;
// The texture the frame's final (window) pass renders into: the drawable's texture, or
// s_offscreenTarget in r_metalNoPresent mode. Set when acquired, cleared at end of frame.
static MTL::Texture               *s_finalTarget = nullptr;
static MTL::Texture               *s_offscreenTarget = nullptr;
static cvar_t                     *s_noPresentCvar = nullptr;
static cvar_t                     *s_presentHzCvar = nullptr;
static uint64_t                    s_lastPresentNs = 0;   // time of the last presented frame
static MTL::CommandBuffer         *s_cmdBuffer = nullptr;
static MTL::RenderCommandEncoder  *s_encoder = nullptr;
static MTL::Texture               *s_depthTexture = nullptr;
struct MetalRenderTargetState {
	bool targetDrawable;
	MTL::Texture *colorTexture;
	MTL::Texture *depthTexture;
};
struct MetalRenderTargetClearState {
	bool pendingClearColor;
	bool pendingClearDepth;
	MTL::ClearColor clearColor;
	double clearDepth;
};
struct MetalRenderTargetKey {
	bool targetDrawable;
	uintptr_t colorTexturePtr;
	uintptr_t depthTexturePtr;

	bool operator==(const MetalRenderTargetKey &other) const
	{
		return targetDrawable == other.targetDrawable &&
		       colorTexturePtr == other.colorTexturePtr &&
		       depthTexturePtr == other.depthTexturePtr;
	}
};
// boost-style hash_combine: XOR-of-shifted-hashes (the previous scheme) collides heavily when
// the inputs are small integers/pointers with correlated low bits.
static inline void GLMetal_HashCombine(size_t &seed, size_t value)
{
	seed ^= value + (size_t)0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

struct MetalRenderTargetKeyHash {
	size_t operator()(const MetalRenderTargetKey &key) const
	{
		size_t h = std::hash<bool>{}(key.targetDrawable);
		GLMetal_HashCombine(h, std::hash<uintptr_t>{}(key.colorTexturePtr));
		GLMetal_HashCombine(h, std::hash<uintptr_t>{}(key.depthTexturePtr));
		return h;
	}
};
static const MTL::ClearColor        s_defaultClearColor = MTL::ClearColor::Make(0.15, 0.18, 0.25, 1.0);
static const double                 s_defaultClearDepth = 1.0;
static MetalRenderTargetState       s_renderTargetState = { true, nullptr, nullptr };
static MetalRenderTargetState       s_openRenderTargetState = { true, nullptr, nullptr };
static bool                         s_openRenderTargetValid = false;
static std::unordered_map<MetalRenderTargetKey, MetalRenderTargetClearState, MetalRenderTargetKeyHash> s_renderTargetClearStates;
static float                       s_depthRangeNear = 0.0f;
static float                       s_depthRangeFar = 1.0f;
static int                         s_viewportX = 0;
static int                         s_viewportY = 0;   // GL bottom-left coords
static int                         s_viewportW = 0;   // 0 means "unset -> use full drawable"
static int                         s_viewportH = 0;
static int                         s_cullMode = -1; // force first apply
static uint32_t                    s_depthStateBits = 0;
static uint32_t                    s_stencilStateBits = 0;
static bool                        s_depthStateValid = false;
// Depth bias (polygon offset) — GL2 ref: tr_init.c:1061 qglPolygonOffset(-1,-2)
// Encoder-local state; must be re-applied on every encoder restart.
static float                       s_depthBias      = 0.0f;
static float                       s_slopeScale     = 0.0f;
static float                       s_depthBiasClamp = 0.0f;
static int                         s_depthW = 0;
static int                         s_depthH = 0;
static bool                        s_frameBegun = false;
static bool                        s_firstFrame = true;
static int                         s_width = 0;
static int                         s_height = 0;
static MTL::Texture               *s_captureTexture = nullptr;
static int                         s_captureWidth = 0;
static int                         s_captureHeight = 0;
static MTL::PixelFormat            s_capturePixelFormat = MTL::PixelFormatInvalid;
static bool                        s_pipelineSelfTestDone = false;
static dispatch_semaphore_t        s_inFlightSemaphore = nullptr;
static bool                        s_inFlightFramePendingSignal = false;
static const long                  s_maxFramesInFlight = 3;
static bool                        s_frameTouchedDrawable = false;
static int                         s_appliedSwapInterval = INT_MIN;
static int                         s_seenSwapIntervalModificationCount = -1;

extern cvar_t *r_swapInterval;
extern cvar_t *r_shadowFilter;

struct PipelineCacheKey {
std::string programName;
uint32_t stateBits;
uint64_t vertexDescHash;
MTL::PixelFormat colorFormat;
MTL::PixelFormat depthStencilFormat;
MTL::PixelFormat stencilFormat;
uint8_t colorWriteMask;

bool operator==(const PipelineCacheKey &other) const
{
return stateBits == other.stateBits &&
       vertexDescHash == other.vertexDescHash &&
       colorFormat == other.colorFormat &&
       depthStencilFormat == other.depthStencilFormat &&
       stencilFormat == other.stencilFormat &&
       colorWriteMask == other.colorWriteMask &&
       programName == other.programName;
}
};

struct PipelineCacheKeyHash {
size_t operator()(const PipelineCacheKey &key) const
{
size_t h = std::hash<std::string>{}(key.programName);
GLMetal_HashCombine(h, std::hash<uint32_t>{}(key.stateBits));
GLMetal_HashCombine(h, std::hash<uint64_t>{}(key.vertexDescHash));
GLMetal_HashCombine(h, std::hash<uint32_t>{}((uint32_t)key.colorFormat));
GLMetal_HashCombine(h, std::hash<uint32_t>{}((uint32_t)key.depthStencilFormat));
GLMetal_HashCombine(h, std::hash<uint32_t>{}((uint32_t)key.stencilFormat));
GLMetal_HashCombine(h, std::hash<uint8_t>{}(key.colorWriteMask));
return h;
}
};

static std::unordered_map<PipelineCacheKey, MTL::RenderPipelineState *, PipelineCacheKeyHash> s_pipelineCache;
static std::unordered_set<PipelineCacheKey, PipelineCacheKeyHash> s_failedPipelineCache;
static std::unordered_map<uint64_t, MTL::DepthStencilState *> s_depthStencilCache;

struct MetalProgramFns {
	MTL::Function *vert;
	MTL::Function *frag;
	MTL::VertexDescriptor *vtxDesc;
	uint64_t vtxDescHash;
};

struct ProgramFnsKey {
	std::string programName;
	uint64_t vertexDescHash;

	bool operator==(const ProgramFnsKey &other) const
	{
		return vertexDescHash == other.vertexDescHash && programName == other.programName;
	}
};

struct ProgramFnsKeyHash {
	size_t operator()(const ProgramFnsKey &key) const
	{
		size_t h = std::hash<std::string>{}(key.programName);
		GLMetal_HashCombine(h, std::hash<uint64_t>{}(key.vertexDescHash));
		return h;
	}
};

// Registered programs. The id (index) is handed out to tr_glsl so draw-time PSO lookups
// avoid building/hashing std::string keys; s_programIds maps name+vtxHash -> id for the
// string-keyed API used by the blit paths.
struct MetalProgramPipelineEntry {
	uint64_t key;                     // blend bits | color format | depth format | write mask
	MTL::RenderPipelineState *pso;    // non-owning (s_pipelineCache owns); nullptr = failed
};
struct MetalProgramRecord {
	std::string name;
	MetalProgramFns fns;
	std::vector<MetalProgramPipelineEntry> pipelines;
};
static std::vector<MetalProgramRecord> s_programs;
static std::unordered_map<ProgramFnsKey, int, ProgramFnsKeyHash> s_programIds;

// Async PSO prewarm results, produced on Metal's compiler threads and drained on the render
// thread into s_pipelineCache.
struct MetalPendingPipeline {
	PipelineCacheKey key;
	MTL::RenderPipelineState *pso; // retained (+1)
};
static std::mutex s_pendingPipelinesMutex;
static std::vector<MetalPendingPipeline> s_pendingPipelines;
static std::atomic<int> s_pendingPipelineCompiles(0);

// texnum -> texture. texnums are small dense integers handed out sequentially by
// tr_image.cpp, so a flat array replaces the former unordered_map lookup on the draw path.
static std::vector<MTL::Texture *> s_textureTable;
// Bumped whenever a texnum's texture is replaced/deleted or samplers are flushed, so
// resolved texture/sampler pointers cached in s_textureBindings are re-resolved.
static uint32_t s_bindingGeneration = 1;
static std::unordered_map<uint32_t, MTL::SamplerState *> s_samplerCache;
static MTL::Texture *s_whiteTexture = nullptr;
static MTL::RenderPipelineState *s_tonemapPipeline = nullptr;
static MTL::RenderPipelineState *s_greyscalePipeline = nullptr;
static MTL::RenderPipelineState *s_shadowmaskPipeline = nullptr;
static MTL::PixelFormat s_shadowmaskPipelineColorFormat = MTL::PixelFormatInvalid;
static int s_shadowmaskPipelineFilterLevel = INT_MIN;
static MTL::RenderPipelineState *s_depthBlurPipeline[2][2] = { { nullptr, nullptr }, { nullptr, nullptr } };
static MTL::PixelFormat s_depthBlurPipelineColorFormat = MTL::PixelFormatInvalid;
static MTL::Function *s_shadowvolumeVertFn = nullptr;
static MTL::Function *s_shadowvolumeFragFn = nullptr;
static MTL::Function *s_shadowfinishVertFn = nullptr;
static MTL::Function *s_shadowfinishFragFn = nullptr;
// GL2 ref: PARITY DEVIATION (E3/B6 SAO compute) — first compute PSO in this file.
static MTL::ComputePipelineState *s_saoPipeline = nullptr;
static uint32_t s_saoFrameCounter = 0;  // monotonic frame index; wraps at UINT32_MAX (fine for dither seed)
static MTL::SamplerState *s_tonemapSampler = nullptr;

static const int METAL_MAX_TEXTURE_BINDINGS = 8;

struct MetalTextureBinding {
	uint32_t texnum;
	int wrapClampMode;
	qboolean filterLinear;
	qboolean mipmap;
	qboolean mipLinear;  // F3: trilinear vs bilinear — true = GL_LINEAR_MIPMAP_LINEAR, false = GL_LINEAR_MIPMAP_NEAREST
	qboolean valid;
	// Resolved objects for the parameters above; valid while resolvedGeneration == s_bindingGeneration.
	MTL::Texture *resolvedTexture;
	MTL::SamplerState *resolvedSampler;
	uint32_t resolvedGeneration;
};

// ---------------------------------------------------------------------------
// Encoder-local state cache: what has actually been set on s_encoder since it was opened.
// Lets the per-draw path skip redundant Objective-C encoder calls. Reset by
// GLMetal_OnEncoderOpened() every time s_encoder is (re)created.
// ---------------------------------------------------------------------------
static const int METAL_MAX_BUFFER_SLOTS = 31;
struct MetalEncoderStateCache {
	MTL::RenderPipelineState *pipeline;
	MTL::DepthStencilState *depthStencil;
	uint32_t stencilReference;
	bool stencilReferenceValid;
	int cullMode;                 // -1 = unknown
	bool viewportValid;
	MTL::Viewport viewport;
	MTL::ScissorRect scissor;
	bool depthBiasValid;
	float depthBias, slopeScale, depthBiasClamp;
	MTL::Texture *fragmentTexture[METAL_MAX_TEXTURE_BINDINGS];
	MTL::SamplerState *fragmentSampler[METAL_MAX_TEXTURE_BINDINGS];
	MTL::Buffer *vertexBuffer[METAL_MAX_BUFFER_SLOTS];
	NS::UInteger vertexOffset[METAL_MAX_BUFFER_SLOTS];
	MTL::Buffer *fragmentBuffer[METAL_MAX_BUFFER_SLOTS];
	NS::UInteger fragmentOffset[METAL_MAX_BUFFER_SLOTS];
};
static MetalEncoderStateCache s_encState;

// Depth clamp (GL_DEPTH_CLAMP) is encoder-local: re-applied whenever an encoder is opened.
static bool s_depthClamp = false;

// Depth-stencil state memo: skips the s_depthStencilCache hash lookup when the key repeats.
static uint64_t s_lastDepthStencilKey = ~0ull;
static MTL::DepthStencilState *s_lastDepthStencilState = nullptr;

// Current render target attachment formats, memoized per render-target identity so the
// draw path does not query texture->pixelFormat() (an Objective-C message) per draw.
struct MetalRenderTargetFormats {
	bool valid;
	bool targetDrawable;
	MTL::Texture *color;
	MTL::Texture *depth;
	uint32_t generation;
	MTL::PixelFormat colorFormat;
	MTL::PixelFormat depthStencilFormat;
};
static MetalRenderTargetFormats s_rtFormats = { false, false, nullptr, nullptr, 0, MTL::PixelFormatInvalid, MTL::PixelFormatInvalid };

// ---------------------------------------------------------------------------
// Per-frame transient memory arenas (vertex/index/uniform streaming).
// One arena per in-flight frame slot; the in-flight semaphore guarantees the GPU has finished
// with an arena before GLMetal_BeginFrame hands it out again.
// ---------------------------------------------------------------------------
struct MetalFrameArenaChunk {
	MTL::Buffer *buffer;
	uint8_t *cpu;
	size_t size;
	size_t used;
};
struct MetalFrameArena {
	std::vector<MetalFrameArenaChunk> chunks;
	size_t current;
};
static const size_t kFrameArenaChunkSize = 4u * 1024u * 1024u;
static MetalFrameArena s_frameArenas[s_maxFramesInFlight];
static int s_frameArenaIndex = 0;

// ---------------------------------------------------------------------------
// Texture upload batching. Texture contents are staged through shared buffers and copied
// into GPU-private textures by one blit command buffer that is committed ahead of the next
// frame command buffer (same queue => ordered), instead of replaceRegion on shared textures
// plus a blocking command buffer per mipmapped texture.
// ---------------------------------------------------------------------------
static MTL::CommandBuffer *s_uploadCmdBuffer = nullptr;
static MTL::BlitCommandEncoder *s_uploadBlit = nullptr;
static size_t s_uploadPendingBytes = 0;
static const size_t kUploadFlushThreshold = 64u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// On-demand readback capture.
// ---------------------------------------------------------------------------
static int s_captureRequestFrames = 0;            // presents still to capture
static uint64_t s_presentSerial = 0;              // number of frames presented
static uint64_t s_captureSerial = 0;              // s_presentSerial value of the captured frame
static MTL::CommandBuffer *s_captureCmdBuffer = nullptr; // retained; command buffer of the last capture

static MetalTextureBinding s_textureBindings[METAL_MAX_TEXTURE_BINDINGS];
static bool s_textureBindingRangeWarned = false;
static bool s_blitAliasWarned = false;
static bool s_blitMsaaWarned = false;

static MTL::Device *MetalDevice(void)
{
	return s_device;
}

static MTL::CommandQueue *MetalQueue(void)
{
	return s_queue;
}

static std::string ProgramNameKey(const char *programName)
{
	return (programName && programName[0]) ? programName : "unnamed";
}

// Called every time s_encoder is (re)created: nothing has been set on the new encoder yet.
// Winding is constant for the whole renderer (see GLMetal_ApplyCullMode), so set it once here.
static void GLMetal_OnEncoderOpened(void)
{
	memset(&s_encState, 0, sizeof(s_encState));
	s_encState.cullMode = -1;
	if (s_encoder) {
		s_encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
		// A new encoder starts in DepthClipModeClip.
		if (s_depthClamp) {
			s_encoder->setDepthClipMode(MTL::DepthClipModeClamp);
		}
	}
}

// GL2 ref: tr_backend.c:915-917,1142-1144 qglEnable/qglDisable(GL_DEPTH_CLAMP)
extern "C" void GLMetal_SetDepthClamp(qboolean enable)
{
	const bool clamp = enable ? true : false;
	if (clamp == s_depthClamp) {
		return;
	}
	s_depthClamp = clamp;
	if (s_encoder) {
		s_encoder->setDepthClipMode(clamp ? MTL::DepthClipModeClamp : MTL::DepthClipModeClip);
	}
}

static void GLMetal_SetEncoderCullMode(MTL::CullMode mode)
{
	if (!s_encoder || s_encState.cullMode == (int)mode) {
		return;
	}
	s_encoder->setCullMode(mode);
	s_encState.cullMode = (int)mode;
}

static void GLMetal_SetEncoderDepthStencilState(MTL::DepthStencilState *state)
{
	if (!s_encoder || !state || s_encState.depthStencil == state) {
		return;
	}
	s_encoder->setDepthStencilState(state);
	s_encState.depthStencil = state;
}

static void GLMetal_SetEncoderStencilReference(uint32_t ref)
{
	if (!s_encoder || (s_encState.stencilReferenceValid && s_encState.stencilReference == ref)) {
		return;
	}
	s_encoder->setStencilReferenceValue(ref);
	s_encState.stencilReference = ref;
	s_encState.stencilReferenceValid = true;
}

// Forget what the encoder has bound at a vertex/fragment buffer slot (after a direct
// setVertexBytes/setFragmentBytes that bypassed the cache).
static void GLMetal_InvalidateVertexSlot(NS::UInteger slot)
{
	if (slot < (NS::UInteger)METAL_MAX_BUFFER_SLOTS) {
		s_encState.vertexBuffer[slot] = nullptr;
	}
}

static void GLMetal_InvalidateFragmentSlot(NS::UInteger slot)
{
	if (slot < (NS::UInteger)METAL_MAX_BUFFER_SLOTS) {
		s_encState.fragmentBuffer[slot] = nullptr;
	}
}

void GLMetal_SetRenderPipelineState(MTL::RenderPipelineState *pso)
{
	if (!s_encoder || !pso || s_encState.pipeline == pso) {
		return;
	}
	s_encoder->setRenderPipelineState(pso);
	s_encState.pipeline = pso;
}

void GLMetal_SetVertexBuffer(MTL::Buffer *buffer, NS::UInteger offset, NS::UInteger slot)
{
	if (!s_encoder || !buffer) {
		return;
	}
	if (slot >= (NS::UInteger)METAL_MAX_BUFFER_SLOTS) {
		s_encoder->setVertexBuffer(buffer, offset, slot);
		return;
	}
	if (s_encState.vertexBuffer[slot] == buffer) {
		if (s_encState.vertexOffset[slot] != offset) {
			s_encoder->setVertexBufferOffset(offset, slot);
			s_encState.vertexOffset[slot] = offset;
		}
		return;
	}
	s_encoder->setVertexBuffer(buffer, offset, slot);
	s_encState.vertexBuffer[slot] = buffer;
	s_encState.vertexOffset[slot] = offset;
}

void GLMetal_SetFragmentBuffer(MTL::Buffer *buffer, NS::UInteger offset, NS::UInteger slot)
{
	if (!s_encoder || !buffer) {
		return;
	}
	if (slot >= (NS::UInteger)METAL_MAX_BUFFER_SLOTS) {
		s_encoder->setFragmentBuffer(buffer, offset, slot);
		return;
	}
	if (s_encState.fragmentBuffer[slot] == buffer) {
		if (s_encState.fragmentOffset[slot] != offset) {
			s_encoder->setFragmentBufferOffset(offset, slot);
			s_encState.fragmentOffset[slot] = offset;
		}
		return;
	}
	s_encoder->setFragmentBuffer(buffer, offset, slot);
	s_encState.fragmentBuffer[slot] = buffer;
	s_encState.fragmentOffset[slot] = offset;
}

// ---------------------------------------------------------------------------
// Frame arenas
// ---------------------------------------------------------------------------
static void GLMetal_ResetFrameArena(MetalFrameArena &arena)
{
	for (auto &chunk : arena.chunks) {
		chunk.used = 0;
	}
	arena.current = 0;
}

static void GLMetal_ReleaseFrameArenas(void)
{
	for (auto &arena : s_frameArenas) {
		for (auto &chunk : arena.chunks) {
			if (chunk.buffer) {
				chunk.buffer->release();
			}
		}
		arena.chunks.clear();
		arena.current = 0;
	}
	s_frameArenaIndex = 0;
}

static void *GLMetal_ArenaAlloc(MetalFrameArena &arena, size_t size, size_t alignment, MTL::Buffer **outBuffer, NS::UInteger *outOffset)
{
	if (alignment == 0) {
		alignment = 16;
	}
	// Find room in the current chunk or any later (already reset) chunk.
	while (arena.current < arena.chunks.size()) {
		MetalFrameArenaChunk &chunk = arena.chunks[arena.current];
		const size_t offset = (chunk.used + alignment - 1) & ~(alignment - 1);
		if (offset + size <= chunk.size) {
			chunk.used = offset + size;
			*outBuffer = chunk.buffer;
			*outOffset = (NS::UInteger)offset;
			return chunk.cpu + offset;
		}
		arena.current++;
	}

	if (!s_device) {
		return nullptr;
	}
	MetalFrameArenaChunk chunk;
	chunk.size = std::max(kFrameArenaChunkSize, size);
	chunk.buffer = s_device->newBuffer((NS::UInteger)chunk.size, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined);
	if (!chunk.buffer) {
		ri.Printf(PRINT_WARNING, "[Metal] frame arena: failed to allocate %zu-byte chunk\n", chunk.size);
		return nullptr;
	}
	chunk.cpu = (uint8_t *)chunk.buffer->contents();
	chunk.used = size;
	arena.chunks.push_back(chunk);
	arena.current = arena.chunks.size() - 1;
	*outBuffer = chunk.buffer;
	*outOffset = 0;
	return chunk.cpu;
}

void *GLMetal_FrameAlloc(size_t size, size_t alignment, MTL::Buffer **outBuffer, NS::UInteger *outOffset)
{
	if (!outBuffer || !outOffset || size == 0) {
		return nullptr;
	}
	if (!s_frameBegun) {
		GLMetal_BeginFrame();
	}
	if (!s_frameBegun) {
		return nullptr;
	}
	return GLMetal_ArenaAlloc(s_frameArenas[s_frameArenaIndex], size, alignment, outBuffer, outOffset);
}

// Constant-address-space buffer offsets must be 256-byte aligned on macOS GPUs.
static const size_t kUniformAlignment = 256;

void GLMetal_SetSharedUniforms(const void *bytes, size_t size, NS::UInteger vertexSlot, NS::UInteger fragmentSlot)
{
	if (!s_encoder || !bytes || size == 0) {
		return;
	}
	MTL::Buffer *buffer = nullptr;
	NS::UInteger offset = 0;
	void *dst = GLMetal_FrameAlloc(size, kUniformAlignment, &buffer, &offset);
	if (!dst) {
		// Fallback: inline bytes (the encoder copies them itself).
		s_encoder->setVertexBytes(bytes, (NS::UInteger)size, vertexSlot);
		s_encoder->setFragmentBytes(bytes, (NS::UInteger)size, fragmentSlot);
		GLMetal_InvalidateVertexSlot(vertexSlot);
		GLMetal_InvalidateFragmentSlot(fragmentSlot);
		return;
	}
	memcpy(dst, bytes, size);
	GLMetal_SetVertexBuffer(buffer, offset, vertexSlot);
	GLMetal_SetFragmentBuffer(buffer, offset, fragmentSlot);
}

static bool GLMetal_RenderTargetsEqual(const MetalRenderTargetState &a, const MetalRenderTargetState &b)
{
	return a.targetDrawable == b.targetDrawable && a.colorTexture == b.colorTexture && a.depthTexture == b.depthTexture;
}

static MetalRenderTargetKey GLMetal_MakeRenderTargetKey(const MetalRenderTargetState &target)
{
	MetalRenderTargetKey key;
	key.targetDrawable = target.targetDrawable;
	key.colorTexturePtr = (uintptr_t)(void *)target.colorTexture;
	key.depthTexturePtr = (uintptr_t)(void *)target.depthTexture;
	return key;
}

static MetalRenderTargetClearState &GLMetal_GetOrCreateRenderTargetClearState(const MetalRenderTargetState &target)
{
	const MetalRenderTargetKey key = GLMetal_MakeRenderTargetKey(target);
	auto it = s_renderTargetClearStates.find(key);
	if (it != s_renderTargetClearStates.end()) {
		return it->second;
	}
	MetalRenderTargetClearState state = { false, false, s_defaultClearColor, s_defaultClearDepth };
	return s_renderTargetClearStates.emplace(key, state).first->second;
}

static void ReleaseProgramFns(MetalProgramFns &fns)
{
	if (fns.vert) {
		fns.vert->release();
	}
	if (fns.frag) {
		fns.frag->release();
	}
	if (fns.vtxDesc) {
		fns.vtxDesc->release();
	}
	fns = { nullptr, nullptr, nullptr, 0 };
}

static const char *MetalErrorString(NS::Error *error)
{
if (!error) {
return "unknown error";
}
NS::String *desc = error->localizedDescription();
if (!desc) {
return "unknown error";
}
return desc->utf8String();
}

// GL internalFormat -> Metal pixel format mapping for tr_image upload seam.
// GL2 ref: tr_image.c:1966, tr_image_dds.c:216
// DDS/GL2 BCx tokens map 1:1 to Metal block-compressed formats; upload row
// pitch is computed in 4x4 blocks below, never as width*4.
static MTL::PixelFormat GLInternalFormatToMetalPixelFormat(int internalFormat)
{
switch (internalFormat) {
case GL_RGBA:
case GL_RGBA8:
case GL_RGBA4:
case GL_RGBA16:
return MTL::PixelFormatRGBA8Unorm;
case GL_SRGB_ALPHA_EXT:
case GL_SRGB8_ALPHA8_EXT:
return MTL::PixelFormatRGBA8Unorm_sRGB;
case GL_LUMINANCE:
case GL_LUMINANCE8:
return MTL::PixelFormatR8Unorm;
case GL_LUMINANCE_ALPHA:
case GL_LUMINANCE8_ALPHA8:
return MTL::PixelFormatRG8Unorm;
case GL_RGB:
case GL_RGB8:
case GL_RGB5:
return MTL::PixelFormatRGBA8Unorm;
case GL_SRGB_EXT:
case GL_SRGB8_EXT:
return MTL::PixelFormatRGBA8Unorm_sRGB;
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
ri.Printf(PRINT_WARNING, "[Metal] Unsupported internal format 0x%X, defaulting to RGBA8\n", internalFormat);
return MTL::PixelFormatRGBA8Unorm;
}
}

static int MetalCompressedBlockBytes(MTL::PixelFormat pixelFormat)
{
switch (pixelFormat) {
case MTL::PixelFormatBC1_RGBA:
case MTL::PixelFormatBC1_RGBA_sRGB:
case MTL::PixelFormatBC4_RUnorm:
case MTL::PixelFormatBC4_RSnorm:
return 8;
case MTL::PixelFormatBC2_RGBA:
case MTL::PixelFormatBC2_RGBA_sRGB:
case MTL::PixelFormatBC3_RGBA:
case MTL::PixelFormatBC3_RGBA_sRGB:
case MTL::PixelFormatBC5_RGUnorm:
case MTL::PixelFormatBC5_RGSnorm:
case MTL::PixelFormatBC6H_RGBFloat:
case MTL::PixelFormatBC6H_RGBUfloat:
case MTL::PixelFormatBC7_RGBAUnorm:
case MTL::PixelFormatBC7_RGBAUnorm_sRGB:
return 16;
default:
return 0;
}
}

static bool MetalIsBlockCompressedFormat(MTL::PixelFormat pixelFormat)
{
return MetalCompressedBlockBytes(pixelFormat) != 0;
}

static bool MetalIsFloatColorFormat(MTL::PixelFormat pixelFormat)
{
	switch (pixelFormat) {
	case MTL::PixelFormatR16Float:
	case MTL::PixelFormatRG16Float:
	case MTL::PixelFormatRGBA16Float:
	case MTL::PixelFormatR32Float:
	case MTL::PixelFormatRG32Float:
	case MTL::PixelFormatRGBA32Float:
		return true;
	default:
		return false;
	}
}

static int MetalBytesPerPixel(MTL::PixelFormat pixelFormat)
{
switch (pixelFormat) {
case MTL::PixelFormatR8Unorm:
return 1;
case MTL::PixelFormatRG8Unorm:
return 2;
case MTL::PixelFormatR16Float:
return 2;
case MTL::PixelFormatRG16Float:
return 4;
case MTL::PixelFormatRGBA8Unorm:
case MTL::PixelFormatRGBA8Unorm_sRGB:
case MTL::PixelFormatBGRA8Unorm:
case MTL::PixelFormatBGRA8Unorm_sRGB:
case MTL::PixelFormatR32Float:
case MTL::PixelFormatDepth32Float:
return 4;
case MTL::PixelFormatRGBA16Float:
case MTL::PixelFormatRG32Float:
case MTL::PixelFormatDepth32Float_Stencil8:
return 8;
case MTL::PixelFormatRGBA32Float:
return 16;
default:
return 4;
}
}

static bool MetalUploadFormatIsRGBA8Source(int uploadFormat)
{
	switch (uploadFormat) {
	case GL_RGBA:
	case GL_RGBA8:
	case GL_SRGB_ALPHA_EXT:
	case GL_SRGB8_ALPHA8_EXT:
		return true;
	default:
		return false;
	}
}

static uint16_t MetalFloat32ToHalfBits(float value)
{
	uint32_t bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	const uint32_t sign = (bits >> 16) & 0x8000u;
	int exp = (int)((bits >> 23) & 0xFFu) - 127 + 15;
	uint32_t mantissa = bits & 0x7FFFFFu;

	if (exp <= 0) {
		if (exp < -10) {
			return (uint16_t)sign;
		}
		mantissa |= 0x800000u;
		const uint32_t shift = (uint32_t)(14 - exp);
		uint32_t rounded = mantissa >> shift;
		if ((mantissa >> (shift - 1)) & 1u) {
			rounded++;
		}
		return (uint16_t)(sign | rounded);
	}

	if (exp >= 31) {
		return (uint16_t)(sign | 0x7C00u);
	}

	uint16_t halfExp = (uint16_t)((uint32_t)exp << 10);
	uint16_t halfMantissa = (uint16_t)(mantissa >> 13);
	if (mantissa & 0x1000u) {
		halfMantissa++;
		if (halfMantissa == 0x400u) {
			halfMantissa = 0;
			halfExp += 0x0400u;
			if (halfExp >= 0x7C00u) {
				halfExp = 0x7C00u;
			}
		}
	}
	return (uint16_t)(sign | halfExp | halfMantissa);
}

static bool MetalConvertRGBA8SourceForFloatDest(const byte *srcPixels, int width, int height, MTL::PixelFormat dstPixelFormat, std::vector<byte> &outBytes)
{
	const int pixelCount = width * height;
	const float byteToUnit = 1.0f / 255.0f;

	if (dstPixelFormat == MTL::PixelFormatR16Float) {
		outBytes.resize((size_t)pixelCount * 2u);
		uint16_t *dst = (uint16_t *)outBytes.data();
		for (int i = 0; i < pixelCount; ++i) {
			dst[i] = MetalFloat32ToHalfBits((float)srcPixels[i * 4] * byteToUnit);
		}
		return true;
	}

	if (dstPixelFormat == MTL::PixelFormatRG16Float) {
		outBytes.resize((size_t)pixelCount * 4u);
		uint16_t *dst = (uint16_t *)outBytes.data();
		for (int i = 0; i < pixelCount; ++i) {
			const int srcBase = i * 4;
			const int dstBase = i * 2;
			dst[dstBase + 0] = MetalFloat32ToHalfBits((float)srcPixels[srcBase + 0] * byteToUnit);
			dst[dstBase + 1] = MetalFloat32ToHalfBits((float)srcPixels[srcBase + 1] * byteToUnit);
		}
		return true;
	}

	if (dstPixelFormat == MTL::PixelFormatRGBA16Float) {
		outBytes.resize((size_t)pixelCount * 8u);
		uint16_t *dst = (uint16_t *)outBytes.data();
		for (int i = 0; i < pixelCount; ++i) {
			const int srcBase = i * 4;
			const int dstBase = i * 4;
			dst[dstBase + 0] = MetalFloat32ToHalfBits((float)srcPixels[srcBase + 0] * byteToUnit);
			dst[dstBase + 1] = MetalFloat32ToHalfBits((float)srcPixels[srcBase + 1] * byteToUnit);
			dst[dstBase + 2] = MetalFloat32ToHalfBits((float)srcPixels[srcBase + 2] * byteToUnit);
			dst[dstBase + 3] = MetalFloat32ToHalfBits((float)srcPixels[srcBase + 3] * byteToUnit);
		}
		return true;
	}

	if (dstPixelFormat == MTL::PixelFormatR32Float) {
		outBytes.resize((size_t)pixelCount * 4u);
		float *dst = (float *)outBytes.data();
		for (int i = 0; i < pixelCount; ++i) {
			dst[i] = (float)srcPixels[i * 4] * byteToUnit;
		}
		return true;
	}

	if (dstPixelFormat == MTL::PixelFormatRG32Float) {
		outBytes.resize((size_t)pixelCount * 8u);
		float *dst = (float *)outBytes.data();
		for (int i = 0; i < pixelCount; ++i) {
			const int srcBase = i * 4;
			const int dstBase = i * 2;
			dst[dstBase + 0] = (float)srcPixels[srcBase + 0] * byteToUnit;
			dst[dstBase + 1] = (float)srcPixels[srcBase + 1] * byteToUnit;
		}
		return true;
	}

	if (dstPixelFormat == MTL::PixelFormatRGBA32Float) {
		outBytes.resize((size_t)pixelCount * 16u);
		float *dst = (float *)outBytes.data();
		for (int i = 0; i < pixelCount; ++i) {
			const int srcBase = i * 4;
			const int dstBase = i * 4;
			dst[dstBase + 0] = (float)srcPixels[srcBase + 0] * byteToUnit;
			dst[dstBase + 1] = (float)srcPixels[srcBase + 1] * byteToUnit;
			dst[dstBase + 2] = (float)srcPixels[srcBase + 2] * byteToUnit;
			dst[dstBase + 3] = (float)srcPixels[srcBase + 3] * byteToUnit;
		}
		return true;
	}

	return false;
}

static void MetalTextureUploadLayout(MTL::PixelFormat pixelFormat, int width, int height, int *bytesPerRow, int *bytesPerImage)
{
const int blockBytes = MetalCompressedBlockBytes(pixelFormat);
if (blockBytes) {
	// GL2 ref: tr_image.c:1966 — BC formats are 4x4 texel blocks.
	const int blocksWide = (width + 3) / 4;
	const int blocksHigh = (height + 3) / 4;
	*bytesPerRow = blocksWide * blockBytes;
	*bytesPerImage = *bytesPerRow * blocksHigh;
	return;
}

*bytesPerRow = width * MetalBytesPerPixel(pixelFormat);
*bytesPerImage = *bytesPerRow * height;
}

static int MetalMipLevelCount(int width, int height, qboolean mipmap)
{
if (!mipmap) {
return 1;
}

int levels = 1;
int w = width;
int h = height;
while (w > 1 || h > 1) {
w = (w > 1) ? (w >> 1) : 1;
h = (h > 1) ? (h >> 1) : 1;
levels++;
}
return levels;
}

static MTL::Texture *CreateDefaultWhiteTexture(MTL::Device *device)
{
if (!device) {
return nullptr;
}

MTL::TextureDescriptor *desc = MTL::TextureDescriptor::alloc()->init();
desc->setTextureType(MTL::TextureType2D);
desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
desc->setWidth(1);
desc->setHeight(1);
desc->setMipmapLevelCount(1);
desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
desc->setStorageMode(MTL::StorageModeShared);

MTL::Texture *texture = device->newTexture(desc);
desc->release();
if (!texture) {
return nullptr;
}

const uint32_t white = 0xFFFFFFFFu;
MTL::Region region = MTL::Region::Make2D(0, 0, 1, 1);
texture->replaceRegion(region, 0, &white, 4);
return texture;
}

static bool GLMetal_EnsureCaptureTexture(int width, int height, MTL::PixelFormat pixelFormat)
{
if (!s_device || width <= 0 || height <= 0 || pixelFormat == MTL::PixelFormatInvalid) {
return false;
}

if (s_captureTexture && s_captureWidth == width && s_captureHeight == height && s_capturePixelFormat == pixelFormat) {
return true;
}

MTL::TextureDescriptor *desc = MTL::TextureDescriptor::texture2DDescriptor(
    pixelFormat, (NS::UInteger)width, (NS::UInteger)height, false);
if (!desc) {
return false;
}
desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
// Unified-memory GPUs can read a shared texture directly (no synchronizeResource copy);
// discrete GPUs need managed storage for CPU readback.
desc->setStorageMode(s_device->hasUnifiedMemory() ? MTL::StorageModeShared : MTL::StorageModeManaged);

MTL::Texture *texture = s_device->newTexture(desc);
// texture2DDescriptor returns autoreleased; release our implicit +1
desc->release();
if (!texture) {
ri.Printf(PRINT_WARNING, "[Metal] failed to create readback capture texture %dx%d\n", width, height);
return false;
}

if (s_captureTexture) {
s_captureTexture->release();
}
s_captureTexture = texture;
s_captureWidth = width;
s_captureHeight = height;
s_capturePixelFormat = pixelFormat;
s_captureSerial = 0; // new texture holds no capture yet
return true;
}

static bool GLMetal_EnsureDepthTexture(int width, int height)
{
if (!s_device || width <= 0 || height <= 0) {
return false;
}

if (s_depthTexture && s_depthW == width && s_depthH == height) {
return true;
}

MTL::TextureDescriptor *desc = MTL::TextureDescriptor::texture2DDescriptor(
    MTL::PixelFormatDepth32Float_Stencil8, (NS::UInteger)width, (NS::UInteger)height, false);
if (!desc) {
return false;
}
desc->setUsage(MTL::TextureUsageRenderTarget);
desc->setStorageMode(MTL::StorageModePrivate);

MTL::Texture *texture = s_device->newTexture(desc);
// texture2DDescriptor returns autoreleased; release our implicit +1
desc->release();
if (!texture) {
ri.Printf(PRINT_WARNING, "[Metal] failed to create depth texture %dx%d\n", width, height);
return false;
}

if (s_depthTexture) {
s_depthTexture->release();
}
s_depthTexture = texture;
s_depthW = width;
s_depthH = height;
return true;
}

static void GLMetal_RefreshRenderTargetFormats(void)
{
	const MetalRenderTargetState &rt = s_renderTargetState;
	MTL::Texture *depth = rt.depthTexture;
	if (!depth && rt.targetDrawable) {
		depth = s_depthTexture;
	}
	if (s_rtFormats.valid &&
	    s_rtFormats.targetDrawable == rt.targetDrawable &&
	    s_rtFormats.color == rt.colorTexture &&
	    s_rtFormats.depth == depth &&
	    s_rtFormats.generation == s_bindingGeneration) {
		return;
	}

	MTL::PixelFormat colorFormat;
	if (rt.targetDrawable) {
		// The drawable's texture always has the layer's pixel format.
		colorFormat = s_colorPixelFormat;
	} else if (rt.colorTexture) {
		colorFormat = rt.colorTexture->pixelFormat();
	} else if (rt.depthTexture) {
		// GL2 ref: depth-only FBO (pshadow capture) has no color attachment.
		// Invalid => PSOs are created without a color attachment, matching the pass descriptor.
		colorFormat = MTL::PixelFormatInvalid;
	} else {
		colorFormat = s_colorPixelFormat;
	}

	s_rtFormats.valid = true;
	s_rtFormats.targetDrawable = rt.targetDrawable;
	s_rtFormats.color = rt.colorTexture;
	s_rtFormats.depth = depth;
	s_rtFormats.generation = s_bindingGeneration;
	s_rtFormats.colorFormat = colorFormat;
	s_rtFormats.depthStencilFormat = depth ? depth->pixelFormat() : MTL::PixelFormatInvalid;
}

static MTL::PixelFormat GLMetal_CurrentRenderTargetColorFormat(void)
{
	GLMetal_RefreshRenderTargetFormats();
	return s_rtFormats.colorFormat;
}

static MTL::PixelFormat GLMetal_StencilFormatForDepthStencilFormat(MTL::PixelFormat depthStencilFormat)
{
	// PARITY-SEAM(Metal-device): Metal PSOs encode stencil format separately; only
	// combined depth-stencil targets expose a stencil attachment format.
	if (depthStencilFormat == MTL::PixelFormatDepth32Float_Stencil8) {
		return MTL::PixelFormatDepth32Float_Stencil8;
	}
	return MTL::PixelFormatInvalid;
}

static bool GLMetal_CurrentTargetSize(int *width, int *height)
{
int w = 0;
int h = 0;

MTL::Texture *color = s_renderTargetState.colorTexture;
if (s_renderTargetState.targetDrawable && s_finalTarget) {
color = s_finalTarget;
}
if (color) {
w = (int)color->width();
h = (int)color->height();
} else if (s_renderTargetState.depthTexture) {
w = (int)s_renderTargetState.depthTexture->width();
h = (int)s_renderTargetState.depthTexture->height();
}

if ((w <= 0 || h <= 0) && s_finalTarget) {
w = (int)s_finalTarget->width();
h = (int)s_finalTarget->height();
}
if ((w <= 0 || h <= 0) && s_depthTexture) {
w = (int)s_depthTexture->width();
h = (int)s_depthTexture->height();
}

if (width) {
*width = w;
}
if (height) {
*height = h;
}
return (w > 0 && h > 0);
}

struct MetalTextureInfo {
	MTL::Texture *texture;
	MTL::PixelFormat pixelFormat;
	MTL::StorageMode storageMode;
	int width;
	int height;
	int sampleCount;
	bool isDrawableTexture;
};

static bool GLMetal_GetTextureInfo(uint32_t texnum, MetalTextureInfo *outInfo)
{
	if (!outInfo) {
		return false;
	}
	memset(outInfo, 0, sizeof(*outInfo));
	if (texnum == 0) {
		return false;
	}
	MTL::Texture *tex = GLMetal_GetTexture(texnum);
	if (!tex) {
		return false;
	}
	outInfo->texture = tex;
	outInfo->pixelFormat = tex->pixelFormat();
	outInfo->storageMode = tex->storageMode();
	outInfo->width = (int)tex->width();
	outInfo->height = (int)tex->height();
	outInfo->sampleCount = (int)tex->sampleCount();
	outInfo->isDrawableTexture = (s_finalTarget && tex == s_finalTarget);
	return true;
}

static void GLMetal_ApplyViewport(void)
{
if (!s_encoder) {
return;
}

int dw = 0;
int dh = 0;
if (!GLMetal_CurrentTargetSize(&dw, &dh)) {
return;
}

// Stored rect in GL (bottom-left) coords; unset (W<=0) => full drawable.
int vx = s_viewportX;
int vy = s_viewportY;
int vw = s_viewportW;
int vh = s_viewportH;
if (vw <= 0 || vh <= 0) {
vx = 0; vy = 0; vw = dw; vh = dh;
}

// Metal viewport: top-left origin, Y flipped.
MTL::Viewport vp;
vp.originX = (double)vx;
vp.originY = (double)(dh - (vy + vh));
vp.width   = (double)vw;
vp.height  = (double)vh;
vp.znear   = (double)s_depthRangeNear;
vp.zfar    = (double)s_depthRangeFar;
// Scissor: same rect, Y-flipped, intersected with [0,dw]x[0,dh].
int sx0 = vx;
int sx1 = vx + vw;
int sy0 = dh - (vy + vh);   // top
int sy1 = dh - vy;          // bottom
if (sx0 < 0)  sx0 = 0;
if (sy0 < 0)  sy0 = 0;
if (sx1 > dw) sx1 = dw;
if (sy1 > dh) sy1 = dh;

MTL::ScissorRect sr;
if (sx1 <= sx0 || sy1 <= sy0) {
// Degenerate/offscreen: collapse to empty 1x1 at origin to keep
// a valid (in-bounds) scissor; nothing should draw through it.
sr.x = 0; sr.y = 0; sr.width = 0; sr.height = 0;
} else {
sr.x      = (NS::UInteger)sx0;
sr.y      = (NS::UInteger)sy0;
sr.width  = (NS::UInteger)(sx1 - sx0);
sr.height = (NS::UInteger)(sy1 - sy0);
}

if (s_encState.viewportValid &&
    memcmp(&s_encState.viewport, &vp, sizeof(vp)) == 0 &&
    memcmp(&s_encState.scissor, &sr, sizeof(sr)) == 0) {
return;
}
s_encoder->setViewport(vp);
s_encoder->setScissorRect(sr);
s_encState.viewport = vp;
s_encState.scissor = sr;
s_encState.viewportValid = true;
}

static MTL::CullMode GLMetal_ToMetalCullMode(int cullMode)
{
if (cullMode == 1) {
return MTL::CullModeFront;
}
if (cullMode == 2) {
return MTL::CullModeBack;
}
return MTL::CullModeNone;
}

static void GLMetal_ApplyCullMode(void)
{
if (!s_encoder) {
return;
}

// Front-face winding for GL2 face-culling parity. GL's default front face is
// CCW and Q3 never changes it; with our projection/viewport setup the same
// CounterClockwise convention reproduces GL2 culling for BOTH the BSP world
// (CT_FRONT_SIDED surfaces) and the sky cloud dome. Verified empirically:
// Clockwise culls the cloud-dome sky; CounterClockwise renders world + sky
// correctly with no see-through geometry.
// Winding is set once per encoder in GLMetal_OnEncoderOpened.
GLMetal_SetEncoderCullMode(GLMetal_ToMetalCullMode(s_cullMode));
}

static uint32_t GLMetal_StencilCompareBitsFromGL(uint32_t glCompareFunc)
{
	switch (glCompareFunc) {
	case GL_NOTEQUAL: return GLMetalStencilCompareNotEqual;
	case GL_EQUAL: return GLMetalStencilCompareEqual;
	case GL_LESS: return GLMetalStencilCompareLess;
	case GL_LEQUAL: return GLMetalStencilCompareLessEqual;
	case GL_GREATER: return GLMetalStencilCompareGreater;
	case GL_GEQUAL: return GLMetalStencilCompareGreaterEqual;
	case GL_NEVER: return GLMetalStencilCompareNever;
	case GL_ALWAYS:
	default:
		return GLMetalStencilCompareAlways;
	}
}

static uint32_t GLMetal_StencilOpBitsFromGL(uint32_t glStencilOp)
{
	switch (glStencilOp) {
	case GL_ZERO: return GLMetalStencilOpZero;
	case GL_REPLACE: return GLMetalStencilOpReplace;
	case GL_INCR: return GLMetalStencilOpIncrementClamp;
	case GL_DECR: return GLMetalStencilOpDecrementClamp;
	case GL_INVERT: return GLMetalStencilOpInvert;
	case GL_INCR_WRAP: return GLMetalStencilOpIncrementWrap;
	case GL_DECR_WRAP: return GLMetalStencilOpDecrementWrap;
	case GL_KEEP:
	default:
		return GLMetalStencilOpKeep;
	}
}

static MTL::CompareFunction GLMetal_StencilCompareFunctionFromBits(uint32_t stencilStateBits)
{
	const uint32_t compareBits = (stencilStateBits & STENCIL_COMPARE_MASK) >> STENCIL_COMPARE_SHIFT;
	switch (compareBits) {
	case GLMetalStencilCompareNotEqual: return MTL::CompareFunctionNotEqual;
	case GLMetalStencilCompareEqual: return MTL::CompareFunctionEqual;
	case GLMetalStencilCompareLess: return MTL::CompareFunctionLess;
	case GLMetalStencilCompareLessEqual: return MTL::CompareFunctionLessEqual;
	case GLMetalStencilCompareGreater: return MTL::CompareFunctionGreater;
	case GLMetalStencilCompareGreaterEqual: return MTL::CompareFunctionGreaterEqual;
	case GLMetalStencilCompareNever: return MTL::CompareFunctionNever;
	case GLMetalStencilCompareAlways:
	default:
		return MTL::CompareFunctionAlways;
	}
}

static MTL::StencilOperation GLMetal_StencilOperationFromBits(uint32_t stencilStateBits, uint32_t opMask, uint32_t opShift)
{
	const uint32_t opBits = (stencilStateBits & opMask) >> opShift;
	switch (opBits) {
	case GLMetalStencilOpZero: return MTL::StencilOperationZero;
	case GLMetalStencilOpReplace: return MTL::StencilOperationReplace;
	case GLMetalStencilOpIncrementClamp: return MTL::StencilOperationIncrementClamp;
	case GLMetalStencilOpDecrementClamp: return MTL::StencilOperationDecrementClamp;
	case GLMetalStencilOpInvert: return MTL::StencilOperationInvert;
	case GLMetalStencilOpIncrementWrap: return MTL::StencilOperationIncrementWrap;
	case GLMetalStencilOpDecrementWrap: return MTL::StencilOperationDecrementWrap;
	case GLMetalStencilOpKeep:
	default:
		return MTL::StencilOperationKeep;
	}
}

static uint32_t GLMetal_StencilReferenceFromBits(uint32_t stencilStateBits)
{
	return (stencilStateBits & STENCIL_REFERENCE_MASK) >> STENCIL_REFERENCE_SHIFT;
}

static uint64_t GLMetal_ComposeDepthStencilCacheKey(uint32_t depthKey)
{
	if ((s_stencilStateBits & STENCIL_TEST_ENABLE_BIT) == 0) {
		// Critical invariant for E5 Phase 2: with stencil disabled, cache key value is
		// byte-identical to legacy depthKey and descriptor leaves stencil at Metal defaults.
		return (uint64_t)depthKey;
	}
	return (uint64_t)depthKey | ((uint64_t)s_stencilStateBits << 32);
}

static MTL::DepthStencilState *GLMetal_GetOrCreateDepthStencilState(uint64_t depthStencilKey)
{
if (!s_device) {
return nullptr;
}
if (depthStencilKey == s_lastDepthStencilKey && s_lastDepthStencilState) {
return s_lastDepthStencilState;
}

auto it = s_depthStencilCache.find(depthStencilKey);
MTL::DepthStencilState *state = (it != s_depthStencilCache.end()) ? it->second : nullptr;
if (state) {
s_lastDepthStencilKey = depthStencilKey;
s_lastDepthStencilState = state;
return state;
}

const uint32_t depthKey = (uint32_t)(depthStencilKey & 0xFFFFFFFFu);
const uint32_t stencilStateBits = (uint32_t)(depthStencilKey >> 32);

MTL::DepthStencilDescriptor *desc = MTL::DepthStencilDescriptor::alloc()->init();
if (!desc) {
return nullptr;
}

desc->setDepthWriteEnabled((depthKey & GLS_DEPTHMASK_TRUE) != 0);
if (depthKey & GLS_DEPTHTEST_DISABLE) {
desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
} else if (depthKey & GLS_DEPTHFUNC_EQUAL) {
desc->setDepthCompareFunction(MTL::CompareFunctionEqual);
} else if (depthKey & GLS_DEPTHFUNC_GREATER) {
desc->setDepthCompareFunction(MTL::CompareFunctionGreater);
} else {
desc->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
}

if (stencilStateBits & STENCIL_TEST_ENABLE_BIT) {
	MTL::StencilDescriptor *stencilDesc = MTL::StencilDescriptor::alloc()->init();
	if (!stencilDesc) {
		desc->release();
		return nullptr;
	}
	// PARITY-SEAM(Metal-device): GL2 qglStencilFunc/qglStencilOp configure front+back
	// identically in stencil shadow passes; Metal requires explicit stencil descriptors.
	// GL2 ref: tr_shadows.c:222-231,261-262
	stencilDesc->setStencilCompareFunction(GLMetal_StencilCompareFunctionFromBits(stencilStateBits));
	stencilDesc->setStencilFailureOperation(GLMetal_StencilOperationFromBits(stencilStateBits, STENCIL_FAIL_MASK, STENCIL_FAIL_SHIFT));
	stencilDesc->setDepthFailureOperation(GLMetal_StencilOperationFromBits(stencilStateBits, STENCIL_DEPTH_FAIL_MASK, STENCIL_DEPTH_FAIL_SHIFT));
	stencilDesc->setDepthStencilPassOperation(GLMetal_StencilOperationFromBits(stencilStateBits, STENCIL_PASS_MASK, STENCIL_PASS_SHIFT));
	stencilDesc->setReadMask(0xFFu);
	stencilDesc->setWriteMask(0xFFu);
	desc->setFrontFaceStencil(stencilDesc);
	desc->setBackFaceStencil(stencilDesc);
	stencilDesc->release();
}

state = s_device->newDepthStencilState(desc);
desc->release();
if (!state) {
ri.Printf(PRINT_WARNING, "[Metal] failed to create depth-stencil state (0x%016llx)\n", (unsigned long long)depthStencilKey);
return nullptr;
}
s_depthStencilCache.emplace(depthStencilKey, state);
s_lastDepthStencilKey = depthStencilKey;
s_lastDepthStencilState = state;
return state;
}

static MTL::DepthStencilState *GLMetal_GetAlwaysPassDepthStencilState(void)
{
// PARITY-STUB (Metal-only): explicit always-pass/no-write depth-stencil bind.
// Metal API validation rejects nil depthStencilState on render encoders; GL2 has no equivalent state object.
// GL2 ref: tr_backend.c:168-170 (2D/final compositing paths disable depth test/depth writes)
return GLMetal_GetOrCreateDepthStencilState((uint64_t)GLS_DEPTHTEST_DISABLE);
}

static void GLMetal_ApplyDepthState(void)
{
if (!s_encoder) {
return;
}
// A render pass without a depth attachment (depthless FBOs, e.g. post-process targets) cannot
// take a depth-testing state: Metal validation aborts the draw. GL treats the depth test as
// always passing when the framebuffer has no depth buffer, so bind the always-pass state.
if (!s_depthStateValid || (s_openRenderTargetValid && !s_openRenderTargetState.depthTexture)) {
MTL::DepthStencilState *alwaysPass = GLMetal_GetAlwaysPassDepthStencilState();
if (alwaysPass) {
	GLMetal_SetEncoderDepthStencilState(alwaysPass);
}
return;
}

const uint32_t depthKey = s_depthStateBits & GLS_DEPTH_BITS;
const uint64_t depthStencilKey = GLMetal_ComposeDepthStencilCacheKey(depthKey);
MTL::DepthStencilState *state = GLMetal_GetOrCreateDepthStencilState(depthStencilKey);
if (!state) {
return;
}
GLMetal_SetEncoderDepthStencilState(state);
// PARITY-SEAM(Metal-device): GL2 qglStencilFunc reference argument is dynamic state;
// Metal sets the reference value on the encoder at bind time.
// GL2 ref: tr_shadows.c:223,262
GLMetal_SetEncoderStencilReference(GLMetal_StencilReferenceFromBits(s_stencilStateBits));
}

// GL2 ref: tr_shade.c:1698-1701 qglEnable(GL_POLYGON_OFFSET_FILL) / tr_init.c:1061 qglPolygonOffset(-1,-2)
// setDepthBias is encoder-local state and must be re-applied whenever the encoder is (re)created.
static void GLMetal_ApplyDepthBias(void)
{
    if (!s_encoder) {
        return;
    }
    if (s_encState.depthBiasValid &&
        s_encState.depthBias == s_depthBias &&
        s_encState.slopeScale == s_slopeScale &&
        s_encState.depthBiasClamp == s_depthBiasClamp) {
        return;
    }
    s_encoder->setDepthBias(s_depthBias, s_slopeScale, s_depthBiasClamp);
    s_encState.depthBiasValid = true;
    s_encState.depthBias = s_depthBias;
    s_encState.slopeScale = s_slopeScale;
    s_encState.depthBiasClamp = s_depthBiasClamp;
}

extern "C" void GLMetal_SetDepthBias(float depthBias, float slopeScale, float clamp)
{
    s_depthBias      = depthBias;
    s_slopeScale     = slopeScale;
    s_depthBiasClamp = clamp;
    GLMetal_ApplyDepthBias();
}

static bool GLMetal_EnsureEncoder(void)
{
if (s_encoder) {
return true;
}
if (!s_cmdBuffer) {
return false;
}

MTL::Texture *colorTexture = s_renderTargetState.colorTexture;
if (s_renderTargetState.targetDrawable) {
if (!s_finalTarget) {
return false;
}
colorTexture = s_finalTarget;
}
// GL2 ref: tr_fbo.c:160-163 — depth-only FBO (pshadow capture) has no color attachment.
// Allow depth-only render passes: bail only when BOTH color and depth are absent.
if (!colorTexture && !s_renderTargetState.depthTexture) {
return false;
}
MetalRenderTargetClearState &clearState = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);

MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
if (!desc) {
return false;
}
// Configure color attachment only when a color texture is present.
// Depth-only passes (e.g., pshadow capture) skip color entirely — fragment color
// output is discarded by Metal (equivalent to GL2 glColorMask(FALSE×4)).
if (colorTexture) {
desc->colorAttachments()->object(0)->setTexture(colorTexture);
desc->colorAttachments()->object(0)->setLoadAction(clearState.pendingClearColor ? MTL::LoadActionClear : MTL::LoadActionLoad);
desc->colorAttachments()->object(0)->setClearColor(clearState.clearColor);
desc->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);
}

MTL::Texture *depthTexture = s_renderTargetState.depthTexture;
if (depthTexture) {
desc->depthAttachment()->setTexture(depthTexture);
desc->depthAttachment()->setLoadAction(clearState.pendingClearDepth ? MTL::LoadActionClear : MTL::LoadActionLoad);
desc->depthAttachment()->setClearDepth(clearState.clearDepth);
desc->depthAttachment()->setStoreAction(MTL::StoreActionStore);
// PARITY-SEAM(Metal-device): stencil attachment is only valid for combined
// depth-stencil textures; depth-only pshadow targets must not bind stencil.
// Stencil follows the depth clear: it is cleared with depth and otherwise preserved
// across render-pass splits (a split between the shadow-volume passes and
// RB_ShadowFinish previously discarded the stencil counts).
if (depthTexture->pixelFormat() == MTL::PixelFormatDepth32Float_Stencil8) {
desc->stencilAttachment()->setTexture(depthTexture);
desc->stencilAttachment()->setLoadAction(clearState.pendingClearDepth ? MTL::LoadActionClear : MTL::LoadActionLoad);
desc->stencilAttachment()->setClearStencil(0);
desc->stencilAttachment()->setStoreAction(MTL::StoreActionStore);
}
}

// RISK-1: renderCommandEncoder returns autoreleased (+0); retain for cross-call lifetime.
s_encoder = s_cmdBuffer->renderCommandEncoder(desc);
if (!s_encoder) {
return false;
}
s_encoder->retain();
GLMetal_OnEncoderOpened();
if (s_renderTargetState.targetDrawable) {
	s_frameTouchedDrawable = true;
}

clearState.pendingClearColor = false;
clearState.pendingClearDepth = false;
s_openRenderTargetState = s_renderTargetState;
s_openRenderTargetValid = true;

// Reapply all encoder-local state. Depth range is part of the viewport object.
GLMetal_ApplyViewport();
GLMetal_ApplyCullMode();
GLMetal_ApplyDepthState();
GLMetal_ApplyDepthBias();   // GL2 ref: tr_shade.c:1700 qglEnable(GL_POLYGON_OFFSET_FILL) re-primed per encoder
return true;
}

static void GLMetal_EndEncoderIfNeeded(const char *reason)
{
(void)reason;
if (!s_encoder) {
return;
}
s_encoder->endEncoding();
// RISK-1: release the +1 retain acquired in GLMetal_EnsureEncoder/blit encoder creation
s_encoder->release();
s_encoder = nullptr;
s_openRenderTargetValid = false;
}

static bool GLMetal_RecordCaptureBlit(void)
{
if (!s_cmdBuffer || !s_finalTarget) {
return false;
}

MTL::Texture *drawableTexture = s_finalTarget;
const int width = (int)drawableTexture->width();
const int height = (int)drawableTexture->height();
const MTL::PixelFormat pixelFormat = drawableTexture->pixelFormat();

if (!GLMetal_EnsureCaptureTexture(width, height, pixelFormat)) {
return false;
}

MTL::BlitCommandEncoder *blit = s_cmdBuffer->blitCommandEncoder();
if (!blit) {
return false;
}
blit->retain();

blit->copyFromTexture(drawableTexture,
    0, 0, MTL::Origin::Make(0, 0, 0),
    MTL::Size::Make((NS::UInteger)width, (NS::UInteger)height, 1),
    s_captureTexture,
    0, 0, MTL::Origin::Make(0, 0, 0));

if (s_captureTexture->storageMode() == MTL::StorageModeManaged) {
blit->synchronizeResource(s_captureTexture);
}

blit->endEncoding();
blit->release();
s_width = width;
s_height = height;
return true;
}

// ---------------------------------------------------------------------------
// r_metalFrameStats N: every N frames print where frame time went — blocked waiting for a
// free in-flight frame slot (the GPU is behind), blocked in nextDrawable (the window
// system is pacing presentation), and everything else (engine + renderer CPU work, and
// the engine's own frame limiter: it never runs a frame in under 1 ms). "backend" is the
// renderer back end's CPU time from GLMetal_BeginFrame to the end of GLMetal_Present with
// both waits excluded; unlike fps it is not capped, so it can compare builds.
// Tells a display-paced timedemo apart from one that measures the renderer.
// ---------------------------------------------------------------------------
static struct {
	cvar_t  *cvar;
	uint64_t lastFrameStart;   // ns
	uint64_t frameNs, slotWaitNs, drawableWaitNs, backendNs;
	uint64_t waitNsAtFrameStart; // slotWaitNs + drawableWaitNs when this frame began
	int      frames;
	int      presented;        // frames that went to the display
} s_frameStats;

static uint64_t GLMetal_NowNs(void)
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Refresh rate of the main display, re-read about once a second (it changes with the
// display mode). 60 when the system does not report one.
static double GLMetal_DisplayRefreshRate(void)
{
	static double rate = 0.0;
	static uint64_t lastQueryNs = 0;
	const uint64_t now = GLMetal_NowNs();

	if (rate <= 0.0 || now - lastQueryNs > 1000000000ull) {
		lastQueryNs = now;
		rate = 0.0;
		CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID());
		if (mode) {
			rate = CGDisplayModeGetRefreshRate(mode);
			CGDisplayModeRelease(mode);
		}
		if (rate < 1.0) {
			rate = 60.0;
		}
	}
	return rate;
}

static bool GLMetal_FrameStatsEnabled(void)
{
	if (!s_frameStats.cvar) {
		s_frameStats.cvar = ri.Cvar_Get("r_metalFrameStats", "0", 0);
	}
	return s_frameStats.cvar->integer > 0;
}

static void GLMetal_FrameStatsBeginFrame(void)
{
	if (!GLMetal_FrameStatsEnabled()) {
		s_frameStats.lastFrameStart = 0;
		s_frameStats.frames = 0;
		s_frameStats.frameNs = s_frameStats.slotWaitNs = s_frameStats.drawableWaitNs = s_frameStats.backendNs = 0;
		return;
	}
	const uint64_t now = GLMetal_NowNs();
	if (s_frameStats.lastFrameStart) {
		s_frameStats.frameNs += now - s_frameStats.lastFrameStart;
		s_frameStats.frames++;
	}
	s_frameStats.lastFrameStart = now;
	if (s_frameStats.frames >= s_frameStats.cvar->integer) {
		const double n = (double)s_frameStats.frames * 1e6; // ns -> ms per frame
		const double frameMs = s_frameStats.frameNs / n;
		const double slotMs = s_frameStats.slotWaitNs / n;
		const double drawableMs = s_frameStats.drawableWaitNs / n;
		ri.Printf(PRINT_ALL, "[Metal] frame stats (%d frames, %d presented): %.3f ms/frame = %.3f waiting for GPU slot + %.3f waiting for drawable + %.3f other; backend %.3f ms\n",
		          s_frameStats.frames, s_frameStats.presented, frameMs, slotMs, drawableMs, frameMs - slotMs - drawableMs,
		          s_frameStats.backendNs / n);
		s_frameStats.frames = 0;
		s_frameStats.presented = 0;
		s_frameStats.frameNs = s_frameStats.slotWaitNs = s_frameStats.drawableWaitNs = s_frameStats.backendNs = 0;
	}
	s_frameStats.waitNsAtFrameStart = s_frameStats.slotWaitNs + s_frameStats.drawableWaitNs;
}

static void GLMetal_AcquireInFlightFrameSlot(void)
{
if (!s_inFlightSemaphore || s_inFlightFramePendingSignal) {
	return;
}
const bool stats = GLMetal_FrameStatsEnabled();
const uint64_t waitStart = stats ? GLMetal_NowNs() : 0;
dispatch_semaphore_wait(s_inFlightSemaphore, DISPATCH_TIME_FOREVER);
if (stats) {
	s_frameStats.slotWaitNs += GLMetal_NowNs() - waitStart;
}
s_inFlightFramePendingSignal = true;
}

// GL2 ref: code/sdl/sdl_glimp.c:801-804 (SDL_GL_SetSwapInterval during context init).
static void GLMetal_UpdateDisplaySyncSetting(qboolean force)
{
	if (!s_layer || !r_swapInterval) {
		return;
	}

	const int currentModificationCount = r_swapInterval->modificationCount;
	if (!force &&
	    !r_swapInterval->modified &&
	    s_seenSwapIntervalModificationCount == currentModificationCount &&
	    s_appliedSwapInterval == r_swapInterval->integer) {
		return;
	}

	const int swapInterval = r_swapInterval->integer;
	const bool displaySyncEnabled = (swapInterval != 0);

#if TARGET_OS_OSX
	// CA::MetalLayer::displaySyncEnabled is not in the standard metal-cpp headers;
	// call via objc_msgSend with dynamically registered selectors (pure C runtime — no ObjC).
	{
		static SEL getDisplaySync = sel_registerName("displaySyncEnabled");
		static SEL setDisplaySync = sel_registerName("setDisplaySyncEnabled:");
		typedef bool (*BoolGetter)(id, SEL);
		typedef void (*BoolSetter)(id, SEL, bool);
		const bool cur = ((BoolGetter)objc_msgSend)((id)s_layer, getDisplaySync);
		if (cur != displaySyncEnabled) {
			((BoolSetter)objc_msgSend)((id)s_layer, setDisplaySync, displaySyncEnabled);
		}
	}
#endif
	if (force || s_appliedSwapInterval != swapInterval) {
		ri.Printf(PRINT_ALL, "[Metal] r_swapInterval %d -> displaySyncEnabled %s\n",
		          swapInterval, displaySyncEnabled ? "ON" : "OFF");
	}

	s_appliedSwapInterval = swapInterval;
	s_seenSwapIntervalModificationCount = currentModificationCount;
}

static void GLMetal_ReleaseInFlightFrameSlot(void)
{
if (!s_inFlightSemaphore || !s_inFlightFramePendingSignal) {
	return;
}
dispatch_semaphore_signal(s_inFlightSemaphore);
s_inFlightFramePendingSignal = false;
}

// GL2 ref: tr_backend.c:1402-1438 (RB_PresentToScreen final system-framebuffer stage).
static bool GLMetal_AcquireDrawableForPresent(void)
{
	if (s_finalTarget) {
		return true;
	}
	if (!s_layer) {
		return false;
	}

	// A frame is either presented (its final pass renders into a drawable of the layer) or
	// rendered into an off-screen texture of the same size and format and not shown.
	//
	// nextDrawable blocks until the window system hands a drawable back, which it does at
	// about the display's refresh rate: presenting every frame caps the frame rate there
	// and spends most of each frame waiting, however fast the renderer is. A display cannot
	// show more frames than its refresh rate anyway, so with vsync off
	// (r_swapInterval 0) only one frame per refresh interval is presented and the frames in
	// between are rendered off-screen without waiting.
	//   r_metalPresentHz -1: presents per second = the display's refresh rate (default)
	//   r_metalPresentHz  0: present every frame (the old behaviour)
	//   r_metalPresentHz  N: at most N presents per second
	// r_metalNoPresent 1 is the benchmark mode: never present. The window shows nothing;
	// screenshots and timedemo still work.
	if (!s_noPresentCvar) {
		s_noPresentCvar = ri.Cvar_Get("r_metalNoPresent", "0", 0);
		s_presentHzCvar = ri.Cvar_Get("r_metalPresentHz", "-1", CVAR_ARCHIVE);
	}
	bool present = !s_noPresentCvar->integer;
	if (present && r_swapInterval && r_swapInterval->integer == 0 && s_presentHzCvar->integer != 0) {
		double hz = (double)s_presentHzCvar->integer;
		if (hz < 0.0) {
			hz = GLMetal_DisplayRefreshRate();
		}
		const uint64_t now = GLMetal_NowNs();
		// a little under one refresh interval, so that no refresh goes without a new frame
		const uint64_t interval = (uint64_t)(0.9 * 1e9 / hz);
		if (s_lastPresentNs && now - s_lastPresentNs < interval) {
			present = false;
		} else {
			s_lastPresentNs = now;
		}
	}
	if (!present) {
		const CGSize size = s_layer->drawableSize();
		const NS::UInteger w = (NS::UInteger)size.width, h = (NS::UInteger)size.height;
		const MTL::PixelFormat format = s_layer->pixelFormat();
		if (s_offscreenTarget && (s_offscreenTarget->width() != w || s_offscreenTarget->height() != h ||
		                          s_offscreenTarget->pixelFormat() != format)) {
			s_offscreenTarget->release();
			s_offscreenTarget = nullptr;
		}
		if (!s_offscreenTarget && w > 0 && h > 0) {
			MTL::TextureDescriptor *desc = MTL::TextureDescriptor::texture2DDescriptor(format, w, h, false);
			desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
			desc->setStorageMode(MTL::StorageModePrivate);
			s_offscreenTarget = MetalDevice()->newTexture(desc);
		}
		if (!s_offscreenTarget) {
			return false;
		}
		s_finalTarget = s_offscreenTarget;
	} else {
		// RISK-1: nextDrawable returns autoreleased (+0); retain for cross-call lifetime.
		const bool stats = GLMetal_FrameStatsEnabled();
		const uint64_t waitStart = stats ? GLMetal_NowNs() : 0;
		s_drawable = s_layer->nextDrawable();
		if (stats) {
			s_frameStats.drawableWaitNs += GLMetal_NowNs() - waitStart;
		}
		if (!s_drawable || !s_drawable->texture()) {
			ri.Printf(PRINT_WARNING, "[Metal] nextDrawable() returned null during present stage\n");
			s_drawable = nullptr;
			return false;
		}
		s_drawable->retain();
		s_finalTarget = s_drawable->texture();
		s_frameStats.presented++;
	}

	const int drawableWidth = (int)s_finalTarget->width();
	const int drawableHeight = (int)s_finalTarget->height();
	if (drawableWidth > 0 && drawableHeight > 0) {
		(void)GLMetal_EnsureDepthTexture(drawableWidth, drawableHeight);
		s_width = drawableWidth;
		s_height = drawableHeight;
		if (s_renderTargetState.targetDrawable) {
			s_renderTargetState.depthTexture = s_depthTexture;
		}
		if (s_viewportW <= 0 || s_viewportH <= 0) {
			s_viewportX = 0;
			s_viewportY = 0;
			s_viewportW = drawableWidth;
			s_viewportH = drawableHeight;
		}
	}

	return true;
}

// GL2 ref: tr_fbo.c:620 (FBO_FastBlit) — Metal device blit primitive.
extern "C" qboolean GLMetal_FastCopyTexture(uint32_t srcTexnum, int srcX, int srcY, int srcW, int srcH,
                                            uint32_t dstTexnum, int dstX, int dstY, int copyW, int copyH)
{
	GLMetal_EndEncoderIfNeeded("fbo-fastcopy");
	if (!s_cmdBuffer) {
		return qfalse;
	}

	MetalTextureInfo srcInfo;
	MetalTextureInfo dstInfo;
	if (!GLMetal_GetTextureInfo(srcTexnum, &srcInfo) || !GLMetal_GetTextureInfo(dstTexnum, &dstInfo)) {
		return qfalse;
	}
	if (srcInfo.isDrawableTexture || dstInfo.isDrawableTexture) {
		return qfalse;
	}
	if (srcInfo.pixelFormat != dstInfo.pixelFormat) {
		return qfalse;
	}
	if (srcInfo.sampleCount != 1 || dstInfo.sampleCount != 1) {
		return qfalse;
	}
	if (srcW <= 0 || srcH <= 0 || copyW <= 0 || copyH <= 0) {
		return qfalse;
	}
	if (srcW != copyW || srcH != copyH) {
		return qfalse;
	}
	if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0) {
		return qfalse;
	}
	if ((srcX + copyW) > srcInfo.width || (srcY + copyH) > srcInfo.height ||
	    (dstX + copyW) > dstInfo.width || (dstY + copyH) > dstInfo.height) {
		return qfalse;
	}

	MTL::BlitCommandEncoder *blit = s_cmdBuffer->blitCommandEncoder();
	if (!blit) {
		return qfalse;
	}
	blit->retain();

	// src/dst boxes are already in Metal top-left texture coords from caller's GL2
	// bottom-left rect conversion; Y-flipped blits must use render-pass path.
	blit->copyFromTexture(srcInfo.texture,
	    0, 0, MTL::Origin::Make((NS::UInteger)srcX, (NS::UInteger)srcY, 0),
	    MTL::Size::Make((NS::UInteger)copyW, (NS::UInteger)copyH, 1),
	    dstInfo.texture,
	    0, 0, MTL::Origin::Make((NS::UInteger)dstX, (NS::UInteger)dstY, 0));

	if (srcInfo.storageMode == MTL::StorageModeManaged) {
		blit->synchronizeResource(srcInfo.texture);
	}
	if (dstInfo.storageMode == MTL::StorageModeManaged) {
		blit->synchronizeResource(dstInfo.texture);
	}

	blit->endEncoding();
	blit->release();
	return qtrue;
}

// GL2 ref: tr_fbo.c:495 (FBO_BlitFromTexture) — Metal device blit primitive.
static const size_t kPostprocessUniformBytes = 112; // PARITY-SEAM(Metal-device): Lambert MetalPostprocessUniforms contract is 7×16 bytes.
// PARITY-SEAM(Metal-device): GL2 multi-TMU postprocess binds (GL_BindToTMU/GL_BindMultiTexture) map to fragment texture slots 1..N.
// GL2 ref: tr_backend.c:998, tr_backend.c:1002, tr_backend.c:1047, tr_backend.c:1048, tr_postprocess.c:89
// Tonemap contract for Lambert/Vasquez wiring: fragment texture(0)=scene color, texture(1)=levels map (TB_LEVELSMAP).
// GL2 ref: tr_postprocess.c:89-93, tr_fbo.c:571, tr_fbo.c:371-382
extern "C" qboolean GLMetal_RenderBlitTextureMulti(uint32_t srcTexnum, const float srcUV[8], uint32_t dstColorTexnum,
                                                   int dstX, int dstY, int dstW, int dstH, int dstFullW, int dstFullH,
                                                   int filter, int stateBits, const float color[4], const char *programName,
                                                   const void *postprocessUniforms, const uint32_t *extraTexnums, int extraTexCount,
                                                   bool fullCoverage)
{
	GLMetal_EndEncoderIfNeeded("fbo-blit");
	if (!s_cmdBuffer || !srcUV || !color || dstW <= 0 || dstH <= 0 || dstFullW <= 0 || dstFullH <= 0) {
		return qfalse;
	}

	MetalTextureInfo srcInfo;
	MetalTextureInfo dstInfo;
	if (!GLMetal_GetTextureInfo(srcTexnum, &srcInfo) || !GLMetal_GetTextureInfo(dstColorTexnum, &dstInfo)) {
		return qfalse;
	}
	if (srcInfo.isDrawableTexture || dstInfo.isDrawableTexture) {
		return qfalse;
	}
	if (srcInfo.texture == dstInfo.texture) {
		if (!s_blitAliasWarned) {
			ri.Printf(PRINT_WARNING, "[Metal] GLMetal_RenderBlitTexture: source/destination texture aliasing unsupported\n");
			s_blitAliasWarned = true;
		}
		return qfalse;
	}
	if (srcInfo.sampleCount != 1 || dstInfo.sampleCount != 1) {
		if (!s_blitMsaaWarned) {
			ri.Printf(PRINT_WARNING, "[Metal] GLMetal_RenderBlitTexture: MSAA blit path unsupported (requires sampleCount == 1)\n");
			s_blitMsaaWarned = true;
		}
		return qfalse;
	}
	if (dstX < 0 || dstY < 0 || (dstX + dstW) > dstFullW || (dstY + dstH) > dstFullH ||
	    dstFullW > dstInfo.width || dstFullH > dstInfo.height) {
		return qfalse;
	}

	GLMetal_SetRenderTarget(dstColorTexnum, 0, false, false);
	if (s_renderTargetState.targetDrawable || !s_renderTargetState.colorTexture) {
		return qfalse;
	}

	// FIX-2 (RISK-3): NS::AutoreleasePool drains per-blit transient objects (RenderPassDescriptor,
	// initial encoder autorelease ref) immediately each call. s_cmdBuffer and the semaphore are NOT
	// inside this scope. s_encoder is retained (+1) after creation and released in
	// GLMetal_EndEncoderIfNeeded, so pool drain does not over-release it.
	NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();

	MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
	if (!desc) {
		pool->drain();
		return qfalse;
	}
	desc->colorAttachments()->object(0)->setTexture(s_renderTargetState.colorTexture);
	// FIX-1: use DontCare when caller guarantees full-coverage overwrite (e.g. ping-pong
	// downsample chain, tonemap final). Avoids TBDR tile-memory load of existing contents.
	desc->colorAttachments()->object(0)->setLoadAction(fullCoverage ? MTL::LoadActionDontCare : MTL::LoadActionLoad);
	desc->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);

	// RISK-1: renderCommandEncoder returns autoreleased (+0); retain for cross-call lifetime.
	s_encoder = s_cmdBuffer->renderCommandEncoder(desc);
	if (!s_encoder) {
		pool->drain();
		return qfalse;
	}
	s_encoder->retain();
	GLMetal_OnEncoderOpened();

	MTL::Viewport vp;
	vp.originX = (double)dstX;
	vp.originY = (double)(dstFullH - (dstY + dstH));
	vp.width = (double)dstW;
	vp.height = (double)dstH;
	vp.znear = 0.0;
	vp.zfar = 1.0;
	s_encoder->setViewport(vp);

	int sx0 = dstX;
	int sx1 = dstX + dstW;
	int sy0 = dstFullH - (dstY + dstH);
	int sy1 = dstFullH - dstY;
	if (sx0 < 0) sx0 = 0;
	if (sy0 < 0) sy0 = 0;
	if (sx1 > dstFullW) sx1 = dstFullW;
	if (sy1 > dstFullH) sy1 = dstFullH;

	MTL::ScissorRect sr;
	if (sx1 <= sx0 || sy1 <= sy0) {
		sr.x = 0;
		sr.y = 0;
		sr.width = 0;
		sr.height = 0;
	} else {
		sr.x = (NS::UInteger)sx0;
		sr.y = (NS::UInteger)sy0;
		sr.width = (NS::UInteger)(sx1 - sx0);
		sr.height = (NS::UInteger)(sy1 - sy0);
	}
	s_encoder->setScissorRect(sr);
	s_encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
	s_encoder->setCullMode(MTL::CullModeNone);
	MTL::DepthStencilState *alwaysPass = GLMetal_GetAlwaysPassDepthStencilState();
	if (alwaysPass) {
		s_encoder->setDepthStencilState(alwaysPass);
	}

	// Pool can be drained here — desc and the initial autoreleased encoder ref are no longer
	// needed. s_encoder's +1 retained ref survives the drain.
	pool->drain();
	pool = nullptr;

	const char *resolvedProgramName = (programName && programName[0]) ? programName : "texturecolor";
	const uint64_t vtxHash = GLMetal_VertexDescHashForAttribs(ATTR_POSITION | ATTR_TEXCOORD);
	MTL::RenderPipelineState *pso = GLMetal_GetPipelineForState(resolvedProgramName, vtxHash, (uint32_t)stateBits);
	MTL::SamplerState *sampler = GLMetal_GetOrCreateSampler(GL_CLAMP_TO_EDGE, (filter == GL_LINEAR) ? qtrue : qfalse, qfalse, qfalse);
	if (!pso || !sampler) {
		GLMetal_EndEncoderIfNeeded("fbo-blit-fail");
		return qfalse;
	}

	const float positions[6][4] = {
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f,  1.0f, 0.0f, 1.0f },
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f,  1.0f, 0.0f, 1.0f },
		{ -1.0f,  1.0f, 0.0f, 1.0f }
	};
	const float texcoords[6][2] = {
		{ srcUV[0], srcUV[1] },
		{ srcUV[2], srcUV[3] },
		{ srcUV[4], srcUV[5] },
		{ srcUV[0], srcUV[1] },
		{ srcUV[4], srcUV[5] },
		{ srcUV[6], srcUV[7] }
	};
	struct {
		float mvp[16];
	} vertU = {
		{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		}
	};
	struct {
		float rgba[4];
	} fragU = { { color[0], color[1], color[2], color[3] } };

	MTL::RenderCommandEncoder *enc = s_encoder;
	enc->setRenderPipelineState(pso);
	enc->setVertexBytes(positions, sizeof(positions), 2);
	enc->setVertexBytes(texcoords, sizeof(texcoords), 3);
	if (postprocessUniforms) {
		// PARITY-SEAM(Metal-device): postprocess fullscreen blits consume Lambert's opaque 112-byte uniform block in both stages.
		enc->setVertexBytes(postprocessUniforms, kPostprocessUniformBytes, 1);
		enc->setFragmentBytes(postprocessUniforms, kPostprocessUniformBytes, 0);
	} else {
		enc->setVertexBytes(&vertU, sizeof(vertU), 1);
		enc->setFragmentBytes(&fragU, sizeof(fragU), 0);
	}
	enc->setFragmentTexture(srcInfo.texture, 0);
	enc->setFragmentSamplerState(sampler, 0);
	if (extraTexCount < 0) {
		ri.Printf(PRINT_DEVELOPER, "[Metal] GLMetal_RenderBlitTextureMulti: clamping negative extraTexCount %d to 0\n", extraTexCount);
		extraTexCount = 0;
	}
	if (extraTexCount > 0 && !extraTexnums) {
		ri.Printf(PRINT_DEVELOPER, "[Metal] GLMetal_RenderBlitTextureMulti: extraTexCount=%d but extraTexnums is NULL; skipping extra binds\n", extraTexCount);
		extraTexCount = 0;
	}
	// PARITY-SEAM(Metal-device): mirror slot-0 sampler policy for extra fragment textures (clamp-to-edge + filter-selected sampler).
	// GL2 ref: tr_fbo.c:570, tr_postprocess.c:89-93
	for (int extraIndex = 0; extraIndex < extraTexCount; ++extraIndex) {
		const uint32_t extraTexnum = extraTexnums[extraIndex];
		const int slot = extraIndex + 1;
		if (extraTexnum == 0) {
			ri.Printf(PRINT_DEVELOPER, "[Metal] GLMetal_RenderBlitTextureMulti: skipping extra tex slot %d due to texnum 0\n", slot);
			enc->setFragmentTexture(nullptr, (NS::UInteger)slot);
			enc->setFragmentSamplerState(nullptr, (NS::UInteger)slot);
			continue;
		}
		MetalTextureInfo extraInfo;
		if (!GLMetal_GetTextureInfo(extraTexnum, &extraInfo) || extraInfo.isDrawableTexture) {
			ri.Printf(PRINT_DEVELOPER, "[Metal] GLMetal_RenderBlitTextureMulti: skipping extra tex slot %d for invalid texnum %u\n", slot, extraTexnum);
			enc->setFragmentTexture(nullptr, (NS::UInteger)slot);
			enc->setFragmentSamplerState(nullptr, (NS::UInteger)slot);
			continue;
		}
		// GL2 has no same-pass read/write hazard for levels map because tonemap samples a distinct 1x1 levels texture.
		// GL2 ref: tr_postprocess.c:89-93, tr_fbo.c:371-382
		if (extraInfo.texture == dstInfo.texture) {
			ri.Printf(PRINT_DEVELOPER, "[Metal] GLMetal_RenderBlitTextureMulti: skipping extra tex slot %d due to render-target alias texnum %u\n", slot, extraTexnum);
			enc->setFragmentTexture(nullptr, (NS::UInteger)slot);
			enc->setFragmentSamplerState(nullptr, (NS::UInteger)slot);
			continue;
		}
		if (extraInfo.sampleCount != 1) {
			ri.Printf(PRINT_DEVELOPER, "[Metal] GLMetal_RenderBlitTextureMulti: skipping extra tex slot %d due to MSAA texnum %u (sampleCount=%u)\n",
			          slot, extraTexnum, (unsigned)extraInfo.sampleCount);
			enc->setFragmentTexture(nullptr, (NS::UInteger)slot);
			enc->setFragmentSamplerState(nullptr, (NS::UInteger)slot);
			continue;
		}
		enc->setFragmentTexture(extraInfo.texture, (NS::UInteger)slot);
		enc->setFragmentSamplerState(sampler, (NS::UInteger)slot);
	}
	enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)6);

	MetalRenderTargetClearState &clearState = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);
	clearState.pendingClearColor = false;
	clearState.pendingClearDepth = false;
	s_openRenderTargetState = s_renderTargetState;
	s_openRenderTargetValid = true;

	GLMetal_EndEncoderIfNeeded("fbo-blit");
	return qtrue;
}

// GL2 ref: tr_fbo.c:495 (FBO_BlitFromTexture) — preserve existing single-source API for current callers.
extern "C" qboolean GLMetal_RenderBlitTexture(uint32_t srcTexnum, const float srcUV[8], uint32_t dstColorTexnum,
                                              int dstX, int dstY, int dstW, int dstH, int dstFullW, int dstFullH,
                                              int filter, int stateBits, const float color[4], const char *programName,
                                              const void *postprocessUniforms, bool fullCoverage)
{
	return GLMetal_RenderBlitTextureMulti(srcTexnum, srcUV, dstColorTexnum,
	                                      dstX, dstY, dstW, dstH, dstFullW, dstFullH,
	                                      filter, stateBits, color, programName,
	                                      postprocessUniforms, nullptr, 0, fullCoverage);
}

static uint32_t SamplerCacheKey(int wrapClampMode, qboolean filterLinear, qboolean mipmap, qboolean mipLinear)
{
// bit 0-15: wrapClampMode, bit 16: filterLinear, bit 17: mipmap, bit 18: mipLinear (F3)
return (uint32_t)((wrapClampMode & 0xFFFF) | ((filterLinear ? 1 : 0) << 16) | ((mipmap ? 1 : 0) << 17) | ((mipLinear ? 1 : 0) << 18));
}

// GL wrap/filter mapping for sampler cache:
// - wrapClampMode == GL_CLAMP_TO_EDGE => ClampToEdge, otherwise Repeat
// - filterLinear true => mag Linear, min Linear
// - filterLinear false => mag Nearest, min Nearest
// - mipmap true => mip filter active; mipLinear selects Linear (trilinear) vs Nearest (bilinear)
// - mipLinear true  => GL_LINEAR_MIPMAP_LINEAR  (trilinear): SamplerMipFilterLinear
// - mipLinear false => GL_LINEAR_MIPMAP_NEAREST (bilinear):  SamplerMipFilterNearest
// F3: mipLinear is a new axis; both variants cache separately via SamplerCacheKey bit 18.
static MTL::SamplerAddressMode GLWrapToMetalAddressMode(int wrapClampMode)
{
return (wrapClampMode == GL_CLAMP_TO_EDGE) ? MTL::SamplerAddressModeClampToEdge : MTL::SamplerAddressModeRepeat;
}

static void ReleaseTextureTable(void)
{
for (MTL::Texture *texture : s_textureTable) {
if (texture) {
texture->release();
}
}
s_textureTable.clear();
s_bindingGeneration++;
}

static void ReleaseSamplerCache(void)
{
for (auto &entry : s_samplerCache) {
if (entry.second) {
entry.second->release();
}
}
s_samplerCache.clear();
s_bindingGeneration++; // cached sampler pointers in s_textureBindings are now dangling
// PARITY-SEAM(Metal-device): s_tonemapSampler is a NON-OWNING cached pointer into
// s_samplerCache. Releasing the cache frees that object, so the reference must be
// nulled here or the next GLMetal_TonemapPass binds a dangling MTLSamplerState
// (objc_retain on freed memory -> EXC_BAD_ACCESS). GL_TextureMode flushes the cache
// during map load / cgame init, which is exactly when the crash was observed.
s_tonemapSampler = nullptr;
}

static MTL::BlendFactor GLStateBlendFactorToMetal(uint32_t blendBits)
{
switch (blendBits) {
case GLS_SRCBLEND_ZERO:
case GLS_DSTBLEND_ZERO:
return MTL::BlendFactorZero;
case GLS_SRCBLEND_ONE:
case GLS_DSTBLEND_ONE:
return MTL::BlendFactorOne;
case GLS_SRCBLEND_DST_COLOR:
return MTL::BlendFactorDestinationColor;
case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
return MTL::BlendFactorOneMinusDestinationColor;
case GLS_SRCBLEND_SRC_ALPHA:
case GLS_DSTBLEND_SRC_ALPHA:
return MTL::BlendFactorSourceAlpha;
case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
return MTL::BlendFactorOneMinusSourceAlpha;
case GLS_SRCBLEND_DST_ALPHA:
case GLS_DSTBLEND_DST_ALPHA:
return MTL::BlendFactorDestinationAlpha;
case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
return MTL::BlendFactorOneMinusDestinationAlpha;
case GLS_SRCBLEND_ALPHA_SATURATE:
return MTL::BlendFactorSourceAlphaSaturated;
case GLS_DSTBLEND_SRC_COLOR:
return MTL::BlendFactorSourceColor;
case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
return MTL::BlendFactorOneMinusSourceColor;
default:
return MTL::BlendFactorOne;
}
}

static MTL::VertexFormat GLAttribToVertexFormat(const vaoAttrib_t &attrib)
{
if (attrib.type == GL_FLOAT) {
switch (attrib.count) {
case 1: return MTL::VertexFormatFloat;
case 2: return MTL::VertexFormatFloat2;
case 3: return MTL::VertexFormatFloat3;
case 4: return MTL::VertexFormatFloat4;
default: return MTL::VertexFormatInvalid;
}
}

if (attrib.type == GL_SHORT) {
if (attrib.normalized) {
switch (attrib.count) {
case 2: return MTL::VertexFormatShort2Normalized;
case 3: return MTL::VertexFormatShort3Normalized;
case 4: return MTL::VertexFormatShort4Normalized;
default: return MTL::VertexFormatInvalid;
}
}
switch (attrib.count) {
case 2: return MTL::VertexFormatShort2;
case 3: return MTL::VertexFormatShort3;
case 4: return MTL::VertexFormatShort4;
default: return MTL::VertexFormatInvalid;
}
}

if (attrib.type == GL_UNSIGNED_SHORT) {
if (attrib.normalized) {
switch (attrib.count) {
case 2: return MTL::VertexFormatUShort2Normalized;
case 3: return MTL::VertexFormatUShort3Normalized;
case 4: return MTL::VertexFormatUShort4Normalized;
default: return MTL::VertexFormatInvalid;
}
}
switch (attrib.count) {
case 2: return MTL::VertexFormatUShort2;
case 3: return MTL::VertexFormatUShort3;
case 4: return MTL::VertexFormatUShort4;
default: return MTL::VertexFormatInvalid;
}
}

if (attrib.type == GL_UNSIGNED_BYTE) {
if (attrib.normalized) {
switch (attrib.count) {
case 2: return MTL::VertexFormatUChar2Normalized;
case 3: return MTL::VertexFormatUChar3Normalized;
case 4: return MTL::VertexFormatUChar4Normalized;
default: return MTL::VertexFormatInvalid;
}
}
switch (attrib.count) {
case 2: return MTL::VertexFormatUChar2;
case 3: return MTL::VertexFormatUChar3;
case 4: return MTL::VertexFormatUChar4;
default: return MTL::VertexFormatInvalid;
}
}

return MTL::VertexFormatInvalid;
}

extern "C" uint64_t GLMetal_VertexDescHashForAttribs(uint32_t attribs)
{
uint64_t hash = 14695981039346656037ULL;
uint64_t a = (uint64_t)attribs;
for (int b = 0; b < 8; b++) {
hash ^= (a & 0xFF);
hash *= 1099511628211ULL;
a >>= 8;
}
return hash;
}

static uint64_t HashVertexDescriptor(const MTL::VertexDescriptor *vertexDesc)
{
if (!vertexDesc) {
return 0;
}

const uint64_t fnvOffset = 1469598103934665603ull;
const uint64_t fnvPrime = 1099511628211ull;
uint64_t hash = fnvOffset;

auto mix = [&](uint64_t v) {
hash ^= v;
hash *= fnvPrime;
};

for (int i = 0; i < VAO_MAX_ATTRIBS; ++i) {
MTL::VertexAttributeDescriptor *a = vertexDesc->attributes()->object(i);
mix((uint64_t)a->format());
mix((uint64_t)a->offset());
mix((uint64_t)a->bufferIndex());
}

for (int i = 0; i < VAO_MAX_ATTRIBS; ++i) {
MTL::VertexBufferLayoutDescriptor *l = vertexDesc->layouts()->object(i);
mix((uint64_t)l->stride());
mix((uint64_t)l->stepFunction());
mix((uint64_t)l->stepRate());
}

return hash;
}

static void ConfigureBlendState(MTL::RenderPipelineColorAttachmentDescriptor *colorAttachment, uint32_t stateBits)
{
const uint32_t srcBlendBits = stateBits & GLS_SRCBLEND_BITS;
const uint32_t dstBlendBits = stateBits & GLS_DSTBLEND_BITS;
const bool enableBlend = (srcBlendBits != 0 || dstBlendBits != 0);

colorAttachment->setBlendingEnabled(enableBlend);
if (!enableBlend) {
return;
}

const MTL::BlendFactor src = GLStateBlendFactorToMetal(srcBlendBits);
const MTL::BlendFactor dst = GLStateBlendFactorToMetal(dstBlendBits);

colorAttachment->setSourceRGBBlendFactor(src);
colorAttachment->setDestinationRGBBlendFactor(dst);
colorAttachment->setRgbBlendOperation(MTL::BlendOperationAdd);

colorAttachment->setSourceAlphaBlendFactor(srcBlendBits == GLS_SRCBLEND_ALPHA_SATURATE ? MTL::BlendFactorOne : src);
colorAttachment->setDestinationAlphaBlendFactor(dst);
colorAttachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
}

static bool GLMetal_LoadProgramFunctions(const char *programName, const char *vertEntry, const char *fragEntry, MTL::Function **outVertFn, MTL::Function **outFragFn)
{
	if (!programName || !vertEntry || !fragEntry || !outVertFn || !outFragFn) {
		return false;
	}
	if (*outVertFn && *outFragFn) {
		return true;
	}

	const char *mslVert = GLMetal_GetShaderSource(programName, qfalse);
	const char *mslFrag = GLMetal_GetShaderSource(programName, qtrue);
	if (!mslVert || !mslFrag) {
		ri.Printf(PRINT_WARNING, "[Metal] missing MSL for '%s'\n", programName);
		return false;
	}

	MTL::Library *vertLib = GLMetal_LoadShaderLibraryFromSource(mslVert, programName);
	MTL::Library *fragLib = GLMetal_LoadShaderLibraryFromSource(mslFrag, programName);
	if (!vertLib || !fragLib) {
		if (vertLib) {
			vertLib->release();
		}
		if (fragLib) {
			fragLib->release();
		}
		return false;
	}

	MTL::Function *vertFn = vertLib->newFunction(NS::String::string(vertEntry, NS::UTF8StringEncoding));
	MTL::Function *fragFn = fragLib->newFunction(NS::String::string(fragEntry, NS::UTF8StringEncoding));
	vertLib->release();
	fragLib->release();
	if (!vertFn || !fragFn) {
		ri.Printf(PRINT_WARNING, "[Metal] function lookup failed for '%s'\n", programName);
		if (vertFn) {
			vertFn->release();
		}
		if (fragFn) {
			fragFn->release();
		}
		return false;
	}

	*outVertFn = vertFn;
	*outFragFn = fragFn;
	return true;
}

static void GLMetal_RunPipelineSelfTest(void)
{
if (s_pipelineSelfTestDone || !s_device) {
return;
}
s_pipelineSelfTestDone = true;

static const char *kSelfTestMSL =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VSOut { float4 position [[position]]; };\n"
"vertex VSOut metal_selftest_vs(uint vid [[vertex_id]]) {\n"
"  VSOut o;\n"
"  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
"  o.position = float4(p[vid], 0.0, 1.0);\n"
"  return o;\n"
"}\n"
"fragment float4 metal_selftest_fs() { return float4(0.0, 1.0, 0.0, 1.0); }\n";

MTL::Library *lib = GLMetal_LoadShaderLibraryFromSource(kSelfTestMSL, "metal-selftest");
if (!lib) {
ri.Printf(PRINT_WARNING, "[Metal] pipeline self-test failed (library compile)\n");
return;
}

NS::String *vsName = NS::String::string("metal_selftest_vs", NS::UTF8StringEncoding);
NS::String *fsName = NS::String::string("metal_selftest_fs", NS::UTF8StringEncoding);
MTL::Function *vs = lib->newFunction(vsName);
MTL::Function *fs = lib->newFunction(fsName);

if (!vs || !fs) {
ri.Printf(PRINT_WARNING, "[Metal] pipeline self-test failed (function lookup)\n");
if (vs) {
vs->release();
}
if (fs) {
fs->release();
}
lib->release();
return;
}

MTL::VertexDescriptor *vertexDesc = MTL::VertexDescriptor::vertexDescriptor();
vertexDesc->retain();
vertexDesc->layouts()->object(0)->setStride(0);
vertexDesc->layouts()->object(0)->setStepFunction(MTL::VertexStepFunctionPerVertex);
vertexDesc->layouts()->object(0)->setStepRate(1);

MTL::RenderPipelineState *pipeline = GLMetal_GetOrCreatePipeline("metal-selftest", vs, fs, vertexDesc, 0, 0);
if (pipeline) {
ri.Printf(PRINT_ALL, "[Metal] pipeline self-test OK\n");
} else {
ri.Printf(PRINT_WARNING, "[Metal] pipeline self-test failed (pipeline creation)\n");
}

vertexDesc->release();
vs->release();
fs->release();
lib->release();
}

MTL::Library *GLMetal_LoadShaderLibraryFromSourceWithDefines(const char *mslSource, const char *label, const char *defines)
{
MTL::Device *device = MetalDevice();
if (!device || !mslSource || !mslSource[0]) {
ri.Printf(PRINT_WARNING, "[Metal] GLMetal_LoadShaderLibraryFromSource: invalid input or device not ready\n");
return nullptr;
}

	std::string sourceText;
	const char *compileSource = mslSource;
	if (defines && defines[0]) {
		sourceText.reserve(strlen(defines) + strlen(mslSource) + 1);
		sourceText.append(defines);
		sourceText.append(mslSource);
		compileSource = sourceText.c_str();
	}

NS::String *source = NS::String::string(compileSource, NS::UTF8StringEncoding);
if (!source) {
ri.Printf(PRINT_WARNING, "[Metal] GLMetal_LoadShaderLibraryFromSource: failed to create source string\n");
return nullptr;
}

MTL::CompileOptions *options = MTL::CompileOptions::alloc()->init();
// Honour [[invariant]] on vertex position outputs, so every pass over a surface produces
// bit-identical depth (the sun depth pre-pass is reused by the main pass with LEQUAL).
options->setPreserveInvariance(true);

NS::Error *error = nullptr;
MTL::Library *library = device->newLibrary(source, options, &error);
options->release();

if (!library) {
ri.Printf(PRINT_WARNING, "[Metal] MSL compile failed (%s): %s\n", (label && label[0]) ? label : "unnamed", MetalErrorString(error));
return nullptr;
}

return library;
}

bool GLMetal_LoadShaderLibraryPair(const char *mslSourceA, const char *mslSourceB, const char *label,
                                   MTL::Library **outA, MTL::Library **outB)
{
	if (!outA || !outB) {
		return false;
	}
	*outA = nullptr;
	*outB = nullptr;
	MTL::Device *device = MetalDevice();
	if (!device || !mslSourceA || !mslSourceA[0] || !mslSourceB || !mslSourceB[0]) {
		ri.Printf(PRINT_WARNING, "[Metal] GLMetal_LoadShaderLibraryPair: invalid input or device not ready\n");
		return false;
	}

	// Kick off both compiles asynchronously so the vertex and fragment libraries build in
	// parallel on Metal's compiler threads, then wait for both.
	struct PairResult {
		MTL::Library *library;
		std::string error;
	};
	PairResult results[2] = { { nullptr, std::string() }, { nullptr, std::string() } };
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	MTL::CompileOptions *options = MTL::CompileOptions::alloc()->init();
// Honour [[invariant]] on vertex position outputs, so every pass over a surface produces
// bit-identical depth (the sun depth pre-pass is reused by the main pass with LEQUAL).
options->setPreserveInvariance(true);
	const char *sources[2] = { mslSourceA, mslSourceB };
	for (int i = 0; i < 2; ++i) {
		NS::String *source = NS::String::string(sources[i], NS::UTF8StringEncoding);
		PairResult *result = &results[i];
		device->newLibrary(source, options, [result, done](MTL::Library *library, NS::Error *error) {
			if (library) {
				library->retain();
				result->library = library;
			} else {
				result->error = MetalErrorString(error);
			}
			dispatch_semaphore_signal(done);
		});
	}
	dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	dispatch_release(done);
	options->release();

	if (!results[0].library || !results[1].library) {
		for (int i = 0; i < 2; ++i) {
			if (results[i].library) {
				results[i].library->release();
			} else {
				ri.Printf(PRINT_WARNING, "[Metal] MSL compile failed (%s, %s): %s\n",
				          (label && label[0]) ? label : "unnamed", i == 0 ? "vertex" : "fragment", results[i].error.c_str());
			}
		}
		return false;
	}
	*outA = results[0].library;
	*outB = results[1].library;
	return true;
}

extern "C" void *GLMetal_CreateStaticBuffer(const void *data, size_t size)
{
	MTL::Device *device = MetalDevice();
	if (!device || !data || !size) {
		return nullptr;
	}
	// newBuffer returns +1; the caller owns it until GLMetal_DestroyStaticBuffer.
	return device->newBuffer(data, size, MTL::ResourceStorageModeShared);
}

extern "C" void GLMetal_DestroyStaticBuffer(void *buffer)
{
	if (buffer) {
		// Command buffers retain the buffers bound to them, so in-flight frames stay valid.
		static_cast<MTL::Buffer *>(buffer)->release();
	}
}

struct MetalShaderCompileJob {
	int count;
	MTL::Library *library[2];
	std::string error[2];
	dispatch_semaphore_t done;
};

MetalShaderCompileJob *GLMetal_BeginShaderLibraryCompile(const char *mslVert, const char *mslFrag)
{
	MTL::Device *device = MetalDevice();
	if (!device || !mslVert || !mslVert[0]) {
		return nullptr;
	}

	MetalShaderCompileJob *job = new MetalShaderCompileJob();
	job->count = (mslFrag && mslFrag[0]) ? 2 : 1;
	job->library[0] = job->library[1] = nullptr;
	job->done = dispatch_semaphore_create(0);

	// The pool drains the source strings and options once Metal has taken its own references.
	NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
	MTL::CompileOptions *options = MTL::CompileOptions::alloc()->init();
// Honour [[invariant]] on vertex position outputs, so every pass over a surface produces
// bit-identical depth (the sun depth pre-pass is reused by the main pass with LEQUAL).
options->setPreserveInvariance(true);
	const char *sources[2] = { mslVert, mslFrag };
	for (int i = 0; i < job->count; ++i) {
		NS::String *source = NS::String::string(sources[i], NS::UTF8StringEncoding);
		// The handler runs on a Metal compiler thread: it only touches the job.
		device->newLibrary(source, options, [job, i](MTL::Library *library, NS::Error *error) {
			if (library) {
				library->retain();
				job->library[i] = library;
			} else {
				job->error[i] = MetalErrorString(error);
			}
			dispatch_semaphore_signal(job->done);
		});
	}
	options->release();
	pool->drain();
	return job;
}

bool GLMetal_FinishShaderLibraryCompile(MetalShaderCompileJob *job, const char *label,
                                        MTL::Library **outVert, MTL::Library **outFrag)
{
	if (outVert) *outVert = nullptr;
	if (outFrag) *outFrag = nullptr;
	if (!job) {
		return false;
	}
	for (int i = 0; i < job->count; ++i) {
		dispatch_semaphore_wait(job->done, DISPATCH_TIME_FOREVER);
	}
	dispatch_release(job->done);

	bool ok = outVert && (job->count == 1 || outFrag);
	for (int i = 0; i < job->count; ++i) {
		if (!job->library[i]) {
			ri.Printf(PRINT_WARNING, "[Metal] MSL compile failed (%s, %s): %s\n",
			          (label && label[0]) ? label : "unnamed", i == 0 ? "vertex" : "fragment", job->error[i].c_str());
			ok = false;
		}
	}
	for (int i = 0; i < job->count; ++i) {
		if (!job->library[i]) {
			continue;
		}
		if (ok) {
			*(i == 0 ? outVert : outFrag) = job->library[i];
		} else {
			job->library[i]->release();
		}
	}
	delete job;
	return ok;
}

MTL::Library *GLMetal_LoadShaderLibraryFromSource(const char *mslSource, const char *label)
{
	return GLMetal_LoadShaderLibraryFromSourceWithDefines(mslSource, label, nullptr);
}

MTL::VertexDescriptor *GLMetal_BuildVertexDescriptorFromVao(const struct vao_s *vao, uint64_t *outHash)
{
if (!vao) {
return nullptr;
}

MTL::VertexDescriptor *vertexDesc = MTL::VertexDescriptor::vertexDescriptor();
vertexDesc->retain();

const int trackedAttribs[4] = {
ATTR_INDEX_POSITION,
ATTR_INDEX_TEXCOORD,
ATTR_INDEX_COLOR,
ATTR_INDEX_NORMAL,
};

const uint32_t sharedStride = vao->attribs[ATTR_INDEX_POSITION].stride;
bool interleaved = sharedStride != 0;
for (int i = 1; i < 4 && interleaved; ++i) {
const vaoAttrib_t &a = vao->attribs[trackedAttribs[i]];
if (!a.enabled) {
continue;
}
if (a.stride != sharedStride) {
interleaved = false;
}
}

int nextBufferIndex = 0;
for (int i = 0; i < 4; ++i) {
const int attrIndex = trackedAttribs[i];
const vaoAttrib_t &a = vao->attribs[attrIndex];
if (!a.enabled) {
continue;
}

const MTL::VertexFormat fmt = GLAttribToVertexFormat(a);
if (fmt == MTL::VertexFormatInvalid) {
ri.Printf(PRINT_WARNING, "[Metal] Unsupported VAO format for attrib %d (type=%u count=%u norm=%u)\n", attrIndex, a.type, a.count, a.normalized);
continue;
}

const int bufferIndex = interleaved ? 0 : nextBufferIndex++;
MTL::VertexAttributeDescriptor *attr = vertexDesc->attributes()->object(attrIndex);
attr->setFormat(fmt);
attr->setOffset(a.offset);
attr->setBufferIndex(bufferIndex);

MTL::VertexBufferLayoutDescriptor *layout = vertexDesc->layouts()->object(bufferIndex);
layout->setStride(a.stride);
layout->setStepFunction(MTL::VertexStepFunctionPerVertex);
layout->setStepRate(1);
}

if (outHash) {
*outHash = HashVertexDescriptor(vertexDesc);
}

return vertexDesc;
}

int GLMetal_RegisterProgramPipeline(const char *programName,
                                    MTL::Function *vertexFunction,
                                    MTL::Function *fragmentFunction,
                                    const MTL::VertexDescriptor *vertexDesc,
                                    uint64_t vertexDescHash)
{
const std::string nameKey = ProgramNameKey(programName);
ProgramFnsKey key { nameKey, vertexDescHash };
MTL::VertexDescriptor *retainedVertexDesc = const_cast<MTL::VertexDescriptor *>(vertexDesc);

if (vertexFunction) {
vertexFunction->retain();
}
if (fragmentFunction) {
fragmentFunction->retain();
}
if (retainedVertexDesc) {
retainedVertexDesc->retain();
}

const MetalProgramFns fns { vertexFunction, fragmentFunction, retainedVertexDesc, vertexDescHash };
auto it = s_programIds.find(key);
if (it != s_programIds.end()) {
MetalProgramRecord &rec = s_programs[(size_t)it->second];
ReleaseProgramFns(rec.fns);
rec.fns = fns;
rec.pipelines.clear();
return it->second;
}

const int id = (int)s_programs.size();
MetalProgramRecord rec;
rec.name = nameKey;
rec.fns = fns;
s_programs.push_back(rec);
s_programIds.emplace(key, id);
return id;
}

// Move completed async prewarm PSOs into the pipeline cache (render thread only).
static void GLMetal_DrainPendingPipelines(void)
{
	std::vector<MetalPendingPipeline> ready;
	{
		std::lock_guard<std::mutex> lock(s_pendingPipelinesMutex);
		if (s_pendingPipelines.empty()) {
			return;
		}
		ready.swap(s_pendingPipelines);
	}
	for (auto &entry : ready) {
		if (!s_pipelineCache.emplace(entry.key, entry.pso).second) {
			entry.pso->release(); // built synchronously in the meantime
		}
	}
}

MTL::RenderPipelineState *GLMetal_GetPipelineForProgram(int programId, uint32_t stateBits)
{
if (programId < 0 || (size_t)programId >= s_programs.size()) {
return nullptr;
}
MetalProgramRecord &rec = s_programs[(size_t)programId];

GLMetal_RefreshRenderTargetFormats();
const uint64_t key = (uint64_t)(stateBits & PSO_AFFECTING_STATE_BITS) |
                     ((uint64_t)(s_rtFormats.colorFormat & 0xFFFF) << 8) |
                     ((uint64_t)(s_rtFormats.depthStencilFormat & 0xFFFF) << 24) |
                     ((uint64_t)MTL::ColorWriteMaskAll << 40);
for (const auto &entry : rec.pipelines) {
if (entry.key == key) {
return entry.pso;
}
}

MTL::RenderPipelineState *pso = GLMetal_GetOrCreatePipelineWithFormats(rec.name.c_str(), rec.fns.vert, rec.fns.frag,
                                                                       rec.fns.vtxDesc, rec.fns.vtxDescHash, stateBits,
                                                                       MTL::ColorWriteMaskAll,
                                                                       s_rtFormats.colorFormat, s_rtFormats.depthStencilFormat);
rec.pipelines.push_back(MetalProgramPipelineEntry { key, pso });
return pso;
}

MTL::RenderPipelineState *GLMetal_GetPipelineForState(const char *programName, uint64_t vertexDescHash, uint32_t stateBits)
{
ProgramFnsKey key { ProgramNameKey(programName), vertexDescHash };
auto it = s_programIds.find(key);
if (it == s_programIds.end()) {
return nullptr;
}
return GLMetal_GetPipelineForProgram(it->second, stateBits);
}

static PipelineCacheKey GLMetal_MakePipelineCacheKey(const char *programName, uint32_t stateBits, uint64_t vertexDescHash,
                                                     MTL::ColorWriteMask colorWriteMask,
                                                     MTL::PixelFormat colorFormat, MTL::PixelFormat depthStencilFormat)
{
// Narrow the cache key to only the bits that affect MTLRenderPipelineState.
PipelineCacheKey key {
(programName && programName[0]) ? programName : "unnamed",
stateBits & PSO_AFFECTING_STATE_BITS,
vertexDescHash,
colorFormat,
depthStencilFormat,
GLMetal_StencilFormatForDepthStencilFormat(depthStencilFormat),
(uint8_t)colorWriteMask
};
return key;
}

static MTL::RenderPipelineDescriptor *GLMetal_BuildPipelineDescriptor(const PipelineCacheKey &key,
                                                                     MTL::Function *vertexFunction,
                                                                     MTL::Function *fragmentFunction,
                                                                     const MTL::VertexDescriptor *vertexDesc,
                                                                     uint32_t stateBits)
{
MTL::RenderPipelineDescriptor *pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
pipelineDesc->setVertexFunction(vertexFunction);
pipelineDesc->setFragmentFunction(fragmentFunction);
pipelineDesc->setVertexDescriptor(const_cast<MTL::VertexDescriptor *>(vertexDesc));
pipelineDesc->setLabel(NS::String::string(key.programName.c_str(), NS::UTF8StringEncoding));

MTL::RenderPipelineColorAttachmentDescriptor *colorAttachment = pipelineDesc->colorAttachments()->object(0);
colorAttachment->setPixelFormat(key.colorFormat);
// PARITY-SEAM(Metal-device): render/depth-only targets use different depth
// formats; PSO attachment formats must exactly match the bound render target.
pipelineDesc->setDepthAttachmentPixelFormat(key.depthStencilFormat);
pipelineDesc->setStencilAttachmentPixelFormat(key.stencilFormat);
// GL2 ref: depth-only FBO render passes (pshadow capture) have no color attachment.
// When colorFormat is Invalid, skip blend state — there is no color attachment to blend.
if (key.colorFormat != MTL::PixelFormatInvalid) {
colorAttachment->setWriteMask((MTL::ColorWriteMask)key.colorWriteMask);
ConfigureBlendState(colorAttachment, stateBits);
}
return pipelineDesc;
}

MTL::RenderPipelineState *GLMetal_GetOrCreatePipelineWithFormats(const char *programName,
                                                                  MTL::Function *vertexFunction,
                                                                  MTL::Function *fragmentFunction,
                                                                  const MTL::VertexDescriptor *vertexDesc,
                                                                  uint64_t vertexDescHash,
                                                                  uint32_t stateBits,
                                                                  MTL::ColorWriteMask colorWriteMask,
                                                                  MTL::PixelFormat colorFormat,
                                                                  MTL::PixelFormat depthStencilFormat)
{
MTL::Device *device = MetalDevice();
if (!device || !vertexFunction || !fragmentFunction) {
ri.Printf(PRINT_WARNING, "[Metal] GLMetal_GetOrCreatePipeline: invalid input or device not ready\n");
return nullptr;
}

if (vertexDesc && vertexDescHash == 0) {
vertexDescHash = HashVertexDescriptor(vertexDesc);
}

const PipelineCacheKey key = GLMetal_MakePipelineCacheKey(programName, stateBits, vertexDescHash, colorWriteMask,
                                                          colorFormat, depthStencilFormat);

auto it = s_pipelineCache.find(key);
if (it != s_pipelineCache.end()) {
return it->second;
}
if (s_failedPipelineCache.find(key) != s_failedPipelineCache.end()) {
return nullptr;
}
// A prewarm compile for this key may have finished since the last drain.
GLMetal_DrainPendingPipelines();
it = s_pipelineCache.find(key);
if (it != s_pipelineCache.end()) {
return it->second;
}

MTL::RenderPipelineDescriptor *pipelineDesc = GLMetal_BuildPipelineDescriptor(key, vertexFunction, fragmentFunction, vertexDesc, stateBits);
NS::Error *error = nullptr;
MTL::RenderPipelineState *pipeline = device->newRenderPipelineState(pipelineDesc, &error);
pipelineDesc->release();

if (!pipeline) {
if (s_failedPipelineCache.emplace(key).second) {
ri.Printf(PRINT_WARNING, "[Metal] Pipeline create failed (%s): %s\n", key.programName.c_str(), MetalErrorString(error));
}
return nullptr;
}

s_pipelineCache.emplace(key, pipeline);
return pipeline;
}

MTL::RenderPipelineState *GLMetal_GetOrCreatePipeline(const char *programName,
                                                       MTL::Function *vertexFunction,
                                                       MTL::Function *fragmentFunction,
                                                       const MTL::VertexDescriptor *vertexDesc,
                                                       uint64_t vertexDescHash,
                                                       uint32_t stateBits,
                                                       MTL::ColorWriteMask colorWriteMask)
{
GLMetal_RefreshRenderTargetFormats();
return GLMetal_GetOrCreatePipelineWithFormats(programName, vertexFunction, fragmentFunction, vertexDesc, vertexDescHash,
                                              stateBits, colorWriteMask,
                                              s_rtFormats.colorFormat, s_rtFormats.depthStencilFormat);
}

void GLMetal_PrewarmProgramPipelines(int programId, const uint32_t *stateBits, int count,
                                     MTL::PixelFormat colorFormat, MTL::PixelFormat depthStencilFormat)
{
MTL::Device *device = MetalDevice();
if (!device || !stateBits || count <= 0 || programId < 0 || (size_t)programId >= s_programs.size()) {
return;
}
const MetalProgramRecord &rec = s_programs[(size_t)programId];
if (!rec.fns.vert || !rec.fns.frag) {
return;
}

std::unordered_set<PipelineCacheKey, PipelineCacheKeyHash> queued;
for (int i = 0; i < count; ++i) {
const PipelineCacheKey key = GLMetal_MakePipelineCacheKey(rec.name.c_str(), stateBits[i], rec.fns.vtxDescHash,
                                                          MTL::ColorWriteMaskAll, colorFormat, depthStencilFormat);
if (!queued.insert(key).second ||
    s_pipelineCache.find(key) != s_pipelineCache.end() ||
    s_failedPipelineCache.find(key) != s_failedPipelineCache.end()) {
continue;
}

MTL::RenderPipelineDescriptor *pipelineDesc = GLMetal_BuildPipelineDescriptor(key, rec.fns.vert, rec.fns.frag, rec.fns.vtxDesc, stateBits[i]);
s_pendingPipelineCompiles.fetch_add(1);
// Completion runs on a Metal compiler thread: only touch the mutex-guarded queue.
device->newRenderPipelineState(pipelineDesc, [key](MTL::RenderPipelineState *pso, NS::Error *) {
	if (pso) {
		pso->retain();
		std::lock_guard<std::mutex> lock(s_pendingPipelinesMutex);
		s_pendingPipelines.push_back(MetalPendingPipeline { key, pso });
	}
	s_pendingPipelineCompiles.fetch_sub(1);
});
pipelineDesc->release();
}
}

// Wait for outstanding async PSO compiles (shutdown only).
static void GLMetal_WaitForPendingPipelineCompiles(void)
{
	while (s_pendingPipelineCompiles.load() > 0) {
		dispatch_semaphore_t sleeper = dispatch_semaphore_create(0);
		dispatch_semaphore_wait(sleeper, dispatch_time(DISPATCH_TIME_NOW, 1000000)); // 1 ms
		dispatch_release(sleeper);
	}
	std::lock_guard<std::mutex> lock(s_pendingPipelinesMutex);
	for (auto &entry : s_pendingPipelines) {
		entry.pso->release();
	}
	s_pendingPipelines.clear();
}

extern "C" void GLMetal_SetDepthState(uint32_t glStateBits)
{
s_depthStateBits = glStateBits;
s_depthStateValid = true;
GLMetal_ApplyDepthState();
}

extern "C" void GLMetal_SetStencilState(qboolean enabled, uint32_t glCompareFunc, uint32_t glPassOp, uint32_t glDepthFailOp, uint32_t glFailOp, uint8_t referenceValue)
{
	if (!enabled) {
		s_stencilStateBits = 0;
		GLMetal_ApplyDepthState();
		return;
	}

	const uint32_t compareBits = GLMetal_StencilCompareBitsFromGL(glCompareFunc);
	const uint32_t passBits = GLMetal_StencilOpBitsFromGL(glPassOp);
	const uint32_t depthFailBits = GLMetal_StencilOpBitsFromGL(glDepthFailOp);
	const uint32_t failBits = GLMetal_StencilOpBitsFromGL(glFailOp);

	s_stencilStateBits = STENCIL_TEST_ENABLE_BIT |
	                     ((compareBits << STENCIL_COMPARE_SHIFT) & STENCIL_COMPARE_MASK) |
	                     ((passBits << STENCIL_PASS_SHIFT) & STENCIL_PASS_MASK) |
	                     ((depthFailBits << STENCIL_DEPTH_FAIL_SHIFT) & STENCIL_DEPTH_FAIL_MASK) |
	                     ((failBits << STENCIL_FAIL_SHIFT) & STENCIL_FAIL_MASK) |
	                     ((((uint32_t)referenceValue) << STENCIL_REFERENCE_SHIFT) & STENCIL_REFERENCE_MASK);

	GLMetal_ApplyDepthState();
}

extern "C" void GLMetal_SetDepthRange(float znear, float zfar)
{
if (znear < 0.0f) {
znear = 0.0f;
} else if (znear > 1.0f) {
znear = 1.0f;
}
if (zfar < 0.0f) {
zfar = 0.0f;
} else if (zfar > 1.0f) {
zfar = 1.0f;
}

s_depthRangeNear = znear;
s_depthRangeFar = zfar;
GLMetal_ApplyViewport();
}

// Set the per-view viewport+scissor rectangle in GL (bottom-left) pixel
// coords. Applied (Y-flipped) to the current encoder. Independent of depth
// range. GL2 ref: SetViewportAndScissor / RB_SetGL2D qglViewport/qglScissor.
extern "C" void GLMetal_SetViewport(int x, int y, int w, int h)
{
s_viewportX = x;
s_viewportY = y;
s_viewportW = w;
s_viewportH = h;
GLMetal_ApplyViewport();
}

extern "C" void GLMetal_SetCullMode(int cullMode)
{
s_cullMode = cullMode;
GLMetal_ApplyCullMode();
}

static int GLMetal_CullModeFromGLCullType(int glCullType)
{
	// tr_local.h cullType_t order: CT_FRONT_SIDED=0, CT_BACK_SIDED=1, CT_TWO_SIDED=2.
	if (glCullType == 2) {
		return 0;
	}
	if (glCullType == 0) {
		return 1;
	}
	return 2;
}

static MTL::RenderPipelineState *GLMetal_GetShadowVolumePipeline(void)
{
	if (!GLMetal_LoadProgramFunctions("shadowvolume", "shadowvolume_vert", "shadowvolume_frag", &s_shadowvolumeVertFn, &s_shadowvolumeFragFn)) {
		return nullptr;
	}
	return GLMetal_GetOrCreatePipeline("shadowvolume",
	                                   s_shadowvolumeVertFn,
	                                   s_shadowvolumeFragFn,
	                                   nullptr,
	                                   0,
	                                   0,
	                                   MTL::ColorWriteMaskNone);
}

// Copies shadow-volume positions into frame memory and binds them + the MVP.
static bool GLMetal_BindShadowVolumeVertices(const float *positions, int numVerts, const float *mvp16)
{
	const size_t positionsSize = (size_t)numVerts * sizeof(float[4]);
	MTL::Buffer *buffer = nullptr;
	NS::UInteger offset = 0;
	void *dst = GLMetal_FrameAlloc(positionsSize, 16, &buffer, &offset);
	if (!dst) {
		ri.Printf(PRINT_DEVELOPER, "[Metal] shadow volume: failed to allocate %zu bytes of frame memory\n", positionsSize);
		return false;
	}
	memcpy(dst, positions, positionsSize);
	GLMetal_SetVertexBuffer(buffer, offset, 2);
	s_encoder->setVertexBytes(mvp16, (NS::UInteger)sizeof(float[16]), 1);
	GLMetal_InvalidateVertexSlot(1);
	return true;
}

extern "C" void GLMetal_DrawShadowVolume(const float *positions, int numVerts, const float *mvp16, int glCullType)
{
	if (!positions || !mvp16) {
		return;
	}
	if (numVerts <= 0) {
		return;
	}
	if (!s_frameBegun) {
		GLMetal_BeginFrame();
	}
	if (!GLMetal_EnsureEncoder()) {
		return;
	}
	MTL::RenderPipelineState *pipeline = GLMetal_GetShadowVolumePipeline();
	if (!pipeline) {
		return;
	}

	GLMetal_SetDepthState(0);
	GLMetal_SetRenderPipelineState(pipeline);
	GLMetal_SetEncoderCullMode(GLMetal_ToMetalCullMode(GLMetal_CullModeFromGLCullType(glCullType)));
	if (!GLMetal_BindShadowVolumeVertices(positions, numVerts, mvp16)) {
		return;
	}
	s_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)numVerts);
	// GL_Cull state is tracked separately (s_cullMode); restore it for the next generic draw.
	GLMetal_ApplyCullMode();
}

// GL2 ref: tr_shadows.c:212-237 — both depth-pass stencil passes, one vertex upload.
extern "C" void GLMetal_DrawShadowVolumeStencilPasses(const float *positions, int numVerts, const float *mvp16)
{
	if (!positions || !mvp16 || numVerts <= 0) {
		return;
	}
	if (!s_frameBegun) {
		GLMetal_BeginFrame();
	}
	if (!GLMetal_EnsureEncoder()) {
		return;
	}
	MTL::RenderPipelineState *pipeline = GLMetal_GetShadowVolumePipeline();
	if (!pipeline) {
		return;
	}

	GLMetal_SetRenderPipelineState(pipeline);
	if (!GLMetal_BindShadowVolumeVertices(positions, numVerts, mvp16)) {
		return;
	}

	// Pass 1: GL_Cull(CT_BACK_SIDED) + stencil INCR on depth pass.
	GLMetal_SetStencilState(qtrue, GL_ALWAYS, GL_INCR, GL_KEEP, GL_KEEP, 1);
	GLMetal_SetDepthState(0);
	GLMetal_SetEncoderCullMode(GLMetal_ToMetalCullMode(GLMetal_CullModeFromGLCullType(1)));
	s_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)numVerts);

	// Pass 2: GL_Cull(CT_FRONT_SIDED) + stencil DECR on depth pass.
	GLMetal_SetStencilState(qtrue, GL_ALWAYS, GL_DECR, GL_KEEP, GL_KEEP, 1);
	GLMetal_SetEncoderCullMode(GLMetal_ToMetalCullMode(GLMetal_CullModeFromGLCullType(0)));
	s_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)numVerts);

	GLMetal_SetStencilState(qfalse, GL_ALWAYS, GL_KEEP, GL_KEEP, GL_KEEP, 0);
	GLMetal_ApplyCullMode();
}

extern "C" void GLMetal_DrawShadowFinish(const float *rgba4)
{
	if (!rgba4) {
		return;
	}
	if (!s_frameBegun) {
		GLMetal_BeginFrame();
	}
	if (!GLMetal_EnsureEncoder()) {
		return;
	}
	if (!GLMetal_LoadProgramFunctions("shadowfinish", "shadowfinish_vert", "shadowfinish_frag", &s_shadowfinishVertFn, &s_shadowfinishFragFn)) {
		return;
	}

	const uint32_t blendStateBits = GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO;
	MTL::RenderPipelineState *pipeline = GLMetal_GetOrCreatePipeline("shadowfinish",
	                                                                 s_shadowfinishVertFn,
	                                                                 s_shadowfinishFragFn,
	                                                                 nullptr,
	                                                                 0,
	                                                                 blendStateBits,
	                                                                 MTL::ColorWriteMaskAll);
	if (!pipeline) {
		return;
	}

	GLMetal_SetDepthState(GLS_DEPTHMASK_TRUE);

	static const float kShadowFinishPositions[6][4] = {
		{ -1.0f,  1.0f, 0.0f, 1.0f },
		{  1.0f,  1.0f, 0.0f, 1.0f },
		{  1.0f, -1.0f, 0.0f, 1.0f },
		{ -1.0f,  1.0f, 0.0f, 1.0f },
		{  1.0f, -1.0f, 0.0f, 1.0f },
		{ -1.0f, -1.0f, 0.0f, 1.0f }
	};

	MTL::RenderCommandEncoder *enc = s_encoder;
	GLMetal_SetRenderPipelineState(pipeline);
	GLMetal_SetEncoderCullMode(MTL::CullModeNone);
	enc->setVertexBytes(kShadowFinishPositions, (NS::UInteger)sizeof(kShadowFinishPositions), 2);
	enc->setFragmentBytes(rgba4, (NS::UInteger)sizeof(float[4]), 0);
	GLMetal_InvalidateVertexSlot(2);
	GLMetal_InvalidateFragmentSlot(0);
	enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)6);
	GLMetal_ApplyCullMode();
}

static MTL::RenderPipelineState *GLMetal_GetOrCreateTonemapPipeline(void)
{
	if (s_tonemapPipeline) {
		return s_tonemapPipeline;
	}

	const char *const programName = "metalPresentTonemap";
	const char *mslVert = GLMetal_GetShaderSource(programName, qfalse);
	const char *mslFrag = GLMetal_GetShaderSource(programName, qtrue);
	if (!mslVert || !mslFrag) {
		ri.Printf(PRINT_WARNING, "[Metal] tonemap pass missing MSL for '%s'\n", programName);
		return nullptr;
	}

	MTL::Library *vertLib = GLMetal_LoadShaderLibraryFromSource(mslVert, programName);
	MTL::Library *fragLib = GLMetal_LoadShaderLibraryFromSource(mslFrag, programName);
	if (!vertLib || !fragLib) {
		if (vertLib) {
			vertLib->release();
		}
		if (fragLib) {
			fragLib->release();
		}
		return nullptr;
	}

	NS::String *vertName = NS::String::string("metalPresentTonemap_vert", NS::UTF8StringEncoding);
	NS::String *fragName = NS::String::string("metalPresentTonemap_frag", NS::UTF8StringEncoding);
	MTL::Function *vertFn = vertLib->newFunction(vertName);
	MTL::Function *fragFn = fragLib->newFunction(fragName);
	if (!vertFn || !fragFn) {
		ri.Printf(PRINT_WARNING, "[Metal] tonemap pass function lookup failed for '%s'\n", programName);
		if (vertFn) {
			vertFn->release();
		}
		if (fragFn) {
			fragFn->release();
		}
		vertLib->release();
		fragLib->release();
		return nullptr;
	}

	MTL::VertexDescriptor *vertexDesc = MTL::VertexDescriptor::vertexDescriptor();
	vertexDesc->retain();
	vertexDesc->layouts()->object(0)->setStride(0);
	vertexDesc->layouts()->object(0)->setStepFunction(MTL::VertexStepFunctionPerVertex);
	vertexDesc->layouts()->object(0)->setStepRate(1);

	MTL::RenderPipelineDescriptor *pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pipelineDesc->setVertexFunction(vertFn);
	pipelineDesc->setFragmentFunction(fragFn);
	pipelineDesc->setVertexDescriptor(vertexDesc);
	pipelineDesc->setLabel(NS::String::string(programName, NS::UTF8StringEncoding));
	pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
	pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
	pipelineDesc->colorAttachments()->object(0)->setBlendingEnabled(false);

	NS::Error *error = nullptr;
	s_tonemapPipeline = MetalDevice() ? MetalDevice()->newRenderPipelineState(pipelineDesc, &error) : nullptr;
	if (!s_tonemapPipeline) {
		ri.Printf(PRINT_WARNING, "[Metal] tonemap pipeline create failed (%s): %s\n", programName, MetalErrorString(error));
	}
	pipelineDesc->release();

	vertexDesc->release();
	vertFn->release();
	fragFn->release();
	vertLib->release();
	fragLib->release();
	return s_tonemapPipeline;
}

static MTL::RenderPipelineState *GLMetal_GetOrCreateGreyscalePipeline(void)
{
	if (s_greyscalePipeline) {
		return s_greyscalePipeline;
	}

	const char *const programName = "metalPresentGreyscale";
	const char *mslVert = GLMetal_GetShaderSource(programName, qfalse);
	const char *mslFrag = GLMetal_GetShaderSource(programName, qtrue);
	if (!mslVert || !mslFrag) {
		ri.Printf(PRINT_WARNING, "[Metal] greyscale pass missing MSL for '%s'\n", programName);
		return nullptr;
	}

	MTL::Library *vertLib = GLMetal_LoadShaderLibraryFromSource(mslVert, programName);
	MTL::Library *fragLib = GLMetal_LoadShaderLibraryFromSource(mslFrag, programName);
	if (!vertLib || !fragLib) {
		if (vertLib) {
			vertLib->release();
		}
		if (fragLib) {
			fragLib->release();
		}
		return nullptr;
	}

	NS::String *vertName = NS::String::string("metalPresentGreyscale_vert", NS::UTF8StringEncoding);
	NS::String *fragName = NS::String::string("metalPresentGreyscale_frag", NS::UTF8StringEncoding);
	MTL::Function *vertFn = vertLib->newFunction(vertName);
	MTL::Function *fragFn = fragLib->newFunction(fragName);
	if (!vertFn || !fragFn) {
		ri.Printf(PRINT_WARNING, "[Metal] greyscale pass function lookup failed for '%s'\n", programName);
		if (vertFn) {
			vertFn->release();
		}
		if (fragFn) {
			fragFn->release();
		}
		vertLib->release();
		fragLib->release();
		return nullptr;
	}

	MTL::VertexDescriptor *vertexDesc = MTL::VertexDescriptor::vertexDescriptor();
	vertexDesc->retain();
	vertexDesc->layouts()->object(0)->setStride(0);
	vertexDesc->layouts()->object(0)->setStepFunction(MTL::VertexStepFunctionPerVertex);
	vertexDesc->layouts()->object(0)->setStepRate(1);

	MTL::RenderPipelineDescriptor *pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pipelineDesc->setVertexFunction(vertFn);
	pipelineDesc->setFragmentFunction(fragFn);
	pipelineDesc->setVertexDescriptor(vertexDesc);
	pipelineDesc->setLabel(NS::String::string(programName, NS::UTF8StringEncoding));
	pipelineDesc->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
	pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
	pipelineDesc->colorAttachments()->object(0)->setBlendingEnabled(false);

	NS::Error *error = nullptr;
	s_greyscalePipeline = MetalDevice() ? MetalDevice()->newRenderPipelineState(pipelineDesc, &error) : nullptr;
	if (!s_greyscalePipeline) {
		ri.Printf(PRINT_WARNING, "[Metal] greyscale pipeline create failed (%s): %s\n", programName, MetalErrorString(error));
	}
	pipelineDesc->release();

	vertexDesc->release();
	vertFn->release();
	fragFn->release();
	vertLib->release();
	fragLib->release();
	return s_greyscalePipeline;
}

// GL2 ref: tr_backend.c:960-1064 (RB_DrawSurfs shadowmask full-screen composite pass).
static MTL::RenderPipelineState *GLMetal_GetOrCreateShadowmaskPipeline(void)
{
	const MTL::PixelFormat colorFormat = GLMetal_CurrentRenderTargetColorFormat();
	const int shadowFilterLevel = (r_shadowFilter != nullptr) ? r_shadowFilter->integer : 0;
	if (colorFormat == MTL::PixelFormatInvalid) {
		ri.Printf(PRINT_WARNING, "[Metal] shadowmask pass missing color render target format\n");
		return nullptr;
	}
	if (s_shadowmaskPipeline &&
		s_shadowmaskPipelineColorFormat == colorFormat &&
		s_shadowmaskPipelineFilterLevel == shadowFilterLevel) {
		return s_shadowmaskPipeline;
	}
	if (s_shadowmaskPipeline) {
		s_shadowmaskPipeline->release();
		s_shadowmaskPipeline = nullptr;
		s_shadowmaskPipelineColorFormat = MTL::PixelFormatInvalid;
		s_shadowmaskPipelineFilterLevel = INT_MIN;
	}

	const char *const programName = "shadowmask";
	const char *mslVert = GLMetal_GetShaderSource(programName, qfalse);
	const char *mslFrag = GLMetal_GetShaderSource(programName, qtrue);
	if (!mslVert || !mslFrag) {
		ri.Printf(PRINT_WARNING, "[Metal] shadowmask pass missing MSL for '%s'\n", programName);
		return nullptr;
	}

	std::string shadowmaskDefines;
	if (shadowFilterLevel >= 1) {
		shadowmaskDefines.append("#define USE_SHADOW_FILTER\n");
	}
	if (shadowFilterLevel >= 2) {
		shadowmaskDefines.append("#define USE_SHADOW_FILTER2\n");
	}
	const char *defines = shadowmaskDefines.empty() ? nullptr : shadowmaskDefines.c_str();

	MTL::Library *vertLib = GLMetal_LoadShaderLibraryFromSourceWithDefines(mslVert, programName, defines);
	MTL::Library *fragLib = GLMetal_LoadShaderLibraryFromSourceWithDefines(mslFrag, programName, defines);
	if (!vertLib || !fragLib) {
		if (vertLib) {
			vertLib->release();
		}
		if (fragLib) {
			fragLib->release();
		}
		return nullptr;
	}

	NS::String *vertName = NS::String::string("shadowmask_vert", NS::UTF8StringEncoding);
	NS::String *fragName = NS::String::string("shadowmask_frag", NS::UTF8StringEncoding);
	MTL::Function *vertFn = vertLib->newFunction(vertName);
	MTL::Function *fragFn = fragLib->newFunction(fragName);
	if (!vertFn || !fragFn) {
		ri.Printf(PRINT_WARNING, "[Metal] shadowmask pass function lookup failed for '%s'\n", programName);
		if (vertFn) {
			vertFn->release();
		}
		if (fragFn) {
			fragFn->release();
		}
		vertLib->release();
		fragLib->release();
		return nullptr;
	}

	MTL::VertexDescriptor *vertexDesc = MTL::VertexDescriptor::vertexDescriptor();
	vertexDesc->retain();
	vertexDesc->layouts()->object(0)->setStride(0);
	vertexDesc->layouts()->object(0)->setStepFunction(MTL::VertexStepFunctionPerVertex);
	vertexDesc->layouts()->object(0)->setStepRate(1);

	MTL::RenderPipelineDescriptor *pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pipelineDesc->setVertexFunction(vertFn);
	pipelineDesc->setFragmentFunction(fragFn);
	pipelineDesc->setVertexDescriptor(vertexDesc);
	pipelineDesc->setLabel(NS::String::string(programName, NS::UTF8StringEncoding));
	pipelineDesc->colorAttachments()->object(0)->setPixelFormat(colorFormat);
	pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
	pipelineDesc->setStencilAttachmentPixelFormat(MTL::PixelFormatInvalid);
	pipelineDesc->colorAttachments()->object(0)->setBlendingEnabled(false);

	NS::Error *error = nullptr;
	s_shadowmaskPipeline = MetalDevice() ? MetalDevice()->newRenderPipelineState(pipelineDesc, &error) : nullptr;
	if (!s_shadowmaskPipeline) {
		ri.Printf(PRINT_WARNING, "[Metal] shadowmask pipeline create failed (%s): %s\n", programName, MetalErrorString(error));
	} else {
		s_shadowmaskPipelineColorFormat = colorFormat;
		s_shadowmaskPipelineFilterLevel = shadowFilterLevel;
	}
	pipelineDesc->release();

	vertexDesc->release();
	vertFn->release();
	fragFn->release();
	vertLib->release();
	fragLib->release();
	return s_shadowmaskPipeline;
}

// GL2 ref: tr_backend.c:1044-1063 (RB_DrawSurfs depth blur two-pass shadow blur block).
static MTL::RenderPipelineState *GLMetal_GetOrCreateDepthBlurPipeline(qboolean horizontal, qboolean useDepth)
{
	const MTL::PixelFormat colorFormat = GLMetal_CurrentRenderTargetColorFormat();
	if (colorFormat == MTL::PixelFormatInvalid) {
		ri.Printf(PRINT_WARNING, "[Metal] depthBlur pass missing color render target format\n");
		return nullptr;
	}

	const int hIndex = horizontal ? 1 : 0;
	const int dIndex = useDepth ? 1 : 0;
	if (s_depthBlurPipelineColorFormat != colorFormat) {
		for (int h = 0; h < 2; ++h) {
			for (int d = 0; d < 2; ++d) {
				if (s_depthBlurPipeline[h][d]) {
					s_depthBlurPipeline[h][d]->release();
					s_depthBlurPipeline[h][d] = nullptr;
				}
			}
		}
		s_depthBlurPipelineColorFormat = colorFormat;
	}
	if (s_depthBlurPipeline[hIndex][dIndex]) {
		return s_depthBlurPipeline[hIndex][dIndex];
	}

	const char *const programName = "depthBlur";
	const char *mslVert = GLMetal_GetShaderSource(programName, qfalse);
	const char *mslFrag = GLMetal_GetShaderSource(programName, qtrue);
	if (!mslVert || !mslFrag) {
		ri.Printf(PRINT_WARNING, "[Metal] depthBlur pass missing MSL for '%s'\n", programName);
		return nullptr;
	}

	std::string depthBlurDefines = horizontal ? "#define USE_HORIZONTAL_BLUR\n" : "#define USE_VERTICAL_BLUR\n";
	if (useDepth) {
		depthBlurDefines.append("#define USE_DEPTH\n");
	}

	MTL::Library *vertLib = GLMetal_LoadShaderLibraryFromSourceWithDefines(mslVert, programName, depthBlurDefines.c_str());
	MTL::Library *fragLib = GLMetal_LoadShaderLibraryFromSourceWithDefines(mslFrag, programName, depthBlurDefines.c_str());
	if (!vertLib || !fragLib) {
		if (vertLib) {
			vertLib->release();
		}
		if (fragLib) {
			fragLib->release();
		}
		return nullptr;
	}

	NS::String *vertName = NS::String::string("depthBlur_vert", NS::UTF8StringEncoding);
	NS::String *fragName = NS::String::string("depthBlur_frag", NS::UTF8StringEncoding);
	MTL::Function *vertFn = vertLib->newFunction(vertName);
	MTL::Function *fragFn = fragLib->newFunction(fragName);
	if (!vertFn || !fragFn) {
		ri.Printf(PRINT_WARNING, "[Metal] depthBlur pass function lookup failed for '%s'\n", programName);
		if (vertFn) {
			vertFn->release();
		}
		if (fragFn) {
			fragFn->release();
		}
		vertLib->release();
		fragLib->release();
		return nullptr;
	}

	MTL::VertexDescriptor *vertexDesc = MTL::VertexDescriptor::vertexDescriptor();
	vertexDesc->retain();
	vertexDesc->layouts()->object(0)->setStride(0);
	vertexDesc->layouts()->object(0)->setStepFunction(MTL::VertexStepFunctionPerVertex);
	vertexDesc->layouts()->object(0)->setStepRate(1);

	MTL::RenderPipelineDescriptor *pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pipelineDesc->setVertexFunction(vertFn);
	pipelineDesc->setFragmentFunction(fragFn);
	pipelineDesc->setVertexDescriptor(vertexDesc);
	pipelineDesc->setLabel(NS::String::string(programName, NS::UTF8StringEncoding));
	pipelineDesc->colorAttachments()->object(0)->setPixelFormat(colorFormat);
	pipelineDesc->setDepthAttachmentPixelFormat(MTL::PixelFormatInvalid);
	pipelineDesc->setStencilAttachmentPixelFormat(MTL::PixelFormatInvalid);
	pipelineDesc->colorAttachments()->object(0)->setBlendingEnabled(false);

	NS::Error *error = nullptr;
	MTL::RenderPipelineState *pipeline = MetalDevice() ? MetalDevice()->newRenderPipelineState(pipelineDesc, &error) : nullptr;
	if (!pipeline) {
		ri.Printf(PRINT_WARNING, "[Metal] depthBlur pipeline create failed (%s): %s\n", programName, MetalErrorString(error));
	}
	pipelineDesc->release();

	vertexDesc->release();
	vertFn->release();
	fragFn->release();
	vertLib->release();
	fragLib->release();

	s_depthBlurPipeline[hIndex][dIndex] = pipeline;
	return s_depthBlurPipeline[hIndex][dIndex];
}

// GL2 ref: tr_backend.c:958-1064 (RB_DrawSurfs shadowmask FBO composite block).
extern "C" void GLMetal_ShadowmaskPass(uint32_t dstColorTexnum, int viewportX, int viewportY, int viewportW, int viewportH,
                                       uint32_t screenDepthTexnum, const uint32_t cascadeTexnum[4], qboolean useCascades,
                                       const MetalShadowmaskUniforms *uniforms)
{
	static const size_t kShadowmaskUniformBytes = 352;
	static const float kShadowmaskPositions[6][4] = {
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f,  1.0f, 0.0f, 1.0f },
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f,  1.0f, 0.0f, 1.0f },
		{ -1.0f,  1.0f, 0.0f, 1.0f }
	};
	static const float kShadowmaskTexcoords[6][2] = {
		{ 0.0f, 1.0f },
		{ 1.0f, 1.0f },
		{ 1.0f, 0.0f },
		{ 0.0f, 1.0f },
		{ 1.0f, 0.0f },
		{ 0.0f, 0.0f }
	};

	if (!uniforms || !cascadeTexnum || dstColorTexnum == 0 || screenDepthTexnum == 0) {
		return;
	}
	if (viewportW <= 0 || viewportH <= 0) {
		return;
	}
	if (!s_frameBegun) {
		GLMetal_BeginFrame();
	}
	if (!s_cmdBuffer) {
		return;
	}

	GLMetal_EndEncoderIfNeeded("shadowmask-pass");
	GLMetal_SetRenderTarget(dstColorTexnum, 0, true, false);
	if (s_renderTargetState.targetDrawable || !s_renderTargetState.colorTexture) {
		return;
	}

	MTL::Texture *screenDepthTex = GLMetal_GetTexture(screenDepthTexnum);
	if (!screenDepthTex) {
		ri.Printf(PRINT_WARNING, "[Metal] GLMetal_ShadowmaskPass: missing screen depth texnum %u\n", screenDepthTexnum);
		return;
	}

	MTL::Texture *cascadeTex[4] = { nullptr, nullptr, nullptr, nullptr };
	for (int i = 0; i < 4; ++i) {
		cascadeTex[i] = GLMetal_GetTexture(cascadeTexnum[i]);
	}

	// PARITY-STUB(Metal-shader-variant): shadowmask MSL is currently baked with USE_SHADOW_CASCADE.
	// Non-cascade mode binds cascade 3 depth map into all cascade sampler slots.
	if (!useCascades) {
		cascadeTex[0] = GLMetal_GetTexture(cascadeTexnum[3]);
		cascadeTex[1] = cascadeTex[0];
		cascadeTex[2] = cascadeTex[0];
		cascadeTex[3] = cascadeTex[0];
	}
	if (!cascadeTex[0] || !cascadeTex[1] || !cascadeTex[2] || !cascadeTex[3]) {
		ri.Printf(PRINT_WARNING, "[Metal] GLMetal_ShadowmaskPass: missing cascade depth texture(s)\n");
		return;
	}

	MTL::RenderPipelineState *pipeline = GLMetal_GetOrCreateShadowmaskPipeline();
	if (!pipeline) {
		return;
	}

	const int targetW = (int)s_renderTargetState.colorTexture->width();
	const int targetH = (int)s_renderTargetState.colorTexture->height();
	if (targetW <= 0 || targetH <= 0) {
		return;
	}

	MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
	if (!desc) {
		return;
	}
	MetalRenderTargetClearState &clearState = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);
	desc->colorAttachments()->object(0)->setTexture(s_renderTargetState.colorTexture);
	desc->colorAttachments()->object(0)->setLoadAction(clearState.pendingClearColor ? MTL::LoadActionClear : MTL::LoadActionLoad);
	desc->colorAttachments()->object(0)->setClearColor(clearState.clearColor);
	desc->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);

	// RISK-1: renderCommandEncoder returns autoreleased (+0); retain for cross-call lifetime.
	s_encoder = s_cmdBuffer->renderCommandEncoder(desc);
	if (!s_encoder) {
		return;
	}
	s_encoder->retain();
	GLMetal_OnEncoderOpened();

	MTL::Viewport vp;
	vp.originX = (double)viewportX;
	vp.originY = (double)(targetH - (viewportY + viewportH));
	vp.width = (double)viewportW;
	vp.height = (double)viewportH;
	vp.znear = 0.0;
	vp.zfar = 1.0;
	s_encoder->setViewport(vp);

	int sx0 = viewportX;
	int sx1 = viewportX + viewportW;
	int sy0 = targetH - (viewportY + viewportH);
	int sy1 = targetH - viewportY;
	if (sx0 < 0) sx0 = 0;
	if (sy0 < 0) sy0 = 0;
	if (sx1 > targetW) sx1 = targetW;
	if (sy1 > targetH) sy1 = targetH;

	MTL::ScissorRect sr;
	if (sx1 <= sx0 || sy1 <= sy0) {
		sr.x = 0;
		sr.y = 0;
		sr.width = 0;
		sr.height = 0;
	} else {
		sr.x = (NS::UInteger)sx0;
		sr.y = (NS::UInteger)sy0;
		sr.width = (NS::UInteger)(sx1 - sx0);
		sr.height = (NS::UInteger)(sy1 - sy0);
	}
	s_encoder->setScissorRect(sr);
	s_encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
	s_encoder->setCullMode(MTL::CullModeNone);
	MTL::DepthStencilState *alwaysPass = GLMetal_GetAlwaysPassDepthStencilState();
	if (alwaysPass) {
		s_encoder->setDepthStencilState(alwaysPass);
	}

	MTL::RenderCommandEncoder *enc = s_encoder;
	enc->setRenderPipelineState(pipeline);
	enc->setVertexBytes(uniforms, (NS::UInteger)kShadowmaskUniformBytes, 1);
	enc->setVertexBytes(kShadowmaskPositions, (NS::UInteger)sizeof(kShadowmaskPositions), 2);
	enc->setVertexBytes(kShadowmaskTexcoords, (NS::UInteger)sizeof(kShadowmaskTexcoords), 3);
	enc->setFragmentBytes(uniforms, (NS::UInteger)kShadowmaskUniformBytes, 0);
	enc->setFragmentTexture(screenDepthTex, 0); // [[texture(0)]]
	enc->setFragmentTexture(cascadeTex[0], 5);  // [[texture(5)]]
	enc->setFragmentTexture(cascadeTex[1], 3);  // [[texture(3)]]
	enc->setFragmentTexture(cascadeTex[2], 1);  // [[texture(1)]]
	enc->setFragmentTexture(cascadeTex[3], 6);  // [[texture(6)]]
	enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)6);

	clearState.pendingClearColor = false;
	clearState.pendingClearDepth = false;
	s_openRenderTargetState = s_renderTargetState;
	s_openRenderTargetValid = true;

	GLMetal_EndEncoderIfNeeded("shadowmask-pass");
}

// GL2 ref: tr_backend.c:1044-1063 (RB_DrawSurfs depth blur two-pass shadow blur block).
extern "C" void GLMetal_DepthBlurPass(uint32_t dstColorTexnum, int viewportX, int viewportY, int viewportW, int viewportH,
                                      uint32_t srcColorTexnum, uint32_t srcDepthTexnum, qboolean horizontal, qboolean useDepth,
                                      const MetalDepthBlurUniforms *uniforms)
{
	static const size_t kDepthBlurUniformBytes = 16;
	static const float kDepthBlurPositions[6][4] = {
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f,  1.0f, 0.0f, 1.0f },
		{ -1.0f, -1.0f, 0.0f, 1.0f },
		{  1.0f,  1.0f, 0.0f, 1.0f },
		{ -1.0f,  1.0f, 0.0f, 1.0f }
	};
	static const float kDepthBlurTexcoords[6][2] = {
		{ 0.0f, 1.0f },
		{ 1.0f, 1.0f },
		{ 1.0f, 0.0f },
		{ 0.0f, 1.0f },
		{ 1.0f, 0.0f },
		{ 0.0f, 0.0f }
	};

	if (!uniforms || dstColorTexnum == 0 || srcColorTexnum == 0) {
		return;
	}
	if (viewportW <= 0 || viewportH <= 0) {
		return;
	}
	if (useDepth && srcDepthTexnum == 0) {
		return;
	}
	if (!s_frameBegun) {
		GLMetal_BeginFrame();
	}
	if (!s_cmdBuffer) {
		return;
	}

	GLMetal_EndEncoderIfNeeded("depthblur-pass");
	GLMetal_SetRenderTarget(dstColorTexnum, 0, true, false);
	if (s_renderTargetState.targetDrawable || !s_renderTargetState.colorTexture) {
		return;
	}

	MTL::Texture *srcColorTex = GLMetal_GetTexture(srcColorTexnum);
	if (!srcColorTex) {
		ri.Printf(PRINT_WARNING, "[Metal] GLMetal_DepthBlurPass: missing source color texnum %u\n", srcColorTexnum);
		return;
	}

	MTL::Texture *srcDepthTex = nullptr;
	if (useDepth) {
		srcDepthTex = GLMetal_GetTexture(srcDepthTexnum);
		if (!srcDepthTex) {
			ri.Printf(PRINT_WARNING, "[Metal] GLMetal_DepthBlurPass: missing source depth texnum %u\n", srcDepthTexnum);
			return;
		}
	}

	MTL::RenderPipelineState *pipeline = GLMetal_GetOrCreateDepthBlurPipeline(horizontal, useDepth);
	if (!pipeline) {
		return;
	}

	const int targetW = (int)s_renderTargetState.colorTexture->width();
	const int targetH = (int)s_renderTargetState.colorTexture->height();
	if (targetW <= 0 || targetH <= 0) {
		return;
	}

	MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
	if (!desc) {
		return;
	}
	MetalRenderTargetClearState &clearState = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);
	desc->colorAttachments()->object(0)->setTexture(s_renderTargetState.colorTexture);
	desc->colorAttachments()->object(0)->setLoadAction(clearState.pendingClearColor ? MTL::LoadActionClear : MTL::LoadActionLoad);
	desc->colorAttachments()->object(0)->setClearColor(clearState.clearColor);
	desc->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);

	// RISK-1: renderCommandEncoder returns autoreleased (+0); retain for cross-call lifetime.
	s_encoder = s_cmdBuffer->renderCommandEncoder(desc);
	if (!s_encoder) {
		return;
	}
	s_encoder->retain();
	GLMetal_OnEncoderOpened();

	MTL::Viewport vp;
	vp.originX = (double)viewportX;
	vp.originY = (double)(targetH - (viewportY + viewportH));
	vp.width = (double)viewportW;
	vp.height = (double)viewportH;
	vp.znear = 0.0;
	vp.zfar = 1.0;
	s_encoder->setViewport(vp);

	int sx0 = viewportX;
	int sx1 = viewportX + viewportW;
	int sy0 = targetH - (viewportY + viewportH);
	int sy1 = targetH - viewportY;
	if (sx0 < 0) sx0 = 0;
	if (sy0 < 0) sy0 = 0;
	if (sx1 > targetW) sx1 = targetW;
	if (sy1 > targetH) sy1 = targetH;

	MTL::ScissorRect sr;
	if (sx1 <= sx0 || sy1 <= sy0) {
		sr.x = 0;
		sr.y = 0;
		sr.width = 0;
		sr.height = 0;
	} else {
		sr.x = (NS::UInteger)sx0;
		sr.y = (NS::UInteger)sy0;
		sr.width = (NS::UInteger)(sx1 - sx0);
		sr.height = (NS::UInteger)(sy1 - sy0);
	}
	s_encoder->setScissorRect(sr);
	s_encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
	s_encoder->setCullMode(MTL::CullModeNone);
	MTL::DepthStencilState *alwaysPass = GLMetal_GetAlwaysPassDepthStencilState();
	if (alwaysPass) {
		s_encoder->setDepthStencilState(alwaysPass);
	}

	MTL::RenderCommandEncoder *enc = s_encoder;
	enc->setRenderPipelineState(pipeline);
	enc->setVertexBytes(uniforms, (NS::UInteger)kDepthBlurUniformBytes, 1);
	enc->setVertexBytes(kDepthBlurPositions, (NS::UInteger)sizeof(kDepthBlurPositions), 2);
	enc->setVertexBytes(kDepthBlurTexcoords, (NS::UInteger)sizeof(kDepthBlurTexcoords), 3);
	enc->setFragmentBytes(uniforms, (NS::UInteger)kDepthBlurUniformBytes, 0);
	enc->setFragmentTexture(srcColorTex, 0); // [[texture(0)]]
	if (useDepth) {
		enc->setFragmentTexture(srcDepthTex, 1); // [[texture(1)]]
	}
	enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)6);

	clearState.pendingClearColor = false;
	clearState.pendingClearDepth = false;
	s_openRenderTargetState = s_renderTargetState;
	s_openRenderTargetValid = true;

	GLMetal_EndEncoderIfNeeded("depthblur-pass");
}

/*
 * GLMetal_GetOrCreateSAOPipeline — lazy-build the SAO compute PSO (built once, cached).
 *
 * GL2 ref: PARITY DEVIATION (ledger E3/B6 — Apple SAO compute replaces GL2 ssao_fp.glsl).
 * This is the first MTL::ComputePipelineState in this file; all prior PSOs are render PSOs.
 *
 * Ownership (metal-cpp):
 *   device->newLibrary / lib->newFunction / device->newComputePipelineState all start with
 *   "new" → return retained objects.  Temporaries (lib, fn) are released after PSO creation.
 *   s_saoPipeline holds the retained compute PSO; released in GLMetal_Shutdown.
 */
static MTL::ComputePipelineState *GLMetal_GetOrCreateSAOPipeline(void)
{
	if (s_saoPipeline) {
		return s_saoPipeline;
	}

	MTL::Device *device = MetalDevice();
	if (!device) {
		return nullptr;
	}

	MTL::Library *lib = GLMetal_LoadShaderLibraryFromSource(GLMetal_GetSAOComputeSource(), "sao-compute");
	if (!lib) {
		ri.Printf(PRINT_WARNING, "[Metal] SAO compute: failed to compile kernel library\n");
		return nullptr;
	}

	NS::String *fnName = NS::String::string("scalableAmbientObscurance", NS::UTF8StringEncoding);
	MTL::Function *fn = lib->newFunction(fnName);
	lib->release();
	if (!fn) {
		ri.Printf(PRINT_WARNING, "[Metal] SAO compute: kernel 'scalableAmbientObscurance' not found\n");
		return nullptr;
	}

	NS::Error *error = nullptr;
	s_saoPipeline = device->newComputePipelineState(fn, &error);
	fn->release();
	if (!s_saoPipeline) {
		ri.Printf(PRINT_WARNING, "[Metal] SAO compute: pipeline creation failed: %s\n", MetalErrorString(error));
		return nullptr;
	}

	return s_saoPipeline;
}

/*
 * GLMetal_DispatchSAO — run the SAO compute pass for the current frame.
 *
 * GL2 ref: PARITY DEVIATION (ledger E3/B6) — replaces tr_backend.c:1067-1128 ssao FBO pass.
 *   ioquake3's backEnd.viewParms.projectionMatrix is GL-convention (clip Z ∈ [-1,1]).
 *   The SAO kernel's NDC→camera-space reconstruction assumes Metal convention (clip Z ∈ [0,1]).
 *
 * PROJECTION CONVENTION (OQ-1 resolution, option a):
 *   We derive the Metal-convention projection by pre-multiplying with the depth-remap matrix:
 *       P_metal = depthRemap * P_gl
 *   where depthRemap = {{1,0,0,0},{0,1,0,0},{0,0,0.5,0},{0,0,0.5,1}} (column-major).
 *   This maps clip Z: z_metal = 0.5*z_gl + 0.5*w, matching the same remap applied to the
 *   render-pipeline MVP via GL_RemapMVPDepthForMetal() in tr_backend.cpp.
 *   glState.projection / backEnd.viewParms.projectionMatrix are NOT Metal-convention —
 *   the remap in GL_RemapMVPDepthForMetal is applied to MVP (projection*modelview) not P alone.
 *   The inverse is computed with simd_inverse (simd/simd.h) on the Metal-convention matrix.
 *
 * ENCODER ORDERING (Dallas SEAM note):
 *   GLMetal_EndEncoderIfNeeded must be called before opening the compute encoder; a render
 *   encoder and a compute encoder cannot be open simultaneously on the same command buffer.
 *
 * DISPATCH STRATEGY:
 *   dispatchThreadgroups with ceil-division is used unconditionally.  This works on all
 *   Metal-capable devices.  Non-uniform dispatchThreads (device->supportsFamily(GPUFamilyApple4))
 *   is more efficient on Apple Silicon but not required for correctness since the kernel guards
 *   against out-of-bounds threads via gid < textureSize check.
 *
 * Parameters:
 *   ssaoTexnum   — texnum of the R8Unorm SSAO output image (tr.screenSsaoImage)
 *   depthTexnum  — texnum of the Depth32Float scene depth image (tr.renderDepthImage)
 *   glProjMat    — 16-float column-major GL-convention projection matrix
 *                  (pass backEnd.viewParms.projectionMatrix)
 */
extern "C" void GLMetal_DispatchSAO(uint32_t ssaoTexnum, uint32_t depthTexnum, const float *glProjMat)
{
	if (!glProjMat) {
		return;
	}

	// (a) End any active render encoder before opening compute encoder.
	// Dallas SEAM rule: never open compute/blit encoder while render encoder is active.
	GLMetal_EndEncoderIfNeeded("sao-compute");

	if (!s_cmdBuffer) {
		return;
	}

	// Fetch the Metal PSO (lazy-built).
	MTL::ComputePipelineState *pso = GLMetal_GetOrCreateSAOPipeline();
	if (!pso) {
		return;
	}

	// Resolve textures.
	MTL::Texture *ssaoTex  = GLMetal_GetTexture(ssaoTexnum);
	MTL::Texture *depthTex = GLMetal_GetTexture(depthTexnum);
	if (!ssaoTex || !depthTex) {
		ri.Printf(PRINT_WARNING, "[Metal] GLMetal_DispatchSAO: missing texture(s) ssao=%u depth=%u\n",
		          ssaoTexnum, depthTexnum);
		return;
	}

	const NS::UInteger depthW = depthTex->width();
	const NS::UInteger depthH = depthTex->height();

	// (d) Fill MetalSAOFrameParams.
	MetalSAOFrameParams frameParams;
	frameParams.invPhysicalSize = simd_make_float2(1.0f / (float)depthW, 1.0f / (float)depthH);
	frameParams.frameCounter    = s_saoFrameCounter++;
	frameParams._pad0           = 0;

	// (d) Fill MetalSAOCameraParams — Metal-convention projection matrix.
	//
	// Derive P_metal = depthRemap * P_gl  (column-major, 4x4).
	// depthRemap only modifies row 2 of the output:
	//   P_metal[col][0] = P_gl[col][0]
	//   P_metal[col][1] = P_gl[col][1]
	//   P_metal[col][2] = 0.5 * P_gl[col][2] + 0.5 * P_gl[col][3]
	//   P_metal[col][3] = P_gl[col][3]
	simd_float4x4 P_metal;
	for (int col = 0; col < 4; col++) {
		P_metal.columns[col][0] = glProjMat[col * 4 + 0];
		P_metal.columns[col][1] = glProjMat[col * 4 + 1];
		P_metal.columns[col][2] = 0.5f * glProjMat[col * 4 + 2] + 0.5f * glProjMat[col * 4 + 3];
		P_metal.columns[col][3] = glProjMat[col * 4 + 3];
	}
	MetalSAOCameraParams camParams;
	camParams.projectionMatrix    = P_metal;
	camParams.invProjectionMatrix = simd_inverse(P_metal);

	// (b) Open compute command encoder.
	MTL::ComputeCommandEncoder *enc = s_cmdBuffer->computeCommandEncoder();
	if (!enc) {
		ri.Printf(PRINT_WARNING, "[Metal] GLMetal_DispatchSAO: failed to create compute encoder\n");
		return;
	}

	// (c) Bind PSO.
	enc->setComputePipelineState(pso);

	// (d) Bind frame and camera params via setBytes (small structs, no separate buffer needed).
	enc->setBytes(&frameParams, sizeof(frameParams), METAL_SAO_BUFFER_FRAME_PARAMS);
	enc->setBytes(&camParams,   sizeof(camParams),   METAL_SAO_BUFFER_CAMERA_PARAMS);

	// (e) Bind textures per tr_ssao.h contract:
	//   texture(0) = ssaoTex  — R8Unorm AO output (write)
	//   texture(1) = depthTex — Depth32Float scene depth (read)
	//   texture(2) = depthTex — same full-res depth (v1: no pyramid; same texture)
	enc->setTexture(ssaoTex,  METAL_SAO_TEXTURE_OUTPUT);
	enc->setTexture(depthTex, METAL_SAO_TEXTURE_DEPTH);
	enc->setTexture(depthTex, METAL_SAO_TEXTURE_DEPTH_MIP);

	// (f) Dispatch. Non-uniform threadgroups (Apple4+ / Mac2) launch exactly one thread per
	// pixel, so edge threadgroups carry no idle lanes; otherwise fall back to ceil-division
	// (the kernel bounds-checks gid against the texture size either way).
	static int s_saoNonUniformDispatch = -1;
	if (s_saoNonUniformDispatch < 0) {
		MTL::Device *device = MetalDevice();
		s_saoNonUniformDispatch = (device && (device->supportsFamily(MTL::GPUFamilyApple4) ||
		                                      device->supportsFamily(MTL::GPUFamilyMac2))) ? 1 : 0;
	}
	const NS::UInteger tgW = 8, tgH = 8;
	const MTL::Size threadsPerGroup = MTL::Size::Make(tgW, tgH, 1);
	if (s_saoNonUniformDispatch) {
		enc->dispatchThreads(MTL::Size::Make(depthW, depthH, 1), threadsPerGroup);
	} else {
		const MTL::Size numGroups = MTL::Size::Make(
		    (depthW + tgW - 1) / tgW,
		    (depthH + tgH - 1) / tgH,
		    1);
		enc->dispatchThreadgroups(numGroups, threadsPerGroup);
	}

	// (g) End compute encoding.
	enc->endEncoding();
}

static MTL::ClearColor s_requestedClearColor = MTL::ClearColor::Make(0.0, 0.0, 0.0, 1.0);

extern "C" void GLMetal_SetClearColor(float r, float g, float b, float a)
{
	s_requestedClearColor = MTL::ClearColor::Make(r, g, b, a);
}

extern "C" void GLMetal_SetRenderTarget(uint32_t colorTexnum, uint32_t depthTexnum, bool clearColor, bool clearDepth)
{
	MetalRenderTargetState desired = s_renderTargetState;

	if (colorTexnum == 0) {
		desired.targetDrawable = true;
		desired.colorTexture = nullptr;
		desired.depthTexture = s_depthTexture;
	} else {
		MTL::Texture *color = GLMetal_GetTexture(colorTexnum);
		if (!color) {
			ri.Printf(PRINT_WARNING, "[Metal] GLMetal_SetRenderTarget: color texnum %u not found\n", colorTexnum);
			return;
		}
		desired.targetDrawable = false;
		desired.colorTexture = color;
		desired.depthTexture = nullptr;
		if (depthTexnum != 0) {
			MTL::Texture *depth = GLMetal_GetTexture(depthTexnum);
			if (!depth) {
				ri.Printf(PRINT_WARNING, "[Metal] GLMetal_SetRenderTarget: depth texnum %u not found\n", depthTexnum);
				return;
			}
			desired.depthTexture = depth;
		}
	}

	const bool targetChanged = !GLMetal_RenderTargetsEqual(desired, s_renderTargetState);
	s_renderTargetState = desired;

	MetalRenderTargetClearState &clearState = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);
	if (clearColor) {
		clearState.pendingClearColor = true;
		clearState.clearColor = s_requestedClearColor;
	}
	if (clearDepth) {
		clearState.pendingClearDepth = true;
	}

	// GL2 ref: tr_backend.c:382 — qglClear fires immediately on the active framebuffer.
	// Metal defers clears to LoadActionClear on the *next* encoder open. If the encoder is
	// already open on this same target (e.g. the 2D menu stretchpics share tr.renderFbo with
	// the 3D banner RenderScene), the pending clear is never applied — the depth buffer retains
	// its Metal zero-initialized value (0.0 = near plane) and all 3D geometry fails the
	// LessEqual depth test → banner completely invisible. Fix: close the encoder when a clear
	// is requested, even if the render target is unchanged, so the next GLMetal_EnsureEncoder
	// opens a fresh pass with MTLLoadActionClear. PARITY-SEAM: GL2 qglClear is immediate;
	// Metal flushes the encoder to honor it — same semantic result, different mechanism.
	const bool clearRequested = clearColor || clearDepth;
	if (s_encoder && (targetChanged || clearRequested) &&
	    (!s_openRenderTargetValid || !GLMetal_RenderTargetsEqual(s_openRenderTargetState, s_renderTargetState) || clearRequested)) {
		GLMetal_EndEncoderIfNeeded("render-target-switch");
	}
}

// GL2 ref: tr_fbo.c:160-163 — depth-only offscreen render target for shadow capture passes.
// Sets up a render pass with only a depth attachment (no color). This is the Metal equivalent
// of GL2 binding a depth-only FBO (pshadow map) + glColorMask(FALSE×4). The absent color
// attachment causes fragment color output to be discarded by the GPU.
// See for-copilot/Renderer/Shaders/AAPLShadow.metal for the standard Apple depth-only pattern.
extern "C" void GLMetal_SetDepthOnlyRenderTarget(uint32_t depthTexnum, bool clearDepth)
{
	MTL::Texture *depth = GLMetal_GetTexture(depthTexnum);
	if (!depth) {
		ri.Printf(PRINT_WARNING, "[Metal] GLMetal_SetDepthOnlyRenderTarget: depth texnum %u not found\n", depthTexnum);
		return;
	}

	MetalRenderTargetState desired;
	desired.targetDrawable = false;
	desired.colorTexture = nullptr;
	desired.depthTexture = depth;

	const bool targetChanged = !GLMetal_RenderTargetsEqual(desired, s_renderTargetState);
	s_renderTargetState = desired;

	MetalRenderTargetClearState &clearState = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);
	if (clearDepth) {
		clearState.pendingClearDepth = true;
	}

	if (s_encoder && targetChanged && (!s_openRenderTargetValid || !GLMetal_RenderTargetsEqual(s_openRenderTargetState, s_renderTargetState))) {
		GLMetal_EndEncoderIfNeeded("depth-only-target-switch");
	}
}

// GL2 ref: tr_backend.c:382 — GL's qglClear executes immediately when an FBO is bound,
// independent of whether any geometry is subsequently drawn. Metal instead defers the
// clear to LoadActionClear, which only fires when the render pass opens (lazily, on the
// first draw via GLMetal_EnsureEncoder). For pshadow depth-capture passes whose occluder
// can rasterize zero fragments in a given frame (e.g. a transient/culled caster), the pass
// would never open, the clear would be skipped, and the depth map would retain STALE
// content from a previous frame's occupant of that map index — producing a phantom
// morphing shadow blob. Forcing the encoder open here honors the pending clear (and stores
// the cleared depth), matching GL2's clear-on-bind semantics.
extern "C" void GLMetal_ForcePendingClears(void)
{
	GLMetal_EnsureEncoder();
}

// GL2 ref: tr_backend.c:1429-1436 (final renderFbo -> system framebuffer present path).
extern "C" void GLMetal_TonemapPass(MTL::Texture *srcTex, float exposure)
{
	if (!srcTex) {
		return;
	}
	if (!s_frameBegun) {
		GLMetal_BeginFrame();
	}
	if (!s_cmdBuffer) {
		return;
	}

	GLMetal_EndEncoderIfNeeded("tonemap-pass");
	GLMetal_SetRenderTarget(0, 0, false, false);
	if (!GLMetal_AcquireDrawableForPresent()) {
		return;
	}

	MTL::RenderPipelineState *pipeline = GLMetal_GetOrCreateTonemapPipeline();
	if (!pipeline) {
		return;
	}
	if (!s_tonemapSampler) {
		s_tonemapSampler = GLMetal_GetOrCreateSampler(GL_CLAMP_TO_EDGE, qtrue, qfalse, qfalse);
	}
	if (!s_tonemapSampler) {
		return;
	}

	MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
	if (!desc) {
		return;
	}
	desc->colorAttachments()->object(0)->setTexture(s_finalTarget);
	desc->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionDontCare);
	desc->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);

	// RISK-1: renderCommandEncoder returns autoreleased (+0); retain for cross-call lifetime.
	s_encoder = s_cmdBuffer->renderCommandEncoder(desc);
	if (!s_encoder) {
		return;
	}
	s_encoder->retain();
	GLMetal_OnEncoderOpened();
	s_frameTouchedDrawable = true;

	const int w = (int)s_finalTarget->width();
	const int h = (int)s_finalTarget->height();
	MTL::Viewport vp;
	vp.originX = 0.0;
	vp.originY = 0.0;
	vp.width = (double)w;
	vp.height = (double)h;
	vp.znear = 0.0;
	vp.zfar = 1.0;
	s_encoder->setViewport(vp);
	MTL::ScissorRect sr = { 0, 0, (NS::UInteger)w, (NS::UInteger)h };
	s_encoder->setScissorRect(sr);
	s_encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
	s_encoder->setCullMode(MTL::CullModeNone);
	MTL::DepthStencilState *alwaysPass = GLMetal_GetAlwaysPassDepthStencilState();
	if (alwaysPass) {
		s_encoder->setDepthStencilState(alwaysPass);
	}

	MTL::RenderCommandEncoder *enc = s_encoder;
	enc->setRenderPipelineState(pipeline);
	enc->setFragmentTexture(srcTex, 0);
	enc->setFragmentSamplerState(s_tonemapSampler, 0);
	enc->setFragmentBytes(&exposure, sizeof(float), 0);
	// UVs come from metalPresentTonemap: it derives uv from clip position and flips
	// uv.y (uv.y = 1.0 - uv.y) so the top-left-origin HDR source samples upright.
	// This pass only binds texture/sampler/exposure and draws the fullscreen triangle.
	enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);

	MetalRenderTargetClearState &clearState = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);
	clearState.pendingClearColor = false;
	clearState.pendingClearDepth = false;
	s_openRenderTargetState = s_renderTargetState;
	s_openRenderTargetValid = true;
}

// GL2 ref: tr_backend.c:1369 RB_DrawGreyscale (greyscale present variant).
extern "C" void GLMetal_GreyscalePass(MTL::Texture *srcTex, float greyscale)
{
	if (!srcTex) {
		return;
	}
	if (!s_frameBegun) {
		GLMetal_BeginFrame();
	}
	if (!s_cmdBuffer) {
		return;
	}

	GLMetal_EndEncoderIfNeeded("greyscale-pass");
	GLMetal_SetRenderTarget(0, 0, false, false);
	if (!GLMetal_AcquireDrawableForPresent()) {
		return;
	}

	MTL::RenderPipelineState *pipeline = GLMetal_GetOrCreateGreyscalePipeline();
	if (!pipeline) {
		return;
	}
	if (!s_tonemapSampler) {
		s_tonemapSampler = GLMetal_GetOrCreateSampler(GL_CLAMP_TO_EDGE, qtrue, qfalse, qfalse);
	}
	if (!s_tonemapSampler) {
		return;
	}

	MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
	if (!desc) {
		return;
	}
	desc->colorAttachments()->object(0)->setTexture(s_finalTarget);
	desc->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionDontCare);
	desc->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);

	// RISK-1: renderCommandEncoder returns autoreleased (+0); retain for cross-call lifetime.
	s_encoder = s_cmdBuffer->renderCommandEncoder(desc);
	if (!s_encoder) {
		return;
	}
	s_encoder->retain();
	GLMetal_OnEncoderOpened();
	s_frameTouchedDrawable = true;

	const int w = (int)s_finalTarget->width();
	const int h = (int)s_finalTarget->height();
	MTL::Viewport vp;
	vp.originX = 0.0;
	vp.originY = 0.0;
	vp.width = (double)w;
	vp.height = (double)h;
	vp.znear = 0.0;
	vp.zfar = 1.0;
	s_encoder->setViewport(vp);
	MTL::ScissorRect sr = { 0, 0, (NS::UInteger)w, (NS::UInteger)h };
	s_encoder->setScissorRect(sr);
	s_encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
	s_encoder->setCullMode(MTL::CullModeNone);
	MTL::DepthStencilState *alwaysPass = GLMetal_GetAlwaysPassDepthStencilState();
	if (alwaysPass) {
		s_encoder->setDepthStencilState(alwaysPass);
	}

	MTL::RenderCommandEncoder *enc = s_encoder;
	enc->setRenderPipelineState(pipeline);
	enc->setFragmentTexture(srcTex, 0);
	enc->setFragmentSamplerState(s_tonemapSampler, 0);
	enc->setFragmentBytes(&greyscale, sizeof(float), 0);
	// UVs come from metalPresentGreyscale: it derives uv from clip position and flips
	// uv.y (uv.y = 1.0 - uv.y) so the top-left-origin source samples upright.
	// This pass only binds texture/sampler/greyscale amount and draws the fullscreen triangle.
	enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);

	MetalRenderTargetClearState &clearState2 = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);
	clearState2.pendingClearColor = false;
	clearState2.pendingClearDepth = false;
	s_openRenderTargetState = s_renderTargetState;
	s_openRenderTargetValid = true;
}

static void GLMetal_RegisterTextureForTexnum(uint32_t texnum, MTL::Texture *texture)
{
if (texnum >= s_textureTable.size()) {
	s_textureTable.resize((size_t)texnum + 1, nullptr);
}
if (s_textureTable[texnum]) {
	s_textureTable[texnum]->release();
}
s_textureTable[texnum] = texture;
s_bindingGeneration++;
}

// ---------------------------------------------------------------------------
// Texture uploads
// ---------------------------------------------------------------------------
// Commit the pending upload command buffer. Must run before any frame command buffer that
// samples the uploaded textures is committed (same queue => executes in commit order).
static void GLMetal_FlushUploads(bool wait)
{
	if (!s_uploadCmdBuffer) {
		return;
	}
	if (s_uploadBlit) {
		s_uploadBlit->endEncoding();
		s_uploadBlit->release();
		s_uploadBlit = nullptr;
	}
	s_uploadCmdBuffer->commit();
	if (wait) {
		s_uploadCmdBuffer->waitUntilCompleted();
	}
	s_uploadCmdBuffer->release();
	s_uploadCmdBuffer = nullptr;
	s_uploadPendingBytes = 0;
}

static MTL::BlitCommandEncoder *GLMetal_GetUploadBlitEncoder(void)
{
	if (s_uploadBlit) {
		return s_uploadBlit;
	}
	if (!s_queue) {
		return nullptr;
	}
	// RISK-1: commandBuffer/blitCommandEncoder return autoreleased (+0); retain both.
	s_uploadCmdBuffer = s_queue->commandBuffer();
	if (!s_uploadCmdBuffer) {
		return nullptr;
	}
	s_uploadCmdBuffer->retain();
	s_uploadBlit = s_uploadCmdBuffer->blitCommandEncoder();
	if (!s_uploadBlit) {
		s_uploadCmdBuffer->release();
		s_uploadCmdBuffer = nullptr;
		return nullptr;
	}
	s_uploadBlit->retain();
	return s_uploadBlit;
}

// Stage `pixels` for one mip level region and record a buffer->texture copy on `blit`.
// Performs the RGBA8 -> float conversion that glTexImage2D would have done.
// Returns the number of source bytes consumed (for walking packed mip chains).
static size_t GLMetal_EncodeTextureRegionUpload(MTL::BlitCommandEncoder *blit, MTL::Texture *texture, int level,
                                                int x, int y, int width, int height,
                                                const byte *pixels, int uploadFormat, bool assumeRGBA8Source)
{
	const MTL::PixelFormat pixelFormat = texture->pixelFormat();
	int bytesPerRow, bytesPerImage;
	MetalTextureUploadLayout(pixelFormat, width, height, &bytesPerRow, &bytesPerImage);
	size_t srcBytes = (size_t)bytesPerImage;

	const byte *uploadPixels = pixels;
	std::vector<byte> convertedBytes;
	const bool needsRGBA8ToFloatConvert =
		!MetalIsBlockCompressedFormat(pixelFormat) &&
		MetalIsFloatColorFormat(pixelFormat) &&
		(assumeRGBA8Source || MetalUploadFormatIsRGBA8Source(uploadFormat));
	if (needsRGBA8ToFloatConvert) {
		// GL2 ref: tr_image.c (glTexImage2D format/type describe SOURCE, internalFormat the DEST).
		// PARITY-SEAM(Metal-device): blit copies do not convert formats; glTexImage2D does.
		srcBytes = (size_t)width * (size_t)height * 4u;
		if (MetalConvertRGBA8SourceForFloatDest(pixels, width, height, pixelFormat, convertedBytes)) {
			uploadPixels = convertedBytes.data();
		}
	}

	if (!blit || !s_device) {
		return srcBytes;
	}
	MTL::Buffer *staging = s_device->newBuffer(uploadPixels, (NS::UInteger)bytesPerImage, MTL::ResourceStorageModeShared);
	if (!staging) {
		ri.Printf(PRINT_WARNING, "[Metal] texture upload: staging buffer allocation failed (%d bytes)\n", bytesPerImage);
		return srcBytes;
	}
	blit->copyFromBuffer(staging, 0, (NS::UInteger)bytesPerRow, 0,
	                     MTL::Size::Make((NS::UInteger)width, (NS::UInteger)height, 1),
	                     texture, 0, (NS::UInteger)level,
	                     MTL::Origin::Make((NS::UInteger)x, (NS::UInteger)y, 0));
	staging->release(); // the command buffer retains it until the copy has executed
	s_uploadPendingBytes += (size_t)bytesPerImage;
	return srcBytes;
}

static void GLMetal_CreateTextureInternal(uint32_t texnum, int width, int height, const void *pixels, qboolean mipmap,
	int wrapClampMode, MTL::PixelFormat pixelFormat, MTL::TextureUsage usage, MTL::StorageMode storageMode,
	const char *label, int uploadFormat, int uploadMipCount)
{
MTL::Device *device = MetalDevice();
if (!device || width <= 0 || height <= 0) {
	ri.Printf(PRINT_WARNING, "[Metal] %s(%u): invalid inputs/device\n", label, texnum);
	return;
}
if (pixelFormat == MTL::PixelFormatInvalid) {
	ri.Printf(PRINT_WARNING, "[Metal] %s(%u): invalid pixel format for GL internal format 0x%X\n", label, texnum, uploadFormat);
	return;
}
if (MetalIsBlockCompressedFormat(pixelFormat) && !device->supportsBCTextureCompression()) {
	// GL2 ref: tr_extensions.c:290 — GL only advertises BC/RGTC/BPTC when supported.
	ri.Printf(PRINT_WARNING, "[Metal] %s(%u): device does not support BC texture compression for GL internal format 0x%X\n", label, texnum, uploadFormat);
	return;
}

int mipLevels = MetalMipLevelCount(width, height, mipmap);
if (MetalIsBlockCompressedFormat(pixelFormat)) {
	// Metal cannot generate BC mipmaps; DDS precomputed levels are uploaded below.
	mipLevels = (uploadMipCount > 1) ? uploadMipCount : 1;
}
(void)wrapClampMode;

MTL::TextureDescriptor *desc = MTL::TextureDescriptor::alloc()->init();
desc->setTextureType(MTL::TextureType2D);
desc->setPixelFormat(pixelFormat);
desc->setWidth(width);
desc->setHeight(height);
desc->setMipmapLevelCount(mipLevels);
desc->setUsage(usage);
desc->setStorageMode(storageMode);

MTL::Texture *texture = device->newTexture(desc);
desc->release();
if (!texture) {
	ri.Printf(PRINT_WARNING, "[Metal] %s(%u): newTexture failed\n", label, texnum);
	return;
}

if (pixels) {
	// Contents are staged and copied by the batched upload command buffer (committed before the
	// next frame), so textures can live in GPU-private memory and mip generation no longer
	// blocks on a per-texture command buffer.
	MTL::BlitCommandEncoder *blit = GLMetal_GetUploadBlitEncoder();
	const byte *mipPixels = (const byte *)pixels;
	int mipWidth = width;
	int mipHeight = height;
	const int levelsToUpload = MetalIsBlockCompressedFormat(pixelFormat) ? mipLevels : 1;
	for (int level = 0; level < levelsToUpload; level++) {
		mipPixels += GLMetal_EncodeTextureRegionUpload(blit, texture, level, 0, 0, mipWidth, mipHeight,
		                                               mipPixels, uploadFormat, false);
		mipWidth = MAX(1, mipWidth >> 1);
		mipHeight = MAX(1, mipHeight >> 1);
	}
	if (blit && !MetalIsBlockCompressedFormat(pixelFormat) && mipmap && mipLevels > 1) {
		blit->generateMipmaps(texture);
	}
	if (s_uploadPendingBytes >= kUploadFlushThreshold) {
		GLMetal_FlushUploads(false);
	}
}

GLMetal_RegisterTextureForTexnum(texnum, texture);
}

extern "C" void GLMetal_CreateTexture(uint32_t texnum, int width, int height, int internalFormat, const void *pixels, qboolean mipmap, int wrapClampMode)
{
const MTL::PixelFormat pixelFormat = GLInternalFormatToMetalPixelFormat(internalFormat);
GLMetal_CreateTextureInternal(texnum, width, height, pixels, mipmap, wrapClampMode, pixelFormat,
	MTL::TextureUsageShaderRead, MTL::StorageModePrivate, "GLMetal_CreateTexture", internalFormat, 1);
}

extern "C" __attribute__((visibility("default"))) void GLMetal_CreateTextureWithFormatAndUsage(uint32_t texnum, int width, int height,
                                                        int internalFormat, const void *pixels, qboolean mipmap, int wrapClampMode,
                                                        int metalPixelFormat, uint32_t metalUsage)
{
const MTL::PixelFormat pixelFormat = (MTL::PixelFormat)metalPixelFormat;
const MTL::TextureUsage usage = (MTL::TextureUsage)metalUsage;
// All textures are GPU-private: initial contents arrive through the upload blit path.
const MTL::StorageMode storageMode = MTL::StorageModePrivate;
GLMetal_CreateTextureInternal(texnum, width, height, pixels, mipmap, wrapClampMode, pixelFormat,
	usage, storageMode, "GLMetal_CreateTextureWithFormatAndUsage", internalFormat, 1);
}

extern "C" __attribute__((visibility("default"))) void GLMetal_CreateTextureWithFormatUsageAndMips(uint32_t texnum, int width, int height,
                                                        int internalFormat, const void *pixels, qboolean mipmap, int wrapClampMode,
                                                        int metalPixelFormat, uint32_t metalUsage, int uploadFormat, int numMips)
{
const MTL::PixelFormat pixelFormat = (MTL::PixelFormat)metalPixelFormat;
const MTL::TextureUsage usage = (MTL::TextureUsage)metalUsage;
// All textures are GPU-private: initial contents arrive through the upload blit path.
const MTL::StorageMode storageMode = MTL::StorageModePrivate;
GLMetal_CreateTextureInternal(texnum, width, height, pixels, mipmap, wrapClampMode, pixelFormat,
	usage, storageMode, "GLMetal_CreateTextureWithFormatUsageAndMips", uploadFormat ? uploadFormat : internalFormat, numMips);
}

extern "C" void GLMetal_UpdateTextureSubImage(uint32_t texnum, int x, int y, int w, int h, const void *pixels)
{
if (!pixels || w <= 0 || h <= 0) {
return;
}

MTL::Texture *texture = GLMetal_GetTexture(texnum);
if (!texture) {
ri.Printf(PRINT_WARNING, "[Metal] GLMetal_UpdateTextureSubImage(%u): texture not found\n", texnum);
return;
}

// GPU-ordered update (the former replaceRegion on a shared texture raced with up to
// s_maxFramesInFlight frames still sampling it). Mid-frame, the copy is encoded into the
// frame command buffer so earlier draws in this frame still see the old contents (GL
// glTexSubImage2D semantics); between frames it joins the batched upload command buffer.
if (s_frameBegun && s_cmdBuffer) {
	GLMetal_EndEncoderIfNeeded("texture-subimage");
	MTL::BlitCommandEncoder *blit = s_cmdBuffer->blitCommandEncoder();
	if (!blit) {
		return;
	}
	blit->retain();
	GLMetal_EncodeTextureRegionUpload(blit, texture, 0, x, y, w, h, (const byte *)pixels, 0, true);
	blit->endEncoding();
	blit->release();
	return;
}

GLMetal_EncodeTextureRegionUpload(GLMetal_GetUploadBlitEncoder(), texture, 0, x, y, w, h, (const byte *)pixels, 0, true);
if (s_uploadPendingBytes >= kUploadFlushThreshold) {
	GLMetal_FlushUploads(false);
}
}

extern "C" void GLMetal_DeleteTexture(uint32_t texnum)
{
if (texnum >= s_textureTable.size() || !s_textureTable[texnum]) {
return;
}
s_textureTable[texnum]->release();
s_textureTable[texnum] = nullptr;
s_bindingGeneration++;
}

extern "C" void GLMetal_SetTextureBinding(int tmu, uint32_t texnum, int wrapClampMode, qboolean filterLinear, qboolean mipmap, qboolean mipLinear)
{
if (tmu < 0 || tmu >= METAL_MAX_TEXTURE_BINDINGS) {
if (!s_textureBindingRangeWarned) {
ri.Printf(PRINT_WARNING, "[Metal] GLMetal_SetTextureBinding: tmu %d out of range [0,%d)\n", tmu, METAL_MAX_TEXTURE_BINDINGS);
s_textureBindingRangeWarned = true;
}
return;
}

MetalTextureBinding &binding = s_textureBindings[tmu];
if (binding.valid && binding.texnum == texnum && binding.wrapClampMode == wrapClampMode &&
    binding.filterLinear == filterLinear && binding.mipmap == mipmap && binding.mipLinear == mipLinear) {
return; // unchanged: keep the resolved texture/sampler
}
binding.texnum = texnum;
binding.wrapClampMode = wrapClampMode;
binding.filterLinear = filterLinear;
binding.mipmap = mipmap;
binding.mipLinear = mipLinear;  // F3: trilinear vs bilinear
binding.valid = qtrue;
binding.resolvedGeneration = 0; // force re-resolve at next bind
}

// Wave 2 reconciliation: read-only accessor so FBO_BlitFromTexture (tr_fbo.cpp) can pick up
// the TB_LEVELSMAP texnum that GL_BindToTMU (tr_backend.cpp) wrote via GLMetal_SetTextureBinding,
// without requiring an explicit FBO_SetLevelsTexnum call from Lambert.
// GL2 ref: tr_postprocess.c:89-91 (GL_BindToTMU before tonemap FBO_Blit auto-reaches u_LevelsMap)
extern "C" uint32_t GLMetal_GetBoundTexnum(int tmu)
{
	if (tmu < 0 || tmu >= METAL_MAX_TEXTURE_BINDINGS)
		return 0;
	return s_textureBindings[tmu].texnum;
}

extern "C" void GLMetal_BindTexturesToCurrentEncoder(void)
{
MTL::RenderCommandEncoder *enc = GLMetal_CurrentRenderEncoder();
if (!enc) {
return;
}

for (int slot = 0; slot < METAL_MAX_TEXTURE_BINDINGS; ++slot) {
MetalTextureBinding &binding = s_textureBindings[slot];
if (slot != 0 && !binding.valid) {
continue;
}

// Resolve texnum/sampler parameters to Metal objects only when the binding or the
// texture/sampler tables changed, not on every draw.
if (binding.resolvedGeneration != s_bindingGeneration) {
MTL::Texture *texture = binding.texnum ? GLMetal_GetTexture(binding.texnum) : nullptr;
if (!texture) {
texture = GLMetal_WhiteTexture();
}
binding.resolvedTexture = texture;
binding.resolvedSampler = GLMetal_GetOrCreateSampler(binding.wrapClampMode, binding.filterLinear, binding.mipmap, binding.mipLinear);
binding.resolvedGeneration = s_bindingGeneration;
}

MTL::Texture *texture = binding.resolvedTexture;
MTL::SamplerState *sampler = binding.resolvedSampler;
if (!texture || !sampler) {
continue;
}

if (s_encState.fragmentTexture[slot] != texture) {
enc->setFragmentTexture(texture, slot);
s_encState.fragmentTexture[slot] = texture;
}
if (s_encState.fragmentSampler[slot] != sampler) {
enc->setFragmentSamplerState(sampler, slot);
s_encState.fragmentSampler[slot] = sampler;
}
}
}

// ============================================================================
// GLMetal_DeviceInit
// ============================================================================
extern "C" void GLMetal_DeviceInit(void *layerPtr, int w, int h)
{
s_device = MTL::CreateSystemDefaultDevice();
if (!s_device) {
ri.Error(ERR_FATAL, "GLMetal_DeviceInit: MTL::CreateSystemDefaultDevice() failed");
return;
}

s_layer = reinterpret_cast<CA::MetalLayer *>(layerPtr);
s_layer->setDevice(s_device);
s_layer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
s_layer->setDrawableSize(CGSizeMake((CGFloat)w, (CGFloat)h));
GLMetal_UpdateDisplaySyncSetting(qtrue);

// RISK-1: newCommandQueue returns +1 owned reference; stored in s_queue, released in Shutdown.
s_queue = s_device->newCommandQueue();
if (!s_queue) {
ri.Error(ERR_FATAL, "GLMetal_DeviceInit: newCommandQueue failed");
return;
}

s_width = w;
s_height = h;
s_stencilStateBits = 0;
s_firstFrame = true;
s_inFlightFramePendingSignal = false;
if (!s_inFlightSemaphore) {
	s_inFlightSemaphore = dispatch_semaphore_create(s_maxFramesInFlight);
}

ri.Printf(PRINT_ALL, "[Metal] device: %s\n", s_device->name()->utf8String());
ri.Printf(PRINT_ALL, "[Metal] drawable size: %dx%d\n", w, h);

GLMetal_RunPipelineSelfTest();

if (s_whiteTexture) {
s_whiteTexture->release();
s_whiteTexture = nullptr;
}
s_whiteTexture = CreateDefaultWhiteTexture(MetalDevice());
if (!s_whiteTexture) {
ri.Printf(PRINT_WARNING, "[Metal] default white texture creation failed\n");
}
}

// ============================================================================
// GLMetal_BeginFrame
// ============================================================================
// GL2 ref: tr_backend.c:337-382 (view-frame begin + clear setup before draw execution).
// Returns qtrue when a new frame was successfully begun; qfalse if the frame was already
// begun or the device is not ready. Callers that need to react to a freshly-begun frame
// (e.g. RB_StretchPic invalidating glState.currentFBO) use the return value; the many
// internal !s_frameBegun guards ignore it and continue to compile unchanged.
extern "C" qboolean GLMetal_BeginFrame(void)
{
if (s_frameBegun || !s_layer || !s_queue) {
return qfalse;
}

GLMetal_FrameStatsBeginFrame();
GLMetal_AcquireInFlightFrameSlot();
GLMetal_UpdateDisplaySyncSetting(qfalse);
// The in-flight semaphore guarantees the frame that last used this arena slot has completed.
s_frameArenaIndex = (s_frameArenaIndex + 1) % (int)(sizeof(s_frameArenas) / sizeof(s_frameArenas[0]));
GLMetal_ResetFrameArena(s_frameArenas[s_frameArenaIndex]);
// Submit textures created since the last frame now so the GPU can start on them early.
GLMetal_FlushUploads(false);
GLMetal_DrainPendingPipelines();
// RISK-1: commandBuffer() returns autoreleased (+0); retain for cross-frame lifetime.
s_cmdBuffer = s_queue->commandBuffer();
if (!s_cmdBuffer) {
ri.Printf(PRINT_WARNING, "[Metal] commandBuffer() returned null — skipping frame\n");
GLMetal_ReleaseInFlightFrameSlot();
return qfalse;
}
s_cmdBuffer->retain();

CGSize layerSize = s_layer->drawableSize();
int frameWidth = (int)layerSize.width;
int frameHeight = (int)layerSize.height;
if (frameWidth <= 0 || frameHeight <= 0) {
	frameWidth = s_width;
	frameHeight = s_height;
}
if (frameWidth > 0 && frameHeight > 0) {
	GLMetal_EnsureDepthTexture(frameWidth, frameHeight);
}
s_renderTargetClearStates.clear();

s_renderTargetState.targetDrawable = true;
s_renderTargetState.colorTexture = nullptr;
s_renderTargetState.depthTexture = s_depthTexture;
MetalRenderTargetClearState &drawableClearState = GLMetal_GetOrCreateRenderTargetClearState(s_renderTargetState);
drawableClearState.pendingClearColor = true;
drawableClearState.clearColor = s_defaultClearColor;
drawableClearState.pendingClearDepth = true;
drawableClearState.clearDepth = s_defaultClearDepth;
s_openRenderTargetValid = false;

s_depthRangeNear = 0.0f;
s_depthRangeFar = 1.0f;
s_viewportX = 0;
s_viewportY = 0;
s_viewportW = frameWidth;
s_viewportH = frameHeight;
s_cullMode = 0;
s_depthStateBits = 0;
s_stencilStateBits = 0;
s_depthStateValid = false;
s_frameTouchedDrawable = false;

s_frameBegun = true;
return qtrue;
}

// ============================================================================
// GLMetal_Present
// ============================================================================
// GL2 ref: tr_backend.c:1448-1493 (RB_SwapBuffers / present lifecycle seam).
extern "C" void GLMetal_Present(void)
{
if (!s_frameBegun) {
GLMetal_BeginFrame();
}

if (!s_cmdBuffer) {
GLMetal_ReleaseInFlightFrameSlot();
s_frameBegun = false;
return;
}

GLMetal_EndEncoderIfNeeded("present");
if (!GLMetal_AcquireDrawableForPresent()) {
	ri.Printf(PRINT_WARNING, "[Metal] present skipped: drawable unavailable\n");
} else if (!s_frameTouchedDrawable) {
	ri.Printf(PRINT_WARNING, "[Metal] present without drawable render pass this frame\n");
}

// Readback capture is on demand: copying the whole drawable every frame is pure overhead
// unless a screenshot/video frame was requested (GLMetal_RequestCapture / GLMetal_ReadPixels).
bool captured = false;
if (s_captureRequestFrames > 0) {
	captured = GLMetal_RecordCaptureBlit();
	s_captureRequestFrames--;
}

// Texture uploads recorded during this frame must execute before it.
GLMetal_FlushUploads(false);

MTL::CommandBuffer *cmdBuffer = s_cmdBuffer;
s_presentSerial++;
if (captured) {
	if (s_captureCmdBuffer) {
		s_captureCmdBuffer->release();
	}
	s_captureCmdBuffer = cmdBuffer;
	s_captureCmdBuffer->retain();
	s_captureSerial = s_presentSerial;
}
// RISK-2: capture semaphore by value in C++ lambda — dispatch_semaphore_t is a C pointer;
// value capture is semantically identical to __block in ObjC. Safe: semaphore is immutable
// after init.
dispatch_semaphore_t inFlightSemaphore = s_inFlightSemaphore; // by value
cmdBuffer->addCompletedHandler([inFlightSemaphore](MTL::CommandBuffer *) {
    if (inFlightSemaphore) {
        dispatch_semaphore_signal(inFlightSemaphore);
    }
});
s_inFlightFramePendingSignal = false;

if (s_drawable) {
	cmdBuffer->presentDrawable(s_drawable);
}
cmdBuffer->commit();

if (s_firstFrame) {
ri.Printf(PRINT_ALL, "[Metal] first frame presented %dx%d\n", s_width, s_height);
s_firstFrame = false;
}

// RISK-1: release retained references after commit.
s_cmdBuffer = nullptr;
cmdBuffer->release();
if (s_drawable) {
	s_drawable->release();
	s_drawable = nullptr;
}
s_finalTarget = nullptr;
if (s_frameStats.lastFrameStart && GLMetal_FrameStatsEnabled()) {
	const uint64_t waited = s_frameStats.slotWaitNs + s_frameStats.drawableWaitNs - s_frameStats.waitNsAtFrameStart;
	s_frameStats.backendNs += GLMetal_NowNs() - s_frameStats.lastFrameStart - waited;
}
s_frameBegun = false;
s_frameTouchedDrawable = false;
}

extern "C" void GLMetal_RequestCapture(void)
{
	// Two presents: the one currently being encoded plus one more, so a caller that keeps
	// reading every frame (video capture) never sees a gap.
	if (s_captureRequestFrames < 2) {
		s_captureRequestFrames = 2;
	}
}

extern "C" qboolean GLMetal_CaptureReady(void)
{
	return (s_captureTexture && s_captureCmdBuffer && s_captureSerial != 0 && s_captureSerial == s_presentSerial)
		? qtrue : qfalse;
}

extern "C" qboolean GLMetal_ReadPixels(int x, int y, int width, int height, void *dest)
{
if (!dest || width <= 0 || height <= 0) {
return qfalse;
}
// Keep capturing while reads continue (video), and arm capture when none is fresh.
GLMetal_RequestCapture();
if (!GLMetal_CaptureReady()) {
return qfalse;
}
// The capture belongs to the last presented frame, which may still be executing.
s_captureCmdBuffer->waitUntilCompleted();
if (s_captureCmdBuffer->status() != MTL::CommandBufferStatusCompleted) {
NS::Error *error = s_captureCmdBuffer->error();
NS::String *desc = error ? error->localizedDescription() : nullptr;
ri.Printf(PRINT_WARNING, "[Metal] readback capture command buffer failed: %s\n", desc ? desc->utf8String() : "no error detail");
return qfalse;
}

if (x < 0 || y < 0 || x + width > s_captureWidth || y + height > s_captureHeight) {
ri.Printf(PRINT_WARNING, "[Metal] GLMetal_ReadPixels rect %d,%d %dx%d outside capture %dx%d\n",
          x, y, width, height, s_captureWidth, s_captureHeight);
return qfalse;
}

const size_t rowBytes = (size_t)width * 4;
std::vector<unsigned char> src(rowBytes * (size_t)height);
MTL::Region region = MTL::Region::Make2D((NS::UInteger)x, (NS::UInteger)y, (NS::UInteger)width, (NS::UInteger)height);
s_captureTexture->getBytes(src.data(), rowBytes, region, 0);

unsigned char *out = (unsigned char *)dest;
const MTL::PixelFormat pixelFormat = s_captureTexture->pixelFormat();
if (pixelFormat == MTL::PixelFormatBGRA8Unorm || pixelFormat == MTL::PixelFormatBGRA8Unorm_sRGB) {
for (int row = 0; row < height; ++row) {
const unsigned char *s = src.data() + (size_t)row * rowBytes;
unsigned char *d = out + (size_t)row * rowBytes;
for (int col = 0; col < width; ++col) {
d[0] = s[2];
d[1] = s[1];
d[2] = s[0];
d[3] = s[3];
s += 4;
d += 4;
}
}
} else {
memcpy(out, src.data(), rowBytes * (size_t)height);
}

return qtrue;
}

extern "C" void GLMetal_GetBackbufferSize(int *width, int *height)
{
if (width) {
*width = s_width;
}
if (height) {
*height = s_height;
}
}

// ============================================================================
// GLMetal_Shutdown
// ============================================================================
extern "C" void GLMetal_Shutdown(void)
{
GLMetal_EndEncoderIfNeeded("shutdown");
if (s_cmdBuffer) {
GLMetal_ReleaseInFlightFrameSlot();
s_cmdBuffer->commit();
s_cmdBuffer->release();
s_cmdBuffer = nullptr;
}
else {
GLMetal_ReleaseInFlightFrameSlot();
}

// Drain the GPU before tearing anything down. Frames presented on earlier ticks
// still have in-flight command buffers whose completion handlers signal
// s_inFlightSemaphore (see GLMetal_Present). Releasing the semaphore, queue, or
// device while those are pending makes libdispatch abort ("semaphore object
// deallocated while in use" -> SIGABRT) and lets the handlers signal freed
// memory. A committed empty command buffer on the same serial queue completes
// only after every prior frame, so waiting on it guarantees the GPU is idle.
GLMetal_FlushUploads(true);
GLMetal_WaitForPendingPipelineCompiles();
if (s_queue) {
MTL::CommandBuffer *drain = s_queue->commandBuffer(); // autoreleased (+0)
if (drain) {
drain->commit();
drain->waitUntilCompleted();
}
}
if (s_drawable) {
s_drawable->release();
s_drawable = nullptr;
}
s_finalTarget = nullptr;
if (s_offscreenTarget) {
s_offscreenTarget->release();
s_offscreenTarget = nullptr;
}
s_noPresentCvar = nullptr;
s_presentHzCvar = nullptr;
s_lastPresentNs = 0;
s_frameStats.cvar = nullptr;
if (s_depthTexture) {
s_depthTexture->release();
s_depthTexture = nullptr;
}
s_depthW = 0;
s_depthH = 0;
if (s_captureTexture) {
s_captureTexture->release();
s_captureTexture = nullptr;
}
s_captureWidth = 0;
s_captureHeight = 0;
s_capturePixelFormat = MTL::PixelFormatInvalid;
if (s_captureCmdBuffer) {
s_captureCmdBuffer->release();
s_captureCmdBuffer = nullptr;
}
s_captureSerial = 0;
s_captureRequestFrames = 0;
s_renderTargetState = { true, nullptr, nullptr };
s_openRenderTargetState = { true, nullptr, nullptr };
s_openRenderTargetValid = false;
s_stencilStateBits = 0;
s_renderTargetClearStates.clear();

for (auto &entry : s_depthStencilCache) {
if (entry.second) {
entry.second->release();
}
}
s_depthStencilCache.clear();
s_lastDepthStencilKey = ~0ull;
s_lastDepthStencilState = nullptr;
memset(&s_encState, 0, sizeof(s_encState));
s_rtFormats.valid = false;
GLMetal_ReleaseFrameArenas();

for (auto &entry : s_pipelineCache) {
if (entry.second) {
entry.second->release();
}
}
s_pipelineCache.clear();
s_failedPipelineCache.clear();
if (s_tonemapPipeline) {
	s_tonemapPipeline->release();
	s_tonemapPipeline = nullptr;
}
if (s_greyscalePipeline) {
	s_greyscalePipeline->release();
	s_greyscalePipeline = nullptr;
}
if (s_shadowmaskPipeline) {
	s_shadowmaskPipeline->release();
	s_shadowmaskPipeline = nullptr;
}
s_shadowmaskPipelineColorFormat = MTL::PixelFormatInvalid;
s_shadowmaskPipelineFilterLevel = INT_MIN;
for (int h = 0; h < 2; ++h) {
	for (int d = 0; d < 2; ++d) {
		if (s_depthBlurPipeline[h][d]) {
			s_depthBlurPipeline[h][d]->release();
			s_depthBlurPipeline[h][d] = nullptr;
		}
	}
}
s_depthBlurPipelineColorFormat = MTL::PixelFormatInvalid;
if (s_shadowvolumeVertFn) {
	s_shadowvolumeVertFn->release();
	s_shadowvolumeVertFn = nullptr;
}
if (s_shadowvolumeFragFn) {
	s_shadowvolumeFragFn->release();
	s_shadowvolumeFragFn = nullptr;
}
if (s_shadowfinishVertFn) {
	s_shadowfinishVertFn->release();
	s_shadowfinishVertFn = nullptr;
}
if (s_shadowfinishFragFn) {
	s_shadowfinishFragFn->release();
	s_shadowfinishFragFn = nullptr;
}
if (s_saoPipeline) {
	s_saoPipeline->release();
	s_saoPipeline = nullptr;
}

for (auto &rec : s_programs) {
ReleaseProgramFns(rec.fns);
}
s_programs.clear();
s_programIds.clear();

ReleaseTextureTable();
ReleaseSamplerCache(); // also nulls non-owning s_tonemapSampler reference
if (s_whiteTexture) {
s_whiteTexture->release();
s_whiteTexture = nullptr;
}

// RISK-1: release owned device and queue references.
if (s_queue) {
s_queue->release();
s_queue = nullptr;
}
if (s_device) {
s_device->release();
s_device = nullptr;
}
if (s_inFlightSemaphore) {
// The GPU drain above guarantees every completion handler has signaled, so the
// semaphore value has returned to its creation count. Reclaim all slots (now
// non-blocking) and restore the count before release: libdispatch aborts if a
// semaphore is released while its value is below the value it was created with.
for (long i = 0; i < s_maxFramesInFlight; ++i) {
dispatch_semaphore_wait(s_inFlightSemaphore, DISPATCH_TIME_FOREVER);
}
for (long i = 0; i < s_maxFramesInFlight; ++i) {
dispatch_semaphore_signal(s_inFlightSemaphore);
}
dispatch_release(s_inFlightSemaphore);
s_inFlightSemaphore = nullptr;
s_inFlightFramePendingSignal = false;
}

s_layer = nullptr;
s_frameBegun = false;
}

MTL::RenderCommandEncoder *GLMetal_CurrentRenderEncoder(void)
{
if (!s_frameBegun) {
GLMetal_BeginFrame();
}
if (!GLMetal_EnsureEncoder()) {
return nullptr;
}
return s_encoder;
}

MTL::Device *GLMetal_Device(void)
{
return MetalDevice();
}

MTL::CommandQueue *GLMetal_Queue(void)
{
return MetalQueue();
}

MTL::PixelFormat GLMetal_ColorPixelFormat(void)
{
return s_colorPixelFormat;
}

MTL::Texture *GLMetal_GetTexture(uint32_t texnum)
{
return (texnum < s_textureTable.size()) ? s_textureTable[texnum] : nullptr;
}

MTL::SamplerState *GLMetal_GetOrCreateSampler(int wrapClampMode, qboolean filterLinear, qboolean mipmap, qboolean mipLinear)
{
const uint32_t key = SamplerCacheKey(wrapClampMode, filterLinear, mipmap, mipLinear);
auto it = s_samplerCache.find(key);
if (it != s_samplerCache.end()) {
return it->second;
}

MTL::Device *device = MetalDevice();
if (!device) {
return nullptr;
}

MTL::SamplerDescriptor *desc = MTL::SamplerDescriptor::alloc()->init();
const MTL::SamplerAddressMode addressMode = GLWrapToMetalAddressMode(wrapClampMode);
desc->setSAddressMode(addressMode);
desc->setTAddressMode(addressMode);
desc->setRAddressMode(addressMode);

if (filterLinear) {
desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
// F3: mipLinear distinguishes trilinear (GL_LINEAR_MIPMAP_LINEAR) from bilinear
//     (GL_LINEAR_MIPMAP_NEAREST). GL2 ref: tr_image.c:2336-2339 (gl_filter_min).
desc->setMipFilter(mipmap ? (mipLinear ? MTL::SamplerMipFilterLinear : MTL::SamplerMipFilterNearest)
                           : MTL::SamplerMipFilterNotMipmapped);
} else {
desc->setMagFilter(MTL::SamplerMinMagFilterNearest);
desc->setMinFilter(MTL::SamplerMinMagFilterNearest);
desc->setMipFilter(mipmap ? (mipLinear ? MTL::SamplerMipFilterLinear : MTL::SamplerMipFilterNearest)
                           : MTL::SamplerMipFilterNotMipmapped);
}

// F1: Anisotropic filtering — Metal always supports it; gate on textureFilterAnisotropic.
// GL2 ref: tr_image.c:2317-2319 (per-texture setMaxAnisotropy via GL_TEXTURE_MAX_ANISOTROPY_EXT).
// GL2 ref: sdl_glimp.c:1031-1043 (textureFilterAnisotropic + maxAnisotropy set from driver cap).
// Apple GPU Metal cap: maxAnisotropy <= 16.  MTLSamplerDescriptor maxAnisotropy >= 1.
{
const NS::UInteger aniso = textureFilterAnisotropic
    ? (NS::UInteger)Com_Clamp(1, 16, maxAnisotropy)
    : 1;
desc->setMaxAnisotropy(aniso);
}

MTL::SamplerState *sampler = device->newSamplerState(desc);
desc->release();
if (!sampler) {
ri.Printf(PRINT_WARNING, "[Metal] GLMetal_GetOrCreateSampler: sampler creation failed\n");
return nullptr;
}

s_samplerCache.emplace(key, sampler);
return sampler;
}

// F5: Sampler-cache flush API — clears and releases all cached MTLSamplerState objects so
// the next bind call lazily rebuilds samplers with updated filter settings.
// Parker calls this from GL_TextureMode when r_textureMode changes at runtime.
// GL2 ref: tr_image.c:112-120 (GL_TextureMode re-applies filter to all textures).
extern "C" void GLMetal_FlushSamplerCache(void)
{
ReleaseSamplerCache();
}

MTL::Texture *GLMetal_WhiteTexture(void)
{
return s_whiteTexture;
}

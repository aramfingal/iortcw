// tr_metal_device.h — Metal device context + frame lifecycle (Phase 5 bootstrap)
// NOT a 1:1 parity file — documented Metal bootstrap module.
#ifndef TR_METAL_DEVICE_H
#define TR_METAL_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "../qcommon/q_shared.h"

typedef struct MetalShadowmaskUniforms_s MetalShadowmaskUniforms;
typedef struct MetalDepthBlurUniforms_s MetalDepthBlurUniforms;

#ifdef __cplusplus
extern "C" {
#endif

// Static GPU buffers (model vertex/index data). The returned handle is an MTL::Buffer*.
void *GLMetal_CreateStaticBuffer(const void *data, size_t size);
void GLMetal_DestroyStaticBuffer(void *buffer);

// Device lifecycle — layerPtr is a CAMetalLayer* (void* for C/C++ compatibility)
void GLMetal_DeviceInit(void *layerPtr, int w, int h);
void GLMetal_Shutdown(void);

// Frame lifecycle
qboolean GLMetal_BeginFrame(void);
void GLMetal_Present(void);

// Reads a rectangle from the most recently completed/presented color capture into
// RGBA8 bytes. Row 0 in dest is the TOP row of the Metal texture (top-down), not
// GL's bottom-up glReadPixels order. Returns qfalse until one frame capture has
// completed. The capture is one presented frame behind any currently encoding frame.
qboolean GLMetal_ReadPixels(int x, int y, int width, int height, void *dest);
// Readback capture is recorded only on demand (it costs a full-drawable copy per frame).
// GLMetal_RequestCapture arms capture for the next presented frames; GLMetal_CaptureReady
// reports whether the most recently presented frame was captured (so ReadPixels will succeed).
// GLMetal_ReadPixels arms capture itself when it has to fail for lack of a fresh capture.
void GLMetal_RequestCapture(void);
qboolean GLMetal_CaptureReady(void);
void GLMetal_GetBackbufferSize(int *width, int *height);
uint64_t GLMetal_VertexDescHashForAttribs(uint32_t attribs);

// Texture lifecycle (texnum-backed side table)
void GLMetal_CreateTexture(uint32_t texnum, int width, int height, int internalFormat, const void *pixels, qboolean mipmap, int wrapClampMode);
__attribute__((visibility("default"))) void GLMetal_CreateTextureWithFormatAndUsage(uint32_t texnum, int width, int height,
    int internalFormat, const void *pixels, qboolean mipmap, int wrapClampMode,
    int metalPixelFormat, uint32_t metalUsage);
__attribute__((visibility("default"))) void GLMetal_CreateTextureWithFormatUsageAndMips(uint32_t texnum, int width, int height,
    int internalFormat, const void *pixels, qboolean mipmap, int wrapClampMode,
    int metalPixelFormat, uint32_t metalUsage, int uploadFormat, int numMips);
void GLMetal_UpdateTextureSubImage(uint32_t texnum, int x, int y, int w, int h, const void *pixels);
void GLMetal_DeleteTexture(uint32_t texnum);

// Draw-time texture binding table
void GLMetal_SetTextureBinding(int tmu, uint32_t texnum, int wrapClampMode, qboolean filterLinear, qboolean mipmap, qboolean mipLinear = qfalse);
// F5: Flush the sampler cache so stale MTLSamplerState objects are dropped and rebuilt
// on next bind with updated filter settings. Parker calls from GL_TextureMode on r_textureMode change.
// GL2 ref: tr_image.c:112-120 (GL_TextureMode re-applies filter to all textures).
void GLMetal_FlushSamplerCache(void);
// Wave 2 reconciliation: read-only TMU texnum query so FBO_BlitFromTexture can read
// s_textureBindings[TB_LEVELSMAP] set by GL_BindToTMU without a separate setter call.
// GL2 ref: tr_postprocess.c:89-91
uint32_t GLMetal_GetBoundTexnum(int tmu);
void GLMetal_BindTexturesToCurrentEncoder(void);
void GLMetal_SetDepthState(uint32_t glStateBits);
void GLMetal_SetStencilState(qboolean enabled, uint32_t glCompareFunc, uint32_t glPassOp, uint32_t glDepthFailOp, uint32_t glFailOp, uint8_t referenceValue);
void GLMetal_SetDepthRange(float znear, float zfar);
// GL2 ref: tr_shade.c:1698-1701 qglEnable/Disable(GL_POLYGON_OFFSET_FILL); tr_init.c:1061 qglPolygonOffset(-1,-2)
// depthBias = GL units (constant offset), slopeScale = GL factor (slope-scale), clamp = 0.0f (no GL equiv).
void GLMetal_SetDepthBias(float depthBias, float slopeScale, float clamp);
void GLMetal_SetDepthClamp(qboolean enable);
void GLMetal_SetViewport(int x, int y, int w, int h);
// cullMode: 0 = none (two-sided), 1 = cull front faces, 2 = cull back faces
void GLMetal_SetCullMode(int cullMode);
void GLMetal_SetRenderTarget(uint32_t colorTexnum, uint32_t depthTexnum, bool clearColor, bool clearDepth);
// qglClearColor: the colour the next GLMetal_SetRenderTarget( ..., clearColor = true, ... )
// clears to. RTCW clears to the fog colour (RB_BeginDrawingView); the default is black.
void GLMetal_SetClearColor(float r, float g, float b, float a);
// GL2 ref: tr_fbo.c:160-163 — depth-only offscreen render target for shadow capture passes.
// Unlike GLMetal_SetRenderTarget (colorTexnum=0 → screen drawable), this targets an offscreen
// depth texture with no color attachment — matching GL2's depth-only FBO bind for pshadow maps.
void GLMetal_SetDepthOnlyRenderTarget(uint32_t depthTexnum, bool clearDepth);
// GL2 ref: tr_backend.c:382 (qglClear applies immediately on FBO bind, before any draw).
// Metal defers LoadActionClear until the render pass opens (first draw). For depth-only
// pshadow capture passes whose occluder may rasterize zero fragments, this forces the
// pending depth clear to be honored NOW, preventing a stale depth map (phantom shadow).
void GLMetal_ForcePendingClears(void);
qboolean GLMetal_FastCopyTexture(uint32_t srcTexnum, int srcX, int srcY, int srcW, int srcH,
                                 uint32_t dstTexnum, int dstX, int dstY, int copyW, int copyH);
qboolean GLMetal_RenderBlitTexture(uint32_t srcTexnum, const float srcUV[8],
                                   uint32_t dstColorTexnum, int dstX, int dstY, int dstW, int dstH,
                                   int dstFullW, int dstFullH, int filter, int stateBits,
                                   const float color[4], const char *programName,
                                   const void *postprocessUniforms, // PARITY-SEAM(Metal-device): optional 112-byte postprocess block.
                                   bool fullCoverage = false);       // FIX-1: pass true for full-coverage overwrites (DontCare load).
qboolean GLMetal_RenderBlitTextureMulti(uint32_t srcTexnum, const float srcUV[8],
                                       uint32_t dstColorTexnum, int dstX, int dstY, int dstW, int dstH,
                                       int dstFullW, int dstFullH, int filter, int stateBits,
                                       const float color[4], const char *programName,
                                       const void *postprocessUniforms,
                                       const uint32_t *extraTexnums, int extraTexCount, // PARITY-SEAM(Metal-device): TB_COLORMAP + additional TMUs for postprocess fragment textures.
                                       bool fullCoverage = false);                       // FIX-1: pass true for full-coverage overwrites (DontCare load).
void GLMetal_DrawShadowVolume(const float *positions, int numVerts, const float *mvp16, int glCullType);
// Both stencil shadow-volume passes (GL2 tr_shadows.c:212-237) from a single vertex upload:
// pass 1 culls back faces with stencil INCR, pass 2 culls front faces with stencil DECR.
void GLMetal_DrawShadowVolumeStencilPasses(const float *positions, int numVerts, const float *mvp16);
void GLMetal_DrawShadowFinish(const float *rgba4);
void GLMetal_ShadowmaskPass(uint32_t dstColorTexnum, int viewportX, int viewportY, int viewportW, int viewportH,
                           uint32_t screenDepthTexnum, const uint32_t cascadeTexnum[4], qboolean useCascades,
                           const MetalShadowmaskUniforms *uniforms);
void GLMetal_DepthBlurPass(uint32_t dstColorTexnum, int viewportX, int viewportY, int viewportW, int viewportH,
                           uint32_t srcColorTexnum, uint32_t srcDepthTexnum, qboolean horizontal, qboolean useDepth,
                           const MetalDepthBlurUniforms *uniforms);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <Metal/Metal.hpp>

struct vao_s;

// Shader/pipeline infrastructure
MTL::Library *GLMetal_LoadShaderLibraryFromSource(const char *mslSource, const char *label);
MTL::Library *GLMetal_LoadShaderLibraryFromSourceWithDefines(const char *mslSource, const char *label, const char *defines);
MTL::VertexDescriptor *GLMetal_BuildVertexDescriptorFromVao(const struct vao_s *vao, uint64_t *outHash);
MTL::RenderPipelineState *GLMetal_GetOrCreatePipeline(const char *programName,
                                                       MTL::Function *vertexFunction,
                                                       MTL::Function *fragmentFunction,
                                                       const MTL::VertexDescriptor *vertexDesc,
                                                       uint64_t vertexDescHash,
                                                       uint32_t stateBits,
                                                       MTL::ColorWriteMask colorWriteMask = MTL::ColorWriteMaskAll);

// Same as GLMetal_GetOrCreatePipeline, but for explicit attachment formats instead of the
// currently bound render target (used to pre-build PSOs for targets that are not bound yet).
MTL::RenderPipelineState *GLMetal_GetOrCreatePipelineWithFormats(const char *programName,
                                                                  MTL::Function *vertexFunction,
                                                                  MTL::Function *fragmentFunction,
                                                                  const MTL::VertexDescriptor *vertexDesc,
                                                                  uint64_t vertexDescHash,
                                                                  uint32_t stateBits,
                                                                  MTL::ColorWriteMask colorWriteMask,
                                                                  MTL::PixelFormat colorFormat,
                                                                  MTL::PixelFormat depthStencilFormat);

// Register a program's pipeline ingredients so PSOs can be built per GL-state at draw time.
// Retains vertexFunction, fragmentFunction, and vertexDesc internally (caller may release its own refs after).
// Safe to call again for the same name (replace/retain-release old).
// Returns a stable program id (>= 0) for GLMetal_GetPipelineForProgram, or -1 on failure.
int GLMetal_RegisterProgramPipeline(const char *programName,
                                    MTL::Function *vertexFunction,
                                    MTL::Function *fragmentFunction,
                                    const MTL::VertexDescriptor *vertexDesc,
                                    uint64_t vertexDescHash);

// Returns (creating + caching via GLMetal_GetOrCreatePipeline if needed) the PSO for
// programName + vertexDescHash under the given GL state bits. Returns nullptr if the program was never registered.
MTL::RenderPipelineState *GLMetal_GetPipelineForState(const char *programName, uint64_t vertexDescHash, uint32_t stateBits);

// Draw-time fast path: PSO for a registered program id under the given GL state bits and the
// currently bound render target. Uses a small per-program cache (no string hashing).
MTL::RenderPipelineState *GLMetal_GetPipelineForProgram(int programId, uint32_t stateBits);

// Queue asynchronous PSO compiles for a registered program against explicit attachment formats
// (typically the scene render target). Results land in the pipeline cache when they complete;
// a draw that needs one earlier simply builds it synchronously.
void GLMetal_PrewarmProgramPipelines(int programId, const uint32_t *stateBits, int count,
                                     MTL::PixelFormat colorFormat, MTL::PixelFormat depthStencilFormat);

// Compile two MSL sources concurrently (vertex + fragment libraries of one permutation).
// Returns true only when both compile; on failure no library is returned.
bool GLMetal_LoadShaderLibraryPair(const char *mslSourceA, const char *mslSourceB, const char *label,
                                   MTL::Library **outA, MTL::Library **outB);

// Asynchronous form of the above, so many permutations can compile at once: Begin issues
// the compile(s) and returns immediately (fragment source may be NULL); Finish waits,
// reports errors, hands back the libraries (+1 retained) and frees the job. Every job
// must be finished exactly once. Returns true only when all requested libraries compiled.
struct MetalShaderCompileJob;
MetalShaderCompileJob *GLMetal_BeginShaderLibraryCompile(const char *mslVert, const char *mslFrag);
bool GLMetal_FinishShaderLibraryCompile(MetalShaderCompileJob *job, const char *label,
                                        MTL::Library **outVert, MTL::Library **outFrag);

// Per-frame transient GPU memory (triple-buffered with the in-flight frame limit).
// Returns a CPU pointer to `size` bytes that stay valid until the frame is presented, plus the
// backing buffer/offset to bind. Returns nullptr if no frame could be begun.
void *GLMetal_FrameAlloc(size_t size, size_t alignment, MTL::Buffer **outBuffer, NS::UInteger *outOffset);

// Redundant-state-filtered binds on the current render encoder.
void GLMetal_SetRenderPipelineState(MTL::RenderPipelineState *pso);
void GLMetal_SetVertexBuffer(MTL::Buffer *buffer, NS::UInteger offset, NS::UInteger slot);
void GLMetal_SetFragmentBuffer(MTL::Buffer *buffer, NS::UInteger offset, NS::UInteger slot);
// Copies `bytes` into frame memory and binds the same range at both stages
// (vertex slot vertexSlot, fragment slot fragmentSlot). Uniform blocks shared by both stages.
void GLMetal_SetSharedUniforms(const void *bytes, size_t size, NS::UInteger vertexSlot, NS::UInteger fragmentSlot);

// Draw-time accessors
MTL::RenderCommandEncoder *GLMetal_CurrentRenderEncoder(void);
MTL::Device *GLMetal_Device(void);
MTL::CommandQueue *GLMetal_Queue(void);
MTL::PixelFormat GLMetal_ColorPixelFormat(void);
MTL::Texture *GLMetal_GetTexture(uint32_t texnum);
// F1+F3: mipLinear distinguishes trilinear (GL_LINEAR_MIPMAP_LINEAR) from bilinear (GL_LINEAR_MIPMAP_NEAREST).
// setMaxAnisotropy is applied when textureFilterAnisotropic global is true.
// GL2 ref: tr_image.c:2317-2319, sdl_glimp.c:1031-1043.
MTL::SamplerState *GLMetal_GetOrCreateSampler(int wrapClampMode, qboolean filterLinear, qboolean mipmap, qboolean mipLinear);
MTL::Texture *GLMetal_WhiteTexture(void);
extern "C" void GLMetal_TonemapPass(MTL::Texture *srcTex, float exposure);
extern "C" void GLMetal_GreyscalePass(MTL::Texture *srcTex, float greyscale);
/*
 * GLMetal_DispatchSAO — SAO compute pass host entry-point (T4b).
 * GL2 ref: PARITY DEVIATION (ledger E3/B6 — Apple SAO compute).
 *
 * ssaoTexnum  : texnum of the R8Unorm SSAO output (tr.screenSsaoImage->texnum).
 * depthTexnum : texnum of the Depth32Float scene depth (tr.renderDepthImage->texnum).
 * glProjMat   : 16-float column-major GL-convention projection matrix
 *               (pass backEnd.viewParms.projectionMatrix).
 *               The device layer derives the Metal-convention version internally.
 *
 * Caller must NOT have an active render encoder; this function calls
 * GLMetal_EndEncoderIfNeeded before opening the compute encoder.
 */
extern "C" void GLMetal_DispatchSAO(uint32_t ssaoTexnum, uint32_t depthTexnum, const float *glProjMat);
#endif

#endif // TR_METAL_DEVICE_H

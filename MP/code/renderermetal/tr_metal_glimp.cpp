// tr_metal_glimp.cpp — GLimp-equivalent for Metal (Phase 5 bootstrap)
// NOT a 1:1 parity file — documented Metal bootstrap module.
// Mirrors the NON-GL parts of code/sdl/sdl_glimp.c: SDL window creation,
// mode/resolution via R_GetModeInfo, glConfig population, ri.IN_Init handoff.

#include <SDL.h>
#include <SDL_metal.h>

extern "C" {
#include "../qcommon/q_shared.h"
#include "../renderer/tr_public.h"
}

// tr_local.h needs to be in extern "C" for C linkage
extern "C" {
#include "tr_local.h"
}

#include "tr_metal_device.h"

// ============================================================================
// File-static globals (mirrors sdl_glimp.c's SDL_window)
// ============================================================================
static SDL_Window*   s_window    = NULL;
static SDL_MetalView s_metalView = NULL;

// Cvars used by glimp (r_mode/r_fullscreen/r_noborder/r_customwidth/r_customheight
// are registered globally in tr_init.cpp; r_allowResize/r_centerWindow are local to glimp)
extern cvar_t *r_mode;
extern cvar_t *r_fullscreen;
extern cvar_t *r_noborder;
extern cvar_t *r_customwidth;
extern cvar_t *r_customheight;

static cvar_t *r_allowResize;
static cvar_t *r_centerWindow;
// r_metalHidden 1: for automated runs on a Mac that is in use. The window is never shown,
// the application is not activated and frames are rendered off-screen without presenting
// (screenshots, video capture and timedemo still work).
static cvar_t *r_metalHidden;

#define R_MODE_FALLBACK 3  // 640x480 — same as sdl_glimp.c

// ============================================================================
// GLimp_Init — mirrors sdl_glimp.c GLimp_Init (non-GL parts)
// ============================================================================
void GLimp_Init(qboolean fixedFunction)
{
    (void)fixedFunction;

    ri.Printf(PRINT_ALL, "[Metal] GLimp_Init\n");

    // Register glimp-local cvars (mirror sdl_glimp.c:1082-1085)
    r_allowResize  = ri.Cvar_Get("r_allowResize", "0", CVAR_ARCHIVE | CVAR_LATCH);
    r_centerWindow = ri.Cvar_Get("r_centerWindow", "0", CVAR_ARCHIVE | CVAR_LATCH);
    r_metalHidden  = ri.Cvar_Get("r_metalHidden", "0", CVAR_LATCH);

    // Abnormal-exit safety (mirror sdl_glimp.c:1088-1094)
    if (ri.Cvar_VariableIntegerValue("com_abnormalExit")) {
        ri.Cvar_Set("r_mode", va("%d", R_MODE_FALLBACK));
        ri.Cvar_Set("r_fullscreen", "0");
        ri.Cvar_Set("r_centerWindow", "0");
        ri.Cvar_Set("com_abnormalExit", "0");
    }

    // Platform-specific pre-init (mirror sdl_glimp.c:1096)
    ri.Sys_GLimpInit();

    // SDL video subsystem
    if (r_metalHidden->integer) {
        // read when SDL registers the application: no activation, no Dock icon
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    }
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
            ri.Error(ERR_FATAL, "[Metal] SDL_InitSubSystem(VIDEO) failed: %s", SDL_GetError());
            return;
        }
    }

    // Determine resolution (mirror sdl_glimp.c:459-484)
    int mode = r_mode->integer;
    if (mode == -2) {
        SDL_DisplayMode desktopMode;
        if (SDL_GetDesktopDisplayMode(0, &desktopMode) == 0 && desktopMode.h > 0) {
            glConfig.vidWidth  = desktopMode.w;
            glConfig.vidHeight = desktopMode.h;
        } else {
            glConfig.vidWidth  = 640;
            glConfig.vidHeight = 480;
        }
        glConfig.windowAspect = (float)glConfig.vidWidth / (float)glConfig.vidHeight;
    } else if (!R_GetModeInfo(&glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode)) {
        ri.Printf(PRINT_ALL, "[Metal] invalid r_mode %d, falling back to %d\n", mode, R_MODE_FALLBACK);
        if (!R_GetModeInfo(&glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, R_MODE_FALLBACK)) {
            glConfig.vidWidth  = 640;
            glConfig.vidHeight = 480;
            glConfig.windowAspect = 640.0f / 480.0f;
        }
    }

    ri.Printf(PRINT_ALL, "[Metal] mode %d: %dx%d\n", mode, glConfig.vidWidth, glConfig.vidHeight);

    // Window flags (mirror sdl_glimp.c:409-520)
    Uint32 flags = SDL_WINDOW_METAL | (r_metalHidden->integer ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);

    if (r_allowResize && r_allowResize->integer)
        flags |= SDL_WINDOW_RESIZABLE;

    if (r_fullscreen && r_fullscreen->integer) {
        flags |= SDL_WINDOW_FULLSCREEN;
        glConfig.isFullscreen = qtrue;
    } else {
        glConfig.isFullscreen = qfalse;
        if (r_noborder && r_noborder->integer)
            flags |= SDL_WINDOW_BORDERLESS;
    }

    // Window position
    int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;
    if (r_centerWindow && r_centerWindow->integer && !glConfig.isFullscreen) {
        SDL_DisplayMode desktopMode;
        if (SDL_GetDesktopDisplayMode(0, &desktopMode) == 0) {
            x = (desktopMode.w / 2) - (glConfig.vidWidth / 2);
            y = (desktopMode.h / 2) - (glConfig.vidHeight / 2);
        }
    }

    // Create window
    s_window = SDL_CreateWindow(CLIENT_WINDOW_TITLE, x, y,
                                glConfig.vidWidth, glConfig.vidHeight, flags);
    if (!s_window) {
        ri.Error(ERR_FATAL, "[Metal] SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }

    // Create Metal view + get CAMetalLayer
    s_metalView = SDL_Metal_CreateView(s_window);
    if (!s_metalView) {
        ri.Error(ERR_FATAL, "[Metal] SDL_Metal_CreateView failed: %s", SDL_GetError());
        return;
    }

    void *rawLayer = SDL_Metal_GetLayer(s_metalView);
    if (!rawLayer) {
        ri.Error(ERR_FATAL, "[Metal] SDL_Metal_GetLayer returned NULL");
        return;
    }

    // Initialize Metal device context (rawLayer is a CAMetalLayer*)
    GLMetal_DeviceInit(rawLayer, glConfig.vidWidth, glConfig.vidHeight);

    // Populate glConfig (mirror sdl_glimp.c:1123-1165)
    glConfig.driverType    = GLDRV_ICD;
    glConfig.hardwareType  = GLHW_GENERIC;
    glConfig.colorBits     = 32;
    glConfig.depthBits     = 24;
    glConfig.stencilBits   = 8;
    glConfig.deviceSupportsGamma = qfalse;
    glConfig.textureCompression  = TC_NONE;

    // Renderer info strings — use Metal device name
    Q_strncpyz(glConfig.vendor_string, "Apple", sizeof(glConfig.vendor_string));
    Q_strncpyz(glConfig.renderer_string, "Metal", sizeof(glConfig.renderer_string));
    Q_strncpyz(glConfig.version_string, "Metal 3.0", sizeof(glConfig.version_string));
    glConfig.extensions_string[0] = '\0';

    ri.Cvar_Get("r_availableModes", "", CVAR_ROM);

    // Input initialization — CRITICAL (mirror sdl_glimp.c:1174)
    ri.IN_Init(s_window);

    ri.Printf(PRINT_ALL, "[Metal] GLimp_Init success: %dx%d\n", glConfig.vidWidth, glConfig.vidHeight);
}

// ============================================================================
// GLimp_Shutdown — mirrors sdl_glimp.c GLimp_Shutdown
// ============================================================================
void GLimp_Shutdown(void)
{
    ri.Printf(PRINT_ALL, "[Metal] GLimp_Shutdown\n");

    GLMetal_Shutdown();

    ri.IN_Shutdown();

    if (s_metalView) {
        SDL_Metal_DestroyView(s_metalView);
        s_metalView = NULL;
    }

    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// ============================================================================
// GLimp_EndFrame — presents the current frame (Metal commit + present)
// ============================================================================
void GLimp_EndFrame(void)
{
    GLMetal_Present();
}

// ============================================================================
// GLimp_Minimize — mirrors sdl_glimp.c
// ============================================================================
void GLimp_Minimize(void)
{
    if (s_window)
        SDL_MinimizeWindow(s_window);
}

// ============================================================================
// GLimp_SetGamma — no-op (Metal doesn't use hardware gamma ramps)
// ============================================================================
void GLimp_SetGamma(unsigned char red[256], unsigned char green[256], unsigned char blue[256])
{
    (void)red; (void)green; (void)blue;
}

// ============================================================================
// GLimp_LogComment — no-op (GL debug log)
// ============================================================================
void GLimp_LogComment(char *comment)
{
    (void)comment;
}

# Metal renderer

A renderer for macOS that draws with Metal instead of OpenGL. It is the Metal
renderer of aramfingal/ioq3 (`code/renderermetal`, a port of the ioquake3 OpenGL2
renderer) with the RTCW changes of rend2 merged in, so it reads the same cvars and
shader scripts as rend2. It exists in both trees: `SP/code/renderermetal` and
`MP/code/renderermetal`.


## Using it

    +set cl_renderer metal

or `cl_renderer metal` followed by `vid_restart` in the console. The library is
`renderer_sp_metal_arm64.dylib` / `renderer_mp_metal_arm64.dylib`, next to the
other renderers. `cl_renderer opengl1` and `cl_renderer rend2` switch back.

Cvars that only this renderer has:

  - `r_metalFrameStats N` - every N frames, print where the frame time went.
  - `r_metalNoPresent 1` - render off screen and never present (benchmarks).
  - `r_metalPresentHz` - with `r_swapInterval 0`, how often to present: -1 (the
    default) once per display refresh, 0 every frame, N at most N times a second.
  - `r_metalHidden 1` - never show the window (automated runs; latched).


## Building it

`make` builds it by default on macOS arm64. `BUILD_RENDERER_METAL=0` leaves it
out. It needs a C++17 compiler and the macOS 13 SDK or later; the metal-cpp
headers it uses are in `code/metal-cpp`. `make-macosx-app.sh` puts the library in
the application bundle when it was built.


## What is different from rend2

  - No ATI TruForm and no NVidia fog distance modes: both are OpenGL extensions.
  - Flares are not tested against the depth buffer. Coronas still follow the
    visibility the game reports for them.
  - Multisampling, sun rays, cube mapping and the shadow blur are switched off
    when the renderer starts (`r_ext_framebuffer_multisample`, `r_drawSunRays`,
    `r_cubeMapping`, `r_shadowBlur`), and `r_ssao` uses a compute kernel instead
    of rend2's fragment shader.
  - The bone and mesh debug drawing of `r_bonesDebug` is not implemented (it is
    not in rend2 either).


## Working on it

  - The shaders are MSL in C strings in `tr_metal_shadersource.cpp`, compiled when
    the renderer starts. A shader that fails to compile does not fail the build:
    the console says `MSL compile failed` and that shader draws nothing.
  - Each uniform block is declared twice, in `tr_shade.cpp` (`Metal*Uniforms`) and
    in the MSL, and the two must match byte for byte.
  - The MSL spells out the values of `DGEN_*`, `TCGEN_*`, `CGEN_*` and `AGEN_*`.
    `tr_glsl.cpp` has static asserts that stop the build when they no longer
    match `tr_local.h`.
  - `tr_metal_prefix.h` is included ahead of every C++ file (`-include`): it
    removes the `min` and `max` macros of `q_shared.h`.
  - The single and multi player copies differ only where rend2 differs between
    the two trees. A change to one usually belongs in the other.

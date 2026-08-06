// Jet renderer configuration for the Meshpunk T-Deck ELF module.
//
// Jet headers resolve #include "JetConfig.hpp" against the include path, so
// this file replaces jet-src/JetConfig.example.hpp for this frontend.
//
// The module rasterises into a band of internal SRAM and pushes each finished
// band with host_blit_rect(), so there is no full-screen framebuffer. Options
// that require a whole-frame buffer (PostFX, checkerboard reconstruction) are
// therefore off and cannot be enabled without changing main_tdeck.cpp.

#ifndef JET_CONFIG_HPP
#define JET_CONFIG_HPP

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// World-space scale
// ---------------------------------------------------------------------------

// Applied to authored coordinates by argWorld() in engine/jet_lua.cpp.
//
// Jet's guidance is 4 for low-resolution targets, on the grounds that the
// sub-pixel edge precision from 8 is not resolvable at this size. 8 is used
// here for a second reason the guidance does not cover: Primitives derive
// vertex NORMALS from the integer vertex positions, so mesh scale sets normal
// precision. Measured on a radius-70 sphere, unscaled normals ranged 999..1024
// against an expected 1024 (worst-case diffuse error 9/255, about one 5-bit
// colour step); at 8x that falls to 2/255.
//
// Upper bound: createSphere evaluates `radius * sin * cos` in int32 before
// dividing down, overflowing past a scaled radius of ~2047, so an authored
// sphere radius must stay under about 255 units at this scale.
#define JET32_WORLD_SCALE 8

// ---------------------------------------------------------------------------
// Core rasterizer options
// ---------------------------------------------------------------------------

// The dirty-tile bitmap targets drivers that upload whole tiles. This module
// uploads bands, so the bitmap would be built and never read.
#define RENDER_TILE_BUFFER 0
#define TILE_WIDTH  32
#define TILE_HEIGHT 32

// One Z per triangle instead of per pixel. With Z_BUFFERING off this is the
// depth value SORT_TRIANGLES orders by.
#define FAST_Z 1
#define LAZY_Z 0

#define SCREEN_DOOR_ALPHA 1
#define SKIP_ZERO_AREA_TRIANGLES 1
#define NOISE_ALPHA 0

// No depth buffer. A per-pixel Z buffer at 320x240 is 150KB, which would not
// fit the internal-SRAM band and would force the whole buffer into PSRAM.
// Depth order comes from the painter's sort instead.
//
// Z_BUFFERING 0 is also what makes Scene::rasterizeBand() safe to call for
// disjoint bands from more than one task.
#define Z_BUFFERING 0

// SORT_TRIANGLES gates ONE thing: the per-object std::sort of the source mesh
// in Scene::renderObject, which is its only reference anywhere in the tree. The
// painter's ordering the renderer actually draws from is the sort in
// Scene::prepareFrame, which is NOT gated by this flag — this does not turn
// depth sorting on or off.
//
// Since prepareFrame's sort became an exact radix sort, this flag can only
// affect triangles whose depth keys are EXACTLY EQUAL: the radix sort fully
// determines the order of any two distinct depths no matter what order they
// were queued in, and it is stable, so only ties still see the queue order this
// flag rearranges. (std::sort is not stable, so what it does to those ties is
// arbitrary rather than defined.)
//
// It costs a measured ~1,550us/frame on the demo scene: it runs over every
// non-culled object's ENTIRE triangle list every frame, and its comparator does
// six scattered transformedVertices[] reads plus two integer divides that
// cannot be folded away (a/3 > b/3 is not a > b in integer arithmetic).
//
// Off, and settled by hardware experiment (2026-07-30): with the exact radix in
// place, flipping this back on changed nothing visible — the remaining
// artifacts on the demo's composite rocket are interpenetrating surfaces, which
// have no correct triangle order for ANY sort to find. There is nothing left
// for this pass to fix; it only cost frame time.
#define SORT_TRIANGLES 0
#define SORT_SCENE_OBJECTS 0
#define SORT_SCENE_REVERSE 0

#define DEPTH_ALPHA_BLEND 1

// MESHPUNK 2026-08-05: OFF for the skyloop-focused build (Phil's call).
// Skyloop is 100% flat colours; this drops the UV field from RenderVertex
// (-12 bytes per queued triangle of PSRAM traffic) and compiles the whole
// affine/segmented-perspective/mip pipeline out of drawTriangle. Parked
// textured games (jrworld, vista, demo, old/*) render flat-coloured until
// this is flipped back — the Texture/material APIs still compile, the
// rasteriser just never samples diffuseMap.
#define TEXTURE_MAPPING 0
#define BILINEAR_FILTER 0

// Jet's own per-pixel perspective correction. UNUSABLE AT THIS WORLD SCALE --
// leave at 0 and use MESHPUNK_SEGMENTED_PERSPECTIVE below instead.
//
// It forms the reciprocal as (FIXED_POINT_SCALE * FIXED_POINT_SCALE) / z, i.e.
// 1048576 / z. Ground depths here run to about 700,000 world units, where that
// truncates to 1, and past ~1,048,576 it truncates to 0 -- and the renderer
// does `if (interpolatedOneOverZ == 0) continue;`, dropping the pixel. Enabling
// this would render holes, not corrected texture. It is written for worlds
// whose depths are in the hundreds.
#define PERSPECTIVE_CORRECT_TEXTURES 0

// MESHPUNK: segmented perspective-correct UVs.
//
// Affine UV interpolation errs in proportion to the depth range a triangle
// spans, so a large ground quad seen up close makes its texture swim as the
// camera moves. Correcting every pixel is not worth it; correcting the ends of
// a short span and interpolating between is, because the error inside a span
// falls off with the square of its length.
//
// Two things make this cheap. The /denom in u/z, v/z and 1/z CANCELS in the
// final ratio ((B/denom)/(A/denom) == B/A), so it is never formed; and the
// reciprocal shift is chosen per triangle from that triangle's own nearest
// depth, which keeps the fixed point precise at any world scale. Per pixel the
// cost is one multiply, one shift and one add per axis.
//
// Requires TEXTURE_MAPPING, and is bypassed when PERSPECTIVE_CORRECT_TEXTURES
// or HALF_WIDTH_BUFFERS is on.
// Segment length. Error inside a span falls off with its length SQUARED, so
// halving this quarters the error. Max UV error against exact perspective on a
// ground triangle at this scale, in texels of a 64-texel tile:
//                    typical      extreme grazing
//   affine            121.5           168.7
//   segment 32          6.94           28.14
//   segment 16          1.94            9.46
//   segment  8          0.51            2.51
// The failure mode of a too-long segment is combing on near-ground views: the
// lower the camera, the larger the depth ratio a ground triangle spans per
// screen pixel, and the more perspective has to be resolved inside one
// segment. 8 combed there on hardware when every pixel sampled a solved UV;
// under checkerboard only alternate pixels do (the rest are horizontal
// neighbour averages), which changes what reaches the screen — the hardware
// verdict for 8-under-checkerboard is separate. Must stay a power of two —
// the boundaries are grid-aligned with a mask in Renderer.cpp.
#define MESHPUNK_SEGMENTED_PERSPECTIVE 1
#define MESHPUNK_PERSPECTIVE_SEGMENT 8

// How often the mip level is re-chosen, in pixels. Independent of the UV
// segment above, and deliberately coarser: UV must be resolved tightly because
// its error shows as geometric distortion, while a mip step is a change of
// blur that nobody can localise to a 4-pixel boundary. The level solve costs a
// second perspective evaluation (one screen row down), so running it at 16
// instead of 4 returns three quarters of that.
#define MESHPUNK_MIP_SEGMENT 16

// MESHPUNK: sub-pixel precision for projected vertex positions.
//
// projectVertex used to truncate straight to whole pixels. Every downstream
// quantity is derived from those three anchors, so the whole texture mapping
// inside a triangle is only as accurate as they are — and this scene draws the
// entire screen with about 13 triangles, each carrying ~12.5 texture repeats.
// That makes the UV gradient 40-160 units per pixel against 16 units per texel,
// so a vertex misplaced by one pixel drags the texture by 2.5 to 10 TEXELS.
// Measured segmented-interpolation error for comparison: 0.12 texels.
//
// Worse, it is discontinuous: each vertex crosses a pixel boundary at its own
// moment as the camera turns, so the mapping jumps rather than drifts — a whole
// quarter of the screen shifting several texels at once. This is the same
// integer-vertex wobble the PS1 is famous for, and it is the artefact that
// survived affine correction, UV rebasing, texlod and mipmapping, because all
// four of those are downstream of it.
//
// 4 bits = 1/16 pixel, cutting the error 16x to ~0.17 texels, level with the
// interpolation error. Positions become Q4; edge functions and `denom` become
// Q8. Loop bounds and buffer indices remain whole pixels via SUBPIXEL_BITS.
#define MESHPUNK_SUBPIXEL_BITS 4
#define MESHPUNK_SUBPIXEL_ONE  (1 << MESHPUNK_SUBPIXEL_BITS)

// One directional light plus an ambient term.
#define LIGHTING 1

#define Z_BRIGHTNESS 0

#define FLOAT_CAMERA_ANGLES 1
#define FLOAT_SIN_CACHE_SCALE 10
#define FLOAT_TAN_CACHE_SCALE 1

// ---------------------------------------------------------------------------
// Buffer layout
// ---------------------------------------------------------------------------

// Full horizontal resolution. HALF_WIDTH_BUFFERS halves fill cost by storing
// one uint16_t per two output columns, but host_blit_rect() pushes pixels
// 1:1 to the panel and does not double columns on scanout, so enabling it
// needs an expansion step in the band blit.
#define HALF_WIDTH_BUFFERS 0

// Interlaced field buffers split the frame into two half-height packed
// buffers. That is a different decomposition from the band loop this module
// runs and the two cannot both own the framebuffer pointer.
#define FIELD_BUFFERS 0
#define SSR_FIELD_REFLECT 0

// ---------------------------------------------------------------------------
// Post-processing effects
// ---------------------------------------------------------------------------

// Every PostFX pass reads back a completed full-resolution frame. Band
// rendering retires each band to the panel before the next one is drawn, so
// no such frame exists in memory.
#define POSTFX_CRT         0
#define POSTFX_CELLSHADING 0
#define POSTFX_ANTIALIASING 0
#define POSTFX_BLOOM        0
#define POSTFX_MOTION_BLUR  0
#define POSTFX_CHROMATIC    0
#define POSTFX_PIXELATE     0

#define CRT_SCANLINE_INTENSITY 48
#define MOTION_BLUR_STRENGTH   50
#define CHROMATIC_OFFSET        2
#define PIXELATE_SIZE           4
#define CELLSHADING_CELL_BITS   4

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

#define DEBUG_OVERDRAW 0

// ---------------------------------------------------------------------------
// Depth / fog tuning  (world-space units, scaled by JET32_WORLD_SCALE)
// ---------------------------------------------------------------------------

#define zBrightFar   (1600 * JET32_WORLD_SCALE)
#define zBrightNear  ( 200 * JET32_WORLD_SCALE)
#define zBrightScale 48

#define depthFogFar  (8192 * JET32_WORLD_SCALE)
#define depthFogNear (6144 * JET32_WORLD_SCALE)

// ---------------------------------------------------------------------------
// Checkerboard rendering
// ---------------------------------------------------------------------------

// Half the pixels per frame, on an (x^y) parity that alternates. The skipped
// pixels are rebuilt from their two horizontal neighbours, both drawn this
// frame, so nothing has to survive from the previous frame -- which is what
// lets this work under band rendering where Jet's row interlacing cannot: one
// band buffer is reused for every band, so a "previous frame" pixel is really
// the previous BAND's pixel.
//
// These macros only gate the FIELD_BUFFERS conflict check and the
// reconstruction call inside Scene::render(). The mode itself is the runtime
// Renderer::checkerboardMode flag, which main_tdeck sets from -checkerboard;
// the band loop calls Scene::reconstructCheckerboard() directly.
#define CHECKERBOARD_MODE 1
#define CHECKERBOARD_RECONSTRUCTION 1

// ---------------------------------------------------------------------------
// Screen-space picking
// ---------------------------------------------------------------------------

// MESHPUNK 2026-08-05: 0 — no shipped game uses picking (audited), and each
// queued triangle was carrying 8 bytes of pick provenance (sourceObject +
// sourceTriangleIndex) through the PSRAM bus for it, plus a per-row test in
// drawTriangle. The jet.pick Lua bindings stay registered as no-ops so old
// scripts don't hard-error. Flip back to 4 if a game ever aims a crosshair.
#define MAX_PICK_QUERIES 0

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------

// ESP_PLATFORM is deliberately NOT defined for this build. Jet's ESP32 branch
// includes esp_attr.h and dsps.h, which are ESP-IDF headers; an ELF module
// links only against host_exports[] in src/elf_host.cpp and has no IDF
// include path. PERF_CRITICAL expands to nothing without it.
//
// CONFIG_IDF_TARGET_ESP32S3 is defined on the command line in build.ps1
// instead. It gates inline ESP32-S3 PIE assembly (128-bit EE.VST.128.XP
// stores) in the span fill and clear paths, which needs no headers.

#endif // JET_CONFIG_HPP

#ifndef RENDERER_HPP
#define RENDERER_HPP

#include <cstdint>
#include "Object.hpp"
#include "Material.hpp"
#include "Light.hpp"
#include "Camera.hpp"
#include "JetConfig.hpp"
#include "Picking.hpp"

/// @def ZBUFFER_STRIDE(w)
/// @brief Z-buffer row stride in pixels for a framebuffer of width @p w.
///
/// The depth buffer is stored at half horizontal resolution only when
/// HALF_WIDTH_BUFFERS is enabled (one z-cell per two output pixels);
/// otherwise it is per-pixel. Frontends should size their z-buffer
/// allocation as `ZBUFFER_STRIDE(w) * h * sizeof(uint16_t)`.
#if HALF_WIDTH_BUFFERS
#define ZBUFFER_STRIDE(w) ((w) / 2)
#else
#define ZBUFFER_STRIDE(w) (w)
#endif

namespace Renderer
{
/// MESHPUNK: diagnostic pixel counters, defined in Renderer.cpp.
/// g_jetPxSpan  — pixels written by fillRGB565Span (the cheap untextured path)
/// g_jetPxLoop  — pixels written by the general per-pixel loop
/// Reset by Scene::prepareFrame(). These make cycles-per-pixel a measurement
/// rather than an estimate from screen area and assumed overdraw.
extern uint32_t g_jetPxSpan;
extern uint32_t g_jetPxLoop;

/// @brief Slim render-time vertex flowing through the transform → queue →
/// rasterise pipeline.
///
/// `Object::Vertex` is the authoring type: game/mesh code always has uv,
/// normal, etc. available regardless of build configuration. RenderVertex
/// is what the per-frame pipeline copies around (Scene's transformed-vertex
/// scratch, the render queue, the painter's sort) — so it only carries the
/// fields the configured pipeline actually consumes. With TEXTURE_MAPPING
/// and LIGHTING both off this is just 12 bytes of position instead of 36,
/// which roughly 3×'s the per-triangle queue/sort copy traffic savings.
///
/// `position` is screen-space x/y with camera-space z after projection.
/// MESHPUNK: narrow UV/normal carried by the render queue.
///
/// The queue is the largest per-frame working set in a scene — 216 triangles at
/// 136 bytes is 28.7 KB, re-walked once per band — and two of RenderVertex's
/// fields were far wider than the values they hold. UVs are guaranteed to fit
/// int16 by rebaseUV in jm_createHeightfield (that is what the per-triangle
/// rebase exists for), and normals are +/-FIXED_POINT_SCALE. Narrowing both
/// takes RenderVertex from 36 to 24 bytes and RenderTri from 136 to 92,
/// cutting the queue to 19.4 KB along with the sort and per-band walk traffic.
///
/// `position` stays int32: it is Q4 sub-pixel screen space and off-screen
/// vertices exceed int16's +/-2048 pixel reach.
///
/// The conversions keep every call site reading .x/.y/.z unchanged.
struct RVUV {
    int16_t x = 0, y = 0;
    RVUV() = default;
    RVUV(const Vector2& v) : x((int16_t)v.x), y((int16_t)v.y) {}
    operator Vector2() const { return Vector2(x, y); }
};
struct RVNormal {
    int16_t x = 0, y = 0, z = 0;
    RVNormal() = default;
    RVNormal(const Vector3& v) : x((int16_t)v.x), y((int16_t)v.y), z((int16_t)v.z) {}
    operator Vector3() const { return Vector3(x, y, z); }
};

struct RenderVertex {
    Vector3 position = {0, 0, 0};
#if TEXTURE_MAPPING
    RVUV uv;                        ///< Texture coordinates (int16, see RVUV).
#endif
#if LIGHTING
    RVNormal normal;                ///< View-space (or mesh-local, see lambertBrightness) normal.
    /// @brief Precomputed Lambert brightness for the object-local-light
    /// path (see Scene.cpp "objectLocalLight"). Only meaningful when the
    /// triangle was queued with brightnessPrecomputed == true.
    uint16_t lambertBrightness = 0;
#endif
};

/// @brief Low-level triangle rasteriser owning a colour and depth buffer.
///
/// Scene drives this class on every render(). External users normally
/// access it through `Scene::getRenderer()` rather than constructing one
/// directly.
class Rasterizer
{
    private:
        uint16_t *framebuffer;
        int screenWidth;
        int screenHeight;
        uint16_t *zBuffer;
        int lastRandom = 0;

    public:
        /// @brief Construct a rasteriser bound to caller-owned buffers.
        /// @param framebuffer RGB565 colour buffer of size screenWidth*screenHeight.
        /// @param screenWidth Width in pixels.
        /// @param screenHeight Height in pixels.
        /// @param zBuffer Depth buffer of size ZBUFFER_STRIDE(screenWidth)*screenHeight, or nullptr if Z_BUFFERING is disabled.
        /// @param camera Optional Camera; can be set later.
        Rasterizer(uint16_t *framebuffer, int screenWidth, int screenHeight, uint16_t *zBuffer, Camera *camera = nullptr)
            : framebuffer(framebuffer), screenWidth(screenWidth), screenHeight(screenHeight), zBuffer(zBuffer), camera(camera) {}

        Camera *camera;                 ///< Camera supplying view/projection state.
        bool interlacedMode = false;    ///< When true, only every other row is drawn each frame.
        bool checkerboardMode = false;  ///< When true, alternates between two complementary checkerboard pixel patterns each frame. Requires double-buffering in the frontend for the reconstruction pass.
        bool wireframeMode = false;     ///< When true, triangles are drawn as outlines (in their material colour) instead of being filled. Skips lighting, texturing and the z-buffer; intended as a debug/visualisation aid. Scene::clearBuffers forces a black background while this is on.
        int randomSeed = 255;           ///< Seed for the screen-door / dither random source.

        /// @brief Y-band clip for partial-screen rendering (band / parallel passes).
        ///
        /// Only rows in [yBandMin, yBandMax) are rasterised. Defaults cover the
        /// full screen so existing callers need no changes. Set both fields before
        /// calling drawTriangle(), or use Scene::rasterizeBand() which manages
        /// them automatically via a thread-local copy of the rasteriser.
        int yBandMin = 0;           ///< First row (inclusive) to rasterise. 0 = top of screen.
        int yBandMax = 0x7FFFFFFF;  ///< First row (exclusive) NOT to rasterise. 0x7FFFFFFF = full height.

        /// MESHPUNK: checkerboard coverage bits — one bit per DRAWN-parity
        /// pixel, set by the general pixel loop when it stores a pixel,
        /// cleared per band by Scene::clearBand(). reconstructCheckerboard()
        /// rebuilds only off-parity pixels with a loop-drawn neighbour;
        /// without this gate it also overwrote pixels flat spans and the sky
        /// clear had drawn on BOTH parities, halftoning every vertical
        /// colour boundary — hw-confirmed as "waves on all vertical lines"
        /// (2026-08-03). Layout: screenHeight rows of (screenWidth/2+7)/8
        /// bytes, bit index = x>>1 within the row. Owned by the Scene.
        uint8_t* cbMask = nullptr;

        /// MESHPUNK: runtime DEPTH_ALPHA_BLEND range, replacing the compile-time
        /// depthFogNear/depthFogFar macros. Those could not track a camera far
        /// plane chosen at runtime, so the fade never triggered when the two
        /// disagreed. Defaults are "off" (both at INT32_MAX, so neither the
        /// >= fogFar nor the > fogNear test can fire, and the difference is
        /// never used as a divisor). Set via Scene::setFog().
        int32_t fogNear = 0x7FFFFFFF;
        int32_t fogFar  = 0x7FFFFFFF;

        /// @name Water reflection support
        /// @brief Set by Scene before rasterising to enable WATER_REFLECT shading mode.
        /// @{
        const uint16_t* gradientColors = nullptr; ///< Per-row sky gradient (screenHeight entries); mirrors backgroundGradientColors.
        int             gradientSize   = 0;        ///< Number of entries in gradientColors (== screenHeight).
        int             frameCounter   = 0;        ///< Scene frame counter (dither parity, etc.).
        float           waterTime      = 0.0f;     ///< Accumulated wall-clock seconds; drives ripple animation.
        /// @}

        /// @brief Optional alternate colour buffer for SSR mirror reads.
        ///
        /// When non-null (and SSR_FIELD_REFLECT is defined), WATER_REFLECT
        /// triangles sample mirror pixels from this buffer instead of the
        /// current `framebuffer`. Intended for the previous interlaced field
        /// buffer so reflections see a fully-rendered prior frame rather than
        /// the partially-drawn current one.  Set via
        /// Game::setReflectBuffer() from the render loop each frame.
        /// nullptr (default) falls back to the current framebuffer.
        uint16_t* reflectBuffer = nullptr;

        /// @name Parallel-band water-reflect ordering
        /// @brief When rasterising in parallel bands across multiple threads,
        /// WATER_REFLECT reads mirror rows that may lie in a band being rendered
        /// simultaneously by another thread.
        /// Setting skipWaterReflect on all band copies lets parallel threads
        /// rasterise everything else first; then a single serial pass with
        /// waterReflectOnly=true draws only the water after all bands have joined,
        /// guaranteeing that geometry is in the framebuffer before SSR reads it.
        /// @{
        bool skipWaterReflect = false;  ///< Skip WATER_REFLECT triangles (parallel-band first pass).
        bool waterReflectOnly = false;  ///< Draw ONLY WATER_REFLECT triangles (serial second pass).
        /// @}

        /// @brief Screen row of the water/sky horizon, computed from camera pitch each frame.
        ///        Used as the reflection axis: mirrorY = 2*waterlineY - y.
        ///        Defaults to screenHeight/2 (level camera); Scene::prepareFrame() updates it.
        int waterlineY = 0;

        /// @name Distance-based texture LOD
        /// @brief Beyond `textureLodFar`, textured triangles drop their texture
        /// and render as flat `material->color`, taking the fast simple-span
        /// fill path for free. Between `textureLodNear` and `textureLodFar`
        /// the sampled texel is cross-faded toward the flat colour, using
        /// screen-door stipple under `SCREEN_DOOR_ALPHA` and a per-pixel
        /// alpha blend otherwise. Disabled by default; the host (Scene)
        /// configures it once with whatever distances suit the scene.
        /// Has no effect when `TEXTURE_MAPPING` is compiled out.
        /// @{
        bool textureLodEnabled = false; ///< Master enable for the texture LOD fade.
        int32_t textureLodNear = 0;     ///< Z below which textures render at full detail.
        int32_t textureLodFar  = 0;     ///< Z at and beyond which textures are dropped entirely.
        /// @}

        /// @name MESHPUNK: atmospheric perspective
        /// @brief Blend a triangle's flat colour toward the SKY COLOUR BEHIND
        /// IT with distance, instead of fading it out.
        ///
        /// The existing `fogNear/fogFar` pair works by dropping the triangle's
        /// ALPHA, which has two costs. It loses `plainOpaqueReplace`, so a
        /// fogged triangle falls off the span-fill fast path; and under
        /// SCREEN_DOOR_ALPHA it stipples, so distant terrain becomes a dither
        /// pattern with sky showing through rather than a soft haze.
        ///
        /// Blending the COLOUR keeps the triangle fully opaque, so it stays on
        /// the fast path — this is cheaper than the alpha fog it replaces, not
        /// just better looking. `atmosGradient`, when set, is the same
        /// per-screen-row sky table the background clear uses, sampled at the
        /// triangle's centre row: distant geometry then melts into exactly the
        /// sky it is standing in front of, which is what real aerial
        /// perspective does and what a single flat fog colour cannot.
        ///
        /// Triangle-constant under FAST_Z, so this costs one blend per
        /// triangle, not per pixel.
        /// @{
        bool     atmosEnabled = false;
        int32_t  atmosNear = 0;         ///< Z below which no haze is applied.
        int32_t  atmosFar  = 0;         ///< Z at which geometry is fully hazed.
        uint8_t  atmosMax  = 255;       ///< Cap on the blend (255 = reach full sky).
        uint16_t atmosColor = 0;        ///< Fallback when atmosGradient is null.
        const uint16_t* atmosGradient = nullptr; ///< Sky colour per screen row.
        int      atmosGradientRows = 0;
        /// Screen row of the HORIZON. Gradient samples are clamped to this row
        /// and below, so terrain never hazes toward the zenith.
        ///
        /// Without it a distant mesa standing above the horizon line samples
        /// the bluest part of the sky and blends into it completely — captured
        /// on hardware as large "holes" that a level-ID pass proved were fully
        /// drawn terrain. Real aerial perspective fades distant ground toward
        /// the pale band AT the horizon, which is also what keeps a silhouette
        /// readable against the sky above it.
        int      atmosHorizonRow = 0;
        /// @}

#if MAX_PICK_QUERIES > 0
        /// Pointers to host-owned pick query/result arrays. Storage is owned
        /// by Scene; the rasterizer just reads queries and writes results.
        const PickQuery* pickQueries = nullptr;
        PickResult*      pickResults = nullptr;
        int              pickQueryCount = 0;     ///< Number of slots in use this frame (0..MAX_PICK_QUERIES).
        Object*          currentPickObject = nullptr;       ///< Set by Scene before each drawTriangle for hit attribution.
        int32_t          currentPickTriangleIndex = -1;     ///< Source-mesh triangle index for hit attribution.
#endif

        /// @brief Test whether a pixel at (x, y) should be drawn given a screen-door alpha.
        /// @param x Pixel X.
        /// @param y Pixel Y.
        /// @param alpha Effective alpha (0..255).
        /// @return True if the pixel passes the dither / parity test.
        bool shouldDrawPixel(int x, int y, uint8_t alpha);

        /// @brief Rasterise a single triangle.
        /// @param v1 First vertex.
        /// @param v2 Second vertex.
        /// @param v3 Third vertex.
        /// @param material Material applied to the triangle.
        /// @param directionalLight Active directional light (may be nullptr).
        /// @param ambientLight Active ambient light (may be nullptr).
        /// @param renderEvenLines Used in interlaced mode to select which row parity to draw.
        /// @param ignoreZBuffer Skip the depth test when true.
        /// @param noWriteZBuffer Skip the depth write when true.
        /// @param zBias Per-triangle depth bias in z-buffer units.
        /// @param objAlpha Per-object alpha multiplier (255 = no fade).
        /// @param brightnessPrecomputed v1/v2/v3.lambertBrightness already holds per-vertex brightness (object-local-light path).
        /// @param avgZHint Caller-supplied triangle average camera-space Z, or
        ///        INT32_MIN (default) to compute it here. Scene::rasterizeBand
        ///        passes the avgZ it already computed (and near/far-culled
        ///        against) at queue time, so the FAST_Z setup can skip the
        ///        recompute and the redundant near/far test. Ignored when
        ///        LAZY_Z is enabled (LAZY_Z needs the max, not the average).
        /// @return True if the triangle produced any rasterizer work.
        /// `bakedColorOverride` >= 0 replaces material->color for this triangle
        /// only (MESHPUNK). Triangles whose lighting was pre-baked used to be
        /// queued against one shared mutable Material, which the deferred
        /// render queue then read long after the colour had moved on; the
        /// colour now travels per-triangle instead. -1 = use the material.
        bool drawTriangle(const RenderVertex &v1, const RenderVertex &v2, const RenderVertex &v3, Material *material, DirectionalLight *directionalLight, AmbientLight *ambientLight, bool renderEvenLines, bool ignoreZBuffer, bool noWriteZBuffer, int zBias, uint8_t objAlpha = 255, bool brightnessPrecomputed = false, int32_t avgZHint = INT32_MIN, int32_t bakedColorOverride = -1);

        /// @brief Map an 8-bit grayscale value to RGB565.
        /// @param grayscale 8-bit luminance.
        /// @return RGB565 colour.
        uint16_t grayscaleToRGB565(uint8_t grayscale);

        /// @brief Replace just the colour buffer pointer.
        /// @param newBuffer New caller-owned RGB565 buffer.
        void setFramebuffer(uint16_t *newBuffer) { framebuffer = newBuffer; }

        /// @brief Hot-swap framebuffer, z-buffer and dimensions (e.g. on window resize).
        /// @param newFramebuffer New caller-owned colour buffer.
        /// @param newZBuffer New caller-owned depth buffer.
        /// @param newWidth New width in pixels.
        /// @param newHeight New height in pixels.
        void resize(uint16_t* newFramebuffer, uint16_t* newZBuffer,
                    int newWidth, int newHeight) {
            framebuffer  = newFramebuffer;
            zBuffer      = newZBuffer;
            screenWidth  = newWidth;
            screenHeight = newHeight;
        }
    };
} // namespace Renderer

#endif // RENDERER_HPP
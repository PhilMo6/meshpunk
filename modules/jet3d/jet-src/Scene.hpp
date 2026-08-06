#ifndef SCENE_HPP
#define SCENE_HPP

#include <vector>
#include <algorithm>
#include "Object.hpp"
#include "Camera.hpp"
#include "Light.hpp"
#include "Renderer.hpp"
#include "JetConfig.hpp"
#include "PostFX.hpp"
#include "Sprite2D.hpp"

namespace Renderer {

/// @brief Top-level container that owns the scene graph and drives rendering.
///
/// Holds the active camera, lights, object list and per-frame state, and
/// exposes a single `render()` entry point that runs the full
/// transform/cull/raster/post-FX pipeline.
class Scene {
public:
    /// @brief Construct a scene bound to caller-owned framebuffers.
    /// @param framebuffer RGB565 colour buffer of size screenWidth*screenHeight.
    /// @param zBuffer Depth buffer of size ZBUFFER_STRIDE(screenWidth)*screenHeight; pass nullptr when Z_BUFFERING is disabled.
    /// @param screenWidth Output width in pixels.
    /// @param screenHeight Output height in pixels.
    Scene(uint16_t* framebuffer, uint16_t* zBuffer, int screenWidth, int screenHeight);
    ~Scene();

    int   frameCounter = 0;       ///< Incremented once per render(); useful for animations and dither parity.
    float waterTime    = 0.0f;    ///< Accumulated wall-clock seconds; set each frame by the caller before render/prepareFrame.

    /// @name Per-frame counters populated by render()
    /// @{
    /// `lastFrameDrawnObjects` is the number of enabled objects that
    /// survived the AABB frustum cull. `lastFrameDrawnTriangles` is the
    /// number of triangles submitted to the rasteriser (renderQueue size
    /// after all culling). `lastFrameRasterizedTriangles` is the subset of
    /// those that produced rasterizer work (drawTriangle returned true).
    int lastFrameDrawnObjects        = 0;
    /// MESHPUNK: of `lastFrameDrawnObjects`, how many actually contributed at
    /// least one triangle. The difference is pure waste — objects that passed
    /// the AABB cull, paid the full per-object setup, and emitted nothing.
    int lastFrameEmittingObjects     = 0;
    int lastFrameDrawnTriangles      = 0;
    int lastFrameRasterizedTriangles = 0;
    /// MESHPUNK: triangle-BAND pairs handed to drawTriangle, i.e. those that
    /// survived the triYExtent pre-test. Compared against
    /// lastFrameRasterizedTriangles this separates two very different wastes:
    /// queue entries the band pre-test rejects cheaply (4-byte read), versus
    /// entries that pay full drawTriangle setup and then bail on zero area,
    /// degenerate denom or near/far Z. Only the second kind is worth attacking.
    int lastFrameTriangleCalls = 0;
    /// @}

    /// @brief Add an object to the scene.
    /// @param obj Object to add. Pointer is borrowed; caller retains ownership.
    void addObject(Object* obj);

    /// @brief MESHPUNK: remove an object from the scene.
    ///
    /// The scene does not own its objects, so this only drops the reference —
    /// the caller still owns the Object. Without it an object can only be
    /// hidden via `enabled`, which leaves it paying a cull test every frame and
    /// growing the list forever in anything that spawns and despawns.
    /// @return true if the object was present.
    bool removeObject(Object* obj);

    /// @brief MESHPUNK: one coarse frustum gate over many member objects —
    ///        the aggregation walk from the reference engine, where a culled
    ///        parent skips its whole subtree (`curr += curr->child_count`).
    ///
    /// prepareFrame() sphere-tests each group's world bound once per frame; a
    /// group fully outside the frustum marks every member `groupCulled`, and
    /// the object walk drops those before ANY per-object work (no centre
    /// transform, no 8-corner cull, no fade math). A group that is visible or
    /// straddling changes nothing — members cull individually as always, so
    /// the gate is purely conservative. Frustum only: distance fades stay
    /// per-member, matching the reference (its distance check is separate
    /// from its subtree cull).
    ///
    /// The per-object walk is a flat cost that grows with TOTAL object count
    /// (~30-80 us each), so this is what keeps `prepare` constant as the
    /// world scales instead of linear in world size.
    struct CullGroup {
        Vector3 centre;          ///< World-space bound centre.
        int32_t radius = 0;      ///< Conservative bounding-sphere radius.
        std::vector<Object*> members;
    };

    /// @brief Create a group over `members`. The bound is the union of each
    ///        member's rotation-proof sphere (centre + centreVolume, radius =
    ///        longest AABB dimension), so member rotation never invalidates
    ///        it. Members must already carry their final position — position
    ///        changes after creation are NOT tracked. The scene owns the
    ///        returned group.
    CullGroup* addCullGroup(const std::vector<Object*>& members);

    /// @brief Disband a group. Members return to normal individual culling.
    /// @return true if the group was present.
    bool removeCullGroup(CullGroup* g);

    /// @brief MESHPUNK: distance-fog range in world units, for DEPTH_ALPHA_BLEND.
    ///
    /// The depthFogNear/depthFogFar macros are compile-time and cannot track a
    /// camera far plane set at runtime, which left the whole feature dead
    /// whenever the two disagreed. Defaults are "off"; pass a range to enable.
    /// @param nearDist Distance at which geometry starts fading.
    /// @param farDist Distance at which geometry is fully faded out.
    void setFog(int32_t nearDist, int32_t farDist);

    /// @brief MESHPUNK: disable distance fog.
    void clearFog();

    /// @brief Add a point light to the scene.
    /// @param light Light to add. Pointer is borrowed; caller retains ownership.
    void addPointLight(PointLight* light);

    /// @brief Register a 2D screen-space overlay to be drawn after every render().
    /// @param sprite Sprite to add. Pointer is borrowed; caller retains ownership.
    void addSprite(Sprite2D* sprite);

    /// @brief Set the active camera.
    /// @param cam Camera pointer (borrowed).
    void setCamera(Camera* cam);
    /// @brief Get the active camera.
    /// @return Pointer to the current camera, or nullptr if none is set.
    Camera* getCamera() { return camera; }

    /// @brief Set the active directional light.
    /// @param light Directional light (borrowed). May be nullptr.
    void setDirectionalLight(DirectionalLight* light);
    /// @brief Get the active directional light.
    DirectionalLight* getDirectionalLight() { return directionalLight; }

    /// @brief Set the active ambient light.
    /// @param light Ambient light (borrowed). May be nullptr.
    void setAmbientLight(AmbientLight* light);
    /// @brief Get the active ambient light.
    AmbientLight* getAmbientLight() { return ambientLight; }

    /// @brief Run the full pipeline for one frame: cull, transform, rasterise, post-FX.
    void render();

    /// @brief Phase 1 of split rendering: clear [yBandMin, yBandMax) on the rasteriser,
    ///        transform and depth-sort all objects. Does NOT rasterise triangles.
    ///
    ///        Call this once per frame before any rasterizeBand() calls.
    ///        The rasteriser's yBandMin/yBandMax gate which rows clearBuffers() clears
    ///        so the framebuffer pointer can be a virtual base (adjusted for band offset).
    void prepareFrame();

    /// @brief Phase 2 of split rendering: rasterise the sorted render queue for
    ///        rows [yMin, yMax) only. Uses a thread-local copy of the rasteriser so
    ///        concurrent calls with non-overlapping y ranges are safe when Z_BUFFERING==0.
    ///
    ///        May be called from multiple threads simultaneously with disjoint bands.
    /// fbBase, when non-null, overrides the scene framebuffer for THIS call
    /// only (a virtual base: row y lands at fbBase + y*screenWidth). This is
    /// what makes concurrent band rasterisation possible — each band worker
    /// passes its own buffer and no shared pointer is touched.
    void rasterizeBand(int yMin, int yMax, uint16_t* fbBase = nullptr);

    /// MESHPUNK: prepareFrame split for DUAL-CORE prepare. Begin runs the
    /// single-threaded frame setup (camera, lights, cull groups, queue
    /// clears); then prepareObjectSlice(slot, stride) calls may run
    /// CONCURRENTLY — each takes the objects at indices ≡ slot (mod stride)
    /// and emits into its own buffer pair (slot 0: renderQueue/triYExtent,
    /// slot 1: the W shadows), so the slices share no mutable state beyond
    /// atomic tallies; then End merges the tallies and runs the painter's
    /// sort over BOTH buffers with composite indices (idx < n0 → primary,
    /// else W at idx-n0 — rasterizeBand resolves the same way).
    /// prepareFrame() composes Begin + Slice(0,1) + End: the serial path is
    /// byte-for-byte the old behaviour.
    void prepareFrameBegin();
    void prepareObjectSlice(int slot, int stride);
    void prepareFrameEnd();

    /// MESHPUNK: the fully reentrant per-band pipeline for MULTI-CORE band
    /// rendering: band-local clear (gradient + checkerboard mask rows),
    /// rasterise, checkerboard reconstruction, sprites — all into `bandBuf`
    /// (which holds rows y0..y1 only), touching NO shared mutable Scene or
    /// Rasterizer state. Call prepareFrame() once, single-threaded, first
    /// (it also allocates the checkerboard mask); any number of
    /// renderBandTo() calls for DISJOINT bands may then run concurrently on
    /// different cores. Ignores the scene-level clear flag — a band always
    /// clears itself.
    void renderBandTo(uint16_t* bandBuf, int y0, int y1);

    /// @brief Clear only the rows [yMin, yMax) of the current framebuffer without
    ///        re-running the transform or sort pipeline. Use this for bands 1+ when the
    ///        render queue from the preceding prepareFrame() call is still valid.
    ///
    ///        Sets the rasteriser's yBandMin/yBandMax before clearing so the
    ///        band-aware clear writes only into the correct region.
    void clearBand(int yMin, int yMax);

    /// @brief Advance the internal frame counter by one. Normally called automatically
    ///        by render(); use this when driving the pipeline via prepareFrame()/rasterizeBand().
    void advanceFrameCounter() { frameCounter++; }

    /// @brief Rebuild the pixels checkerboardMode skipped, from their left and
    ///        right neighbours. Scoped to the rasteriser's current yBandMin/
    ///        yBandMax, so the band loop calls it once per band. No-op unless
    ///        Rasterizer::checkerboardMode is set.
    ///
    ///        Call it after rasterizeBand() and BEFORE drawSprites() or any
    ///        overlay pass: those write the framebuffer directly rather than
    ///        through the rasteriser, so they are not checkerboarded and this
    ///        would blur them. render() places it at the same point.
    /// fbBase/y0/y1 override the shared framebuffer + band bounds for
    /// reentrant use (see renderBandTo); defaults read the shared state.
    void reconstructCheckerboard(uint16_t* fbBase = nullptr,
                                 int y0 = -1, int y1 = -1);

    /// @brief Get total scene statistics (independent of camera position).
    /// @param objectCount Out: number of enabled objects.
    /// @param triangleCount Out: total triangle count across enabled objects.
    /// @param vertexCount Out: total vertex count across enabled objects.
    void getStatistics(int& objectCount, int& triangleCount, int& vertexCount);

    /// MESHPUNK: mark the cached getStatistics totals stale. addObject and
    /// removeObject do this themselves; call it after anything else that
    /// changes what the totals would be — in practice toggling Object::enabled,
    /// which the Scene cannot observe.
    void invalidateStatistics() { statsDirty = true; }

    /// @brief Set the colour used to clear the framebuffer.
    /// @param color RGB565 clear colour.
    void setBackcolor(uint16_t color) { backcolor = color; }

    /// @brief Replace the colour buffer pointer (without changing dimensions).
    /// @param framebuffer New caller-owned RGB565 buffer.
    void setFramebuffer(uint16_t *framebuffer);

    /// @brief Enable or disable per-frame framebuffer clearing.
    /// @param clear True to clear before rendering, false to preserve previous content.
    void setClearBuffer(bool clear) { clearRenderBuffer = clear; }

    /// @brief Hot-swap framebuffer, z-buffer and dimensions (e.g. on window resize).
    ///
    /// The caller owns both buffers and is responsible for freeing the old
    /// ones AFTER this call returns. PostFX is recreated internally to
    /// pick up the new dimensions.
    /// @param newFramebuffer New caller-owned colour buffer.
    /// @param newZBuffer New caller-owned depth buffer.
    /// @param newWidth New width in pixels.
    /// @param newHeight New height in pixels.
    void resize(uint16_t* newFramebuffer, uint16_t* newZBuffer,
                int newWidth, int newHeight);

    /// @brief Get the underlying rasteriser.
    Rasterizer* getRenderer() { return renderer; }
    /// @brief Get the mutable list of scene objects.
    std::vector<Object*>& getObjects() { return objects; }
    /// @brief Get the mutable list of point lights.
    std::vector<PointLight*>& getPointLights() { return pointLights; }
    /// @brief Get the mutable list of materials owned by the scene.
    std::vector<Material*>& getMaterials() { return materials; }
    /// @brief Get the list of registered 2D sprites.
    /// On HALF_WIDTH_BUFFERS builds the display layer composites sprites
    /// during scanout at full resolution; expose the list so it can do so.
    std::vector<Sprite2D*>& getSprites() { return sprites; }

#if MAX_PICK_QUERIES > 0
    /// @brief Submit screen-space pick points to be tested during the next render().
    ///
    /// Excess queries beyond MAX_PICK_QUERIES are silently dropped. Pass
    /// count == 0 (or just don't call this) to disable picking. The renderer
    /// reads queries during render() and writes the matching slots in the
    /// pick result array. Both arrays are owned by Scene; the host should
    /// copy queries in by value and read results back after render().
    /// @param queries Caller-owned array of pick queries.
    /// @param count Number of valid entries in @p queries.
    void setPickQueries(const PickQuery* queries, int count);

    /// @brief Get the pick results from the most recent render() call.
    /// @return Pointer to an internal array of MAX_PICK_QUERIES results.
    const PickResult* getPickResults() const { return pickResults; }

    /// @brief Get the number of active pick queries set for the next render().
    int getPickQueryCount() const { return pickQueryCount; }
#endif


    uint16_t* backgroundGradientColors = nullptr;   ///< Optional per-row background gradient (screenHeight entries) used during clear.

    /// @name Distance-based level of detail (LOD)
    /// @brief Global LOD selection driven by camera-to-object distance.
    ///
    /// `lodScale` is the world-units-per-LOD-step. Setting it to 0 (the
    /// default) disables global LOD and every Object renders its own
    /// mesh as before. With `lodScale = 4096`, an Object with two LOD
    /// meshes attached renders LOD 0 below 4096 units, LOD 1 from 4096
    /// to 8191, LOD 2 from 8192 onward (or fades out / persists past
    /// the last LOD depending on the Object's `lodPersist` flag).
    ///
    /// `lodBias` is added to the computed level globally — a bias of -1
    /// pushes everything one LOD step higher in detail, +1 cheaper. This
    /// composes with the per-Object `lodBias` and is intended for runtime
    /// quality knobs (e.g. perf-driven dynamic adjustment).
    /// @{
    int32_t lodScale = 0;   ///< World units per LOD step; 0 disables global LOD.
    int8_t  lodBias  = 0;   ///< Scene-wide LOD level offset (added to each object's choice).
    /// @}

private:
    struct RenderTri {
        RenderVertex v1, v2, v3;
        Material* material;
        int32_t avgZ;
        /// MESHPUNK: painter's-sort key depth, separate from avgZ because avgZ
        /// also feeds the near/far cull and the FAST_Z hint drawTriangle uses
        /// for fog. Only the ORDERING should follow Object::sortDepth; the
        /// depth the rasteriser and fog see must stay the true average.
        int32_t sortZ;
        bool ignoreZBuffer;
        bool noWriteZBuffer;
        int8_t zBias;
        // Per-object alpha multiplier (255 = no per-object fade); folded
        // into the per-pixel screen-door alpha at raster time.
        uint8_t objAlpha;
        // When true, v1/v2/v3.lambertBrightness has been precomputed in
        // object-local space by renderObject (see "objectLocalLight" path
        // in Scene.cpp). drawTriangle skips its own jetShadeBrightness
        // calls in that case and reads the cached values directly. Only
        // ever set for objects whose materials are all non-specular.
        bool brightnessPrecomputed = false;

        // MESHPUNK: per-triangle baked colour, carried BY VALUE.
        //
        // Triangles baked with light=true used to be queued pointing at the
        // single shared `s_bakedMat`, whose colour was overwritten just before
        // each emit. That is only sound if a triangle rasterises immediately;
        // this renderer queues every triangle, sorts the queue, and reads
        // `material->color` much later — so the whole scene drew in the colour
        // of whichever baked triangle happened to be queued LAST. Symptom on
        // hardware: all terrain one colour, changing with viewpoint, and
        // turning brown (the prop colour) whenever a prop came near, which also
        // made the prop indistinguishable from the ground it stood on.
        //
        // ORDERING IS LOAD-BEARING: `colorBaked` must be declared BEFORE
        // `bakedColor`. The bool takes the odd padding byte and the uint16 the
        // aligned pair, so both fit in padding that already existed and the
        // struct stays 136 bytes. Declared the other way round the uint16
        // must 2-align, strands a byte, and the struct grows to 140
        // (host-measured both ways).
        bool     colorBaked = false;
        uint16_t bakedColor = 0;
#if MAX_PICK_QUERIES > 0
        // Source object + ORIGINAL triangle index (in obj->triangles) for
        // pick attribution. Carried through the painter sort.
        Object* sourceObject;
        int32_t sourceTriangleIndex;
#endif
    };
    std::vector<RenderTri> renderQueue;
    // Slot-1 emit buffers for the concurrent prepare slice (see
    // prepareObjectSlice). Composite indices: idx < renderQueue.size() reads
    // the primary, otherwise renderQueueW[idx - n0].
    std::vector<RenderTri> renderQueueW;
    std::vector<int32_t>   triYExtentW;
    // Per-slot walk tallies, merged by prepareFrameEnd.
    int prepDrawn[2]    = { 0, 0 };
    int prepEmitting[2] = { 0, 0 };
    // Per-object slice assignment, rebuilt each frame by prepareFrameBegin's
    // greedy triangle-weight balance; prepareObjectSlice(slot, 2) walks only
    // its tagged objects. (Index parity anti-correlated with skyloop's
    // detail/twin bake order and starved the worker.)
    std::vector<uint8_t> objSliceTag;
    // Painter's-sort output as indices into renderQueue, rebuilt by
    // prepareFrame() each frame. Sorting (scattering) 4-byte indices
    // instead of whole RenderTri structs avoids a full second copy of the
    // queue per frame; rasterizeBand() walks this to draw in depth order.
    std::vector<int32_t> renderOrder;

    // MESHPUNK: scratch for the painter's radix sort in prepareFrame(). The
    // depth keys are lifted out of renderQueue into these compact 4-byte arrays
    // so the sort's passes never touch the 132-byte RenderTri structs again.
    // Members rather than locals so the capacity survives between frames and
    // nothing is allocated per frame.
    std::vector<uint32_t> radixKeyA, radixKeyB;
    std::vector<int32_t>  radixIdxA, radixIdxB;

    // MESHPUNK: screen-space Y extent of each queued triangle, parallel to
    // renderQueue and packed as (maxY << 16) | (minY & 0xFFFF) so one aligned
    // 4-byte read answers "does this triangle touch this band?".
    //
    // rasterizeBand() walks the whole queue once PER BAND, and drawTriangle
    // does not reject an out-of-band triangle until after it has dereferenced
    // all three 36-byte RenderVertex structs to build a bounding box. That made
    // the re-walk 81-85% of raster time on hardware (measured 3.12us per
    // triangle per band; actual pixel fill was only 8.4ms of a 58ms raster),
    // because the ~136-byte structs are scattered PSRAM reads in sorted order.
    // Testing this array first turns a 282KB scattered read per band into an
    // 8KB sequential one.
    std::vector<int32_t> triYExtent;

    // MESHPUNK: per-band bins over renderOrder, rebuilt by prepareFrameEnd
    // when bandRowsHint is set. bandBinStart[b]..bandBinStart[b+1] indexes
    // bandBinEntries, which holds composite queue indices in draw (depth)
    // order for exactly the triangles whose Y extent touches band b. A
    // rasterizeBand call aligned to the hint grid iterates its bin instead
    // of walking the whole sorted queue with the extent pre-test — the walk
    // was up to bands x queue x 8 bytes of PSRAM reads per frame. Entries
    // duplicate per band spanned, so memory is queue length x average span.
    int  bandRowsHint  = 0;      // 0 = binning off (setBandRows)
    bool bandBinsValid = false;
    std::vector<int32_t> bandBinEntries, bandBinStart, bandBinCursor;

    // MESHPUNK: cached getStatistics totals — see the definition for why.
    int  statTriangles = 0, statVertices = 0;
    bool statsDirty    = true;

public:
    /// MESHPUNK: hard ceiling on queued triangles per frame. 0 = unbounded
    /// (the original behaviour).
    ///
    /// renderQueue grows geometrically, and at 136 bytes per entry a
    /// 4096 -> 8192 step wants a 1,088 KB contiguous block while still holding
    /// the old 544 KB. That is 1.59 MB of transient large-block demand arriving
    /// mid-frame, in a heap whose largest free block never recovers contiguity
    /// after a world is freed — and it crashed a 256,000-unit world on the
    /// first frame after the rebuild (ROM memcpy, excvaddr exactly 4096*136).
    ///
    /// Reserving once at startup, when the heap is fresh, converts that from a
    /// mid-run failure into a bounded up-front cost. The cap then guarantees
    /// the vector can never reallocate. Dropping triangles is a real visual
    /// compromise, but only in scenes already far past playable: a frame
    /// emitting 4,096 triangles measures ~13 fps on hardware.
    size_t maxQueuedTriangles = 0;
    /// Triangles dropped by that cap on the last prepareFrame(). Non-zero means
    /// the frame was NOT fully drawn — never let this be silent.
    int    lastFrameDroppedTriangles = 0;

    /// Reserve the render queue and every array parallel to it. Call once at
    /// startup; also sets maxQueuedTriangles to the same figure.
    void reserveRenderQueue(size_t triangles);

    /// MESHPUNK: tell the scene the band height the module renders with so
    /// prepareFrameEnd can bin triangles per band (see bandBinEntries).
    /// 0 disables binning; misaligned rasterizeBand calls fall back to the
    /// full sorted-queue walk either way, so this is purely an accelerator.
    void setBandRows(int rows) { bandRowsHint = rows > 0 ? rows : 0; }

    /// MESHPUNK: set by the frontend when it abandons a prepare wait (the
    /// dead-worker timeout). prepareObjectSlice's WORKER slice checks it
    /// per object and stops emitting, so a late slice can never race the
    /// next frame's Begin clearing the queues it writes. Cleared by the
    /// frontend just before each prepSeq publish.
    volatile bool prepAbort = false;

    /// MESHPUNK: per-slice prepare tallies for the perf log — slice 0 is
    /// the module task's half of the objects, slice 1 the worker's.
    int prepSliceDrawn(int s) const { return prepDrawn[s ? 1 : 0]; }
    int prepSliceTris(int s) const {
        return (int)(s ? renderQueueW.size() : renderQueue.size());
    }


    /// Bytes one queued triangle occupies. Exposed so the boot log can report
    /// the reservation in real units without hardcoding a number that drifts
    /// the moment RenderTri changes.
    static size_t renderTriBytes();
private:

    // Pack/unpack helpers. Values are clamped OUTWARD into int16 so the extent
    // is never narrower than the truth: a near-plane-clipped vertex can project
    // to tens of thousands of pixels, and a wrongly-narrowed extent would drop
    // a triangle the rasteriser should have drawn.
    static inline int32_t packYExtent(int32_t minY, int32_t maxY) {
        // Saturate rather than truncate: a raw cast of a +50000 projection
        // wraps to a negative int16 and would invert the comparison. Both ends
        // saturate far outside any band, so clamping cannot change a verdict.
        if (minY < -32768) minY = -32768; else if (minY > 32767) minY = 32767;
        if (maxY < -32768) maxY = -32768; else if (maxY > 32767) maxY = 32767;
        return ((int32_t)(int16_t)maxY << 16) | (uint16_t)(int16_t)minY;
    }
    static inline int32_t yExtentMin(int32_t p) { return (int16_t)(p & 0xFFFF); }
    static inline int32_t yExtentMax(int32_t p) { return (int16_t)(p >> 16); }

    Camera* camera;
    DirectionalLight* directionalLight;
    AmbientLight* ambientLight;
    Rasterizer* renderer = nullptr;
    PostFX* postFX = nullptr;

    uint16_t* framebuffer;
    uint16_t* zBuffer;
    int screenWidth;
    int screenHeight;
    bool* scanlinesUpdated;

    std::vector<Object*> objects;
    std::vector<PointLight*> pointLights;
    std::vector<Material*> materials;
    std::vector<Sprite2D*> sprites;

    uint16_t backcolor = 0;
    bool clearRenderBuffer = true;
    bool renderEvenLines = false;

    // Frustum side-plane normal lengths for the quick sphere cull in
    // cullObject(): |(fovFactor, ±screenW/2)| and |(fovFactor, ±screenH/2)|.
    // Recomputed once per frame in prepareFrame() because fovFactor
    // changes at runtime (boost FOV kick).
    float cullPlaneLh = 1.0f;
    float cullPlaneLv = 1.0f;

    bool cullObject(Object* obj,
                    int32_t camCosX, int32_t camSinX,
                    int32_t camCosY, int32_t camSinY,
                    int32_t camCosZ, int32_t camSinZ) const;

    // MESHPUNK: cull groups (see the public CullGroup docs). Owned here.
    std::vector<CullGroup*> cullGroups;

    // World-space sphere vs frustum, the same transform and plane tests as
    // cullObject's quick path. Returns true only when the sphere is FULLY
    // outside one frustum half-space — conservative by construction.
    bool cullSphereWorld(const Vector3& worldCentre, int32_t radius,
                         int32_t camCosX, int32_t camSinX,
                         int32_t camCosY, int32_t camSinY,
                         int32_t camCosZ, int32_t camSinZ) const;

    void renderObject(Object* obj,
                      int32_t camCosX, int32_t camSinX,
                      int32_t camCosY, int32_t camSinY,
                      int32_t camCosZ, int32_t camSinZ,
                      uint8_t objAlpha,
                      Object* meshSource = nullptr,
                      int emitSlot = 0);
    void clearBuffers();

public:
    /// @brief Composite all enabled sprites onto the current framebuffer.
    ///        Normally called automatically by render(); call explicitly when
    ///        driving the pipeline via prepareFrame()/rasterizeBand().
    /// fbBase/y0/y1 override the shared framebuffer + band bounds for
    /// reentrant use (see renderBandTo); defaults read the shared state.
    void drawSprites(uint16_t* fbBase = nullptr, int y0 = -1, int y1 = -1);

private:

#if MAX_PICK_QUERIES > 0
    PickQuery  pickQueries[MAX_PICK_QUERIES];
    PickResult pickResults[MAX_PICK_QUERIES];
    int        pickQueryCount = 0;
#endif
};

} // namespace Renderer

#endif // SCENE_HPP

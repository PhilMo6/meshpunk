#include "Scene.hpp"
#include "TrigLUT.hpp"
#include "Renderer.hpp"
#include "JetConfig.hpp"
#include <cstring> // For memset
#include <algorithm> // For std::min, std::max
#include <cmath> // For sqrtf (per-object distance fade / LOD pick)

namespace Renderer {

// Static emissive material used to render triangles whose lighting has been
// pre-baked into Triangle::bakedColor.  The colour field is overwritten
// per-triangle before emitTri() is called; all other fields are constant.
// ONE PER EMIT SLOT: the colour field is written per-triangle and captured
// by pointer identity in emitTri; a single shared instance would let the two
// concurrent prepare slices overwrite each other's colour between the write
// and the capture. Initialised from prepareFrameBegin (single-threaded).
static Material s_bakedMat[2];
static bool     s_bakedMatInit = false;
static void initBakedMat() {
    if (s_bakedMatInit) return;
    for (int i = 0; i < 2; ++i) {
        s_bakedMat[i].diffuseMap  = nullptr;
        s_bakedMat[i].shader      = nullptr;
        s_bakedMat[i].emissive    = true;
        s_bakedMat[i].alpha       = 255;
        s_bakedMat[i].diffuse     = 0;
        s_bakedMat[i].specular    = 0;
        s_bakedMat[i].shadingMode = ShadingMode::FLAT;
        s_bakedMat[i].name        = nullptr;
    }
    s_bakedMatInit = true;
}

#if LIGHTING
// Diffuse-only Lambert shading, evaluated in whatever frame N and L
// share (object-local for the precompute path below). Matches the
// diffuse branch of Renderer.cpp's jetShadeBrightness when
// specularCoef == 0: 12-bit reduce, clamp, square-falloff, scale by
// lightIntensity, scale by diffuseCoef, clamp.
//
// The specular branch is intentionally absent — this helper is only
// used for objects whose materials are ALL non-specular, which is the
// exact precondition for skipping the view-space normal transform.
// Inlining the specular branch here would mean its `N.z < 0` test
// would be reading an object-local component instead of view-space Z
// and give wrong results.
static inline uint16_t sceneLambertDiffuse(const Vector3& N, const Vector3& L,
                                           uint16_t lightIntensity,
                                           uint8_t diffuseCoef)
{
    if (lightIntensity > 255) lightIntensity = 255;
    int64_t lit = Vector3::dotProduct(N, L);
    if (lit <= 0) return 0;
    uint32_t lambert = (uint32_t)(lit >> 12);
    if (lambert > 255) lambert = 255;
    lambert = (lambert * lambert + 128) >> 8;       // squared falloff
    lambert = (lambert * lightIntensity) >> 8;
    uint32_t diffuseTerm = (lambert * diffuseCoef) >> 8;
    if (diffuseTerm > 255) diffuseTerm = 255;
    return (uint16_t)diffuseTerm;
}
#endif

// Returns true if the object's AABB is entirely outside the view frustum.
bool Scene::cullObject(Object* obj,
                       int32_t camCosX, int32_t camSinX,
                       int32_t camCosY, int32_t camSinY,
                       int32_t camCosZ, int32_t camSinZ) const {
    if (obj->isBillboard) return false; // keep billboards simple for now

    Vector3 camPos(camera->position);
    Vector3 objPos(obj->position);
    // MESHPUNK: the AABB is authored in unscaled local space, so an object
    // using transformScale has to have its bounds scaled to match or the
    // frustum test rejects it while it is still on screen.
    Vector3 bMinS = obj->boundingBoxMin;
    Vector3 bMaxS = obj->boundingBoxMax;
    if (obj->transformScale) {
        bMinS.assign((int32_t)(((int64_t)bMinS.x * obj->scale.x) / FIXED_POINT_SCALE),
                     (int32_t)(((int64_t)bMinS.y * obj->scale.y) / FIXED_POINT_SCALE),
                     (int32_t)(((int64_t)bMinS.z * obj->scale.z) / FIXED_POINT_SCALE));
        bMaxS.assign((int32_t)(((int64_t)bMaxS.x * obj->scale.x) / FIXED_POINT_SCALE),
                     (int32_t)(((int64_t)bMaxS.y * obj->scale.y) / FIXED_POINT_SCALE),
                     (int32_t)(((int64_t)bMaxS.z * obj->scale.z) / FIXED_POINT_SCALE));
    }
    const Vector3& bMin = bMinS;
    const Vector3& bMax = bMaxS;

    int outLeft = 0, outRight = 0, outTop = 0, outBottom = 0, outNear = 0, outFar = 0;
    float   fovFactor = camera->fovFactor;
    int32_t nearPlane = camera->nearPlane;
    int32_t farPlane  = camera->farPlane;

    // ---- Quick sphere-vs-frustum classification ----------------------------
    // One point transform (~20 ops) instead of the 8-corner AABB test
    // (~240 ops) for the vast majority of objects. Conservative bounding
    // sphere: centre at position+centreVolume (same rotation-ignoring
    // convention as prepareFrame's far pre-cull) with radius = longest
    // AABB dimension, which always covers the true half-diagonal
    // (halfDiag <= 0.866*maxExtent) plus slack for rotated meshes.
    //
    //   * sphere entirely outside ANY single frustum half-space → culled.
    //   * sphere entirely inside ALL half-spaces → visible.
    //   * straddling a boundary → fall through to the precise 8-corner test.
    //
    // The side-plane half-space checks use the plane equation directly
    // (signed distance × normal length), so they're valid regardless of
    // the sphere's z sign — no divides, no per-object sqrt (plane normal
    // lengths cullPlaneLh/Lv are cached per frame in prepareFrame()).
    {
        const float r = (float)std::max({bMax.x - bMin.x,
                                         bMax.y - bMin.y,
                                         bMax.z - bMin.z});
        constexpr float invFps = 1.0f / (float)FIXED_POINT_SCALE;
        const float cYc = (float)camCosY * invFps, cYs = (float)camSinY * invFps;
        const float cXc = (float)camCosX * invFps, cXs = (float)camSinX * invFps;
        const float cZc = (float)camCosZ * invFps, cZs = (float)camSinZ * invFps;
        const float px = (float)(objPos.x + obj->centreVolume.x - camPos.x);
        const float py = (float)(objPos.y + obj->centreVolume.y - camPos.y);
        const float pz = (float)(objPos.z + obj->centreVolume.z - camPos.z);
        // Camera rotation Y, X, Z — same order as the corner loop below.
        const float t1x =  px * cYc + pz * cYs;
        const float t1z = -px * cYs + pz * cYc;
        const float t2y =  py * cXc - t1z * cXs;
        const float t2z =  py * cXs + t1z * cXc;
        const float cxv =  t1x * cZc - t2y * cZs;
        const float cyv =  t1x * cZs + t2y * cZc;
        const float czv =  t2z;

        if (czv + r < (float)nearPlane) return true;   // fully behind
        if (czv - r > (float)farPlane)  return true;   // fully beyond

        const float hw  = (float)screenWidth  * 0.5f;
        const float hh  = (float)screenHeight * 0.5f;
        const float rLh = r * cullPlaneLh;
        const float rLv = r * cullPlaneLv;
        // Signed distance × plane length to each side plane: positive =
        // outside that plane's visible half-space.
        const float dR =  cxv * fovFactor - czv * hw;   // right of right plane
        const float dL = -cxv * fovFactor - czv * hw;   // left of left plane
        const float dT =  cyv * fovFactor - czv * hh;   // above top plane
        const float dB = -cyv * fovFactor - czv * hh;   // below bottom plane
        if (dR > rLh || dL > rLh || dT > rLv || dB > rLv) return true;

        if (czv - r > (float)nearPlane && czv + r < (float)farPlane &&
            dR < -rLh && dL < -rLh && dT < -rLv && dB < -rLv)
            return false;   // fully inside — skip the 8-corner test
    }

    // MESHPUNK: under a matrix override the Euler-based corner rotation below
    // would use stale angles. The sphere test above already handled the
    // rotation-invariant rejections; keep the object (conservative).
    if (obj->useMatrix) return false;

    int32_t objCosX = lookupCosI(obj->rotation.x), objSinX = lookupSinI(obj->rotation.x);
    int32_t objCosY = lookupCosI(obj->rotation.y), objSinY = lookupSinI(obj->rotation.y);
    int32_t objCosZ = lookupCosI(obj->rotation.z), objSinZ = lookupSinI(obj->rotation.z);

    for (int i = 0; i < 8; ++i) {
        Vector3 p(
            (i & 1) ? bMax.x : bMin.x,
            (i & 2) ? bMax.y : bMin.y,
            (i & 4) ? bMax.z : bMin.z);

        // Object rotation (X, Y, Z) — same order as renderObject
        //
        // MESHPUNK: 64-bit intermediates, for exactly the reason the camera
        // rotation below already carries them — and this half was missed when
        // that one was fixed.
        //
        // These operands are OBJECT-LOCAL AABB corners, and `coord * 1024`
        // overflows int32 past 2,097,152 world units (262,144 authored). That
        // is not a large object, it is a large OFFSET: a mesh built centred on
        // the world origin and cut up by splitCells hands every cell
        // world-space vertices, so a cell's local coordinates are its distance
        // from the map centre. Past that radius the corners land at arbitrary
        // positions and the cell is spuriously culled.
        //
        // It overflows even at ZERO rotation, because the multiply happens
        // before the divide. Hardware signature: terrain missing in a pattern
        // that depends on distance from the MAP CENTRE and on camera angle,
        // while props standing on the same ground still draw — props are
        // separate small meshes whose local coordinates never approach the
        // rail. On a 1.6 km map only ~8% of cells fell inside it.
        #define JET_OROT2(a, ca, b, sb) \
            (int32_t)(((int64_t)(a) * (ca) + (int64_t)(b) * (sb)) / FIXED_POINT_SCALE)
        p.assign(p.x,
                 JET_OROT2(p.y, objCosX, p.z, -objSinX),
                 JET_OROT2(p.y, objSinX, p.z,  objCosX));
        p.assign(JET_OROT2(p.x, objCosY, p.z,  objSinY),
                  p.y,
                 JET_OROT2(p.z, objCosY, p.x, -objSinY));
        p.assign(JET_OROT2(p.x, objCosZ, p.y, -objSinZ),
                 JET_OROT2(p.x, objSinZ, p.y,  objCosZ),
                  p.z);
        #undef JET_OROT2

        // Translation
        p.add(objPos);
        p.add(camPos.inverse());

        // Camera rotation (Y, X, Z)
        //
        // MESHPUNK: 64-bit intermediates. These operands are CAMERA-RELATIVE
        // world coordinates, and `coord * FIXED_POINT_SCALE` overflows int32
        // past ~2.1M world units (262,144 authored) — at which point corners
        // land at arbitrary coordinates and the object is spuriously culled.
        //
        // It only ever bit LARGE objects, which is why it hid for so long: the
        // float sphere test above returns early for anything it can classify
        // outright, and only a bounding sphere that STRADDLES a frustum plane
        // reaches this loop. Small meshes are decided upstairs; a coarse
        // world tile is exactly the thing that straddles. Symptom on hardware
        // was distant terrain vanishing at altitude while nearby tiles stayed,
        // and tiles popping back at the screen edge as the camera turned.
        #define JET_ROT2(a, ca, b, sb) \
            (int32_t)(((int64_t)(a) * (ca) + (int64_t)(b) * (sb)) / FIXED_POINT_SCALE)
        Vector3 r;
        r.assign(JET_ROT2(p.x, camCosY,  p.z, camSinY),
                  p.y,
                 JET_ROT2(p.z, camCosY, -p.x, camSinY)); p = r;
        r.assign(p.x,
                 JET_ROT2(p.y, camCosX, -p.z, camSinX),
                 JET_ROT2(p.y, camSinX,  p.z, camCosX)); p = r;
        r.assign(JET_ROT2(p.x, camCosZ, -p.y, camSinZ),
                 JET_ROT2(p.x, camSinZ,  p.y, camCosZ),
                  p.z); p = r;
        #undef JET_ROT2

        if (p.z < nearPlane) { outNear++; continue; }
        if (p.z > farPlane)  { outFar++;  continue; }
        if (p.z <= 0)        { outNear++; continue; }

        const float invZ = fovFactor / (float)p.z;
        int32_t sx = (int32_t)(p.x * invZ) + screenWidth / 2;
        int32_t sy = screenHeight / 2 - (int32_t)(p.y * invZ);
        if (sx < 0)            outLeft++;
        if (sx > screenWidth)  outRight++;
        if (sy < 0)            outTop++;
        if (sy > screenHeight) outBottom++;
    }

    return (outNear == 8 || outFar == 8 ||
            outLeft == 8 || outRight == 8 ||
            outTop  == 8 || outBottom == 8);
}

Scene::Scene(uint16_t* framebuffer, uint16_t* zBuffer, int screenWidth, int screenHeight)
    : camera(nullptr), directionalLight(nullptr), ambientLight(nullptr),
      framebuffer(framebuffer), zBuffer(zBuffer), screenWidth(screenWidth), screenHeight(screenHeight), scanlinesUpdated(nullptr) {
    initializeTrigTables();
    initBakedMat();
    renderer = new Rasterizer(framebuffer, screenWidth, screenHeight, zBuffer);
    postFX = new PostFX(screenWidth, screenHeight);
}
Scene::~Scene() {
    if (renderer) {
        free(renderer->cbMask);   // MESHPUNK: scene owns the coverage mask
        delete renderer;
        renderer = nullptr;
    }
    if (postFX) {
        delete postFX;
        postFX = nullptr;
    }
    for (CullGroup* g : cullGroups) delete g;   // MESHPUNK: scene owns groups
    cullGroups.clear();
}

void Scene::setFramebuffer(uint16_t *newBuffer) {
    framebuffer = newBuffer;
    renderer->setFramebuffer(newBuffer);
}

void Scene::resize(uint16_t* newFramebuffer, uint16_t* newZBuffer,
                   int newWidth, int newHeight) {
    framebuffer  = newFramebuffer;
    zBuffer      = newZBuffer;
    screenWidth  = newWidth;
    screenHeight = newHeight;
    if (renderer) {
        renderer->resize(newFramebuffer, newZBuffer, newWidth, newHeight);
    }
    // PostFX caches its own dimensions and (when enabled) owns scratch
    // buffers sized to them. Easiest correct thing is to rebuild it.
    if (postFX) {
        delete postFX;
        postFX = new PostFX(newWidth, newHeight);
    }
}

size_t Scene::renderTriBytes() { return sizeof(RenderTri); }

// MESHPUNK: see the declaration. Every array here is indexed in lockstep with
// renderQueue, so they are all reserved together — otherwise the queue stops
// reallocating and its shadows keep doing it.
void Scene::reserveRenderQueue(size_t triangles) {
    if (triangles == 0) return;
    renderQueue.reserve(triangles);
    triYExtent.reserve(triangles);
    // The concurrent prepare slice's buffers, and the sort arrays sized for
    // the COMBINED worst case (each slice can fill its own cap).
    renderQueueW.reserve(triangles);
    triYExtentW.reserve(triangles);
    renderOrder.reserve(triangles * 2);
    radixKeyA.reserve(triangles * 2);
    radixKeyB.reserve(triangles * 2);
    radixIdxA.reserve(triangles * 2);
    radixIdxB.reserve(triangles * 2);
    // Band bins: entries duplicate per band spanned; small triangles span
    // 1-2 bands, so 3x the combined cap covers the realistic worst case
    // without a mid-run large alloc (resize() still grows it if ever beaten).
    bandBinEntries.reserve(triangles * 3);
    maxQueuedTriangles = triangles;
}

void Scene::addObject(Object* obj) {
    objects.push_back(obj);
    statsDirty = true;          // MESHPUNK: see getStatistics
}

// MESHPUNK
bool Scene::removeObject(Object* obj) {
    // Sweep the object out of any cull group so a group never holds a
    // dangling pointer. Rare operation; the linear scans are fine.
    for (CullGroup* g : cullGroups) {
        for (size_t i = 0; i < g->members.size(); ++i) {
            if (g->members[i] == obj) {
                g->members.erase(g->members.begin() + i);
                break;
            }
        }
    }
    obj->groupCulled = false;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i] == obj) {
            objects.erase(objects.begin() + i);
            statsDirty = true;      // MESHPUNK: see getStatistics
            return true;
        }
    }
    return false;
}

// MESHPUNK: see the CullGroup docs in Scene.hpp.
Scene::CullGroup* Scene::addCullGroup(const std::vector<Object*>& members) {
    if (members.empty()) return nullptr;
    // Union of each member's rotation-proof bound: centre + centreVolume,
    // half-extent = longest AABB dimension (the same conservative convention
    // as every sphere test in this file, so rotation cannot invalidate it).
    int64_t mnx = INT64_MAX, mny = INT64_MAX, mnz = INT64_MAX;
    int64_t mxx = INT64_MIN, mxy = INT64_MIN, mxz = INT64_MIN;
    for (Object* m : members) {
        if (!m) continue;
        const int64_t ext = std::max({
            m->boundingBoxMax.x - m->boundingBoxMin.x,
            m->boundingBoxMax.y - m->boundingBoxMin.y,
            m->boundingBoxMax.z - m->boundingBoxMin.z});
        const int64_t cx = (int64_t)m->position.x + m->centreVolume.x;
        const int64_t cy = (int64_t)m->position.y + m->centreVolume.y;
        const int64_t cz = (int64_t)m->position.z + m->centreVolume.z;
        mnx = std::min(mnx, cx - ext); mxx = std::max(mxx, cx + ext);
        mny = std::min(mny, cy - ext); mxy = std::max(mxy, cy + ext);
        mnz = std::min(mnz, cz - ext); mxz = std::max(mxz, cz + ext);
    }
    if (mnx > mxx) return nullptr;   // every entry was null

    CullGroup* g = new CullGroup();
    g->members.reserve(members.size());
    for (Object* m : members) if (m) g->members.push_back(m);
    g->centre = Vector3((int32_t)((mnx + mxx) / 2),
                        (int32_t)((mny + mxy) / 2),
                        (int32_t)((mnz + mxz) / 2));
    const float hx = (float)(mxx - mnx) * 0.5f;
    const float hy = (float)(mxy - mny) * 0.5f;
    const float hz = (float)(mxz - mnz) * 0.5f;
    const float r  = sqrtf(hx * hx + hy * hy + hz * hz);
    g->radius = (r < 2147483000.0f) ? (int32_t)r : INT32_MAX;
    cullGroups.push_back(g);
    return g;
}

// MESHPUNK
bool Scene::removeCullGroup(CullGroup* g) {
    for (size_t i = 0; i < cullGroups.size(); ++i) {
        if (cullGroups[i] == g) {
            for (Object* m : g->members) m->groupCulled = false;
            cullGroups.erase(cullGroups.begin() + i);
            delete g;
            return true;
        }
    }
    return false;
}

// MESHPUNK: world-space sphere vs frustum — the same view transform and
// signed-distance plane tests as cullObject's quick path, minus the
// object-specific parts. True = fully outside one half-space.
bool Scene::cullSphereWorld(const Vector3& worldCentre, int32_t radius,
                            int32_t camCosX, int32_t camSinX,
                            int32_t camCosY, int32_t camSinY,
                            int32_t camCosZ, int32_t camSinZ) const {
    const float r = (float)radius;
    constexpr float invFps = 1.0f / (float)FIXED_POINT_SCALE;
    const float cYc = (float)camCosY * invFps, cYs = (float)camSinY * invFps;
    const float cXc = (float)camCosX * invFps, cXs = (float)camSinX * invFps;
    const float cZc = (float)camCosZ * invFps, cZs = (float)camSinZ * invFps;
    const float px = (float)(worldCentre.x - camera->position.x);
    const float py = (float)(worldCentre.y - camera->position.y);
    const float pz = (float)(worldCentre.z - camera->position.z);
    // Camera rotation Y, X, Z — the same order as cullObject.
    const float t1x =  px * cYc + pz * cYs;
    const float t1z = -px * cYs + pz * cYc;
    const float t2y =  py * cXc - t1z * cXs;
    const float t2z =  py * cXs + t1z * cXc;
    const float cxv =  t1x * cZc - t2y * cZs;
    const float cyv =  t1x * cZs + t2y * cZc;
    const float czv =  t2z;

    if (czv + r < (float)camera->nearPlane) return true;   // fully behind
    if (czv - r > (float)camera->farPlane)  return true;   // fully beyond

    const float fovFactor = camera->fovFactor;
    const float hw  = (float)screenWidth  * 0.5f;
    const float hh  = (float)screenHeight * 0.5f;
    const float rLh = r * cullPlaneLh;
    const float rLv = r * cullPlaneLv;
    const float dR =  cxv * fovFactor - czv * hw;
    const float dL = -cxv * fovFactor - czv * hw;
    const float dT =  cyv * fovFactor - czv * hh;
    const float dB = -cyv * fovFactor - czv * hh;
    return dR > rLh || dL > rLh || dT > rLv || dB > rLv;
}

// MESHPUNK
void Scene::setFog(int32_t nearDist, int32_t farDist) {
    if (!renderer) return;
    if (farDist <= nearDist) { clearFog(); return; }
    renderer->fogNear = nearDist;
    renderer->fogFar  = farDist;
}

// MESHPUNK
void Scene::clearFog() {
    if (!renderer) return;
    renderer->fogNear = INT32_MAX;
    renderer->fogFar  = INT32_MAX;
}

void Scene::addPointLight(PointLight* light) {
    pointLights.push_back(light);
}

void Scene::setCamera(Camera* cam) {
    camera = cam;
    renderer->camera = cam;
}

void Scene::setDirectionalLight(DirectionalLight* light) {
    directionalLight = light;
}

void Scene::setAmbientLight(AmbientLight* light) {
    ambientLight = light;
}

#if MAX_PICK_QUERIES > 0
void Scene::setPickQueries(const PickQuery* queries, int count) {
    if (count < 0) count = 0;
    if (count > MAX_PICK_QUERIES) count = MAX_PICK_QUERIES;
    pickQueryCount = count;
    for (int i = 0; i < count; ++i) {
        pickQueries[i] = queries[i];
    }
    // Slots beyond `count` keep their previous contents but are ignored
    // by the rasterizer (it loops 0..pickQueryCount). We don't bother
    // zeroing them.
}
#endif

// Exact per-channel average of two RGB565 pixels, rounding each channel down.
// 0xF7DE clears the low bit of every channel (bit 11 red, bit 5 green, bit 0
// blue) so the shared halves can be added back without carrying between them.
// Four ALU ops instead of an unpack/average/repack per channel.
static inline uint16_t jet_avg565(uint16_t a, uint16_t b) {
    return (uint16_t)((a & b) + (((a ^ b) & 0xF7DE) >> 1));
}

void PERF_CRITICAL Scene::reconstructCheckerboard(uint16_t* fbBase,
                                                  int py0, int py1) {
    // Fill every pixel the rasteriser skipped this frame with the average of
    // its two horizontal neighbours, both of which were rendered this frame.
    //
    // Purely spatial on purpose. The pixel's own previous value is NOT a tap:
    // under band rendering one band buffer is reused for every band in the
    // frame, so whatever sits in an unrendered pixel is the PREVIOUS BAND's
    // content, not this pixel's last frame. Reading it would comb the image.
    // Dropping the temporal tap is what makes checkerboard work with bands
    // where Jet's row interlacing cannot.
    //
    // A separate pass over the finished band, so painter's-algorithm overdraw
    // cannot average a gap pixel repeatedly.
    //
    // Runs before drawSprites() and the overlay: both write the framebuffer
    // directly rather than through the rasteriser, so they are not
    // checkerboarded and must not be blurred by this.
    if (!renderer || !renderer->checkerboardMode) return;

    const uint16_t* const fbb = fbBase ? fbBase : framebuffer;
    const int cbParity = renderEvenLines ? 0 : 1;
    const int yStart   = (py0 >= 0) ? py0 : renderer->yBandMin;
    const int yEnd     = std::min((py1 >= 0) ? py1 : renderer->yBandMax,
                                  screenHeight);

    // Coverage gate (see Rasterizer::cbMask): rebuild an off-parity pixel
    // ONLY when the pixel loop drew one of its horizontal neighbours this
    // frame — those are the pixels the loop's stride actually skipped.
    // Everything else (flat spans, the sky clear) wrote both parities and
    // must be left exactly as drawn; averaging over those halftoned every
    // vertical colour boundary in the scene ("waves on all vertical lines",
    // hw 2026-08-03). No mask (allocation failed) = no reconstruction: a
    // stale pixel is a lesser artifact than a corrupted drawn one.
    const uint8_t* mask = renderer->cbMask;
    if (!mask) return;
    const uint32_t halfW = (uint32_t)screenWidth >> 1;

    for (int y = yStart; y < yEnd; ++y) {
        uint16_t* row = (uint16_t*)fbb + y * screenWidth;
        const uint32_t rowBit = (uint32_t)y * halfW;

        // Bit for the drawn-parity pixel at column dx (same flat index the
        // rasteriser used when setting it).
        auto drawn = [&](int dx) -> bool {
            const uint32_t ci = rowBit + ((uint32_t)dx >> 1);
            return (mask[ci >> 3] >> (ci & 7u)) & 1u;
        };

        // Missing pixels are those where ((x ^ y) & 1) != cbParity, i.e.
        // (x & 1) == ((y & 1) ^ (1 - cbParity)). Walking them with a stride of
        // two costs no per-pixel branch at all.
        const int x0 = ((y & 1) ^ (1 - cbParity)) & 1;

        int x = x0;
        if (x == 0) {                       // left edge: only a right neighbour
            if (drawn(1)) row[0] = row[1];
            x = 2;
        }
        const int xLast = screenWidth - 1;
        for (; x < xLast; x += 2) {
            const bool dl = drawn(x - 1), dr = drawn(x + 1);
            if (dl & dr)      row[x] = jet_avg565(row[x - 1], row[x + 1]);
            else if (dl)      row[x] = row[x - 1];
            else if (dr)      row[x] = row[x + 1];
        }
        if (x == xLast) {                   // right edge: only a left neighbour
            if (drawn(xLast - 1)) row[xLast] = row[xLast - 1];
        }
    }
}

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// clearBuffers() row fill using 128-bit EE.VST.128.XP stores (4 × uint32
// per instruction). dest must be 16-byte aligned; n32 need not be a
// multiple of 4 — trailing elements are handled with scalar stores.
static void jet_fill_u32x16(uint32_t* dest, uint32_t val, int n32) {
    const int n16 = n32 >> 2;
    if (n16 > 0) {
        __asm__ volatile (
            "ee.movi.32.q q0, %[v], 0\n\t"
            "ee.movi.32.q q0, %[v], 1\n\t"
            "ee.movi.32.q q0, %[v], 2\n\t"
            "ee.movi.32.q q0, %[v], 3\n\t"
            : : [v] "r"(val)
        );
        // EE.VST.128.XP takes the post-increment stride as a register,
        // not a bare immediate: as += stride_reg after each 128-bit store.
        // 4x-unrolled: the volatile asm + memory clobber stops GCC from
        // unrolling this itself, so retire 64 bytes per loop iteration by
        // hand and mop up the remainder one store at a time.
        const int stride = 16;
        for (int i = n16 >> 2; i > 0; --i) {
            __asm__ volatile (
                "ee.vst.128.xp q0, %[p], %[s]\n\t"
                "ee.vst.128.xp q0, %[p], %[s]\n\t"
                "ee.vst.128.xp q0, %[p], %[s]\n\t"
                "ee.vst.128.xp q0, %[p], %[s]\n\t"
                : [p] "+r"(dest) : [s] "r"(stride) : "memory"
            );
        }
        for (int i = n16 & 3; i > 0; --i) {
            __asm__ volatile (
                "ee.vst.128.xp q0, %[p], %[s]\n\t"
                : [p] "+r"(dest) : [s] "r"(stride) : "memory"
            );
        }
    }
    for (int r = n32 & 3; r-- > 0; ) *dest++ = val;
}
#endif

void PERF_CRITICAL Scene::clearBuffers() {
    //cast the framebuffer to a 32-bit pointer
    uint32_t* framebuffer32 = (uint32_t*)framebuffer;

    // Wireframe mode forces a solid black background regardless of the
    // configured backcolor / gradient so the outlines pop. We detour the
    // gradient and backcolor for the duration of this clear and restore
    // them on the way out so the host-visible state is unchanged.
    uint16_t* savedGradient = backgroundGradientColors;
    uint16_t  savedBack     = backcolor;
    const bool wireClear = (renderer && renderer->wireframeMode);
    if (wireClear) {
        backgroundGradientColors = nullptr;
        backcolor = 0;
    }

    if (clearRenderBuffer) {
        // Checkerboard mode takes the full clear in the final branch below, not
        // a parity-selective one. reconstructCheckerboard() overwrites every
        // opposite-parity pixel from its horizontal neighbours before the band
        // is blitted, so nothing ever reads what the clear left in those pixels.
        // A 32-bit run fill is much cheaper than a per-pixel parity test, and
        // it removes the dependency on a persistent previous-frame buffer that
        // band rendering cannot provide.
        if (renderer->interlacedMode) {
            // Band scoping, same as the non-interlaced branch below: with
            // virtual-base-pointer band rendering the buffer only covers
            // [yBandMin, yBandMax), so walking the full height writes past it.
            const int yIlEnd = renderer ? std::min(renderer->yBandMax, screenHeight)
                                        : screenHeight;
            int yIlStart = renderer ? renderer->yBandMin : 0;
            // Keep the field parity the rasteriser draws: rows where
            // (y & 1) == renderEvenLines. Band starts are even, so this only
            // ever nudges by one.
            if ((yIlStart & 1) != ((int)renderEvenLines & 1)) ++yIlStart;
            for (int y = yIlStart; y < yIlEnd; y += 2) {
                #if DEBUG_OVERDRAW
                uint16_t lineColor = 0;
                uint32_t lineColor32 = 0;
                #else
                uint16_t lineColor = backgroundGradientColors ? backgroundGradientColors[y] : backcolor;
                uint32_t lineColor32 = (lineColor << 16) | lineColor;
                #endif
                #if HALF_WIDTH_BUFFERS
                const int divisor = 4;
                #else
                const int divisor = 2;
                #endif
                #if FIELD_BUFFERS
                // Field-buffer layout: packed half-height. Row index = y>>1.
                uint32_t* lineStart = framebuffer32 + (y >> 1) * (screenWidth / divisor);
                #else
                uint32_t* lineStart = framebuffer32 + y * (screenWidth / divisor);
                #endif
#if defined(CONFIG_IDF_TARGET_ESP32S3)
                jet_fill_u32x16(lineStart, lineColor32, screenWidth / divisor);
#else
                for (int x = 0; x < screenWidth / divisor; x++) {
                    lineStart[x] = lineColor32;
                }
#endif
                #if Z_BUFFERING
                #if HALF_WIDTH_BUFFERS
                memset(zBuffer + (y / 2) * (screenWidth / 2), 0xFF, (screenWidth / 2) * sizeof(uint16_t));
                #else
                memset(zBuffer + y * screenWidth, 0xFF, screenWidth * sizeof(uint16_t));
                #endif
                #endif
            }
        } else {
            // Non-interlaced clear: fill every row. Honour the per-row
            // background gradient if one is set; otherwise fall back to
            // a solid backcolor. Note that `memset(fb, backcolor, ...)` is
            // wrong for a 16-bit backcolor because memset writes bytes —
            // we'd get backcolor's low byte duplicated. Pack two pixels per
            // 32-bit store and walk rows so the gradient (if any) actually
            // shows up.
            // HALF_WIDTH_BUFFERS makes the buffer half-width. FIELD_BUFFERS
            // additionally halves the height (each field buffer covers every other
            // row). When only HALF_WIDTH_BUFFERS is set the buffer is half-width
            // but FULL-height, so rowCount must be screenHeight in that case.
            #if HALF_WIDTH_BUFFERS
            const int rowPixels = screenWidth / 2;
            #if FIELD_BUFFERS
            const int rowCount  = screenHeight / 2;
            #else
            const int rowCount  = screenHeight;
            #endif
            #else
            const int rowPixels = screenWidth;
            const int rowCount  = screenHeight;
            #endif
            const int row32     = rowPixels / 2; // pairs of pixels per row
            // Respect yBandMin/yBandMax so the clear only touches the rows
            // assigned to this band (critical for virtual-base-pointer band
            // rendering where the buffer only covers [yBandMin, yBandMax)).
            const int yClearStart = renderer ? renderer->yBandMin             : 0;
            const int yClearEnd   = renderer ? std::min(renderer->yBandMax, rowCount) : rowCount;
            for (int y = yClearStart; y < yClearEnd; ++y) {
                #if DEBUG_OVERDRAW
                const uint16_t lineColor = 0;
                #else
                // Source row in the gradient table maps 1:1 with the
                // logical screen row (`screenHeight`). When the back buffer
                // is half-height (HALF_WIDTH_BUFFERS implies a y/2-style
                // layout in some configs), index by `y` directly because
                // rowCount already accounts for the halving.
                const uint16_t lineColor = backgroundGradientColors
                                           ? backgroundGradientColors[y * (screenHeight / rowCount)]
                                           : backcolor;
                #endif
                const uint32_t lineColor32 = ((uint32_t)lineColor << 16) | lineColor;
                uint32_t* lineStart = framebuffer32 + y * row32;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
                jet_fill_u32x16(lineStart, lineColor32, row32);
#else
                for (int x = 0; x < row32; ++x) lineStart[x] = lineColor32;
#endif
            }
            #if Z_BUFFERING
            // Z-buffer stride matches the rasterizer's depth-buffer layout
            // (see ZBUFFER_STRIDE in Renderer.hpp): half-width when
            // HALF_WIDTH_BUFFERS is on, per-pixel otherwise. Height is the
            // full screen height regardless.
            #if HALF_WIDTH_BUFFERS
            memset(zBuffer, 0xFF, (size_t)(screenWidth / 2) * screenHeight * sizeof(uint16_t));
            #else
            memset(zBuffer, 0xFF, (size_t)screenWidth * screenHeight * sizeof(uint16_t));
            #endif
            #endif
        }
    }
    //memset(scanlinesUpdated, 0, screenHeight * sizeof(bool));

    // Restore the host-visible background state regardless of whether we
    // detoured it for the wireframe-mode clear above.
    if (wireClear) {
        backgroundGradientColors = savedGradient;
        backcolor = savedBack;
    }
}

void Scene::prepareFrame() {
    prepareFrameBegin();
    prepareObjectSlice(0, 1);
    prepareFrameEnd();
}

void Scene::prepareFrameBegin() {
    if (!camera) return;
    // MESHPUNK: reset the rasterised counter for the frame.
    //
    // rasterizeBand() used to ASSIGN this, which was correct when render()
    // called it once for the whole screen — but a banded host calls it once per
    // band, so the value reported was whichever band ran last, not the frame.
    // That made "queued 216, rasterised 13" look like 94% waste when it was
    // comparing a frame total against one band of eight.
    lastFrameRasterizedTriangles = 0;
    lastFrameTriangleCalls       = 0;
    g_jetPxSpan = 0;
    g_jetPxLoop = 0;
    // renderEvenLines drives the frame-parity selection used by both interlaced
    // and checkerboard modes.  In interlaced mode it selects which rows to draw;
    // in checkerboard mode it selects which (x+y) pixel parity to draw.  When
    // neither mode is active we force it to false so drawTriangle's
    // `yStart = interlacedMode ? (minY + renderEvenLines) : minY` never shifts
    // the first scanline by a stray ±1.
    renderEvenLines = (renderer->interlacedMode || renderer->checkerboardMode)
                      ? (frameCounter % 2 == 0)
                      : false;
    // Checkerboard coverage mask: allocate HERE, the single-threaded point
    // of the frame — renderBandTo runs concurrently and must never race an
    // allocation. (clearBand keeps its own lazy alloc for the serial path.)
    if (renderer->checkerboardMode && !renderer->cbMask) {
        const size_t rowBytes = (size_t)(screenWidth / 2) / 8;
        renderer->cbMask = (uint8_t*)calloc(rowBytes * (size_t)screenHeight, 1);
    }
    clearBuffers();

#if MAX_PICK_QUERIES > 0
    // Reset pick results for this frame and hand the arrays to the
    // rasterizer. With FIELD_BUFFERS the rasterizer only writes one
    // parity of rows per frame, so a query landing on the "off" parity
    // would never be tested by drawTriangle's per-row pick loop. Snap
    // the y to a row this frame's field actually covers so the host's
    // requested pixel still produces a hit on roughly the right
    // location (off by one row at most). The snapped y is what gets
    // stored in PickResult.y so the caller can render their cursor on
    // the same row the renderer inspected.
    for (int i = 0; i < pickQueryCount; ++i) {
        pickResults[i] = PickResult{};
    #if FIELD_BUFFERS
        if (renderer->interlacedMode && pickQueries[i].y >= 0) {
            const int desiredParity = renderEvenLines ? 0 : 1;
            int sy = pickQueries[i].y;
            if ((sy & 1) != desiredParity) {
                // Prefer nudging up; clamp to a valid row at the bottom.
                if (sy + 1 < screenHeight) sy += 1; else if (sy > 0) sy -= 1;
            }
            pickQueries[i].y = (int16_t)sy;
        }
    #endif
    }
    renderer->pickQueries     = pickQueries;
    renderer->pickResults     = pickResults;
    renderer->pickQueryCount  = pickQueryCount;
#endif

#if LIGHTING
    if (directionalLight) directionalLight->updateViewSpaceDirection(camera);
#endif

    // Thread sky gradient and frame counter into the rasterizer so
    // WATER_REFLECT shading can sample them. Intentionally outside
    // #if LIGHTING — works with LIGHTING=0 on firmware.
    renderer->gradientColors = backgroundGradientColors;
    renderer->gradientSize   = backgroundGradientColors ? screenHeight : 0;
    renderer->frameCounter   = frameCounter;
    renderer->waterTime      = waterTime;

    int32_t camCosX, camSinX, camCosY, camSinY, camCosZ, camSinZ;
    camera->getRotationMatrix(camCosX, camSinX, camCosY, camSinY, camCosZ, camSinZ);

    // Frustum side-plane normal lengths for cullObject's quick sphere
    // test. fovFactor changes at runtime (boost FOV kick) so refresh per
    // frame rather than caching at init.
    {
        const float f  = camera->fovFactor;
        const float hw = (float)screenWidth  * 0.5f;
        const float hh = (float)screenHeight * 0.5f;
        cullPlaneLh = sqrtf(f * f + hw * hw);
        cullPlaneLv = sqrtf(f * f + hh * hh);
    }

    // Compute the screen row of the water/sky horizon from the camera's pitch.
    // Derivation: a world point at the water horizon (infinite Z along forward)
    // projects to sy = screenH/2 + camSinX * fovFactor / FIXED_POINT_SCALE.
    // WATER_REFLECT uses mirrorY = 2*waterlineY - y so reflections align
    // correctly for both near (low on screen) and distant (high on screen) objects.
    renderer->waterlineY = screenHeight / 2
                         + (int)(camSinX * camera->fovFactor / 1024.0f);

    // MESHPUNK: the aggregation gate (see CullGroup in Scene.hpp). One sphere
    // test per group; a group fully outside the frustum stamps every member
    // groupCulled so the walk below drops them at a single bool load each —
    // no centre transform, no 8-corner cull, no fade math. Runs after the
    // cullPlaneLh/Lv refresh above, which cullSphereWorld reads.
    for (CullGroup* g : cullGroups) {
        const bool out = cullSphereWorld(g->centre, g->radius,
                                         camCosX, camSinX,
                                         camCosY, camSinY,
                                         camCosZ, camSinZ);
        for (Object* m : g->members) m->groupCulled = out;
    }

    renderQueue.clear();
    triYExtent.clear();          // MESHPUNK: parallel to renderQueue
    renderQueueW.clear();
    triYExtentW.clear();
    lastFrameDroppedTriangles = 0;
    prepDrawn[0] = prepDrawn[1] = 0;
    prepEmitting[0] = prepEmitting[1] = 0;

    // MESHPUNK: balance the two prepare slices by PREDICTED work.
    // The old index-parity split anti-correlated with skyloop's bake order
    // (detail sector, then its coarse twin, alternating), which put every
    // heavy sector on slice 0 and every twin on slice 1 — hw instrumentation
    // showed s0 at 3-6x s1 on every heavy frame. Greedy assign-to-lighter-
    // bin, weighted by last frame's OUTCOME: an object that actually emitted
    // costs its full mesh walk; one the per-object culls skipped (every skip
    // site zeroes lastEmitted) costs only its tests. Source-count weighting
    // alone left 5-20ms wait spikes whenever frustum culling fell
    // asymmetrically across the slices. Runs single-threaded here; slices
    // read the tags concurrently. Mispredictions (an object newly entering
    // the frustum) imbalance one frame, then converge.
    objSliceTag.resize(objects.size());
    {
        size_t load[2] = { 0, 0 };
        for (size_t i = 0; i < objects.size(); ++i) {
            Object* o = objects[i];
            size_t w = 1;
            if (o->enabled && !o->groupCulled)
                w = (o->lastEmitted > 0) ? o->triangles.size() + 8 : 2;
            const int s = (load[1] < load[0]) ? 1 : 0;
            objSliceTag[i] = (uint8_t)s;
            load[s] += w;
        }
    }
}  // end prepareFrameBegin()

// The object walk, strided so two slices can run on different cores at
// once. The camera trig is recomputed per slice (trivial, deterministic)
// rather than stashed, so the slices need nothing from Begin but the
// cleared buffers.
void Scene::prepareObjectSlice(int slot, int stride) {
    int32_t camCosX, camSinX, camCosY, camSinY, camCosZ, camSinZ;
    camera->getRotationMatrix(camCosX, camSinX, camCosY, camSinY,
                              camCosZ, camSinZ);
    int drawnObjs = 0, emittingObjs = 0;

    const int objN = (int)objects.size();
    for (int oi = 0; oi < objN; ++oi) {
        // Abandoned prepare (frontend timeout): the worker slice stops
        // before its next object so its emissions can never race the next
        // frame's Begin clearing these queues. See Scene.hpp prepAbort.
        if (slot != 0 && prepAbort) break;
        // stride 1 = the serial path takes everything; stride 2 = the
        // parallel path follows the per-frame balance tags Begin computed
        // (see objSliceTag) instead of index parity.
        if (stride > 1 && objSliceTag[oi] != slot) continue;
        Object* obj = objects[oi];
        if (!obj->enabled || obj->groupCulled) continue;
        // 1) Quick sphere far-cull before the expensive 8-corner AABB test.
        //    distSq to the object centre is computed unconditionally so it
        //    is also available for the fade ramps and LOD pick below,
        //    replacing the old lazy-compute block. The conservative sphere
        //    radius used is the object's longest bounding-box dimension
        //    (always >= the true bounding-sphere radius — never drops a
        //    visible object).
        uint8_t objAlpha = 255;
        const int32_t _ocx = (obj->position.x + obj->centreVolume.x) - camera->position.x;
        const int32_t _ocy = (obj->position.y + obj->centreVolume.y) - camera->position.y;
        const int32_t _ocz = (obj->position.z + obj->centreVolume.z) - camera->position.z;
        int64_t distSq = (int64_t)_ocx*_ocx + (int64_t)_ocy*_ocy + (int64_t)_ocz*_ocz;
        int32_t dist   = -1;
        {
            const int32_t maxExtent = std::max({
                obj->boundingBoxMax.x - obj->boundingBoxMin.x,
                obj->boundingBoxMax.y - obj->boundingBoxMin.y,
                obj->boundingBoxMax.z - obj->boundingBoxMin.z});
            const int64_t farCutoff = static_cast<int64_t>(camera->farPlane) + maxExtent;
            if (distSq > farCutoff * farCutoff) { obj->lastEmitted = 0; continue; }
        }
        // 2) Object-level AABB frustum cull (all 8 corners; full rotation).
        if (cullObject(obj, camCosX, camSinX, camCosY, camSinY, camCosZ, camSinZ)) {
            obj->lastEmitted = 0;   // keeps the slice-balance weights honest
            continue;
        }
        // 3) Per-object distance fade (two ramps, multiplied):
        //     - fadeFar > 0:   close=opaque, far=invisible (decor fade-out).
        //     - appearFar > 0: close=invisible, far=opaque (LOD impostor
        //                      that pops in at distance).
        //     Both can be combined on the same object if you want a
        //     visibility "band" — opaque only between two distances.
        //     fadeFar==0 / appearFar==0 disable the respective ramp.
        //     Distance is measured in world space from the camera to the
        //     object's centre (position + centreVolume). Beyond fadeFar
        //     OR closer than appearNear the object is skipped entirely
        //     (no transform, no per-tri work), so this is a real perf
        //     win not just a visual fade.
        // distSq is always valid here (computed above for the sphere pre-cull).
        if (obj->fadeFar > 0 || obj->appearFar > 0) {
            if (obj->fadeFar > 0) {
                const int64_t farSq = (int64_t)obj->fadeFar * obj->fadeFar;
                if (distSq >= farSq) { obj->lastEmitted = 0; continue; } // fully past fade-out
                const int64_t nearSq = (int64_t)obj->fadeNear * obj->fadeNear;
                if (distSq > nearSq && obj->fadeFar > obj->fadeNear) {
                    if (dist < 0) dist = (int32_t)sqrtf((float)distSq);
                    const int32_t span = obj->fadeFar - obj->fadeNear;
                    const int32_t over = dist - obj->fadeNear;
                    int32_t a = 255 - (over * 255) / span;
                    if (a < 0) a = 0;
                    if (a > 255) a = 255;
                    objAlpha = (uint8_t)((objAlpha * a) / 255);
                }
            }

            // Appear-in ramp (LOD impostor).
            if (obj->appearFar > 0) {
                const int64_t nearSq = (int64_t)obj->appearNear * obj->appearNear;
                if (distSq <= nearSq) { obj->lastEmitted = 0; continue; } // still too close
                const int64_t farSq = (int64_t)obj->appearFar * obj->appearFar;
                if (distSq < farSq && obj->appearFar > obj->appearNear) {
                    if (dist < 0) dist = (int32_t)sqrtf((float)distSq);
                    const int32_t span = obj->appearFar - obj->appearNear;
                    const int32_t over = dist - obj->appearNear;
                    int32_t a = (over * 255) / span;
                    if (a < 0) a = 0;
                    if (a > 255) a = 255;
                    objAlpha = (uint8_t)((objAlpha * a) / 255);
                }
                // distSq >= farSq: fully appeared, multiplier already 255.
            }

            if (objAlpha == 0) { obj->lastEmitted = 0; continue; }
        }

        // 1c) Global LOD pick. The head Object IS LOD 0; entries in
        //     obj->lodMeshes are LOD 1, 2, ... in order. Beyond the last
        //     available LOD: cull (default) or clamp (`lodPersist`).
        //     The picked Object* contributes ONLY mesh data; the head's
        //     transform / flags / AABB / fade ramps still drive the draw.
        Object* meshSource = obj;
        obj->lastLodLevel = 0;   // MESHPUNK: reported via obj:lodlevel()
        if (lodScale > 0) {
            if (dist < 0 && distSq >= 0) dist = (int32_t)sqrtf((float)distSq);
            int32_t level = (dist < 0 ? 0 : dist / lodScale);
            level += (int32_t)lodBias + (int32_t)obj->lodBias;
            if (level < 0) level = 0;

            const int availableLODs = (int)obj->lodMeshes.size();
            if (level == 0) {
                meshSource = obj;
            } else if (level <= availableLODs) {
                Object* candidate = obj->lodMeshes[level - 1];
                meshSource = candidate ? candidate : obj;
                obj->lastLodLevel = (int)level;
            } else if (obj->lodPersist) {
                if (availableLODs > 0) {
                    Object* candidate = obj->lodMeshes[availableLODs - 1];
                    meshSource = candidate ? candidate : obj;
                    obj->lastLodLevel = availableLODs;
                }
                // else: no LOD chain at all, draw the head as-is.
            } else {
                obj->lastLodLevel = -1;
                obj->lastEmitted = 0;
                continue; // ran out of LODs and not persisting → cull.
            }
        }

        // 2) Transform + project + per-triangle cull, push into this slot's
        //    emit buffers.
        renderObject(obj, camCosX, camSinX, camCosY, camSinY, camCosZ, camSinZ,
                     objAlpha, meshSource, slot);
        ++drawnObjs;
        // MESHPUNK: an object can survive the AABB cull, pay the full ~27us of
        // per-object setup, and then contribute NOTHING — every triangle
        // off-screen or sub-pixel. Counting that here is free (renderObject
        // just set lastEmitted) and separates "too many objects in the world"
        // from "the object cull stopped filtering usefully".
        if (obj->lastEmitted > 0) ++emittingObjs;
    }
    prepDrawn[slot ? 1 : 0]    = drawnObjs;
    prepEmitting[slot ? 1 : 0] = emittingObjs;
}  // end prepareObjectSlice()

void Scene::prepareFrameEnd() {
    lastFrameDrawnObjects    = prepDrawn[0] + prepDrawn[1];
    lastFrameEmittingObjects = prepEmitting[0] + prepEmitting[1];
    lastFrameDrawnTriangles  = static_cast<int>(renderQueue.size()
                                              + renderQueueW.size());

    // 3) Global painter's sort. Three bands:
    //      0. noWriteZBuffer  — drawn first, so later geometry paints over
    //                           them (e.g. skyboxes).
    //      1. Normal          — main scene, back-to-front by effective Z.
    //      2. ignoreZBuffer   — drawn last, unconditionally on top.
    //
    // Within the normal band, the sort key is `avgZ - zBias * zBiasScale`.
    // Bigger zBias pulls a triangle toward the camera in the sort, so it
    // draws later than coplanar geometry without that bias. The scale has
    // to be large enough to beat within-triangle avgZ variation on
    // typical world-scale geometry (~hundreds of units) so decals reliably
    // win coplanar fights, but not so large that a biased decal draws
    // *over* geometry that's genuinely much closer (where avgZ differences
    // are thousands). Tune if the scale of the world changes significantly.
    constexpr int32_t zBiasScale = 256;
    // MESHPUNK: EXACT LSD radix sort of the painter's order, replacing the
    // K-bucket counting sort that stood here.
    //
    // Why exact. The bucket sort's width was the resolution limit of the whole
    // painter's order: any two triangles closer in depth than one bucket drew in
    // queue order instead of depth order. That is what let the demo rocket's fin
    // bars — cube parts driven through a cylinder body in a single baked mesh —
    // paint over the body. Widening 64 -> 256 buckets cut the worst-case
    // inversion from 14.3 to 3.4 authored units and made it better but not gone,
    // because where those parts interpenetrate the depth difference is smaller
    // still. A radix sort has NO bucket: it is exact at every depth.
    //
    // It is also strictly better than the per-object std::sort this build
    // dropped (SORT_TRIANGLES 0), which only ever ordered within one object and
    // left cross-object ties to bucket order.
    //
    // Shape: the depth keys are lifted ONCE into compact 4-byte arrays, so the
    // sort's passes never touch the 132-byte RenderTri structs again; only
    // indices and keys move. Keys are normalised as (maxKey - key), which is
    // non-negative and makes ASCENDING order mean farthest-first, so no sign
    // handling is needed and equal depths keep queue order (LSD radix is
    // stable). The pass count adapts to the key range actually present — a
    // typical scene spans well under 16 bits and takes two passes; the loop
    // covers the full 32 bits if a scene ever needs it.
    //
    // 8-bit digits rather than the 11 I first proposed: for the ranges this
    // engine sees both need the same TWO passes, but 8 bits costs 1KB of
    // counters and 256 prefix steps per pass instead of 8KB and 2048.
    //
    // As before, only 4-byte INDICES are scattered into renderOrder; the
    // RenderTri entries stay where push_back left them.
    {
        // Composite indices over BOTH emit buffers: idx < n0 reads the
        // primary queue, otherwise renderQueueW[idx - n0]. rasterizeBand
        // resolves indices the same way, so nothing is ever copied or
        // merged — the sort output is the only shared ordering.
        const int n0 = static_cast<int>(renderQueue.size());
        const int N  = n0 + static_cast<int>(renderQueueW.size());
        auto triAt = [&](int32_t i) -> const RenderTri& {
            return i < n0 ? renderQueue[i] : renderQueueW[i - n0];
        };
        renderOrder.resize(N);
        if (N > 1) {
            // Pass 1: size the three draw bands.
            int band0N = 0, band2N = 0;
            for (int32_t i = 0; i < N; ++i) {
                const RenderTri& t = triAt(i);
                if (t.noWriteZBuffer)      band0N++;
                else if (t.ignoreZBuffer)  band2N++;
            }
            const int normalN = N - band0N - band2N;

            radixKeyA.resize(normalN); radixKeyB.resize(normalN);
            radixIdxA.resize(normalN); radixIdxB.resize(normalN);

            // Pass 2: emit the two unsorted bands straight into their slots and
            // gather the normal band's keys, tracking the range as we go.
            int b0 = 0;
            int b2 = band0N + normalN;
            int m  = 0;
            int32_t minKey = INT32_MAX, maxKey = INT32_MIN;
            for (int32_t i = 0; i < N; ++i) {
                const RenderTri& t = triAt(i);
                if (t.noWriteZBuffer) { renderOrder[b0++] = i; continue; }
                if (t.ignoreZBuffer)  { renderOrder[b2++] = i; continue; }
                const int32_t key = t.sortZ - static_cast<int32_t>(t.zBias) * zBiasScale;
                radixKeyA[m] = static_cast<uint32_t>(key);
                radixIdxA[m] = i;
                if (key < minKey) minKey = key;
                if (key > maxKey) maxKey = key;
                ++m;
            }

            uint32_t* keyA = radixKeyA.data();
            uint32_t* keyB = radixKeyB.data();
            int32_t*  idxA = radixIdxA.data();
            int32_t*  idxB = radixIdxB.data();

            // A zero range means every triangle sits at the same depth, so queue
            // order is already the stable answer and the passes would be no-ops.
            const uint64_t range = (normalN > 1)
                ? static_cast<uint64_t>(static_cast<int64_t>(maxKey) - minKey)
                : 0;
            if (range > 0) {
                for (int j = 0; j < normalN; ++j) {
                    keyA[j] = static_cast<uint32_t>(
                        static_cast<int64_t>(maxKey) - static_cast<int32_t>(keyA[j]));
                }
                int bits = 0;
                while ((range >> bits) != 0) ++bits;   // bits the range needs

                constexpr int RB    = 8;
                constexpr int RSIZE = 1 << RB;
                constexpr uint32_t RMASK = RSIZE - 1;
                int counts[RSIZE];

                for (int shift = 0; shift < bits; shift += RB) {
                    memset(counts, 0, sizeof(counts));
                    for (int j = 0; j < normalN; ++j)
                        ++counts[(keyA[j] >> shift) & RMASK];
                    int sum = 0;
                    for (int d = 0; d < RSIZE; ++d) {
                        const int c = counts[d];
                        counts[d] = sum;
                        sum += c;
                    }
                    for (int j = 0; j < normalN; ++j) {
                        const int p = counts[(keyA[j] >> shift) & RMASK]++;
                        keyB[p] = keyA[j];
                        idxB[p] = idxA[j];
                    }
                    uint32_t* tk = keyA; keyA = keyB; keyB = tk;
                    int32_t*  ti = idxA; idxA = idxB; idxB = ti;
                }
            }

            for (int j = 0; j < normalN; ++j) renderOrder[band0N + j] = idxA[j];
        } else if (N == 1) {
            renderOrder[0] = 0;
        }
    }

    // MESHPUNK: per-band bins over the final draw order (see Scene.hpp).
    // Two passes over renderOrder — count, prefix-sum, scatter — walking the
    // 4-byte extent arrays sequentially, so building the bins costs one queue
    // walk where consuming them saves one PER BAND. Scattering in draw order
    // keeps every bin depth-sorted for free. Single-threaded here; read-only
    // by the time bands run on both cores.
    bandBinsValid = false;
    {
        const int rows = bandRowsHint;
        const bool extentsOk = (triYExtent.size() == renderQueue.size())
                            && (triYExtentW.size() == renderQueueW.size());
        if (rows > 0 && extentsOk) {
            const int32_t n0v = static_cast<int32_t>(renderQueue.size());
            const int bandCount = (screenHeight + rows - 1) / rows;
            bandBinStart.assign(bandCount + 1, 0);
            const int32_t lastRow = screenHeight - 1;
            auto extAt = [&](int32_t idx) {
                return idx < n0v ? triYExtent[idx] : triYExtentW[idx - n0v];
            };
            for (const int32_t idx : renderOrder) {
                const int32_t ext = extAt(idx);
                if (yExtentMax(ext) < 0 || yExtentMin(ext) > lastRow) continue;
                const int b0 = std::max<int32_t>(yExtentMin(ext), 0) / rows;
                const int b1 = std::min<int32_t>(yExtentMax(ext), lastRow) / rows;
                for (int b = b0; b <= b1; ++b) ++bandBinStart[b + 1];
            }
            for (int b = 0; b < bandCount; ++b)
                bandBinStart[b + 1] += bandBinStart[b];
            bandBinEntries.resize(bandBinStart[bandCount]);
            bandBinCursor.assign(bandBinStart.begin(),
                                 bandBinStart.end() - 1);
            for (const int32_t idx : renderOrder) {
                const int32_t ext = extAt(idx);
                if (yExtentMax(ext) < 0 || yExtentMin(ext) > lastRow) continue;
                const int b0 = std::max<int32_t>(yExtentMin(ext), 0) / rows;
                const int b1 = std::min<int32_t>(yExtentMax(ext), lastRow) / rows;
                for (int b = b0; b <= b1; ++b)
                    bandBinEntries[bandBinCursor[b]++] = idx;
            }
            bandBinsValid = true;
        }
    }
}  // end prepareFrameEnd()

// See the header. Reentrant by construction: everything is either read-only
// shared state (gradient table, render queue, materials) or local to this
// band's rows (bandBuf, the mask's row range).
void Scene::renderBandTo(uint16_t* bandBuf, int y0, int y1) {
    if (!renderer || y1 <= y0) return;
    if (y1 > screenHeight) y1 = screenHeight;

    // Band-local clear: gradient/backcolor rows straight into the buffer.
    // Wireframe mode's black detour is honoured without touching the shared
    // gradient pointer (the clearBuffers version temporarily swaps it, which
    // would race a concurrent band).
    const bool wire = renderer->wireframeMode;
    const int row32 = screenWidth / 2;
    for (int y = y0; y < y1; ++y) {
        const uint16_t c = wire ? 0
                         : (backgroundGradientColors ? backgroundGradientColors[y]
                                                     : backcolor);
        const uint32_t c32 = ((uint32_t)c << 16) | c;
        uint32_t* line = (uint32_t*)(bandBuf + (size_t)(y - y0) * screenWidth);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        jet_fill_u32x16(line, c32, row32);
#else
        for (int x = 0; x < row32; ++x) line[x] = c32;
#endif
    }

    // Checkerboard mask rows (allocated by prepareFrame; see cbMask).
    if (renderer->checkerboardMode && renderer->cbMask) {
        const size_t rowBytes = (size_t)(screenWidth / 2) / 8;
        memset(renderer->cbMask + (size_t)y0 * rowBytes, 0,
               (size_t)(y1 - y0) * rowBytes);
    }

    uint16_t* const base = bandBuf - (size_t)y0 * screenWidth;
    rasterizeBand(y0, y1, base);
    reconstructCheckerboard(base, y0, y1);
    drawSprites(base, y0, y1);
}

void Scene::clearBand(int yMin, int yMax) {
    if (!renderer) return;
    renderer->yBandMin = yMin;
    renderer->yBandMax = yMax;
    // Checkerboard coverage mask (see Rasterizer::cbMask): lazily allocated,
    // this band's rows cleared before the rasteriser marks its writes.
    if (renderer->checkerboardMode) {
        const size_t rowBytes = (size_t)(screenWidth / 2) / 8;
        if (!renderer->cbMask)
            renderer->cbMask = (uint8_t*)calloc(rowBytes * (size_t)screenHeight, 1);
        if (renderer->cbMask) {
            const int y0 = yMin < 0 ? 0 : yMin;
            const int y1 = yMax > screenHeight ? screenHeight : yMax;
            if (y1 > y0)
                memset(renderer->cbMask + (size_t)y0 * rowBytes, 0,
                       (size_t)(y1 - y0) * rowBytes);
        }
    }
    clearBuffers();
}

void Scene::rasterizeBand(int yMin, int yMax, uint16_t* fbBase) {
    // Create a thread-local copy of the rasteriser so each band worker has
    // its own yBandMin/yBandMax. Only framebuffer/zbuffer ptrs are shared;
    // writes go to non-overlapping y regions so there is no write race.
    // fbBase overrides the shared framebuffer on the COPY only, which is
    // what lets two cores rasterise different bands into different buffers.
    Rasterizer bandRast = *renderer;
    bandRast.yBandMin = yMin;
    bandRast.yBandMax = yMax;
    if (fbBase) bandRast.setFramebuffer(fbBase);

    // 4) Flush. Count triangles that actually entered the rasterizer
    // (drawTriangle returned true). Triangles dropped by per-tri checks
    // inside drawTriangle (alpha=0, zero-area, near/far Z, degenerate
    // denom) return false and don't count toward the rasterized total.
    int rasterized = 0;
    int calls = 0;
    // MESHPUNK: the extent arrays are only valid when they were filled
    // alongside their queues this frame; fall back to testing everything if
    // anything got out of step, so a stale or partial array can never
    // silently drop geometry. Indices are COMPOSITE over both prepare
    // slices' buffers (see prepareFrameEnd).
    const int32_t n0 = static_cast<int32_t>(renderQueue.size());
    const bool useYPreTest = (triYExtent.size() == renderQueue.size())
                          && (triYExtentW.size() == renderQueueW.size());
    const int32_t bandLast = yMax - 1;

    // Per-band bin fast path (see prepareFrameEnd): a call aligned to the
    // hint grid and spanning one band iterates exactly the triangles binned
    // for it — depth-ordered, pre-test already implied by membership. Any
    // other call shape (full-screen render(), odd ranges) takes the walk.
    const int32_t* idxPtr = renderOrder.data();
    const int32_t* idxEnd = idxPtr + renderOrder.size();
    bool preTest = useYPreTest;
    if (bandBinsValid && bandRowsHint > 0
        && yMin % bandRowsHint == 0 && yMax <= yMin + bandRowsHint) {
        const int b = yMin / bandRowsHint;
        if (b >= 0 && b + 1 < static_cast<int>(bandBinStart.size())) {
            idxPtr  = bandBinEntries.data() + bandBinStart[b];
            idxEnd  = bandBinEntries.data() + bandBinStart[b + 1];
            preTest = false;
        }
    }

    for (; idxPtr < idxEnd; ++idxPtr) {
        const int32_t idx = *idxPtr;
        // Reject out-of-band triangles on one 4-byte read, BEFORE touching the
        // ~136-byte RenderTri. This is the whole point of the array — see
        // Scene.hpp. Everything the rasteriser would do to a rejected triangle
        // is skipped, and a kept triangle pays only this extra compare.
        if (preTest) {
            const int32_t ext = idx < n0 ? triYExtent[idx]
                                         : triYExtentW[idx - n0];
            if (yExtentMax(ext) < yMin || yExtentMin(ext) > bandLast) continue;
        }
        ++calls;
        const RenderTri& t = idx < n0 ? renderQueue[idx]
                                      : renderQueueW[idx - n0];
#if MAX_PICK_QUERIES > 0
        bandRast.currentPickObject        = t.sourceObject;
        bandRast.currentPickTriangleIndex = t.sourceTriangleIndex;
#endif
        // t.avgZ rides along as the FAST_Z depth hint: emitTri computed the
        // same three-vertex average and already culled it against near/far,
        // so drawTriangle skips both the recompute and the redundant test.
        if (bandRast.drawTriangle(t.v1, t.v2, t.v3, t.material,
                                   directionalLight, ambientLight,
                                   renderEvenLines,
                                   t.ignoreZBuffer, t.noWriteZBuffer,
                                   (int)t.zBias, t.objAlpha,
                                   t.brightnessPrecomputed, t.avgZ,
                                   // MESHPUNK: the baked colour travels with
                                   // the triangle, not through the shared
                                   // material it was queued against.
                                   t.colorBaked ? (int32_t)t.bakedColor : -1)) {
            ++rasterized;
        }
    }
    // MESHPUNK: ACCUMULATE across bands (prepareFrame zeroes it). Note this
    // counts triangle-BAND draws, not distinct triangles — a triangle spanning
    // three bands counts three times. That is the right unit for cost, because
    // drawTriangle runs once per band it touches.
    // Atomic: two cores accumulate concurrently under renderBandTo.
    __atomic_add_fetch(&lastFrameRasterizedTriangles, rasterized,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&lastFrameTriangleCalls, calls, __ATOMIC_RELAXED);
}

void Scene::render() {
    if (!camera) return;
    prepareFrame();
    rasterizeBand(0, screenHeight);

    // Checkerboard reconstruction: fill in opposite-parity pixels from the
    // freshly-rendered current-parity neighbours so PostFX sees a fully-populated
    // buffer. Done as a separate pass after all triangles are drawn so that
    // painter's algorithm overdraw cannot cause repeated averaging.
    #if defined(CHECKERBOARD_RECONSTRUCTION) && CHECKERBOARD_RECONSTRUCTION
    if (renderer->checkerboardMode) {
        reconstructCheckerboard();
    }
    #endif

    #if POSTFX_ANTIALIASING
    postFX->applyFXAA(framebuffer);
    #endif

    #if POSTFX_BLOOM
    postFX->applyBloom(framebuffer);
    #endif

    #if POSTFX_CRT
    postFX->applyCRT(framebuffer);
    #endif

    #if POSTFX_PIXELATE
    postFX->applyPixelate(framebuffer);
    #endif

    #if POSTFX_CHROMATIC
    postFX->applyChromatic(framebuffer);
    #endif

    #if POSTFX_MOTION_BLUR
    postFX->applyMotionBlur(framebuffer);
    #endif

    drawSprites();

    frameCounter++;
}

void Scene::addSprite(Sprite2D* sprite) {
    if (sprite) sprites.push_back(sprite);
}

// ---------------------------------------------------------------------------
// drawSprites — called at end of render(), after PostFX, before frameCounter++
// ---------------------------------------------------------------------------
// Blends each registered Sprite2D onto the framebuffer in zOrder sequence.
// Two paths:
//   textured  — iterate source rows, colour-key skip, optional alpha blend
//   solid     — span fill with material->color, optional alpha blend
//
// Alpha compositing uses a fast 5-bit RGB565 lerp:
//   out = src + ((dst - src) * inv_alpha >> 5)
// which is exact at 0 and 255 and has ≤1 LSB error at mid values.
// ---------------------------------------------------------------------------
void Scene::drawSprites(uint16_t* fbBase, int py0, int py1) {
    // On HALF_WIDTH_BUFFERS builds, sprites are composited at full output
    // resolution during Display::pushFrame() scanout instead — writing them
    // into the half-width framebuffer here would halve their horizontal
    // resolution and use the wrong stride.
#if HALF_WIDTH_BUFFERS
    (void)fbBase; (void)py0; (void)py1;
    return;
#else
    if (sprites.empty()) return;
    uint16_t* const fb = fbBase ? fbBase : framebuffer;

    // Sort by zOrder each frame (list is typically tiny — insertion sort
    // territory, but std::stable_sort keeps it simple and correct).
    std::stable_sort(sprites.begin(), sprites.end(),
        [](const Sprite2D* a, const Sprite2D* b){ return a->zOrder < b->zOrder; });

    for (Sprite2D* sp : sprites) {
        if (!sp || !sp->enabled || !sp->material) continue;
        const uint8_t matAlpha = sp->material->alpha;
        // Combined alpha: 0 = invisible, 255 = opaque.
        const int combined = ((int)sp->alpha * (int)matAlpha) / 255;
        if (combined == 0) continue;

        const Texture* tex = sp->material->diffuseMap;
        const bool opaque  = (combined == 255);

        // Clip dest rect to framebuffer bounds.
        const int srcW = tex ? tex->width  : sp->width;
        const int srcH = tex ? tex->height : sp->height;
        if (srcW <= 0 || srcH <= 0) continue;
        const int dstW = srcW * sp->scale;
        const int dstH = srcH * sp->scale;

        const int x0 = sp->x, y0 = sp->y;
        const int x1 = x0 + dstW, y1 = y0 + dstH;
        // Clipped dest rect
        // MESHPUNK: clip vertically to the rasteriser's current band, not to the
        // whole screen. Band rendering never materialises a full frame — the
        // framebuffer pointer is a virtual base covering only
        // [yBandMin, yBandMax) — so a screen-height clip would write outside the
        // band buffer. Row addressing (framebuffer + dy * screenWidth) is
        // already correct against that virtual base, so this is the only change
        // needed to make drawSprites() safe to call once per band.
        const int bandLo = (py0 >= 0) ? py0
                         : (renderer ? renderer->yBandMin : 0);
        const int bandHiRaw = (py1 >= 0) ? py1
                            : (renderer ? renderer->yBandMax : screenHeight);
        const int bandHi = bandHiRaw < screenHeight ? bandHiRaw : screenHeight;
        const int dx0 = (x0 < 0) ? 0 : x0;
        const int dy0 = (y0 < bandLo) ? bandLo : y0;
        const int dx1 = (x1 > screenWidth) ? screenWidth : x1;
        const int dy1 = (y1 > bandHi) ? bandHi : y1;
        if (dx0 >= dx1 || dy0 >= dy1) continue;

        // Source start offsets for the unscaled (scale==1) path.
        const int sx0 = dx0 - x0;
        const int sy0 = dy0 - y0;

        if (tex && sp->scale > 1) {
            // ---- Nearest-neighbour upscale textured blit (scale > 1) ---------
            // fp8 step: advances source coord by (1/scale) per output pixel.
            const int xStep = (srcW << 8) / dstW;
            const int yStep = (srcH << 8) / dstH;
            const bool colorKey = tex->hasAlpha;
            const uint16_t keyColor = tex->alphaColor;
            const uint16_t* src = tex->data;
            int sf_y = (dy0 - y0) * yStep;
            for (int dy = dy0; dy < dy1; ++dy, sf_y += yStep) {
                const int sy  = sf_y >> 8;   // nearest-neighbour: integer texel
                uint16_t* dstRow = fb + dy * screenWidth + dx0;
                int sf_x = (dx0 - x0) * xStep;
                const int w = dx1 - dx0;
                for (int i = 0; i < w; ++i, sf_x += xStep) {
                    const int sx  = sf_x >> 8;
                    const uint16_t s = src[sy * srcW + sx];
                    if (colorKey && s == keyColor) continue;
                    const int sr = (s >> 11) & 0x1F;
                    const int sg = (s >>  5) & 0x3F;
                    const int sb =  s        & 0x1F;
                    if (sp->blendMode == BlendMode::BLEND_ADD) {
                        const uint16_t d = dstRow[i];
                        int ar=((d>>11)&0x1F)+sr; if(ar>0x1F)ar=0x1F;
                        int ag=((d>>5) &0x3F)+sg; if(ag>0x3F)ag=0x3F;
                        int ab=(d      &0x1F)+sb; if(ab>0x1F)ab=0x1F;
                        dstRow[i]=(uint16_t)((ar<<11)|(ag<<5)|ab);
                    } else if (combined == 255) {
                        dstRow[i]=(uint16_t)((sr<<11)|(sg<<5)|sb);
                    } else {
                        const uint16_t d = dstRow[i];
                        const int inv=255-combined;
                        const int dr=(d>>11)&0x1F,dg=(d>>5)&0x3F,db=d&0x1F;
                        dstRow[i]=(uint16_t)(
                            (((sr*combined+dr*inv)/255)<<11)|
                            (((sg*combined+dg*inv)/255)<<5)|
                             ((sb*combined+db*inv)/255));
                    }
                }
            }
        } else if (tex) {
            // ---- Textured sprite blit ----------------------------------------
            const bool colorKey = tex->hasAlpha;
            const uint16_t keyColor = tex->alphaColor;
            const uint16_t* src = tex->data;

            if (sp->blendMode == BlendMode::BLEND_ADD) {
                // Saturating additive: dst = clamp(dst + src). No alpha scaling.
                for (int dy = dy0; dy < dy1; ++dy) {
                    const int sy = sy0 + (dy - dy0);
                    const uint16_t* srcRow = src + sy * tex->width + sx0;
                    uint16_t*       dstRow = fb + dy * screenWidth + dx0;
                    const int w = dx1 - dx0;
                    for (int i = 0; i < w; ++i) {
                        const uint16_t s = srcRow[i];
                        if (colorKey && s == keyColor) continue;
                        const uint16_t d = dstRow[i];
                        const int sr = (s >> 11) & 0x1F;
                        const int sg = (s >>  5) & 0x3F;
                        const int sb =  s        & 0x1F;
                        int ar = ((d >> 11) & 0x1F) + sr; if (ar > 0x1F) ar = 0x1F;
                        int ag = ((d >>  5) & 0x3F) + sg; if (ag > 0x3F) ag = 0x3F;
                        int ab = ( d        & 0x1F) + sb; if (ab > 0x1F) ab = 0x1F;
                        dstRow[i] = (uint16_t)((ar << 11) | (ag << 5) | ab);
                    }
                }
            } else {
                for (int dy = dy0; dy < dy1; ++dy) {
                    const int sy = sy0 + (dy - dy0);
                    const uint16_t* srcRow = src + sy * tex->width + sx0;
                    uint16_t*       dstRow = fb + dy * screenWidth + dx0;
                    const int w = dx1 - dx0;

                    if (opaque) {
                        if (colorKey) {
                            for (int i = 0; i < w; ++i) {
                                if (srcRow[i] != keyColor) dstRow[i] = srcRow[i];
                            }
                        } else {
                            for (int i = 0; i < w; ++i) dstRow[i] = srcRow[i];
                        }
                    } else {
                        // 5-bit lerp: decompose RGB565 into R5/G6/B5 channels,
                        // lerp each independently, repack.
                        const int inv = 255 - combined;
                        for (int i = 0; i < w; ++i) {
                            const uint16_t s = srcRow[i];
                            if (colorKey && s == keyColor) continue;
                            const uint16_t d = dstRow[i];
                            const int sr = (s >> 11) & 0x1F;
                            const int sg = (s >>  5) & 0x3F;
                            const int sb =  s        & 0x1F;
                            const int dr = (d >> 11) & 0x1F;
                            const int dg = (d >>  5) & 0x3F;
                            const int db =  d        & 0x1F;
                            const int or_ = (sr * combined + dr * inv) / 255;
                            const int og  = (sg * combined + dg * inv) / 255;
                            const int ob  = (sb * combined + db * inv) / 255;
                            dstRow[i] = (uint16_t)((or_ << 11) | (og << 5) | ob);
                        }
                    }
                }
            }
        } else {
            // ---- Solid rectangle fill ----------------------------------------
            const uint16_t col = sp->material->color;

            if (sp->blendMode == BlendMode::BLEND_ADD) {
                // Additive: add colour directly, no alpha scaling.
                const int acr = (col >> 11) & 0x1F;
                const int acg = (col >>  5) & 0x3F;
                const int acb =  col        & 0x1F;
                for (int dy = dy0; dy < dy1; ++dy) {
                    uint16_t* row = fb + dy * screenWidth + dx0;
                    const int w = dx1 - dx0;
                    for (int i = 0; i < w; ++i) {
                        const uint16_t d = row[i];
                        int ar = ((d >> 11) & 0x1F) + acr; if (ar > 0x1F) ar = 0x1F;
                        int ag = ((d >>  5) & 0x3F) + acg; if (ag > 0x3F) ag = 0x3F;
                        int ab = ( d        & 0x1F) + acb; if (ab > 0x1F) ab = 0x1F;
                        row[i] = (uint16_t)((ar << 11) | (ag << 5) | ab);
                    }
                }
            } else {
                if (opaque) {
                    for (int dy = dy0; dy < dy1; ++dy) {
                        uint16_t* row = fb + dy * screenWidth + dx0;
                        const int w = dx1 - dx0;
                        for (int i = 0; i < w; ++i) row[i] = col;
                    }
                } else {
                    const int inv = 255 - combined;
                    const int cr = (col >> 11) & 0x1F;
                    const int cg = (col >>  5) & 0x3F;
                    const int cb =  col        & 0x1F;
                    for (int dy = dy0; dy < dy1; ++dy) {
                        uint16_t* row = fb + dy * screenWidth + dx0;
                        const int w = dx1 - dx0;
                        for (int i = 0; i < w; ++i) {
                            const uint16_t d = row[i];
                            const int dr = (d >> 11) & 0x1F;
                            const int dg = (d >>  5) & 0x3F;
                            const int db =  d        & 0x1F;
                            const int or_ = (cr * combined + dr * inv) / 255;
                            const int og  = (cg * combined + dg * inv) / 255;
                            const int ob  = (cb * combined + db * inv) / 255;
                            row[i] = (uint16_t)((or_ << 11) | (og << 5) | ob);
                        }
                    }
                }
            }
        }
    }
#endif // !HALF_WIDTH_BUFFERS
}

void Scene::getStatistics(int& objectCount, int& triangleCount, int& vertexCount) {
    // MESHPUNK: cached. The walk touches two span sizes inside EVERY object,
    // i.e. a scattered read across every 216-byte Object in PSRAM. A HUD that
    // calls this once a frame therefore costs real time at world scale —
    // measured ~1,500us/frame at 1,278 objects, more than the entire Lua
    // callback used to cost. The totals only change when objects are added,
    // removed, or enabled/disabled, and all three set statsDirty.
    if (statsDirty) {
        statTriangles = 0;
        statVertices  = 0;
        for (const auto& obj : objects) {
            if (!obj->enabled) continue;
            statTriangles += static_cast<int>(obj->triangles.size());
            statVertices  += static_cast<int>(obj->vertices.size());
        }
        statsDirty = false;
    }
    objectCount   = static_cast<int>(objects.size());
    triangleCount = statTriangles;
    vertexCount   = statVertices;
}

// MESHPUNK: .iram.text like drawTriangle (upstream marks this PERF_CRITICAL
// = IRAM on ESP builds). This is the prepare hot loop BOTH cores run
// concurrently; fetching it from internal SRAM takes its instruction
// traffic off the PSRAM bus the slices already saturate with data. Needs
// Scene.cpp in build.ps1's $literals_in_text so l32r literals land inside
// the section (verify the audit after any edit here).
#define JET_HOT_SCENE __attribute__((section(".iram.text")))
void JET_HOT_SCENE PERF_CRITICAL Scene::renderObject(Object* obj,
                                     int32_t camCosX, int32_t camSinX,
                                     int32_t camCosY, int32_t camSinY,
                                     int32_t camCosZ, int32_t camSinZ,
                                     uint8_t objAlpha,
                                     Object* meshSource,
                                     int emitSlot) {
    // Slot-selected emit buffers: slot 1 is the concurrent prepare slice's
    // private pair, so two renderObject calls on different cores never
    // touch the same vector (see prepareObjectSlice).
    std::vector<RenderTri>& emitQ   = emitSlot ? renderQueueW : renderQueue;
    std::vector<int32_t>&   emitExt = emitSlot ? triYExtentW  : triYExtent;
    Material* const         bakedMat = &s_bakedMat[emitSlot ? 1 : 0];
    // meshSource decouples "which mesh do we rasterise" from "where / how
    // does the object live in the world". Defaults to obj itself, so the
    // non-LOD path is unchanged. When the global LOD system picks a
    // reduced-detail mesh, that Object* is passed in here while obj keeps
    // ownership of the transform, flags, AABB and fade state.
    if (!meshSource) meshSource = obj;

    // Reusable scratch buffers — kept across calls so we don't pay for a
    // heap alloc per object per frame. ONE PAIR PER EMIT SLOT: the two
    // prepare slices run this function on different cores concurrently, and
    // the single-static version this replaced would have had them writing
    // each other's vertex transforms. RenderVertex (not Object::Vertex):
    // only the fields the configured pipeline consumes are carried, and the
    // transform loop below writes every live field, so no upfront copy of
    // the source vertex array is needed at all.
    static std::vector<RenderVertex> s_txVerts[2];
    static std::vector<Vector3>      s_camPos[2];
    std::vector<RenderVertex>& transformedVertices = s_txVerts[emitSlot ? 1 : 0];
    std::vector<Vector3>&      camSpacePos         = s_camPos[emitSlot ? 1 : 0];
    const size_t vertCount = meshSource->vertices.size();
    transformedVertices.resize(vertCount);
    // Parallel array of camera-space positions (pre-projection). Needed so
    // triangles straddling the near plane can be clipped geometrically —
    // otherwise a single vertex slipping behind the near plane would force
    // the whole triangle to be discarded, leaving a visible hole in the
    // world right under the camera.
    camSpacePos.resize(vertCount);

    Vector3 camPos(camera->position);
    #if FLOAT_CAMERA_ANGLES
    Vector3_f camRotF(camera->rotation);
    #endif
    Vector3 objPos(obj->position);

    float   fovFactor = camera->fovFactor;
    bool isBillboard = obj->isBillboard;
    CullingMode cullingMode = obj->cullingMode;
    bool ignoreZBuffer = obj->ignoreZBuffer;
    bool noWriteZBuffer = obj->noWriteZBuffer;

    // Hoist object rotation trig out of the per-vertex loop — these are
    // constant for every vertex on the object. Also short-circuit the entire
    // rotation block when the object has zero rotation (true for most static
    // scenery), saving 12 mul + 6 div + 9 add per vertex.
    const bool objHasRotation = !isBillboard &&
        (obj->useMatrix ||
         obj->rotation.x != 0 || obj->rotation.y != 0 || obj->rotation.z != 0);

    // Composed object rotation matrix (Rz * Ry * Rx, since the previous
    // per-vertex code applied X→Y→Z). Storing all 9 entries at
    // FIXED_POINT_SCALE scale lets the per-vertex transform collapse from
    // three cascaded rotations (12 muls + 12 div per vertex) into a single
    // 3×3 mat-vec (9 muls + 3 div), with the same again saved on the
    // normal when LIGHTING is on. The trig product entries are pre-divided
    // by FIXED_POINT_SCALE so the entire matrix lives at one consistent
    // scale.
    int32_t objM00=FIXED_POINT_SCALE, objM01=0, objM02=0;
    int32_t objM10=0, objM11=FIXED_POINT_SCALE, objM12=0;
    int32_t objM20=0, objM21=0, objM22=FIXED_POINT_SCALE;
    // Float counterparts pre-divided by FIXED_POINT_SCALE. Per-vertex mat-vecs
    // use hardware-FPU fmul-add instead of int64_t multiply+shift. Defaults
    // are identity; vertex loop guards on objHasRotation so they're never
    // consumed when false.
    float fObjM00=1.0f, fObjM01=0.0f, fObjM02=0.0f;
    float fObjM10=0.0f, fObjM11=1.0f, fObjM12=0.0f;
    float fObjM20=0.0f, fObjM21=0.0f, fObjM22=1.0f;
    if (obj->useMatrix) {
        // MESHPUNK: rig-bone path — the override matrix is already the
        // composed object rotation at unit scale. Mirror it into the int32
        // form so any consumer of either representation stays consistent.
        fObjM00 = obj->matrix[0]; fObjM01 = obj->matrix[1]; fObjM02 = obj->matrix[2];
        fObjM10 = obj->matrix[3]; fObjM11 = obj->matrix[4]; fObjM12 = obj->matrix[5];
        fObjM20 = obj->matrix[6]; fObjM21 = obj->matrix[7]; fObjM22 = obj->matrix[8];
        objM00 = (int32_t)(fObjM00 * FIXED_POINT_SCALE);
        objM01 = (int32_t)(fObjM01 * FIXED_POINT_SCALE);
        objM02 = (int32_t)(fObjM02 * FIXED_POINT_SCALE);
        objM10 = (int32_t)(fObjM10 * FIXED_POINT_SCALE);
        objM11 = (int32_t)(fObjM11 * FIXED_POINT_SCALE);
        objM12 = (int32_t)(fObjM12 * FIXED_POINT_SCALE);
        objM20 = (int32_t)(fObjM20 * FIXED_POINT_SCALE);
        objM21 = (int32_t)(fObjM21 * FIXED_POINT_SCALE);
        objM22 = (int32_t)(fObjM22 * FIXED_POINT_SCALE);
    } else if (objHasRotation) {
        const int32_t cx = lookupCosI(obj->rotation.x);
        const int32_t sx = lookupSinI(obj->rotation.x);
        const int32_t cy = lookupCosI(obj->rotation.y);
        const int32_t sy = lookupSinI(obj->rotation.y);
        const int32_t cz = lookupCosI(obj->rotation.z);
        const int32_t sz = lookupSinI(obj->rotation.z);
        // K = Ry * Rx (at FPS scale; trig-product entries divided once).
        const int32_t k00 = cy;
        const int32_t k01 = (int32_t)((int64_t)sy * sx / FIXED_POINT_SCALE);
        const int32_t k02 = (int32_t)((int64_t)sy * cx / FIXED_POINT_SCALE);
        const int32_t k10 = 0;
        const int32_t k11 = cx;
        const int32_t k12 = -sx;
        const int32_t k20 = -sy;
        const int32_t k21 = (int32_t)((int64_t)cy * sx / FIXED_POINT_SCALE);
        const int32_t k22 = (int32_t)((int64_t)cy * cx / FIXED_POINT_SCALE);
        // M = Rz * K (at FPS scale).
        objM00 = (int32_t)(((int64_t)cz * k00 - (int64_t)sz * k10) / FIXED_POINT_SCALE);
        objM01 = (int32_t)(((int64_t)cz * k01 - (int64_t)sz * k11) / FIXED_POINT_SCALE);
        objM02 = (int32_t)(((int64_t)cz * k02 - (int64_t)sz * k12) / FIXED_POINT_SCALE);
        objM10 = (int32_t)(((int64_t)sz * k00 + (int64_t)cz * k10) / FIXED_POINT_SCALE);
        objM11 = (int32_t)(((int64_t)sz * k01 + (int64_t)cz * k11) / FIXED_POINT_SCALE);
        objM12 = (int32_t)(((int64_t)sz * k02 + (int64_t)cz * k12) / FIXED_POINT_SCALE);
        objM20 = k20;
        objM21 = k21;
        objM22 = k22;
        fObjM00=(float)objM00/FIXED_POINT_SCALE; fObjM01=(float)objM01/FIXED_POINT_SCALE; fObjM02=(float)objM02/FIXED_POINT_SCALE;
        fObjM10=(float)objM10/FIXED_POINT_SCALE; fObjM11=(float)objM11/FIXED_POINT_SCALE; fObjM12=(float)objM12/FIXED_POINT_SCALE;
        fObjM20=(float)objM20/FIXED_POINT_SCALE; fObjM21=(float)objM21/FIXED_POINT_SCALE; fObjM22=(float)objM22/FIXED_POINT_SCALE;
    }

    // Composed camera rotation matrix (Rz * Rx * Ry, since the previous
    // per-vertex code applied Y→X→Z). Same scheme as the object matrix.
    // Computed per object render rather than once per scene because the
    // call cost is negligible (~9 muls / 9 divs) compared to the per-
    // vertex savings; hoisting to renderScene would shave nine muls per
    // *object*, not per vertex.
    int32_t camM00, camM01, camM02;
    int32_t camM10, camM11, camM12;
    int32_t camM20, camM21, camM22;
    {
        const int32_t cx = camCosX, sx = camSinX;
        const int32_t cy = camCosY, sy = camSinY;
        const int32_t cz = camCosZ, sz = camSinZ;
        // K = Rx * Ry
        const int32_t k00 = cy;
        const int32_t k01 = 0;
        const int32_t k02 = sy;
        const int32_t k10 = (int32_t)((int64_t)sx * sy / FIXED_POINT_SCALE);
        const int32_t k11 = cx;
        const int32_t k12 = (int32_t)(-(int64_t)sx * cy / FIXED_POINT_SCALE);
        const int32_t k20 = (int32_t)(-(int64_t)cx * sy / FIXED_POINT_SCALE);
        const int32_t k21 = sx;
        const int32_t k22 = (int32_t)((int64_t)cx * cy / FIXED_POINT_SCALE);
        // M = Rz * K
        camM00 = (int32_t)(((int64_t)cz * k00 - (int64_t)sz * k10) / FIXED_POINT_SCALE);
        camM01 = (int32_t)(((int64_t)cz * k01 - (int64_t)sz * k11) / FIXED_POINT_SCALE);
        camM02 = (int32_t)(((int64_t)cz * k02 - (int64_t)sz * k12) / FIXED_POINT_SCALE);
        camM10 = (int32_t)(((int64_t)sz * k00 + (int64_t)cz * k10) / FIXED_POINT_SCALE);
        camM11 = (int32_t)(((int64_t)sz * k01 + (int64_t)cz * k11) / FIXED_POINT_SCALE);
        camM12 = (int32_t)(((int64_t)sz * k02 + (int64_t)cz * k12) / FIXED_POINT_SCALE);
        camM20 = k20;
        camM21 = k21;
        camM22 = k22;
    }
    // Float camera matrix: pre-divided by FIXED_POINT_SCALE once per object.
    const float fCamM00=(float)camM00/FIXED_POINT_SCALE, fCamM01=(float)camM01/FIXED_POINT_SCALE, fCamM02=(float)camM02/FIXED_POINT_SCALE;
    const float fCamM10=(float)camM10/FIXED_POINT_SCALE, fCamM11=(float)camM11/FIXED_POINT_SCALE, fCamM12=(float)camM12/FIXED_POINT_SCALE;
    const float fCamM20=(float)camM20/FIXED_POINT_SCALE, fCamM21=(float)camM21/FIXED_POINT_SCALE, fCamM22=(float)camM22/FIXED_POINT_SCALE;

    // Combined object→camera transform, composed ONCE per object:
    //   p_cam = Cam · (Obj·p + objPos − camPos) = (Cam·Obj)·p + Cam·(objPos − camPos)
    // so the per-vertex work collapses from two 3×3 mat-vecs plus two
    // vector adds down to a single mat-vec with the translation folded
    // into the accumulate. fM is also the combined rotation for normals
    // (both factors are pure rotations; translation is excluded there).
    // For unrotated objects (most scenery) fM is just the camera matrix.
    float fM00, fM01, fM02, fM10, fM11, fM12, fM20, fM21, fM22;
    if (objHasRotation) {
        fM00 = fCamM00*fObjM00 + fCamM01*fObjM10 + fCamM02*fObjM20;
        fM01 = fCamM00*fObjM01 + fCamM01*fObjM11 + fCamM02*fObjM21;
        fM02 = fCamM00*fObjM02 + fCamM01*fObjM12 + fCamM02*fObjM22;
        fM10 = fCamM10*fObjM00 + fCamM11*fObjM10 + fCamM12*fObjM20;
        fM11 = fCamM10*fObjM01 + fCamM11*fObjM11 + fCamM12*fObjM21;
        fM12 = fCamM10*fObjM02 + fCamM11*fObjM12 + fCamM12*fObjM22;
        fM20 = fCamM20*fObjM00 + fCamM21*fObjM10 + fCamM22*fObjM20;
        fM21 = fCamM20*fObjM01 + fCamM21*fObjM11 + fCamM22*fObjM21;
        fM22 = fCamM20*fObjM02 + fCamM21*fObjM12 + fCamM22*fObjM22;
    } else {
        fM00 = fCamM00; fM01 = fCamM01; fM02 = fCamM02;
        fM10 = fCamM10; fM11 = fCamM11; fM12 = fCamM12;
        fM20 = fCamM20; fM21 = fCamM21; fM22 = fCamM22;
    }
    const float fDx = (float)(objPos.x - camPos.x);
    const float fDy = (float)(objPos.y - camPos.y);
    const float fDz = (float)(objPos.z - camPos.z);
    const float fTx = fCamM00*fDx + fCamM01*fDy + fCamM02*fDz;
    const float fTy = fCamM10*fDx + fCamM11*fDy + fCamM12*fDz;
    const float fTz = fCamM20*fDx + fCamM21*fDy + fCamM22*fDz;

#if LIGHTING
    // Object-local lighting precompute eligibility.
    //
    // The view-space normal transform exists only so that the rasterizer
    // can evaluate the view-facing specular term (`N.z < 0` in
    // jetShadeBrightness). For objects whose materials are ALL non-
    // specular (`material->specular == 0`), there is no consumer of the
    // view-space frame — diffuse Lambert is rotation-invariant — so we
    // can skip the per-vertex normal transform entirely and instead
    // transform the world-space light direction into the object's local
    // space ONCE per object, then dot it against the untransformed
    // vertex normal to get brightness. That brightness is cached on the
    // vertex and read straight back out by drawTriangle, which also
    // skips its own jetShadeBrightness call when the flag is set.
    //
    // On the ESP32 with FLAT shading + non-specular materials being the
    // common case (track, ground, parapets, hulls), this deletes 9 muls
    // + 3 shifts per vertex plus the dot/clamp/square cycle from the
    // renderer's per-triangle path — replaced by a single 9-mul + 3-shift
    // light-direction transform amortized over the whole object.
    bool objectLocalLight = false;
    Vector3 objLightDir = {0, 0, 0};
    uint16_t objLightIntensity = 0;
    uint8_t  objDiffuseCoef = 255;
    if (directionalLight && !meshSource->triangles.empty()) {
        bool allNonSpecular = true;
        // Per-triangle material walk. Cheap — a handful of byte loads
        // per face — and exits early on the first specular material we
        // hit so specular-heavy objects (vehicles with shiny paint)
        // skip the rest of the scan. If we ever start caching this on
        // the Object itself we can drop the walk entirely; for now the
        // overhead is well below the savings on qualifying objects.
        //
        // Also disqualifies PHONG materials: PHONG interpolates per-
        // pixel from the vertex normals against directionalLight->lightDir
        // (view-space) and there's no place to feed cached brightness
        // back in. FLAT and GOURAUD both consume a per-triangle/vertex
        // scalar brightness so they slot the cache in cleanly.
        for (const auto& tri : meshSource->triangles) {
            if (!tri.material) continue;
            if (tri.material->specular != 0 ||
                tri.material->shadingMode == ShadingMode::PHONG) {
                allNonSpecular = false;
                break;
            }
        }
        if (allNonSpecular) {
            objectLocalLight = true;
            objLightIntensity = directionalLight->intensity;
            // Diffuse coef varies per triangle; we use the first triangle's
            // material as a representative. If diffuse coefs were ever
            // mixed across an object's faces, GOURAUD-style midtones
            // would shift slightly compared to the renderer's path. In
            // practice diffuse is a per-shading-style constant on every
            // material in this project (255 default).
            const Material* m0 = meshSource->triangles[0].material;
            if (m0) objDiffuseCoef = m0->diffuse;

            // Transform worldLightDir into object-local space. With M
            // composed as Rz*Ry*Rx (the same axis order applied to
            // positions and normals above) and orthonormal, the inverse
            // is the transpose: rows of M^T are columns of M, so we
            // dot worldLightDir with the COLUMNS of objM. For identity
            // object rotation we just use worldLightDir directly.
            const Vector3& Lw = directionalLight->worldLightDir;
            if (objHasRotation) {
                objLightDir.assign(
                    (int32_t)(((int64_t)Lw.x * objM00 + (int64_t)Lw.y * objM10 + (int64_t)Lw.z * objM20) / FIXED_POINT_SCALE),
                    (int32_t)(((int64_t)Lw.x * objM01 + (int64_t)Lw.y * objM11 + (int64_t)Lw.z * objM21) / FIXED_POINT_SCALE),
                    (int32_t)(((int64_t)Lw.x * objM02 + (int64_t)Lw.y * objM12 + (int64_t)Lw.z * objM22) / FIXED_POINT_SCALE));
            } else {
                objLightDir = Lw;
            }
        }
    }
#endif

    // MESHPUNK: per-object scale, applied here in the transform.
    //
    // Object::scale and Object::transformScale are declared upstream but read
    // nowhere, so setting them had no effect at all — the only working scale
    // path was bakeScale(), which is uniform-only and permanently rewrites the
    // mesh (so it cannot animate). Applying it here is non-destructive and
    // supports a per-axis factor.
    //
    // Normals get the INVERSE scale, which is the inverse-transpose for a
    // diagonal matrix: stretching an axis tilts a surface's normal the other
    // way. Scaling them by the same factor as positions would light a
    // non-uniformly scaled object as though it were unscaled. They are then
    // renormalised because sceneLambertDiffuse and the renderer's shading both
    // require length FIXED_POINT_SCALE.
    const bool applyScale = obj->transformScale &&
        (obj->scale.x != FIXED_POINT_SCALE ||
         obj->scale.y != FIXED_POINT_SCALE ||
         obj->scale.z != FIXED_POINT_SCALE) &&
        obj->scale.x != 0 && obj->scale.y != 0 && obj->scale.z != 0;
    const int32_t objSx = obj->scale.x, objSy = obj->scale.y, objSz = obj->scale.z;

    // Transform vertices and normals. Reads from the authoring vertices
    // (meshSource->vertices), writes only the configured pipeline's live
    // fields into the slim RenderVertex scratch array.
    for (size_t vi = 0; vi < vertCount; ++vi) {
        const Object::Vertex& srcVert = meshSource->vertices[vi];
        RenderVertex& dst = transformedVertices[vi];
        Vector3 pos(srcVert.position);
#if LIGHTING
        Vector3 normal(srcVert.normal);
#endif

        if (applyScale) {
            pos.assign(
                (int32_t)(((int64_t)pos.x * objSx) / FIXED_POINT_SCALE),
                (int32_t)(((int64_t)pos.y * objSy) / FIXED_POINT_SCALE),
                (int32_t)(((int64_t)pos.z * objSz) / FIXED_POINT_SCALE));
#if LIGHTING
            int32_t nx = (int32_t)(((int64_t)normal.x * FIXED_POINT_SCALE) / objSx);
            int32_t ny = (int32_t)(((int64_t)normal.y * FIXED_POINT_SCALE) / objSy);
            int32_t nz = (int32_t)(((int64_t)normal.z * FIXED_POINT_SCALE) / objSz);
            const float nlen = std::sqrt((float)nx * nx + (float)ny * ny + (float)nz * nz);
            if (nlen > 1.0f) {
                const float k = (float)FIXED_POINT_SCALE / nlen;
                nx = (int32_t)(nx * k);
                ny = (int32_t)(ny * k);
                nz = (int32_t)(nz * k);
            }
            normal.assign(nx, ny, nz);
#endif
        }

        // Y-axis (cylindrical) billboard: pre-rotate the vertex offset
        // by +camRotY around Y so the camera's Y rotation below cancels
        // it out exactly, leaving the offset's X axis mapped to
        // camera-right and Y axis to world-Y. The billboard's world-
        // space POSITION (objPos) goes through the normal camera
        // transform unchanged, so it occupies real 3D space and
        // distance-fades / sorts correctly; only its local orientation
        // is locked to face the camera in yaw. Pitch/roll still apply
        // through the camera transform so the billboard appears
        // tilted exactly as a vertical real object would.
        if (isBillboard) {
            pos.assign((pos.x * camCosY - pos.z * camSinY) / FIXED_POINT_SCALE,
                        pos.y,
                       (pos.x * camSinY + pos.z * camCosY) / FIXED_POINT_SCALE);
        }

        // Object rotation, world translation and camera rotation in ONE
        // 3×3 mat-vec with the translation folded into the accumulate:
        // p_cam = fM·p + fT (see the composition above). Billboards enter
        // here with their yaw pre-rotation already applied and fM equal to
        // the bare camera matrix, which reproduces the old pipeline order
        // exactly.
        {
            const float fpx = (float)pos.x, fpy = (float)pos.y, fpz = (float)pos.z;
            pos.assign(
                (int32_t)(fpx * fM00 + fpy * fM01 + fpz * fM02 + fTx),
                (int32_t)(fpx * fM10 + fpy * fM11 + fpz * fM12 + fTy),
                (int32_t)(fpx * fM20 + fpy * fM21 + fpz * fM22 + fTz));
        }
#if LIGHTING
        // Normals use the combined ROTATION only — no translation. The
        // object-local-light path skips the transform entirely and shades
        // from the untransformed mesh-local normal below.
        if (!objectLocalLight) {
            const float fnx = (float)normal.x, fny = (float)normal.y, fnz = (float)normal.z;
            normal.assign(
                (int32_t)(fnx * fM00 + fny * fM01 + fnz * fM02),
                (int32_t)(fnx * fM10 + fny * fM11 + fnz * fM12),
                (int32_t)(fnx * fM20 + fny * fM21 + fnz * fM22));
        }
#endif

        // Perspective projection — float fovFactor lets us use a reciprocal
        // multiply instead of 64-bit integer divide, leveraging the hardware
        // FPU on ESP32-S3/P4 (64-bit div is software-emulated on those cores).
        if (pos.z == 0) pos.z = 1; // avoid divide-by-zero
        // Record camera-space position (pre-projection) for near-plane clipping.
        camSpacePos[vi] = pos;
        const float invZ = fovFactor / (float)pos.z;
        // MESHPUNK: Q4 sub-pixel, and ROUNDED rather than truncated toward
        // zero — truncation biased the two halves of the screen in opposite
        // directions. See MESHPUNK_SUBPIXEL_BITS in JetConfig.hpp.
        dst.position.x = (int32_t)floorf(pos.x * invZ * (float)MESHPUNK_SUBPIXEL_ONE + 0.5f)
                       + (screenWidth  / 2) * MESHPUNK_SUBPIXEL_ONE;
        dst.position.y = (screenHeight / 2) * MESHPUNK_SUBPIXEL_ONE
                       - (int32_t)floorf(pos.y * invZ * (float)MESHPUNK_SUBPIXEL_ONE + 0.5f);
        dst.position.z = pos.z;

#if TEXTURE_MAPPING
        dst.uv = srcVert.uv;
#endif
#if LIGHTING
        // Store transformed normal (only consumed by the lit shading paths).
        dst.normal = normal;
        // Object-local-light path: precompute the per-vertex Lambert
        // brightness here using the mesh-local normal + object-local
        // light direction. drawTriangle reads this back and skips its
        // own jetShadeBrightness call. For FLAT triangles all three
        // verts of a face share the same normal (computeFlatNormals
        // stamps identical normals) so the per-triangle FLAT path can
        // pick any one — it uses v1 already.
        dst.lambertBrightness = objectLocalLight
            ? sceneLambertDiffuse(normal, objLightDir, objLightIntensity, objDiffuseCoef)
            : 0;
#endif
    }

#if SORT_TRIANGLES
    // Sort the triangles by depth
    std::sort(meshSource->triangles.begin(), meshSource->triangles.end(), [&](const Object::Triangle& a, const Object::Triangle& b) {
        const auto& v1 = transformedVertices[a.v1];
        const auto& v2 = transformedVertices[a.v2];
        const auto& v3 = transformedVertices[a.v3];
        int32_t z1 = v1.position.z;
        int32_t z2 = v2.position.z;
        int32_t z3 = v3.position.z;
        return (z1 + z2 + z3) / 3 > (transformedVertices[b.v1].position.z + transformedVertices[b.v2].position.z + transformedVertices[b.v3].position.z) / 3;
    });
#endif

    // ------------------------------------------------------------------
    // Near-plane clipping helpers
    // ------------------------------------------------------------------
    // Without these, a triangle with even one vertex behind the near plane
    // is discarded whole (projected x/y would be garbage for that vertex),
    // producing visible holes in geometry right under the camera. We clip
    // straddling triangles to z==nearPlane and project the resulting new
    // vertices fresh, with UVs/normals interpolated along each clipped edge.
    const int32_t nz = camera->nearPlane;

    auto lerpI32 = [](int32_t a, int32_t b, int32_t tFixed) -> int32_t {
        return a + (int32_t)(((int64_t)tFixed * (b - a)) / FIXED_POINT_SCALE);
    };

    // Given a camera-space position, produce a RenderVertex with screen-
    // space x/y, camera-space z kept in position.z, and the provided normal
    // and uv (both interpolated upstream in camera / texture space). The
    // normal/uv/brightness parameters are only stored when the configured
    // pipeline carries those fields.
    auto projectVertex = [&](const Vector3& cam,
                             const Vector3& normalCamSpace,
                             const Vector2& uv,
                             uint16_t lambertBrightness = 0) -> RenderVertex {
        RenderVertex v;
        const float invZ = (cam.z != 0) ? fovFactor / (float)cam.z : 0.0f;
        // MESHPUNK: Q4 sub-pixel, rounded. See MESHPUNK_SUBPIXEL_BITS.
        v.position.x = (int32_t)floorf(cam.x * invZ * (float)MESHPUNK_SUBPIXEL_ONE + 0.5f)
                     + (screenWidth  / 2) * MESHPUNK_SUBPIXEL_ONE;
        v.position.y = (screenHeight / 2) * MESHPUNK_SUBPIXEL_ONE
                     - (int32_t)floorf(cam.y * invZ * (float)MESHPUNK_SUBPIXEL_ONE + 0.5f);
        v.position.z = cam.z;
#if LIGHTING
        v.normal = normalCamSpace;
        v.lambertBrightness = lambertBrightness;
#else
        (void)normalCamSpace; (void)lambertBrightness;
#endif
#if TEXTURE_MAPPING
        v.uv = uv;
#else
        (void)uv;
#endif
        return v;
    };

    // Clip edge from A (behind near plane) → B (in front). Returns vertex on
    // the near plane with all attributes interpolated.
    auto clipEdge = [&](const RenderVertex& A, const RenderVertex& B,
                        const Vector3& camA, const Vector3& camB) -> RenderVertex {
        int32_t dz = camB.z - camA.z;
        if (dz == 0) dz = 1;
        int32_t t = (int32_t)(((int64_t)(nz - camA.z) * FIXED_POINT_SCALE) / dz);
        Vector3 camNew(lerpI32(camA.x, camB.x, t),
                       lerpI32(camA.y, camB.y, t),
                       nz);
#if LIGHTING
        Vector3 n(lerpI32(A.normal.x, B.normal.x, t),
                  lerpI32(A.normal.y, B.normal.y, t),
                  lerpI32(A.normal.z, B.normal.z, t));
        // Interpolate the precomputed Lambert brightness along the clipped
        // edge too. Without this, clipped vertices land in projectVertex
        // with the default lambertBrightness=0 and the brightnessPrecomputed
        // path downstream reads pure black — for FLAT that turns the whole
        // face black whenever v1 happens to be the clipped vertex; for
        // GOURAUD it gradient-blends toward black at the clip seam,
        // producing the dark wedge artefacts visible on triangles whose
        // vertices straddle the near plane.
        const uint16_t lb = (uint16_t)(A.lambertBrightness +
            (int32_t)(((int64_t)t * (int32_t)(B.lambertBrightness - A.lambertBrightness)) / FIXED_POINT_SCALE));
#else
        Vector3 n(0, 0, 0);
        const uint16_t lb = 0;
#endif
#if TEXTURE_MAPPING
        Vector2 uv(lerpI32(A.uv.x, B.uv.x, t),
                   lerpI32(A.uv.y, B.uv.y, t));
#else
        Vector2 uv(0, 0);
#endif
        return projectVertex(camNew, n, uv, lb);
    };

    // Emit a projected triangle into renderQueue (does screen-bounds + backface
    // cull). Reused by both the fast path and the clipped path.
    auto emitTri = [&](const RenderVertex& a,
                       const RenderVertex& b,
                       const RenderVertex& c,
                       Material* mat
#if MAX_PICK_QUERIES > 0
                       , int32_t srcTriIdx
#endif
                       ) {
        // Per-triangle near/far cull on the average camera-space Z.
        // Object cull rejects entirely-outside boxes; this catches the
        // remaining far-plane tris on objects that straddle it (large
        // ground tiles, big cliff faces). We do this BEFORE the off-
        // screen XY tests + shoelace + queue-push so a doomed tri pays
        // none of those costs (and never enters the painter's-sort).
        // depthFogFar == farPlane in the current build, so this also
        // subsumes the depth-fog alpha=0 early-out that drawTriangle
        // would have done after a full setup.
        const int32_t avgZ = (a.position.z + b.position.z + c.position.z) / 3;
        if (avgZ > camera->farPlane || avgZ < camera->nearPlane) return;

        // MESHPUNK: positions are Q4, so the screen edges are too.
        const int32_t sw_sp = screenWidth  * MESHPUNK_SUBPIXEL_ONE;
        const int32_t sh_sp = screenHeight * MESHPUNK_SUBPIXEL_ONE;
        if (a.position.x < 0 && b.position.x < 0 && c.position.x < 0) return;
        if (a.position.x > sw_sp && b.position.x > sw_sp && c.position.x > sw_sp) return;
        if (a.position.y < 0 && b.position.y < 0 && c.position.y < 0) return;
        if (a.position.y > sh_sp && b.position.y > sh_sp && c.position.y > sh_sp) return;

        // 64-bit shoelace: projected coords from a near-plane-clipped vertex
        // can be tens of thousands of units, which would overflow a 32-bit
        // signed product and accidentally flip the backface-cull sign. That
        // was causing large floor triangles near the camera to vanish
        // (typically in the bottom-left quadrant where cam.x/cam.y are most
        // negative).
        int64_t shoelaceArea = (int64_t)a.position.x * (b.position.y - c.position.y) +
                               (int64_t)b.position.x * (c.position.y - a.position.y) +
                               (int64_t)c.position.x * (a.position.y - b.position.y);

        bool shouldCull = false;
        switch (cullingMode) {
            case CullingMode::CULL_BACKFACES: shouldCull = (shoelaceArea <= 0); break;
            case CullingMode::CULL_FRONTFACES: shouldCull = (shoelaceArea >= 0); break;
            case CullingMode::NO_CULLING: break;
        }
        if (shouldCull) return;

        // MESHPUNK: flip negative-winding triangles for EVERY culling mode, not
        // just NO_CULLING. The rasteriser's span test requires all three edge
        // functions >= 0, which only holds for positive screen-space winding.
        // CULL_FRONTFACES culls shoelaceArea >= 0, so every triangle it keeps
        // has NEGATIVE winding — upstream left those unflipped, so they entered
        // the render queue and then failed the edge test at every pixel,
        // drawing nothing at all. (CULL_BACKFACES already culls the negatives,
        // so this branch never fires there; NO_CULLING behaviour is unchanged.)
        RenderTri rt;
        if (shoelaceArea < 0) {
            rt.v1 = c; rt.v2 = b; rt.v3 = a;
        } else {
            rt.v1 = a; rt.v2 = b; rt.v3 = c;
        }
        rt.material       = mat;
        // MESHPUNK: capture the baked colour BY VALUE. `bakedMat` is this
        // slice's slot of s_bakedMat[]; its colour was overwritten by the
        // caller immediately before this call, so right here is the only
        // moment it is still the colour that belongs to THIS triangle.
        // Detecting it by pointer identity keeps emitTri's signature (and
        // the JET_EMIT_TRI macro) untouched.
        if (mat == bakedMat) {
            rt.colorBaked = true;
            rt.bakedColor = bakedMat->color;
        } else {
            rt.colorBaked = false;
        }
        rt.ignoreZBuffer  = ignoreZBuffer;
        rt.noWriteZBuffer = noWriteZBuffer;
        rt.zBias          = obj->zBias;
        rt.objAlpha       = objAlpha;
        // MESHPUNK: zBias participates in the painter's sort. Without a depth
        // buffer the sort key is the only depth ordering there is, and upstream
        // ignored zBias here, leaving the field with no effect in this build.
        // Scope: the bias breaks ordering between triangles whose averaged
        // depths are within |zBias| of each other — congruent coplanar cells.
        // It cannot rescue a large polygon whose centroid depth differs from
        // its neighbours' by more than that; decals must also follow FAST_Z's
        // small-triangle rule (subdivide, and lift slightly off the surface).
        // Applied to the sort key only; the near/far cull above is unbiased.
        rt.avgZ           = avgZ - obj->zBias;
        // MESHPUNK: ordering depth. FARTHEST makes a surface sort by its back
        // edge so anything standing on it wins — see Object::SortDepth. The
        // max costs less than the average it replaces (no divide).
        if (obj->sortDepth == SortDepth::FARTHEST) {
            int32_t mz = a.position.z > b.position.z ? a.position.z : b.position.z;
            if (c.position.z > mz) mz = c.position.z;
            rt.sortZ = mz;
        } else {
            rt.sortZ = avgZ;
        }
#if LIGHTING
        rt.brightnessPrecomputed = objectLocalLight;
#endif
#if MAX_PICK_QUERIES > 0
        rt.sourceObject        = obj;
        rt.sourceTriangleIndex = srcTriIdx;
#endif
        // MESHPUNK: hard cap, checked HERE rather than at the top of emitTri.
        // Everything above has already rejected the triangles that were never
        // going to be drawn, so a triangle reaching this point would genuinely
        // have been queued — which makes the drop count exactly "triangles
        // missing from the picture" instead of a number inflated by ordinary
        // culling. The early-out would have been cheaper, but an overflowing
        // frame is pathological by definition and an honest diagnostic is worth
        // more there than saving setup work on a frame we are already
        // reporting as broken. Placed before BOTH pushes so the queue and
        // triYExtent can never fall out of lockstep.
        if (maxQueuedTriangles && emitQ.size() >= maxQueuedTriangles) {
            // Atomic: both prepare slices can drop concurrently.
            __atomic_add_fetch(&lastFrameDroppedTriangles, 1,
                               __ATOMIC_RELAXED);
            return;
        }

        // MESHPUNK: record the screen Y extent for rasterizeBand's per-band
        // pre-test. Taken from the ORIGINAL a/b/c rather than rt.v1/v2/v3
        // because the winding flip above only reorders the same three
        // vertices, so the extent is identical either way.
        // MESHPUNK: positions are Q4; the band pre-test works in whole rows, so
        // convert here. Arithmetic shift floors toward negative, which widens
        // the extent for off-screen-top triangles — safe, since this test is
        // only ever allowed to be more permissive than the rasteriser's.
        int32_t triMinY = a.position.y >> MESHPUNK_SUBPIXEL_BITS;
        int32_t triMaxY = triMinY;
        const int32_t bY = b.position.y >> MESHPUNK_SUBPIXEL_BITS;
        const int32_t cY = c.position.y >> MESHPUNK_SUBPIXEL_BITS;
        if (bY < triMinY) triMinY = bY;
        if (bY > triMaxY) triMaxY = bY;
        if (cY < triMinY) triMinY = cY;
        if (cY > triMaxY) triMaxY = cY;
        // drawTriangle masks its own bounds with & ~1; widen by one row here
        // rather than replicate the masking, so the pre-test can only ever be
        // more permissive than the rasteriser's own test.
        emitExt.push_back(packYExtent(triMinY - 1, triMaxY + 1));

        emitQ.push_back(rt);
    };

    // Render triangles with backface culling and shading
    // MESHPUNK: record how many triangles this object actually contributed to
    // the render queue, so "nothing is drawn" can be told apart from "drawn but
    // not visible" without guessing.
    const size_t meshpunkQueueStart = emitQ.size();

    for (size_t triIdx = 0; triIdx < meshSource->triangles.size(); ++triIdx) {
        const auto& triangle = meshSource->triangles[triIdx];
        const auto& vA = transformedVertices[triangle.v1];
        const auto& vB = transformedVertices[triangle.v2];
        const auto& vC = transformedVertices[triangle.v3];
        const Vector3& cA = camSpacePos[triangle.v1];
        const Vector3& cB = camSpacePos[triangle.v2];
        const Vector3& cC = camSpacePos[triangle.v3];

        // Classify each vertex against the near plane.
        const int outMask = (cA.z < nz ? 1 : 0)
                          | (cB.z < nz ? 2 : 0)
                          | (cC.z < nz ? 4 : 0);

        if (outMask == 7) continue;               // fully behind near plane

#if MAX_PICK_QUERIES > 0
        const int32_t srcTriIdx = (int32_t)triIdx;
        #define JET_EMIT_TRI(A, B, C, M)  emitTri((A), (B), (C), (M), srcTriIdx)
#else
        #define JET_EMIT_TRI(A, B, C, M)  emitTri((A), (B), (C), (M))
#endif

        if (outMask == 0) {                       // fast path: fully in front
            Material* effectiveMat = triangle.material;
            if (triangle.colorBaked) {
                bakedMat->color = triangle.bakedColor;
                effectiveMat = bakedMat;
            }
            JET_EMIT_TRI(vA, vB, vC, effectiveMat);
            continue;
        }

        // Straddling near plane — produce a clipped polygon (3 or 4 verts)
        // while preserving winding order of the original triangle.
        const RenderVertex* vs[3]  = { &vA, &vB, &vC };
        const Vector3*        cvs[3] = { &cA, &cB, &cC };
        const bool in[3] = { (outMask & 1) == 0,
                             (outMask & 2) == 0,
                             (outMask & 4) == 0 };

        RenderVertex poly[4];
        int polyN = 0;
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            if (in[i]) poly[polyN++] = *vs[i];
            if (in[i] != in[j]) {
                // One endpoint in, one out — add the near-plane intersection.
                if (in[i])
                    poly[polyN++] = clipEdge(*vs[j], *vs[i], *cvs[j], *cvs[i]);
                else
                    poly[polyN++] = clipEdge(*vs[i], *vs[j], *cvs[i], *cvs[j]);
            }
        }

        if (polyN >= 3) {
            Material* effectiveMat = triangle.material;
            if (triangle.colorBaked) { bakedMat->color = triangle.bakedColor; effectiveMat = bakedMat; }
            JET_EMIT_TRI(poly[0], poly[1], poly[2], effectiveMat);
        }
        if (polyN == 4) JET_EMIT_TRI(poly[0], poly[2], poly[3], triangle.colorBaked ? bakedMat : triangle.material);
        #undef JET_EMIT_TRI
    }

    // MESHPUNK: see meshpunkQueueStart above.
    obj->lastEmitted = (int)(emitQ.size() - meshpunkQueueStart);
}

} // namespace Renderer

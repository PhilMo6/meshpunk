// Mesh system v2 for the jet3d module.
//
// The RECIPE is the source of truth: a builder records the parts it is given
// (primitive type + parameters + transform + material) and evaluation into
// triangles happens at bake time. Keeping the recipe is what makes the hard
// features cheap:
//
//   - INTERIOR CULLING needs no mesh CSG. Every solid primitive is CONVEX
//     with a known analytic inside-test, so "is this point buried inside
//     another part?" is an inverse transform plus a few compares, and a
//     triangle whose three corners are all strictly inside one convex solid
//     is provably buried (convexity) and safely dropped. Triangles that
//     straddle a solid are recursively 4-split and re-tested; what survives
//     of a buried region is a thin skirt at the intersection, sized by the
//     bake tolerance. Probes include the centroid and edge midpoints because
//     a long triangle can pass THROUGH a solid with all three corners
//     outside (the fin-bar-through-cylinder case).
//
//   - Future LOD is a re-evaluation of the recipe at coarser segment counts,
//     not a decimation problem. Future rigs group parts under bones. (Neither
//     is implemented here yet; the format reserves room.)
//
// Pipeline: evaluate() -> cull() -> weld() -> build() | save().
//
// .jmsh v2 ("JMS2") is a CHUNKED, ZERO-COPY format: a section table over one
// contiguous blob, with the vertex section stored in Renderer::Object::Vertex
// native layout. Loading = one read + in-place fixups; the Object borrows its
// arrays straight from the blob (JetSpan::borrow) and frees the blob when it
// dies (Object::adoptBlob). Old "JMSH" v1 files are rejected with a clear
// error — the loader for them is gone, and the demo re-bakes its own file.
//
// Section tags reserved for later phases: PART/CLIP (rigs, animation),
// CELL (spatial cells), LODn (coarser re-bakes), SORT (pre-sorted orders).

#ifndef JET_MESH_H
#define JET_MESH_H

#include <stdint.h>
#include <vector>

namespace Renderer { class Object; class Material;
                     class DirectionalLight; class AmbientLight; }

// Everything the file format knows about a material.
struct JetMatParams {
    uint16_t color;
    uint8_t  alpha, diffuse, specular, emissive, shading;
};

// Spatial cell of a large static mesh: a contiguous slice of VERT/TRIS whose
// triangle indices are CELL-LOCAL. Each becomes its own Object so the frustum
// cull can reject whole regions with one AABB test instead of transforming
// every vertex of the whole mesh. Declared here because the builder holds a
// vector of them.
struct JmshCellRec {
    uint32_t vertOff, vertCount;
    uint32_t triOff,  triCount;
};

class JetMeshBuilder {
public:
    // Per-part transform. Rotation is degrees, applied in Jet's object order
    // (Rz * Ry * Rx); scale is a plain multiplier per axis; translation is in
    // world-scaled units.
    struct Transform {
        float tx = 0, ty = 0, tz = 0;
        float rx = 0, ry = 0, rz = 0;
        float sx = 1, sy = 1, sz = 1;
    };

    enum PartType : uint8_t {
        PT_CUBE, PT_SPHERE, PT_PLANE, PT_PYRAMID,
        PT_CYLINDER, PT_CAPSULE, PT_QUAD, PT_GRID,
        // Terrain patch: a grid whose vertex heights come from `Part::heights`.
        // a=width b=depth c=cols d=rows (VERTEX counts, so cols*rows samples).
        PT_HEIGHTFIELD,
        // Quad strip through arbitrary 3D stations (`Part::ribbonPts`): one
        // quad between each consecutive left/right edge pair, optionally
        // closed into a loop. THE primitive for tracks, roads and walls —
        // transformed QUAD parts cannot form trapezoids, so any curved run
        // built from them cracks at every joint.
        PT_RIBBON,
    };

    // One recipe entry. a..d are the primitive's own arguments, world-scaled
    // where they are dimensions: cube(w,h,d) sphere(r,seg) plane(w,h)
    // pyramid(base,h) cylinder(r,h,seg) capsule(r,h,seg) quad(w,h)
    // grid(w,h,rows,cols). mat2 is the grid's alternate material.
    struct Part {
        PartType type;
        bool     caps = true;          // cylinder end caps
        int32_t  a = 0, b = 0, c = 0, d = 0;
        Transform xf;
        Renderer::Material* mat  = nullptr;
        Renderer::Material* mat2 = nullptr;

        // Rigging. bone 0 is the implicit whole-mesh bone of an unrigged
        // recipe; named parts get bone indices assigned by the Lua layer in
        // order of first appearance. Pivot is the bone's rotation origin in
        // world-scaled recipe space (shared by every part of the bone).
        uint16_t bone = 0;

        // PT_HEIGHTFIELD only: cols*rows vertex heights in world units, row
        // major (row = Z, col = X), matching createGrid's vertex layout.
        //
        // These live in the RECIPE rather than being sampled from a callback at
        // bake time, because evaluate() has to stay reproducible:
        // releaseEvaluated throws the geometry away and any later save or
        // rebake reruns the pipeline with no Lua in reach. int16 caps relief at
        // +/-4096 authored units — far more than terrain needs — and keeps a
        // patch's recipe to 2 bytes per sample, which is the whole point of
        // storing POINTS rather than tiles.
        std::vector<int16_t> heights;

        // PT_HEIGHTFIELD only: sample on a TRIANGULAR lattice instead of a
        // square one — odd rows are offset by half a cell and row spacing is
        // scaled by sqrt(3)/2, so every interior vertex has six equidistant
        // neighbours. The samples array stays a plain rows x cols rectangle;
        // only the positions and the triangulation change.
        //
        // Why: a square grid triangulated with a fixed diagonal gives every
        // quad the same split, and the surface picks up a visible directional
        // grain. A triangular lattice has no preferred direction. (A hexagonal
        // lattice of POINTS is the dual of this — same thing, and unlike
        // hexagons a triangle still subdivides 4->1 cleanly.)
        bool triRows = false;

        // PT_HEIGHTFIELD only: emit UVs spanning the whole patch, 0..1024
        // (FIXED_POINT_SCALE) corner to corner. Off by default because every
        // other primitive's UVs mean something different and an untextured
        // patch has no use for them.
        bool uvSpan = false;

        // PT_HEIGHTFIELD only: WORLD units per texture repeat. When non-zero it
        // overrides uvSpan's stretch-to-fit and the texture tiles at a fixed
        // real-world size no matter how big the patch is.
        //
        // Stretching one image across a tile makes the detail scale change with
        // the LOD level, which is exactly the "scaled for something completely
        // different" look. A repeat length is a physical property of the
        // material and must not depend on tile size.
        //
        // Tile size SHOULD be a whole multiple of this, so that a patch placed
        // on the tile grid continues its neighbour's phase (UVs are measured
        // from the patch's own minimum corner). int16 UVs cap this at 32
        // repeats per patch.
        int32_t uvRepeat = 0;

        // PT_HEIGHTFIELD only, requires uvRepeat > 0: give each grid cell one
        // of 8 dihedral orientations (4 rotations x mirror) of the tiling
        // texture, chosen by a hash of the cell's corner heights + index. The
        // repeats then stop lining up in rows, which is what the eye reads as
        // tiling — the period is unchanged but the grid alignment dissolves.
        // Wall faces and skirts keep their normal mapping. Costs nothing at
        // runtime: it is only UV assignment, and a dihedral map is linear so
        // perspective correction and mip selection behave identically. The
        // trade is pattern-phase discontinuity at cell borders.
        bool uvMix = false;

        // PT_HEIGHTFIELD only: when non-empty, each cell's GROUND faces take
        // one material from this list instead of `mat`, chosen by the same
        // cell hash that drives uvMix (independent bits, so variant and
        // orientation do not correlate). Papering the ground with several
        // same-palette tile variants breaks the pattern repetition that one
        // tile cannot avoid. There is no per-triangle material-switch cost in
        // this renderer — the rasteriser hoists texture pointers per triangle
        // — so the only budget that matters is the texture cache working set.
        // Wall faces and skirts are unaffected.
        std::vector<Renderer::Material*> groundVariants;

        // PT_HEIGHTFIELD only: store SMOOTH per-vertex normals on ground
        // faces, computed by central differences over the height lattice,
        // instead of the per-face normal. This is what makes GOURAUD mean
        // something on a heightfield — with per-face normals all three
        // vertices carry the same normal and Gouraud degenerates to flat.
        // Wall faces keep the face normal either way (crisp cliff shading),
        // and the face normal still decides the wall/ground material split.
        // With triRows the odd-row half-cell shift is ignored by the
        // difference stencil — the error is a fraction of a cell and lighting
        // is the only consumer.
        bool smoothNormals = false;

        // PT_RIBBON only: station edge points, world-scaled, six int32 per
        // station — left x,y,z then right x,y,z. `ribbonClosed` adds the
        // wrapping quad from the last station back to the first. Faces are
        // oriented upward (+Y) by the same winding rule the heightfield uses.
        std::vector<int32_t> ribbonPts;
        bool ribbonClosed = false;

        // PT_HEIGHTFIELD only: faces whose normal is less upright than this
        // (cosine, in FIXED_POINT_SCALE units) take `mat2` instead of `mat`.
        // 0 disables. This is what puts a striated rock texture on cliff faces
        // and a ground texture on the flats, from ONE part.
        int32_t wallCos = 0;

        // PT_HEIGHTFIELD only: stored heights are left-shifted by this many bits
        // before use, so a patch's usable range is +/-32,767 << heightShift
        // world units at a quantisation of 1 << heightShift.
        //
        // WHY: int16 heights cap relief at 4,096 AUTHORED units. A world 2.5
        // million units across with 4,096 of relief is an aspect ratio of 1:600
        // — a plain, whatever the noise does, and it looked like one. Widening
        // the samples to int32 would double a figure the original design cared
        // about (2 bytes per stored point); a shift keeps the storage and buys
        // the range, and terrain has no use for sub-unit height precision.
        // Chosen per patch from its own measured extent, so near tiles keep
        // full precision and only the huge coarse ones give any up.
        uint8_t heightShift = 0;

        // PT_HEIGHTFIELD only: drop a vertical curtain of this many WORLD units
        // from the patch perimeter.
        //
        // This is the crack fix, and it is the one that actually works. Two
        // levels of a terrain LOD agree at shared CORNERS but not along the
        // edge between them — the coarse edge is a straight line across samples
        // the fine edge follows, and the measured divergence is 43.7 to 280.7
        // authored units depending on density. Matching the surfaces (band
        // limiting, baked geomorphing) chases that error; a skirt makes it
        // irrelevant, because the gap is filled by geometry that is already
        // terrain-coloured. Costs 4*(n-1) quads per patch.
        int32_t skirt = 0;
    };

    // One bone of a rigged recipe. Index 0 is the implicit root that unnamed
    // parts belong to; the recipe is RIGGED when any further bone exists.
    // parent is an index into this array (-1 for a root) and always precedes
    // its children, so composition can run in plain array order. Every part
    // of a bone shares the bone's pivot (world-scaled recipe space).
    struct Bone {
        uint32_t nameHash = 0;
        int16_t  parent   = -1;
        int32_t  px = 0, py = 0, pz = 0;
        // Builder-side only (the file stores the hash): clip sampling hands
        // the animator a pose table keyed by these names.
        char name[24] = {};
    };
    std::vector<Bone> bones;
    bool rigged() const { return bones.size() > 1; }

    // A baked animation clip: keyCount keys sampled at `rate` Hz, KEY-MAJOR
    // (all bones of key 0, then key 1, ...) for playback locality. Positions
    // are bone-local offsets in world-scaled int16; quaternions Q1.14.
    struct ClipKey { int16_t px, py, pz, qx, qy, qz, qw; };
    struct Clip {
        uint32_t nameHash = 0;
        uint16_t rate     = 15;
        uint16_t keyCount = 0;
        std::vector<ClipKey> keys;   // keyCount * bones.size()
    };
    std::vector<Clip> clips;

    // Evaluated geometry. _pad is explicit because weld() compares vertices
    // with memcmp over the whole struct. BTri carries its source part index
    // so interior culling never tests a triangle against its own solid.
    struct BVert { int32_t px, py, pz; int16_t nx, ny, nz; int16_t u, v;
                   int16_t _pad; };
    struct BTri  { uint32_t a, b, c; uint16_t mat; uint16_t part; };

    std::vector<Part> parts;
    // Adding a part invalidates the recorded counts as well as the geometry —
    // otherwise a post-bake stats() query would answer with the old mesh's
    // numbers after the recipe had already changed underneath it.
    void addPart(const Part& p) {
        parts.push_back(p); dirty = true; lastStats.counted = false;
    }

    // Evaluated state, valid after evaluate().
    std::vector<BVert>               verts;
    std::vector<BTri>                tris;
    std::vector<JetMatParams>        matParams;
    std::vector<Renderer::Material*> mats;
    bool dirty = true;

    // Filled by the pipeline for logging. verts/tris/mats are captured by
    // releaseEvaluated() so a post-bake b:stats() can still answer without
    // re-running the pipeline (and without reporting PRE-cull counts, which is
    // what a naive re-evaluate would return).
    struct Stats {
        int  trisDropped = 0, trisAdded = 0, vertsWelded = 0;
        int  verts = 0, tris = 0, mats = 0;
        bool counted = false;
    };
    Stats lastStats;

    // Recipe -> verts/tris/materials. Clears any previous evaluation, so the
    // pipeline is deterministic no matter how many times it reruns.
    bool evaluate();

    // Interior-surface removal. toleranceAuthored is the subdivision stop in
    // AUTHORED units (pre-world-scale); the surviving buried skirt at an
    // intersection is about this wide. Returns net triangles removed.
    int cull(float toleranceAuthored);

    // Enforce a maximum triangle edge length by recursive longest-edge
    // bisection. Returns triangles added.
    //
    // This is the systematic answer to FAST_Z's one-depth-per-triangle rule.
    // A triangle's sort key is a single depth, so a LARGE triangle viewed at a
    // glancing angle is badly represented by any single value: a floor tile
    // 285 units across can sort nearer than a small object standing on it and
    // paint over it. No sort can fix that — the correct answer varies per
    // pixel — but bounding triangle size bounds the error. Doing it at bake
    // costs nothing at runtime, and spatial cells keep the extra triangles
    // affordable by culling the ones off screen.
    int subdivide(float maxEdgeWorld);

    // Merge exactly-identical vertices, drop degenerates. Returns removed.
    int weld();

    // Split the evaluated mesh into a grid of spatial cells of cellSizeWorld
    // units, keyed by triangle centroid. Rewrites verts/tris into cell-major
    // order with CELL-LOCAL triangle indices and fills `cells`, so save() and
    // build() consume the result with no further grouping. Vertices shared
    // across a cell boundary are duplicated (each cell owns its own copy).
    // Call AFTER weld(); returns the cell count (1 = not worth splitting).
    // Unrigged meshes only.
    int splitCells(int32_t cellSizeWorld);
    std::vector<JmshCellRec> cells;

    // Scene-ready Object with owned arrays. Null when empty or when the
    // vertex count exceeds uint16 indexing. Caller owns it. When `cells` is
    // populated, pass a cell index to get that cell's Object instead of the
    // whole mesh (whose triangle indices are cell-local and not globally
    // valid).
    Renderer::Object* build(int cell = -1) const;

    // Write the evaluated state as a JMS2 blob. Call after the pipeline.
    bool save(const char* path, uint8_t worldScale) const;

    // -----------------------------------------------------------------------
    // Primitive-native LOD: a coarser level is a RE-EVALUATION of the recipe
    // with segment counts scaled down and parts smaller than minPart (world
    // units, longest dimension) dropped entirely — no mesh decimation exists
    // or is needed. Unrigged recipes only.
    // -----------------------------------------------------------------------
    struct LodSpec { float segScale = 0.5f; int32_t minPart = 0; };

    // Derive the coarser recipe for one LOD level. Returns false when the
    // part should be dropped at this level.
    static bool lodPart(const Part& in, const LodSpec& s, Part& out);

    // Run the full pipeline over the derived recipe and append the result to
    // `lods`, with material indices REMAPPED onto this builder's table (LOD
    // levels share the main MATS section; dropping parts changes material
    // discovery order, so the remap is by Material identity). Call after the
    // main pipeline. Returns false if the level came out empty.
    bool bakeLod(const LodSpec& spec, float toleranceWorld);

    // Evaluated LOD levels, filled by the Lua bake orchestration (each level
    // is a full evaluate/cull/weld run over the derived recipe). Saved as
    // LODn sections; instantiated into Object::lodMeshes chains.
    struct LodLevel { std::vector<BVert> verts; std::vector<BTri> tris; };
    std::vector<LodLevel> lods;

    // Owned-array Object for one baked LOD level (mesh data only; the head's
    // transform drives the draw). Null on bad index / empty level.
    Renderer::Object* buildLod(size_t level) const;

    // -----------------------------------------------------------------------
    // Pipeline settings are RECIPE state, not call arguments
    // -----------------------------------------------------------------------
    // The recipe is the source of truth and evaluate() is documented as safe to
    // rerun, so everything that SHAPES the result has to live beside the parts —
    // otherwise a rerun silently produces a different mesh. That is exactly the
    // defect that made `b:bake{lod=...}` followed by a plain `b:save(path)`
    // write a file with no LOD sections. Holding the settings here makes any
    // rerun reproduce the same mesh, which in turn is what makes it safe to
    // throw the evaluated geometry away (see releaseEvaluated).
    //
    // Distances are AUTHORED units; run() applies the world scale. LodSpec is
    // the exception and keeps its documented world-unit minPart.
    struct Pipeline {
        bool  cull              = true;
        float toleranceAuthored = 4.0f;
        float maxEdgeAuthored   = 0.0f;   // 0 = off
        float cellSizeAuthored  = 0.0f;   // 0 = off
        // Split cells in X/Z only, never by height.
        //
        // splitCells is a 3D grid by default, which is right for a solid but
        // WRONG for a heightfield: it stacks cells vertically, and a cell's
        // centre then carries its own Y band. Every distance test downstream
        // (fade, appear, LOD) measures to that centre, so two cells covering
        // the SAME ground at different heights get different distances and a
        // distance band slices a horizontal strip out of the level — captured
        // on hardware as a sky-coloured gap with the same level still drawn
        // above and below it. Vertical splitting also buys no culling on a
        // heightfield, whose cells already overlap in X/Z.
        bool  cellSplit2D       = false;
        std::vector<LodSpec> lodSpecs;
    };
    Pipeline pipe;

    // Run the whole pipeline from `pipe`: evaluate -> cull -> subdivide ->
    // weld -> splitCells -> bakeLod per spec. Reruns are idempotent.
    bool run(float worldScale);

    // Drop the evaluated geometry, keeping the recipe.
    //
    // build() has already copied what it needs into owned Objects and save()
    // reruns the pipeline, so after a bake this data is dead — but a builder
    // userdata is ONE POINTER, so Lua's GC sees no pressure from it and will
    // not finalize it during a loop that bakes hundreds of meshes. A 144-tile
    // world therefore carried 4.28 MB of dead builders against 3.96 MB of live
    // world and ran the device out of PSRAM. Measured on the host: 99% of a
    // post-bake builder's bytes are freed here, leaving ~90 B of recipe.
    //
    // Swaps with empty vectors rather than clear(): clear() keeps capacity, and
    // capacity is most of the waste.
    void releaseEvaluated();
};

// ---------------------------------------------------------------------------
// Zero-copy load
// ---------------------------------------------------------------------------

// On-disk records the Lua layer also reads (fixed layout, pointer-free).
struct JmshPartRec {
    uint32_t nameHash;
    int16_t  parent;            // bone index, -1 root; parents precede children
    uint16_t pad;
    int32_t  px, py, pz;        // pivot, world-scaled
    uint32_t vertOff, vertCount;   // elements into VERT; verts are BONE-LOCAL
    uint32_t triOff,  triCount;    // elements into TRIS; indices are BONE-LOCAL
};
struct JmshClipRec {
    uint32_t nameHash;
    uint16_t rate;
    uint16_t keyCount;
    uint32_t keyOff;            // element offset into the clip-key pool
    uint32_t reserved;
};

// A parsed-but-not-instantiated JMS2 file: one malloc'd blob plus pointers
// into it. On failure `blob` is null. Ownership of the blob passes to the
// instantiated Object(s); call jet_mesh_file_free instead if abandoning.
// partData/clipData stay valid for as long as the blob lives (i.e. until the
// adopting Object is destroyed) — copy what must outlive the rig.
struct JetMeshFile {
    void*     blob      = nullptr;
    uint32_t  vertCount = 0;
    uint32_t  triCount  = 0;
    uint32_t  matCount  = 0;
    void*     vertData  = nullptr;   // Object::Vertex native layout
    void*     triData   = nullptr;   // JmshTriRec array (pre-fixup)
    const JetMatParams* matData = nullptr;

    // Rig sections (null / 0 for unrigged files).
    uint32_t  partCount = 0;
    const JmshPartRec* partData = nullptr;
    uint32_t  clipCount = 0;
    const JmshClipRec* clipData = nullptr;
    const JetMeshBuilder::ClipKey* clipKeys = nullptr;
    uint32_t  clipKeyCount = 0;

    // LOD sections (LOD1..LOD4). Each is a self-contained coarser mesh over
    // the same material table.
    struct Lod {
        uint32_t vertCount = 0, triCount = 0;
        void* vertData = nullptr;
        void* triData  = nullptr;
    };
    uint32_t lodCount = 0;
    Lod      lods[4];

    // Spatial cells (0 for unsplit files).
    uint32_t cellCount = 0;
    const JmshCellRec* cellData = nullptr;
};

// ---------------------------------------------------------------------------
// Rig math, shared between the clip sampler, the runtime tick and the host
// tests — the tests must exercise the SHIPPED formulas, not copies. All
// rotation composition matches the bake's matrix order (Rz * Ry * Rx).
// ---------------------------------------------------------------------------

struct JmQuat { float x, y, z, w; };

inline JmQuat jm_eulerToQuat(float rxDeg, float ryDeg, float rzDeg)
{
    const float DEG = 0.017453292519943295f;
    const float hx = rxDeg * DEG * 0.5f, hy = ryDeg * DEG * 0.5f,
                hz = rzDeg * DEG * 0.5f;
    const float cx = __builtin_cosf(hx), sx = __builtin_sinf(hx);
    const float cy = __builtin_cosf(hy), sy = __builtin_sinf(hy);
    const float cz = __builtin_cosf(hz), sz = __builtin_sinf(hz);
    JmQuat q;
    q.w = cz*cy*cx + sz*sy*sx;
    q.x = cz*cy*sx - sz*sy*cx;
    q.y = cz*sy*cx + sz*cy*sx;
    q.z = sz*cy*cx - cz*sy*sx;
    return q;
}

inline void jm_quatToMatrix(const JmQuat& q, float m[9])
{
    const float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    const float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    const float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    m[0] = 1 - 2*(yy + zz); m[1] = 2*(xy - wz);     m[2] = 2*(xz + wy);
    m[3] = 2*(xy + wz);     m[4] = 1 - 2*(xx + zz); m[5] = 2*(yz - wx);
    m[6] = 2*(xz - wy);     m[7] = 2*(yz + wx);     m[8] = 1 - 2*(xx + yy);
}

inline void jm_eulerMatrix(float rxDeg, float ryDeg, float rzDeg, float m[9])
{
    const float DEG = 0.017453292519943295f;
    const float cx = __builtin_cosf(rxDeg*DEG), sx = __builtin_sinf(rxDeg*DEG);
    const float cy = __builtin_cosf(ryDeg*DEG), sy = __builtin_sinf(ryDeg*DEG);
    const float cz = __builtin_cosf(rzDeg*DEG), sz = __builtin_sinf(rzDeg*DEG);
    m[0] = cz*cy; m[1] = cz*sy*sx - sz*cx; m[2] = cz*sy*cx + sz*sx;
    m[3] = sz*cy; m[4] = sz*sy*sx + cz*cx; m[5] = sz*sy*cx - cz*sx;
    m[6] = -sy;   m[7] = cy*sx;            m[8] = cy*cx;
}

inline void jm_mat3Mul(const float a[9], const float b[9], float o[9])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c];
}

// Read + validate + rescale (when the file's worldScale differs). True on
// success. Rejects JMSH v1 files explicitly.
bool jet_mesh_read(const char* path, uint8_t worldScale, JetMeshFile* out);

void jet_mesh_file_free(JetMeshFile* f);

// Turn a parsed UNRIGGED file into a scene Object. `mats` must hold matCount
// engine materials matching the file's material table. The Object borrows the
// blob's arrays where the ABI allows (32-bit pointers: triangle records are
// rewritten in place into Object::Triangle) and copies where it does not
// (host builds with 64-bit pointers). Consumes the blob either way.
// When the file carries LOD sections and outLods is given, the LOD levels are
// instantiated as mesh-only Objects wired into the head's lodMeshes chain and
// ALSO written to outLods (the caller owns them — destroy them BEFORE the
// head, which adopts the shared blob).
Renderer::Object* jet_mesh_instantiate(JetMeshFile& f,
                                       Renderer::Material* const* mats,
                                       Renderer::Object** outLods = nullptr,
                                       int maxLods = 0);

// Turn a parsed RIGGED file into one Object per bone, written to outBones in
// PART order. Every Object borrows subranges of the ONE blob; the LAST bone
// adopts it, so the caller must destroy the last bone AFTER all the others.
// Returns the bone count, or 0 on failure (nothing allocated, blob intact).
int jet_mesh_instantiate_rig(JetMeshFile& f, Renderer::Material* const* mats,
                             Renderer::Object** outBones, int maxBones);

// Turn a parsed CELLED file into one Object per spatial cell, written to
// outCells. Same ownership rule as the rig: every Object borrows the ONE blob
// and the LAST one adopts it, so it must be destroyed last. Returns the count,
// or 0 on failure (blob intact).
int jet_mesh_instantiate_cells(JetMeshFile& f, Renderer::Material* const* mats,
                               Renderer::Object** outCells, int maxCells);

#endif // JET_MESH_H

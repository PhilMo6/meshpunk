// Mesh system v2 — see jet_mesh.h for the design. Pipeline order is
// evaluate() -> cull() -> weld() -> build()/save(); each stage is safe to
// rerun because evaluate() rebuilds from the recipe deterministically.

#include "jet_mesh.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "Jet.hpp"
using namespace Renderer;

// lroundf is not among the host exports; round explicitly.
static inline int32_t jm_round(float x) { return (int32_t)(x >= 0 ? x + 0.5f : x - 0.5f); }

// ---------------------------------------------------------------------------
// File layout
// ---------------------------------------------------------------------------
// All records little-endian, sections 16-aligned inside one blob. The vertex
// record IS Renderer::Object::Vertex (asserted below) so the loader can hand
// the section straight to the renderer. The triangle record is pointer-free
// and the same 16 bytes as the device's Object::Triangle, so on-device it is
// rewritten in place (material index -> Material*).

struct JmshHeader {
    char     magic[4];        // "JMS2"
    uint8_t  version;         // 1
    uint8_t  worldScale;
    uint16_t sectionCount;
    uint32_t reserved0, reserved1;
};
struct JmshSection { uint32_t tag, offset, count, bytes; };
struct JmshTriRec {
    uint16_t a, b, c, mat, bakedColor;
    uint8_t  colorBaked, pad;
    uint32_t reserved;
};

static_assert(sizeof(JmshHeader)  == 16, "header layout");
static_assert(sizeof(JmshSection) == 16, "section layout");
static_assert(sizeof(JmshTriRec)  == 16, "tri record layout");
static_assert(sizeof(Object::Vertex) == 36, "vertex record is the native layout");

#define JM_TAG(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                         ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
static const uint32_t TAG_MATS = JM_TAG('M','A','T','S');
static const uint32_t TAG_VERT = JM_TAG('V','E','R','T');
static const uint32_t TAG_TRIS = JM_TAG('T','R','I','S');
static const uint32_t TAG_PART = JM_TAG('P','A','R','T');
static const uint32_t TAG_CLIP = JM_TAG('C','L','I','P');
static const uint32_t TAG_CELL = JM_TAG('C','E','L','L');
static_assert(sizeof(JmshCellRec) == 16, "cell record layout");

static_assert(sizeof(JmshPartRec) == 36, "part record layout");
static_assert(sizeof(JmshClipRec) == 16, "clip record layout");
static_assert(sizeof(JetMeshBuilder::ClipKey) == 14, "clip key layout");

static inline uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

// ---------------------------------------------------------------------------
// Evaluate: recipe -> triangles
// ---------------------------------------------------------------------------

// Rotation matrix in Jet's object order, Rz*Ry*Rx. Bake time is authoring
// time, so float precision here beats replicating the integer LUT chain.
static void jm_rotation(const JetMeshBuilder::Transform& t, float m[9])
{
    const float DEG = 0.017453292519943295f;
    const float cx = cosf(t.rx * DEG), sx = sinf(t.rx * DEG);
    const float cy = cosf(t.ry * DEG), sy = sinf(t.ry * DEG);
    const float cz = cosf(t.rz * DEG), sz = sinf(t.rz * DEG);
    m[0] = cz*cy; m[1] = cz*sy*sx - sz*cx; m[2] = cz*sy*cx + sz*sx;
    m[3] = sz*cy; m[4] = sz*sy*sx + cz*cx; m[5] = sz*sy*cx - cz*sx;
    m[6] = -sy;   m[7] = cy*sx;            m[8] = cy*cx;
}

// PT_HEIGHTFIELD: a terrain patch built from the recipe's stored samples.
//
// Vertices are NOT shared between triangles. Flat shading takes its brightness
// from vertices[tri.v1].normal, so a shared corner would hand some triangles a
// neighbour's face normal and the relief would light wrongly. Emitting three
// vertices per triangle with the true face normal is the only way to get
// per-face lighting; weld() afterwards merges any that genuinely coincide.
//
// Winding and vertex layout mirror createGrid exactly (row -> Z, col -> X,
// faces (v0,v3,v2) + (v2,v1,v0)) so the surface faces +Y under the default
// CULL_BACKFACES, and the DIAGONAL IS EXPLICIT AND CONSISTENT — a quad whose
// four corners differ in height is not planar, and picking the diagonal by
// accident would change the surface and hand FAST_Z a triangle whose single
// sort depth misrepresents it.
static Object* jm_createHeightfield(const JetMeshBuilder::Part& p)
{
    const int cols = p.c, rows = p.d;
    if (cols < 2 || rows < 2) return nullptr;
    if ((int)p.heights.size() < cols * rows) return nullptr;

    Object* o = new Object();
    const int32_t hw = p.a / 2, hd = p.b / 2;
    const int colDiv = cols - 1, rowDiv = rows - 1;

    // Reserve EXACTLY: two triangles per cell, three unshared vertices each.
    // Left to grow, the vertex vector doubles — a 38,400-vertex patch asks for
    // a 65,536-slot block (2.25 MB of Object::Vertex) while still holding the
    // 32,768-slot one it is outgrowing. That single allocation is what failed
    // on a device rebuild, where the largest free block is 3.62 MB rather than
    // the 5.12 MB available at boot.
    const size_t skirtTris = p.skirt > 0 ? (size_t)(colDiv + rowDiv) * 4 : 0;
    const size_t triCount  = (size_t)colDiv * rowDiv * 2 + skirtTris;
    o->vertices.reserve(triCount * 3);
    o->triangles.reserve(triCount);

    // Triangular lattice: odd rows are shifted half a cell in X, which is what
    // breaks the square grid's uniform diagonal and removes the directional
    // grain. Two things it deliberately does NOT do:
    //
    //  - No sqrt(3)/2 row compression. Equilateral cells would shrink the patch
    //    in Z and it would no longer fill its stated footprint. The triangles
    //    become isoceles instead, which is visually indistinguishable on
    //    terrain and keeps patches tileable, which matters far more.
    //  - The FIRST and LAST vertex of an odd row are NOT shifted. Shifting them
    //    would push the patch half a cell past its own footprint on one side
    //    and leave a notch on the other. Clamping them keeps the outer edge
    //    straight and identical to the even rows', so neighbouring patches and
    //    levels still butt exactly; only the two boundary cells of each odd row
    //    are slightly irregular, which no one can see on a hillside.
    const int32_t halfCell = p.a / (2 * colDiv);
    auto vx = [&](int r, int c) {
        int32_t x = (int32_t)(((int64_t)c * p.a) / colDiv) - hw;
        if (p.triRows && (r & 1) && c > 0 && c < colDiv) x += halfCell;
        return x;
    };
    auto vz = [&](int r) {
        return (int32_t)(((int64_t)r * p.b) / rowDiv) - hd;
    };
    auto vy = [&](int r, int c) {
        return (int32_t)p.heights[r * cols + c] << p.heightShift;
    };

    // UVs span the whole patch corner to corner, taken from the vertex's actual
    // X/Z so the triangular lattice's half-cell shift maps correctly too. The
    // renderer's UV space is 0..FIXED_POINT_SCALE, not 0..1.
    // A heightfield's natural UV is a planar XZ projection, which is right for
    // ground and WRONG for a cliff: a vertical face has nearly constant x/z, so
    // the texture smears into vertical streaks up it. The reference art's rock
    // faces are banded HORIZONTALLY (strata), which is what you get by taking
    // V from HEIGHT on those faces instead. U comes from x+z so it still varies
    // along the face whichever way it runs.
    // RAW, UNWRAPPED UVs. Wrapping happens per TRIANGLE in rebase3 below, never
    // per vertex — see the comment there for why that distinction is the whole
    // ball game.
    auto uvWall = [&](int32_t x, int32_t y, int32_t z, int64_t& u, int64_t& v) {
        if (!p.uvSpan || p.uvRepeat <= 0) { u = 0; v = 0; return; }
        u = ((int64_t)(x + z) * FIXED_POINT_SCALE) / p.uvRepeat;
        v = ((int64_t)(-y)    * FIXED_POINT_SCALE) / p.uvRepeat;
    };

    auto uvOf = [&](int32_t x, int32_t z, int64_t& u, int64_t& v) {
        if (!p.uvSpan) { u = 0; v = 0; return; }
        if (p.uvRepeat > 0) {
            // Fixed world scale: the texture tiles every uvRepeat units, so
            // detail size is a property of the material and not of the patch.
            // Left UNWRAPPED and unclamped; rebase3 fits it per triangle.
            u = ((int64_t)(x + hw) * FIXED_POINT_SCALE) / p.uvRepeat;
            v = ((int64_t)(z + hd) * FIXED_POINT_SCALE) / p.uvRepeat;
        } else {
            // Patch-span mapping: already 0..FIXED_POINT_SCALE corner to
            // corner, so it neither needs nor tolerates rebasing.
            int64_t uu = ((int64_t)(x + hw) * FIXED_POINT_SCALE) / (p.a ? p.a : 1);
            int64_t vv = ((int64_t)(z + hd) * FIXED_POINT_SCALE) / (p.b ? p.b : 1);
            if (uu < 0) uu = 0; if (uu > FIXED_POINT_SCALE) uu = FIXED_POINT_SCALE;
            if (vv < 0) vv = 0; if (vv > FIXED_POINT_SCALE) vv = FIXED_POINT_SCALE;
            u = uu; v = vv;
        }
    };

    // Fit a triangle's UVs into int16 by subtracting ONE COMMON multiple of
    // FIXED_POINT_SCALE from all of them.
    //
    // Texture::getPixel wraps mod FIXED_POINT_SCALE, so subtracting any
    // multiple of it is invisible — but it has to be the SAME multiple across
    // the triangle, or the interpolation between its vertices is destroyed.
    // Wrapping each vertex INDEPENDENTLY (mod 32768, which int16 storage
    // forces) is what broke: a 100,000-unit quad spans 12,800 UV units against
    // a 32,768 period, so a boundary landed inside 39% of quads per axis and
    // 63% of quads had at least one garbled axis. Such a triangle interpolates
    // BACKWARD through ~19.5 repeats instead of forward through 12.5, which
    // reads as the texture warping.
    //
    // Rebasing to the triangle's own minimum leaves values in
    // [0, span + FIXED_POINT_SCALE), and a quad's span is far inside int16.
    auto rebaseUV = [](int n, int64_t* vals, int16_t* out) {
        int64_t lo = vals[0];
        for (int i = 1; i < n; ++i) if (vals[i] < lo) lo = vals[i];
        // FLOOR to a multiple (not truncate) so negative coordinates work.
        const int64_t q = (lo >= 0)
            ? (lo / FIXED_POINT_SCALE)
            : -((-lo + FIXED_POINT_SCALE - 1) / FIXED_POINT_SCALE);
        const int64_t base = q * FIXED_POINT_SCALE;
        for (int i = 0; i < n; ++i) {
            int64_t r = vals[i] - base;
            // Only reachable if one triangle spans 32 texture repeats, which
            // no sane uvRepeat/quad pairing produces; clamped rather than
            // allowed to alias.
            if (r < -32768) r = -32768;
            if (r >  32767) r =  32767;
            out[i] = (int16_t)r;
        }
    };

    // Smooth per-vertex normals (see Part::smoothNormals): one normal per
    // LATTICE vertex from central differences of the stored heights, shared
    // by every face that touches the vertex. One-sided differences at the
    // patch edges. 1024-scale (FIXED_POINT_SCALE), normalised.
    std::vector<Vector3> latticeN;
    if (p.smoothNormals) {
        latticeN.resize((size_t)cols * rows);
        const float cw = (float)p.a / (float)colDiv;
        const float cd = (float)p.b / (float)rowDiv;
        for (int r = 0; r < rows; ++r) {
            const int rm = r > 0      ? r - 1 : r;
            const int rp = r < rowDiv ? r + 1 : r;
            for (int c = 0; c < cols; ++c) {
                const int cm = c > 0      ? c - 1 : c;
                const int cp = c < colDiv ? c + 1 : c;
                const float dhx = (float)(((int32_t)p.heights[r * cols + cp]
                                         - (int32_t)p.heights[r * cols + cm]) << p.heightShift);
                const float dhz = (float)(((int32_t)p.heights[rp * cols + c]
                                         - (int32_t)p.heights[rm * cols + c]) << p.heightShift);
                // Surface Y = h(X,Z); the upward normal is (-dh/dX, 1, -dh/dZ).
                const float nxf = -dhx / ((float)(cp - cm) * cw);
                const float nzf = -dhz / ((float)(rp - rm) * cd);
                const float il  = (float)FIXED_POINT_SCALE
                                / sqrtf(nxf * nxf + 1.0f + nzf * nzf);
                latticeN[(size_t)r * cols + c] =
                    Vector3((int32_t)(nxf * il), (int32_t)il, (int32_t)(nzf * il));
            }
        }
    }

    // Per-cell dihedral orientation for uvMix (see Part::uvMix). Set by the
    // cell loop before each cell's two emit() calls: bit 2 = mirror U,
    // bits 0-1 = quarter-turn count. 0 = identity.
    int cellUvXf = 0;
    // Per-cell ground material index into p.groundVariants (see there). Set by
    // the same cell-loop hash; 0 when the list is empty.
    int cellVar = 0;

    auto emit = [&](int r0,int c0, int r1,int c1, int r2,int c2) {
        int32_t x0=vx(r0,c0), y0=vy(r0,c0), z0=vz(r0);
        int32_t x1=vx(r1,c1), y1=vy(r1,c1), z1=vz(r1);
        int32_t x2=vx(r2,c2), y2=vy(r2,c2), z2=vz(r2);
        // Face normal via cross product, in float — the edge vectors can be
        // tens of thousands of world units and their products overflow int32.
        float nx, ny, nz;
        auto normal = [&]{
            const float ux=(float)(x1-x0), uy=(float)(y1-y0), uz=(float)(z1-z0);
            const float wx=(float)(x2-x0), wy=(float)(y2-y0), wz=(float)(z2-z0);
            nx = uy*wz - uz*wy; ny = uz*wx - ux*wz; nz = ux*wy - uy*wx;
        };
        normal();
        // A heightfield is BY DEFINITION an upward surface, so +Y is the
        // definition of its front face, not a property of my index order.
        // Orienting each face that way keeps both the square and triangular
        // layouts correct without a separate winding rule per layout.
        // The lattice coordinates travel with their corner through the flip
        // so smooth per-vertex normals stay attached to the right vertex.
        int cr1 = r1, cc1 = c1, cr2 = r2, cc2 = c2;
        if (ny < 0.0f) {
            int32_t tx=x1, ty=y1, tz=z1;
            x1=x2; y1=y2; z1=z2;  x2=tx; y2=ty; z2=tz;
            cr1=r2; cc1=c2; cr2=r1; cc2=c1;
            normal();
        }
        const float len = sqrtf(nx*nx + ny*ny + nz*nz);
        const float s = (len > 0.0f) ? (float)FIXED_POINT_SCALE / len : 0.0f;
        const Vector3 n((int32_t)(nx*s), (int32_t)(ny*s), (int32_t)(nz*s));

        // A face steeper than wallCos is a cliff, not ground: it takes the wall
        // material AND the height-based UV. One part therefore yields both
        // surfaces, which is the whole reason a terraced heightfield can read
        // as a mesa.
        const bool isWall = (p.wallCos > 0 && p.mat2 && n.y < p.wallCos);
        Material* fm = isWall ? p.mat2
                     : (p.groundVariants.empty() ? p.mat
                                                 : p.groundVariants[cellVar]);

        const uint16_t base = (uint16_t)o->vertices.size();
        int64_t ru[3], rv[3];
        if (isWall) {
            uvWall(x0,y0,z0,ru[0],rv[0]);
            uvWall(x1,y1,z1,ru[1],rv[1]);
            uvWall(x2,y2,z2,ru[2],rv[2]);
        } else {
            uvOf(x0,z0,ru[0],rv[0]);
            uvOf(x1,z1,ru[1],rv[1]);
            uvOf(x2,z2,ru[2],rv[2]);
            // uvMix: rotate/mirror this cell's window of the tiling pattern.
            // The pattern wraps mod FIXED_POINT_SCALE and the sampler masks,
            // so negated coordinates land on valid texels; a dihedral map is
            // linear, so interpolation and rebaseUV work unchanged. Never
            // applied to patch-span UVs (uvRepeat == 0): those map a specific
            // baked image corner to corner and must not be reoriented.
            if (cellUvXf != 0 && p.uvRepeat > 0) {
                for (int i = 0; i < 3; ++i) {
                    int64_t tu = ru[i], tv = rv[i];
                    if (cellUvXf & 4) tu = -tu;
                    switch (cellUvXf & 3) {
                    case 1: { const int64_t t = tu; tu = tv;  tv = -t; } break;
                    case 2: tu = -tu; tv = -tv;                         break;
                    case 3: { const int64_t t = tu; tu = -tv; tv = t; } break;
                    }
                    ru[i] = tu; rv[i] = tv;
                }
            }
        }
        int16_t uu[3], vv[3];
        rebaseUV(3, ru, uu);
        rebaseUV(3, rv, vv);
        const int16_t u0=uu[0], u1=uu[1], u2=uu[2];
        const int16_t v0=vv[0], v1=vv[1], v2=vv[2];
        // Ground faces take the smooth lattice normals when they exist; walls
        // keep the face normal so cliffs shade as crisp slabs (and because the
        // lattice stencil straddles the lip and would smear the strata).
        const bool sm = !latticeN.empty() && !isWall;
        const Vector3& n0 = sm ? latticeN[(size_t)r0  * cols + c0 ] : n;
        const Vector3& n1 = sm ? latticeN[(size_t)cr1 * cols + cc1] : n;
        const Vector3& n2 = sm ? latticeN[(size_t)cr2 * cols + cc2] : n;
        o->addVertex({ {x0,y0,z0}, {u0,v0}, n0 });
        o->addVertex({ {x1,y1,z1}, {u1,v1}, n1 });
        o->addVertex({ {x2,y2,z2}, {u2,v2}, n2 });
        o->addTriangle(base, base+1, base+2, fm);
    };

    const bool wantUvMix    = p.uvMix && p.uvRepeat > 0;
    const bool wantCellHash = wantUvMix || !p.groundVariants.empty();
    for (int r = 0; r < rows - 1; ++r) {
        for (int c = 0; c < cols - 1; ++c) {
            if (wantCellHash) {
                // Cell hash: the four raw corner heights anchor it to the
                // world (reproducible from the recipe alone); the index term
                // keeps dead-flat regions, where all four heights are equal,
                // from collapsing to one shared value. Bits 0-2 drive the
                // uvMix orientation, bits 3+ the ground variant, so the two
                // do not correlate.
                uint32_t h = (uint32_t)(uint16_t)p.heights[r * cols + c]           * 0x9E3779B1u
                           ^ (uint32_t)(uint16_t)p.heights[r * cols + c + 1]       * 0x85EBCA77u
                           ^ (uint32_t)(uint16_t)p.heights[(r + 1) * cols + c]     * 0xC2B2AE3Du
                           ^ (uint32_t)(uint16_t)p.heights[(r + 1) * cols + c + 1] * 0x27D4EB2Fu;
                h ^= (uint32_t)(r * 73 + c) * 0x9E3779B9u;
                h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
                cellUvXf = wantUvMix ? (int)(h & 7u) : 0;
                cellVar  = p.groundVariants.empty()
                         ? 0
                         : (int)((h >> 3) % (uint32_t)p.groundVariants.size());
            }
            if (!p.triRows) {
                emit(r,c,  r+1,c,  r+1,c+1);      // (v0, v3, v2)
                emit(r+1,c+1,  r,c+1,  r,c);      // (v2, v1, v0)
            } else if ((r & 1) == 0) {
                // Row r unshifted, row r+1 shifted right: the shifted vertex
                // sits between this row's pair, so each pair of triangles
                // alternates apex-down / apex-up.
                emit(r,c,   r+1,c,   r,c+1);
                emit(r+1,c, r+1,c+1, r,c+1);
            } else {
                emit(r,c,   r+1,c,   r+1,c+1);
                emit(r,c,   r+1,c+1, r,c+1);
            }
        }
    }

    // Perimeter skirt. See Part::skirt: a neighbouring patch at a different
    // sample density agrees at shared corners but not between them, and the
    // gap that leaves shows sky. A curtain hanging from the edge fills it with
    // terrain-coloured geometry, which is all the gap ever needed. It also
    // makes the level structure independent of the height function — nothing
    // has to be band-limited or morphed for the boundary to hold.
    if (p.skirt > 0) {
        auto curtain = [&](int32_t xa, int32_t ya, int32_t za,
                           int32_t xb, int32_t yb, int32_t zb,
                           float ox, float oz)
        {
            const float len = sqrtf(ox*ox + oz*oz);
            const float ns  = (len > 0.0f) ? (float)FIXED_POINT_SCALE / len : 0.0f;
            const Vector3 n((int32_t)(ox*ns), 0, (int32_t)(oz*ns));

            const int32_t yA = ya - p.skirt, yB = yb - p.skirt;
            // The winding that faces outward depends on which way the edge is
            // traversed, so test the cross product against the outward
            // direction rather than assuming a rule per side.
            const float cx = -(float)p.skirt * (float)(zb - za);
            const float cz =  (float)p.skirt * (float)(xb - xa);
            const bool flip = (cx*ox + cz*oz) < 0.0f;

            // A skirt is vertical, so it takes the wall mapping too. All four
            // corners rebase against ONE base (see rebaseUV): the curtain is a
            // single quad and its two triangles must agree along their shared
            // edge. The bottom pair needs its own V — reusing the top's would
            // smear one texel row down the whole drop.
            int64_t cu[4], cv[4];
            uvWall(xa, ya, za, cu[0], cv[0]);
            uvWall(xb, yb, zb, cu[1], cv[1]);
            uvWall(xa, yA, za, cu[2], cv[2]);
            uvWall(xb, yB, zb, cu[3], cv[3]);
            int16_t cuo[4], cvo[4];
            rebaseUV(4, cu, cuo);
            rebaseUV(4, cv, cvo);
            const int16_t ua=cuo[0], va=cvo[0], ub=cuo[1], vb=cvo[1];
            const int16_t uA=cuo[2], vA=cvo[2], uB=cuo[3], vB=cvo[3];

            auto tri = [&](int32_t px0,int32_t py0,int32_t pz0, int16_t tu0,int16_t tv0,
                           int32_t px1,int32_t py1,int32_t pz1, int16_t tu1,int16_t tv1,
                           int32_t px2,int32_t py2,int32_t pz2, int16_t tu2,int16_t tv2)
            {
                const uint16_t base = (uint16_t)o->vertices.size();
                o->addVertex({ {px0,py0,pz0}, {tu0,tv0}, n });
                if (flip) {
                    o->addVertex({ {px2,py2,pz2}, {tu2,tv2}, n });
                    o->addVertex({ {px1,py1,pz1}, {tu1,tv1}, n });
                } else {
                    o->addVertex({ {px1,py1,pz1}, {tu1,tv1}, n });
                    o->addVertex({ {px2,py2,pz2}, {tu2,tv2}, n });
                }
                // A skirt is vertical by definition, so it is wall.
                o->addTriangle(base, base+1, base+2,
                               (p.wallCos > 0 && p.mat2) ? p.mat2 : p.mat);
            };


            tri(xa,ya,za, ua,va,  xa,yA,za, uA,vA,  xb,yB,zb, uB,vB);
            tri(xa,ya,za, ua,va,  xb,yB,zb, uB,vB,  xb,yb,zb, ub,vb);
        };

        for (int c = 0; c < colDiv; ++c) {
            const int rl = rows - 1;
            curtain(vx(0,c),  vy(0,c),  vz(0),
                    vx(0,c+1),vy(0,c+1),vz(0),   0.0f, -1.0f);
            curtain(vx(rl,c), vy(rl,c), vz(rl),
                    vx(rl,c+1),vy(rl,c+1),vz(rl), 0.0f,  1.0f);
        }
        for (int r = 0; r < rowDiv; ++r) {
            const int cl = cols - 1;
            curtain(vx(r,0),  vy(r,0),  vz(r),
                    vx(r+1,0),vy(r+1,0),vz(r+1), -1.0f, 0.0f);
            curtain(vx(r,cl), vy(r,cl), vz(r),
                    vx(r+1,cl),vy(r+1,cl),vz(r+1), 1.0f, 0.0f);
        }
    }

    o->calculateBoundingBox();
    return o;
}

// See PT_RIBBON. Unshared vertices with the face normal, exactly like the
// heightfield's emit: FLAT shading reads v1's normal, so shared corners
// would light neighbouring faces with each other's normals.
static Object* jm_createRibbon(const JetMeshBuilder::Part& p)
{
    const int n = (int)(p.ribbonPts.size() / 6);
    if (n < 2) return nullptr;
    const int quads = p.ribbonClosed ? n : n - 1;

    Object* o = new Object();
    o->vertices.reserve((size_t)quads * 6);
    o->triangles.reserve((size_t)quads * 2);

    auto emit = [&](const int32_t* a, const int32_t* b, const int32_t* c) {
        float ux = (float)(b[0]-a[0]), uy = (float)(b[1]-a[1]), uz = (float)(b[2]-a[2]);
        float wx = (float)(c[0]-a[0]), wy = (float)(c[1]-a[1]), wz = (float)(c[2]-a[2]);
        float nx = uy*wz - uz*wy, ny = uz*wx - ux*wz, nz = ux*wy - uy*wx;
        if (ny < 0.0f) {                      // face upward, like a heightfield
            const int32_t* t = b; b = c; c = t;
            nx = -nx; ny = -ny; nz = -nz;
        }
        const float len = sqrtf(nx*nx + ny*ny + nz*nz);
        const float s = (len > 0.0f) ? (float)FIXED_POINT_SCALE / len : 0.0f;
        const Vector3 fn((int32_t)(nx*s), (int32_t)(ny*s), (int32_t)(nz*s));
        const uint16_t base = (uint16_t)o->vertices.size();
        o->addVertex({ {a[0],a[1],a[2]}, {0,0}, fn });
        o->addVertex({ {b[0],b[1],b[2]}, {0,0}, fn });
        o->addVertex({ {c[0],c[1],c[2]}, {0,0}, fn });
        o->addTriangle(base, base+1, base+2, p.mat);
    };

    for (int i = 0; i < quads; ++i) {
        const int j = (i + 1) % n;
        const int32_t* Li = &p.ribbonPts[(size_t)i * 6];
        const int32_t* Ri = Li + 3;
        const int32_t* Lj = &p.ribbonPts[(size_t)j * 6];
        const int32_t* Rj = Lj + 3;
        emit(Li, Lj, Rj);
        emit(Rj, Ri, Li);
    }
    return o;
}

static Object* jm_create(const JetMeshBuilder::Part& p)
{
    if (p.type == JetMeshBuilder::PT_HEIGHTFIELD)
        return jm_createHeightfield(p);
    if (p.type == JetMeshBuilder::PT_RIBBON)
        return jm_createRibbon(p);
    switch (p.type) {
        case JetMeshBuilder::PT_CUBE:
            return Primitives::createCube(p.a, p.b, p.c, p.mat);
        case JetMeshBuilder::PT_SPHERE:
            return Primitives::createSphere(p.a, p.b, p.mat);
        case JetMeshBuilder::PT_PLANE:
            return Primitives::createPlane(p.a, p.b, p.mat);
        case JetMeshBuilder::PT_PYRAMID:
            return Primitives::createPyramid(p.a, p.b, p.mat);
        case JetMeshBuilder::PT_CYLINDER:
            return Primitives::createCylinder(p.a, p.b, p.c, p.caps, p.mat, p.d);
        case JetMeshBuilder::PT_CAPSULE:
            return Primitives::createCapsule(p.a, p.b, p.c, p.mat);
        case JetMeshBuilder::PT_QUAD:
            return Primitives::createQuad(p.a, p.b, p.mat);
        case JetMeshBuilder::PT_GRID:
            return Primitives::createGrid(p.a, p.b, p.c, p.d, p.mat,
                                          p.mat2 ? p.mat2 : p.mat, false);
    }
    return nullptr;
}

bool JetMeshBuilder::evaluate()
{
    verts.clear(); tris.clear(); matParams.clear(); mats.clear();
    lastStats = Stats();

    for (size_t pi = 0; pi < parts.size(); ++pi) {
        const Part& part = parts[pi];
        Object* src = jm_create(part);
        if (!src) return false;

        float m[9];
        jm_rotation(part.xf, m);
        const Transform& t = part.xf;
        const uint32_t base = (uint32_t)verts.size();
        // The source object's size is known, so grow to exactly what this part
        // needs instead of doubling into a block twice the size while still
        // holding the old one. On a big terrain patch that transient double is
        // megabytes, and it is what makes a rebuild fail where a boot-time
        // build succeeds.
        verts.reserve(base + src->vertices.size());
        tris.reserve(tris.size() + src->triangles.size());

        for (size_t i = 0; i < src->vertices.size(); ++i) {
            const Object::Vertex& sv = src->vertices[i];

            const float px = (float)sv.position.x * t.sx;
            const float py = (float)sv.position.y * t.sy;
            const float pz = (float)sv.position.z * t.sz;

            BVert v = {};
            v.px = jm_round(px*m[0] + py*m[1] + pz*m[2] + t.tx);
            v.py = jm_round(px*m[3] + py*m[4] + pz*m[5] + t.ty);
            v.pz = jm_round(px*m[6] + py*m[7] + pz*m[8] + t.tz);

            // Normals: inverse scale (inverse-transpose of a diagonal),
            // rotate, renormalise to 1024 — same rule as the runtime
            // transformScale path.
            float nx = (float)sv.normal.x / (t.sx != 0 ? t.sx : 1);
            float ny = (float)sv.normal.y / (t.sy != 0 ? t.sy : 1);
            float nz = (float)sv.normal.z / (t.sz != 0 ? t.sz : 1);
            const float rnx = nx*m[0] + ny*m[1] + nz*m[2];
            const float rny = nx*m[3] + ny*m[4] + nz*m[5];
            const float rnz = nx*m[6] + ny*m[7] + nz*m[8];
            const float len = sqrtf(rnx*rnx + rny*rny + rnz*rnz);
            const float k = (len > 0.001f) ? 1024.0f / len : 0.0f;
            v.nx = (int16_t)jm_round(rnx * k);
            v.ny = (int16_t)jm_round(rny * k);
            v.nz = (int16_t)jm_round(rnz * k);

            v.u = (int16_t)sv.uv.x;
            v.v = (int16_t)sv.uv.y;
            // The pad doubles as the bone tag during the pipeline: it is part
            // of weld()'s memcmp key, so vertices of different bones can never
            // merge — each bone's geometry must stay a contiguous, separately
            // transformable unit. Zero for unrigged recipes (single bone).
            v._pad = (int16_t)part.bone;
            verts.push_back(v);
        }

        for (size_t i = 0; i < src->triangles.size(); ++i) {
            const Object::Triangle& st = src->triangles[i];
            Material* mm = st.material;

            int mi = -1;
            for (size_t k = 0; k < mats.size(); ++k)
                if (mats[k] == mm) { mi = (int)k; break; }
            if (mi < 0) {
                mi = (int)mats.size();
                mats.push_back(mm);
                JetMatParams mp;
                mp.color    = mm ? mm->color : 0xFFFF;
                mp.alpha    = mm ? mm->alpha : 255;
                mp.diffuse  = mm ? mm->diffuse : 255;
                mp.specular = mm ? mm->specular : 0;
                mp.emissive = mm ? (mm->emissive ? 1 : 0) : 0;
                mp.shading  = mm ? (uint8_t)mm->shadingMode : 0;
                matParams.push_back(mp);
            }

            BTri bt;
            bt.a = base + st.v1; bt.b = base + st.v2; bt.c = base + st.v3;
            bt.mat  = (uint16_t)mi;
            bt.part = (uint16_t)pi;
            tris.push_back(bt);
        }

        delete src;
    }

    dirty = false;
    return !verts.empty() && !tris.empty();
}

// ---------------------------------------------------------------------------
// Interior culling
// ---------------------------------------------------------------------------

namespace {

struct Solid {
    uint16_t part;
    uint16_t bone;
    JetMeshBuilder::PartType type;
    float m[9];               // forward rotation; inverse = transpose
    float tx, ty, tz;
    float isx, isy, isz;      // 1 / scale
    float A, B;               // canonical params (local units, see tests)
    float C;
    float eps;                // local-space burial margin
};

// Solid volumes only: open surfaces have no inside, and a cylinder without
// caps is an open tube.
static bool jm_isSolid(const JetMeshBuilder::Part& p)
{
    switch (p.type) {
        case JetMeshBuilder::PT_CUBE:
        case JetMeshBuilder::PT_SPHERE:
        case JetMeshBuilder::PT_PYRAMID:
        case JetMeshBuilder::PT_CAPSULE:
            return true;
        case JetMeshBuilder::PT_CYLINDER:
            return p.caps;
        default:
            return false;
    }
}

static bool jm_inside(const Solid& s, float wx, float wy, float wz)
{
    // Forward was T * R * S, so the inverse is S^-1 * R^T * (p - T).
    const float dx = wx - s.tx, dy = wy - s.ty, dz = wz - s.tz;
    float lx = (s.m[0]*dx + s.m[3]*dy + s.m[6]*dz) * s.isx;
    float ly = (s.m[1]*dx + s.m[4]*dy + s.m[7]*dz) * s.isy;
    float lz = (s.m[2]*dx + s.m[5]*dy + s.m[8]*dz) * s.isz;
    const float e = s.eps;

    switch (s.type) {
        case JetMeshBuilder::PT_CUBE:
            return fabsf(lx) < s.A - e && fabsf(ly) < s.B - e &&
                   fabsf(lz) < s.C - e;
        case JetMeshBuilder::PT_SPHERE: {
            const float r = s.A - e;
            return lx*lx + ly*ly + lz*lz < r * r;
        }
        case JetMeshBuilder::PT_CYLINDER: {
            const float r = s.A - e;
            return fabsf(ly) < s.B - e && lx*lx + lz*lz < r * r;
        }
        case JetMeshBuilder::PT_CAPSULE: {
            const float r = s.A - e;
            float oy = fabsf(ly) - s.B;
            if (oy < 0) oy = 0;
            return lx*lx + oy*oy + lz*lz < r * r;
        }
        case JetMeshBuilder::PT_PYRAMID: {
            // Base at y=0, apex at y=B; the cross-section shrinks linearly.
            if (ly < e || ly > s.B - e) return false;
            const float half = s.A * (1.0f - ly / s.B) - e;
            return half > 0 && fabsf(lx) < half && fabsf(lz) < half;
        }
        default:
            return false;
    }
}

struct CullCtx {
    std::vector<Solid> solids;
    std::vector<JetMeshBuilder::BVert>* verts;
    std::vector<JetMeshBuilder::BTri>   out;
    float  minEdge2;
    int    dropped;
    size_t vertCap;
};

// "Foreign" means: a DIFFERENT part of the SAME bone. Parts of one bone are
// rigid relative to each other forever, so burial between them is permanent
// and safe to cull. Parts of different bones MOVE relative to each other —
// geometry buried in the rest pose can emerge when the rig is posed, so
// cross-bone culling would eat visible animation frames. Unrigged recipes put
// everything on bone 0, which reduces this to the plain any-other-part rule.
static inline bool jm_buriedByForeign(const CullCtx& cx, uint16_t part,
                                      uint16_t bone, float x, float y, float z)
{
    for (size_t i = 0; i < cx.solids.size(); ++i) {
        const Solid& s = cx.solids[i];
        if (s.part == part || s.bone != bone) continue;
        if (jm_inside(s, x, y, z)) return true;
    }
    return false;
}

static uint32_t jm_midVert(CullCtx& cx, const JetMeshBuilder::BVert& a,
                           const JetMeshBuilder::BVert& b)
{
    JetMeshBuilder::BVert v = {};
    v.px = (int32_t)(((int64_t)a.px + b.px) / 2);
    v.py = (int32_t)(((int64_t)a.py + b.py) / 2);
    v.pz = (int32_t)(((int64_t)a.pz + b.pz) / 2);
    float nx = (float)(a.nx + b.nx), ny = (float)(a.ny + b.ny),
          nz = (float)(a.nz + b.nz);
    const float len = sqrtf(nx*nx + ny*ny + nz*nz);
    const float k = (len > 0.001f) ? 1024.0f / len : 0.0f;
    v.nx = (int16_t)jm_round(nx * k);
    v.ny = (int16_t)jm_round(ny * k);
    v.nz = (int16_t)jm_round(nz * k);
    v.u = (int16_t)((a.u + b.u) / 2);
    v.v = (int16_t)((a.v + b.v) / 2);
    cx.verts->push_back(v);
    return (uint32_t)(cx.verts->size() - 1);
}

// Bisection halves ONE edge per level (the 4-split halved all three), so the
// depth limit is doubled to reach the same refinement floor.
static const int JM_CULL_MAX_DEPTH = 12;

static void jm_processTri(CullCtx& cx, uint32_t ia, uint32_t ib, uint32_t ic,
                          uint16_t mat, uint16_t part, uint16_t bone, int depth)
{
    // Copies, not references: midpoint appends below can reallocate verts.
    const JetMeshBuilder::BVert A = (*cx.verts)[ia];
    const JetMeshBuilder::BVert B = (*cx.verts)[ib];
    const JetMeshBuilder::BVert C = (*cx.verts)[ic];

    const float ax = (float)A.px, ay = (float)A.py, az = (float)A.pz;
    const float bx = (float)B.px, by = (float)B.py, bz = (float)B.pz;
    const float cxx = (float)C.px, cy = (float)C.py, cz = (float)C.pz;

    // Fully buried: one convex solid (of another part) containing all three
    // corners provably contains the whole triangle.
    for (size_t i = 0; i < cx.solids.size(); ++i) {
        const Solid& s = cx.solids[i];
        if (s.part == part || s.bone != bone) continue;
        if (jm_inside(s, ax, ay, az) && jm_inside(s, bx, by, bz) &&
            jm_inside(s, cxx, cy, cz)) {
            cx.dropped++;
            return;
        }
    }

    // Straddle probes: corners, centroid, edge midpoints. The centroid and
    // midpoints matter because a long triangle can pass straight through a
    // solid with all three corners outside.
    const bool touched =
        jm_buriedByForeign(cx, part, bone, ax, ay, az) ||
        jm_buriedByForeign(cx, part, bone, bx, by, bz) ||
        jm_buriedByForeign(cx, part, bone, cxx, cy, cz) ||
        jm_buriedByForeign(cx, part, bone, (ax+bx+cxx)/3, (ay+by+cy)/3, (az+bz+cz)/3) ||
        jm_buriedByForeign(cx, part, bone, (ax+bx)/2, (ay+by)/2, (az+bz)/2) ||
        jm_buriedByForeign(cx, part, bone, (bx+cxx)/2, (by+cy)/2, (bz+cz)/2) ||
        jm_buriedByForeign(cx, part, bone, (cxx+ax)/2, (cy+ay)/2, (cz+az)/2);

    if (!touched) {
        cx.out.push_back({ ia, ib, ic, mat, part });
        return;
    }

    // Straddler: refine until the tolerance floor. At the floor the sliver is
    // decided by its CENTROID: buried -> dropped, outside -> kept. Keeping
    // every floor sliver is the no-holes-ever rule, but it doubles the skirt
    // and exploded the demo rocket 22x; the centroid rule instead bounds any
    // gap at half the tolerance, along a seam the intersecting part's own
    // surface covers. That is the accepted trade — tolerance is the knob.
    const float e0 = (bx-ax)*(bx-ax) + (by-ay)*(by-ay) + (bz-az)*(bz-az);
    const float e1 = (cxx-bx)*(cxx-bx) + (cy-by)*(cy-by) + (cz-bz)*(cz-bz);
    const float e2 = (ax-cxx)*(ax-cxx) + (ay-cy)*(ay-cy) + (az-cz)*(az-cz);
    float longest = e0 > e1 ? e0 : e1;
    if (e2 > longest) longest = e2;

    if (depth >= JM_CULL_MAX_DEPTH || longest < cx.minEdge2 ||
        cx.verts->size() > cx.vertCap) {
        if (jm_buriedByForeign(cx, part, bone,
                               (ax+bx+cxx)/3, (ay+by+cy)/3, (az+bz+cz)/3)) {
            cx.dropped++;
        } else {
            cx.out.push_back({ ia, ib, ic, mat, part });
        }
        return;
    }

    // LONGEST-EDGE BISECTION, not a 4-split. Each refinement level emits one
    // clean sibling instead of three: the demo rocket bakes to 656 triangles
    // where the 4-split produced 1,694, at the same tolerance and with the
    // same zero buried-area result. The depth limit is doubled to compensate
    // (bisection halves one edge per level where the 4-split halved all
    // three). Orientation is preserved by replacing the longest edge's
    // endpoints in place.
    const uint32_t im = (e0 >= e1 && e0 >= e2) ? jm_midVert(cx, A, B)
                      : (e1 >= e2)             ? jm_midVert(cx, B, C)
                                               : jm_midVert(cx, C, A);
    if (e0 >= e1 && e0 >= e2) {
        jm_processTri(cx, ia, im, ic, mat, part, bone, depth + 1);
        jm_processTri(cx, im, ib, ic, mat, part, bone, depth + 1);
    } else if (e1 >= e2) {
        jm_processTri(cx, ia, ib, im, mat, part, bone, depth + 1);
        jm_processTri(cx, ia, im, ic, mat, part, bone, depth + 1);
    } else {
        jm_processTri(cx, ia, ib, im, mat, part, bone, depth + 1);
        jm_processTri(cx, im, ib, ic, mat, part, bone, depth + 1);
    }
}

} // namespace

int JetMeshBuilder::cull(float toleranceWorld)
{
    if (tris.empty() || parts.size() < 2) return 0;

    CullCtx cx;
    cx.verts   = &verts;
    cx.dropped = 0;
    cx.vertCap = 60000;   // stop refining before u16 indexing is at risk
    if (toleranceWorld < 1.0f) toleranceWorld = 1.0f;
    cx.minEdge2 = toleranceWorld * toleranceWorld;
    const float eps = toleranceWorld * 0.25f;

    for (size_t pi = 0; pi < parts.size(); ++pi) {
        const Part& p = parts[pi];
        if (!jm_isSolid(p)) continue;

        Solid s;
        s.part = (uint16_t)pi;
        s.bone = p.bone;
        s.type = p.type;
        jm_rotation(p.xf, s.m);
        s.tx = p.xf.tx; s.ty = p.xf.ty; s.tz = p.xf.tz;
        const float sx = (p.xf.sx != 0) ? p.xf.sx : 1;
        const float sy = (p.xf.sy != 0) ? p.xf.sy : 1;
        const float sz = (p.xf.sz != 0) ? p.xf.sz : 1;
        s.isx = 1.0f / sx; s.isy = 1.0f / sy; s.isz = 1.0f / sz;

        switch (p.type) {
            case PT_CUBE:     s.A = p.a * 0.5f; s.B = p.b * 0.5f; s.C = p.c * 0.5f; break;
            case PT_SPHERE:   s.A = (float)p.a; s.B = 0; s.C = 0; break;
            case PT_CYLINDER: s.A = (float)p.a; s.B = p.b * 0.5f; s.C = 0; break;
            case PT_CAPSULE:  s.A = (float)p.a; s.B = p.b * 0.5f; s.C = 0; break;
            case PT_PYRAMID:  s.A = p.a * 0.5f; s.B = (float)p.b; s.C = 0; break;
            default: continue;
        }
        // Epsilon is applied in LOCAL units; divide by the smallest scale so
        // the margin is at least `eps` world units on every axis — the
        // conservative direction (a bigger local margin keeps more).
        float smin = sx < sy ? sx : sy;
        if (sz < smin) smin = sz;
        if (smin <= 0) smin = 1;
        s.eps = eps / smin;

        cx.solids.push_back(s);
    }
    if (cx.solids.empty()) return 0;

    const int before = (int)tris.size();
    cx.out.reserve(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        const BTri t = tris[i];
        jm_processTri(cx, t.a, t.b, t.c, t.mat, t.part,
                      parts[t.part].bone, 0);
    }

    tris.swap(cx.out);
    lastStats.trisDropped = cx.dropped;
    lastStats.trisAdded   = (int)tris.size() - (before - cx.dropped);
    if (lastStats.trisAdded < 0) lastStats.trisAdded = 0;
    return before - (int)tris.size();
}

// ---------------------------------------------------------------------------
// Max-edge subdivision
// ---------------------------------------------------------------------------

namespace {
// Midpoint vertex, normals averaged and renormalised to 1024. Same rule as the
// cull refiner's, kept separate because that one carries a CullCtx.
static uint32_t jm_mid(std::vector<JetMeshBuilder::BVert>& v,
                       const JetMeshBuilder::BVert& a,
                       const JetMeshBuilder::BVert& b)
{
    JetMeshBuilder::BVert m = {};
    m.px = (int32_t)(((int64_t)a.px + b.px) / 2);
    m.py = (int32_t)(((int64_t)a.py + b.py) / 2);
    m.pz = (int32_t)(((int64_t)a.pz + b.pz) / 2);
    float nx = (float)(a.nx + b.nx), ny = (float)(a.ny + b.ny),
          nz = (float)(a.nz + b.nz);
    const float len = sqrtf(nx*nx + ny*ny + nz*nz);
    const float k = (len > 0.001f) ? 1024.0f / len : 0.0f;
    m.nx = (int16_t)jm_round(nx * k);
    m.ny = (int16_t)jm_round(ny * k);
    m.nz = (int16_t)jm_round(nz * k);
    m.u = (int16_t)((a.u + b.u) / 2);
    m.v = (int16_t)((a.v + b.v) / 2);
    m._pad = a._pad;              // stays within one bone
    v.push_back(m);
    return (uint32_t)(v.size() - 1);
}
} // namespace

int JetMeshBuilder::subdivide(float maxEdgeWorld)
{
    if (maxEdgeWorld <= 0 || tris.empty()) return 0;
    const float lim2 = maxEdgeWorld * maxEdgeWorld;
    const int before = (int)tris.size();

    std::vector<BTri> work;
    work.swap(tris);
    tris.reserve(before * 2);

    // Iterative worklist rather than recursion: a big floor quad can bisect
    // many times and the module task's stack is a scarce, shared resource.
    size_t guard = 0;
    const size_t guardMax = (size_t)before * 256 + 4096;
    while (!work.empty()) {
        if (++guard > guardMax || verts.size() > 60000) {
            // Bail safely: emit the remainder unsplit rather than overrun
            // uint16 indexing or spin.
            for (size_t i = 0; i < work.size(); ++i) tris.push_back(work[i]);
            break;
        }
        const BTri t = work.back();
        work.pop_back();

        const BVert& A = verts[t.a];
        const BVert& B = verts[t.b];
        const BVert& C = verts[t.c];
        const float e0 = (float)(B.px-A.px)*(B.px-A.px) + (float)(B.py-A.py)*(B.py-A.py) + (float)(B.pz-A.pz)*(B.pz-A.pz);
        const float e1 = (float)(C.px-B.px)*(C.px-B.px) + (float)(C.py-B.py)*(C.py-B.py) + (float)(C.pz-B.pz)*(C.pz-B.pz);
        const float e2 = (float)(A.px-C.px)*(A.px-C.px) + (float)(A.py-C.py)*(A.py-C.py) + (float)(A.pz-C.pz)*(A.pz-C.pz);
        float longest = e0 > e1 ? e0 : e1;
        if (e2 > longest) longest = e2;
        if (longest <= lim2) { tris.push_back(t); continue; }

        // Bisect the longest edge, preserving winding.
        if (e0 >= e1 && e0 >= e2) {
            const uint32_t m = jm_mid(verts, verts[t.a], verts[t.b]);
            work.push_back({ t.a, m, t.c, t.mat, t.part });
            work.push_back({ m, t.b, t.c, t.mat, t.part });
        } else if (e1 >= e2) {
            const uint32_t m = jm_mid(verts, verts[t.b], verts[t.c]);
            work.push_back({ t.a, t.b, m, t.mat, t.part });
            work.push_back({ t.a, m, t.c, t.mat, t.part });
        } else {
            const uint32_t m = jm_mid(verts, verts[t.c], verts[t.a]);
            work.push_back({ t.a, t.b, m, t.mat, t.part });
            work.push_back({ m, t.b, t.c, t.mat, t.part });
        }
    }
    return (int)tris.size() - before;
}

// ---------------------------------------------------------------------------
// Weld
// ---------------------------------------------------------------------------

// Open-addressed hash of vertex -> first index. Vertices are already integers
// (positions world-scaled, normals 1024-scale, uv 1024-scale), so equality is
// exact — no epsilon, no tolerance tuning, and flat-shaded facets survive
// because their shared positions carry different normals.
int JetMeshBuilder::weld()
{
    const size_t n = verts.size();
    if (n < 2) return 0;

    size_t cap = 1;
    while (cap < n * 2) cap <<= 1;
    std::vector<int32_t> table(cap, -1);
    std::vector<uint32_t> remap(n);

    // The table stores COMPACTED indices and comparisons read the compacted
    // prefix verts[0..out-1], which this loop never overwrites. The previous
    // version stored ORIGINAL indices while compacting the same array in
    // place: once `out` overtook a referenced original slot, its bytes
    // belonged to a DIFFERENT (later) vertex, a duplicate probe could
    // memcmp-match that impostor, and the remap wired the triangle corner to
    // an arbitrary earlier vertex — one corner of one triangle snapping to a
    // far-away point ("wall triangle stretched across the curve",
    // hw 2026-08-05, reproduced deterministically by scratchpad/wallaudit).
    size_t out = 0;
    for (size_t i = 0; i < n; ++i) {
        const BVert v = verts[i];
        uint32_t h = 2166136261u;
        const uint8_t* p = (const uint8_t*)&v;
        for (size_t b = 0; b < sizeof(BVert); ++b) h = (h ^ p[b]) * 16777619u;

        size_t slot = h & (cap - 1);
        int32_t found = -1;
        while (table[slot] >= 0) {
            const BVert& w = verts[(size_t)table[slot]];
            if (memcmp(&w, &v, sizeof(BVert)) == 0) { found = table[slot]; break; }
            slot = (slot + 1) & (cap - 1);
        }

        if (found >= 0) {
            remap[i] = (uint32_t)found;
        } else {
            verts[out] = v;
            table[slot] = (int32_t)out;
            remap[i] = (uint32_t)out;
            ++out;
        }
    }
    const int removed = (int)(n - out);
    verts.resize(out);

    size_t tout = 0;
    for (size_t i = 0; i < tris.size(); ++i) {
        BTri t = tris[i];
        t.a = remap[t.a]; t.b = remap[t.b]; t.c = remap[t.c];
        // A triangle whose corners collapsed together has no area; welding
        // itself cannot merge distinct corners of a valid triangle (their
        // positions differ), so anything degenerate here was degenerate input.
        if (t.a == t.b || t.b == t.c || t.a == t.c) continue;
        tris[tout++] = t;
    }
    tris.resize(tout);
    lastStats.vertsWelded = removed;
    return removed;
}

// ---------------------------------------------------------------------------
// Spatial cells
// ---------------------------------------------------------------------------

int JetMeshBuilder::splitCells(int32_t cellSizeWorld)
{
    cells.clear();
    if (cellSizeWorld <= 0 || tris.empty() || verts.empty()) return 1;

    int32_t mn[3] = { INT32_MAX, INT32_MAX, INT32_MAX };
    int32_t mx[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
    for (size_t i = 0; i < verts.size(); ++i) {
        const int32_t p[3] = { verts[i].px, verts[i].py, verts[i].pz };
        for (int a = 0; a < 3; ++a) {
            if (p[a] < mn[a]) mn[a] = p[a];
            if (p[a] > mx[a]) mx[a] = p[a];
        }
    }
    int32_t dim[3];
    for (int a = 0; a < 3; ++a) {
        dim[a] = (mx[a] - mn[a]) / cellSizeWorld + 1;
        if (dim[a] < 1) dim[a] = 1;
        if (dim[a] > 64) dim[a] = 64;   // keep the key space sane
    }
    // See Pipeline::cellSplit2D. A heightfield must never be cut by height:
    // the cell centre is what every distance test downstream measures to.
    if (pipe.cellSplit2D) dim[1] = 1;
    const int64_t total = (int64_t)dim[0] * dim[1] * dim[2];
    if (total <= 1 || total > 4096) return 1;   // not worth splitting

    // Bucket triangles by centroid cell.
    std::vector<uint32_t> key(tris.size());
    std::vector<uint32_t> used(total, 0xFFFFFFFFu);
    std::vector<uint32_t> order;
    order.reserve(tris.size());
    uint32_t nCells = 0;
    for (size_t i = 0; i < tris.size(); ++i) {
        const BVert& a = verts[tris[i].a];
        const BVert& b = verts[tris[i].b];
        const BVert& c = verts[tris[i].c];
        const int32_t cx = (a.px + b.px + c.px) / 3;
        const int32_t cy = (a.py + b.py + c.py) / 3;
        const int32_t cz = (a.pz + b.pz + c.pz) / 3;
        int32_t ix = (cx - mn[0]) / cellSizeWorld;
        int32_t iy = pipe.cellSplit2D ? 0 : (cy - mn[1]) / cellSizeWorld;
        int32_t iz = (cz - mn[2]) / cellSizeWorld;
        if (ix < 0) ix = 0; if (ix >= dim[0]) ix = dim[0] - 1;
        if (iy < 0) iy = 0; if (iy >= dim[1]) iy = dim[1] - 1;
        if (iz < 0) iz = 0; if (iz >= dim[2]) iz = dim[2] - 1;
        key[i] = (uint32_t)((iz * dim[1] + iy) * dim[0] + ix);
        if (used[key[i]] == 0xFFFFFFFFu) used[key[i]] = nCells++;
    }
    if (nCells < 2) return 1;

    // Rebuild cell-major with per-cell vertex ownership. A vertex on a cell
    // boundary is duplicated into every cell that references it, which is what
    // makes each cell a self-contained borrowable range.
    std::vector<BVert> nv;
    std::vector<BTri>  nt;
    nv.reserve(verts.size() + verts.size() / 4);
    nt.reserve(tris.size());
    std::vector<uint32_t> remap(verts.size());
    std::vector<uint32_t> stamp(verts.size(), 0xFFFFFFFFu);

    for (uint32_t cellSeq = 0; cellSeq < nCells; ++cellSeq) {
        JmshCellRec rec;
        rec.vertOff = (uint32_t)nv.size();
        rec.triOff  = (uint32_t)nt.size();
        for (size_t i = 0; i < tris.size(); ++i) {
            if (used[key[i]] != cellSeq) continue;
            const BTri& t = tris[i];
            const uint32_t idx[3] = { t.a, t.b, t.c };
            uint32_t local[3];
            for (int k = 0; k < 3; ++k) {
                if (stamp[idx[k]] != cellSeq) {
                    stamp[idx[k]] = cellSeq;
                    remap[idx[k]] = (uint32_t)(nv.size() - rec.vertOff);
                    nv.push_back(verts[idx[k]]);
                }
                local[k] = remap[idx[k]];
            }
            BTri o = t;
            o.a = local[0]; o.b = local[1]; o.c = local[2];
            nt.push_back(o);
        }
        rec.vertCount = (uint32_t)nv.size() - rec.vertOff;
        rec.triCount  = (uint32_t)nt.size() - rec.triOff;
        if (rec.vertCount > 65535) { cells.clear(); return 1; }
        cells.push_back(rec);
    }

    verts.swap(nv);
    tris.swap(nt);
    return (int)cells.size();
}

// ---------------------------------------------------------------------------
// Primitive-native LOD
// ---------------------------------------------------------------------------

// A part's THINNEST dimension — what `minPart` compares against.
//
// Thinnest, not longest, because thinness is what makes a part stop being
// worth drawing at distance: a 96x8x8 fin bar is a major feature by length but
// vanishes to a hairline, while its parent body is thick in every axis. Using
// the longest dimension made the two indistinguishable (96 vs 90) so no
// threshold could drop one and keep the other.
//
// Hazard worth knowing: a large thin slab (a wall, a floor plate) is also
// "thin" by this measure and will be dropped by a coarse minPart. Pick minPart
// relative to the detail you mean to lose, not to the scene scale.
static int32_t jm_partExtent(const JetMeshBuilder::Part& p)
{
    auto mn3 = [](int32_t a, int32_t b, int32_t c) {
        int32_t m = a < b ? a : b;
        return m < c ? m : c;
    };
    switch (p.type) {
        case JetMeshBuilder::PT_CUBE:     return mn3(p.a, p.b, p.c);
        case JetMeshBuilder::PT_SPHERE:   return 2 * p.a;
        case JetMeshBuilder::PT_CYLINDER: return (2*p.a < p.b) ? 2*p.a : p.b;
        case JetMeshBuilder::PT_CAPSULE:  return 2 * p.a;   // waist thickness
        case JetMeshBuilder::PT_PYRAMID:  return (p.a < p.b) ? p.a : p.b;
        default:                          return (p.a < p.b) ? p.a : p.b;
    }
}

bool JetMeshBuilder::lodPart(const Part& in, const LodSpec& s, Part& out)
{
    if (s.minPart > 0 && jm_partExtent(in) < s.minPart) return false;
    out = in;
    auto scaleSeg = [&](int32_t seg, int32_t lo) {
        int32_t v = (int32_t)(seg * s.segScale + 0.5f);
        return v < lo ? lo : v;
    };
    switch (in.type) {
        case PT_SPHERE:   out.b = scaleSeg(in.b, 4); break;
        case PT_CYLINDER:
        case PT_CAPSULE:  out.c = scaleSeg(in.c, 3); break;
        case PT_GRID:     out.c = scaleSeg(in.c, 2);
                          out.d = scaleSeg(in.d, 2); break;
        case PT_HEIGHTFIELD: {
            // Resample by STRIDE, never by interpolation. A coarse level must
            // take a strict SUBSET of the fine level's samples so that a shared
            // corner has bit-identical height in both — that is what stops a
            // gap opening along a LOD boundary. Only strides that divide the
            // spans evenly are allowed, so the last row/column stays on the
            // patch edge.
            int stride = 1;
            while ((float)stride * 2.0f <= 1.0f / (s.segScale > 0 ? s.segScale : 1)
                   && ((in.c - 1) % (stride * 2)) == 0
                   && ((in.d - 1) % (stride * 2)) == 0)
                stride *= 2;
            if (stride == 1) break;               // cannot coarsen cleanly
            out.c = (in.c - 1) / stride + 1;
            out.d = (in.d - 1) / stride + 1;
            out.heights.resize((size_t)out.c * out.d);
            for (int r = 0; r < out.d; ++r)
                for (int c = 0; c < out.c; ++c)
                    out.heights[(size_t)r * out.c + c] =
                        in.heights[(size_t)(r * stride) * in.c + (c * stride)];
            break;
        }
        default: break;   // flat-faced primitives have nothing to coarsen
    }
    return true;
}

bool JetMeshBuilder::bakeLod(const LodSpec& spec, float toleranceWorld)
{
    JetMeshBuilder lb;
    lb.bones = bones;
    for (size_t i = 0; i < parts.size(); ++i) {
        Part o;
        if (lodPart(parts[i], spec, o)) lb.parts.push_back(o);
    }
    if (lb.parts.empty()) return false;
    if (!lb.evaluate()) return false;
    if (toleranceWorld > 0) lb.cull(toleranceWorld);   // <= 0: caller said no cull
    lb.weld();
    if (lb.verts.empty() || lb.tris.empty() || lb.verts.size() > 65535)
        return false;

    // Remap the level's material indices onto the MAIN table by identity.
    std::vector<uint16_t> remap(lb.mats.size());
    for (size_t i = 0; i < lb.mats.size(); ++i) {
        int mi = -1;
        for (size_t k = 0; k < mats.size(); ++k)
            if (mats[k] == lb.mats[i]) { mi = (int)k; break; }
        if (mi < 0) return false;   // LOD parts are a subset; cannot happen
        remap[i] = (uint16_t)mi;
    }
    for (size_t i = 0; i < lb.tris.size(); ++i)
        lb.tris[i].mat = remap[lb.tris[i].mat];

    LodLevel lv;
    lv.verts.swap(lb.verts);
    lv.tris.swap(lb.tris);
    lods.push_back(lv);
    return true;
}

// ---------------------------------------------------------------------------
// Build (owned-array Object)
// ---------------------------------------------------------------------------

Object* JetMeshBuilder::build(int cell) const
{
    if (verts.empty() || tris.empty()) return nullptr;
    if (mats.size() != matParams.size()) return nullptr;

    size_t v0 = 0, vn = verts.size(), t0 = 0, tn = tris.size();
    if (cell >= 0) {
        if ((size_t)cell >= cells.size()) return nullptr;
        v0 = cells[cell].vertOff; vn = cells[cell].vertCount;
        t0 = cells[cell].triOff;  tn = cells[cell].triCount;
    } else if (!cells.empty()) {
        return nullptr;   // celled mesh: indices are cell-local, ask per cell
    }
    if (vn > 65535) return nullptr;

    Object* o = new Object();
    o->vertices.reserve(vn);
    for (size_t i = 0; i < vn; ++i) {
        const BVert& v = verts[v0 + i];
        o->addVertex({ { v.px, v.py, v.pz },
                       { v.u, v.v },
                       { v.nx, v.ny, v.nz } });
    }
    for (size_t i = 0; i < tn; ++i) {
        const BTri& t = tris[t0 + i];
        o->addTriangle((uint16_t)t.a, (uint16_t)t.b, (uint16_t)t.c,
                       mats[t.mat]);
    }
    o->calculateBoundingBox();
    return o;
}

Object* JetMeshBuilder::buildLod(size_t level) const
{
    if (level >= lods.size()) return nullptr;
    const LodLevel& lv = lods[level];
    if (lv.verts.empty() || lv.tris.empty() || lv.verts.size() > 65535)
        return nullptr;

    Object* o = new Object();
    o->vertices.reserve(lv.verts.size());
    for (size_t i = 0; i < lv.verts.size(); ++i) {
        const BVert& v = lv.verts[i];
        o->addVertex({ { v.px, v.py, v.pz },
                       { v.u, v.v },
                       { v.nx, v.ny, v.nz } });
    }
    for (size_t i = 0; i < lv.tris.size(); ++i) {
        const BTri& t = lv.tris[i];
        o->addTriangle((uint16_t)t.a, (uint16_t)t.b, (uint16_t)t.c,
                       mats[t.mat]);
    }
    o->calculateBoundingBox();
    return o;
}

// ---------------------------------------------------------------------------
// Pipeline execution and teardown
// ---------------------------------------------------------------------------

bool JetMeshBuilder::run(float worldScale)
{
    if (!evaluate()) return false;

    const float tolW = pipe.toleranceAuthored * worldScale;
    if (pipe.cull) cull(tolW);

    // maxedge caps triangle size so FAST_Z's single depth per triangle stays a
    // fair representative. Runs before weld so the new vertices get merged.
    if (pipe.maxEdgeAuthored > 0.0f)
        subdivide(pipe.maxEdgeAuthored * worldScale);

    weld();

    // Spatial cells and LOD levels are both derived state: clearing them
    // unconditionally is CORRECT here precisely because the specs that produce
    // them live in `pipe`, so a rerun reproduces them exactly.
    cells.clear();
    if (pipe.cellSizeAuthored > 0.0f && !rigged())
        splitCells((int32_t)(pipe.cellSizeAuthored * worldScale));

    lods.clear();
    if (!rigged()) {
        for (size_t i = 0; i < pipe.lodSpecs.size() && i < 4; ++i)
            bakeLod(pipe.lodSpecs[i], pipe.cull ? tolW : 0.0f);
    }

    lastStats.verts   = (int)verts.size();
    lastStats.tris    = (int)tris.size();
    lastStats.mats    = (int)matParams.size();
    lastStats.counted = true;
    return true;
}

void JetMeshBuilder::releaseEvaluated()
{
    std::vector<BVert>().swap(verts);
    std::vector<BTri>().swap(tris);
    std::vector<LodLevel>().swap(lods);
    std::vector<JmshCellRec>().swap(cells);
    std::vector<JetMatParams>().swap(matParams);
    std::vector<Renderer::Material*>().swap(mats);
    dirty = true;          // any later use reruns the pipeline from the recipe
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

bool JetMeshBuilder::save(const char* path, uint8_t worldScale) const
{
    if (!path || verts.empty() || tris.empty()) return false;
    if (verts.size() > 65535) return false;
    if (matParams.empty() || matParams.size() > 65535) return false;

    // Rigged output is grouped BONE-MAJOR so each bone's Object can borrow one
    // contiguous vertex/triangle range from the blob. Vertex positions become
    // bone-local (pivot subtracted) and triangle indices bone-local. The
    // unrigged path is the same machinery with a single implicit bone and a
    // zero pivot, minus the PART/CLIP sections.
    const bool rig = rigged();
    const size_t nBones = rig ? bones.size() : 1;
    const size_t nVerts = verts.size();

    // Bone-major vertex order. BVert::_pad carries the bone tag.
    std::vector<uint32_t> newIdx(nVerts);
    std::vector<uint32_t> boneVertBase(nBones + 1, 0);
    std::vector<uint32_t> order(nVerts);
    {
        std::vector<uint32_t> counts(nBones, 0);
        for (size_t i = 0; i < nVerts; ++i) {
            uint32_t b = rig ? (uint32_t)(uint16_t)verts[i]._pad : 0;
            if (b >= nBones) return false;
            counts[b]++;
        }
        for (size_t b = 0; b < nBones; ++b)
            boneVertBase[b + 1] = boneVertBase[b] + counts[b];
        std::vector<uint32_t> cursor(boneVertBase.begin(), boneVertBase.end() - 1);
        for (size_t i = 0; i < nVerts; ++i) {
            uint32_t b = rig ? (uint32_t)(uint16_t)verts[i]._pad : 0;
            newIdx[i] = cursor[b];
            order[cursor[b]++] = (uint32_t)i;
        }
    }

    // Bone-major triangle order + per-bone counts.
    std::vector<uint32_t> boneTriBase(nBones + 1, 0);
    std::vector<uint32_t> triOrder(tris.size());
    {
        std::vector<uint32_t> counts(nBones, 0);
        for (size_t i = 0; i < tris.size(); ++i) {
            uint32_t b = rig ? parts[tris[i].part].bone : 0;
            if (b >= nBones) return false;
            counts[b]++;
        }
        for (size_t b = 0; b < nBones; ++b)
            boneTriBase[b + 1] = boneTriBase[b] + counts[b];
        std::vector<uint32_t> cursor(boneTriBase.begin(), boneTriBase.end() - 1);
        for (size_t i = 0; i < tris.size(); ++i) {
            uint32_t b = rig ? parts[tris[i].part].bone : 0;
            triOrder[cursor[b]++] = (uint32_t)i;
        }
    }

    // Clip key pool size (bones * keys per clip).
    uint32_t clipKeyTotal = 0;
    for (size_t c = 0; c < clips.size(); ++c)
        clipKeyTotal += (uint32_t)clips[c].keys.size();

    // LOD sections apply to unrigged bakes only (per-bone LOD chains are a
    // later phase); at most 4 levels.
    const size_t nLods = (!rig && lods.size() <= 4) ? lods.size() : 0;

    // splitCells() already produced cell-major verts with cell-local triangle
    // indices, which is exactly what the unrigged identity grouping above
    // passes through untouched — so only the section has to be added here.
    const size_t nCells = (!rig && !cells.empty()) ? cells.size() : 0;

    const uint16_t sectionCount = (uint16_t)(3 + (rig ? 1 : 0)
                                    + ((rig && !clips.empty()) ? 1 : 0)
                                    + nLods + (nCells ? 1 : 0));
    const uint32_t hdrBytes  = sizeof(JmshHeader)
                             + sectionCount * sizeof(JmshSection);
    const uint32_t matsOff   = align16(hdrBytes);
    const uint32_t matsBytes = (uint32_t)(matParams.size() * 8);
    const uint32_t vertOff   = align16(matsOff + matsBytes);
    const uint32_t vertBytes = (uint32_t)(nVerts * sizeof(Object::Vertex));
    const uint32_t trisOff   = align16(vertOff + vertBytes);
    const uint32_t trisBytes = (uint32_t)(tris.size() * sizeof(JmshTriRec));
    const uint32_t partOff   = align16(trisOff + trisBytes);
    const uint32_t partBytes = rig ? (uint32_t)(nBones * sizeof(JmshPartRec)) : 0;
    const uint32_t clipOff   = align16(partOff + partBytes);
    const uint32_t clipBytes = (uint32_t)(clips.size() * sizeof(JmshClipRec)
                                          + clipKeyTotal * sizeof(ClipKey));

    // LOD section: 16-byte header {vertCount, triCount, 0, 0} + verts + tris.
    uint32_t lodOff[4] = {};
    uint32_t lodBytes[4] = {};
    uint32_t runOff = align16(clipOff + (rig ? clipBytes : 0));
    for (size_t i = 0; i < nLods; ++i) {
        lodOff[i] = runOff;
        lodBytes[i] = 16
            + (uint32_t)(lods[i].verts.size() * sizeof(Object::Vertex))
            + (uint32_t)(lods[i].tris.size() * sizeof(JmshTriRec));
        runOff = align16(lodOff[i] + lodBytes[i]);
    }
    const uint32_t cellOff   = runOff;
    const uint32_t cellBytes = (uint32_t)(nCells * sizeof(JmshCellRec));

    FILE* f = fopen(path, "wb");
    if (!f) return false;

    JmshHeader h = {};
    h.magic[0]='J'; h.magic[1]='M'; h.magic[2]='S'; h.magic[3]='2';
    h.version      = 1;
    h.worldScale   = worldScale;
    h.sectionCount = sectionCount;

    JmshSection secs[10] = {
        { TAG_MATS, matsOff, (uint32_t)matParams.size(), matsBytes },
        { TAG_VERT, vertOff, (uint32_t)nVerts,           vertBytes },
        { TAG_TRIS, trisOff, (uint32_t)tris.size(),      trisBytes },
    };
    uint16_t sn = 3;
    if (rig) {
        secs[sn++] = { TAG_PART, partOff, (uint32_t)nBones, partBytes };
        if (!clips.empty())
            secs[sn++] = { TAG_CLIP, clipOff, (uint32_t)clips.size(), clipBytes };
    }
    for (size_t i = 0; i < nLods; ++i)
        secs[sn++] = { JM_TAG('L','O','D', (char)('1' + i)),
                       lodOff[i], (uint32_t)(i + 1), lodBytes[i] };
    if (nCells)
        secs[sn++] = { TAG_CELL, cellOff, (uint32_t)nCells, cellBytes };

    bool ok = sn == sectionCount
           && fwrite(&h, sizeof(h), 1, f) == 1
           && fwrite(secs, sectionCount * sizeof(JmshSection), 1, f) == 1;

    // Zero-pad up to an offset. Sections are close together, so a small
    // static pad buffer covers every gap.
    auto padTo = [&](uint32_t off) {
        long cur = ftell(f);
        if (cur < 0 || (uint32_t)cur > off) { ok = false; return; }
        static const uint8_t zeros[16] = {};
        uint32_t gap = off - (uint32_t)cur;
        while (ok && gap) {
            uint32_t n = gap > 16 ? 16 : gap;
            ok = fwrite(zeros, 1, n, f) == n;
            gap -= n;
        }
    };

    padTo(matsOff);
    for (size_t i = 0; ok && i < matParams.size(); ++i) {
        const JetMatParams& m = matParams[i];
        uint8_t rec[8] = {
            (uint8_t)(m.color & 0xFF), (uint8_t)(m.color >> 8),
            m.alpha, m.diffuse, m.specular, m.emissive, m.shading, 0
        };
        ok = ok && fwrite(rec, sizeof(rec), 1, f) == 1;
    }

    padTo(vertOff);
    for (size_t o = 0; ok && o < nVerts; ++o) {
        const BVert& v = verts[order[o]];
        const uint32_t b = rig ? (uint32_t)(uint16_t)v._pad : 0;
        Object::Vertex rec = {};
        rec.position = { v.px - (rig ? bones[b].px : 0),
                         v.py - (rig ? bones[b].py : 0),
                         v.pz - (rig ? bones[b].pz : 0) };
        rec.uv       = { v.u, v.v };
        rec.normal   = { v.nx, v.ny, v.nz };
        ok = fwrite(&rec, sizeof(rec), 1, f) == 1;
    }

    padTo(trisOff);
    for (size_t o = 0; ok && o < tris.size(); ++o) {
        const BTri& t = tris[triOrder[o]];
        const uint32_t b = rig ? parts[t.part].bone : 0;
        const uint32_t base = boneVertBase[b];
        JmshTriRec rec = {};
        rec.a = (uint16_t)(newIdx[t.a] - base);
        rec.b = (uint16_t)(newIdx[t.b] - base);
        rec.c = (uint16_t)(newIdx[t.c] - base);
        rec.mat = t.mat;
        ok = fwrite(&rec, sizeof(rec), 1, f) == 1;
    }

    if (rig) {
        padTo(partOff);
        for (size_t b = 0; ok && b < nBones; ++b) {
            JmshPartRec rec = {};
            rec.nameHash  = bones[b].nameHash;
            rec.parent    = bones[b].parent;
            rec.px = bones[b].px; rec.py = bones[b].py; rec.pz = bones[b].pz;
            rec.vertOff   = boneVertBase[b];
            rec.vertCount = boneVertBase[b + 1] - boneVertBase[b];
            rec.triOff    = boneTriBase[b];
            rec.triCount  = boneTriBase[b + 1] - boneTriBase[b];
            ok = fwrite(&rec, sizeof(rec), 1, f) == 1;
        }
        if (!clips.empty()) {
            padTo(clipOff);
            uint32_t keyOff = 0;
            for (size_t c = 0; ok && c < clips.size(); ++c) {
                JmshClipRec rec = {};
                rec.nameHash = clips[c].nameHash;
                rec.rate     = clips[c].rate;
                rec.keyCount = clips[c].keyCount;
                rec.keyOff   = keyOff;
                keyOff += (uint32_t)clips[c].keys.size();
                ok = fwrite(&rec, sizeof(rec), 1, f) == 1;
            }
            for (size_t c = 0; ok && c < clips.size(); ++c) {
                if (clips[c].keys.empty()) continue;
                ok = fwrite(clips[c].keys.data(),
                            clips[c].keys.size() * sizeof(ClipKey), 1, f) == 1;
            }
        }
    }

    for (size_t li = 0; ok && li < nLods; ++li) {
        padTo(lodOff[li]);
        const uint32_t hdr4[4] = { (uint32_t)lods[li].verts.size(),
                                   (uint32_t)lods[li].tris.size(), 0, 0 };
        ok = fwrite(hdr4, sizeof(hdr4), 1, f) == 1;
        for (size_t i = 0; ok && i < lods[li].verts.size(); ++i) {
            const BVert& v = lods[li].verts[i];
            Object::Vertex rec = {};
            rec.position = { v.px, v.py, v.pz };
            rec.uv       = { v.u, v.v };
            rec.normal   = { v.nx, v.ny, v.nz };
            ok = fwrite(&rec, sizeof(rec), 1, f) == 1;
        }
        for (size_t i = 0; ok && i < lods[li].tris.size(); ++i) {
            const BTri& t = lods[li].tris[i];
            JmshTriRec rec = {};
            rec.a = (uint16_t)t.a; rec.b = (uint16_t)t.b; rec.c = (uint16_t)t.c;
            rec.mat = t.mat;
            ok = fwrite(&rec, sizeof(rec), 1, f) == 1;
        }
    }

    if (ok && nCells) {
        padTo(cellOff);
        ok = ok && fwrite(cells.data(), nCells * sizeof(JmshCellRec), 1, f) == 1;
    }

    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

void jet_mesh_file_free(JetMeshFile* f)
{
    if (f && f->blob) { free(f->blob); f->blob = nullptr; }
}

bool jet_mesh_read(const char* path, uint8_t worldScale, JetMeshFile* out)
{
    if (!out) return false;
    *out = JetMeshFile();

    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < (long)sizeof(JmshHeader) || fsize > 8 * 1024 * 1024) {
        fclose(f); return false;
    }

    uint8_t* blob = (uint8_t*)malloc((size_t)fsize);
    if (!blob) { fclose(f); return false; }
    const bool readOk = fread(blob, 1, (size_t)fsize, f) == (size_t)fsize;
    fclose(f);
    if (!readOk) { free(blob); return false; }

    const JmshHeader* h = (const JmshHeader*)blob;
    // A v1 "JMSH" file is a stale bake from before the format change; the
    // caller logs the failure and the demo re-saves its own meshes.
    if (memcmp(h->magic, "JMS2", 4) != 0 || h->version != 1 ||
        h->sectionCount < 3 || h->sectionCount > 32) {
        free(blob); return false;
    }
    const uint32_t tableEnd = sizeof(JmshHeader)
                            + h->sectionCount * sizeof(JmshSection);
    if (tableEnd > (uint32_t)fsize) { free(blob); return false; }

    const JmshSection* secs = (const JmshSection*)(blob + sizeof(JmshHeader));
    const JmshSection *sm = nullptr, *sv = nullptr, *st = nullptr;
    const JmshSection *sp = nullptr, *sc = nullptr, *sl = nullptr;
    const JmshSection *slod[4] = {};
    for (uint16_t i = 0; i < h->sectionCount; ++i) {
        const JmshSection& s = secs[i];
        if (s.offset > (uint32_t)fsize || s.bytes > (uint32_t)fsize - s.offset) {
            free(blob); return false;
        }
        if (s.tag == TAG_MATS) sm = &s;
        else if (s.tag == TAG_VERT) sv = &s;
        else if (s.tag == TAG_TRIS) st = &s;
        else if (s.tag == TAG_PART) sp = &s;
        else if (s.tag == TAG_CLIP) sc = &s;
        else if (s.tag == TAG_CELL) sl = &s;
        else if (s.tag >= JM_TAG('L','O','D','1') &&
                 s.tag <= JM_TAG('L','O','D','4')) {
            const uint32_t lvl = (s.tag >> 24) - '1';   // 0-based
            if (lvl < 4) slod[lvl] = &s;
        }
        // Unknown sections are fine: later phases add CELL/SORT.
    }
    if (!sm || !sv || !st ||
        sm->count == 0 || sm->count > 65535 || sm->bytes != sm->count * 8 ||
        sv->count < 3 || sv->count > 65535 ||
        sv->bytes != sv->count * sizeof(Object::Vertex) ||
        st->count == 0 || st->bytes != st->count * sizeof(JmshTriRec) ||
        (sv->offset & 3) != 0 || (st->offset & 3) != 0) {
        free(blob); return false;
    }
    if (sp && (sp->count < 2 || sp->count > 256 ||
               sp->bytes != sp->count * sizeof(JmshPartRec) ||
               (sp->offset & 3) != 0)) {
        free(blob); return false;
    }
    // Rig sections: parents must precede children, and the bone ranges must
    // partition the vertex and triangle arrays exactly in order — the
    // instantiate path hands out contiguous subranges on that assumption.
    if (sp) {
        const JmshPartRec* pr = (const JmshPartRec*)(blob + sp->offset);
        uint32_t vRun = 0, tRun = 0;
        for (uint32_t i = 0; i < sp->count; ++i) {
            if (pr[i].parent >= (int32_t)i) { free(blob); return false; }
            if (pr[i].vertOff != vRun || pr[i].triOff != tRun) {
                free(blob); return false;
            }
            vRun += pr[i].vertCount;
            tRun += pr[i].triCount;
        }
        if (vRun != sv->count || tRun != st->count) { free(blob); return false; }
    }
    uint32_t clipKeyCount = 0;
    if (sc) {
        if (!sp || sc->count == 0 || sc->count > 256 ||
            sc->bytes < sc->count * sizeof(JmshClipRec) ||
            (sc->offset & 3) != 0) {
            free(blob); return false;
        }
        clipKeyCount = (uint32_t)((sc->bytes - sc->count * sizeof(JmshClipRec))
                                  / sizeof(JetMeshBuilder::ClipKey));
        const JmshClipRec* cr = (const JmshClipRec*)(blob + sc->offset);
        for (uint32_t i = 0; i < sc->count; ++i) {
            if (cr[i].keyCount == 0 ||
                cr[i].keyOff + (uint32_t)cr[i].keyCount * sp->count > clipKeyCount) {
                free(blob); return false;
            }
        }
    }

    // Materials: unpack in place is unnecessary — the 8-byte packed record is
    // read field-by-field into a small owned array? No: JetMatParams is
    // exactly the packed record's shape (u16 + 5 u8, padded); read via the
    // packed bytes instead of aliasing to stay layout-independent.
    // The caller receives a pointer to a decoded copy stored in the blob's
    // material section, which is rewritten in place (record and struct are
    // both 8 bytes with color in the first two).
    static_assert(sizeof(JetMatParams) == 8, "matparams packing");
    {
        uint8_t* mrec = blob + sm->offset;
        for (uint32_t i = 0; i < sm->count; ++i, mrec += 8) {
            JetMatParams p;
            p.color    = (uint16_t)(mrec[0] | (mrec[1] << 8));
            p.alpha    = mrec[2];
            p.diffuse  = mrec[3];
            p.specular = mrec[4];
            p.emissive = mrec[5];
            p.shading  = mrec[6];
            memcpy(mrec, &p, sizeof(p));
        }
    }

    // Cells: same partition rule as bones — ranges must tile the arrays in
    // order, because each cell hands out a contiguous borrowed slice.
    if (sl) {
        if (sp || sl->count < 2 || sl->count > 4096 ||
            sl->bytes != sl->count * sizeof(JmshCellRec) ||
            (sl->offset & 3) != 0) {
            free(blob); return false;
        }
        const JmshCellRec* cr = (const JmshCellRec*)(blob + sl->offset);
        uint32_t vRun = 0, tRun = 0;
        for (uint32_t i = 0; i < sl->count; ++i) {
            if (cr[i].vertOff != vRun || cr[i].triOff != tRun) {
                free(blob); return false;
            }
            vRun += cr[i].vertCount;
            tRun += cr[i].triCount;
        }
        if (vRun != sv->count || tRun != st->count) { free(blob); return false; }
    }

    // Validate triangle indices before anyone trusts them. Rigged and CELLED
    // files use LOCAL indices, so each group's triangles validate against that
    // group's vertex count rather than the global one.
    if (sl) {
        const JmshTriRec* tr = (const JmshTriRec*)(blob + st->offset);
        const JmshCellRec* cr = (const JmshCellRec*)(blob + sl->offset);
        for (uint32_t c = 0; c < sl->count; ++c) {
            const uint32_t n = cr[c].vertCount;
            for (uint32_t i = 0; i < cr[c].triCount; ++i) {
                const JmshTriRec& t = tr[cr[c].triOff + i];
                if (t.a >= n || t.b >= n || t.c >= n || t.mat >= sm->count) {
                    free(blob); return false;
                }
            }
        }
    } else {
        const JmshTriRec* tr = (const JmshTriRec*)(blob + st->offset);
        if (sp) {
            const JmshPartRec* pr = (const JmshPartRec*)(blob + sp->offset);
            for (uint32_t b = 0; b < sp->count; ++b) {
                const uint32_t n = pr[b].vertCount;
                for (uint32_t i = 0; i < pr[b].triCount; ++i) {
                    const JmshTriRec& t = tr[pr[b].triOff + i];
                    if (t.a >= n || t.b >= n || t.c >= n || t.mat >= sm->count) {
                        free(blob); return false;
                    }
                }
            }
        } else {
            for (uint32_t i = 0; i < st->count; ++i) {
                if (tr[i].a >= sv->count || tr[i].b >= sv->count ||
                    tr[i].c >= sv->count || tr[i].mat >= sm->count) {
                    free(blob); return false;
                }
            }
        }
    }

    // LOD sections: levels must be contiguous from 1; each is a 16-byte
    // header {vertCount, triCount, 0, 0} + native verts + tri records over
    // the shared material table.
    uint32_t nLods = 0;
    struct { uint32_t v, t; uint8_t *verts, *tris; } lodPtr[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        if (!slod[i]) break;
        const JmshSection& s = *slod[i];
        if ((s.offset & 3) != 0 || s.bytes < 16) { free(blob); return false; }
        const uint32_t* lh = (const uint32_t*)(blob + s.offset);
        const uint32_t lv = lh[0], lt = lh[1];
        if (lv < 3 || lv > 65535 || lt == 0 ||
            s.bytes != 16 + lv * sizeof(Object::Vertex) + lt * sizeof(JmshTriRec)) {
            free(blob); return false;
        }
        uint8_t* vbase = blob + s.offset + 16;
        uint8_t* tbase = vbase + lv * sizeof(Object::Vertex);
        const JmshTriRec* tr = (const JmshTriRec*)tbase;
        for (uint32_t k = 0; k < lt; ++k) {
            if (tr[k].a >= lv || tr[k].b >= lv || tr[k].c >= lv ||
                tr[k].mat >= sm->count) {
                free(blob); return false;
            }
        }
        lodPtr[i].v = lv; lodPtr[i].t = lt;
        lodPtr[i].verts = vbase; lodPtr[i].tris = tbase;
        nLods = i + 1;
    }

    // World-scale migration: positions rescale in place, 16.16 ratio —
    // including every LOD level's vertices.
    if (h->worldScale != worldScale && h->worldScale != 0) {
        const int64_t ratio = ((int64_t)worldScale << 16) / h->worldScale;
        auto rescale = [&](Object::Vertex* vr, uint32_t n) {
            for (uint32_t i = 0; i < n; ++i) {
                vr[i].position.x = (int32_t)(((int64_t)vr[i].position.x * ratio) >> 16);
                vr[i].position.y = (int32_t)(((int64_t)vr[i].position.y * ratio) >> 16);
                vr[i].position.z = (int32_t)(((int64_t)vr[i].position.z * ratio) >> 16);
            }
        };
        rescale((Object::Vertex*)(blob + sv->offset), sv->count);
        for (uint32_t i = 0; i < nLods; ++i)
            rescale((Object::Vertex*)lodPtr[i].verts, lodPtr[i].v);
    }

    out->blob      = blob;
    out->vertCount = sv->count;
    out->triCount  = st->count;
    out->matCount  = sm->count;
    out->vertData  = blob + sv->offset;
    out->triData   = blob + st->offset;
    out->matData   = (const JetMatParams*)(blob + sm->offset);
    if (sp) {
        out->partCount = sp->count;
        out->partData  = (const JmshPartRec*)(blob + sp->offset);
    }
    if (sc) {
        out->clipCount    = sc->count;
        out->clipData     = (const JmshClipRec*)(blob + sc->offset);
        out->clipKeys     = (const JetMeshBuilder::ClipKey*)
            (blob + sc->offset + sc->count * sizeof(JmshClipRec));
        out->clipKeyCount = clipKeyCount;
    }
    if (sl) {
        out->cellCount = sl->count;
        out->cellData  = (const JmshCellRec*)(blob + sl->offset);
    }
    out->lodCount = nLods;
    for (uint32_t i = 0; i < nLods; ++i) {
        out->lods[i].vertCount = lodPtr[i].v;
        out->lods[i].triCount  = lodPtr[i].t;
        out->lods[i].vertData  = lodPtr[i].verts;
        out->lods[i].triData   = lodPtr[i].tris;
    }
    return true;
}

// Shared triangle materialisation for one contiguous record range: in-place
// rewrite where the ABI matches (device, 32-bit pointers), copy otherwise.
static void jm_fixupTris(Object* o, JmshTriRec* rec, uint32_t off,
                         uint32_t count, Material* const* mats)
{
    if (sizeof(void*) == 4 && sizeof(Object::Triangle) == sizeof(JmshTriRec)) {
        Object::Triangle* tri = (Object::Triangle*)rec;
        for (uint32_t i = 0; i < count; ++i) {
            const JmshTriRec r = rec[off + i];
            tri[off + i].v1 = r.a; tri[off + i].v2 = r.b; tri[off + i].v3 = r.c;
            tri[off + i].material   = mats[r.mat];
            tri[off + i].bakedColor = r.bakedColor;
            tri[off + i].colorBaked = r.colorBaked != 0;
        }
        o->triangles.borrow(tri + off, count);
    } else {
        o->triangles.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const JmshTriRec r = rec[off + i];
            Object::Triangle t;
            t.v1 = r.a; t.v2 = r.b; t.v3 = r.c;
            t.material   = mats[r.mat];
            t.bakedColor = r.bakedColor;
            t.colorBaked = r.colorBaked != 0;
            o->triangles.push_back(t);
        }
    }
}

int jet_mesh_instantiate_cells(JetMeshFile& f, Material* const* mats,
                               Object** outCells, int maxCells)
{
    if (!f.blob || !mats || !f.cellData || (int)f.cellCount > maxCells)
        return 0;

    Object::Vertex* verts = (Object::Vertex*)f.vertData;
    JmshTriRec*     rec   = (JmshTriRec*)f.triData;

    for (uint32_t c = 0; c < f.cellCount; ++c) {
        const JmshCellRec& cr = f.cellData[c];
        Object* o = new Object();
        o->vertices.borrow(verts + cr.vertOff, cr.vertCount);
        jm_fixupTris(o, rec, cr.triOff, cr.triCount, mats);
        o->calculateBoundingBox();   // the point: a tight per-cell AABB
        outCells[c] = o;
    }
    outCells[f.cellCount - 1]->adoptBlob(f.blob);
    f.blob = nullptr;
    return (int)f.cellCount;
}

int jet_mesh_instantiate_rig(JetMeshFile& f, Material* const* mats,
                             Object** outBones, int maxBones)
{
    if (!f.blob || !mats || !f.partData || (int)f.partCount > maxBones)
        return 0;

    Object::Vertex* verts = (Object::Vertex*)f.vertData;
    JmshTriRec*     rec   = (JmshTriRec*)f.triData;

    for (uint32_t b = 0; b < f.partCount; ++b) {
        const JmshPartRec& pr = f.partData[b];
        Object* o = new Object();
        o->vertices.borrow(verts + pr.vertOff, pr.vertCount);
        jm_fixupTris(o, rec, pr.triOff, pr.triCount, mats);
        o->useMatrix = true;   // rig code owns the transform from here on
        o->calculateBoundingBox();
        outBones[b] = o;
    }

    // The LAST bone owns the blob: the caller must destroy it after all the
    // other bones, whose spans borrow from the same allocation.
    outBones[f.partCount - 1]->adoptBlob(f.blob);
    f.blob = nullptr;
    return (int)f.partCount;
}

Object* jet_mesh_instantiate(JetMeshFile& f, Material* const* mats,
                             Object** outLods, int maxLods)
{
    if (!f.blob || !mats) return nullptr;

    Object* o = new Object();
    o->vertices.borrow((Object::Vertex*)f.vertData, f.vertCount);
    jm_fixupTris(o, (JmshTriRec*)f.triData, 0, f.triCount, mats);

    // LOD levels: mesh-only Objects (never added to a scene themselves) wired
    // into the head's lodMeshes chain. They borrow the same blob, which the
    // HEAD adopts — the caller destroys them before the head.
    if (outLods && maxLods > 0) {
        for (uint32_t i = 0; i < f.lodCount && (int)i < maxLods; ++i) {
            Object* lo = new Object();
            lo->vertices.borrow((Object::Vertex*)f.lods[i].vertData,
                                f.lods[i].vertCount);
            jm_fixupTris(lo, (JmshTriRec*)f.lods[i].triData, 0,
                         f.lods[i].triCount, mats);
            lo->calculateBoundingBox();
            o->lodMeshes.push_back(lo);
            outLods[i] = lo;
        }
    }

    o->adoptBlob(f.blob);
    f.blob = nullptr;
    o->calculateBoundingBox();
    return o;
}

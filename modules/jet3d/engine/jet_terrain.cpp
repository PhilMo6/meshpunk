#include "jet_terrain.h"

#include <math.h>
#include <stdlib.h>
#include <algorithm>
#include <vector>

// Only sqrtf is used from the math library; jet_mesh.cpp already depends on it,
// so it is known to resolve through host_exports[]. floorf is deliberately NOT
// used — ifloor below is an integer cast and costs no call — and no powf/sinf/
// cosf appears anywhere in this file, because every one of those is a symbol
// the ELF loader would have to find.

static JetLandCfg g_land;
static bool       g_landInit = false;

static inline uint16_t rgb565(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Linear blend of two RGB565 colours, per channel at native width.
static inline uint16_t blend565(uint16_t a, uint16_t b, float k) {
    if (k <= 0.0f) return a;
    if (k >= 1.0f) return b;
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int r = ar + (int)((float)(br - ar) * k);
    const int g = ag + (int)((float)(bg - ag) * k);
    const int bl = ab + (int)((float)(bb - ab) * k);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void landDefaults(JetLandCfg& c) {
    if (!c.cGrass) c.cGrass = rgb565( 86, 118,  62);
    if (!c.cRock)  c.cRock  = rgb565(112, 106,  96);
    if (!c.cSnow)  c.cSnow  = rgb565(226, 230, 238);
    if (!c.cSand)  c.cSand  = rgb565(178, 162, 112);
    if (!c.cSoil)  c.cSoil  = rgb565(104,  84,  58);
}

void jet_land_config(const JetLandCfg& c) {
    g_land = c;
    if (g_land.octaves < 1)  g_land.octaves = 1;
    if (g_land.octaves > 12) g_land.octaves = 12;
    if (g_land.featureLen < 1.0f) g_land.featureLen = 1.0f;
    landDefaults(g_land);
    g_landInit = true;
}

const JetLandCfg& jet_land_cfg() {
    if (!g_landInit) { landDefaults(g_land); g_landInit = true; }
    return g_land;
}

// ---------------------------------------------------------------------------
// Value noise with an analytic derivative
// ---------------------------------------------------------------------------

static inline int ifloor(float v) {
    const int i = (int)v;
    return (v < 0.0f && (float)i != v) ? i - 1 : i;
}

static inline float hash2(int ix, int iz, uint32_t seed) {
    uint32_t h = (uint32_t)ix * 374761393u
               + (uint32_t)iz * 668265263u
               + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    // [-1, 1)
    return (float)(int32_t)(h & 0x00FFFFFFu) * (1.0f / 8388608.0f) - 1.0f;
}

// Quintic interpolation, not cubic. Cubic leaves the SECOND derivative
// discontinuous at the lattice lines, and since the erosion term feeds on the
// derivative that discontinuity shows up as a faint square grid in the relief.
static inline void vnoise(float x, float z, uint32_t seed,
                          float& n, float& dx, float& dz)
{
    const int   ix = ifloor(x), iz = ifloor(z);
    const float u  = x - (float)ix, v = z - (float)iz;

    const float su = u*u*u*(u*(u*6.0f - 15.0f) + 10.0f);
    const float sv = v*v*v*(v*(v*6.0f - 15.0f) + 10.0f);
    const float du = 30.0f*u*u*(u*(u - 2.0f) + 1.0f);
    const float dv = 30.0f*v*v*(v*(v - 2.0f) + 1.0f);

    const float a = hash2(ix,     iz,     seed);
    const float b = hash2(ix + 1, iz,     seed);
    const float c = hash2(ix,     iz + 1, seed);
    const float d = hash2(ix + 1, iz + 1, seed);

    const float k1 = b - a, k2 = c - a, k3 = a - b - c + d;
    n  = a + k1*su + k2*sv + k3*su*sv;
    dx = du * (k1 + k3*sv);
    dz = dv * (k2 + k3*su);
}

// Plain fBm for the large-scale terms, where erosion damping is not wanted.
// `baseWave` is octave 0's wavelength in WORLD units and `spacing` the sampling
// step, so these terms band-limit on the same rule as the main height.
static float fbmLod(float x, float z, uint32_t seed, int oct,
                    float baseWave, float spacing)
{
    float f = 1.0f, amp = 1.0f, sum = 0.0f, norm = 0.0f, n, dx, dz;
    for (int i = 0; i < oct; ++i) {
        norm += amp;
        if (i == 0 || spacing <= 0.0f || (baseWave / f) >= 3.0f * spacing) {
            vnoise(x*f, z*f, seed + (uint32_t)i * 131u, n, dx, dz);
            sum += amp * n;
        }
        amp *= 0.5f;
        f   *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

float jet_land_height(float x, float z) { return jet_land_height_lod(x, z, 0.0f); }

float jet_land_height_lod(float x, float z, float spacing)
{
    const JetLandCfg& C = jet_land_cfg();

    // Work in PERIODS of the largest feature, so the accumulated derivative is
    // O(1) and the damping term below is scale-free. Feeding raw authored units
    // in here makes |grad| ~1e-5 and the erosion term does nothing at all.
    float px = x / C.featureLen;
    float pz = z / C.featureLen;

    // Domain distortion: displace the input by a slower noise. This is what
    // turns fBm's isotropic lumps into ridges, basins and river-like valleys
    // without any extra octaves.
    if (C.warp != 0.0f) {
        float wx, wz, d0, d1;
        vnoise(px * 0.5f + 11.3f, pz * 0.5f +  4.7f, C.seed ^ 0x9E37u, wx, d0, d1);
        vnoise(px * 0.5f -  6.1f, pz * 0.5f + 19.2f, C.seed ^ 0x51EDu, wz, d0, d1);
        px += C.warp * wx;
        pz += C.warp * wz;
    }

    float f = 1.0f, amp = 1.0f;
    float sum = 0.0f, norm = 0.0f;
    float dx = 0.0f, dz = 0.0f;

    for (int i = 0; i < C.octaves; ++i) {
        // Normalisation ALWAYS counts every octave, whether or not this
        // sampling can carry it. Dividing by only the octaves used would let a
        // one-octave coarse level renormalise back up to full amplitude and
        // come out TALLER than the fine surface it approximates.
        norm += amp;

        // Band limit. Octave i has wavelength featureLen/f in world units; a
        // sampling of `spacing` cannot represent anything finer than 2*spacing.
        // Octave 0 is always kept so a very coarse patch is still ground.
        const bool carries = (spacing <= 0.0f) || (i == 0)
                          || ((C.featureLen / f) >= 3.0f * spacing);

        if (carries) {
            float n, ndx, ndz;
            vnoise(px * f, pz * f, C.seed + (uint32_t)i * 131u, n, ndx, ndz);

            // True derivative of amp*noise(f*p) w.r.t. p. With the canonical
            // gain 0.5 / lacunarity 2 the product amp*f is 1, so the
            // accumulated slope grows about linearly with octave count and the
            // damping term stays in a useful range instead of saturating.
            // Only the LARGE-SCALE slope drives erosion, so the accumulator is
            // frozen after `erosionOctaves`.
            //
            // Erosion physically means a steep hillside sheds its fine
            // material; it does not mean detail suppresses itself. Letting the
            // derivative accumulate over every octave made the damping compound
            // — measured at 11 octaves, level-1 slope collapsed to mean 0.030
            // and the near ground rendered glassy, while the mid levels (fewer
            // octaves in range) stayed rougher. Freezing the accumulator makes
            // the damping ONE large-scale factor applied uniformly to all finer
            // detail, which is both the physical behaviour and stable against
            // changing the octave count.
            if (i < C.erosionOctaves) {
                dx += amp * f * ndx;
                dz += amp * f * ndz;
            }
            const float ms = (dx*dx + dz*dz) * 0.0625f;   // ~[0,1] after 4 oct
            sum += amp * n / (1.0f + C.erosion * ms);
        }

        amp *= C.gain;
        f   *= C.lacunarity;
    }

    float h = (norm > 0.0f) ? sum / norm : 0.0f;

    // AMPLIFY, then clamp. Two effects conspire to squash the raw sum: fBm's
    // octaves rarely peak together, and the erosion divisor only ever reduces.
    // Measured on the host before this line existed: max reached 0.64 of relief
    // and the standard deviation was 0.13, so the snow and sand rules never
    // fired anywhere in a 4M-unit world and every scatter candidate was
    // accepted. 1.75 lands the peaks on the rails.
    //
    // The clamp is not cosmetic: Part::heights is int16 in WORLD units, which
    // caps a patch at 4,096 authored units. relief 2600 * maxLift 1.5 * 1.0 =
    // 3,900, inside it. Without the clamp a tall peak would wrap.
    h *= 1.75f;
    if (h >  1.0f) h =  1.0f;
    if (h < -1.0f) h = -1.0f;

    // ROUGHNESS MASK: where the detail is rough and where it is smooth.
    const float maskWave = C.featureLen / 0.17f;
    const float mask = fbmLod(px * 0.17f + 3.9f, pz * 0.17f - 2.4f,
                              C.seed ^ 0xA53Cu, 3, maskWave, spacing) * 0.5f + 0.5f;
    float lift = C.plainsBias + (1.0f - C.plainsBias) * mask * mask * 1.9f;
    if (lift > 1.5f) lift = 1.5f;

    // CONTINENTS: a separate low-frequency term ADDED to the elevation rather
    // than scaling it. Scaling alone only makes ground rougher or smoother; it
    // never moves the base height, so nothing sits above the snow line or below
    // the sand line for any distance and a wide view is uniform green mush —
    // which is exactly what the first host render showed. Adding elevation is
    // what produces basins, plateaus and ranges, and it is what makes the
    // height-based colour rules mean something over a whole region.
    //
    // Its wavelength is deliberately far coarser than featureLen so it survives
    // the band limit at the outermost ring, where the sampling step is ~107,000
    // units. A continent finer than that would vanish exactly where the long
    // view needs it most.
    const float contWave = C.featureLen * C.continentLen;
    const float cont = fbmLod(px / C.continentLen - 7.2f,
                              pz / C.continentLen + 5.5f,
                              C.seed ^ 0x1F3Bu, 4, contWave, spacing);

    float out = h * lift * (1.0f - C.continentAmp) + cont * C.continentAmp * 1.9f;

    // TERRACE. Quantise into bands and steepen the transition between them, so
    // flat ground stays flat and the change between levels becomes a riser.
    // The blend keeps a little of the smooth form so the tops are not dead
    // level and the risers are not perfectly vertical (a vertical face is
    // invisible edge-on and sorts badly under a painter's algorithm).
    //
    // NOTE the interaction with band limiting: a riser is a step, so it carries
    // energy at every wavelength and a coarse tile cannot represent it. That is
    // correct and wanted — a distant mesa reads as a block, which is exactly how
    // it reads in Junk Runner's own distance shots.
    if (C.terraceSteps > 0) {
        const float steps = (float)C.terraceSteps;
        const float u = out * steps;
        const int   band = ifloor(u);
        float frac = u - (float)band;
        frac = (frac - 0.5f) * C.terraceSharp + 0.5f;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        frac = frac * frac * (3.0f - 2.0f * frac);      // soften the corners
        const float terraced = ((float)band + frac) / steps;
        out = out * (1.0f - C.terraceMix) + terraced * C.terraceMix;
    }

    // Normalisation rail only. This used to be the int16 storage limit as well;
    // Part::heightShift lifted that, so `relief` is now free to be a real
    // mountain height rather than the 4,096 authored units the samples could
    // hold. Keeping the rail bounds the output to a known multiple of relief.
    const float rail = 1.45f;
    if (out >  rail) out =  rail;
    if (out < -rail) out = -rail;
    return out * C.relief;
}

// ---------------------------------------------------------------------------
// Patches and colour maps
// ---------------------------------------------------------------------------

void jet_land_patch(float cx, float cz, float w, float d, int cols, int rows,
                    bool triRows, float* out, float* outMin, float* outMax,
                    float fitstep)
{
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    const int   lastC = cols - 1;
    const float stepX = w / (float)lastC;
    const float stepZ = d / (float)(rows - 1);
    const float x0    = cx - w * 0.5f;
    const float z0    = cz - d * 0.5f;
    const float half  = stepX * 0.5f;

    // Band-limit against the COARSER of the two spacings. A strip of a ring can
    // be long and thin, and taking only one axis would let detail through that
    // the other axis cannot represent. fitstep overrides both: the patch then
    // samples the finest level's filtered surface (see the header).
    const float lodStep = fitstep > 0.0f ? fitstep
                        : (stepX > stepZ ? stepX : stepZ);

    float lo = 1e30f, hi = -1e30f;
    for (int r = 0; r < rows; ++r) {
        const float z = z0 + stepZ * (float)r;
        for (int c = 0; c < cols; ++c) {
            // Mirror jm_createHeightfield's vx() EXACTLY, including the clamp
            // that leaves the first and last vertex of an odd row unshifted so
            // the patch edge stays straight and neighbours still butt.
            float x = x0 + stepX * (float)c;
            if (triRows && (r & 1) && c > 0 && c < lastC) x += half;

            const float h = jet_land_height_lod(x, z, lodStep);
            out[r * cols + c] = h;
            if (h < lo) lo = h;
            if (h > hi) hi = h;
        }
    }
    if (outMin) *outMin = lo;
    if (outMax) *outMax = hi;
}

// See the header. The interpolant evaluated here MUST match the mesh the
// renderer draws, so the cell split mirrors jm_createHeightfield's square
// triangulation exactly: triangle A = (r,c)(r+1,c)(r+1,c+1) covering u <= v,
// triangle B = (r+1,c+1)(r,c+1)(r,c) covering u >= v, with u the column
// fraction and v the row fraction inside the cell.
void jet_land_fit(float cx, float cz, float w, float d, int cols, int rows,
                  float* h, float fitstep, float margin)
{
    if (!h || cols < 2 || rows < 2 || fitstep <= 0.0f) return;
    const float stepX = w / (float)(cols - 1);
    const float stepZ = d / (float)(rows - 1);
    if (fitstep >= stepX && fitstep >= stepZ) return;   // nothing finer to fit
    const float x0 = cx - w * 0.5f;
    const float z0 = cz - d * 0.5f;

    // Per-cell worst excess of the coarse interpolant over the fine surface.
    const int cellsX = cols - 1, cellsZ = rows - 1;
    std::vector<float> excess((size_t)cellsX * cellsZ, 0.0f);

    // Fine lattice points per cell axis. The fine surface between its own
    // lattice points is linear, and the coarse interpolant is linear too, so
    // the maximum of their difference over a cell is attained AT a lattice
    // point — sampling the lattice is exact, not an approximation.
    const int fx = (int)(stepX / fitstep + 0.5f);
    const int fz = (int)(stepZ / fitstep + 0.5f);

    for (int r = 0; r < cellsZ; ++r) {
        for (int c = 0; c < cellsX; ++c) {
            const float hA = h[r * cols + c];             // (u0,v0)
            const float hB = h[(r + 1) * cols + c];       // (u0,v1)
            const float hC = h[(r + 1) * cols + c + 1];   // (u1,v1)
            const float hD = h[r * cols + c + 1];         // (u1,v0)
            const float cxw = x0 + stepX * (float)c;
            const float czw = z0 + stepZ * (float)r;
            float worst = 0.0f;
            for (int j = 0; j <= fz; ++j) {
                const float v = (float)j / (float)fz;
                const float z = czw + stepZ * v;
                for (int i = 0; i <= fx; ++i) {
                    const float u = (float)i / (float)fx;
                    const float coarse = (u <= v)
                        ? hA + (hB - hA) * v + (hC - hB) * u
                        : hA + (hD - hA) * u + (hC - hD) * v;
                    const float fine =
                        jet_land_height_lod(cxw + stepX * u, z, fitstep);
                    const float e = coarse - fine;
                    if (e > worst) worst = e;
                }
            }
            excess[(size_t)r * cellsX + c] = worst;
        }
    }

    // Lower each vertex by the worst excess of its adjacent cells. The
    // interpolant is monotone in the vertex heights, so one pass provably
    // removes every measured excess — no iteration. Vertices whose adjacent
    // cells all measured clean stay EXACT, which is what keeps shared-lattice
    // agreement with the fine level intact across most of the patch.
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            float drop = 0.0f;
            if (r > 0      && c > 0)      drop = std::max(drop, excess[(size_t)(r-1) * cellsX + (c-1)]);
            if (r > 0      && c < cellsX) drop = std::max(drop, excess[(size_t)(r-1) * cellsX + c]);
            if (r < cellsZ && c > 0)      drop = std::max(drop, excess[(size_t)r * cellsX + (c-1)]);
            if (r < cellsZ && c < cellsX) drop = std::max(drop, excess[(size_t)r * cellsX + c]);
            if (drop > 0.0f) h[r * cols + c] -= drop + margin;
        }
    }
}

// Colour + baked shade for one texel, given its height and the two slope
// components already computed by the caller (finite differences over the texel
// grid, which is both cheaper than re-deriving analytically and exactly matches
// what the eye reads at this resolution).
static uint16_t landShade(const JetLandCfg& C, float h, float dhdx, float dhdz,
                          float curv, float x, float z, float step)
{
    const float t = h / (C.relief > 0.0f ? C.relief : 1.0f);

    // Slope as rise/run. The normal is (-dhdx, 1, -dhdz) before normalising.
    const float slope2 = dhdx*dhdx + dhdz*dhdz;
    const float slope  = sqrtf(slope2);

    // ELEVATION RAMP, not thresholds. Hard bands put snow on 1% of the world
    // and sand on 2%, leaving 97% one flat green — measured, and it looked
    // exactly like that. A ramp gives every altitude its own colour, so a wide
    // view reads as regions instead of mush, and the sand/snow settings become
    // the ENDS of the ramp rather than rare special cases.
    uint16_t base;
    {
        const float lo = C.sandLevel, hi = C.snowLine;
        const uint16_t stops[6] = {
            C.cSand,                              // basins
            blend565(C.cGrass, C.cSoil,  0.30f),  // low ground, damp
            C.cGrass,                             // the broad middle
            blend565(C.cGrass, C.cSand,  0.38f),  // dry upland
            blend565(C.cRock,  C.cGrass, 0.25f),  // treeline
            C.cSnow,                              // peaks
        };
        float u = (t - lo) / ((hi - lo) != 0.0f ? (hi - lo) : 1.0f);
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        const float fpos = u * 5.0f;
        int   i = (int)fpos;
        if (i > 4) i = 4;
        base = blend565(stops[i], stops[i + 1], fpos - (float)i);
    }

    // Rock on the steep faces, over whatever the ramp chose. Applied as a blend
    // rather than a replacement so cliffs sit in their surroundings instead of
    // looking pasted on.
    {
        float k = slope / (C.rockSlope > 0.0f ? C.rockSlope : 1.0f);
        if (k > 1.0f) k = 1.0f;
        base = blend565(base, C.cRock, k * k);
    }

    // Break up flat colour with a high-frequency tint: the "detail texture"
    // contribution, folded into the bake instead of costing a second texture
    // unit we do not have. Its wavelength is tied to the TEXEL STEP, not to a
    // world constant — a fixed frequency would alias into blotches on the
    // coarse maps, whose texels are hundreds of times wider.
    const float gs    = step * 4.0f;
    const float grain = fbmLod(x / gs, z / gs, C.seed ^ 0x2B7u, 2, gs, step);

    // Lambert against the fine normal, plus curvature as cheap ambient
    // occlusion — concave ground darkens, ridges catch light.
    //
    // shadeGain exaggerates the normal before lighting it. Real slopes here
    // average 0.075, which tilts the normal about 4 degrees off vertical and
    // makes N.L essentially constant — the baked map would carry colour but no
    // relief, which defeats the point of baking it. Steepening the normal for
    // SHADING ONLY (the geometry is untouched) is what makes ridges and gullies
    // legible on a coarse tile.
    const float gx = dhdx * C.shadeGain, gz = dhdz * C.shadeGain;
    const float inv = 1.0f / sqrtf(gx*gx + gz*gz + 1.0f);
    const float nx = -gx * inv, ny = inv, nz = -gz * inv;
    float ndl = nx*C.lightX + ny*C.lightY + nz*C.lightZ;
    if (ndl < 0.0f) ndl = 0.0f;

    float shade = C.ambient + (1.0f - C.ambient) * ndl;
    shade *= (1.0f - 0.35f * curv);
    shade *= (1.0f + 0.06f * grain);
    if (shade < 0.05f) shade = 0.05f;
    if (shade > 1.60f) shade = 1.60f;

    int r = (int)((float)((base >> 11) & 0x1F) * shade);
    int g = (int)((float)((base >>  5) & 0x3F) * shade);
    int b = (int)((float)( base        & 0x1F) * shade);
    if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void jet_land_bake(float cx, float cz, float size, int texels, uint16_t* px)
{
    if (texels < 2 || !px) return;
    const JetLandCfg& C = jet_land_cfg();

    // One height row-grid of (texels+1)^2 samples, then slopes from
    // neighbours: ONE height evaluation per texel instead of the four a
    // per-texel central difference would cost.
    const int   n    = texels + 1;
    const float step = size / (float)texels;
    const float x0   = cx - size * 0.5f;
    const float z0   = cz - size * 0.5f;

    float* h = (float*)malloc((size_t)n * (size_t)n * sizeof(float));
    if (!h) {
        for (int i = 0; i < texels * texels; ++i) px[i] = C.cGrass;
        return;
    }
    for (int r = 0; r < n; ++r) {
        const float z = z0 + step * (float)r;
        for (int c = 0; c < n; ++c)
            h[r*n + c] = jet_land_height_lod(x0 + step * (float)c, z, step);
    }

    const float invStep = 1.0f / step;
    for (int r = 0; r < texels; ++r) {
        for (int c = 0; c < texels; ++c) {
            const float h00 = h[r*n + c],       h10 = h[r*n + c + 1];
            const float h01 = h[(r+1)*n + c],   h11 = h[(r+1)*n + c + 1];
            const float mid = (h00 + h10 + h01 + h11) * 0.25f;
            const float dhdx = ((h10 + h11) - (h00 + h01)) * 0.5f * invStep;
            const float dhdz = ((h01 + h11) - (h00 + h10)) * 0.5f * invStep;

            // Curvature proxy: how far this cell sits below the mean of its
            // ring of neighbours. Positive in hollows, negative on ridges.
            float ring = 0.0f; int rn = 0;
            if (c > 0)          { ring += h[r*n + c - 1]; ++rn; }
            if (c + 2 < n)      { ring += h[r*n + c + 2]; ++rn; }
            if (r > 0)          { ring += h[(r-1)*n + c]; ++rn; }
            if (r + 2 < n)      { ring += h[(r+2)*n + c]; ++rn; }
            float curv = 0.0f;
            if (rn) {
                curv = (ring / (float)rn - mid) / (C.relief * 0.10f + 1.0f);
                if (curv < -1.0f) curv = -1.0f;
                if (curv >  1.0f) curv =  1.0f;
            }

            px[r*texels + c] = landShade(C, mid, dhdx, dhdz, curv,
                                         x0 + step * ((float)c + 0.5f),
                                         z0 + step * ((float)r + 0.5f), step);
        }
    }
    free(h);
}

// See the header. Central differences + a 4-neighbour curvature ring, the
// same stencil shape jet_land_bake builds from its sample grid, so a painted
// triangle and a baked texel of the same spot agree.
uint16_t jet_land_shade_at(float x, float z, float spacing)
{
    const JetLandCfg& C = jet_land_cfg();
    const float s  = spacing > 1.0f ? spacing : 1.0f;
    const float hC = jet_land_height_lod(x,     z,     s);
    const float hW = jet_land_height_lod(x - s, z,     s);
    const float hE = jet_land_height_lod(x + s, z,     s);
    const float hN = jet_land_height_lod(x,     z - s, s);
    const float hS = jet_land_height_lod(x,     z + s, s);
    const float dhdx = (hE - hW) / (2.0f * s);
    const float dhdz = (hS - hN) / (2.0f * s);
    float curv = ((hW + hE + hN + hS) * 0.25f - hC)
               / (C.relief * 0.10f + 1.0f);
    if (curv < -1.0f) curv = -1.0f;
    if (curv >  1.0f) curv =  1.0f;
    return landShade(C, hC, dhdx, dhdz, curv, x, z, s);
}

// ---------------------------------------------------------------------------
// Baked horizon panorama. See JetHorizonCfg in the header for the why.
// ---------------------------------------------------------------------------
float jet_land_horizon(const JetHorizonCfg& c, int w, int h, uint16_t* px)
{
    if (!px || w < 2 || h < 2) return 0.0f;
    const JetLandCfg& C = jet_land_cfg();

    const float rNear = c.rNear > 1.0f ? c.rNear : 1.0f;
    const float rFar  = c.rFar > rNear * 1.001f ? c.rFar : rNear * 2.0f;
    int steps = c.steps < 8 ? 8 : (c.steps > 1024 ? 1024 : c.steps);

    // Constant RELATIVE step, so near ridges are sampled finely and the far
    // distance is not oversampled. A linear march would spend most of its
    // samples where one texel covers kilometres.
    const float ratio = powf(rFar / rNear, 1.0f / (float)steps);

    const float elevLo = c.elevLo;
    const float elevHi = c.elevHi > c.elevLo + 1e-5f ? c.elevHi : c.elevLo + 1e-5f;
    const float invSpan = (float)(h - 1) / (elevHi - elevLo);

    const float hazeSpan = (c.hazeFar - c.hazeNear) > 1.0f
                         ? (c.hazeFar - c.hazeNear) : 1.0f;

    float peak = -3.14159265f;

    // Distant ranges, ramped in with distance so the seam with the meshed
    // terrain stays exact. See JetHorizonCfg::rangeAmp.
    const float rgFrom = c.rangeFrom > 0.0f ? c.rangeFrom : rNear;
    const float rgFull = c.rangeFull > 0.0f ? c.rangeFull
                                            : (rNear + (rFar - rNear) * 0.5f);
    const float rgSpan = (rgFull - rgFrom) > 1.0f ? (rgFull - rgFrom) : 1.0f;
    const float rgLen  = c.rangeLen > 1.0f ? c.rangeLen : 1.0f;

    // Kept SEPARATE from the land height on purpose. landShade coloures from
    // h/relief against snowLine, and a 1.5 km range against 90 m relief gives
    // t = 16.7 — every distant peak saturated to pure snow. The lift decides
    // the SILHOUETTE; the colour still comes from the local terrain, biased
    // toward rock in proportion to how much of the height is range.
    auto rangeLift = [&](float x, float z, float sp, float r) -> float {
        if (c.rangeAmp <= 0.0f) return 0.0f;
        float t = (r - rgFrom) / rgSpan;
        if (t <= 0.0f) return 0.0f;
        if (t > 1.0f) t = 1.0f;
        t = t * t * (3.0f - 2.0f * t);                 // smoothstep
        const float n = fbmLod(x / rgLen + 11.3f, z / rgLen - 4.7f,
                               C.seed ^ 0x7C31u, 4, rgLen, sp);
        float ridge = 1.0f - (n < 0.0f ? -n : n);      // ridged, not blobby
        ridge *= ridge;
        return c.rangeAmp * ridge * t;
    };

    // Partial-column window (see JetHorizonCfg::colStart/colCount).
    int i0 = c.colStart, i1 = (c.colCount > 0) ? c.colStart + c.colCount : w;
    if (i0 < 0) i0 = 0;
    if (i1 > w) i1 = w;

    for (int i = i0; i < i1; ++i) {
        // Sky gradient first; ridges paint down over it from the top.
        for (int row = 0; row < h; ++row) {
            px[row * w + i] = blend565(c.skyTop, c.skyBot,
                                       (float)row / (float)(h - 1));
        }

        const float th = 6.28318531f * (float)i / (float)w;
        const float dx = sinf(th), dz = cosf(th);

        // NEAR TO FAR with a running occlusion line: `highest` is the topmost
        // row already covered by something nearer, so a sample paints only
        // what it newly reveals and the column finishes the moment the line
        // reaches the top.
        int highest = h;
        float r = rNear;
        for (int s = 0; s < steps && highest > 0; ++s, r *= ratio) {
            const float sp = r * (ratio - 1.0f);
            const float x  = c.camX + dx * r;
            const float z  = c.camZ + dz * r;
            // Band-limited to the step: finer detail than one sample can carry
            // would only shimmer between rebakes.
            const float hLocal = jet_land_height_lod(x, z, sp);
            const float lift   = rangeLift(x, z, sp, r);
            const float hh     = hLocal + lift;

            // Small-angle elevation: at the band angles this ring works in
            // (|a| < 0.2 rad) atan(x) = x within 1.3%, and this runs once per
            // march sample — the atan2f it replaces was a measurable share of
            // a multi-second bake. elevLo/elevHi compare in the same units.
            const float a = (hh - c.camY) / r;
            if (a > peak) peak = a;
            if (a <= elevLo) continue;

            int row = (int)((elevHi - a) * invSpan);
            if (row < 0) row = 0;
            if (row >= highest) continue;   // hidden behind a nearer ridge

            // Only now is the colour worth two more height samples.
            const float d  = sp > 1.0f ? sp : 1.0f;
            const float hx = jet_land_height_lod(x + d, z, sp)
                           + rangeLift(x + d, z, sp, r);
            const float hz = jet_land_height_lod(x, z + d, sp)
                           + rangeLift(x, z + d, sp, r);
            // Slope from the FULL surface (that is what is lit), colour from
            // the LOCAL height (that is what the ground is made of).
            uint16_t col = landShade(C, hLocal, (hx - hh) / d, (hz - hh) / d,
                                     0.0f, x, z, d);
            if (lift > 0.0f) {
                // High ground is rock and bare, so bias toward it with the
                // share of the height the range contributed. Capped: a range
                // is still land, not a grey wall.
                float k = lift / (c.rangeAmp > 1.0f ? c.rangeAmp : 1.0f);
                if (k > 1.0f) k = 1.0f;
                col = blend565(col, C.cRock, k * 0.65f);
            }

            float k = (r - c.hazeNear) / hazeSpan;
            if (k < 0.0f) k = 0.0f;
            if (k > 1.0f) k = 1.0f;
            col = blend565(col, c.haze, k * c.hazeMax);

            for (int rr = row; rr < highest; ++rr) px[rr * w + i] = col;
            highest = row;
        }
    }
    return peak;
}

// ---------------------------------------------------------------------------
// Tiling detail textures
// ---------------------------------------------------------------------------

// Value noise on a lattice that WRAPS at `period`. Opposite edges of the
// texture therefore sample identical lattice values, so the tile is seamless by
// construction rather than by blending the borders.
static float pnoise(float x, float z, int period, uint32_t seed)
{
    const int ix = ifloor(x), iz = ifloor(z);
    const float u = x - (float)ix, v = z - (float)iz;
    const float su = u*u*u*(u*(u*6.0f - 15.0f) + 10.0f);
    const float sv = v*v*v*(v*(v*6.0f - 15.0f) + 10.0f);

    auto wrap = [period](int k) { return ((k % period) + period) % period; };
    const int x0 = wrap(ix), x1 = wrap(ix + 1);
    const int z0 = wrap(iz), z1 = wrap(iz + 1);

    const float a = hash2(x0, z0, seed), b = hash2(x1, z0, seed);
    const float c = hash2(x0, z1, seed), d = hash2(x1, z1, seed);
    const float k1 = b - a, k2 = c - a, k3 = a - b - c + d;
    return a + k1*su + k2*sv + k3*su*sv;
}

// Periodic fBm over a unit square: `oct` octaves at periods base, 2*base, ...
static float pfbm(float u, float v, int base, int oct, uint32_t seed)
{
    float f = (float)base, amp = 1.0f, sum = 0.0f, norm = 0.0f;
    int   per = base;
    for (int i = 0; i < oct; ++i) {
        sum  += amp * pnoise(u * f, v * f, per, seed + (uint32_t)i * 71u);
        norm += amp;
        amp *= 0.5f;
        f   *= 2.0f;
        per *= 2;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

void jet_land_tiletex(int kind, int texels, uint16_t base, uint16_t accent,
                      uint16_t* px, uint32_t variant)
{
    if (!px || texels < 2) return;
    const JetLandCfg& C = jet_land_cfg();
    const uint32_t sd = C.seed ^ (0x5A17u + (uint32_t)kind * 9176u)
                      ^ (variant * 0x517CC1B7u);

    for (int y = 0; y < texels; ++y) {
        const float v = (float)y / (float)texels;
        for (int x = 0; x < texels; ++x) {
            const float u = (float)x / (float)texels;

            float mix, shade;
            switch (kind) {
            case JT_STRATA: {
                // Horizontal bands. The reference art's cliff faces are read
                // almost entirely by these — the geometry is a plain slab.
                const float wobble = pfbm(u, v, 2, 3, sd) * 0.08f;
                const float band   = v * 7.0f + wobble * 7.0f;
                const int   bi     = ifloor(band);
                float bf = band - (float)bi;
                bf = bf < 0.5f ? bf * 2.0f : (1.0f - bf) * 2.0f;
                mix   = bf * 0.75f + (hash2(bi, 0, sd) * 0.5f + 0.5f) * 0.25f;
                shade = 1.0f + pfbm(u, v, 8, 3, sd ^ 3u) * 0.16f;
                break;
            }
            case JT_SAND: {
                // Fine ripples: one strong direction plus a slow warp.
                const float w = pfbm(u, v, 2, 2, sd) * 0.35f;
                float r = (u * 11.0f + w * 11.0f);
                r = r - (float)ifloor(r);
                r = r < 0.5f ? r * 2.0f : (1.0f - r) * 2.0f;
                mix   = r * 0.5f;
                shade = 1.0f + (r - 0.5f) * 0.22f;
                break;
            }
            case JT_GRASS: {
                const float clump = pfbm(u, v, 4, 4, sd);
                mix   = clump * 0.5f + 0.5f;
                shade = 1.0f + pfbm(u, v, 16, 2, sd ^ 7u) * 0.20f;
                break;
            }
            case JT_ROCK: {
                const float f1 = pfbm(u, v, 4, 4, sd);
                const float f2 = pfbm(u, v, 8, 3, sd ^ 11u);
                mix   = (f1 * 0.6f + f2 * 0.4f) * 0.5f + 0.5f;
                shade = 1.0f + f2 * 0.26f;
                break;
            }
            default: {   // JT_GROUND — dirt: fine grain, low landmark energy
                // The lowest octaves are what reveal tiling: a base-2 blotch
                // puts ~2 distinctive blobs in the tile, and the eye locks
                // onto the same blob repeating every uvRepeat world units.
                // Weight therefore rises with frequency — the base-3 layer is
                // kept faint for broad tone, base-6 carries the visible
                // texture, base-16 the grain. Higher-frequency features are
                // too small to track from one repeat to the next.
                const float blotch  = pfbm(u, v, 3, 2, sd);
                const float mid     = pfbm(u, v, 6, 2, sd ^ 9u);
                const float speckle = pfbm(u, v, 16, 2, sd ^ 5u);
                mix   = (blotch * 0.20f + mid * 0.35f + speckle * 0.45f)
                        * 0.5f + 0.5f;
                shade = 1.0f + speckle * 0.18f;
                break;
            }
            }

            if (mix < 0.0f) mix = 0.0f;
            if (mix > 1.0f) mix = 1.0f;
            uint16_t c = blend565(base, accent, mix);

            int r = (int)((float)((c >> 11) & 0x1F) * shade);
            int g = (int)((float)((c >>  5) & 0x3F) * shade);
            int b = (int)((float)( c        & 0x1F) * shade);
            if (r > 31) r = 31; if (r < 0) r = 0;
            if (g > 63) g = 63; if (g < 0) g = 0;
            if (b > 31) b = 31; if (b < 0) b = 0;
            px[y * texels + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }

    // MESHPUNK: low-pass the finished tile.
    //
    // These kinds are deliberately high-frequency — JT_GROUND is speckle, one
    // texel of contrast against its neighbour. That is the worst possible input
    // for a minified nearest sampler: it puts maximum energy at exactly the
    // frequency undersampling turns into moiré. A mip chain fixes the far
    // field, but level 0 is still shown up close and still crawls.
    //
    // A 3x3 tent, WRAPPED at the edges so the tile stays seamless by
    // construction (the same property pnoise gives the pattern itself). One
    // pass only — enough to take the single-texel spikes off without turning
    // the surface to mush.
    {
        const size_t n = (size_t)texels * (size_t)texels;
        uint16_t* tmp = (uint16_t*)malloc(n * 2u);
        if (tmp) {
            for (int y = 0; y < texels; ++y) {
                const int ym = (y - 1 + texels) % texels;
                const int yp = (y + 1) % texels;
                for (int x = 0; x < texels; ++x) {
                    const int xm = (x - 1 + texels) % texels;
                    const int xp = (x + 1) % texels;
                    int r = 0, g = 0, b = 0;
                    // weights 1,2,1 / 2,4,2 / 1,2,1 = 16
                    const int ys[3] = { ym, y, yp };
                    const int xs[3] = { xm, x, xp };
                    const int wy[3] = { 1, 2, 1 };
                    const int wx[3] = { 1, 2, 1 };
                    for (int j = 0; j < 3; ++j)
                        for (int i = 0; i < 3; ++i) {
                            const uint16_t c = px[ys[j] * texels + xs[i]];
                            const int w = wy[j] * wx[i];
                            r += (int)((c >> 11) & 0x1F) * w;
                            g += (int)((c >>  5) & 0x3F) * w;
                            b += (int)( c        & 0x1F) * w;
                        }
                    tmp[y * texels + x] =
                        (uint16_t)(((r / 16) << 11) | ((g / 16) << 5) | (b / 16));
                }
            }
            for (size_t i = 0; i < n; ++i) px[i] = tmp[i];
            free(tmp);
        }
    }
}

bool jet_land_scatter(float cx, float cz, float size, int i, int n,
                      float* outX, float* outZ, float* outY, uint32_t* outRand)
{
    if (n < 1) return false;
    const JetLandCfg& C = jet_land_cfg();

    // Jitter grid: one candidate per sub-cell, displaced by a hash. Minimum
    // spacing falls out of the grid, so this is blue-noise enough for props
    // without the cost of a real Poisson-disk pass.
    int side = 1;
    while (side * side < n) ++side;
    const int gx = i % side, gz = i / side;
    if (gz >= side) return false;

    const float cell = size / (float)side;
    const float bx   = cx - size * 0.5f + cell * ((float)gx + 0.5f);
    const float bz   = cz - size * 0.5f + cell * ((float)gz + 0.5f);

    const uint32_t seed = C.seed ^ 0x7F4Au;
    const int qx = ifloor(bx * 0.001f), qz = ifloor(bz * 0.001f);
    const float jx = hash2(qx, qz, seed);
    const float jz = hash2(qx + 977, qz + 311, seed);
    const float x  = bx + jx * cell * 0.38f;
    const float z  = bz + jz * cell * 0.38f;

    // Reject on the same rules the colour map uses, so props never stand on
    // rock faces or below the sand line.
    const float e  = size * 0.004f + 1.0f;
    const float y  = jet_land_height(x, z);
    const float dx = (jet_land_height(x + e, z) - jet_land_height(x - e, z)) / (2.0f * e);
    const float dz = (jet_land_height(x, z + e) - jet_land_height(x, z - e)) / (2.0f * e);
    if (dx*dx + dz*dz > C.rockSlope * C.rockSlope * 0.55f) return false;
    if (y / (C.relief > 0.0f ? C.relief : 1.0f) < C.sandLevel) return false;

    if (outX) *outX = x;
    if (outZ) *outZ = z;
    if (outY) *outY = y;
    if (outRand) {
        uint32_t h = (uint32_t)qx * 2654435761u ^ (uint32_t)qz * 246822519u ^ seed;
        h = (h ^ (h >> 15)) * 2246822519u;
        *outRand = h ^ (h >> 13);
    }
    return true;
}

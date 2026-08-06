// Procedural land: ONE height function plus the rules that colour it.
//
// WHY THIS IS C AND NOT LUA. Every tile of the world needs its vertex heights
// AND a baked colour map, and the colour map is the expensive one: a 32x32
// texture is 1,024 samples against 81 for a 9x9 patch. Sampling that from Lua
// costs a lua_call per texel. In C a whole tile — heights, normals, rule-based
// colour, baked lighting — is a few hundred microseconds, which is what makes
// "rebuild the ring when the camera crosses a cell" affordable at all.
//
// FAKE EROSION. Real terrain reads as eroded because slopes are smooth and
// flats carry detail, which physical simulation produces and plain fBm does
// not. Simulation is a non-local iterative process and cannot be evaluated at
// a point. The substitute (Quilez, "noise with derivatives") accumulates the
// analytic DERIVATIVE of the noise and damps each further octave by the slope
// so far: 1/(1 + |grad|^2). Steep ground stops accumulating detail, flat ground
// keeps it. It is a pure point function — no neighbours, no iteration — so any
// tile at any level can be built in isolation, which is the property the whole
// streaming ring depends on.

#ifndef JET_TERRAIN_H
#define JET_TERRAIN_H

#include <stdint.h>

// All distances are AUTHORED units (the same units Lua passes to the builder);
// the world scale is applied by the callers that need world units.
struct JetLandCfg {
    uint32_t seed       = 1337u;
    float    relief     = 2600.0f;   // peak-to-centre amplitude
    float    featureLen = 190000.0f; // wavelength of the largest octave
    int      octaves    = 7;
    float    gain       = 0.5f;      // amplitude per octave (G = 2^-H)
    float    lacunarity = 2.03f;     // detuned off 2.0: exact octaves align
                                     // their extrema and print a visible grid
    float    erosion    = 1.0f;      // 0 = plain fBm, 1 = full slope damping
    int      erosionOctaves = 4;     // octaves that FEED the slope accumulator
    float    warp       = 0.35f;     // domain distortion, in input periods
    float    plainsBias = 0.55f;     // how much of the world the mask flattens

    // TERRACING — the shape Junk Runner actually has.
    //
    // Its screenshots are not rolling hills: the ground you drive on is FLAT,
    // and every bit of drama is a mesa, butte or canyon wall with a near
    // vertical, visibly striated face. Rolling fBm gives neither the flat
    // playable ground nor the vertical relief.
    //
    // Quantising the height into bands with sharp risers produces exactly that
    // from the same noise: most of each band is level, the transition between
    // bands is a cliff. It also renders CHEAPER — flat ground means coplanar
    // faces sharing one normal and one colour.
    int      terraceSteps = 0;       // 0 = off; otherwise bands over +/-relief
    float    terraceSharp = 7.0f;    // riser steepness; 1 = no terracing
    float    terraceMix   = 0.85f;   // blend toward the terraced form

    // Continents: an ADDED low-frequency elevation, not a multiplier. Without
    // it a wide view is uniform green — nothing stays above the snow line or
    // below the sand line long enough to read as a region.
    float    continentLen = 20.0f;   // wavelength as a MULTIPLE of featureLen
    float    continentAmp = 0.45f;   // share of the total elevation it owns

    // Colour rules. Heights are fractions of `relief`, slope is |dh/dx| in
    // authored units per authored unit.
    float    snowLine   = 0.55f;
    float    rockSlope  = 0.55f;
    float    sandLevel  = -0.42f;
    uint16_t cGrass = 0, cRock = 0, cSnow = 0, cSand = 0, cSoil = 0;

    // Baked lighting for the colour maps: direction the light comes FROM.
    float    lightX = -0.55f, lightY = 0.72f, lightZ = 0.42f;
    float    ambient = 0.42f;        // floor brightness, 0..1
    float    shadeGain = 1.8f;       // normal exaggeration for the bake only
};

// Install the configuration. Colours of 0 are replaced with the defaults.
void jet_land_config(const JetLandCfg& c);
const JetLandCfg& jet_land_cfg();

// Height at an authored world position, in authored units. Full detail.
float jet_land_height(float x, float z);

// Height band-limited to a sampling `spacing`: octaves whose wavelength is
// finer than 2*spacing are omitted.
//
// This is NOT the geomorph machinery of the earlier ring experiments — nothing
// is blended and no level is matched to another. It is only "do not add detail
// this sampling cannot carry", which is what stops a coarse mesh being noise.
// The mesh and its colour map deliberately use DIFFERENT spacings: the texture
// is sampled far finer than the geometry, and that difference is precisely the
// detail the texture exists to restore.
float jet_land_height_lod(float x, float z, float spacing);

// Fill `out` with (samples x samples) heights covering the square centred on
// (cx, cz) with the given authored `size`, row-major (row = Z, col = X),
// inclusive of both edges — so neighbouring tiles that share an edge sample the
// SAME positions and agree exactly. Returns the min/max height seen.
//
// `triRows` MUST match Part::triRows. The triangular lattice shifts odd rows
// half a cell in X, and sampling a regular grid for shifted vertices offsets
// every other row of the surface by half a cell — which reads on screen as a
// washboard, and did.
// `fitstep` > 0 overrides the band-limit spacing: the patch then samples the
// SAME filtered surface a fitstep-spaced patch would, instead of its own
// coarser filter. Coarse-LOD patches pass the finest level's spacing so a
// vertex both levels share carries EXACTLY the same height — the
// agree-by-construction half of coarse-from-fine. 0 = classic behaviour
// (band-limit at own spacing).
void jet_land_patch(float cx, float cz, float w, float d, int cols, int rows,
                    bool triRows, float* out, float* outMin, float* outMax,
                    float fitstep = 0.0f);

// Clamp a coarse patch UNDER the fine surface. Samples the fitstep lattice
// across the patch, evaluates the coarse mesh's own triangle interpolant
// (same square split as jm_createHeightfield) at every fine point, and
// lowers each coarse vertex by the worst positive excess found in its
// adjacent cells, plus `margin`. Provably poke-proof: the interpolant is
// monotone in the vertex heights, so one pass removes every measured excess.
//
// *** hw-measured 2026-08-03: WRONG TOOL FOR TERRACED TERRAIN — currently
// unused. *** A chord from a mesa-top vertex to a basin vertex measures the
// basin floor beneath it as excess up to the FULL relief, so on terrain with
// cliffs everywhere the pass collapses the coarse surface to its basin
// floors (a lower envelope — flat), and drops that size exceed the skirts,
// opening sky cracks at patch borders (patches clamp independently). Only
// suitable for terrain whose relief is smooth at the coarse-cell scale.
// The shipped alternative: sample far levels densely enough that the
// band-limit keeps the feature octave, plus a small measured sink.
// With triRows the check runs on the unshifted lattice (approximate).
void jet_land_fit(float cx, float cz, float w, float d, int cols, int rows,
                  float* h, float fitstep, float margin);

// Bake an RGB565 colour map for the same square. `px` must hold texels*texels
// uint16. Colour comes from the height/slope rules; brightness from the FINE
// surface normal plus a curvature term, so the texture carries relief the
// coarse mesh threw away. This is the whole point of the exercise.
void jet_land_bake(float cx, float cz, float size, int texels, uint16_t* px);

// One shaded palette sample at (x, z), authored units — the same landShade
// rules jet_land_bake and the horizon use (elevation ramp, rock-by-slope,
// grain, Lambert + curvature AO with the cfg light), for painting mesh
// TRIANGLES instead of texels. `spacing` band-limits the five height reads
// and sets the grain wavelength: pass roughly the size of the area one
// colour will cover.
uint16_t jet_land_shade_at(float x, float z, float spacing);

// ---------------------------------------------------------------------------
// Tiling detail textures — a SMALL FIXED SET, generated once at start-up.
//
// This replaces baking a unique colour map per tile. That cost ran during ring
// rebuilds (every cell crossing) and produced a texture stretched across a
// whole tile, which is the wrong scale entirely: the reference art has a fine
// detail texture repeating every metre or two, not one image per region.
// A handful of small seamless tiles, repeated at a fixed WORLD scale, is both
// cheaper and closer to the target.
//
// Seamlessness is structural, not blended: the noise lattice wraps at the
// texture period, so opposite edges sample the SAME lattice values.
// ---------------------------------------------------------------------------
enum JetTileKind {
    JT_GROUND = 0,   // speckled dirt — the surface you drive on
    JT_STRATA = 1,   // horizontal banding — mesa faces and canyon walls
    JT_SAND   = 2,   // fine directional ripples
    JT_GRASS  = 3,   // clumped patches
    JT_ROCK   = 4,   // fractured, no preferred direction
};

// Fill `px` (texels*texels RGB565) with a seamless tile. `texels` should be a
// power of two so the wrap periods divide it exactly. `variant` folds into the
// noise seed: same kind + palette + character, different pattern — for papering
// ground with several non-identical tiles.
void jet_land_tiletex(int kind, int texels, uint16_t base, uint16_t accent,
                      uint16_t* px, uint32_t variant = 0);

// ---------------------------------------------------------------------------
// BAKED HORIZON PANORAMA — the entire far view as one textured ring.
//
// Past some distance a triangle stops being worth its cost: a ridge 30 km away
// occupies a few pixels however many triangles describe it. So the far world
// becomes a cylinder around the camera carrying a pre-rendered silhouette, and
// costs one object instead of a level of terrain.
//
// One ray per texture column is marched NEAR TO FAR while keeping the highest
// row painted so far; a sample only paints the rows ABOVE that line, and the
// column is finished as soon as the line reaches the top. That is the classic
// heightmap horizon walk: nearer ridges occlude what is behind them by
// construction, and painting is O(height) per column rather than O(steps *
// height). Radial stepping is GEOMETRIC, so resolution is constant in angular
// terms instead of wasting samples on the far distance.
//
// Because it is baked from the same jet_land_height as the mesh, the backdrop
// always agrees with the terrain in front of it — extending the mesh later
// cannot desync the two.
// ---------------------------------------------------------------------------
struct JetHorizonCfg {
    float camX = 0.0f, camY = 0.0f, camZ = 0.0f;  // viewpoint, authored units
    float rNear = 0.0f, rFar = 0.0f;              // radial band to march
    float elevLo = -0.05f, elevHi = 0.12f;        // rows span this, in RADIANS
    int   steps  = 160;                           // radial samples per column
    uint16_t skyTop = 0, skyBot = 0;              // gradient behind the ridges
    uint16_t haze  = 0;                           // colour distance blends to
    float hazeNear = 0.0f, hazeFar = 0.0f;
    float hazeMax  = 0.92f;                       // blend fraction at hazeFar

    // DISTANT RANGES — the reason a 50 km horizon is not an empty band.
    //
    // This world is calibrated at 1,000 authored units per metre with 90 m
    // mesas, matching the reference art. 90 m subtends 0.1 degrees at 50 km:
    // measured, the baked peak elevation came out at -0.0008 rad, i.e. the
    // whole landscape sat BELOW eye level and the panorama was a flat band.
    // `continentAmp` cannot fix that — it is a SHARE of the elevation
    // (`h*lift*(1-amp) + cont*amp*1.9`), so raising it redistributes relief
    // rather than adding any.
    //
    // Real landscapes have local relief AND regional relief; this is the
    // second one. It is RIDGED noise (1-|n|, squared) so it reads as ranges
    // rather than blobs, and it RAMPS IN with distance — zero at rangeFrom so
    // the backdrop still agrees exactly with the meshed terrain at the seam,
    // full by rangeFull. Nothing the player can walk on is affected.
    float rangeAmp  = 0.0f;         // peak height in AUTHORED units; 0 = off
    float rangeLen  = 15000000.0f;  // wavelength, authored units
    float rangeFrom = 0.0f;         // ramp start; 0 = rNear
    float rangeFull = 0.0f;         // ramp end;   0 = halfway to rFar

    // Partial bake: only columns [colStart, colStart+colCount) are written
    // (colCount 0 = all). This is what makes the panorama a CONTINUOUS
    // AMORTISED sweep instead of a multi-second stall: the caller refills a
    // few columns per frame from the camera's current position, and the seam
    // walks the full circle every few dozen frames. Rows outside the band
    // are still repainted with the sky gradient per column, so no state
    // carries over between sweeps.
    int colStart = 0;
    int colCount = 0;
};

// Fill `px` (w*h RGB565, row 0 = elevHi, row h-1 = elevLo) with the panorama.
// Returns the highest elevation angle any terrain reached, in radians, so the
// caller can check its elevation range was not clipping the peaks.
float jet_land_horizon(const JetHorizonCfg& c, int w, int h, uint16_t* px);

// A deterministic scatter point inside the tile centred on (cx, cz): index `i`
// of `n`, returned in authored world coordinates with its ground height. Slope
// and height rules reject unsuitable spots; returns false when rejected, so a
// caller loops over i and keeps what it gets (Poisson-ish by construction —
// the jitter grid guarantees minimum spacing).
bool jet_land_scatter(float cx, float cz, float size, int i, int n,
                      float* outX, float* outZ, float* outY, uint32_t* outRand);

#endif // JET_TERRAIN_H

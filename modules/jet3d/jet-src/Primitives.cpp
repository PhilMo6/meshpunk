#include "Primitives.hpp"
#include "TrigLUT.hpp"
#include "Shader.hpp"
#include <cmath>

namespace Primitives
{
    void computeNormal(Object *obj, uint16_t idx1, uint16_t idx2, uint16_t idx3)
    {
        Object::Vertex &v1 = obj->vertices[idx1];
        Object::Vertex &v2 = obj->vertices[idx2];
        Object::Vertex &v3 = obj->vertices[idx3];

        Vector3 u = v2.position - v1.position;
        Vector3 v = v3.position - v1.position;
        Vector3 n = u.cross(v);

        // Normalize the normal vector to maintain consistent lighting
        int32_t length = n.length();
        if (length == 0)
            length = 1; // Avoid division by zero

        n.assign(n.x * FIXED_POINT_SCALE / length,
                 n.y * FIXED_POINT_SCALE / length,
                 n.z * FIXED_POINT_SCALE / length);

        v1.normal.assign(n.x, n.y, n.z);
    }

    Object *createCube(int32_t width, int32_t height, int32_t depth, Material *material)
    {
        Object *cube = new Object();

        int32_t hw = width / 2;
        int32_t hh = height / 2;
        int32_t hd = depth / 2;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        // Define the vertices for each face of the cube
        // Front face
        cube->addVertex({{-hw, -hh, hd}, {0, maxV}, {0, 0, FIXED_POINT_SCALE}});   // v0
        cube->addVertex({{hw, -hh, hd}, {maxU, maxV}, {0, 0, FIXED_POINT_SCALE}}); // v1
        cube->addVertex({{hw, hh, hd}, {maxU, 0}, {0, 0, FIXED_POINT_SCALE}});     // v2
        cube->addVertex({{-hw, hh, hd}, {0, 0}, {0, 0, FIXED_POINT_SCALE}});       // v3

        // Back face
        cube->addVertex({{-hw, -hh, -hd}, {0, maxV}, {0, 0, -FIXED_POINT_SCALE}});   // v4
        cube->addVertex({{hw, -hh, -hd}, {maxU, maxV}, {0, 0, -FIXED_POINT_SCALE}}); // v5
        cube->addVertex({{hw, hh, -hd}, {maxU, 0}, {0, 0, -FIXED_POINT_SCALE}});     // v6
        cube->addVertex({{-hw, hh, -hd}, {0, 0}, {0, 0, -FIXED_POINT_SCALE}});       // v7

        // Left face
        cube->addVertex({{-hw, -hh, -hd}, {0, maxV}, {-FIXED_POINT_SCALE, 0, 0}});   // v8
        cube->addVertex({{-hw, -hh, hd}, {maxU, maxV}, {-FIXED_POINT_SCALE, 0, 0}}); // v9
        cube->addVertex({{-hw, hh, hd}, {maxU, 0}, {-FIXED_POINT_SCALE, 0, 0}});     // v10
        cube->addVertex({{-hw, hh, -hd}, {0, 0}, {-FIXED_POINT_SCALE, 0, 0}});       // v11

        // Right face
        cube->addVertex({{hw, -hh, hd}, {0, maxV}, {FIXED_POINT_SCALE, 0, 0}});     // v12
        cube->addVertex({{hw, -hh, -hd}, {maxU, maxV}, {FIXED_POINT_SCALE, 0, 0}}); // v13
        cube->addVertex({{hw, hh, -hd}, {maxU, 0}, {FIXED_POINT_SCALE, 0, 0}});     // v14
        cube->addVertex({{hw, hh, hd}, {0, 0}, {FIXED_POINT_SCALE, 0, 0}});         // v15

        // Top face
        cube->addVertex({{-hw, hh, hd}, {0, maxV}, {0, FIXED_POINT_SCALE, 0}});   // v16
        cube->addVertex({{hw, hh, hd}, {maxU, maxV}, {0, FIXED_POINT_SCALE, 0}}); // v17
        cube->addVertex({{hw, hh, -hd}, {maxU, 0}, {0, FIXED_POINT_SCALE, 0}});   // v18
        cube->addVertex({{-hw, hh, -hd}, {0, 0}, {0, FIXED_POINT_SCALE, 0}});     // v19

        // Bottom face
        cube->addVertex({{-hw, -hh, -hd}, {0, maxV}, {0, -FIXED_POINT_SCALE, 0}});   // v20
        cube->addVertex({{hw, -hh, -hd}, {maxU, maxV}, {0, -FIXED_POINT_SCALE, 0}}); // v21
        cube->addVertex({{hw, -hh, hd}, {maxU, 0}, {0, -FIXED_POINT_SCALE, 0}});     // v22
        cube->addVertex({{-hw, -hh, hd}, {0, 0}, {0, -FIXED_POINT_SCALE, 0}});       // v23

        // Add faces
        cube->addFace(0, 1, 2, 3, material);     // Front face
        cube->addFace(5, 4, 7, 6, material);     // Back face
        cube->addFace(8, 9, 10, 11, material);   // Left face
        cube->addFace(12, 13, 14, 15, material); // Right face
        cube->addFace(16, 17, 18, 19, material); // Top face
        cube->addFace(20, 21, 22, 23, material); // Bottom face

        cube->calculateBoundingBox();

        return cube;
    }

    Object *createDebugCube(int32_t width, int32_t height, int32_t depth)
    {
        Object *cube = new Object();

        int32_t hw = width / 2;
        int32_t hh = height / 2;
        int32_t hd = depth / 2;

        // 24 verts: 4 per face, each carrying its own face normal. The
        // previous 8-vertex layout shared corners across 6 faces but only
        // stamped a ±Z normal on each, leaving 4 of 6 faces with garbage
        // normals (rendered as ambient-only under LIGHTING).
        const int32_t N = FIXED_POINT_SCALE;

        // Front (+Z)
        cube->addVertex({{-hw, -hh,  hd}, {0, 0}, {0, 0,  N}});
        cube->addVertex({{ hw, -hh,  hd}, {0, 0}, {0, 0,  N}});
        cube->addVertex({{ hw,  hh,  hd}, {0, 0}, {0, 0,  N}});
        cube->addVertex({{-hw,  hh,  hd}, {0, 0}, {0, 0,  N}});
        // Back (-Z)
        cube->addVertex({{ hw, -hh, -hd}, {0, 0}, {0, 0, -N}});
        cube->addVertex({{-hw, -hh, -hd}, {0, 0}, {0, 0, -N}});
        cube->addVertex({{-hw,  hh, -hd}, {0, 0}, {0, 0, -N}});
        cube->addVertex({{ hw,  hh, -hd}, {0, 0}, {0, 0, -N}});
        // Left (-X)
        cube->addVertex({{-hw, -hh, -hd}, {0, 0}, {-N, 0, 0}});
        cube->addVertex({{-hw, -hh,  hd}, {0, 0}, {-N, 0, 0}});
        cube->addVertex({{-hw,  hh,  hd}, {0, 0}, {-N, 0, 0}});
        cube->addVertex({{-hw,  hh, -hd}, {0, 0}, {-N, 0, 0}});
        // Right (+X)
        cube->addVertex({{ hw, -hh,  hd}, {0, 0}, { N, 0, 0}});
        cube->addVertex({{ hw, -hh, -hd}, {0, 0}, { N, 0, 0}});
        cube->addVertex({{ hw,  hh, -hd}, {0, 0}, { N, 0, 0}});
        cube->addVertex({{ hw,  hh,  hd}, {0, 0}, { N, 0, 0}});
        // Top (+Y)
        cube->addVertex({{-hw,  hh,  hd}, {0, 0}, {0,  N, 0}});
        cube->addVertex({{ hw,  hh,  hd}, {0, 0}, {0,  N, 0}});
        cube->addVertex({{ hw,  hh, -hd}, {0, 0}, {0,  N, 0}});
        cube->addVertex({{-hw,  hh, -hd}, {0, 0}, {0,  N, 0}});
        // Bottom (-Y)
        cube->addVertex({{-hw, -hh, -hd}, {0, 0}, {0, -N, 0}});
        cube->addVertex({{ hw, -hh, -hd}, {0, 0}, {0, -N, 0}});
        cube->addVertex({{ hw, -hh,  hd}, {0, 0}, {0, -N, 0}});
        cube->addVertex({{-hw, -hh,  hd}, {0, 0}, {0, -N, 0}});

        Material *frontMaterial  = new Material(0xF800, nullptr, nullptr, false); // Red
        Material *backMaterial   = new Material(0x07E0, nullptr, nullptr, false); // Green
        Material *leftMaterial   = new Material(0x001F, nullptr, nullptr, false); // Blue
        Material *rightMaterial  = new Material(0xFFE0, nullptr, nullptr, false); // Yellow
        Material *topMaterial    = new Material(0xF81F, nullptr, nullptr, false); // Magenta
        Material *bottomMaterial = new Material(0x07FF, nullptr, nullptr, false); // Cyan

        cube->addFace( 0,  1,  2,  3, frontMaterial);
        cube->addFace( 4,  5,  6,  7, backMaterial);
        cube->addFace( 8,  9, 10, 11, leftMaterial);
        cube->addFace(12, 13, 14, 15, rightMaterial);
        cube->addFace(16, 17, 18, 19, topMaterial);
        cube->addFace(20, 21, 22, 23, bottomMaterial);

        cube->calculateBoundingBox();

        return cube;
    }

    Object *createGrid(int32_t width, int32_t height, int32_t rows, int32_t cols, Material *material, Material *material2, bool perCellUV)
    {
        Object *grid = new Object();

        int32_t hw = width / 2;
        int32_t hh = height / 2;

        // MESHPUNK: spacing divides by (n-1), not n.
        //
        // `rows`/`cols` are VERTEX counts and the loops below emit indices
        // 0..n-1, so the last vertex has to land on the far edge for the mesh
        // to actually be `width` across. Dividing by n instead left the grid
        // spanning width*(cols-1)/cols and centred at -width/(2*cols) — a
        // 4-vertex grid covered three quarters of its requested size, sitting
        // off to one side. On a tiled world that shows up as a visible gap
        // between neighbouring tiles (25% of a tile at segs=4) and every tile
        // offset from the lattice position it was placed at.
        //
        // The UV block below already divides by (cols - 1), i.e. it always
        // assumed c == cols-1 was the far edge. The positions simply disagreed
        // with the primitive's own convention.
        // Positions are interpolated from the index rather than accumulated
        // from a truncated per-step spacing: 8000/3 truncates to 2666, and
        // stepping that three times lands 2 units short of the edge. Small,
        // but it is exactly the hairline seam that tiling is meant to remove,
        // and c == colDiv now yields `width - hw` == +hw exactly.
        const int32_t rowDiv = rows > 1 ? rows - 1 : 1;
        const int32_t colDiv = cols > 1 ? cols - 1 : 1;

        // Define vertices for the grid
        for (int32_t r = 0; r < rows; ++r)
        {
            for (int32_t c = 0; c < cols; ++c)
            {
                int32_t x = (int32_t)(((int64_t)c * width)  / colDiv) - hw;
                int32_t y = (int32_t)(((int64_t)r * height) / rowDiv) - hh;

                Vector2 uv;
                if (perCellUV) {
                    // UV coordinates that repeat for each cell (0 to FIXED_POINT_SCALE)
                    uv = {
                        (c % 2) * FIXED_POINT_SCALE,
                        (r % 2) * FIXED_POINT_SCALE
                    };
                } else {
                    // Original UV mapping across entire grid
                    uv = {
                        c * FIXED_POINT_SCALE / (cols - 1),
                        r * FIXED_POINT_SCALE / (rows - 1)
                    };
                }
                grid->addVertex({{x, 0, y}, uv, {0, FIXED_POINT_SCALE, 0}});
            }
        }

        // Create faces for the grid
        for (int32_t r = 0; r < rows - 1; ++r)
        {
            for (int32_t c = 0; c < cols - 1; ++c)
            {
                int32_t v0 = r * cols + c;
                int32_t v1 = v0 + 1;
                int32_t v2 = v1 + cols;
                int32_t v3 = v0 + cols;

                //Use alternating materials for each cell
                // MESHPUNK: wound v0,v3,v2,v1 rather than v0,v1,v2,v3. Every
                // vertex above declares a +Y normal, but the ascending order
                // gives (v1-v0)x(v2-v0) = -Y, so the mesh was lit as facing up
                // and culled as facing down. Under the default CULL_BACKFACES
                // a grid therefore drew nothing at all, and callers had to pass
                // CULL_FRONTFACES to see the surface they had already told the
                // lighting was upward. Reversing the quad makes geometry agree
                // with the declared normals; UVs are per-vertex so they are
                // unaffected.
                grid->addFace(v0, v3, v2, v1, (r + c) % 2 == 0 ? material : material2);
            }
        }

        grid->calculateBoundingBox();

        return grid;
    }

    Object *createPlane(int32_t width, int32_t height, Material *material)
    {
        Object *plane = new Object();

        int32_t hw = width / 2;
        int32_t hh = height / 2;

        // Define vertices (lying on the XZ plane at Y = 0)
        plane->addVertex({{-hw, 0, -hh}, {0, 0}, {0, FIXED_POINT_SCALE, 0}}); // v0
        plane->addVertex({{hw, 0, -hh}, {FIXED_POINT_SCALE, 0}, {0, FIXED_POINT_SCALE, 0}});  // v1
        plane->addVertex({{hw, 0, hh}, {FIXED_POINT_SCALE, FIXED_POINT_SCALE}, {0, FIXED_POINT_SCALE, 0}});   // v2
        plane->addVertex({{-hw, 0, hh}, {0, FIXED_POINT_SCALE}, {0, FIXED_POINT_SCALE, 0}});  // v3

        // MESHPUNK: reversed, same defect as createGrid — the four vertices
        // declare +Y normals while 0,1,2,3 winds -Y, so the plane was invisible
        // under the default CULL_BACKFACES.
        plane->addFace(0, 3, 2, 1, material);

        plane->calculateBoundingBox();

        return plane;
    }

    Object *createPyramid(int32_t baseSize, int32_t height, Material *material)
    {
        Object *pyramid = new Object();

        int32_t halfBase = baseSize / 2;
        const int32_t N = FIXED_POINT_SCALE;

        // 4 base verts + 4 sides × 3 verts = 16 verts. Per-face vertex
        // ownership avoids the destructive shared-vertex normal stamping
        // the previous implementation had: it used 5 shared verts and
        // overwrote each base corner's downward normal with whichever
        // side face happened to reference it last, leaving the base
        // tris with garbage normals and the apex with no normal at all.
        Vector2 apexUV = {FIXED_POINT_SCALE / 2, FIXED_POINT_SCALE / 2};

        // Base (-Y), CCW from below so the normal points down.
        pyramid->addVertex({{-halfBase, 0, -halfBase}, {0, 0},                                       {0, -N, 0}});
        pyramid->addVertex({{ halfBase, 0,  halfBase}, {FIXED_POINT_SCALE, FIXED_POINT_SCALE},       {0, -N, 0}});
        pyramid->addVertex({{ halfBase, 0, -halfBase}, {FIXED_POINT_SCALE, 0},                       {0, -N, 0}});
        pyramid->addVertex({{-halfBase, 0,  halfBase}, {0, FIXED_POINT_SCALE},                       {0, -N, 0}});
        // MESHPUNK: base winding corrected. As shipped these were (0,1,2) and
        // (0,3,1), whose (v1-v0)x(v2-v0) points +Y — into the solid — despite
        // the comment above. Verified numerically against Jet's own convention
        // (the one createSphere follows, where that cross product is outward).
        pyramid->addTriangle(0, 2, 1, material);
        pyramid->addTriangle(0, 1, 3, material);

        // Four side faces. Each gets its own three verts so the apex can
        // hold per-side normals — `computeFlatNormals` then assigns the
        // real cross-product face normal to all three.
        auto addSide = [&](int32_t x0, int32_t z0, int32_t x1, int32_t z1) {
            uint16_t b = (uint16_t)pyramid->vertices.size();
            pyramid->addVertex({{x0, 0, z0},         {0, 0},                                 {0, 0, 0}});
            pyramid->addVertex({{x1, 0, z1},         {FIXED_POINT_SCALE, 0},                 {0, 0, 0}});
            pyramid->addVertex({{ 0, height, 0},     apexUV,                                 {0, 0, 0}});
            // MESHPUNK: was (b, b+1, b+2) = (base0, base1, apex), which yields
            // an INWARD normal — backface culling then removed the faces
            // nearest the camera and kept the far ones, so the pyramid
            // rendered inside-out. Apex before the second base vertex fixes
            // the winding, and computeFlatNormals below then derives outward
            // face normals, correcting the lighting with it.
            pyramid->addTriangle(b, b + 2, b + 1, material);
        };
        // Wind each side CCW viewed from outside so outward normals win
        // backface culling.
        addSide(-halfBase, -halfBase,  halfBase, -halfBase); // front (-Z)
        addSide( halfBase, -halfBase,  halfBase,  halfBase); // right (+X)
        addSide( halfBase,  halfBase, -halfBase,  halfBase); // back (+Z)
        addSide(-halfBase,  halfBase, -halfBase, -halfBase); // left (-X)

        pyramid->computeFlatNormals();
        pyramid->calculateBoundingBox();

        return pyramid;
    }

    Object *createSphere(int32_t radius, int32_t segments, Material *material, int32_t uScale, int32_t vScale)
    {
        Object *sphere = new Object();

        // Ensure at least 3 segments
        if (segments < 3)
            segments = 3;

        // Angles in fixed-point degrees
        int32_t angleStep = ANGLE_MAX / segments;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        // Generate vertices
        for (int32_t latAngle = 0; latAngle <= 180; latAngle += angleStep)
        {
            int32_t sinLat = lookupSinI(latAngle % ANGLE_MAX);
            int32_t cosLat = lookupCosI(latAngle % ANGLE_MAX);

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
                int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
                int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

                // Calculate vertex position
                Vector3 vertexPosition = {radius * sinLat * cosLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE,
                                          radius * cosLat / FIXED_POINT_SCALE,
                                          radius * sinLat * sinLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE};

                // Fixed UV coordinates
                Vector2 uv = {
                    (lonAngle * maxU * uScale) / 360,
                    (latAngle * maxV * vScale) / 180
                };

                // Normal vector (normalized position vector)
                Vector3 normal = vertexPosition * FIXED_POINT_SCALE / radius;

                sphere->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate faces
        int latSegments = 180 / angleStep;
        int lonSegments = 360 / angleStep;

        for (int lat = 0; lat < latSegments; ++lat)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
                int current = lat * (lonSegments + 1) + lon;
                int next = current + lonSegments + 1;

                // To wrap around, connect the last column with the first
                int v0 = current;
                int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
                int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
                int v3 = next;

                // Add a quad face using the four vertices
                sphere->addFace(v0, v1, v2, v3, material);
            }
        }

        sphere->calculateBoundingBox();

        return sphere;
    }

    Object *createCapsule(int32_t radius, int32_t height, int32_t segments, Material *material)
    {
        Object *capsule = new Object();

        // Ensure at least 3 segments
        if (segments < 3)
            segments = 3;

        // Angles in fixed-point degrees
        int32_t angleStep = ANGLE_MAX / segments;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        // Generate vertices for the top hemisphere
        for (int32_t latAngle = 0; latAngle <= 90; latAngle += angleStep)
        {
            int32_t sinLat = lookupSinI(latAngle % ANGLE_MAX);
            int32_t cosLat = lookupCosI(latAngle % ANGLE_MAX);

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // Calculate vertex position
            Vector3 vertexPosition = {radius * sinLat * cosLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE,
                          radius * cosLat / FIXED_POINT_SCALE + height / 2,
                          radius * sinLat * sinLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (latAngle * maxV) / 180
            };

            // Normal vector (normalized position vector)
            Vector3 normal = vertexPosition * FIXED_POINT_SCALE / radius;

            capsule->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate vertices for the cylinder part
        for (int32_t i = 0; i <= segments; ++i)
        {
            int32_t y = height / 2 - i * height / segments;

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // Calculate vertex position
            Vector3 vertexPosition = {radius * cosLon / FIXED_POINT_SCALE,
                          y,
                          radius * sinLon / FIXED_POINT_SCALE};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (i * maxV) / segments
            };

            // Normal vector (normalized position vector)
            Vector3 normal = vertexPosition * FIXED_POINT_SCALE / radius;

            capsule->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate vertices for the bottom hemisphere
        for (int32_t latAngle = 90; latAngle <= 180; latAngle += angleStep)
        {
            int32_t sinLat = lookupSinI(latAngle % ANGLE_MAX);
            int32_t cosLat = lookupCosI(latAngle % ANGLE_MAX);

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // Calculate vertex position
            Vector3 vertexPosition = {radius * sinLat * cosLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE,
                          radius * cosLat / FIXED_POINT_SCALE - height / 2,
                          radius * sinLat * sinLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (latAngle * maxV) / 180
            };

            // Normal vector (normalized position vector)
            Vector3 normal = vertexPosition * FIXED_POINT_SCALE / radius;

            capsule->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate faces for the top hemisphere
        int latSegments = 90 / angleStep;
        int lonSegments = 360 / angleStep;

        for (int lat = 0; lat < latSegments; ++lat)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int current = lat * (lonSegments + 1) + lon;
            int next = current + lonSegments + 1;

            // To wrap around, connect the last column with the first
            int v0 = current;
            int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
            int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
            int v3 = next;

            // Add a quad face using the four vertices
            capsule->addFace(v0, v1, v2, v3, material);
            }
        }

        // Generate faces for the cylinder part
        int offset = (latSegments + 1) * (lonSegments + 1);
        for (int i = 0; i < segments; ++i)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int current = offset + i * (lonSegments + 1) + lon;
            int next = current + lonSegments + 1;

            // To wrap around, connect the last column with the first
            int v0 = current;
            int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
            int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
            int v3 = next;

            // Add a quad face using the four vertices
            capsule->addFace(v0, v1, v2, v3, material);
            }
        }

        // Generate faces for the bottom hemisphere
        offset += (segments + 1) * (lonSegments + 1);
        for (int lat = 0; lat < latSegments; ++lat)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int current = offset + lat * (lonSegments + 1) + lon;
            int next = current + lonSegments + 1;

            // To wrap around, connect the last column with the first
            int v0 = current;
            int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
            int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
            int v3 = next;

            // Add a quad face using the four vertices
            capsule->addFace(v0, v1, v2, v3, material);
            }
        }

        capsule->calculateBoundingBox();

        return capsule;
    }

    Object *createCylinder(int32_t radius, int32_t height, int32_t segments, bool closedCaps, Material *material, int32_t rings)
    {
        Object *cylinder = new Object();

        // Ensure at least 3 segments
        if (segments < 3)
            segments = 3;

        // Angles in fixed-point degrees
        int32_t angleStep = ANGLE_MAX / segments;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        // Generate vertices for the top and bottom circles
        for (int32_t i = 0; i <= 1; ++i)
        {
            int32_t y = (i == 0) ? height / 2 : -height / 2;

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // MESHPUNK: int64 intermediate. `radius * cosLon` is int32 * int32,
            // and cosLon reaches FIXED_POINT_SCALE (1024), so this overflowed
            // for any radius above 2,097,152 — reached by using a cylinder as a
            // horizon backdrop, where the radius has to clear the whole map.
            Vector3 vertexPosition = {
                (int32_t)(((int64_t)radius * cosLon) / FIXED_POINT_SCALE),
                y,
                (int32_t)(((int64_t)radius * sinLon) / FIXED_POINT_SCALE)};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (i * maxV)
            };

            // Normal vector (normalized position vector)
            Vector3 normal = {cosLon, 0, sinLon};

            cylinder->addVertex({vertexPosition, uv, normal});
            }
        }

        // MESHPUNK: the barrel's VERTICAL subdivision is independent of the
        // angular one. 0 preserves the original segments-for-both behaviour.
        const int32_t vrings = (rings > 0) ? rings : segments;

        // Generate vertices for the side faces
        for (int32_t i = 0; i <= vrings; ++i)
        {
            int32_t y = height / 2 - i * height / vrings;

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // MESHPUNK: int64 intermediate — see the top/bottom rings above.
            Vector3 vertexPosition = {
                (int32_t)(((int64_t)radius * cosLon) / FIXED_POINT_SCALE),
                y,
                (int32_t)(((int64_t)radius * sinLon) / FIXED_POINT_SCALE)};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (i * maxV) / vrings
            };

            // Normal vector (normalized position vector)
            Vector3 normal = {cosLon, 0, sinLon};

            cylinder->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate faces for the top and bottom circles
        int lonSegments = 360 / angleStep;

        // MESHPUNK: the caps are built from dedicated vertices appended here.
        //
        // Upstream fanned the top cap around index `lonSegments + 1` and the
        // bottom around `2 * (lonSegments + 1)`, but no centre vertices are ever
        // created: those indices are the first vertex of the BOTTOM ring and the
        // first SIDE-WALL vertex respectively. The result was fans hubbed on the
        // far end and on the wall — triangles cutting through the body, mostly
        // backface-culled, leaving the cylinder open at both ends.
        //
        // The existing ring vertices also carry radial normals ({cos,0,sin}),
        // which are right for the wall and wrong for a cap, so reusing them
        // would light the caps as if they were vertical. Dedicated rings with
        // +/-Y normals are appended instead. Appending (rather than inserting)
        // keeps every existing index — and therefore the side-wall face loop
        // below — untouched.
        //
        // Winding follows Jet's convention, established by the sphere and the
        // side wall: (v1-v0) x (v2-v0) points outward. For a ring point a(theta)
        // and its successor b(theta+step), (a-c) x (b-c) points -Y, so the top
        // cap is wound (centre, b, a) and the bottom (centre, a, b).
        if (closedCaps)
        {
            const int capBase = (int)cylinder->vertices.size();

            // Top centre, then the top ring; same again for the bottom.
            const int topCentre = capBase;
            cylinder->addVertex({{0, height / 2, 0}, {maxU / 2, maxV / 2},
                                 {0, FIXED_POINT_SCALE, 0}});
            const int topRing = (int)cylinder->vertices.size();
            for (int lon = 0; lon < lonSegments; ++lon)
            {
                int32_t lonAngle = lon * angleStep;
                int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
                int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);
                // MESHPUNK: int64 intermediate — see the barrel rings above.
                Vector3 p = {(int32_t)(((int64_t)radius * cosLon) / FIXED_POINT_SCALE),
                             height / 2,
                             (int32_t)(((int64_t)radius * sinLon) / FIXED_POINT_SCALE)};
                Vector2 uv = {(int32_t)(maxU / 2 + cosLon / 2),
                              (int32_t)(maxV / 2 + sinLon / 2)};
                cylinder->addVertex({p, uv, {0, FIXED_POINT_SCALE, 0}});
            }

            const int botCentre = (int)cylinder->vertices.size();
            cylinder->addVertex({{0, -height / 2, 0}, {maxU / 2, maxV / 2},
                                 {0, -FIXED_POINT_SCALE, 0}});
            const int botRing = (int)cylinder->vertices.size();
            for (int lon = 0; lon < lonSegments; ++lon)
            {
                int32_t lonAngle = lon * angleStep;
                int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
                int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);
                // MESHPUNK: int64 intermediate — see the barrel rings above.
                Vector3 p = {(int32_t)(((int64_t)radius * cosLon) / FIXED_POINT_SCALE),
                             -height / 2,
                             (int32_t)(((int64_t)radius * sinLon) / FIXED_POINT_SCALE)};
                Vector2 uv = {(int32_t)(maxU / 2 + cosLon / 2),
                              (int32_t)(maxV / 2 + sinLon / 2)};
                cylinder->addVertex({p, uv, {0, -FIXED_POINT_SCALE, 0}});
            }

            for (int lon = 0; lon < lonSegments; ++lon)
            {
                const int nextLon = (lon + 1) % lonSegments;
                cylinder->addTriangle((uint16_t)topCentre,
                                      (uint16_t)(topRing + nextLon),
                                      (uint16_t)(topRing + lon), material);
                cylinder->addTriangle((uint16_t)botCentre,
                                      (uint16_t)(botRing + lon),
                                      (uint16_t)(botRing + nextLon), material);
            }
        }

        // Generate faces for the side faces
        int offset = 2 * (lonSegments + 1);
        for (int i = 0; i < vrings; ++i)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int current = offset + i * (lonSegments + 1) + lon;
            int next = current + lonSegments + 1;

            // To wrap around, connect the last column with the first
            int v0 = current;
            int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
            int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
            int v3 = next;

            // Add a quad face using the four vertices
            cylinder->addFace(v0, v1, v2, v3, material);
            }
        }

        cylinder->calculateBoundingBox();

        return cylinder;
    }

    Object *createQuad(int32_t width, int32_t height, Material *material)
    {
        Object *quad = new Object();

        // Define vertices for a plane centered at the origin
        int32_t hw = width / 2;
        int32_t hh = height / 2;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        quad->addVertex({{-hw, -hh, 0}, {0, 0},       {0, 0, FIXED_POINT_SCALE}}); // v0
        quad->addVertex({{ hw, -hh, 0}, {maxU, 0},    {0, 0, FIXED_POINT_SCALE}}); // v1
        quad->addVertex({{ hw,  hh, 0}, {maxU, maxV}, {0, 0, FIXED_POINT_SCALE}}); // v2
        quad->addVertex({{-hw,  hh, 0}, {0, maxV},    {0, 0, FIXED_POINT_SCALE}}); // v3

        // Define two triangles
        quad->addTriangle(0, 1, 2, material);
        quad->addTriangle(0, 2, 3, material);

        quad->calculateBoundingBox();

        return quad;
    }

    Object *createBillboard(int32_t width, int32_t height, Material *material)
    {
        Object *billboard = createQuad(width, height, material);
        billboard->isBillboard = true;
        return billboard;
    }

} // namespace Primitives

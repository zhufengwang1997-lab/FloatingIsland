#include "scene/IslandMesh.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

// ---- Noise -----------------------------------------------------------------

float hash(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

// Smoothly interpolated random values on an integer lattice, in [0, 1].
float valueNoise(float x, float y, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);
    const float u = fx * fx * (3.0f - 2.0f * fx);
    const float v = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash(xi, yi, seed), b = hash(xi + 1, yi, seed);
    const float c = hash(xi, yi + 1, seed), d = hash(xi + 1, yi + 1, seed);
    return glm::mix(glm::mix(a, b, u), glm::mix(c, d, u), v);
}

// Fractal noise: several octaves of value noise, each finer and fainter. Roughly [0, 1].
float fbm(float x, float y, uint32_t seed, int octaves = 5) {
    float sum = 0.0f, amplitude = 0.5f, frequency = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * valueNoise(x * frequency, y * frequency, seed + static_cast<uint32_t>(i) * 101u);
        norm += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return sum / norm;
}

// ---- Island shape ----------------------------------------------------------

// Distance from the centre relative to a noisy coastline: < 1 is land, >= 1 is open sky.
float coastDistance(float x, float z, const IslandParams& p) {
    const float r = std::sqrt(x * x + z * z) / p.radius;
    const float coastline = 0.75f + 0.35f * fbm(x / p.radius * 1.5f + 10.0f, z / p.radius * 1.5f, p.seed + 50);
    return r / coastline;
}

float topHeight(float x, float z, float rn, const IslandParams& p) {
    const float falloff = 1.0f - glm::smoothstep(0.45f, 1.0f, rn);  // flatten towards the coast
    const float hills = fbm(x * 0.035f, z * 0.035f, p.seed);
    return p.maxHeight * falloff * (0.15f + 1.2f * hills * hills);
}

float bottomDepth(float x, float z, float rn, const IslandParams& p) {
    const float cone = std::pow(1.0f - rn, 0.6f);  // deepest in the middle, zero at the coast
    const float rough = 0.55f + 0.6f * fbm(x * 0.08f, z * 0.08f, p.seed + 200);
    return p.depth * cone * rough;
}

glm::vec3 topColor(float height, float rn, const IslandParams& p) {
    const glm::vec3 sand{0.76f, 0.66f, 0.42f};
    const glm::vec3 grass{0.20f, 0.42f, 0.12f};
    const glm::vec3 darkGrass{0.12f, 0.30f, 0.08f};
    const glm::vec3 rock{0.42f, 0.40f, 0.37f};
    const float t = height / p.maxHeight;
    glm::vec3 c = glm::mix(grass, darkGrass, glm::smoothstep(0.2f, 0.6f, t));
    c = glm::mix(c, rock, glm::smoothstep(0.65f, 0.85f, t));
    c = glm::mix(c, sand, glm::smoothstep(0.85f, 0.97f, rn));
    return c;
}

glm::vec3 bottomColor(float depth, const IslandParams& p) {
    const glm::vec3 soil{0.36f, 0.25f, 0.16f};
    const glm::vec3 deepRock{0.20f, 0.17f, 0.15f};
    return glm::mix(soil, deepRock, glm::smoothstep(0.1f, 0.8f, depth / p.depth));
}

// ---- Mesh building ---------------------------------------------------------

void addLayer(MeshData& mesh, const IslandParams& p, bool top) {
    const int n = p.resolution;
    const int side = n + 1;
    // The coastline reaches out to ~1.1 x radius, so the grid covers a bit more than that.
    const float extent = 1.15f * p.radius;
    const float step = 2.0f * extent / static_cast<float>(n);
    auto gridX = [&](int i) { return -extent + static_cast<float>(i) * step; };

    // Distance to the coastline at each grid point: < 1 is land.
    std::vector<float> rn(static_cast<size_t>(side * side));
    for (int j = 0; j < side; ++j)
        for (int i = 0; i < side; ++i) rn[j * side + i] = coastDistance(gridX(i), gridX(j), p);

    // A vertex of this layer at a point on the island. At the coastline (r = 1) both layers have
    // y = 0: the hills flatten out and the underside thins to nothing there, so they meet exactly.
    auto makeVertex = [&](float x, float z, float r) {
        Vertex v{};
        if (top) {
            const float h = r >= 1.0f ? 0.0f : topHeight(x, z, r, p);
            v.position = {x, h, z};
            v.color = topColor(h, std::min(r, 1.0f), p);
            // Grass and sand are very rough; exposed rock on the peaks a little less so.
            v.material = {glm::mix(0.9f, 0.65f, glm::smoothstep(0.65f, 0.85f, h / p.maxHeight)), 0.0f};
        } else {
            const float d = r >= 1.0f ? 0.0f : bottomDepth(x, z, r, p);
            v.position = {x, -d, z};
            v.color = bottomColor(d, p);
            v.material = {0.85f, 0.0f};
        }
        mesh.vertices.push_back(v);
        return static_cast<uint32_t>(mesh.vertices.size() - 1);
    };

    // Land grid points, created on first use.
    std::vector<int64_t> gridIndex(static_cast<size_t>(side * side), -1);
    auto gridVertex = [&](int g) {
        if (gridIndex[g] < 0) gridIndex[g] = makeVertex(gridX(g % side), gridX(g / side), rn[g]);
        return static_cast<uint32_t>(gridIndex[g]);
    };
    // Points where a grid edge crosses the coastline, shared by the two triangles on that edge.
    std::unordered_map<uint64_t, uint32_t> crossings;
    auto coastVertex = [&](int inner, int outer) {
        const uint64_t key = static_cast<uint64_t>(std::min(inner, outer)) << 32 | static_cast<uint32_t>(std::max(inner, outer));
        if (auto it = crossings.find(key); it != crossings.end()) return it->second;
        // Start from the linear estimate along the edge, then bisect on the real coastline so the
        // point lies on it (r = 1) rather than on the grid's linear approximation of it.
        const glm::vec2 a{gridX(inner % side), gridX(inner / side)};
        const glm::vec2 b{gridX(outer % side), gridX(outer / side)};
        float lo = 0.0f, hi = 1.0f;  // r(lo) < 1 <= r(hi)
        for (int k = 0; k < 12; ++k) {
            const float mid = 0.5f * (lo + hi);
            const glm::vec2 q = glm::mix(a, b, mid);
            (coastDistance(q.x, q.y, p) < 1.0f ? lo : hi) = mid;
        }
        const glm::vec2 q = glm::mix(a, b, 0.5f * (lo + hi));
        const uint32_t v = makeVertex(q.x, q.y, 1.0f);
        crossings.emplace(key, v);
        return v;
    };

    // Each triangle of the grid, clipped to the land: the corners on land are kept, and where an
    // edge leaves the land a new vertex goes exactly on the coastline. What is left is a triangle
    // or a quad (a fan of two triangles). Clipping keeps the winding order.
    auto clippedTriangle = [&](int g0, int g1, int g2) {
        const int g[3] = {g0, g1, g2};
        uint32_t polygon[4];
        int count = 0;
        for (int e = 0; e < 3; ++e) {
            const int from = g[e], to = g[(e + 1) % 3];
            const bool fromIn = rn[from] < 1.0f, toIn = rn[to] < 1.0f;
            if (fromIn) polygon[count++] = gridVertex(from);
            if (fromIn != toIn) polygon[count++] = fromIn ? coastVertex(from, to) : coastVertex(to, from);
        }
        for (int k = 1; k + 1 < count; ++k) mesh.indices.insert(mesh.indices.end(), {polygon[0], polygon[k], polygon[k + 1]});
    };

    // Two triangles per grid cell. Top faces wind counter-clockwise seen from above (+Y), the
    // underside the other way.
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int a = j * side + i, b = a + 1, c = a + side, d = c + 1;
            if (top) {
                clippedTriangle(a, c, b);
                clippedTriangle(b, c, d);
            } else {
                clippedTriangle(a, b, c);
                clippedTriangle(b, d, c);
            }
        }
    }
}

// Smooth normals: each vertex averages the normals of the triangles around it.
void computeNormals(MeshData& mesh) {
    for (Vertex& v : mesh.vertices) v.normal = glm::vec3(0.0f);
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        Vertex& a = mesh.vertices[mesh.indices[t]];
        Vertex& b = mesh.vertices[mesh.indices[t + 1]];
        Vertex& c = mesh.vertices[mesh.indices[t + 2]];
        const glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);  // area-weighted
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (Vertex& v : mesh.vertices) {
        const float len = glm::length(v.normal);
        v.normal = len > 0.0f ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

}  // namespace

float islandNoise(float x, float z, uint32_t seed) {
    return fbm(x, z, seed, 4);
}

IslandSurface sampleIslandSurface(float x, float z, const IslandParams& p) {
    IslandSurface s;
    s.coast = coastDistance(x, z, p);
    s.land = s.coast < 1.0f;
    if (!s.land) return s;
    s.height = topHeight(x, z, s.coast, p);
    // Normal from central differences of the height field.
    const float e = 0.25f;
    const float dx = topHeight(x + e, z, coastDistance(x + e, z, p), p) - topHeight(x - e, z, coastDistance(x - e, z, p), p);
    const float dz = topHeight(x, z + e, coastDistance(x, z + e, p), p) - topHeight(x, z - e, coastDistance(x, z - e, p), p);
    s.normal = glm::normalize(glm::vec3(-dx, 2.0f * e, -dz));
    s.rockiness = glm::smoothstep(0.65f, 0.85f, s.height / p.maxHeight);
    s.sand = glm::smoothstep(0.85f, 0.97f, s.coast);
    return s;
}

MeshData generateIsland(const IslandParams& params) {
    MeshData mesh;
    addLayer(mesh, params, true);
    addLayer(mesh, params, false);
    computeNormals(mesh);
    return mesh;
}

#include "scene/IslandMesh.h"

#include <algorithm>
#include <cmath>

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

    // Which grid points are land.
    std::vector<float> rn(static_cast<size_t>(side * side));
    for (int j = 0; j < side; ++j)
        for (int i = 0; i < side; ++i) rn[j * side + i] = coastDistance(gridX(i), gridX(j), p);
    auto inside = [&](int i, int j) {
        return i >= 0 && j >= 0 && i < side && j < side && rn[j * side + i] < 1.0f;
    };

    std::vector<int32_t> index(static_cast<size_t>(side * side), -1);
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            if (!inside(i, j)) continue;
            const float x = gridX(i), z = gridX(j), r = rn[j * side + i];
            // Coast vertices sit at y = 0 in both layers so the top and the underside meet exactly.
            const bool coast = !inside(i - 1, j) || !inside(i + 1, j) || !inside(i, j - 1) || !inside(i, j + 1);
            Vertex v{};
            if (top) {
                const float h = coast ? 0.0f : topHeight(x, z, r, p);
                v.position = {x, h, z};
                v.color = topColor(h, coast ? 1.0f : r, p);
            } else {
                const float d = coast ? 0.0f : bottomDepth(x, z, r, p);
                v.position = {x, -d, z};
                v.color = bottomColor(d, p);
            }
            index[j * side + i] = static_cast<int32_t>(mesh.vertices.size());
            mesh.vertices.push_back(v);
        }
    }

    // Two triangles per grid cell, kept only when all three corners are land.
    // Top faces wind counter-clockwise seen from above (+Y), the underside the other way.
    auto tri = [&](int32_t a, int32_t b, int32_t c) {
        if (a < 0 || b < 0 || c < 0) return;
        mesh.indices.insert(mesh.indices.end(),
                            {static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c)});
    };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int32_t a = index[j * side + i], b = index[j * side + i + 1];
            const int32_t c = index[(j + 1) * side + i], d = index[(j + 1) * side + i + 1];
            if (top) {
                tri(a, c, b);
                tri(b, c, d);
            } else {
                tri(a, b, c);
                tri(b, d, c);
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

MeshData generateIsland(const IslandParams& params) {
    MeshData mesh;
    addLayer(mesh, params, true);
    addLayer(mesh, params, false);
    computeNormals(mesh);
    return mesh;
}

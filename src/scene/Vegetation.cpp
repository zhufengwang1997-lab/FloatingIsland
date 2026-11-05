#include "scene/Vegetation.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace {

// ---- Random numbers ----------------------------------------------------------

// Deterministic hash of a grid cell and a channel, in [0, 1).
float hash01(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ seed * 83492791u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// ---- Low-poly mesh building --------------------------------------------------

// Faceted look: every triangle has its own three vertices and a flat normal. `inside` is a point
// the face should look away from, so callers don't have to get the winding right by hand.
void addFacet(MeshData& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 inside, glm::vec3 color,
              float roughness) {
    glm::vec3 n = glm::cross(b - a, c - a);
    if (glm::dot(n, (a + b + c) / 3.0f - inside) < 0.0f) {
        std::swap(b, c);
        n = -n;
    }
    n = glm::normalize(n);
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& p : {a, b, c}) mesh.vertices.push_back({p, n, color, {roughness, 0.0f}});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
}

// Slight per-facet brightness variation, so flat-shaded surfaces don't look plastic.
glm::vec3 facetColor(glm::vec3 color, int facet, uint32_t seed) {
    return color * lerp(0.88f, 1.08f, hash01(facet, 0, seed));
}

// Truncated cone (radius r0 at y0 to r1 at y1) around the Y axis; r1 = 0 makes a cone.
// Caps the bottom so it looks closed from below.
void addFrustum(MeshData& mesh, glm::vec3 base, float y1, float r0, float r1, int sides, glm::vec3 color,
                float roughness, uint32_t seed) {
    const float y0 = base.y;
    auto ring = [&](int i, float r, float y) {
        const float a = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(sides);
        return glm::vec3(base.x + r * std::cos(a), y, base.z + r * std::sin(a));
    };
    for (int i = 0; i < sides; ++i) {
        const glm::vec3 b0 = ring(i, r0, y0), b1 = ring(i + 1, r0, y0);
        const glm::vec3 t0 = ring(i, r1, y1), t1 = ring(i + 1, r1, y1);
        const glm::vec3 axis{base.x, 0.5f * (y0 + y1), base.z};
        const glm::vec3 c = facetColor(color, i, seed);
        addFacet(mesh, b0, t0, b1, axis, c, roughness);
        if (r1 > 0.0f) addFacet(mesh, b1, t0, t1, axis, c, roughness);
        addFacet(mesh, glm::vec3(base.x, y0, base.z), b0, b1, glm::vec3(base.x, y0 + 1.0f, base.z),
                 c * 0.8f, roughness);
    }
}

// Icosphere with one subdivision (80 faces). Each vertex is pushed in or out by a random amount
// (bumpiness), and the whole blob is scaled by `scale` before being moved to `centre`.
void addBlob(MeshData& mesh, glm::vec3 centre, glm::vec3 scale, float bumpiness, glm::vec3 color, float roughness,
             uint32_t seed) {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<glm::vec3> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (glm::vec3& p : v) p = glm::normalize(p);
    std::vector<glm::uvec3> faces = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                                     {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                     {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                                     {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    // Split every triangle into four, sharing midpoints between neighbouring faces.
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> midpoints;
    auto midpoint = [&](uint32_t a, uint32_t b) {
        const auto key = std::minmax(a, b);
        auto it = midpoints.find(key);
        if (it != midpoints.end()) return it->second;
        v.push_back(glm::normalize(v[a] + v[b]));
        const auto index = static_cast<uint32_t>(v.size() - 1);
        midpoints[key] = index;
        return index;
    };
    std::vector<glm::uvec3> split;
    for (const glm::uvec3& f : faces) {
        const uint32_t ab = midpoint(f.x, f.y), bc = midpoint(f.y, f.z), ca = midpoint(f.z, f.x);
        split.insert(split.end(), {{f.x, ab, ca}, {f.y, bc, ab}, {f.z, ca, bc}, {ab, bc, ca}});
    }
    // Displace per shared vertex, so neighbouring facets still meet.
    for (size_t i = 0; i < v.size(); ++i) {
        const float bump = 1.0f + bumpiness * (hash01(static_cast<int>(i), 1, seed) * 2.0f - 1.0f);
        v[i] = centre + v[i] * bump * scale;
    }
    for (size_t i = 0; i < split.size(); ++i) {
        const glm::uvec3& f = split[i];
        addFacet(mesh, v[f.x], v[f.y], v[f.z], centre, facetColor(color, static_cast<int>(i), seed), roughness);
    }
}

MeshData makePine() {
    MeshData m;
    const glm::vec3 bark{0.28f, 0.18f, 0.10f};
    const glm::vec3 needles{0.07f, 0.22f, 0.09f};
    addFrustum(m, {0, -0.3f, 0}, 1.4f, 0.20f, 0.12f, 6, bark, 0.9f, 1);
    addFrustum(m, {0, 0.9f, 0}, 3.1f, 1.50f, 0.0f, 9, needles, 0.85f, 2);
    addFrustum(m, {0, 2.0f, 0}, 3.9f, 1.15f, 0.0f, 9, needles * 1.08f, 0.85f, 3);
    addFrustum(m, {0, 3.0f, 0}, 4.7f, 0.80f, 0.0f, 9, needles * 1.16f, 0.85f, 4);
    return m;
}

MeshData makeBroadleaf() {
    MeshData m;
    const glm::vec3 bark{0.30f, 0.20f, 0.12f};
    const glm::vec3 leaves{0.16f, 0.36f, 0.08f};
    addFrustum(m, {0, -0.3f, 0}, 2.6f, 0.25f, 0.15f, 6, bark, 0.9f, 5);
    addBlob(m, {0, 3.1f, 0}, {1.7f, 1.4f, 1.7f}, 0.18f, leaves, 0.8f, 6);
    addBlob(m, {0.8f, 3.8f, 0.3f}, {1.1f, 0.95f, 1.1f}, 0.18f, leaves * 1.12f, 0.8f, 7);
    return m;
}

MeshData makeRock() {
    MeshData m;
    addBlob(m, {0, 0, 0}, {1.0f, 0.65f, 1.0f}, 0.28f, {0.43f, 0.41f, 0.38f}, 0.85f, 8);
    return m;
}

// A tuft of six tapered blades, each three triangles. Normals point mostly up rather than across
// the blade: lit like the ground they grow from, the blades don't flicker as they face the sun or not.
MeshData makeGrassTuft() {
    MeshData m;
    const glm::vec3 root{0.09f, 0.22f, 0.05f};
    const glm::vec3 tip{0.36f, 0.58f, 0.16f};
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    constexpr int kBlades = 6;
    for (int i = 0; i < kBlades; ++i) {
        const float angle = glm::two_pi<float>() * (static_cast<float>(i) + 0.3f * hash01(i, 2, 9)) / kBlades;
        const glm::vec3 across{std::cos(angle), 0.0f, std::sin(angle)};  // blade width
        const glm::vec3 lean{-across.z, 0.0f, across.x};                 // blade bends this way
        const float radius = 0.08f + 0.12f * hash01(i, 3, 9);
        const glm::vec3 base = lean * radius + glm::vec3(0.0f, -0.05f, 0.0f);
        const float height = lerp(0.45f, 0.8f, hash01(i, 4, 9));
        const float width = 0.06f;
        const float bend = 0.18f;
        const glm::vec3 n = glm::normalize(up + lean * 0.35f);
        const auto first = static_cast<uint32_t>(m.vertices.size());
        auto vertex = [&](glm::vec3 p, float t) {
            // t^2 keeps the lower half of each blade darker: a baked stand-in for the shade the
            // neighbouring blades cast (grass is not drawn into the shadow map).
            m.vertices.push_back({p, n, glm::mix(root, tip, t * t), {0.8f, 0.0f}});
        };
        vertex(base - across * width, 0.0f);
        vertex(base + across * width, 0.0f);
        vertex(base - across * width * 0.6f + up * height * 0.5f + lean * bend * 0.3f, 0.5f);
        vertex(base + across * width * 0.6f + up * height * 0.5f + lean * bend * 0.3f, 0.5f);
        vertex(base + up * height + lean * bend, 1.0f);
        m.indices.insert(m.indices.end(), {first, first + 1, first + 3, first, first + 3, first + 2,
                                           first + 2, first + 3, first + 4});
    }
    return m;
}

// ---- Placement -----------------------------------------------------------------

// Visits one jittered point per grid cell of the given spacing, covering the whole island.
template <typename F>
void scatter(const IslandParams& island, float spacing, uint32_t seed, F&& visit) {
    const float extent = 1.15f * island.radius;
    const int cells = static_cast<int>(std::ceil(2.0f * extent / spacing));
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const float x = -extent + (static_cast<float>(i) + hash01(i, j, seed)) * spacing;
            const float z = -extent + (static_cast<float>(j) + hash01(i, j, seed + 1)) * spacing;
            auto random = [&, channel = 2u]() mutable { return hash01(i, j, seed + channel++); };
            visit(x, z, random);
        }
    }
}

InstanceData makeInstance(float x, float y, float z, float scale, float yaw, float tint, float wind) {
    InstanceData d;
    d.positionScale = {x, y, z, scale};
    d.params = {yaw, tint, wind, -1.0f};  // no LOD rank; the renderer ranks distance-LOD kinds
    return d;
}

}  // namespace

std::vector<VegetationKind> generateVegetation(const IslandParams& island, uint32_t seed, float grassDensity) {
    VegetationKind pines{"Pine trees", makePine(), {}, false};
    VegetationKind broadleaves{"Broadleaf trees", makeBroadleaf(), {}, false};
    VegetationKind rocks{"Rocks", makeRock(), {}, false};
    VegetationKind grass{"Grass", makeGrassTuft(), {}, true, false, true};
    const float twoPi = glm::two_pi<float>();

    // Trees: in forest patches (low-frequency noise), on gentle green slopes away from the beach.
    scatter(island, 2.6f, seed, [&](float x, float z, auto random) {
        const IslandSurface s = sampleIslandSurface(x, z, island);
        if (!s.land || s.coast > 0.82f) return;
        const float forest = islandNoise(x * 0.06f + 30.0f, z * 0.06f, seed + 300);
        const float chance = glm::smoothstep(0.42f, 0.62f, forest) * (1.0f - s.rockiness) * (1.0f - s.sand) *
                             glm::smoothstep(0.85f, 0.95f, s.normal.y);
        if (random() >= chance) return;
        const float scale = lerp(0.8f, 1.4f, random());
        const float highland = s.height / island.maxHeight;
        const bool pine = random() < lerp(0.25f, 0.85f, glm::smoothstep(0.15f, 0.45f, highland));
        // Sink the trunk a little: the terrain mesh is a linear interpolation of this height field.
        (pine ? pines : broadleaves)
            .instances.push_back(makeInstance(x, s.height - 0.1f, z, scale, random() * twoPi,
                                              lerp(0.85f, 1.15f, random()), 0.004f));
    });

    // Rocks: mostly on the bare peaks, a few scattered everywhere including the beach.
    scatter(island, 4.5f, seed + 1000, [&](float x, float z, auto random) {
        const IslandSurface s = sampleIslandSurface(x, z, island);
        if (!s.land || s.coast > 0.95f) return;
        if (random() >= 0.12f + 0.75f * s.rockiness) return;
        const float scale = lerp(0.4f, 1.6f, random() * random());  // many small, few large
        rocks.instances.push_back(makeInstance(x, s.height - 0.25f * scale, z, scale, random() * twoPi,
                                               lerp(0.8f, 1.15f, random()), 0.0f));
    });

    // Grass: dense on green ground, thinning out on rock, sand and steep slopes.
    // Density scales tufts per area, so the grid spacing shrinks with its square root.
    const float grassSpacing = 0.42f / std::sqrt(std::max(grassDensity, 0.01f));
    scatter(island, grassSpacing, seed + 2000, [&](float x, float z, auto random) {
        const IslandSurface s = sampleIslandSurface(x, z, island);
        if (!s.land || s.coast > 0.93f) return;
        const float patchiness = 0.5f + 0.5f * islandNoise(x * 0.2f, z * 0.2f, seed + 400);
        const float chance = (1.0f - s.rockiness) * (1.0f - 0.9f * s.sand) *
                             glm::smoothstep(0.7f, 0.85f, s.normal.y) * patchiness;
        if (random() >= chance) return;
        // Darker on the hills, like the terrain colour underneath.
        const float tint = lerp(1.1f, 0.75f, glm::smoothstep(0.2f, 0.6f, s.height / island.maxHeight)) *
                           lerp(0.85f, 1.1f, random());
        grass.instances.push_back(makeInstance(x, s.height, z, lerp(0.7f, 1.3f, random()), random() * twoPi,
                                               tint, 0.35f));
    });

    std::vector<VegetationKind> kinds;
    for (VegetationKind* k : {&pines, &broadleaves, &rocks, &grass}) {
        if (!k->instances.empty()) kinds.push_back(std::move(*k));
    }
    return kinds;
}

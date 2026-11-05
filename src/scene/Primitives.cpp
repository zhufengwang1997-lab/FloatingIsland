#include "scene/Primitives.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

MeshData generateSphere(float radius, int segments, int rings, glm::vec3 color, float roughness, float metallic) {
    MeshData mesh;
    // Rings run from the north pole (phi = 0) to the south pole (phi = pi);
    // segments go around the Y axis (theta = 0..2pi). The seam column is duplicated.
    for (int r = 0; r <= rings; ++r) {
        const float phi = glm::pi<float>() * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s <= segments; ++s) {
            const float theta = glm::two_pi<float>() * static_cast<float>(s) / static_cast<float>(segments);
            const glm::vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            mesh.vertices.push_back({n * radius, n, color, {roughness, metallic}});
        }
    }
    const int row = segments + 1;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const uint32_t a = static_cast<uint32_t>(r * row + s), b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(row), d = c + 1;
            // Counter-clockwise seen from outside.
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    }
    return mesh;
}

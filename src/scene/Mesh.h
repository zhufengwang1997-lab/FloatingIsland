#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// Interleaved vertex layout shared by every mesh. Matches VSInput in shaders/mesh.hlsl.
struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;     // base color (albedo), linear
    glm::vec2 material;  // x: roughness, y: metallic
};

// Per-instance data for instanced draws (vertex binding 1, input rate instance).
// Matches InstanceInput in shaders/common.hlsli.
struct InstanceData {
    glm::vec4 positionScale{0.0f, 0.0f, 0.0f, 1.0f};  // xyz: position in object space, w: uniform scale
    glm::vec4 params{0.0f, 1.0f, 0.0f, -1.0f};        // x: yaw (radians), y: color tint, z: wind sway, w: LOD rank
    // (LOD rank, 0..1: distance LOD drops instances with high ranks first; negative: no LOD)
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // counter-clockwise front faces
};

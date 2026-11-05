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

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // counter-clockwise front faces
};

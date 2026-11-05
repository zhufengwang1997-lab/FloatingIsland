#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct IslandParams {
    float radius = 40.0f;     // rough island radius in world units
    int resolution = 192;     // grid cells along each side
    float maxHeight = 12.0f;  // tallest hills on top
    float depth = 30.0f;      // how far the rock hangs below
    uint32_t seed = 7;
};

// A floating island: a hilly top surface and a hanging rock underside,
// joined at an irregular coastline. Generated from value noise on a grid.
MeshData generateIsland(const IslandParams& params = {});

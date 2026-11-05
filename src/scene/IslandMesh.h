#pragma once

#include "scene/Mesh.h"

struct IslandParams {
    float radius = 40.0f;     // rough island radius in world units
    int resolution = 192;     // grid cells along each side
    float maxHeight = 12.0f;  // tallest hills on top
    float depth = 30.0f;      // how far the rock hangs below
    uint32_t seed = 7;
};

// The island's top surface at a point, for placing objects on it.
struct IslandSurface {
    bool land = false;        // inside the coastline
    float height = 0.0f;      // top surface y
    float coast = 1.0f;       // 0 at the centre, 1 at the coastline
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float rockiness = 0.0f;   // 0 grass .. 1 bare rock (the peaks), as in the surface colour
    float sand = 0.0f;        // 0 .. 1 towards the beach along the coast
};
IslandSurface sampleIslandSurface(float x, float z, const IslandParams& params = {});

// Smooth fractal noise in about [0, 1] (the same noise the terrain uses).
float islandNoise(float x, float z, uint32_t seed);

// A floating island: a hilly top surface and a hanging rock underside,
// joined at an irregular coastline. Generated from value noise on a grid.
MeshData generateIsland(const IslandParams& params = {});

#pragma once

#include "scene/IslandMesh.h"
#include "scene/Mesh.h"

#include <string>
#include <vector>

// One kind of scattered object: a single mesh drawn with many instances.
struct VegetationKind {
    std::string name;
    MeshData mesh;  // in instance space: origin at the base, +Y up
    std::vector<InstanceData> instances;
    bool twoSided = false;  // thin geometry (grass blades), drawn without back-face culling
};

// Pine trees, broadleaf trees, rocks and grass tufts scattered over the island's top surface.
// Placement follows the terrain: trees in forest patches on gentle slopes, rocks mostly on the
// rocky peaks, grass on green ground. Deterministic for a given seed.
// grassDensity multiplies the number of grass tufts (used to make benchmarks GPU-heavy).
std::vector<VegetationKind> generateVegetation(const IslandParams& island = {}, uint32_t seed = 11,
                                               float grassDensity = 1.0f);

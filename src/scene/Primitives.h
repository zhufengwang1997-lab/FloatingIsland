#pragma once

#include "scene/Mesh.h"

// UV sphere centred at the origin, one material for the whole surface.
MeshData generateSphere(float radius, int segments, int rings, glm::vec3 color, float roughness, float metallic);

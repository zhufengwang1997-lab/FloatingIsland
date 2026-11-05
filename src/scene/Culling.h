#pragma once

#include <glm/glm.hpp>

// Axis-aligned bounding box in world space.
struct Aabb {
    glm::vec3 min{1e30f};
    glm::vec3 max{-1e30f};

    void expand(glm::vec3 p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void expand(const Aabb& b) {
        min = glm::min(min, b.min);
        max = glm::max(max, b.max);
    }
    bool empty() const { return min.x > max.x; }
};

// The bounds of a box after transforming it (the box around its eight transformed corners).
Aabb transformAabb(const Aabb& box, const glm::mat4& m);

// The six planes of a view-projection matrix's view volume, pointing inwards: a point p is inside
// when dot(plane.xyz, p) + plane.w >= 0 for all of them. Assumes Vulkan clip space (depth 0..1).
struct Frustum {
    glm::vec4 planes[6];
};

Frustum extractFrustum(const glm::mat4& viewProj);

// False only when the box is certainly outside (entirely behind one plane). Boxes near a corner of
// the frustum can pass while outside it; that only costs drawing a little too much, never too little.
bool intersects(const Frustum& frustum, const Aabb& box);

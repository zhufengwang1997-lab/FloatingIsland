#include "scene/Culling.h"

Aabb transformAabb(const Aabb& box, const glm::mat4& m) {
    Aabb out;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                               (i & 4) ? box.max.z : box.min.z};
        out.expand(glm::vec3(m * glm::vec4(corner, 1.0f)));
    }
    return out;
}

Frustum extractFrustum(const glm::mat4& viewProj) {
    // Gribb & Hartmann: clip = M * p, and p is inside when -w <= x <= w, -w <= y <= w, 0 <= z <= w.
    // Each inequality is a plane built from rows of M (glm is column-major, so row i is m[c][i]).
    auto row = [&](int i) { return glm::vec4(viewProj[0][i], viewProj[1][i], viewProj[2][i], viewProj[3][i]); };
    const glm::vec4 x = row(0), y = row(1), z = row(2), w = row(3);
    Frustum f;
    f.planes[0] = w + x;  // left
    f.planes[1] = w - x;  // right
    f.planes[2] = w + y;  // bottom / top (Vulkan's Y flip just swaps which is which)
    f.planes[3] = w - y;
    f.planes[4] = z;      // near (depth >= 0)
    f.planes[5] = w - z;  // far
    for (glm::vec4& p : f.planes) p /= glm::length(glm::vec3(p));
    return f;
}

bool intersects(const Frustum& frustum, const Aabb& box) {
    for (const glm::vec4& p : frustum.planes) {
        // The box corner furthest along the plane normal: if even that one is behind the plane,
        // the whole box is.
        const glm::vec3 far{p.x >= 0.0f ? box.max.x : box.min.x, p.y >= 0.0f ? box.max.y : box.min.y,
                            p.z >= 0.0f ? box.max.z : box.min.z};
        if (glm::dot(glm::vec3(p), far) + p.w < 0.0f) return false;
    }
    return true;
}

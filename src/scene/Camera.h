#pragma once

#include <glm/glm.hpp>

struct GLFWwindow;

// Fly camera. Hold the right mouse button to look around;
// WASD to move, E / Q up and down, Shift to move faster.
class Camera {
public:
    void update(GLFWwindow* window, float dt);

    glm::vec3 forward() const;
    glm::mat4 view() const;
    // Vulkan conventions: depth in [0, 1] (GLM_FORCE_DEPTH_ZERO_TO_ONE) and +Y down in clip space.
    glm::mat4 projection(float aspect) const;

    glm::vec3 position{0.0f, 28.0f, 85.0f};
    float yaw = glm::radians(-90.0f);   // -90 degrees looks down -Z
    float pitch = glm::radians(-18.0f);
    float fovY = glm::radians(60.0f);
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    float moveSpeed = 20.0f;            // units per second
    float lookSensitivity = 0.003f;     // radians per pixel

private:
    bool looking_ = false;
    double lastCursorX_ = 0.0;
    double lastCursorY_ = 0.0;
};

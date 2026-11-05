#include "scene/Camera.h"

#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace {
constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
}

void Camera::update(GLFWwindow* window, float dt) {
    // Mouse look while the right button is held; the cursor is hidden and unbounded meanwhile.
    const bool rightDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (rightDown) {
        double x = 0.0, y = 0.0;
        glfwGetCursorPos(window, &x, &y);
        if (!looking_) {
            looking_ = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else {
            yaw += static_cast<float>(x - lastCursorX_) * lookSensitivity;
            pitch -= static_cast<float>(y - lastCursorY_) * lookSensitivity;
            pitch = std::clamp(pitch, glm::radians(-89.0f), glm::radians(89.0f));
        }
        lastCursorX_ = x;
        lastCursorY_ = y;
    } else if (looking_) {
        looking_ = false;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    const glm::vec3 front = forward();
    const glm::vec3 right = glm::normalize(glm::cross(front, kWorldUp));
    glm::vec3 move{0.0f};
    auto held = [window](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    if (held(GLFW_KEY_W)) move += front;
    if (held(GLFW_KEY_S)) move -= front;
    if (held(GLFW_KEY_D)) move += right;
    if (held(GLFW_KEY_A)) move -= right;
    if (held(GLFW_KEY_E)) move += kWorldUp;
    if (held(GLFW_KEY_Q)) move -= kWorldUp;
    if (glm::dot(move, move) > 0.0f) {
        const float speed = moveSpeed * (held(GLFW_KEY_LEFT_SHIFT) ? 4.0f : 1.0f);
        position += glm::normalize(move) * speed * dt;
    }
}

glm::vec3 Camera::forward() const {
    return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
}

glm::mat4 Camera::view() const {
    return glm::lookAt(position, position + forward(), kWorldUp);
}

glm::mat4 Camera::projection(float aspect) const {
    glm::mat4 proj = glm::perspective(fovY, aspect, nearPlane, farPlane);
    proj[1][1] *= -1.0f;  // GLM assumes OpenGL's +Y up; Vulkan clip space has +Y down
    return proj;
}

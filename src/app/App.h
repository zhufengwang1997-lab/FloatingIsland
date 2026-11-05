#pragma once

#include "renderer/Renderer.h"
#include "rhi/VulkanContext.h"
#include "scene/Camera.h"

#include <string>

struct GLFWwindow;

// Owns the window and the main loop.
class App {
public:
    // maxFrames > 0 quits after that many frames; 0 runs until the window is closed.
    // A non-empty screenshotPath saves the last of those frames there.
    void run(int maxFrames = 0, const std::string& screenshotPath = {});

private:
    void init();
    void loadScene();
    void mainLoop(int maxFrames, const std::string& screenshotPath);
    void shutdown();

    GLFWwindow* window_ = nullptr;
    VulkanContext ctx_;
    Renderer renderer_;
    Camera camera_;
};

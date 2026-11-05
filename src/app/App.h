#pragma once

#include "renderer/Renderer.h"
#include "rhi/VulkanContext.h"

struct GLFWwindow;

// Owns the window and the main loop.
class App {
public:
    // maxFrames > 0 quits after that many frames; 0 runs until the window is closed.
    void run(int maxFrames = 0);

private:
    void init();
    void mainLoop(int maxFrames);
    void shutdown();

    GLFWwindow* window_ = nullptr;
    VulkanContext ctx_;
    Renderer renderer_;
};

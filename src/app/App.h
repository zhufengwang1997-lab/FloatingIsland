#pragma once

#include "app/Benchmark.h"
#include "app/DebugUi.h"
#include "renderer/Renderer.h"
#include "rhi/VulkanContext.h"
#include "scene/Camera.h"

#include <string>

struct GLFWwindow;

struct AppOptions {
    int maxFrames = 0;           // > 0: quit after that many frames; 0: run until the window is closed
    std::string screenshotPath;  // non-empty: save the last frame there (needs maxFrames)
    bool vsync = true;
    int benchmarkFrames = 0;     // > 0: run the benchmark camera path for that many measured frames
    std::string benchmarkCsv;    // optional per-frame GPU timings of the benchmark
    float grassDensity = 1.0f;   // multiplies the number of grass tufts
    int sceneRepeat = 16;        // benchmark: draw the scene passes this many times per frame
};

// Owns the window and the main loop.
class App {
public:
    void run(const AppOptions& options);

private:
    void init(bool vsync, float grassDensity);
    void loadScene(float grassDensity);
    void mainLoop(int maxFrames, const std::string& screenshotPath, Benchmark* benchmark);
    void shutdown();

    GLFWwindow* window_ = nullptr;
    VulkanContext ctx_;
    Renderer renderer_;
    Camera camera_;
    DebugUi ui_;
};

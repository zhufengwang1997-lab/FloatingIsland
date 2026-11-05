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
    bool frustumCulling = true;  // skip chunks outside the camera view (off for A/B tests)
    bool distanceLod = true;     // thin out distant grass (off for A/B tests)
    bool cloudSea = true;        // the sea of clouds below the island
    bool showUi = true;          // debug panel visible at start
    float timeOfDay = 15.0f;     // hours, 0..24
    bool dayCycle = false;       // let the time of day run by itself
    bool grassShadows = false;   // draw grass into the shadow map (off by default; on for A/B tests)
};

// Owns the window and the main loop.
class App {
public:
    void run(const AppOptions& options);

private:
    void init(bool vsync, const AppOptions& options);
    void loadScene(const AppOptions& options);
    void mainLoop(int maxFrames, const std::string& screenshotPath, Benchmark* benchmark);
    void shutdown();

    GLFWwindow* window_ = nullptr;
    VulkanContext ctx_;
    Renderer renderer_;
    Camera camera_;
    DebugUi ui_;
};

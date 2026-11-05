#pragma once

#include "scene/Sky.h"

#include <vulkan/vulkan.h>

#include <cstdint>

class Camera;
class Renderer;
class VulkanContext;
struct GLFWwindow;

// Dear ImGui overlay: frame timings (CPU and per-pass GPU), scene stats and live render settings.
// Toggle with F1 or H.
class DebugUi {
public:
    // Call after the app has installed its own GLFW callbacks: ImGui chains to them.
    void init(GLFWwindow* window, const VulkanContext& ctx, const Renderer& renderer);
    void shutdown();

    // Builds this frame's UI and applies the settings to the renderer.
    // Call once per frame before Renderer::drawFrame, which draws the result.
    void build(Renderer& renderer, const Camera& camera, float cpuFrameMs);

    // True while ImGui is using the mouse or keyboard, so the camera should ignore them.
    bool wantsInput() const;
    void setVsync(bool enabled) { vsync_ = enabled; }
    void setTimeOfDay(float hour, bool animate) {
        hour_ = hour;
        dayCycle_ = animate;
    }

    bool visible = true;

private:
    void drawStats(const Renderer& renderer, const Camera& camera);
    void drawSettings(Renderer& renderer);

    bool initialized_ = false;

    // Time of day, which sets the sun (and by night the moon), the sky and the exposure.
    // 15:00 gives about the sun direction of the earlier steps, so benchmarks stay comparable.
    float hour_ = 15.0f;
    bool dayCycle_ = false;        // advance the time of day by itself
    float hoursPerSecond_ = 0.2f;  // with dayCycle_: one day in two minutes
    SunPath sunPath_;
    float sunIntensity_ = 3.5f;
    bool vsync_ = true;

    static constexpr int kHistory = 240;
    float cpuHistory_[kHistory]{};
    float gpuHistory_[kHistory]{};
    int historyOffset_ = 0;
};

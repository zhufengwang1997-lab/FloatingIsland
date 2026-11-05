#pragma once

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

    bool visible = true;

private:
    void drawStats(const Renderer& renderer, const Camera& camera);
    void drawSettings(Renderer& renderer);

    bool initialized_ = false;

    // Sun as angles; the renderer gets the direction vector. Defaults match Renderer's initial sun.
    float sunAzimuthDeg_ = 30.3f;    // around +Y, from +X towards +Z
    float sunElevationDeg_ = 35.7f;  // above the horizon
    float sunIntensity_ = 3.5f;
    bool vsync_ = true;

    static constexpr int kHistory = 240;
    float cpuHistory_[kHistory]{};
    float gpuHistory_[kHistory]{};
    int historyOffset_ = 0;
};

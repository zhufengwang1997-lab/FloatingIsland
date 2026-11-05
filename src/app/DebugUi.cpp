#include "app/DebugUi.h"

#include "renderer/Renderer.h"
#include "rhi/VkCheck.h"
#include "rhi/VulkanContext.h"
#include "scene/Camera.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

void checkVk(VkResult result) {
    if (result != VK_SUCCESS) std::fprintf(stderr, "ImGui Vulkan backend: %s\n", string_VkResult(result));
}

}  // namespace

void DebugUi::init(GLFWwindow* window, const VulkanContext& ctx, const Renderer& renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // don't litter the working directory with imgui.ini
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;

    ImGui_ImplGlfw_InitForVulkan(window, true);

    const Swapchain& swapchain = renderer.swapchain();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &swapchain.format;  // outlives init: owned by the renderer

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = ctx.instance;
    info.PhysicalDevice = ctx.physicalDevice;
    info.Device = ctx.device;
    info.QueueFamily = ctx.graphicsQueueFamily;
    info.Queue = ctx.graphicsQueue;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;  // backend owns its pool
    info.MinImageCount = 2;
    info.ImageCount = std::max<uint32_t>(2, static_cast<uint32_t>(swapchain.images.size()));
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = rendering;
    info.CheckVkResultFn = checkVk;
    ImGui_ImplVulkan_Init(&info);
    initialized_ = true;
}

void DebugUi::shutdown() {
    if (!initialized_) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}

bool DebugUi::wantsInput() const {
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void DebugUi::build(Renderer& renderer, const Camera& camera, float cpuFrameMs) {
    cpuHistory_[historyOffset_] = cpuFrameMs;
    gpuHistory_[historyOffset_] = renderer.profiler().milliseconds(GpuScope::Frame);
    historyOffset_ = (historyOffset_ + 1) % kHistory;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (visible) {
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({360, 0}, ImGuiCond_FirstUseEver);
        ImGui::Begin("FloatingIsland (F1 / H)");
        drawStats(renderer, camera);
        drawSettings(renderer);
        ImGui::End();
    }
    ImGui::Render();

    // Apply the settings every frame, so the renderer always matches the widgets.
    if (dayCycle_) hour_ = std::fmod(hour_ + hoursPerSecond_ * cpuFrameMs / 1000.0f, 24.0f);
    renderer.sky = evaluateSky(sunDirectionAt(hour_, sunPath_), sunIntensity_);
    renderer.sunDirection = renderer.sky.lightDirection;
    renderer.sunColor = renderer.sky.lightColor;
    renderer.setVsync(vsync_);
}

void DebugUi::drawStats(const Renderer& renderer, const Camera& camera) {
    if (!ImGui::CollapsingHeader("Timings", ImGuiTreeNodeFlags_DefaultOpen)) return;

    const GpuProfiler& gpu = renderer.profiler();
    const float cpuMs = cpuHistory_[(historyOffset_ + kHistory - 1) % kHistory];
    ImGui::Text("CPU frame   %6.2f ms  (%.0f fps)", cpuMs, cpuMs > 0.0f ? 1000.0f / cpuMs : 0.0f);
    if (gpu.supported()) {
        for (GpuScope s : {GpuScope::Frame, GpuScope::Shadow, GpuScope::Main, GpuScope::Composite, GpuScope::Ui}) {
            ImGui::Text("GPU %-12s%6.3f ms", gpuScopeName(s), gpu.milliseconds(s));
        }
        // The composite pass can include waiting for the display to release the swapchain image,
        // so shadow + main is the number to compare between optimizations.
        ImGui::Text("GPU scene      %6.3f ms  (shadow + main)",
                    gpu.milliseconds(GpuScope::Shadow) + gpu.milliseconds(GpuScope::Main));
    } else {
        ImGui::TextDisabled("GPU timestamps not supported on this queue");
    }

    // Frame time graph: CPU (frame-to-frame) and GPU (whole command buffer).
    float maxMs = 1.0f;
    for (int i = 0; i < kHistory; ++i) maxMs = std::max({maxMs, cpuHistory_[i], gpuHistory_[i]});
    const float graphTop = std::ceil(maxMs / 5.0f) * 5.0f;  // round up to 5 ms steps
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "CPU ms (0-%.0f)", graphTop);
    ImGui::PlotLines("##cpu", cpuHistory_, kHistory, historyOffset_, overlay, 0.0f, graphTop, {-1, 50});
    std::snprintf(overlay, sizeof(overlay), "GPU ms (0-%.0f)", graphTop);
    ImGui::PlotLines("##gpu", gpuHistory_, kHistory, historyOffset_, overlay, 0.0f, graphTop, {-1, 50});

    const FrameStats& stats = renderer.stats();
    const Swapchain& sc = renderer.swapchain();
    ImGui::Text("Draw calls  %u   Triangles %.2f M  (all passes)", stats.drawCalls, static_cast<double>(stats.triangles) / 1e6);
    ImGui::Text("Chunks      main %u / %u   shadow %u / %u", stats.chunksVisible, stats.chunksTotal,
                stats.shadowChunksVisible, stats.shadowChunksTotal);
    ImGui::Text("Instances   %llu drawn  (main pass)", static_cast<unsigned long long>(stats.instancesDrawn));
    ImGui::Text("Swapchain   %ux%u, %zu images, %s", sc.extent.width, sc.extent.height, sc.images.size(),
                presentModeName(sc.presentMode));
    ImGui::Text("Camera      (%.1f, %.1f, %.1f)", camera.position.x, camera.position.y, camera.position.z);
}

void DebugUi::drawSettings(Renderer& renderer) {
    if (!ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::SeparatorText("Time of day");
    const int minutes = static_cast<int>(hour_ * 60.0f) % (24 * 60);
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
    ImGui::SliderFloat("Hour", &hour_, 0.0f, 24.0f, clock);
    ImGui::Checkbox("Day cycle", &dayCycle_);
    if (dayCycle_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##speed", &hoursPerSecond_, 0.02f, 4.0f, "%.2f h/s", ImGuiSliderFlags_Logarithmic);
    }
    ImGui::SliderFloat("Latitude", &sunPath_.latitudeDeg, 0.0f, 80.0f, "%.0f deg");
    ImGui::SliderFloat("Heading", &sunPath_.headingDeg, -180.0f, 180.0f, "%.0f deg");
    ImGui::SliderFloat("Sun intensity", &sunIntensity_, 0.0f, 10.0f, "%.2f");
    const SkyState& sky = renderer.sky;
    ImGui::Text("Sun elevation %.1f deg   light: %s", glm::degrees(std::asin(sky.sunDirection.y)),
                sky.lightDirection == sky.sunDirection ? "sun" : "moon");

    ImGui::SeparatorText("Scene");
    ImGui::SliderFloat("Wind", &renderer.windStrength, 0.0f, 3.0f, "%.2f");
    ImGui::Checkbox("Frustum culling", &renderer.frustumCulling);
    ImGui::SameLine();
    ImGui::Checkbox("Freeze", &renderer.freezeCulling);
    ImGui::Checkbox("Sea of clouds", &renderer.cloudSea);
    ImGui::Checkbox("Grass distance LOD", &renderer.distanceLod);
    if (renderer.distanceLod) {
        ImGui::DragFloatRange2("LOD distance", &renderer.lodStart, &renderer.lodEnd, 0.5f, 0.0f, 300.0f, "%.0f", "%.0f");
        ImGui::SliderFloat("Far fraction", &renderer.lodMinFraction, 0.2f, 1.0f, "%.2f");
    }
    if (ImGui::BeginTable("objects", 4, ImGuiTableFlags_SizingStretchProp)) {
        for (uint32_t i = 0; i < renderer.objectCount(); ++i) {
            const std::string& name = renderer.objectName(i);
            if (name.empty()) continue;  // unnamed objects (test spheres) are not listed
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Checkbox(name.c_str(), &renderer.objectVisible(i));
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            ImGui::Checkbox("shadow", &renderer.objectCastsShadow(i));
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::Text("x%u", renderer.objectInstances(i));
            ImGui::TableNextColumn();
            ImGui::Text("%.1fk tris", static_cast<double>(renderer.objectTriangles(i)) / 1e3);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Camera");
    ImGui::SliderFloat("Exposure", &renderer.exposure, 0.1f, 4.0f, "%.2f");

    ImGui::SeparatorText("Shadow bias");
    ImGui::SliderFloat("Constant", &renderer.shadowBiasConstant, 0.0f, 8.0f, "%.2f");
    ImGui::SliderFloat("Slope", &renderer.shadowBiasSlope, 0.0f, 8.0f, "%.2f");

    ImGui::SeparatorText("Presentation");
    ImGui::Checkbox("Vsync", &vsync_);
}

#include "app/App.h"

#include "scene/IslandMesh.h"
#include "scene/Primitives.h"
#include "scene/Vegetation.h"

#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <stdexcept>

void App::run(const AppOptions& options) {
    if (options.benchmarkFrames > 0) {
        // Uncapped frame rate and no UI, so the numbers measure the scene.
        Benchmark benchmark(options.benchmarkFrames, options.benchmarkCsv);
        init(false);
        ui_.visible = false;
        mainLoop(benchmark.totalFrames(), options.screenshotPath, &benchmark);
        benchmark.report();
    } else {
        init(options.vsync);
        mainLoop(options.maxFrames, options.screenshotPath, nullptr);
    }
    shutdown();
}

void App::init(bool vsync) {
    // Hand GLFW the loader we link against, instead of letting it search for libvulkan itself.
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");
    if (!glfwVulkanSupported()) throw std::runtime_error("GLFW cannot use Vulkan on this system");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan, not OpenGL
    window_ = glfwCreateWindow(1280, 720, "FloatingIsland", nullptr, nullptr);
    if (!window_) throw std::runtime_error("glfwCreateWindow failed");

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int, int) {
        static_cast<App*>(glfwGetWindowUserPointer(w))->renderer_.onFramebufferResized();
    });
    glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int) {
        if (action != GLFW_PRESS) return;
        if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(w, GLFW_TRUE);
        if (key == GLFW_KEY_F1) {
            DebugUi& ui = static_cast<App*>(glfwGetWindowUserPointer(w))->ui_;
            ui.visible = !ui.visible;
        }
    });

    ctx_.init(window_);
    renderer_.setVsync(vsync);  // before init, so the first swapchain already uses it
    renderer_.init(ctx_, window_);
    std::printf("Present mode: %s\n", presentModeName(renderer_.swapchain().presentMode));
    ui_.init(window_, ctx_, renderer_);
    ui_.setVsync(vsync);  // after our callbacks, which ImGui chains to
    loadScene();
}

void App::loadScene() {
    const MeshData island = generateIsland();
    std::printf("Island: %zu vertices, %zu triangles\n", island.vertices.size(), island.indices.size() / 3);
    renderer_.addObject(island, glm::mat4(1.0f), "Island");

    // Vegetation: one instanced draw call per kind.
    for (const VegetationKind& kind : generateVegetation()) {
        std::printf("%s: %zu instances x %zu triangles\n", kind.name.c_str(), kind.instances.size(),
                    kind.mesh.indices.size() / 3);
        renderer_.addInstanced(kind.mesh, kind.instances, kind.twoSided, kind.name);
    }

    // Material test spheres floating above the island: roughness 0.1 -> 0.9 from left to right,
    // red plastic on the lower row, gold on the upper row.
    const glm::vec3 plastic{0.80f, 0.12f, 0.10f};
    const glm::vec3 gold{1.00f, 0.78f, 0.34f};
    for (int i = 0; i < 5; ++i) {
        const float roughness = 0.1f + 0.2f * static_cast<float>(i);
        const float x = -16.0f + 8.0f * static_cast<float>(i);
        renderer_.addObject(generateSphere(2.5f, 48, 24, plastic, roughness, 0.0f),
                            glm::translate(glm::mat4(1.0f), {x, 20.0f, 20.0f}));
        renderer_.addObject(generateSphere(2.5f, 48, 24, gold, roughness, 1.0f),
                            glm::translate(glm::mat4(1.0f), {x, 27.0f, 20.0f}));
    }

    // Everything above fits in this sphere, so it all casts and receives shadows.
    renderer_.setShadowBounds({0.0f, -5.0f, 0.0f}, 52.0f);
    std::fflush(stdout);
}

void App::mainLoop(int maxFrames, const std::string& screenshotPath, Benchmark* benchmark) {
    double lastTitleUpdate = glfwGetTime();
    double lastFrame = lastTitleUpdate;
    int frames = 0;
    int totalFrames = 0;
    // CPU frame time averaged after a warm-up, for automated runs.
    constexpr int kWarmupFrames = 60;
    double measureStart = 0.0;
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        const double frameStart = glfwGetTime();
        const float dt = static_cast<float>(frameStart - lastFrame);
        lastFrame = frameStart;
        if (benchmark) {
            benchmark->pose(totalFrames, camera_);
            renderer_.timeOverride = Benchmark::animationTime(totalFrames);
        } else if (!ui_.wantsInput()) {
            camera_.update(window_, dt);
        }
        ui_.build(renderer_, camera_, dt * 1000.0f);
        if (!screenshotPath.empty() && totalFrames + 1 == maxFrames) renderer_.requestCapture(screenshotPath);
        renderer_.drawFrame(camera_);
        if (benchmark) benchmark->record(totalFrames, dt * 1000.0f, renderer_.profiler());
        if (++totalFrames == kWarmupFrames) measureStart = glfwGetTime();
        if (maxFrames > 0 && totalFrames >= maxFrames) break;

        // Average frame time in the title bar, refreshed twice a second.
        ++frames;
        const double now = glfwGetTime();
        if (now - lastTitleUpdate >= 0.5) {
            const double elapsed = now - lastTitleUpdate;
            char title[128];
            std::snprintf(title, sizeof(title), "FloatingIsland | %.2f ms (%.0f fps)",
                          1000.0 * elapsed / frames, frames / elapsed);
            glfwSetWindowTitle(window_, title);
            lastTitleUpdate = now;
            frames = 0;
        }
    }

    // Averages for automated runs (--frames), so measurements can be compared between commits.
    if (maxFrames > kWarmupFrames && !benchmark) {
        const double ms = 1000.0 * (glfwGetTime() - measureStart) / (totalFrames - kWarmupFrames);
        std::printf("CPU frame (avg after warm-up): %.2f ms (%.0f fps)\n", ms, 1000.0 / ms);
    }
    const GpuProfiler& gpu = renderer_.profiler();
    if (maxFrames > 0 && gpu.supported() && !benchmark) {
        std::printf("GPU ms (smoothed): frame %.3f | shadow %.3f | main %.3f | composite %.3f | ui %.3f\n",
                    gpu.milliseconds(GpuScope::Frame), gpu.milliseconds(GpuScope::Shadow),
                    gpu.milliseconds(GpuScope::Main), gpu.milliseconds(GpuScope::Composite),
                    gpu.milliseconds(GpuScope::Ui));
        std::printf("GPU scene (shadow + main): %.3f ms\n",
                    gpu.milliseconds(GpuScope::Shadow) + gpu.milliseconds(GpuScope::Main));
    }
}

void App::shutdown() {
    vkDeviceWaitIdle(ctx_.device);
    ui_.shutdown();
    renderer_.shutdown();
    ctx_.shutdown();
    glfwDestroyWindow(window_);
    glfwTerminate();
}

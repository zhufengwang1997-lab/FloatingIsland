#include "app/App.h"

#include "scene/IslandMesh.h"

#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <stdexcept>

void App::run(int maxFrames) {
    init();
    mainLoop(maxFrames);
    shutdown();
}

void App::init() {
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
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) glfwSetWindowShouldClose(w, GLFW_TRUE);
    });

    ctx_.init(window_);
    renderer_.init(ctx_, window_);
    renderer_.loadMesh(generateIsland());
}

void App::mainLoop(int maxFrames) {
    double lastTitleUpdate = glfwGetTime();
    double lastFrame = lastTitleUpdate;
    int frames = 0;
    int totalFrames = 0;
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        const double frameStart = glfwGetTime();
        camera_.update(window_, static_cast<float>(frameStart - lastFrame));
        lastFrame = frameStart;
        renderer_.drawFrame(camera_);
        if (maxFrames > 0 && ++totalFrames >= maxFrames) break;

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
}

void App::shutdown() {
    vkDeviceWaitIdle(ctx_.device);
    renderer_.shutdown();
    ctx_.shutdown();
    glfwDestroyWindow(window_);
    glfwTerminate();
}

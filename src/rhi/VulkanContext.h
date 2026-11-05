#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

struct GLFWwindow;

// Owns the Vulkan objects that live for the whole program:
// instance, debug messenger, window surface, physical device, logical device and queue.
class VulkanContext {
public:
    void init(GLFWwindow* window);
    void shutdown();

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;   // also used for present
    uint32_t graphicsQueueFamily = 0;
    bool validationEnabled = false;

private:
    void createInstance();
    void createDebugMessenger();
    void pickPhysicalDevice();
    void createDevice();

    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
};

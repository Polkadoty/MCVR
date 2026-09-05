#pragma once

#include "common/shared.hpp"
#include "core/vulkan/image.hpp"
#include <cstdint>
#include <memory>
#include <string>

struct FrameworkContext;

// All entry points run on the render/present thread. Java writes only its own
// option fields; the framework calls configure after observing them.
class FrameGenManager {
public:
    struct FrameInput {
        std::shared_ptr<FrameworkContext> context;
        std::shared_ptr<vk::DeviceLocalImage> linearDepth;    // positive view Z, R16_SFLOAT
        std::shared_ptr<vk::DeviceLocalImage> motionVectors; // current->previous pixels, RG16_SFLOAT
        std::shared_ptr<vk::DeviceLocalImage> hudlessColor;  // final display size/color space, before UI
        vk::Data::WorldUBO world{};                        // snapshot, never a retained mapped pointer
        bool reset = false;                              // teleport/world/pipeline reset
    };

    static bool init(); // after StreamlineContext::onDeviceCreated; Off by default
    // enabled is user intent AND runtime gate (world, focus, no menus/pause).
    // Returns whether framework must recreate before the next enabled frame.
    static bool configure(bool enabled, uint32_t generatedFrames = 1);
    static bool needsSwapchainRecreate();
    // Drain GPU/SL inputs before destroying a swapchain. Returns false on failure.
    static bool beforeSwapchainRecreate();
    // After OLD swapchain is destroyed, BEFORE creating the new one. Changes
    // plugin load state only with no swapchain owned by the old hooks remaining.
    static bool prepareNewSwapchain();
    static bool afterSwapchainRecreate();
    // Called while context->worldCommandBuffer is recording, after all world
    // output writes. Records linear->device-depth conversion and sets SL tags.
    static bool tagFrame(const FrameInput &input);
    // MUST be called on present thread immediately after vkQueuePresentKHR.
    static bool captureInputCompletion(uint32_t frameSlot, const char *source = "present");
    // MUST succeed before ANY queue writes/reallocates that slot's tagged images.
    // Holding shared_ptrs protects destruction only, never subsequent GPU writes.
    static bool waitForInputCompletion(uint32_t frameSlot, VkDevice device, uint64_t timeoutNs,
                                       uint64_t *elapsedUs = nullptr);
    static bool waitForAllInputCompletions(VkDevice device, uint64_t timeoutNs);
    // Slow explicit recovery if capture had no trustworthy completion value.
    // Requires no new submissions; waits device idle before releasing references.
    static bool drainAfterDeviceIdle(VkDevice device);
    // Called after draining, before device/Streamline destruction.
    static bool shutdown();
    static bool isActive();
    static bool isAvailable();
    static uint32_t maxFramesToGenerate();
    static std::string latencyDiagnostics();
};

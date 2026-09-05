#pragma once

#include "common/shared.hpp"
#include "common/singleton.hpp"
#include "core/all_extern.hpp"
#include "core/render/modules/world/dlss/dlss_wrapper.hpp"
#include "core/render/pipeline.hpp"
#include "core/vulkan/all_core_vulkan.hpp"

#include <chrono>
#include <map>
#include <mutex>

class Framework;
class UIModule;
struct UIModuleContext;

class FrameResourceRetainer : public SharedObject<FrameResourceRetainer> {
  public:
    explicit FrameResourceRetainer(uint32_t imageCount);

    template <typename T>
    void retain(std::shared_ptr<T> resource);

    // Caller must drain every GPU queue and FG input completion before resetting.
    // No previously submitted frame may still use any retained resource.
    void resetAfterDeviceIdle(uint32_t imageCount);
    // Caller must wait this frame slot's fence before releasing its retired resources.
    void beginFrame(uint32_t frameIndex);

  private:
    std::vector<std::vector<std::shared_ptr<void>>> retainedResourcesByFrame_;
    uint32_t currentFrameIndex_ = 0;
    std::recursive_mutex mtx_;
};

struct FrameworkContext : public SharedObject<FrameworkContext> {
    std::weak_ptr<Framework> framework;

    uint32_t frameIndex;

    std::shared_ptr<vk::Instance> instance;
    std::shared_ptr<vk::Window> window;
    std::shared_ptr<vk::PhysicalDevice> physicalDevice;
    std::shared_ptr<vk::Device> device;
    std::shared_ptr<vk::VMA> vma;
    std::shared_ptr<vk::Swapchain> swapchain;
    std::shared_ptr<vk::SwapchainImage> swapchainImage;
    std::shared_ptr<vk::CommandPool> commandPool;
    std::shared_ptr<vk::Semaphore> imageAcquiredSemaphore = nullptr;
    std::shared_ptr<vk::Semaphore> commandProcessedSemaphore;
    std::shared_ptr<vk::Fence> commandFinishedFence;

    std::shared_ptr<vk::CommandBuffer> uploadCommandBuffer;
    std::shared_ptr<vk::CommandBuffer> overlayCommandBuffer;
    std::shared_ptr<vk::CommandBuffer> worldCommandBuffer;
    std::shared_ptr<vk::CommandBuffer> fuseCommandBuffer;

    FrameworkContext(std::shared_ptr<Framework> framework, uint32_t frame_index);
    ~FrameworkContext();

    void fuseFinal();
};

class Framework : public SharedObject<Framework> {
    friend FrameworkContext;
    friend FrameResourceRetainer;

  public:
    Framework();
    ~Framework();

    void init(GLFWwindow *window);
    void acquireContext();
    void submitCommand();
    void present();
    void recreate();
    void waitDeviceIdle();
    void waitRenderQueueIdle();
    void waitBackendQueueIdle();
    void close();
    bool isRunning();

    void takeScreenshot(bool withUI, int width, int height, int channel, void *dstPointer);

    std::recursive_mutex &recreateMtx();

    std::shared_ptr<vk::Instance> instance();
    std::shared_ptr<vk::Window> window();
    std::shared_ptr<vk::PhysicalDevice> physicalDevice();
    std::shared_ptr<vk::Device> device();
    std::shared_ptr<vk::VMA> vma();
    std::shared_ptr<vk::Swapchain> swapchain();
    std::shared_ptr<vk::CommandPool> mainCommandPool();
    std::shared_ptr<vk::CommandPool> asyncCommandPool();

    std::shared_ptr<vk::CommandBuffer> worldAsyncCommandBuffer();

    std::vector<std::shared_ptr<vk::Semaphore>> &commandProcessedSemaphores();
    std::vector<std::shared_ptr<vk::Fence>> &commandFinishedFences();
    std::vector<std::shared_ptr<FrameworkContext>> &contexts();
    std::shared_ptr<FrameworkContext> safeAcquireCurrentContext();

    std::shared_ptr<Pipeline> pipeline();

    FrameResourceRetainer &frameResourceRetainer();

  private:
    std::shared_ptr<vk::Semaphore> acquireSemaphore();
    void recycleSemaphore(std::shared_ptr<vk::Semaphore> semaphore);
    uint32_t effectiveFrameRateLimit() const;
    void limitFrameRate();

  private:
    std::shared_ptr<vk::Instance> instance_;
    std::shared_ptr<vk::Window> window_;
    std::shared_ptr<vk::PhysicalDevice> physicalDevice_;
    std::shared_ptr<vk::Device> device_;
    std::shared_ptr<vk::VMA> vma_;
    std::shared_ptr<vk::Swapchain> swapchain_;
    std::shared_ptr<vk::CommandPool> mainCommandPool_;
    std::shared_ptr<vk::CommandPool> asyncCommandPool_;

    std::vector<std::shared_ptr<vk::CommandBuffer>> uploadCommandBuffers_;
    std::vector<std::shared_ptr<vk::CommandBuffer>> overlayCommandBuffers_;
    std::vector<std::shared_ptr<vk::CommandBuffer>> worldCommandBuffers_;
    std::vector<std::shared_ptr<vk::CommandBuffer>> fuseCommandBuffers_;
    std::shared_ptr<vk::CommandBuffer> worldAsyncCommandBuffer_;

    std::shared_ptr<Pipeline> pipeline_;

    std::vector<std::shared_ptr<vk::Semaphore>> commandProcessedSemaphores_;
    std::vector<std::shared_ptr<vk::Fence>> commandFinishedFences_;

    std::vector<std::shared_ptr<FrameworkContext>> contexts_;

    std::shared_ptr<FrameworkContext> currentContext_ = nullptr;
    uint32_t currentContextIndex_ = 0;
    std::queue<uint32_t> indexHistory_;

    std::queue<std::shared_ptr<vk::Semaphore>> recycledImageAcquiredSemaphores_;
    std::recursive_mutex recreateMtx_;

    bool running_ = true;
    std::chrono::steady_clock::time_point frameLimitAnchor_{};
    uint32_t frameLimitFps_ = 0;
    bool reflexPacedFrame_ = false;

    std::shared_ptr<FrameResourceRetainer> frameResourceRetainer_;
};

template <typename T>
void FrameResourceRetainer::retain(std::shared_ptr<T> resource) {
    std::unique_lock<std::recursive_mutex> lck(mtx_);

    if (resource != nullptr) {
        retainedResourcesByFrame_.at(currentFrameIndex_).push_back(resource);

#ifdef DEBUG
        if constexpr (std::is_same_v<T, vk::DeviceLocalImage>) {
            std::cout << "Frame resource retainer enqueued image (" << resource->debugName
                      << ") in frame: " << currentFrameIndex_
                      << std::endl;
        }
#endif
    }
}

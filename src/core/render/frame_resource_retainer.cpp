#include "core/render/render_framework.hpp"

#include <stdexcept>

FrameResourceRetainer::FrameResourceRetainer(uint32_t imageCount) {
    resetAfterDeviceIdle(imageCount);
}

void FrameResourceRetainer::resetAfterDeviceIdle(uint32_t imageCount) {
    if (imageCount == 0) {
        throw std::invalid_argument("Frame resource retainer requires at least one swapchain image");
    }
    // Allocate before touching existing ownership so allocation failure preserves
    // every retained resource and the current slot. GPU completion is a caller
    // precondition; this CPU container cannot determine when resources are safe.
    decltype(retainedResourcesByFrame_) emptySlots(imageCount);
    std::unique_lock<std::recursive_mutex> lock(mtx_);
    retainedResourcesByFrame_.swap(emptySlots);
    currentFrameIndex_ = 0;
}

void FrameResourceRetainer::beginFrame(uint32_t frameIndex) {
    std::unique_lock<std::recursive_mutex> lock(mtx_);
    // Reject an invalid index before modifying the active slot or releasing refs.
    auto &resources = retainedResourcesByFrame_.at(frameIndex);
    currentFrameIndex_ = frameIndex;
    resources.clear();
}

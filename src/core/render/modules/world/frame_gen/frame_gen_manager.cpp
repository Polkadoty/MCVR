// Frame-generation lifecycle adapted from PEQHUB/MCVR 8e1a148 (GPL-3.0).
#include "core/render/modules/world/frame_gen/frame_gen_manager.hpp"
#include "core/render/modules/world/frame_gen/frame_gen_camera.hpp"
#include "core/render/streamline_context.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {
enum class Completion { None, AwaitingPresent, Timeline, Unknown };
struct Slot {
    Completion completion{Completion::None};
    VkSemaphore semaphore{};
    uint64_t value{};
    // Keep every input and the conversion descriptors/pipeline alive. These
    // references do NOT authorize writes; waitForInputCompletion does that.
    std::array<std::shared_ptr<vk::DeviceLocalImage>, 3> inputs;
    std::shared_ptr<vk::DeviceLocalImage> deviceDepth;
    std::shared_ptr<vk::DescriptorTable> descriptors;
    VkImage boundLinearDepth{};
};
std::vector<Slot> slots;
std::shared_ptr<vk::ComputePipeline> depthPipeline;
frame_gen::CameraHistory cameraHistory;
bool initialized{}, supported{}, requested{}, active{}, needRecreate{}, recreating{}, faulted{}, confirmed{};
uint32_t maxFrames{}, frames{1}, minimumDimension{};
uint64_t captured{}, waited{}, lastWaitUs{};
uint32_t lastStatus{}, lastPresented{};
std::string diagnostic{"Off by default"};

void note(const std::string &message) {
    diagnostic = message;
    std::ofstream(Renderer::folderPath / "frame-generation-status.log", std::ios::app)
        << message << std::endl;
}
bool off() {
    if (!active) return true;
    if (!StreamlineContext::setDlssGOptions(sl::DLSSGMode::eOff)) return false;
    active = false;
    confirmed = false;
    cameraHistory.reset();
    return true;
}
bool fail(const std::string &message) {
    note("Frame generation disabled: " + message);
    faulted = true;
    confirmed = false;
    requested = false;
    needRecreate = true;
    // If Off itself fails, keep active=true so teardown cannot pretend success.
    off();
    return false;
}
bool pending() {
    for (const auto &slot : slots) if (slot.completion != Completion::None) return true;
    return false;
}
void clearCompleted(Slot &slot) {
    slot.completion = Completion::None;
    slot.semaphore = VK_NULL_HANDLE;
    slot.value = 0;
    slot.inputs = {};
}

struct DepthParameters { float p22, p32, p23, p33, farDepth; };
void createDepthResources(Slot &slot, const FrameGenManager::FrameInput &input) {
    auto context = input.context;
    auto source = input.linearDepth;
    const auto width = source->width(), height = source->height();
    if (!slot.deviceDepth || slot.deviceDepth->width() != width || slot.deviceDepth->height() != height) {
        slot.deviceDepth = vk::DeviceLocalImage::create(context->device, context->vma, width, height, 1,
            VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        slot.descriptors.reset();
    }
    if (!slot.descriptors || slot.boundLinearDepth != source->vkImage()) {
        slot.descriptors = vk::DescriptorTableBuilder{}
            .beginDescriptorLayoutSet().beginDescriptorLayoutSetBinding()
            .defineDescriptorLayoutSetBinding({.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT})
            .defineDescriptorLayoutSetBinding({.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT})
            .endDescriptorLayoutSetBinding().endDescriptorLayoutSet()
            .definePushConstant({.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(DepthParameters)})
            .build(context->device);
        slot.descriptors->bindImage(source, VK_IMAGE_LAYOUT_GENERAL, 0, 0);
        slot.descriptors->bindImage(slot.deviceDepth, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        slot.boundLinearDepth = source->vkImage();
    }
    if (!depthPipeline) {
        auto shader = vk::Shader::create(context->device,
            (Renderer::folderPath / "shaders/world/frame_gen/linear_to_device_depth_comp.spv").string());
        depthPipeline = vk::ComputePipelineBuilder{}.defineShader(shader)
            .definePipelineLayout(slot.descriptors).build(context->device);
    }
}

void generalBarrier(std::shared_ptr<vk::CommandBuffer> command,
                    std::shared_ptr<vk::DeviceLocalImage> image, VkAccessFlags2 destinationAccess) {
    const bool fresh = image->imageLayout() == VK_IMAGE_LAYOUT_UNDEFINED;
    command->barriersBufferImage({}, {{
        .srcStageMask = fresh ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = fresh ? VK_ACCESS_2_NONE : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = destinationAccess,
        .oldLayout = image->imageLayout(), .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = vk::wholeColorSubresourceRange
    }});
    image->imageLayout() = VK_IMAGE_LAYOUT_GENERAL;
}
void convertDepth(Slot &slot, const FrameGenManager::FrameInput &input, bool reversed) {
    auto command = input.context->worldCommandBuffer;
    generalBarrier(command, input.linearDepth, VK_ACCESS_2_SHADER_READ_BIT);
    generalBarrier(command, slot.deviceDepth, VK_ACCESS_2_SHADER_WRITE_BIT);
    command->bindDescriptorTable(slot.descriptors, VK_PIPELINE_BIND_POINT_COMPUTE)->bindComputePipeline(depthPipeline);
    const auto &projection = input.world.cameraProjMat;
    DepthParameters parameters{projection[2][2], projection[3][2], projection[2][3], projection[3][3], reversed ? 0.0f : 1.0f};
    vkCmdPushConstants(command->vkCommandBuffer(), slot.descriptors->vkPipelineLayout(),
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
    vkCmdDispatch(command->vkCommandBuffer(), (slot.deviceDepth->width() + 15) / 16,
        (slot.deviceDepth->height() + 15) / 16, 1);
    generalBarrier(command, slot.deviceDepth, VK_ACCESS_2_MEMORY_READ_BIT);
    generalBarrier(command, input.motionVectors, VK_ACCESS_2_MEMORY_READ_BIT);
}
sl::Resource resource(const std::shared_ptr<vk::DeviceLocalImage> &image) {
    sl::Resource result{};
    result.type = sl::ResourceType::eTex2d;
    result.native = reinterpret_cast<void *>(image->vkImage());
    result.memory = reinterpret_cast<void *>(image->vkDeviceMemory());
    result.view = reinterpret_cast<void *>(image->vkImageView());
    result.state = image->imageLayout();
    result.width = image->width(); result.height = image->height();
    result.nativeFormat = image->vkFormat();
    result.mipLevels = 1; result.arrayLayers = image->layer();
    result.flags = 0; result.usage = image->usageFlags();
    return result;
}
} // namespace

bool FrameGenManager::init() {
    if (initialized) return supported;
    initialized = true;
    if (!StreamlineContext::isDlssGSupported()) return false;
    if (!StreamlineContext::getDlssGCapabilities(maxFrames, minimumDimension)) return false;
    supported = maxFrames > 0 && StreamlineContext::isReflexAvailable();
    note(supported ? "DLSS-G available; Off by default; max generated frames=" + std::to_string(maxFrames)
                   : "DLSS-G unavailable; ordinary rendering retained");
    return supported;
}
bool FrameGenManager::configure(bool enabled, uint32_t generatedFrames) {
    bool want = enabled && supported && !faulted;
    uint32_t count = std::clamp(generatedFrames, 1u, std::max(1u, maxFrames));
    if (want != requested || (want && count != frames)) {
        requested = want; frames = count; needRecreate = true;
        if (!off()) return fail("could not turn DLSS-G Off before transition");
    }
    return needRecreate;
}
bool FrameGenManager::needsSwapchainRecreate() { return needRecreate; }
bool FrameGenManager::beforeSwapchainRecreate() {
    recreating = true;
    if (!off()) return fail("DLSS-G refused Off before swapchain teardown");
    if (pending()) return fail("attempted teardown before input completion");
    if (!StreamlineContext::clearResourceTags()) return fail("could not clear old resource tags");
    cameraHistory.reset();
    slots.clear(); depthPipeline.reset(); // caller has already waited ordinary device work idle
    return true;
}
bool FrameGenManager::prepareNewSwapchain() {
    if (pending() || active) return fail("plugin transition attempted with live frame inputs");
    if (StreamlineContext::isAvailable() && StreamlineContext::isDlssGLoaded() != requested) {
        if (!StreamlineContext::setFeatureLoaded(sl::kFeatureDLSS_G, requested)) return fail("plugin load/unload failed");
        if (requested && !StreamlineContext::setDlssGOptions(sl::DLSSGMode::eOff)) return fail("fresh plugin Off failed");
    }
    return true;
}
bool FrameGenManager::afterSwapchainRecreate() {
    recreating = false;
    needRecreate = false;
    if (!requested || !supported || faulted) return true;
    // Reflex is mandatory while FG is On, even when its separate user toggle is Off.
    if (!StreamlineContext::setReflexOptions(sl::ReflexMode::eLowLatency)
        || !StreamlineContext::setDlssGOptions(sl::DLSSGMode::eOn, frames)) return fail("activation failed");
    active = true;
    confirmed = false;
    note("DLSS-G activation requested after swapchain recreation; awaiting successful generated presents");
    return true;
}
bool FrameGenManager::tagFrame(const FrameInput &input) {
    if (!active || recreating || needRecreate) return false;
    if (!input.context || !input.context->worldCommandBuffer || !input.linearDepth || !input.motionVectors
        || !input.hudlessColor || !StreamlineContext::getCurrentFrameToken()) return fail("incomplete frame inputs");
    auto depth = input.linearDepth, motion = input.motionVectors, color = input.hudlessColor;
    const auto width = depth->width(), height = depth->height();
    if (!width || !height || width != motion->width() || height != motion->height()
        || depth->vkFormat() != VK_FORMAT_R16_SFLOAT || motion->vkFormat() != VK_FORMAT_R16G16_SFLOAT
        || color->width() != input.context->swapchainImage->width()
        || color->height() != input.context->swapchainImage->height()
        || std::min(color->width(), color->height()) < minimumDimension)
        return fail("unsupported depth/MV format or mismatched input/display dimensions");
    const auto frameSlot = input.context->frameIndex;
    if (frameSlot >= slots.size()) slots.resize(static_cast<size_t>(frameSlot) + 1);
    auto &slot = slots[frameSlot];
    if (slot.completion != Completion::None) return fail("input slot reused before completion wait");
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i == frameSlot || slots[i].completion == Completion::None) continue;
        for (const auto &retained : slots[i].inputs)
            if (retained == depth || retained == motion || retained == color) return fail("tagged inputs alias another pending frame slot");
    }
    sl::Constants constants{};
    if (!cameraHistory.fill(input.world, width, height, StreamlineContext::getFrameIndex(), input.reset, constants))
        return fail("invalid camera/projection constants");
    try {
        createDepthResources(slot, input);
        slot.inputs = {depth, motion, color};
        convertDepth(slot, input, constants.depthInverted == sl::Boolean::eTrue);
        sl::Resource resources[] = {resource(slot.deviceDepth), resource(motion), resource(color)};
        sl::Extent extents[] = {{0,0,width,height}, {0,0,width,height}, {0,0,color->width(),color->height()}};
        sl::ResourceTag tags[] = {
            {&resources[0], sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extents[0]},
            {&resources[1], sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extents[1]},
            {&resources[2], sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extents[2]}
        };
        if (!StreamlineContext::setConstants(constants)) return fail("common constants rejected");
        // Mark unknown before crossing the API: even a failing tagging call may
        // have consumed some tags. Do not release/reuse these inputs on failure.
        slot.completion = Completion::Unknown;
        if (!StreamlineContext::tagResources(tags, 3, reinterpret_cast<void *>(input.context->worldCommandBuffer->vkCommandBuffer())))
            return fail("resource tagging rejected");
        slot.completion = Completion::AwaitingPresent;
        return true;
    } catch (const std::exception &e) { return fail(e.what()); }
}
bool FrameGenManager::captureInputCompletion(uint32_t frameSlot, const char *) {
    if (frameSlot >= slots.size() || slots[frameSlot].completion == Completion::None) return true;
    auto &slot = slots[frameSlot];
    if (slot.completion != Completion::AwaitingPresent) return fail("invalid present/capture ordering");
    slot.completion = Completion::Unknown;
    sl::DLSSGState state{};
    if (!StreamlineContext::getDlssGState(state))
        return fail("DLSS-G state query failed; device-idle recovery required. " + StreamlineContext::lastError());
    // Capture the real result even when feature creation produced no timeline.
    // Unknown inputs remain retained until explicit device-idle recovery.
    lastStatus = static_cast<uint32_t>(state.status);
    lastPresented = state.numFramesActuallyPresented;
    if (!state.inputsProcessingCompletionFence || !state.lastPresentInputsProcessingCompletionFenceValue)
        return fail("no trustworthy DLSS-G input-completion timeline; status=" + std::to_string(lastStatus)
            + "; actuallyPresented=" + std::to_string(lastPresented)
            + "; device-idle recovery required. " + StreamlineContext::lastError());
    slot.semaphore = reinterpret_cast<VkSemaphore>(state.inputsProcessingCompletionFence);
    slot.value = state.lastPresentInputsProcessingCompletionFenceValue;
    slot.completion = Completion::Timeline;
    ++captured;
    lastStatus = static_cast<uint32_t>(state.status);
    lastPresented = state.numFramesActuallyPresented;
    if (lastStatus != 0) return fail("DLSS-G runtime status=" + std::to_string(lastStatus));
    if (!confirmed && lastPresented > 1) {
        confirmed = true;
        note("DLSS-G generated presents confirmed; actuallyPresented=" + std::to_string(lastPresented));
    }
    if (captured == 1 || captured % 300 == 0) {
        std::ofstream(Renderer::folderPath / "frame-generation-status.log", std::ios::app)
            << latencyDiagnostics() << std::endl;
    }
    return true;
}
bool FrameGenManager::waitForInputCompletion(uint32_t frameSlot, VkDevice device, uint64_t timeoutNs, uint64_t *elapsedUs) {
    if (elapsedUs) *elapsedUs = 0;
    if (frameSlot >= slots.size() || slots[frameSlot].completion == Completion::None) return true;
    auto &slot = slots[frameSlot];
    if (slot.completion != Completion::Timeline || !device) return false;
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1; wait.pSemaphores = &slot.semaphore; wait.pValues = &slot.value;
    auto start = std::chrono::steady_clock::now();
    VkResult result = vkWaitSemaphores(device, &wait, timeoutNs);
    lastWaitUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    if (elapsedUs) *elapsedUs = lastWaitUs;
    if (result != VK_SUCCESS) { note("Input completion wait failed: " + std::to_string(result)); return false; }
    ++waited;
    clearCompleted(slot);
    return true;
}
bool FrameGenManager::waitForAllInputCompletions(VkDevice device, uint64_t timeoutNs) {
    for (uint32_t i = 0; i < slots.size(); ++i) if (!waitForInputCompletion(i, device, timeoutNs)) return false;
    return true;
}
bool FrameGenManager::drainAfterDeviceIdle(VkDevice device) {
    if (!off() || !device || vkDeviceWaitIdle(device) != VK_SUCCESS) return false;
    if (!StreamlineContext::clearResourceTags()) return false;
    for (auto &slot : slots) clearCompleted(slot);
    cameraHistory.reset();
    return true;
}
bool FrameGenManager::shutdown() {
    if (!off() || pending() || !StreamlineContext::clearResourceTags()) return false;
    slots.clear(); depthPipeline.reset(); cameraHistory.reset();
    initialized = supported = requested = active = needRecreate = recreating = faulted = confirmed = false;
    return true;
}
bool FrameGenManager::isActive() { return active; }
bool FrameGenManager::isAvailable() { return supported && !faulted; }
bool FrameGenManager::hasFailed() { return faulted; }
std::string FrameGenManager::statusText() { return diagnostic; }
uint32_t FrameGenManager::maxFramesToGenerate() { return maxFrames; }
std::string FrameGenManager::latencyDiagnostics() {
    std::ostringstream out;
    out << "supported=" << supported << "; active=" << active << "; pending=" << pending()
        << "; confirmed=" << confirmed << "; status=" << lastStatus << "; actuallyPresented=" << lastPresented
        << "; captures=" << captured << "; waits=" << waited << "; lastWaitUs=" << lastWaitUs
        << "; " << diagnostic;
    return out.str();
}

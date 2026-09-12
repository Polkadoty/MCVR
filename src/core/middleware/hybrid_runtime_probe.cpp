// SPDX-License-Identifier: GPL-3.0-only
// Standalone Windows OpenGL/Vulkan external-memory correctness probe.
// No Minecraft changes, no visible window, and no driver installation.
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <GL/gl.h>
#include "core/render/renderer.hpp"
#include "core/render/render_framework.hpp"
#include <jni.h>
#include <array>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <cstdlib>

static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static void vkcheck(VkResult result) {
    require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result));
}
static void glcheck(const char* stage) {
    GLenum error = glGetError();
    require(error == GL_NO_ERROR, std::string(stage) + " GL error " + std::to_string(error));
}
template<class T> T glproc(const char* name) {
    auto p = wglGetProcAddress(name);
    require(p && reinterpret_cast<intptr_t>(p) != -1 && reinterpret_cast<uintptr_t>(p) > 3,
            std::string("Missing GL function: ") + name);
    return reinterpret_cast<T>(p);
}
#define GLPROC(name, result, ...) auto name = glproc<result(APIENTRY*)(__VA_ARGS__)>(#name)
static void APIENTRY glDebug(GLenum, GLenum, GLuint, GLenum, GLsizei, const char* message, const void*) {
    std::cerr << "GL diagnostic: " << message << '\n';
}
static size_t validationErrors = 0;
static VKAPI_ATTR VkBool32 VKAPI_CALL vkDebug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::cerr << "Vulkan diagnostic: " << data->pMessage << '\n';
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    return VK_FALSE;
}

static int runtimeProbe(int argc, char** argv) { try {
    bool validate = false, debugGL = false;
    uint32_t width = 64, height = 32, frames = 32;
    std::string formatName = "rgba8";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--validate") validate = true;
        else if (arg == "--gl-debug") debugGL = true;
        else if (arg == "--no-gl-debug") debugGL = false;
        else if (arg == "--format" && i + 1 < argc) formatName = argv[++i];
        else if (arg == "--width" && i + 1 < argc) width = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--height" && i + 1 < argc) height = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--frames" && i + 1 < argc) frames = static_cast<uint32_t>(std::stoul(argv[++i]));
        else throw std::runtime_error("Unknown or incomplete option: " + arg);
    }
    require(width >= 4 && width <= 4096 && height >= 2 && height <= 4096 && frames > 0 && frames <= 512, "Test dimensions/count out of range");
    require(formatName == "rgba8" || formatName == "rgba16f" || formatName == "depth32f", "Unsupported test format");
    const bool depth = formatName == "depth32f", hdr = formatName == "rgba16f";
    const VkFormat testFormat = depth ? VK_FORMAT_D32_SFLOAT : hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    const GLenum glFormat = depth ? 0x8CAC : hdr ? 0x881A : GL_RGBA8;
    const auto aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const uint32_t pixelBytes = hdr ? 8 : 4;
    std::cout << std::unitbuf;
    require(wglGetCurrentContext() != nullptr, "No current GL context on game render thread");
    using DebugCallback = void(APIENTRY*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const char*, const void*);
    GLPROC(glDebugMessageCallback, void, DebugCallback, const void*);
    if (debugGL) { glEnable(0x92E0); glEnable(0x8242); glDebugMessageCallback(glDebug, nullptr); }
    std::cout << "Test format=" << formatName << " width=" << width << " height=" << height << " frames=" << frames << " GLDebug=" << debugGL << '\n';
    std::cout << "OpenGL vendor=" << glGetString(GL_VENDOR) << "\nOpenGL renderer="
              << glGetString(GL_RENDERER) << "\nOpenGL version=" << glGetString(GL_VERSION) << '\n';
    GLPROC(glGetStringi, const GLubyte*, GLenum, GLuint);
    GLint extCount = 0; glGetIntegerv(0x821D, &extCount);
    auto hasExt = [&](const char* name) {
        for (GLint i = 0; i < extCount; ++i)
            if (std::strcmp(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i)), name) == 0) return true;
        return false;
    };
    for (const char* extension : {"GL_EXT_memory_object", "GL_EXT_memory_object_win32", "GL_EXT_semaphore", "GL_EXT_semaphore_win32"}) {
        bool present = hasExt(extension); std::cout << extension << '=' << present << '\n';
        require(present, std::string("Missing extension: ") + extension);
    }
    GLPROC(glGetUnsignedBytei_vEXT, void, GLenum, GLuint, GLubyte*);
    GLPROC(glGetUnsignedBytevEXT, void, GLenum, GLubyte*);
    GLPROC(glCreateMemoryObjectsEXT, void, GLsizei, GLuint*);
    GLPROC(glMemoryObjectParameterivEXT, void, GLuint, GLenum, const GLint*);
    GLPROC(glImportMemoryWin32HandleEXT, void, GLuint, uint64_t, GLenum, void*);
    GLPROC(glTexStorageMem2DEXT, void, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, uint64_t);
    GLPROC(glDeleteMemoryObjectsEXT, void, GLsizei, const GLuint*);
    GLPROC(glGenFramebuffers, void, GLsizei, GLuint*);
    GLPROC(glBindFramebuffer, void, GLenum, GLuint);
    GLPROC(glFramebufferTexture2D, void, GLenum, GLenum, GLenum, GLuint, GLint);
    GLPROC(glCheckFramebufferStatus, GLenum, GLenum);
    GLPROC(glDeleteFramebuffers, void, GLsizei, const GLuint*);
    GLPROC(glGenSemaphoresEXT, void, GLsizei, GLuint*);
    GLPROC(glImportSemaphoreWin32HandleEXT, void, GLuint, GLenum, void*);
    GLPROC(glWaitSemaphoreEXT, void, GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
    GLPROC(glSignalSemaphoreEXT, void, GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
    GLPROC(glDeleteSemaphoresEXT, void, GLsizei, const GLuint*);

    std::array<uint8_t, VK_UUID_SIZE> glUUID{}, glDriverUUID{};
    glGetUnsignedBytei_vEXT(0x9597, 0, glUUID.data()); // DEVICE_UUID_EXT
    glGetUnsignedBytevEXT(0x9598, glDriverUUID.data()); glcheck("device UUID");
    auto framework = Renderer::instance().framework();
    require(framework != nullptr, "Radiance renderer not initialized");
    // Startup-only diagnostics: no concurrent game frame may use this queue.
    framework->waitDeviceIdle();
    auto gpu = framework->physicalDevice()->vkPhysicalDevice();
    auto device = framework->device()->vkDevice();
    auto queue = framework->device()->mainVkQueue();
    auto family = framework->physicalDevice()->mainQueueIndex();
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext = &id;
    vkGetPhysicalDeviceProperties2(gpu, &props);
    require(!std::memcmp(id.deviceUUID, glUUID.data(), VK_UUID_SIZE), "Renderer/GL device UUID mismatch");
    require(!std::memcmp(id.driverUUID, glDriverUUID.data(), VK_UUID_SIZE), "Renderer/GL driver UUID mismatch");
    std::cout << "[Hybrid] Uses actual Radiance Vulkan device=true\n";
    uint32_t nExt = 0; vkcheck(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &nExt, nullptr));
    std::vector<VkExtensionProperties> exts(nExt); vkcheck(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &nExt, exts.data()));
    const char* deviceExtensions[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
    for (const auto* name : deviceExtensions) {
        bool found = false; for (auto& ext : exts) if (!std::strcmp(name, ext.extensionName)) found = true;
        require(found, std::string("Missing Vulkan extension: ") + name);
    }
    constexpr auto handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    constexpr VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    constexpr VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT}) {
        VkPhysicalDeviceExternalImageFormatInfo ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO}; ei.handleType = handleType;
        VkPhysicalDeviceImageFormatInfo2 fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2}; fi.pNext = &ei;
        fi.format = format; fi.type = VK_IMAGE_TYPE_2D; fi.tiling = VK_IMAGE_TILING_OPTIMAL;
        fi.usage = format == VK_FORMAT_D32_SFLOAT ? depthUsage : colorUsage;
        VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 fp{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2}; fp.pNext = &ep;
        auto result = vkGetPhysicalDeviceImageFormatProperties2(gpu, &fi, &fp);
        auto flags = result == VK_SUCCESS ? ep.externalMemoryProperties.externalMemoryFeatures : 0;
        std::cout << "External image format=" << format << " query=" << result << " featureFlags=" << flags << '\n';
        if (format == testFormat) require(result == VK_SUCCESS && (flags & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT), "Selected format export unavailable");
    }
    VkPhysicalDeviceExternalSemaphoreInfo esi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    esi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkExternalSemaphoreProperties esp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES}; vkGetPhysicalDeviceExternalSemaphoreProperties(gpu, &esi, &esp);
    require(esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT, "Semaphore export unavailable");
    auto exportMemory = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
    auto exportSemaphore = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR"));
    require(exportMemory && exportSemaphore, "Missing Vulkan export functions");
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    auto memoryType = [&](uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("No compatible memory type");
    };
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; external.handleTypes = handleType;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; imageInfo.pNext = &external; imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = testFormat; imageInfo.extent = {width, height, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL; imageInfo.usage = depth ? depthUsage : colorUsage;
    VkImage image; vkcheck(vkCreateImage(device, &imageInfo, nullptr, &image));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(device, image, &mr);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dedicated.image = image;
    VkExportMemoryWin32HandleInfoKHR winExport{VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    winExport.dwAccess = GENERIC_ALL; dedicated.pNext = &winExport;
    VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; exportInfo.handleTypes = handleType; exportInfo.pNext = &dedicated;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.pNext = &exportInfo; alloc.allocationSize = mr.size;
    alloc.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    std::cout << "Allocation size=" << mr.size << " memoryType=" << alloc.memoryTypeIndex << '\n';
    VkDeviceMemory memory; vkcheck(vkAllocateMemory(device, &alloc, nullptr, &memory)); vkcheck(vkBindImageMemory(device, image, memory, 0));
    VkMemoryGetWin32HandleInfoKHR mh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR}; mh.memory = memory; mh.handleType = handleType;
    HANDLE memoryHandle; vkcheck(exportMemory(device, &mh, &memoryHandle));
    GLuint glMemory, texture, framebuffer; glCreateMemoryObjectsEXT(1, &glMemory);
    glcheck("create memory object");
    GLint isDedicated = GL_TRUE; glMemoryObjectParameterivEXT(glMemory, 0x9581, &isDedicated);
    glcheck("dedicated memory parameter");
    glImportMemoryWin32HandleEXT(glMemory, mr.size, 0x9587, memoryHandle);
    // Win32 imports retain application ownership of the NT handle. Keep it
    // alive until the import completes: this NVIDIA driver defers the import
    // without synchronous debugging, and closing it immediately fails.
    // This initialization wait is outside the per-frame GPU handoff.
    glFinish(); glcheck("memory import"); CloseHandle(memoryHandle);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, 0x9580, 0x9584); // optimal tiling
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, glFormat, width, height, glMemory, 0); glcheck("shared texture storage");
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(0x8D40, framebuffer);
    glFramebufferTexture2D(0x8D40, depth ? 0x8D00 : 0x8CE0, GL_TEXTURE_2D, texture, 0);
    if (depth) { glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE); }
    require(glCheckFramebufferStatus(0x8D40) == 0x8CD5, "Shared GL framebuffer incomplete"); glcheck("framebuffer");
    std::array<VkSemaphore, 2> semaphores{}; std::array<GLuint, 2> glSemaphores{}; glGenSemaphoresEXT(2, glSemaphores.data());
    for (int i = 0; i < 2; ++i) {
        VkExportSemaphoreCreateInfo exportSem{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO}; exportSem.handleTypes = esi.handleType;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext = &exportSem;
        vkcheck(vkCreateSemaphore(device, &sci, nullptr, &semaphores[i]));
        VkSemaphoreGetWin32HandleInfoKHR sh{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR}; sh.semaphore = semaphores[i]; sh.handleType = esi.handleType;
        HANDLE h; vkcheck(exportSemaphore(device, &sh, &h)); glImportSemaphoreWin32HandleEXT(glSemaphores[i], 0x9587, h);
        glFinish(); glcheck("semaphore import"); CloseHandle(h);
    }
    glcheck("semaphore import");
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = VkDeviceSize(width) * height * pixelBytes; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer readback; vkcheck(vkCreateBuffer(device, &bci, nullptr, &readback)); vkGetBufferMemoryRequirements(device, readback, &mr);
    alloc.pNext = nullptr; alloc.allocationSize = mr.size;
    alloc.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory hostMemory; vkcheck(vkAllocateMemory(device, &alloc, nullptr, &hostMemory)); vkcheck(vkBindBufferMemory(device, readback, hostMemory, 0));
    void* mapped; vkcheck(vkMapMemory(device, hostMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex = family; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool; vkcheck(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd; vkcheck(vkAllocateCommandBuffers(device, &cai, &cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; vkcheck(vkCreateFence(device, &fci, nullptr, &fence));
    auto begin = [&] { vkcheck(vkResetCommandBuffer(cmd, 0)); VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkcheck(vkBeginCommandBuffer(cmd, &bi)); };
    auto barrier = [&](VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t src, uint32_t dst, VkAccessFlags read, VkAccessFlags write) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image = image; b.oldLayout = oldLayout; b.newLayout = newLayout;
        b.srcQueueFamilyIndex = src; b.dstQueueFamilyIndex = dst; b.srcAccessMask = read; b.dstAccessMask = write;
        b.subresourceRange = {VkImageAspectFlags(aspect), 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    auto seedFromVulkan = [&](uint32_t frame) {
        VkImageSubresourceRange range{VkImageAspectFlags(aspect), 0, 1, 0, 1};
        if (depth) {
            VkClearDepthStencilValue value{frame & 1 ? 0.25f : 0.5f, 0};
            vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        } else {
            const float high = hdr ? 4.0f : 1.0f;
            VkClearColorValue value{{frame & 1 ? high : 0.0f, frame & 1 ? 0.0f : high, high, 1.0f}};
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
        }
    };
    begin(); barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, family, family, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    seedFromVulkan(0);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family, VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0); vkcheck(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &semaphores[0]; vkcheck(vkQueueSubmit(queue, 1, &submit, fence));
    vkcheck(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
    constexpr GLenum generalLayout = 0x958D;
    size_t errors = 0;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        glWaitSemaphoreEXT(glSemaphores[0], 0, nullptr, 1, &texture, &generalLayout);
        // Verification only: prove GL observes the Vulkan write as well.
        std::array<float, 4> seed{};
        glReadPixels(width - 1, height - 1, 1, 1, depth ? GL_DEPTH_COMPONENT : GL_RGBA, GL_FLOAT, seed.data());
        if (depth) {
            if (seed[0] != (frame & 1 ? 0.25f : 0.5f)) ++errors;
        } else {
            float value = hdr ? 4.0f : 1.0f;
            std::array<float, 4> expected{frame & 1 ? value : 0.0f, frame & 1 ? 0.0f : value, value, 1.0f};
            if (seed != expected) ++errors;
        }
        glEnable(GL_SCISSOR_TEST); glDisable(GL_DITHER); glScissor(0, 0, width * 3 / 4, height);
        const float high = hdr ? 4.0f : 1.0f;
        glClearColor(frame & 1 ? high : 0.0f, 0, frame & 1 ? 0.0f : high, 1);
        glClearDepth(frame & 1 ? 0.75 : 1.0); glDepthMask(GL_TRUE);
        glClear(depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
        glScissor(0, 0, width / 2, height / 2);
        glClearColor(0, high, 0, 1); glClearDepth(0.0);
        glClear(depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
        glSignalSemaphoreEXT(glSemaphores[1], 0, nullptr, 1, &texture, &generalLayout); glFlush(); glcheck("render/signal");
        vkcheck(vkResetFences(device, 1, &fence)); begin();
        barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_EXTERNAL, family, 0, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{}; region.imageSubresource = {VkImageAspectFlags(aspect), 0, 0, 1}; region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        VkMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, family, family, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        seedFromVulkan(frame + 1);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family, VK_QUEUE_FAMILY_EXTERNAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        vkcheck(vkEndCommandBuffer(cmd)); VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &semaphores[1]; submit.pWaitDstStageMask = &waitStage;
        vkcheck(vkQueueSubmit(queue, 1, &submit, fence)); vkcheck(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
        auto* pixels = static_cast<uint8_t*>(mapped);
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
            bool green = x < width / 2 && y < height / 2;
            bool vulkan = x >= width * 3 / 4;
            std::array<uint8_t, 4> expected = green ? std::array<uint8_t, 4>{0,255,0,255} :
                frame & 1 ? std::array<uint8_t, 4>{255,0,0,255} : std::array<uint8_t, 4>{0,0,255,255};
            if (vulkan) expected = frame & 1 ? std::array<uint8_t, 4>{255,0,255,255} : std::array<uint8_t, 4>{0,255,255,255};
            auto* actual = pixels + (size_t(y) * width + x) * pixelBytes;
            if (depth) {
                float value = vulkan ? (frame & 1 ? 0.25f : 0.5f) : green ? 0.0f : frame & 1 ? 0.75f : 1.0f;
                if (std::memcmp(actual, &value, 4)) ++errors;
            } else if (hdr) {
                std::array<uint16_t, 4> half{};
                for (int k = 0; k < 3; ++k) half[k] = expected[k] ? 0x4400 : 0; // Half-float 4.0
                half[3] = 0x3c00; // Half-float 1.0
                if (std::memcmp(actual, half.data(), 8)) ++errors;
            } else if (std::memcmp(actual, expected.data(), 4)) ++errors;
        }
    }
    std::cout << "SharedImage format=" << formatName << " frames=" << frames << " pixelsChecked=" << uint64_t(width) * height * frames << " GLReadChecks=" << frames << " mismatches=" << errors
              << "\nGPU semaphore handoff=true\nCPU readback used only for verification=true\nPerformance benchmark=false\n";
    glFinish(); vkcheck(vkDeviceWaitIdle(device));
    glBindFramebuffer(0x8D40, 0); glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture);
    glDeleteMemoryObjectsEXT(1, &glMemory); glDeleteSemaphoresEXT(2, glSemaphores.data());
    vkUnmapMemory(device, hostMemory); vkDestroyBuffer(device, readback, nullptr); vkFreeMemory(device, hostMemory, nullptr);
    vkDestroyImage(device, image, nullptr); vkFreeMemory(device, memory, nullptr);
    for (auto semaphore : semaphores) vkDestroySemaphore(device, semaphore, nullptr);
    vkDestroyFence(device, fence, nullptr); vkDestroyCommandPool(device, pool, nullptr);
    std::cout << "VulkanValidation=" << validate << " errors=" << validationErrors << '\n';
    std::cout << "RESULT=" << (errors || validationErrors ? "FAIL" : "PASS") << '\n'; return errors || validationErrors ? 1 : 0;
} catch (const std::exception& error) {
    std::cerr << "RESULT=FAIL reason=" << error.what() << '\n';
    // This isolated process owns all resources; exit also releases them on a failed probe.
    return 2;
} }

extern "C" JNIEXPORT jboolean JNICALL Java_com_radiance_hybrid_NativeInterop_verifyRendererSharing(JNIEnv*, jclass) {
    // Each call recreates all shared images/FBOs and checks both directions.
    for (const char* format : {"rgba8", "rgba16f", "depth32f"}) {
        char* args[] = {const_cast<char*>("runtime"), const_cast<char*>("--format"), const_cast<char*>(format),
                       const_cast<char*>("--width"), const_cast<char*>("160"),
                       const_cast<char*>("--height"), const_cast<char*>("90"),
                       const_cast<char*>("--frames"), const_cast<char*>("8")};
        if (runtimeProbe(9, args) != 0) return JNI_FALSE;
    }
    std::cout << "[Hybrid] Runtime sharing verification PASS (all three formats)" << std::endl;
    return JNI_TRUE;
}

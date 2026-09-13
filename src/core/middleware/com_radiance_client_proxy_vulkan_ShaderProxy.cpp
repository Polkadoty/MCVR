#include "com_radiance_client_proxy_vulkan_ShaderProxy.h"

#include "core/all_extern.hpp"
#include "core/render/buffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/vulkan/physical_device.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace {
std::string toStdString(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars == nullptr ? "" : chars);
    if (chars != nullptr) { env->ReleaseStringUTFChars(value, chars); }
    return result;
}
} // namespace

// Keep the JNI boundary strict: malformed layouts must become Java errors, not GPU faults.
namespace {
uint32_t overlayAttributeBytes(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT: return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT: return 2;
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8_SINT: return 3;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT: return 4;
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT: return 2;
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT: return 4;
        case VK_FORMAT_R16G16B16_UNORM:
        case VK_FORMAT_R16G16B16_SNORM:
        case VK_FORMAT_R16G16B16_USCALED:
        case VK_FORMAT_R16G16B16_SSCALED:
        case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16_SINT: return 6;
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT: return 8;
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT: return 4;
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT: return 8;
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT: return 12;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT: return 16;
        default: throw std::invalid_argument("Unsupported overlay attribute format");
    }
}
}

extern "C" JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_ShaderProxy_registerShaderWithLayout(
    JNIEnv *env, jclass, jstring shaderKey, jint stride, jintArray attributes, jint drawMode,
    jint uniformSize, jstring vertexShaderPath, jstring fragmentShaderPath) {
    try {
        auto framework = Renderer::instance().framework();
        if (framework == nullptr) throw std::runtime_error("Renderer is not initialized");
        auto physical = framework->physicalDevice();
        auto limits = physical->properties().limits;
        if (attributes == nullptr || stride <= 0 || static_cast<uint32_t>(stride) > limits.maxVertexInputBindingStride)
            throw std::invalid_argument("Invalid overlay vertex stride or attributes");
        jsize count = env->GetArrayLength(attributes);
        if (count == 0 || count % 3 != 0 || static_cast<uint32_t>(count / 3) > limits.maxVertexInputAttributes)
            throw std::invalid_argument("Invalid overlay attribute count");
        std::vector<jint> packed(count);
        env->GetIntArrayRegion(attributes, 0, count, packed.data());
        if (env->ExceptionCheck()) return -1;
        vk::VertexLayoutInfo layout{};
        layout.bindingDescription = {0, static_cast<uint32_t>(stride), VK_VERTEX_INPUT_RATE_VERTEX};
        std::unordered_set<uint32_t> locations;
        for (jsize i = 0; i < count; i += 3) {
            auto location = static_cast<uint32_t>(packed[i]);
            auto format = static_cast<VkFormat>(packed[i + 1]);
            auto offset = static_cast<uint32_t>(packed[i + 2]);
            uint32_t bytes = overlayAttributeBytes(format);
            if (location >= limits.maxVertexInputAttributes || !locations.insert(location).second
                || offset > limits.maxVertexInputAttributeOffset || offset > static_cast<uint32_t>(stride)
                || bytes > static_cast<uint32_t>(stride) - offset)
                throw std::invalid_argument("Overlay attribute exceeds device limits or vertex stride");
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical->vkPhysicalDevice(), format, &properties);
            if ((properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0)
                throw std::invalid_argument("Overlay vertex format is unsupported by this GPU");
            layout.attributeDescriptions.push_back({location, 0, format, offset});
        }
        if (uniformSize < 0) throw std::invalid_argument("Negative overlay uniform size");
        return static_cast<jint>(framework->pipeline()->uiModule()->registerOverlayDrawShader(
            toStdString(env, shaderKey), 0, drawMode, uniformSize, toStdString(env, vertexShaderPath),
            toStdString(env, fragmentShaderPath), {}, &layout));
    } catch (const std::invalid_argument &error) {
        env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"), error.what());
    } catch (const std::exception &error) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), error.what());
    }
    return -1;
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_ShaderProxy_registerShader(
    JNIEnv *env,
    jclass,
    jstring shaderKey,
    jint vertexFormatType,
    jint drawMode,
    jint uniformSize,
    jstring vertexShaderPath,
    jstring fragmentShaderPath,
    jobjectArray defineNames,
    jobjectArray defineValues) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return -1;

    std::unordered_map<std::string, std::string> definitions;
    if (defineNames != nullptr && defineValues != nullptr) {
        jsize count = std::min(env->GetArrayLength(defineNames), env->GetArrayLength(defineValues));
        for (jsize i = 0; i < count; ++i) {
            auto name = static_cast<jstring>(env->GetObjectArrayElement(defineNames, i));
            auto value = static_cast<jstring>(env->GetObjectArrayElement(defineValues, i));
            definitions.emplace(toStdString(env, name), toStdString(env, value));
            env->DeleteLocalRef(name);
            env->DeleteLocalRef(value);
        }
    }

    auto shaderId = framework->pipeline()->uiModule()->registerOverlayDrawShader(
        toStdString(env, shaderKey), vertexFormatType, drawMode, uniformSize,
        toStdString(env, vertexShaderPath), toStdString(env, fragmentShaderPath), definitions);
    return static_cast<jint>(shaderId);
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_ShaderProxy_draw(
    JNIEnv *, jclass, jint vertexId, jint indexId, jint shaderId, jint indexCount, jint indexType, jlong uniformPtr,
    jint uniformSize) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return;
    auto vertexBuffer = Renderer::instance().buffers()->getBuffer(vertexId);
    auto indexBuffer = Renderer::instance().buffers()->getBuffer(indexId);
    uint32_t uniformOffset = 0;
    Renderer::instance().buffers()->appendOverlayDrawUniform(
        reinterpret_cast<uint8_t *>(uniformPtr), uniformSize, uniformOffset);
    auto context = framework->safeAcquireCurrentContext();
    auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
    pipelineContext->uiModuleContext->drawIndexed(vertexBuffer, indexBuffer, shaderId, uniformOffset, indexCount,
                                                  static_cast<VkIndexType>(indexType));
}

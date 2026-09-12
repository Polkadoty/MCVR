#include "com_radiance_client_proxy_vulkan_TextureProxy.h"

#include "core/render/emission.hpp"
#include "core/render/renderer.hpp"
#include "core/render/textures.hpp"
#include "texture_proxy_errors.hpp"
#include <windows.h>
#include <GL/gl.h>
#include <stdexcept>

namespace {
// Selected once, before Minecraft allocates any texture identities. OpenGL owns
// the numeric namespace; Vulkan reserves the same identity for its separate image.
HGLRC hybridTextureContext = nullptr;
}

extern "C" {
JNIEXPORT void JNICALL Java_com_radiance_hybrid_NativeInterop_enableTextureNamespace(JNIEnv *env, jclass) {
    texture_proxy::guardUpdate(env, [&] {
        auto textures = Renderer::instance().textures();
        if (!textures || !wglGetCurrentContext()) throw std::runtime_error("Hybrid texture namespace requires initialized Vulkan and current GL context");
        std::scoped_lock lock(textures->mtx_);
        if (hybridTextureContext) return;
        if (!textures->textures_.empty()) throw std::runtime_error("Cannot switch texture namespace after texture allocation");
        hybridTextureContext = wglGetCurrentContext();
    });
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_generateTextureId(JNIEnv *env, jclass) {
    auto textures = Renderer::instance().textures();
    if (!hybridTextureContext) return textures ? textures->allocateTexture() : 0;
    jint result = -1;
    texture_proxy::guardUpdate(env, [&] {
        if (!textures || wglGetCurrentContext() != hybridTextureContext)
            throw std::runtime_error("Hybrid texture allocation outside owning GL context");
        std::scoped_lock lock(textures->mtx_);
        GLuint id = 0;
        do {
            glGenTextures(1, &id);
            if (id == 0) throw std::runtime_error("OpenGL texture name allocation failed");
            // The existing native registry does not release logical identities.
            // Keep any recycled GL names reserved until context destruction so
            // they cannot alias an older Vulkan texture still referenced by meshes.
        } while (textures->textures_.contains(id));
        textures->textures_.emplace(id, nullptr);
        textures->samplers.emplace(id, nullptr);
        result = static_cast<jint>(id);
    });
    return result;
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_prepareImage(
    JNIEnv *env, jclass, jint id, jint maxLevel, jint width, jint height, jint format) {
    texture_proxy::guardUpdate(env, [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) return;
        auto vkFormat = static_cast<VkFormat>(format);
        textures->initializeTexture(id, maxLevel, width, height, vkFormat);
        if (auto emission = textures->emission(); emission != nullptr) {
            emission->resetTexture(static_cast<uint32_t>(id));
        }
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_setFilter(
    JNIEnv *env, jclass, jint id, jint samplingMode, jint mipmapMode) {
    texture_proxy::guardUpdate(env, [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) return;
        auto vkSamplingMode = static_cast<VkFilter>(samplingMode);
        auto vkMipmapMode = static_cast<VkSamplerMipmapMode>(mipmapMode);
        textures->setSamplingMode(id, vkSamplingMode, vkMipmapMode);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_setClamp(JNIEnv *env,
                                                                                   jclass,
                                                                                   jint id,
                                                                                   jint addressMode) {
    texture_proxy::guardUpdate(env, [&] {
        auto textures = Renderer::instance().textures();
        if (textures == nullptr) return;
        auto vkSamplerAddressMode = static_cast<VkSamplerAddressMode>(addressMode);
        textures->setAddressMode(id, vkSamplerAddressMode);
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_queueUpload(JNIEnv *,
                                                                                      jclass,
                                                                                      jlong srcPointer,
                                                                                      jint srcSizeInBytes,
                                                                                      jint srcRowPixels,
                                                                                      jint dstId,
                                                                                      jint srcOffsetX,
                                                                                      jint srcOffsetY,
                                                                                      jint dstOffsetX,
                                                                                      jint dstOffsetY,
                                                                                      jint width,
                                                                                      jint height,
                                                                                      jint level) {
    auto textures = Renderer::instance().textures();
    if (textures == nullptr) return;
    textures->queueUpload(reinterpret_cast<uint8_t *>(srcPointer), srcSizeInBytes, srcRowPixels, dstId, srcOffsetX,
                          srcOffsetY, dstOffsetX, dstOffsetY, width, height, level);
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_uploadEmissionTileNative(JNIEnv *,
                                                                                                   jclass,
                                                                                                   jint textureId,
                                                                                                   jlong tileKey,
                                                                                                   jlong cellsPtr,
                                                                                                   jint cellCount) {
    auto textures = Renderer::instance().textures();
    if (textures == nullptr) return;
    auto emission = textures->emission();
    if (emission == nullptr) return;

    emission->updateTile(static_cast<uint32_t>(textureId), static_cast<uint64_t>(tileKey),
                         reinterpret_cast<const EmissionCellUpload *>(cellsPtr), cellCount);
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_TextureProxy_performQueuedUpload(JNIEnv *, jclass) {
    auto textures = Renderer::instance().textures();
    if (textures == nullptr) return;
    textures->performQueuedUpload();
}
}

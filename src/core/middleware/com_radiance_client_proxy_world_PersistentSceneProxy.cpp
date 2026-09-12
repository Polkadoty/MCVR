#include "core/render/renderer.hpp"
#include "core/render/textures.hpp"
#include "core/render/world.hpp"
#include <jni.h>
#include <stdexcept>

namespace {
void fail(JNIEnv *env, const char *type, const char *message) {
    if (env->ExceptionCheck()) return;
    if (auto c = env->FindClass(type)) env->ThrowNew(c, message);
}
template <class F>
void guard(JNIEnv *env, F &&f) {
    try {
        f();
    } catch (const std::invalid_argument &e) {
        fail(env, "java/lang/IllegalArgumentException", e.what());
    } catch (const std::exception &e) { fail(env, "java/lang/IllegalStateException", e.what()); } catch (...) {
        fail(env, "java/lang/IllegalStateException", "unknown persistent scene native error");
    }
}
persistent::Scene &scene() {
    auto renderer = Renderer::try_instance();
    if (!renderer || !renderer->world()) throw std::runtime_error("Radiance world renderer unavailable");
    return renderer->world()->persistentScene();
}
std::span<const std::byte> packet(JNIEnv *env, jobject buffer, jint length) {
    if (!buffer || length < 0 || length > 64 * 1024 * 1024) throw std::invalid_argument("invalid scene packet size");
    auto address = env->GetDirectBufferAddress(buffer);
    auto capacity = env->GetDirectBufferCapacity(buffer);
    if (!address || capacity < length)
        throw std::invalid_argument("scene packet must be a direct buffer of sufficient capacity");
    return {static_cast<const std::byte *>(address), static_cast<size_t>(length)};
}
} // namespace
extern "C" {
JNIEXPORT jlong JNICALL Java_com_radiance_client_proxy_world_PersistentSceneProxy_reset(JNIEnv *env, jclass) {
    jlong result = 0;
    guard(env, [&] { result = static_cast<jlong>(scene().reset()); });
    return result;
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_PersistentSceneProxy_material(
    JNIEnv *env, jclass, jlong epoch, jlong id, jlong revision, jint texture, jint alpha, jfloat emission) {
    guard(env, [&] {
        auto &target = scene();
        auto textures = Renderer::instance().textures();
        if (texture < 0 || alpha < 0) throw std::invalid_argument("invalid texture/material mode");
        std::lock_guard lock(textures->mtx_);
        auto it = textures->textures_.find(static_cast<uint32_t>(texture));
        if (it == textures->textures_.end() || !it->second)
            throw std::invalid_argument("texture is not registered with Radiance");
        target.material(epoch, persistent::Material{static_cast<uint64_t>(id), static_cast<uint64_t>(revision),
                                                    static_cast<uint32_t>(texture), static_cast<uint32_t>(alpha),
                                                    emission, it->second});
    });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_PersistentSceneProxy_mesh(JNIEnv *env,
                                                                                      jclass,
                                                                                      jlong epoch,
                                                                                      jlong id,
                                                                                      jlong revision,
                                                                                      jlong material,
                                                                                      jlong materialRevision,
                                                                                      jobject data,
                                                                                      jint bytes) {
    guard(env, [&] { scene().mesh(epoch, id, revision, material, materialRevision, packet(env, data, bytes)); });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_PersistentSceneProxy_instances(
    JNIEnv *env, jclass, jlong epoch, jobject data, jint bytes) {
    guard(env, [&] { scene().instances(epoch, packet(env, data, bytes)); });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_PersistentSceneProxy_retireMesh(JNIEnv *env,
                                                                                            jclass,
                                                                                            jlong epoch,
                                                                                            jlong id) {
    guard(env, [&] { scene().retireMesh(epoch, id); });
}
JNIEXPORT void JNICALL Java_com_radiance_client_proxy_world_PersistentSceneProxy_retireMaterial(JNIEnv *env,
                                                                                                jclass,
                                                                                                jlong epoch,
                                                                                                jlong id) {
    guard(env, [&] { scene().retireMaterial(epoch, id); });
}
}

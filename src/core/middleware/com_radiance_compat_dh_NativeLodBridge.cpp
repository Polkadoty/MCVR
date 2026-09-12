#include <jni.h>
#include "core/render/renderer.hpp"
#include "core/render/world.hpp"
#include "core/render/compat/dh/lod_scene.hpp"

namespace {
void fail(JNIEnv* env, const std::exception& error) {
    auto type = env->FindClass("java/lang/IllegalStateException");
    if (type) env->ThrowNew(type, error.what());
}
}
extern "C" {
JNIEXPORT void JNICALL Java_com_radiance_compat_dh_NativeLodBridge_configure(JNIEnv* env, jclass, jlong live, jlong queue) {
    try {
        if (live < 0 || queue < 0) throw std::invalid_argument("Negative DH memory limit");
        if (auto world = Renderer::instance().world()) world->lods()->configure(static_cast<size_t>(live), static_cast<size_t>(queue));
    } catch (const std::exception& error) { fail(env, error); }
}
JNIEXPORT jboolean JNICALL Java_com_radiance_compat_dh_NativeLodBridge_selectionPublished(JNIEnv* env, jclass, jlong epoch) {
    try {
        if (auto world = Renderer::instance().world()) return world->lods()->selectionPublished(static_cast<uint64_t>(epoch));
        return false;
    } catch (const std::exception& error) { fail(env, error); return false; }
}

JNIEXPORT void JNICALL Java_com_radiance_compat_dh_NativeLodBridge_beginWorld(JNIEnv* env, jclass, jlong epoch) {
    try { if (auto world = Renderer::instance().world()) world->lods()->beginWorld(static_cast<uint64_t>(epoch)); }
    catch (const std::exception& error) { fail(env, error); }
}
JNIEXPORT jboolean JNICALL Java_com_radiance_compat_dh_NativeLodBridge_upload(JNIEnv* env, jclass, jobject buffer) {
    try {
        auto world = Renderer::instance().world();
        if (!world) return false;
        auto data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buffer));
        auto size = env->GetDirectBufferCapacity(buffer);
        if (!data || size < 80 || size > 80 + 2 * 1024 * 1024) throw std::invalid_argument("Invalid direct LOD packet");
        return world->lods()->enqueue(std::span(data, static_cast<size_t>(size)));
    } catch (const std::exception& error) { fail(env, error); return false; }
}
JNIEXPORT void JNICALL Java_com_radiance_compat_dh_NativeLodBridge_select(JNIEnv* env, jclass, jlong epoch,
        jlongArray ids, jlongArray revisions) {
    try {
        auto world = Renderer::instance().world();
        if (!world) return;
        if (!ids || !revisions) throw std::invalid_argument("Missing selected LOD arrays");
        jsize count = env->GetArrayLength(ids);
        if (count > 2048 || env->GetArrayLength(revisions) != count) throw std::invalid_argument("Invalid selected LOD arrays");
        std::vector<jlong> idCopy(count), revisionCopy(count);
        env->GetLongArrayRegion(ids, 0, count, idCopy.data());
        if (env->ExceptionCheck()) return;
        env->GetLongArrayRegion(revisions, 0, count, revisionCopy.data());
        if (env->ExceptionCheck()) return;
        std::vector<std::pair<uint64_t,uint64_t>> selected;
        for (jsize i = 0; i < count; ++i) {
            if (revisionCopy[i] < 0) throw std::invalid_argument("Negative LOD revision");
            selected.emplace_back(static_cast<uint64_t>(idCopy[i]), static_cast<uint64_t>(revisionCopy[i]));
        }
        world->lods()->select(static_cast<uint64_t>(epoch), selected);
    } catch (const std::exception& error) { fail(env, error); }
}
JNIEXPORT void JNICALL Java_com_radiance_compat_dh_NativeLodBridge_closeWorld(JNIEnv* env, jclass) {
    try { if (auto world = Renderer::instance().world()) world->lods()->close(); }
    catch (const std::exception& error) { fail(env, error); }
}
}

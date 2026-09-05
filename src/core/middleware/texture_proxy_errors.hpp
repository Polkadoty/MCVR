#pragma once

#include <jni.h>
#include <exception>

namespace texture_proxy {
// Never unwind a native exception through the JVM. Preserve an existing Java
// exception if JNI has already reported a failure (including class lookup).
template <typename Update>
void guardUpdate(JNIEnv *env, Update &&update) noexcept {
    if (env->ExceptionCheck()) return;
    try {
        update();
    } catch (const std::exception &error) {
        if (env->ExceptionCheck()) return;
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) {
            env->ThrowNew(exceptionClass, error.what());
            env->DeleteLocalRef(exceptionClass);
        }
    } catch (...) {
        if (env->ExceptionCheck()) return;
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) {
            env->ThrowNew(exceptionClass, "Unknown native texture update failure");
            env->DeleteLocalRef(exceptionClass);
        }
    }
}
} // namespace texture_proxy

#pragma once
#include <jni.h>
#include <exception>
#include <iostream>
#include <utility>

namespace radiance {
template<class Operation>
void guardedSubmitCommand(JNIEnv* env, Operation&& operation) {
    try {
        std::forward<Operation>(operation)();
    } catch (const std::exception& error) {
        std::cerr << "[Radiance] Native submitCommand failed: " << error.what() << std::endl;
        if (auto type = env->FindClass("java/lang/IllegalStateException")) env->ThrowNew(type, error.what());
    } catch (...) {
        constexpr auto message = "Unknown native exception while submitting Radiance frame";
        std::cerr << "[Radiance] " << message << std::endl;
        if (auto type = env->FindClass("java/lang/IllegalStateException")) env->ThrowNew(type, message);
    }
    // A pending Java exception aborts submitCommandAndPresent; never continue/present this frame.
}
}

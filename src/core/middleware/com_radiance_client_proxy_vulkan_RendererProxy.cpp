#include "core/middleware/jni_frame_guard.hpp"
#include "com_radiance_client_proxy_vulkan_RendererProxy.h"

#include "core/all_extern.hpp"
#include "core/render/buffers.hpp"
#include "core/render/modules/ui_module.hpp"
#include "core/render/pipeline.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/renderer.hpp"
#include "core/render/textures.hpp"
#include "core/render/world.hpp"

#include <algorithm>
#include <unordered_map>

#if defined(_WIN32)
#    include <windows.h>
using DYNLIB_HANDLE = HMODULE;

static DYNLIB_HANDLE try_get_loaded_handle(const wchar_t *wname) {
    return GetModuleHandleW(wname);
}

static FARPROC getproc(DYNLIB_HANDLE h, const char *sym) {
    FARPROC p = GetProcAddress(h, sym);
    if (!p) {
        std::cerr << "GetProcAddress failed: " << sym << std::endl;
        std::abort();
    }
    return p;
}

#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#    include <dlfcn.h>
using DYNLIB_HANDLE = void *;

static DYNLIB_HANDLE try_get_loaded_handle(const char *name) {
    return dlopen(name, RTLD_NOW | RTLD_NOLOAD);
}

static void *getproc(DYNLIB_HANDLE h, const char *sym) {
    void *p = dlsym(h, sym);
    if (!p) {
        std::cerr << "dlsym failed: " << sym << " — " << dlerror() << std::endl;
        std::abort();
    }
    return p;
}

#else
#    error "Unsupported platform"
#endif

static DYNLIB_HANDLE bind_handle_from_candidates(JNIEnv *env, jobjectArray jnames) {
    jsize n = env->GetArrayLength(jnames);
    if (n == 0) return nullptr;
#if defined(_WIN32)
    for (jsize i = 0; i < n; ++i) {
        jstring s = (jstring)env->GetObjectArrayElement(jnames, i);
        const jchar *w = env->GetStringChars(s, nullptr);
        DYNLIB_HANDLE h = try_get_loaded_handle(reinterpret_cast<const wchar_t *>(w));
        env->ReleaseStringChars(s, w);
        env->DeleteLocalRef(s);
        if (h) return h;
    }
#else
    for (jsize i = 0; i < n; ++i) {
        jstring s = (jstring)env->GetObjectArrayElement(jnames, i);
        const char *c = env->GetStringUTFChars(s, nullptr);
        DYNLIB_HANDLE h = try_get_loaded_handle(c);
        env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
        if (h) return h;
    }
#endif
    return nullptr;
}

static void bind_symbols(DYNLIB_HANDLE h) {
#if defined(_WIN32)
    auto gp = [&](const char *sym) { return getproc(h, sym); };
#else
    auto gp = [&](const char *sym) { return getproc(h, sym); };
#endif
    p_glfwInit = reinterpret_cast<PFN_glfwInit>(gp("glfwInit"));
    p_glfwTerminate = reinterpret_cast<PFN_glfwTerminate>(gp("glfwTerminate"));
    p_glfwGetWindowSize = reinterpret_cast<PFN_glfwGetWindowSize>(gp("glfwGetWindowSize"));
    p_glfwCreateWindowSurface = reinterpret_cast<PFN_glfwCreateWindowSurface>(gp("glfwCreateWindowSurface"));
    p_glfwGetRequiredInstanceExtensions =
        reinterpret_cast<PFN_glfwGetRequiredInstanceExtensions>(gp("glfwGetRequiredInstanceExtensions"));
    p_glfwSetWindowTitle = reinterpret_cast<PFN_glfwSetWindowTitle>(gp("glfwSetWindowTitle"));
    p_glfwSetFramebufferSizeCallback =
        reinterpret_cast<PFN_glfwSetFramebufferSizeCallback>(gp("glfwSetFramebufferSizeCallback"));
    p_glfwGetFramebufferSize = reinterpret_cast<PFN_glfwGetFramebufferSize>(gp("glfwGetFramebufferSize"));
    p_glfwWaitEvents = reinterpret_cast<PFN_glfwWaitEvents>(gp("glfwWaitEvents"));
}

static std::u16string JStringToU16(JNIEnv* env, jstring jstr) {
    if (!jstr) return {};
    const jchar* chars = env->GetStringChars(jstr, nullptr);
    jsize len = env->GetStringLength(jstr);
    std::u16string u16(reinterpret_cast<const char16_t*>(chars),
                       reinterpret_cast<const char16_t*>(chars) + len);
    env->ReleaseStringChars(jstr, chars);
    return u16;
}

static std::filesystem::path JStringToPath(JNIEnv* env, jstring jstr) {
    std::u16string u16 = JStringToU16(env, jstr);
    return std::filesystem::path(u16);
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_initFolderPath(JNIEnv *env,
                                                                                          jclass,
                                                                                          jstring folderPath) {
    if (folderPath == NULL) { return; }

    Renderer::folderPath = JStringToU16(env, folderPath);
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_initRenderer(JNIEnv *env,
                                                                                        jclass,
                                                                                        jobjectArray candidates,
                                                                                        jlong windowHandle) {
    try {
        DYNLIB_HANDLE h = bind_handle_from_candidates(env, candidates);
        if (!h) {
            std::cerr << "[GLFW-Bind] Could not find already-loaded GLFW via NOLOAD/GetModuleHandle."
                         " Ensure Java(LWJGL) loads GLFW before JNI and pass correct names/paths."
                      << std::endl;
            std::abort();
        }
        bind_symbols(h);

        GLFWwindow *window = (GLFWwindow *)(intptr_t)windowHandle;
        Renderer::init(window);
        Renderer::instance().framework()->acquireContext();
    } catch (const std::exception &error) {
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) env->ThrowNew(exceptionClass, error.what());
    } catch (...) {
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) env->ThrowNew(exceptionClass, "Unknown native initRenderer failure");
    }
}

JNIEXPORT jint JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_maxSupportedTextureSize(JNIEnv *, jclass) {
    auto maxImageSize = Renderer::instance().framework()->physicalDevice()->properties().limits.maxImageDimension2D;
    return maxImageSize;
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_acquireContext(JNIEnv *env, jclass) {
    try {
        auto framework = Renderer::instance().framework();
        if (framework != nullptr) framework->acquireContext();
    } catch (const std::exception &error) {
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) env->ThrowNew(exceptionClass, error.what());
    } catch (...) {
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) env->ThrowNew(exceptionClass, "Unknown native acquireContext failure");
    }
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_submitCommand(JNIEnv *env, jclass) {
    radiance::guardedSubmitCommand(env, [] {
        auto framework = Renderer::instance().framework();
        if (framework != nullptr) framework->submitCommand();
    });
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_present(JNIEnv *env, jclass) {
    // C++ exceptions must not unwind across the JNI boundary. Keep the renderer's
    // diagnostic in Minecraft's crash report, including pipeline rebuild failures.
    try {
        auto framework = Renderer::instance().framework();
        if (framework == nullptr) return;
        framework->present();
    } catch (const std::exception &error) {
        std::cerr << "[Radiance] Native present failed: " << error.what() << std::endl;
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) {
            env->ThrowNew(exceptionClass, error.what());
        }
    } catch (...) {
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) {
            env->ThrowNew(exceptionClass, "Unknown native exception while presenting Radiance frame");
        }
    }
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_fuseWorld(JNIEnv *, jclass) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return;
    auto context = framework->safeAcquireCurrentContext();
    auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
    pipelineContext->fuseWorld();
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_postBlur(JNIEnv *, jclass) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return;
    auto world = Renderer::instance().world();
    if (world != nullptr && world->shouldRender()) return;
    auto context = framework->safeAcquireCurrentContext();
    auto pipelineContext = framework->pipeline()->acquirePipelineContext(context);
    pipelineContext->uiModuleContext->postBlur(6);
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_close(JNIEnv *env, jclass) {
    try {
        auto framework = Renderer::instance().framework();
        if (framework == nullptr) return;
        Renderer::instance().close();
    } catch (const std::exception &error) {
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) env->ThrowNew(exceptionClass, error.what());
    } catch (...) {
        jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
        if (exceptionClass != nullptr) env->ThrowNew(exceptionClass, "Unknown native close failure");
    }
}

JNIEXPORT void JNICALL
Java_com_radiance_client_proxy_vulkan_RendererProxy_shouldRenderWorld(JNIEnv *, jclass, jboolean shouldRenderWorld) {
    auto world = Renderer::instance().world();
    if (world == nullptr) return;
    world->shouldRender() = shouldRenderWorld;
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_takeScreenshot(
    JNIEnv *, jclass, jboolean withUI, jint width, jint height, jint channel, jlong pointer) {
    auto framework = Renderer::instance().framework();
    if (framework == nullptr) return;
    framework->takeScreenshot(withUI, width, height, channel, reinterpret_cast<void *>(pointer));
}

JNIEXPORT void JNICALL Java_com_radiance_client_proxy_vulkan_RendererProxy_setFrameGenerationAllowed(JNIEnv *, jclass, jboolean allowed) {
    Renderer::options.frameGenerationAllowed = allowed;
}

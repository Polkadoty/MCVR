// Streamline integration adapted from PEQHUB/MCVR 8e1a148 (GPL-3.0).
#include "core/render/streamline_context.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace {
HMODULE interposer{};
bool initialized{}, deviceReady{}, reflexSupported{}, fgSupported{}, fgLoaded{}, requirementsValid{true};
uint32_t frameIndex{}, graphicsQueues{}, computeQueues{}, opticalQueues{};
uint32_t maxGeneratedFrames{}, minimumFGDimension{};
sl::FrameToken *frameToken{};
std::vector<std::string> instanceExtensions, deviceExtensions, features12, features13;
std::wstring pluginDirectory;
std::ofstream logFile;
std::mutex logMutex;
std::string errorMessage;
std::atomic<int> callbackError{};

PFun_slInit *initSL{};
PFun_slShutdown *shutdownSL{};
PFun_slIsFeatureSupported *isFeatureSupported{};
PFun_slGetFeatureFunction *getFeatureFunction{};
PFun_slGetNewFrameToken *newFrameToken{};
PFun_slGetFeatureRequirements *getRequirements{};
PFun_slSetConstants *setConstantsSL{};
PFun_slSetTagForFrame *setTags{};
PFun_slSetFeatureLoaded *setLoaded{};
PFun_slReflexSetOptions *setReflex{};
PFun_slReflexSleep *sleepReflex{};
PFun_slReflexGetState *stateReflex{};
PFun_slPCLSetMarker *setMarker{};
PFun_slDLSSGSetOptions *setFG{};
PFun_slDLSSGGetState *stateFG{};

void log(const std::string &message) {
    std::lock_guard lock(logMutex);
    if (logFile) logFile << message << std::endl;
}
bool fail(const std::string &message) { errorMessage = message; log(message); return false; }
bool resultOK(sl::Result result, const char *operation) {
    if (result == sl::Result::eOk) return true;
    return fail(std::string(operation) + " failed: " + std::to_string(static_cast<int>(result)));
}
void sdkLog(sl::LogType, const char *message) { if (message) log(message); }
void fgError(const sl::APIError &error) {
    // NVIDIA requires the callback to return immediately; no file IO/locks here.
    callbackError.store(error.vkRes ? error.vkRes : -1, std::memory_order_relaxed);
}
bool checkCallback() {
    int error = callbackError.exchange(0, std::memory_order_relaxed);
    return !error || fail("DLSS-G asynchronous API error: " + std::to_string(error));
}

// Verify the actual trusted signer's subject from the successful WinTrust chain.
// This avoids accepting an unrelated valid signer or trusting an unverified
// certificate merely embedded alongside a different signature in the file.
bool signedByNvidia(const std::filesystem::path &path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{sizeof(trust)};
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &trust);
    bool valid = false;
    if (status == ERROR_SUCCESS) {
        auto data = WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto signer = data ? WTHelperGetProvSignerFromChain(data, 0, FALSE, 0) : nullptr;
        auto cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        if (cert && cert->pCert) {
            DWORD size = CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
            std::wstring name(size, L'\0');
            if (size > 1 && CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                    0, nullptr, name.data(), size) == size) {
                name.resize(size - 1);
                valid = name == L"NVIDIA Corporation";
            }
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust);
    return valid;
}
template<class T> bool core(const char *name, T *&pointer) {
    pointer = reinterpret_cast<T *>(GetProcAddress(interposer, name));
    return pointer || fail(std::string("Missing Streamline export: ") + name);
}
template<class T> bool feature(sl::Feature id, const char *name, T *&pointer) {
    pointer = nullptr;
    void *raw{};
    if (!resultOK(getFeatureFunction(id, name, raw), name) || !raw) return false;
    pointer = reinterpret_cast<T *>(raw);
    return true;
}
bool loadFG() {
    return feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", setFG)
        && feature(sl::kFeatureDLSS_G, "slDLSSGGetState", stateFG);
}
void appendUnique(std::vector<std::string> &out, uint32_t count, const char *const *names) {
    for (uint32_t i = 0; names && i < count; ++i)
        if (names[i] && std::find(out.begin(), out.end(), names[i]) == out.end()) out.emplace_back(names[i]);
}
bool requirements() {
    for (auto id : {sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G}) {
        sl::FeatureRequirements req{};
        if (!resultOK(getRequirements(id, req), "slGetFeatureRequirements")) return false;
        appendUnique(instanceExtensions, req.vkNumInstanceExtensions, req.vkInstanceExtensions);
        appendUnique(deviceExtensions, req.vkNumDeviceExtensions, req.vkDeviceExtensions);
        appendUnique(features12, req.vkNumFeatures12, req.vkFeatures12);
        appendUnique(features13, req.vkNumFeatures13, req.vkFeatures13);
        graphicsQueues = std::max(graphicsQueues, req.vkNumGraphicsQueuesRequired);
        computeQueues = std::max(computeQueues, req.vkNumComputeQueuesRequired);
        opticalQueues = std::max(opticalQueues, req.vkNumOpticalFlowQueuesRequired);
    }
    for (const auto &s : instanceExtensions) log("Required instance extension: " + s);
    for (const auto &s : deviceExtensions) log("Required device extension: " + s);
    for (const auto &s : features12) log("Required Vulkan 1.2 feature: " + s);
    for (const auto &s : features13) log("Required Vulkan 1.3 feature: " + s);
    return true;
}
} // namespace

bool StreamlineContext::init(const wchar_t *path) {
    if (initialized) return true;
    if (!path || !*path) return false;
    pluginDirectory = std::filesystem::absolute(path).wstring();
    const auto directory = std::filesystem::path(pluginDirectory);
    const auto dll = directory / L"sl.interposer.dll";
    if (!std::filesystem::is_regular_file(dll)) return false; // ordinary optional fallback
    logFile.open(directory / "streamline.log", std::ios::trunc);
    // Allocate room for the NUL that WideCharToMultiByte writes, then remove it.
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pluginDirectory.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return fail("Invalid Streamline plugin path");
    std::string utf8(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pluginDirectory.c_str(), -1, utf8.data(), size, nullptr, nullptr))
        return fail("Streamline plugin path conversion failed");
    utf8.resize(static_cast<size_t>(size - 1));
    log("Plugin path: " + utf8);
    if (!signedByNvidia(dll)) return fail("sl.interposer.dll has no valid NVIDIA Authenticode signature");
    interposer = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!interposer) return fail("Loading Streamline failed: " + std::to_string(GetLastError()));
    bool loaded = core("slInit", initSL) && core("slShutdown", shutdownSL)
        && core("slIsFeatureSupported", isFeatureSupported) && core("slGetFeatureFunction", getFeatureFunction)
        && core("slGetNewFrameToken", newFrameToken) && core("slGetFeatureRequirements", getRequirements)
        && core("slSetConstants", setConstantsSL) && core("slSetTagForFrame", setTags)
        && core("slSetFeatureLoaded", setLoaded);
    if (!loaded) { FreeLibrary(interposer); interposer = nullptr; return false; }
    const wchar_t *pluginPaths[] = {pluginDirectory.c_str()};
    const sl::Feature features[] = {sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS_G};
    sl::Preferences preferences{};
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    preferences.flags = preferences.flags | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.logMessageCallback = sdkLog;
    preferences.pathToLogsAndData = pluginDirectory.c_str();
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 3;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "Radiance-0.1.5-port.1-mc1.21.1";
    preferences.projectId = "c1d4ef20-4f11-4da3-9aa8-1cf81cf37a91";
    preferences.applicationId = 0; // custom project UUID, never an NVIDIA-assigned ID
    if (!resultOK(initSL(preferences, sl::kSDKVersion), "slInit")) {
        FreeLibrary(interposer); interposer = nullptr; return false;
    }
    initialized = true;
    fgLoaded = true;
    if (!requirements()) { shutdown(); return false; }
    log("Streamline initialized (interposer mode); Frame Generation remains Off");
    return true;
}
bool StreamlineContext::onDeviceCreated(void *physicalDevice) {
    if (!initialized || !physicalDevice) return false;
    if (deviceReady) return true;
    if (!requirementsValid) {
        deviceReady = true;
        setFeatureLoaded(sl::kFeatureDLSS_G, false);
        return true;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physicalDevice;
    reflexSupported = resultOK(isFeatureSupported(sl::kFeatureReflex, adapter), "Reflex adapter support")
        && feature(sl::kFeatureReflex, "slReflexSetOptions", setReflex)
        && feature(sl::kFeatureReflex, "slReflexSleep", sleepReflex)
        && feature(sl::kFeatureReflex, "slReflexGetState", stateReflex);
    const bool pcl = feature(sl::kFeaturePCL, "slPCLSetMarker", setMarker);
    fgSupported = resultOK(isFeatureSupported(sl::kFeatureDLSS_G, adapter), "DLSS-G adapter support") && loadFG();
    deviceReady = true;
    if (reflexSupported && !setReflexOptions(sl::ReflexMode::eOff)) reflexSupported = false;
    if (!pcl || !reflexSupported) fgSupported = false; // FG requires complete Reflex/PCL integration
    if (fgSupported && !setDlssGOptions(sl::DLSSGMode::eOff)) fgSupported = false;
    if (fgSupported) {
        sl::DLSSGState capability{};
        if (getDlssGState(capability)) {
            maxGeneratedFrames = capability.numFramesToGenerateMax;
            minimumFGDimension = capability.minWidthOrHeight;
        } else fgSupported = false;
    }
    // Query/copy capability scalars first, then unload before the first swapchain
    // exists. Default-Off startup must not create an unnecessary FG proxy chain.
    if (!setFeatureLoaded(sl::kFeatureDLSS_G, false)) fgSupported = false;
    return true; // unsupported features do not invalidate an otherwise working Vulkan device
}
void StreamlineContext::shutdown() {
    if (!initialized) return;
    if (fgLoaded && setFG) setDlssGOptions(sl::DLSSGMode::eOff);
    if (reflexSupported) setReflexOptions(sl::ReflexMode::eOff);
    resultOK(shutdownSL(), "slShutdown");
    initialized = deviceReady = reflexSupported = fgSupported = fgLoaded = false;
    frameToken = nullptr;
    // Keep the module mapped: Vulkan destruction may still reference interposer entry points.
    // Never switch volk to unloaded function addresses during fallback/shutdown.
    instanceExtensions.clear(); deviceExtensions.clear(); features12.clear(); features13.clear();
    log("Streamline shut down");
}
bool StreamlineContext::isInitialized() { return initialized; }
bool StreamlineContext::isAvailable() { return initialized && deviceReady; }
bool StreamlineContext::isReflexAvailable() { return isAvailable() && reflexSupported; }
bool StreamlineContext::isDlssGSupported() { return isAvailable() && fgSupported; }
bool StreamlineContext::isDlssGLoaded() { return fgLoaded; }
bool StreamlineContext::getDlssGCapabilities(uint32_t &maxFrames, uint32_t &minDimension) {
    maxFrames = maxGeneratedFrames; minDimension = minimumFGDimension;
    return isDlssGSupported() && maxFrames > 0;
}
void StreamlineContext::invalidateRequirements(const std::string &reason) {
    requirementsValid = false;
    reflexSupported = fgSupported = false;
    fail("Optional Streamline features disabled: " + reason);
}
void *StreamlineContext::getVkGetInstanceProcAddr() { return initialized ? reinterpret_cast<void *>(GetProcAddress(interposer, "vkGetInstanceProcAddr")) : nullptr; }
void *StreamlineContext::getVkCreateDevice() { return initialized ? reinterpret_cast<void *>(GetProcAddress(interposer, "vkCreateDevice")) : nullptr; }
void *StreamlineContext::getVkGetDeviceProcAddr() { return initialized ? reinterpret_cast<void *>(GetProcAddress(interposer, "vkGetDeviceProcAddr")) : nullptr; }
const std::vector<std::string> &StreamlineContext::getRequiredInstanceExtensions() { return instanceExtensions; }
const std::vector<std::string> &StreamlineContext::getRequiredDeviceExtensions() { return deviceExtensions; }
const std::vector<std::string> &StreamlineContext::getRequiredVulkan12Features() { return features12; }
const std::vector<std::string> &StreamlineContext::getRequiredVulkan13Features() { return features13; }
uint32_t StreamlineContext::requiredGraphicsQueues() { return graphicsQueues; }
uint32_t StreamlineContext::requiredComputeQueues() { return computeQueues; }
uint32_t StreamlineContext::requiredOpticalFlowQueues() { return opticalQueues; }
bool StreamlineContext::setReflexOptions(sl::ReflexMode mode, uint32_t limit) {
    if (!isReflexAvailable() || !setReflex) return false;
    sl::ReflexOptions options{}; options.mode = mode; options.frameLimitUs = limit;
    return resultOK(setReflex(options), "slReflexSetOptions");
}
bool StreamlineContext::reflexSleep() { return isReflexAvailable() && sleepReflex && frameToken && resultOK(sleepReflex(*frameToken), "slReflexSleep"); }
bool StreamlineContext::getReflexState(sl::ReflexState &state) { return isReflexAvailable() && stateReflex && resultOK(stateReflex(state), "slReflexGetState"); }
bool StreamlineContext::pclSetMarker(sl::PCLMarker marker) { return isAvailable() && setMarker && frameToken && resultOK(setMarker(marker, *frameToken), "slPCLSetMarker"); }
bool StreamlineContext::setDlssGOptions(sl::DLSSGMode mode, uint32_t frames) {
    if (!isAvailable() || !fgLoaded || !setFG) return false;
    sl::DLSSGOptions options{};
    options.mode = mode; options.numFramesToGenerate = frames;
    // Off must not destroy completion timelines still referenced by in-flight
    // input slots. Explicit feature unload happens only after the manager drains.
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    options.onErrorCallback = fgError;
    return resultOK(setFG(sl::ViewportHandle(0), options), "slDLSSGSetOptions");
}
bool StreamlineContext::getDlssGState(sl::DLSSGState &state) {
    return isAvailable() && fgLoaded && stateFG && checkCallback()
        && resultOK(stateFG(sl::ViewportHandle(0), state, nullptr), "slDLSSGGetState");
}
bool StreamlineContext::setConstants(const sl::Constants &constants) {
    return isAvailable() && frameToken && resultOK(setConstantsSL(constants, *frameToken, sl::ViewportHandle(0)), "slSetConstants");
}
bool StreamlineContext::tagResources(const sl::ResourceTag *tags, uint32_t count, void *commandBuffer) {
    return isAvailable() && frameToken && resultOK(setTags(*frameToken, sl::ViewportHandle(0), tags, count,
        static_cast<sl::CommandBuffer *>(commandBuffer)), "slSetTagForFrame");
}
bool StreamlineContext::clearResourceTags() {
    if (!isAvailable() || !frameToken) return true; // no frame ever tagged
    sl::ResourceTag tags[] = {
        {nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
        {nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
        {nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent}
    };
    return tagResources(tags, 3);
}
bool StreamlineContext::setFeatureLoaded(sl::Feature id, bool loaded) {
    if (!isAvailable() || !resultOK(setLoaded(id, loaded), "slSetFeatureLoaded")) return false;
    if (id == sl::kFeatureDLSS_G) {
        fgLoaded = loaded;
        setFG = nullptr; stateFG = nullptr; // plugin unload invalidates feature pointers
        if (loaded && !loadFG()) return false;
    }
    return true;
}
bool StreamlineContext::advanceFrame() {
    frameToken = nullptr;
    if (!isAvailable()) return false;
    ++frameIndex;
    if (!resultOK(newFrameToken(frameToken, &frameIndex), "slGetNewFrameToken")) { frameToken = nullptr; return false; }
    return frameToken != nullptr;
}
sl::FrameToken *StreamlineContext::getCurrentFrameToken() { return frameToken; }
uint32_t StreamlineContext::getFrameIndex() { return frameIndex; }
std::string StreamlineContext::lastError() { return errorMessage; }
#else
namespace { const std::vector<std::string> noRequirements; }
bool StreamlineContext::init(const wchar_t *) { return false; }
bool StreamlineContext::onDeviceCreated(void *) { return false; }
void StreamlineContext::shutdown() {}
bool StreamlineContext::isInitialized() { return false; }
bool StreamlineContext::isAvailable() { return false; }
bool StreamlineContext::isReflexAvailable() { return false; }
bool StreamlineContext::isDlssGSupported() { return false; }
bool StreamlineContext::isDlssGLoaded() { return false; }
bool StreamlineContext::getDlssGCapabilities(uint32_t &maxFrames, uint32_t &minDimension) { maxFrames = minDimension = 0; return false; }
void StreamlineContext::invalidateRequirements(const std::string &) {}
void *StreamlineContext::getVkGetInstanceProcAddr() { return nullptr; }
void *StreamlineContext::getVkCreateDevice() { return nullptr; }
void *StreamlineContext::getVkGetDeviceProcAddr() { return nullptr; }
const std::vector<std::string> &StreamlineContext::getRequiredInstanceExtensions() { return noRequirements; }
const std::vector<std::string> &StreamlineContext::getRequiredDeviceExtensions() { return noRequirements; }
const std::vector<std::string> &StreamlineContext::getRequiredVulkan12Features() { return noRequirements; }
const std::vector<std::string> &StreamlineContext::getRequiredVulkan13Features() { return noRequirements; }
uint32_t StreamlineContext::requiredGraphicsQueues() { return 0; }
uint32_t StreamlineContext::requiredComputeQueues() { return 0; }
uint32_t StreamlineContext::requiredOpticalFlowQueues() { return 0; }
bool StreamlineContext::setReflexOptions(sl::ReflexMode, uint32_t) { return false; }
bool StreamlineContext::reflexSleep() { return false; }
bool StreamlineContext::getReflexState(sl::ReflexState &) { return false; }
bool StreamlineContext::pclSetMarker(sl::PCLMarker) { return false; }
bool StreamlineContext::setDlssGOptions(sl::DLSSGMode, uint32_t) { return false; }
bool StreamlineContext::getDlssGState(sl::DLSSGState &) { return false; }
bool StreamlineContext::setConstants(const sl::Constants &) { return false; }
bool StreamlineContext::tagResources(const sl::ResourceTag *, uint32_t, void *) { return false; }
bool StreamlineContext::clearResourceTags() { return true; }
bool StreamlineContext::setFeatureLoaded(sl::Feature, bool) { return false; }
bool StreamlineContext::advanceFrame() { return false; }
sl::FrameToken *StreamlineContext::getCurrentFrameToken() { return nullptr; }
uint32_t StreamlineContext::getFrameIndex() { return 0; }
std::string StreamlineContext::lastError() { return "Streamline disabled on this platform"; }
#endif

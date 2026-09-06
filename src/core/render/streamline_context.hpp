#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

// Dynamic Streamline 2.12 integration; no import-library or DLL dependency when
// runtime files are absent. All calls except the log callback are render-thread owned.
class StreamlineContext {
public:
    static bool init(const wchar_t *pluginPath); // before ANY Vulkan API call
    static bool onDeviceCreated(void *vkPhysicalDevice);
    static void shutdown(); // after all FG inputs drained, before destroying Vulkan
    static bool isAvailable();
    static bool isInitialized();
    static bool isReflexAvailable();
    static bool isDlssGSupported();
    static bool isDlssGLoaded();
    // True while Streamline owns the initialized NGX device; direct RR must not shut it down.
    static bool ownsNgxLifetime();
    static bool getDlssGCapabilities(uint32_t &maxGeneratedFrames, uint32_t &minimumDimension);
    static void invalidateRequirements(const std::string &reason);
    static void *getVkGetInstanceProcAddr();
    static void *getVkCreateDevice();
    static void *getVkGetDeviceProcAddr();
    static const std::vector<std::string> &getRequiredInstanceExtensions();
    static const std::vector<std::string> &getRequiredDeviceExtensions();
    static const std::vector<std::string> &getRequiredVulkan12Features();
    static const std::vector<std::string> &getRequiredVulkan13Features();
    static uint32_t requiredGraphicsQueues();
    static uint32_t requiredComputeQueues();
    static uint32_t requiredOpticalFlowQueues();
    static bool setReflexOptions(sl::ReflexMode mode, uint32_t frameLimitUs = 0);
    static bool reflexSleep();
    static bool getReflexState(sl::ReflexState &state);
    static bool pclSetMarker(sl::PCLMarker marker);
    static bool setDlssGOptions(sl::DLSSGMode mode, uint32_t generatedFrames = 1);
    static bool getDlssGState(sl::DLSSGState &state);
    static bool setConstants(const sl::Constants &constants);
    static bool tagResources(const sl::ResourceTag *tags, uint32_t count, void *commandBuffer = nullptr);
    static bool clearResourceTags();
    static bool setFeatureLoaded(sl::Feature feature, bool loaded);
    static bool advanceFrame(); // invalidates token on failure; once per real frame
    static sl::FrameToken *getCurrentFrameToken();
    static uint32_t getFrameIndex();
    static std::string lastError();
};

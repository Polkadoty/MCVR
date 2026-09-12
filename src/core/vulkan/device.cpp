#include "core/vulkan/device.hpp"
#include "core/render/streamline_context.hpp"

#include "core/render/modules/world/dlss/dlss_wrapper.hpp"
#include "core/render/modules/world/xess_upscaler/xess_wrapper.hpp"
#include "core/vulkan/instance.hpp"
#include "core/vulkan/physical_device.hpp"

#include <cstring>
#include <iostream>
#include <unordered_set>
#include <vector>

std::ostream &deviceCout() {
    return std::cout << "[Device] ";
}

std::ostream &deviceCerr() {
    return std::cerr << "[Device] ";
}

vk::Device::Device(std::shared_ptr<Instance> instance,
                   std::shared_ptr<Window> window,
                   std::shared_ptr<PhysicalDevice> physicalDevice)
    : instance_(instance), window_(window), physicalDevice_(physicalDevice) {
    // enabled device extensions
    std::vector<const char *> enabledExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
        // VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
    };
    std::vector<std::string> dlssRequiredExtensions;
    bool dlssRequirementQuerySuccess = false;
#ifdef MCVR_ENABLE_XESS
    std::vector<std::string> xessRequiredExtensions;
    bool xessRequirementQuerySuccess = false;
#endif

    std::vector<VkExtensionProperties> dlssExtensions;
    NVSDK_NGX_Result dlssResult =
        NgxContext::getDlssRRRequiredDeviceExtensions(instance_, physicalDevice_, dlssExtensions);
    if (NVSDK_NGX_FAILED(dlssResult)) {
        deviceCerr() << "dlss device extensions unavailable; skipping." << std::endl;
    } else {
        dlssRequirementQuerySuccess = true;
#ifdef DEBUG
        deviceCout() << "dlss instance extensions:" << std::endl;
#endif
        for (const auto &dlssExtension : dlssExtensions) {
            dlssRequiredExtensions.emplace_back(dlssExtension.extensionName);
            if (std::strcmp(dlssExtension.extensionName, "VK_EXT_buffer_device_address") == 0)
                continue; // already enabled using PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
#ifdef DEBUG
            deviceCout() << "\t" << dlssExtension.extensionName << std::endl;
#endif
            enabledExtensions.push_back(dlssExtension.extensionName);
        }
    }

#ifdef MCVR_ENABLE_XESS
    std::vector<const char *> xessExtensions;
    if (mcvr::XeSSWrapper::getRequiredDeviceExtensions(instance_->vkInstance(), physicalDevice_->vkPhysicalDevice(),
                                                       xessExtensions)) {
        xessRequirementQuerySuccess = true;
#    ifdef DEBUG
        deviceCout() << "xess device extensions:" << std::endl;
#    endif
        for (const char *extension : xessExtensions) {
            xessRequiredExtensions.emplace_back(extension);
#    ifdef DEBUG
            deviceCout() << "\t" << extension << std::endl;
#    endif
            enabledExtensions.push_back(extension);
        }
    } else {
        deviceCerr() << "xess device extensions unavailable; skipping." << std::endl;
    }
#endif

    for (const auto &extension : StreamlineContext::getRequiredDeviceExtensions()) {
        enabledExtensions.push_back(extension.c_str());
    }

    uint32_t deviceExtensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_->vkPhysicalDevice(), nullptr, &deviceExtensionCount, nullptr);
    std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice_->vkPhysicalDevice(), nullptr, &deviceExtensionCount,
                                         deviceExtensions.data());

    std::unordered_set<std::string> supportedExtensions;
    supportedExtensions.reserve(deviceExtensions.size());
    for (const auto &ext : deviceExtensions) { supportedExtensions.insert(ext.extensionName); }

    if (supportedExtensions.find(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME) != supportedExtensions.end()) {
        enabledExtensions.push_back(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME);
    }

    auto areRequiredExtensionsSupported = [&](const std::vector<std::string> &requiredExtensions) {
        for (const auto &requiredExtension : requiredExtensions) {
            if (requiredExtension == "VK_EXT_buffer_device_address") {
                // Covered by Vulkan 1.2 buffer device address feature path
                continue;
            }
            if (supportedExtensions.find(requiredExtension) == supportedExtensions.end()) { return false; }
        }
        return true;
    };

    if (!areRequiredExtensionsSupported(StreamlineContext::getRequiredDeviceExtensions())) {
        StreamlineContext::invalidateRequirements("Required Vulkan device extension unavailable");
    }

    dlssDeviceExtensionsCompatible_ = instance_->isDlssInstanceExtensionsCompatible() && dlssRequirementQuerySuccess &&
                                      areRequiredExtensionsSupported(dlssRequiredExtensions);
    if (!dlssDeviceExtensionsCompatible_) {
        deviceCerr() << "dlss device extension requirements are not fully satisfied." << std::endl;
    }

#ifdef MCVR_ENABLE_XESS
    xessDeviceExtensionsCompatible_ = instance_->isXessInstanceExtensionsCompatible() && xessRequirementQuerySuccess &&
                                      areRequiredExtensionsSupported(xessRequiredExtensions);
    if (!xessDeviceExtensionsCompatible_) {
        deviceCerr() << "xess device extension requirements are not fully satisfied." << std::endl;
    }
#endif

    std::vector<const char *> filteredExtensions;
    filteredExtensions.reserve(enabledExtensions.size());
    std::unordered_set<std::string> seenExtensions;
    for (const auto *ext : enabledExtensions) {
        if (supportedExtensions.find(ext) == supportedExtensions.end()) {
            deviceCerr() << "extension not supported, skipping: " << ext << std::endl;
            continue;
        }
        if (!seenExtensions.insert(ext).second) { continue; }
        filteredExtensions.push_back(ext);
    }

#ifdef DEBUG
    deviceCout() << "selected instance extensions:" << std::endl;
    for (int i = 0; i < filteredExtensions.size(); i++) { deviceCout() << "\t" << filteredExtensions[i] << std::endl; }
#endif

    // query supported features
    VkPhysicalDeviceMaintenance5Features supportedMaintenance5{};
    supportedMaintenance5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES;

    VkPhysicalDeviceCoherentMemoryFeaturesAMD supportedCoherentMemoryFeatures{};
    supportedCoherentMemoryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD;
    supportedCoherentMemoryFeatures.pNext = &supportedMaintenance5;

    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT supportedVertexInputDynamicState{};
    supportedVertexInputDynamicState.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT;
    supportedVertexInputDynamicState.pNext = &supportedCoherentMemoryFeatures;

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT supportedExtendedDynamicState3{};
    supportedExtendedDynamicState3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    supportedExtendedDynamicState3.pNext = &supportedVertexInputDynamicState;

    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT supportedExtendedDynamicState2{};
    supportedExtendedDynamicState2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    supportedExtendedDynamicState2.pNext = &supportedExtendedDynamicState3;

    VkPhysicalDeviceVulkan13Features supportedVulkan13{};
    supportedVulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    supportedVulkan13.pNext = &supportedExtendedDynamicState2;

    VkPhysicalDeviceVulkan11Features supportedVulkan11{};
    supportedVulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    supportedVulkan11.pNext = &supportedVulkan13;

    VkPhysicalDeviceVulkan12Features supportedVulkan12{};
    supportedVulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    supportedVulkan12.pNext = &supportedVulkan11;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAccelerationStructureFeatures{};
    supportedAccelerationStructureFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    supportedAccelerationStructureFeatures.pNext = &supportedVulkan12;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR supportedRayTracingFeatures{};
    supportedRayTracingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    supportedRayTracingFeatures.pNext = &supportedAccelerationStructureFeatures;

    VkPhysicalDeviceFeatures2 supportedFeatures2{};
    supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supportedFeatures2.pNext = &supportedRayTracingFeatures;

    vkGetPhysicalDeviceFeatures2(physicalDevice_->vkPhysicalDevice(), &supportedFeatures2);

    std::unordered_set<std::string> selectedExtensions;
    selectedExtensions.reserve(filteredExtensions.size());
    for (const auto *ext : filteredExtensions) { selectedExtensions.insert(ext); }
    auto hasExtension = [&](const char *name) { return selectedExtensions.find(name) != selectedExtensions.end(); };

    // enabling features
    VkPhysicalDeviceMaintenance5Features maintenance5Features{};
    maintenance5Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES;
    maintenance5Features.maintenance5 =
        hasExtension(VK_KHR_MAINTENANCE_5_EXTENSION_NAME) ? supportedMaintenance5.maintenance5 : VK_FALSE;

    VkPhysicalDeviceCoherentMemoryFeaturesAMD coherentMemoryFeatures{};
    coherentMemoryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD;
    coherentMemoryFeatures.pNext = &maintenance5Features;
    coherentMemoryFeatures.deviceCoherentMemory = hasExtension(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME) ?
                                                      supportedCoherentMemoryFeatures.deviceCoherentMemory :
                                                      VK_FALSE;

    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInputDynamicState{};
    vertexInputDynamicState.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT;
    vertexInputDynamicState.pNext = &coherentMemoryFeatures;
    vertexInputDynamicState.vertexInputDynamicState = hasExtension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) ?
                                                          supportedVertexInputDynamicState.vertexInputDynamicState :
                                                          VK_FALSE;

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT extendedDynamicState3{};
    extendedDynamicState3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    extendedDynamicState3.pNext = &vertexInputDynamicState;
    if (hasExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)) {
        extendedDynamicState3.extendedDynamicState3PolygonMode =
            supportedExtendedDynamicState3.extendedDynamicState3PolygonMode;
        extendedDynamicState3.extendedDynamicState3ColorBlendEnable =
            supportedExtendedDynamicState3.extendedDynamicState3ColorBlendEnable;
        extendedDynamicState3.extendedDynamicState3ColorBlendEquation =
            supportedExtendedDynamicState3.extendedDynamicState3ColorBlendEquation;
        extendedDynamicState3.extendedDynamicState3ColorWriteMask =
            supportedExtendedDynamicState3.extendedDynamicState3ColorWriteMask;
        extendedDynamicState3.extendedDynamicState3LogicOpEnable =
            supportedExtendedDynamicState3.extendedDynamicState3LogicOpEnable;
    }

    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extendedDynamicState2{};
    extendedDynamicState2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
    extendedDynamicState2.pNext = &extendedDynamicState3;
    if (hasExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME)) {
        extendedDynamicState2.extendedDynamicState2 = supportedExtendedDynamicState2.extendedDynamicState2;
        extendedDynamicState2.extendedDynamicState2LogicOp =
            supportedExtendedDynamicState2.extendedDynamicState2LogicOp ? VK_TRUE : VK_FALSE;

        // Store the flag for runtime checks
        extendedDynamicState2LogicOp_ = (extendedDynamicState2.extendedDynamicState2LogicOp == VK_TRUE);

#ifdef DEBUG
        std::cout << "[Device-Debug] extendedDynamicState2: "
                  << (extendedDynamicState2.extendedDynamicState2 ? "YES" : "NO") << std::endl;
        std::cout << "[Device-Debug] extendedDynamicState2LogicOp: "
                  << (extendedDynamicState2.extendedDynamicState2LogicOp ? "YES" : "NO") << std::endl;
#endif

        extendedDynamicState2.extendedDynamicState2PatchControlPoints = VK_FALSE;
    } else {
        std::cerr << "[Device-Debug] VK_EXT_extended_dynamic_state2 NOT FOUND!" << std::endl;
    }

    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13Features.pNext = &extendedDynamicState2;
    vulkan13Features.shaderDemoteToHelperInvocation = supportedVulkan13.shaderDemoteToHelperInvocation;
    vulkan13Features.synchronization2 = supportedVulkan13.synchronization2;

    VkPhysicalDeviceVulkan11Features vulkan11Features{};
    vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan11Features.pNext = &vulkan13Features;
    vulkan11Features.storageBuffer16BitAccess = supportedVulkan11.storageBuffer16BitAccess;

    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.pNext = &vulkan11Features;
    vulkan12Features.bufferDeviceAddress = supportedVulkan12.bufferDeviceAddress;
    vulkan12Features.descriptorBindingUpdateUnusedWhilePending =
        supportedVulkan12.descriptorBindingUpdateUnusedWhilePending;
    vulkan12Features.descriptorBindingPartiallyBound = supportedVulkan12.descriptorBindingPartiallyBound;
    vulkan12Features.descriptorIndexing = supportedVulkan12.descriptorIndexing;
    vulkan12Features.runtimeDescriptorArray = supportedVulkan12.runtimeDescriptorArray;
    vulkan12Features.descriptorBindingVariableDescriptorCount =
        supportedVulkan12.descriptorBindingVariableDescriptorCount;
    vulkan12Features.shaderSampledImageArrayNonUniformIndexing =
        supportedVulkan12.shaderSampledImageArrayNonUniformIndexing;
    vulkan12Features.descriptorBindingUniformBufferUpdateAfterBind =
        supportedVulkan12.descriptorBindingUniformBufferUpdateAfterBind;
    vulkan12Features.descriptorBindingSampledImageUpdateAfterBind =
        supportedVulkan12.descriptorBindingSampledImageUpdateAfterBind;
    vulkan12Features.descriptorBindingStorageImageUpdateAfterBind =
        supportedVulkan12.descriptorBindingStorageImageUpdateAfterBind;
    vulkan12Features.descriptorBindingStorageBufferUpdateAfterBind =
        supportedVulkan12.descriptorBindingStorageBufferUpdateAfterBind;
    vulkan12Features.timelineSemaphore = supportedVulkan12.timelineSemaphore;
    vulkan12Features.shaderFloat16 = supportedVulkan12.shaderFloat16;
    vulkan12Features.shaderBufferInt64Atomics = supportedVulkan12.shaderBufferInt64Atomics;
    vulkan12Features.shaderStorageBufferArrayNonUniformIndexing =
        supportedVulkan12.shaderStorageBufferArrayNonUniformIndexing;
    vulkan12Features.shaderStorageImageArrayNonUniformIndexing =
        supportedVulkan12.shaderStorageImageArrayNonUniformIndexing;
    vulkan12Features.shaderUniformBufferArrayNonUniformIndexing =
        supportedVulkan12.shaderUniformBufferArrayNonUniformIndexing;

    for (const auto &name : StreamlineContext::getRequiredVulkan12Features()) {
        bool recognized = false;
        if (name == "samplerMirrorClampToEdge") {
            recognized = true;
            if (supportedVulkan12.samplerMirrorClampToEdge) vulkan12Features.samplerMirrorClampToEdge = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "drawIndirectCount") {
            recognized = true;
            if (supportedVulkan12.drawIndirectCount) vulkan12Features.drawIndirectCount = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "storageBuffer8BitAccess") {
            recognized = true;
            if (supportedVulkan12.storageBuffer8BitAccess) vulkan12Features.storageBuffer8BitAccess = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "uniformAndStorageBuffer8BitAccess") {
            recognized = true;
            if (supportedVulkan12.uniformAndStorageBuffer8BitAccess) vulkan12Features.uniformAndStorageBuffer8BitAccess = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "storagePushConstant8") {
            recognized = true;
            if (supportedVulkan12.storagePushConstant8) vulkan12Features.storagePushConstant8 = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderBufferInt64Atomics") {
            recognized = true;
            if (supportedVulkan12.shaderBufferInt64Atomics) vulkan12Features.shaderBufferInt64Atomics = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderSharedInt64Atomics") {
            recognized = true;
            if (supportedVulkan12.shaderSharedInt64Atomics) vulkan12Features.shaderSharedInt64Atomics = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderFloat16") {
            recognized = true;
            if (supportedVulkan12.shaderFloat16) vulkan12Features.shaderFloat16 = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderInt8") {
            recognized = true;
            if (supportedVulkan12.shaderInt8) vulkan12Features.shaderInt8 = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorIndexing") {
            recognized = true;
            if (supportedVulkan12.descriptorIndexing) vulkan12Features.descriptorIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderInputAttachmentArrayDynamicIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderInputAttachmentArrayDynamicIndexing) vulkan12Features.shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderUniformTexelBufferArrayDynamicIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderUniformTexelBufferArrayDynamicIndexing) vulkan12Features.shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderStorageTexelBufferArrayDynamicIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderStorageTexelBufferArrayDynamicIndexing) vulkan12Features.shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderUniformBufferArrayNonUniformIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderUniformBufferArrayNonUniformIndexing) vulkan12Features.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderSampledImageArrayNonUniformIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderSampledImageArrayNonUniformIndexing) vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderStorageBufferArrayNonUniformIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderStorageBufferArrayNonUniformIndexing) vulkan12Features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderStorageImageArrayNonUniformIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderStorageImageArrayNonUniformIndexing) vulkan12Features.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderInputAttachmentArrayNonUniformIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderInputAttachmentArrayNonUniformIndexing) vulkan12Features.shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderUniformTexelBufferArrayNonUniformIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderUniformTexelBufferArrayNonUniformIndexing) vulkan12Features.shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderStorageTexelBufferArrayNonUniformIndexing") {
            recognized = true;
            if (supportedVulkan12.shaderStorageTexelBufferArrayNonUniformIndexing) vulkan12Features.shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingUniformBufferUpdateAfterBind") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingUniformBufferUpdateAfterBind) vulkan12Features.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingSampledImageUpdateAfterBind") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingSampledImageUpdateAfterBind) vulkan12Features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingStorageImageUpdateAfterBind") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingStorageImageUpdateAfterBind) vulkan12Features.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingStorageBufferUpdateAfterBind") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingStorageBufferUpdateAfterBind) vulkan12Features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingUniformTexelBufferUpdateAfterBind") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind) vulkan12Features.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingStorageTexelBufferUpdateAfterBind") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind) vulkan12Features.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingUpdateUnusedWhilePending") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingUpdateUnusedWhilePending) vulkan12Features.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingPartiallyBound") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingPartiallyBound) vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "descriptorBindingVariableDescriptorCount") {
            recognized = true;
            if (supportedVulkan12.descriptorBindingVariableDescriptorCount) vulkan12Features.descriptorBindingVariableDescriptorCount = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "runtimeDescriptorArray") {
            recognized = true;
            if (supportedVulkan12.runtimeDescriptorArray) vulkan12Features.runtimeDescriptorArray = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "samplerFilterMinmax") {
            recognized = true;
            if (supportedVulkan12.samplerFilterMinmax) vulkan12Features.samplerFilterMinmax = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "scalarBlockLayout") {
            recognized = true;
            if (supportedVulkan12.scalarBlockLayout) vulkan12Features.scalarBlockLayout = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "imagelessFramebuffer") {
            recognized = true;
            if (supportedVulkan12.imagelessFramebuffer) vulkan12Features.imagelessFramebuffer = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "uniformBufferStandardLayout") {
            recognized = true;
            if (supportedVulkan12.uniformBufferStandardLayout) vulkan12Features.uniformBufferStandardLayout = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderSubgroupExtendedTypes") {
            recognized = true;
            if (supportedVulkan12.shaderSubgroupExtendedTypes) vulkan12Features.shaderSubgroupExtendedTypes = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "separateDepthStencilLayouts") {
            recognized = true;
            if (supportedVulkan12.separateDepthStencilLayouts) vulkan12Features.separateDepthStencilLayouts = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "hostQueryReset") {
            recognized = true;
            if (supportedVulkan12.hostQueryReset) vulkan12Features.hostQueryReset = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "timelineSemaphore") {
            recognized = true;
            if (supportedVulkan12.timelineSemaphore) vulkan12Features.timelineSemaphore = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "bufferDeviceAddress") {
            recognized = true;
            if (supportedVulkan12.bufferDeviceAddress) vulkan12Features.bufferDeviceAddress = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "bufferDeviceAddressCaptureReplay") {
            recognized = true;
            if (supportedVulkan12.bufferDeviceAddressCaptureReplay) vulkan12Features.bufferDeviceAddressCaptureReplay = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "bufferDeviceAddressMultiDevice") {
            recognized = true;
            if (supportedVulkan12.bufferDeviceAddressMultiDevice) vulkan12Features.bufferDeviceAddressMultiDevice = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "vulkanMemoryModel") {
            recognized = true;
            if (supportedVulkan12.vulkanMemoryModel) vulkan12Features.vulkanMemoryModel = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "vulkanMemoryModelDeviceScope") {
            recognized = true;
            if (supportedVulkan12.vulkanMemoryModelDeviceScope) vulkan12Features.vulkanMemoryModelDeviceScope = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "vulkanMemoryModelAvailabilityVisibilityChains") {
            recognized = true;
            if (supportedVulkan12.vulkanMemoryModelAvailabilityVisibilityChains) vulkan12Features.vulkanMemoryModelAvailabilityVisibilityChains = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderOutputViewportIndex") {
            recognized = true;
            if (supportedVulkan12.shaderOutputViewportIndex) vulkan12Features.shaderOutputViewportIndex = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "shaderOutputLayer") {
            recognized = true;
            if (supportedVulkan12.shaderOutputLayer) vulkan12Features.shaderOutputLayer = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (name == "subgroupBroadcastDynamicId") {
            recognized = true;
            if (supportedVulkan12.subgroupBroadcastDynamicId) vulkan12Features.subgroupBroadcastDynamicId = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 12 feature: " + name);
        }
        if (!recognized) StreamlineContext::invalidateRequirements("Unknown Vulkan 12 feature: " + name);
    }
    for (const auto &name : StreamlineContext::getRequiredVulkan13Features()) {
        bool recognized = false;
        if (name == "robustImageAccess") {
            recognized = true;
            if (supportedVulkan13.robustImageAccess) vulkan13Features.robustImageAccess = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "inlineUniformBlock") {
            recognized = true;
            if (supportedVulkan13.inlineUniformBlock) vulkan13Features.inlineUniformBlock = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "descriptorBindingInlineUniformBlockUpdateAfterBind") {
            recognized = true;
            if (supportedVulkan13.descriptorBindingInlineUniformBlockUpdateAfterBind) vulkan13Features.descriptorBindingInlineUniformBlockUpdateAfterBind = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "pipelineCreationCacheControl") {
            recognized = true;
            if (supportedVulkan13.pipelineCreationCacheControl) vulkan13Features.pipelineCreationCacheControl = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "privateData") {
            recognized = true;
            if (supportedVulkan13.privateData) vulkan13Features.privateData = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "shaderDemoteToHelperInvocation") {
            recognized = true;
            if (supportedVulkan13.shaderDemoteToHelperInvocation) vulkan13Features.shaderDemoteToHelperInvocation = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "shaderTerminateInvocation") {
            recognized = true;
            if (supportedVulkan13.shaderTerminateInvocation) vulkan13Features.shaderTerminateInvocation = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "subgroupSizeControl") {
            recognized = true;
            if (supportedVulkan13.subgroupSizeControl) vulkan13Features.subgroupSizeControl = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "computeFullSubgroups") {
            recognized = true;
            if (supportedVulkan13.computeFullSubgroups) vulkan13Features.computeFullSubgroups = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "synchronization2") {
            recognized = true;
            if (supportedVulkan13.synchronization2) vulkan13Features.synchronization2 = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "textureCompressionASTC_HDR") {
            recognized = true;
            if (supportedVulkan13.textureCompressionASTC_HDR) vulkan13Features.textureCompressionASTC_HDR = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "shaderZeroInitializeWorkgroupMemory") {
            recognized = true;
            if (supportedVulkan13.shaderZeroInitializeWorkgroupMemory) vulkan13Features.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "dynamicRendering") {
            recognized = true;
            if (supportedVulkan13.dynamicRendering) vulkan13Features.dynamicRendering = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "shaderIntegerDotProduct") {
            recognized = true;
            if (supportedVulkan13.shaderIntegerDotProduct) vulkan13Features.shaderIntegerDotProduct = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (name == "maintenance4") {
            recognized = true;
            if (supportedVulkan13.maintenance4) vulkan13Features.maintenance4 = VK_TRUE;
            else StreamlineContext::invalidateRequirements("Unsupported Vulkan 13 feature: " + name);
        }
        if (!recognized) StreamlineContext::invalidateRequirements("Unknown Vulkan 13 feature: " + name);
    }

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures = {};
    accelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    accelerationStructureFeatures.pNext = &vulkan12Features;
    if (hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
        accelerationStructureFeatures.accelerationStructure =
            supportedAccelerationStructureFeatures.accelerationStructure;
        accelerationStructureFeatures.descriptorBindingAccelerationStructureUpdateAfterBind =
            supportedAccelerationStructureFeatures.descriptorBindingAccelerationStructureUpdateAfterBind;
    }

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingFeatures = {};
    rayTracingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rayTracingFeatures.pNext = &accelerationStructureFeatures;
    if (hasExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
        rayTracingFeatures.rayTracingPipeline = supportedRayTracingFeatures.rayTracingPipeline;
    }

    VkPhysicalDeviceFeatures features = {};
    features.independentBlend = supportedFeatures2.features.independentBlend;
    features.shaderClipDistance = supportedFeatures2.features.shaderClipDistance;
    features.shaderCullDistance = supportedFeatures2.features.shaderCullDistance;
    features.logicOp = supportedFeatures2.features.logicOp;
    features.fillModeNonSolid = supportedFeatures2.features.fillModeNonSolid;
    features.depthBiasClamp = supportedFeatures2.features.depthBiasClamp;
    features.shaderInt64 = supportedFeatures2.features.shaderInt64;
    features.shaderFloat64 = supportedFeatures2.features.shaderFloat64;
    features.shaderInt16 = supportedFeatures2.features.shaderInt16;
    features.shaderStorageImageReadWithoutFormat = supportedFeatures2.features.shaderStorageImageReadWithoutFormat;
    features.shaderStorageImageWriteWithoutFormat = supportedFeatures2.features.shaderStorageImageWriteWithoutFormat;

    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &rayTracingFeatures;
    features2.features = features;

#ifdef MCVR_ENABLE_XESS
    if (xessDeviceExtensionsCompatible_) {
        void *featureChain = &features2;
        if (!mcvr::XeSSWrapper::getRequiredDeviceFeatures(instance_->vkInstance(), physicalDevice_->vkPhysicalDevice(),
                                                          &featureChain)) {
            xessDeviceExtensionsCompatible_ = false;
            deviceCerr() << "xess device feature requirements are not fully satisfied." << std::endl;
        }
    }
#endif

    auto slCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(StreamlineContext::getVkCreateDevice());
    PFN_vkCreateDevice createDeviceFn = slCreateDevice ? slCreateDevice : vkCreateDevice;

    // create logical device
    VkDeviceCreateInfo deviceCreateInfo = {};
    if (physicalDevice_->mainQueueIndex() == physicalDevice_->secondaryQueueIndex()) {
        std::vector<float> queuePriorities{{1.0, 0.0}};
        VkDeviceQueueCreateInfo queueCreateInfo = {};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = physicalDevice_->mainQueueIndex();
        queueCreateInfo.queueCount = 2;
        queueCreateInfo.pQueuePriorities = queuePriorities.data();

        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
        deviceCreateInfo.queueCreateInfoCount = 1;
        deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(filteredExtensions.size());
        deviceCreateInfo.ppEnabledExtensionNames = filteredExtensions.data();
        deviceCreateInfo.pNext = &features2;
        deviceCreateInfo.pEnabledFeatures = nullptr;

        if (createDeviceFn(physicalDevice_->vkPhysicalDevice(), &deviceCreateInfo, nullptr, &device_) != VK_SUCCESS) {
            deviceCerr() << "Failed to create logical device!" << std::endl;
            exit(EXIT_FAILURE);
        }
    } else {
        std::vector<float> queuePriorities{{1.0, 0.0}};
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos(2);
        queueCreateInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfos[0].queueFamilyIndex = physicalDevice_->mainQueueIndex();
        queueCreateInfos[0].queueCount = 1;
        queueCreateInfos[0].pQueuePriorities = &queuePriorities[0];

        queueCreateInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfos[1].queueFamilyIndex = physicalDevice_->secondaryQueueIndex();
        queueCreateInfos[1].queueCount = 1;
        queueCreateInfos[1].pQueuePriorities = &queuePriorities[1];

        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
        deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(filteredExtensions.size());
        deviceCreateInfo.ppEnabledExtensionNames = filteredExtensions.data();
        deviceCreateInfo.pNext = &features2;
        deviceCreateInfo.pEnabledFeatures = nullptr;

        if (createDeviceFn(physicalDevice_->vkPhysicalDevice(), &deviceCreateInfo, nullptr, &device_) != VK_SUCCESS) {
            deviceCerr() << "Failed to create logical device!" << std::endl;
            exit(EXIT_FAILURE);
        }
    }

    volkLoadDevice(device_);
    // Keep all device entry points behind the interposer, including command hooks.
    // Loading through its GDPA avoids a mixture of raw and wrapped device handles.
    auto slGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(StreamlineContext::getVkGetDeviceProcAddr());
    if (slGdpa) {
        vkGetDeviceProcAddr = slGdpa;
        volkLoadDevice(device_);
    }
    StreamlineContext::onDeviceCreated(physicalDevice_->vkPhysicalDevice());

#ifdef DEBUG
    deviceCout() << "Logical device created successfully!" << std::endl;
#endif

    vkGetDeviceQueue(device_, physicalDevice_->mainQueueIndex(), 0, &mainQueue_);
    vkGetDeviceQueue(device_, physicalDevice_->secondaryQueueIndex(),
                     physicalDevice_->mainQueueIndex() == physicalDevice_->secondaryQueueIndex() ? 1 : 0,
                     &secondaryQueue_);
}

vk::Device::~Device() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

#ifdef DEBUG
    deviceCout() << "device deconstructed" << std::endl;
#endif
}

VkDevice &vk::Device::vkDevice() {
    return device_;
}

VkQueue &vk::Device::mainVkQueue() {
    return mainQueue_;
}

VkQueue &vk::Device::secondaryQueue() {
    return secondaryQueue_;
}

bool vk::Device::hasExtendedDynamicState2LogicOp() const {
    return extendedDynamicState2LogicOp_;
}

bool vk::Device::isDlssDeviceExtensionsCompatible() const {
    return dlssDeviceExtensionsCompatible_;
}

bool vk::Device::isXessDeviceExtensionsCompatible() const {
    return xessDeviceExtensionsCompatible_;
}

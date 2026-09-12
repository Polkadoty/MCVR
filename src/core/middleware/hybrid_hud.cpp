// SPDX-License-Identifier: GPL-3.0-only
// Render-thread HUD transport. GL -> external image -> registered Vulkan texture.
// No CPU image readback in publish(); readback exists only in verifyHudPixel().
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <GL/gl.h>
#include <jni.h>
#include "core/render/renderer.hpp"
#include "core/render/render_framework.hpp"
#include "core/render/textures.hpp"
#include "texture_proxy_errors.hpp"
#include <array>
#include <memory>
#include <stdexcept>

namespace {
void check(VkResult result) { if (result != VK_SUCCESS) throw std::runtime_error("HUD Vulkan result " + std::to_string(result)); }
template<class T> T proc(const char* name) {
    auto p = wglGetProcAddress(name);
    if (!p || reinterpret_cast<uintptr_t>(p) <= 3 || reinterpret_cast<intptr_t>(p) == -1)
        throw std::runtime_error(std::string("Missing HUD GL entry point: ") + name);
    return reinterpret_cast<T>(p);
}
#define GP(name, result, ...) const auto name = proc<result(APIENTRY*)(__VA_ARGS__)>(#name)
void glCheck() { auto e = glGetError(); if (e) throw std::runtime_error("HUD GL error " + std::to_string(e)); }
uint32_t memoryType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(gpu, &p);
    for (uint32_t i=0;i<p.memoryTypeCount;++i)
        if ((bits & (1u<<i)) && (p.memoryTypes[i].propertyFlags & flags)==flags) return i;
    throw std::runtime_error("HUD memory type unavailable");
}
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
             uint32_t src, uint32_t dst, VkAccessFlags read, VkAccessFlags write) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image=image; b.oldLayout=from; b.newLayout=to; b.srcQueueFamilyIndex=src; b.dstQueueFamilyIndex=dst;
    b.srcAccessMask=read; b.dstAccessMask=write; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
struct GLBindings {
    GLint read=0,draw=0,texture=0; GLboolean scissor=GL_FALSE;
    GLBindings() { glGetIntegerv(0x8CAA,&read); glGetIntegerv(0x8CA6,&draw); glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture); scissor=glIsEnabled(GL_SCISSOR_TEST); }
    ~GLBindings() {
        GP(glBindFramebuffer,void,GLenum,GLuint);
        glBindFramebuffer(0x8CA8,read); glBindFramebuffer(0x8CA9,draw); glBindTexture(GL_TEXTURE_2D,texture);
        if(scissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
    }
};
struct HudTransport {
    VkDevice device=VK_NULL_HANDLE; VkQueue queue=VK_NULL_HANDLE; uint32_t family=0,width=0,height=0;
    HGLRC context=nullptr;
    VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    std::array<VkCommandBuffer,3> commands{}; std::array<VkFence,3> fences{}; std::array<bool,3> pending{};
    std::array<VkSemaphore,2> sem{}; std::array<GLuint,2> glSem{};
    GLuint glMemory=0,texture=0,fbo=0; size_t cursor=0;
    HANDLE importHandle=nullptr;
    ~HudTransport() {
        // Only resize/shutdown (or initialization failure) waits for all users.
        if(context && context==wglGetCurrentContext())glFinish();
        if(device)vkDeviceWaitIdle(device);
        if(importHandle)CloseHandle(importHandle);
        if(context && context==wglGetCurrentContext()) {
            GP(glDeleteFramebuffers,void,GLsizei,const GLuint*);
            GP(glDeleteMemoryObjectsEXT,void,GLsizei,const GLuint*);
            GP(glDeleteSemaphoresEXT,void,GLsizei,const GLuint*);
            if(fbo)glDeleteFramebuffers(1,&fbo); if(texture)glDeleteTextures(1,&texture);
            if(glMemory)glDeleteMemoryObjectsEXT(1,&glMemory);
            for(auto s:glSem)if(s)glDeleteSemaphoresEXT(1,&s);
        }
        if(device) {
            for(auto f:fences)if(f)vkDestroyFence(device,f,nullptr);
            for(auto s:sem)if(s)vkDestroySemaphore(device,s,nullptr);
            if(pool)vkDestroyCommandPool(device,pool,nullptr);
            if(image)vkDestroyImage(device,image,nullptr);
            if(memory)vkFreeMemory(device,memory,nullptr);
        }
    }
    VkCommandBuffer begin() {
        if(pending[cursor]) { check(vkWaitForFences(device,1,&fences[cursor],VK_TRUE,10000000000ull)); pending[cursor]=false; }
        check(vkResetFences(device,1,&fences[cursor])); auto cmd=commands[cursor];
        check(vkResetCommandBuffer(cmd,0)); VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; check(vkBeginCommandBuffer(cmd,&bi)); return cmd;
    }
    void submit(VkCommandBuffer cmd, bool waitGL) {
        check(vkEndCommandBuffer(cmd)); VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO}; info.commandBufferCount=1;info.pCommandBuffers=&cmd;
        info.signalSemaphoreCount=1;info.pSignalSemaphores=&sem[0];
        if(waitGL){info.waitSemaphoreCount=1;info.pWaitSemaphores=&sem[1];info.pWaitDstStageMask=&stage;}
        check(vkQueueSubmit(queue,1,&info,fences[cursor]));pending[cursor]=true;cursor=(cursor+1)%commands.size();
    }
    void initialize(uint32_t w,uint32_t h) {
        context=wglGetCurrentContext(); if(!context)throw std::runtime_error("HUD requires current GL context");
        width=w;height=h; auto fw=Renderer::instance().framework();
        device=fw->device()->vkDevice();queue=fw->device()->mainVkQueue();family=fw->physicalDevice()->mainQueueIndex();
        auto gpu=fw->physicalDevice()->vkPhysicalDevice();
        GLBindings bindings;
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.pNext=&ext;ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent={w,h,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
        ii.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        check(vkCreateImage(device,&ii,nullptr,&image));VkMemoryRequirements mr;vkGetImageMemoryRequirements(device,image,&mr);
        VkExportMemoryWin32HandleInfoKHR win{VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};win.dwAccess=GENERIC_ALL;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=image;dedicated.pNext=&win;
        VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};ex.handleTypes=ext.handleTypes;ex.pNext=&dedicated;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.pNext=&ex;ai.allocationSize=mr.size;ai.memoryTypeIndex=memoryType(gpu,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device,&ai,nullptr,&memory));check(vkBindImageMemory(device,image,memory,0));
        auto exportMemory=reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device,"vkGetMemoryWin32HandleKHR"));
        auto exportSem=reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(device,"vkGetSemaphoreWin32HandleKHR"));
        if(!exportMemory||!exportSem)throw std::runtime_error("HUD Vulkan external handle functions unavailable");
        VkMemoryGetWin32HandleInfoKHR mh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};mh.memory=memory;mh.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        check(exportMemory(device,&mh,&importHandle));
        GP(glCreateMemoryObjectsEXT,void,GLsizei,GLuint*);GP(glMemoryObjectParameterivEXT,void,GLuint,GLenum,const GLint*);
        GP(glImportMemoryWin32HandleEXT,void,GLuint,uint64_t,GLenum,void*);GP(glTexStorageMem2DEXT,void,GLenum,GLsizei,GLenum,GLsizei,GLsizei,GLuint,uint64_t);
        GP(glGenFramebuffers,void,GLsizei,GLuint*);GP(glBindFramebuffer,void,GLenum,GLuint);GP(glFramebufferTexture2D,void,GLenum,GLenum,GLenum,GLuint,GLint);
        GP(glCheckFramebufferStatus,GLenum,GLenum);
        glCreateMemoryObjectsEXT(1,&glMemory);GLint yes=GL_TRUE;glMemoryObjectParameterivEXT(glMemory,0x9581,&yes);
        glImportMemoryWin32HandleEXT(glMemory,mr.size,0x9587,importHandle);glFinish();glCheck();CloseHandle(importHandle);importHandle=nullptr;
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexParameteri(GL_TEXTURE_2D,0x9580,0x9584);
        glTexStorageMem2DEXT(GL_TEXTURE_2D,1,GL_RGBA8,w,h,glMemory,0);
        glGenFramebuffers(1,&fbo);glBindFramebuffer(0x8D40,fbo);glFramebufferTexture2D(0x8D40,0x8CE0,GL_TEXTURE_2D,texture,0);
        if(glCheckFramebufferStatus(0x8D40)!=0x8CD5)throw std::runtime_error("HUD shared FBO incomplete");
        GP(glGenSemaphoresEXT,void,GLsizei,GLuint*);GP(glImportSemaphoreWin32HandleEXT,void,GLuint,GLenum,void*);
        glGenSemaphoresEXT(2,glSem.data());
        for(size_t i=0;i<2;i++) {
            VkExportSemaphoreCreateInfo es{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};es.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&es;check(vkCreateSemaphore(device,&si,nullptr,&sem[i]));
            VkSemaphoreGetWin32HandleInfoKHR sh{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};sh.semaphore=sem[i];sh.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            check(exportSem(device,&sh,&importHandle));glImportSemaphoreWin32HandleEXT(glSem[i],0x9587,importHandle);glFinish();glCheck();CloseHandle(importHandle);importHandle=nullptr;
        }
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device,&pi,nullptr,&pool));VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ci.commandPool=pool;ci.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ci.commandBufferCount=static_cast<uint32_t>(commands.size());check(vkAllocateCommandBuffers(device,&ci,commands.data()));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};for(auto& f:fences)check(vkCreateFence(device,&fi,nullptr,&f));
        auto cmd=begin();barrier(cmd,image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,family,VK_QUEUE_FAMILY_EXTERNAL,0,0);submit(cmd,false);glCheck();
    }
    void publish(GLuint source,const std::shared_ptr<vk::DeviceLocalImage>& destination) {
        if(context!=wglGetCurrentContext())throw std::runtime_error("HUD context changed");
        if(!destination||destination->width()!=width||destination->height()!=height||destination->vkFormat()!=VK_FORMAT_R8G8B8A8_UNORM)
            throw std::runtime_error("HUD destination image mismatch");
        // Obtain a reusable command before enqueuing a new GL semaphore signal.
        auto cmd=begin();GLBindings bindings;
        GP(glWaitSemaphoreEXT,void,GLuint,GLuint,const GLuint*,GLuint,const GLuint*,const GLenum*);
        GP(glSignalSemaphoreEXT,void,GLuint,GLuint,const GLuint*,GLuint,const GLuint*,const GLenum*);
        GP(glBindFramebuffer,void,GLenum,GLuint);GP(glBlitFramebuffer,void,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
        constexpr GLenum general=0x958D;
        glWaitSemaphoreEXT(glSem[0],0,nullptr,1,&texture,&general);
        glBindFramebuffer(0x8CA8,source);glBindFramebuffer(0x8CA9,fbo);glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0,0,width,height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glSignalSemaphoreEXT(glSem[1],0,nullptr,1,&texture,&general);glFlush();
        barrier(cmd,image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_QUEUE_FAMILY_EXTERNAL,family,0,VK_ACCESS_TRANSFER_READ_BIT);
        auto dst=destination->vkImage();auto old=destination->imageLayout();
        barrier(cmd,dst,old,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
                old==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.extent={width,height,1};
        vkCmdCopyImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        barrier(cmd,dst,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        barrier(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,family,VK_QUEUE_FAMILY_EXTERNAL,VK_ACCESS_TRANSFER_READ_BIT,0);
        submit(cmd,true);destination->imageLayout()=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;glCheck();
    }
};
// Explicitly reset before the native renderer or current GL context is destroyed.
std::unique_ptr<HudTransport> hud;
}

extern "C" JNIEXPORT void JNICALL Java_com_radiance_hybrid_NativeInterop_publishHud(JNIEnv* env,jclass,jint source,jint width,jint height,jint destination) {
    texture_proxy::guardUpdate(env,[&] {
        if(width<=0||height<=0||width>16384||height>16384)throw std::runtime_error("Invalid HUD dimensions");
        auto fw=Renderer::instance().framework();auto textures=Renderer::instance().textures();
        std::scoped_lock lock(textures->mtx_,fw->recreateMtx());
        auto image=textures->texture(destination);
        if(!hud||hud->width!=width||hud->height!=height) {
            hud.reset();auto next=std::make_unique<HudTransport>();next->initialize(width,height);hud=std::move(next);
        }
        hud->publish(source,image);
    });
}
extern "C" JNIEXPORT void JNICALL Java_com_radiance_hybrid_NativeInterop_closeHud(JNIEnv* env,jclass) {
    texture_proxy::guardUpdate(env,[&] {hud.reset();});
}
extern "C" JNIEXPORT jint JNICALL Java_com_radiance_hybrid_NativeInterop_readHudPixel(JNIEnv* env,jclass,jint texture,jint x,jint y) {
    jint result=0;
    texture_proxy::guardUpdate(env,[&] {
        auto fw=Renderer::instance().framework();auto textures=Renderer::instance().textures();
        std::scoped_lock lock(textures->mtx_,fw->recreateMtx());
        auto image=textures->texture(texture);auto device=fw->device()->vkDevice();
        if(!image||x<0||y<0||x>=image->width()||y>=image->height())throw std::runtime_error("HUD diagnostic coordinate invalid");
        struct Readback {
            VkDevice device;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;void* mapped=nullptr;
            ~Readback(){vkDeviceWaitIdle(device);if(mapped)vkUnmapMemory(device,memory);if(fence)vkDestroyFence(device,fence,nullptr);if(pool)vkDestroyCommandPool(device,pool,nullptr);if(buffer)vkDestroyBuffer(device,buffer,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);}
        } r{device};
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=4;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;check(vkCreateBuffer(device,&bi,nullptr,&r.buffer));
        VkMemoryRequirements mr;vkGetBufferMemoryRequirements(device,r.buffer,&mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;
        ai.memoryTypeIndex=memoryType(fw->physicalDevice()->vkPhysicalDevice(),mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device,&ai,nullptr,&r.memory));check(vkBindBufferMemory(device,r.buffer,r.memory,0));check(vkMapMemory(device,r.memory,0,VK_WHOLE_SIZE,0,&r.mapped));
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=fw->physicalDevice()->mainQueueIndex();check(vkCreateCommandPool(device,&pi,nullptr,&r.pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=r.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        VkCommandBuffer cmd;check(vkAllocateCommandBuffers(device,&ca,&cmd));VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(cmd,&cb));
        auto old=image->imageLayout();barrier(cmd,image->vkImage(),old,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageOffset={x,y,0};region.imageExtent={1,1,1};
        vkCmdCopyImageToBuffer(cmd,image->vkImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffer,1,&region);
        barrier(cmd,image->vkImage(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,old,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        check(vkEndCommandBuffer(cmd));VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(device,&fi,nullptr,&r.fence));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;check(vkQueueSubmit(fw->device()->mainVkQueue(),1,&si,r.fence));
        check(vkWaitForFences(device,1,&r.fence,VK_TRUE,10000000000ull));
        auto p=static_cast<uint8_t*>(r.mapped);result=(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
    });return result;
}

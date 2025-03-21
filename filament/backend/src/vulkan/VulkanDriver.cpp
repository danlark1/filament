/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vulkan/VulkanDriver.h"

#include "CommandStreamDispatcher.h"
#include "DataReshaper.h"
#include "VulkanBuffer.h"
#include "VulkanCommands.h"
#include "VulkanDriverFactory.h"
#include "VulkanHandles.h"
#include "VulkanImageUtility.h"
#include "VulkanMemory.h"

#include <backend/platforms/VulkanPlatform.h>

#include <utils/CString.h>
#include <utils/FixedCapacityVector.h>
#include <utils/Panic.h>

#ifndef NDEBUG
#include <set>
#endif

#if FILAMENT_VULKAN_VERBOSE
#include <stack>
static std::stack<std::string> renderPassMarkers;
#endif

using namespace bluevk;

using utils::FixedCapacityVector;

// Vulkan functions often immediately dereference pointers, so it's fine to pass in a pointer
// to a stack-allocated variable.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-stack-address"
#pragma clang diagnostic ignored "-Wunused-parameter"

namespace filament::backend {

namespace {

VmaAllocator createAllocator(VkInstance instance, VkPhysicalDevice physicalDevice,
        VkDevice device) {
    VmaAllocator allocator;
    VmaVulkanFunctions const funcs {
#if VMA_DYNAMIC_VULKAN_FUNCTIONS
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
#else
        .vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties,
        .vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties,
        .vkAllocateMemory = vkAllocateMemory,
        .vkFreeMemory = vkFreeMemory,
        .vkMapMemory = vkMapMemory,
        .vkUnmapMemory = vkUnmapMemory,
        .vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges,
        .vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges,
        .vkBindBufferMemory = vkBindBufferMemory,
        .vkBindImageMemory = vkBindImageMemory,
        .vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements,
        .vkGetImageMemoryRequirements = vkGetImageMemoryRequirements,
        .vkCreateBuffer = vkCreateBuffer,
        .vkDestroyBuffer = vkDestroyBuffer,
        .vkCreateImage = vkCreateImage,
        .vkDestroyImage = vkDestroyImage,
        .vkCmdCopyBuffer = vkCmdCopyBuffer,
        .vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2KHR,
        .vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2KHR
#endif
    };
    VmaAllocatorCreateInfo const allocatorInfo {
        .physicalDevice = physicalDevice,
        .device = device,
        .pVulkanFunctions = &funcs,
        .instance = instance,
    };
    vmaCreateAllocator(&allocatorInfo, &allocator);
    return allocator;
}

std::shared_ptr<VulkanTexture> createEmptyTexture(VkDevice device, VkPhysicalDevice physicalDevice,
        VulkanContext const& context, VmaAllocator allocator,
        std::shared_ptr<VulkanCommands> commands, VulkanStagePool& stagePool) {
    std::shared_ptr<VulkanTexture> emptyTexture = std::make_shared<VulkanTexture>(device,
            physicalDevice, context, allocator, commands, SamplerType::SAMPLER_2D, 1,
            TextureFormat::RGBA8, 1, 1, 1, 1,
            TextureUsage::DEFAULT | TextureUsage::COLOR_ATTACHMENT | TextureUsage::SUBPASS_INPUT,
            stagePool);
    uint32_t black = 0;
    PixelBufferDescriptor pbd(&black, 4, PixelDataFormat::RGBA, PixelDataType::UBYTE);
    emptyTexture->updateImage(pbd, 1, 1, 1, 0, 0, 0, 0);
    return emptyTexture;
}

#if VK_ENABLE_VALIDATION
VKAPI_ATTR VkBool32 VKAPI_CALL debugReportCallback(VkDebugReportFlagsEXT flags,
        VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location,
        int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData) {
    if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
        utils::slog.e << "VULKAN ERROR: (" << pLayerPrefix << ") " << pMessage << utils::io::endl;
    } else {
        utils::slog.w << "VULKAN WARNING: (" << pLayerPrefix << ") " << pMessage << utils::io::endl;
    }
    utils::slog.e << utils::io::endl;
    return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* cbdata,
        void* pUserData) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        utils::slog.e << "VULKAN ERROR: (" << cbdata->pMessageIdName << ") " << cbdata->pMessage
                      << utils::io::endl;
    } else {
        // TODO: emit best practices warnings about aggressive pipeline barriers.
        if (strstr(cbdata->pMessage, "ALL_GRAPHICS_BIT") ||
                strstr(cbdata->pMessage, "ALL_COMMANDS_BIT")) {
            return VK_FALSE;
        }
        utils::slog.w << "VULKAN WARNING: (" << cbdata->pMessageIdName << ") " << cbdata->pMessage
                      << utils::io::endl;
    }
    utils::slog.e << utils::io::endl;
    return VK_FALSE;
}
#endif// VK_ENABLE_VALIDATION

} // anonymous namespace

using ImgUtil = VulkanImageUtility;

Dispatcher VulkanDriver::getDispatcher() const noexcept {
    return ConcreteDispatcher<VulkanDriver>::make();
}

VulkanDriver::VulkanDriver(VulkanPlatform* platform, VulkanContext const& context,
        Platform::DriverConfig const& driverConfig) noexcept
    : mPlatform(platform),
      mAllocator(createAllocator(mPlatform->getInstance(), mPlatform->getPhysicalDevice(),
              mPlatform->getDevice())),
      mContext(context),
      mHandleAllocator("Handles", driverConfig.handleArenaSize),
      mBlitter(mStagePool, mPipelineCache, mFramebufferCache, mSamplerCache) {

#if VK_ENABLE_VALIDATION
    UTILS_UNUSED const PFN_vkCreateDebugReportCallbackEXT createDebugReportCallback
            = vkCreateDebugReportCallbackEXT;
    VkResult result;
    if (mContext.isDebugUtilsSupported()) {
        VkDebugUtilsMessengerCreateInfoEXT const createInfo = {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .pNext = nullptr,
                .flags = 0,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                   | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
                .pfnUserCallback = debugUtilsCallback,
        };
        result = vkCreateDebugUtilsMessengerEXT(mPlatform->getInstance(), &createInfo, VKALLOC,
                &mDebugMessenger);
        ASSERT_POSTCONDITION(result == VK_SUCCESS, "Unable to create Vulkan debug messenger.");
    } else if (createDebugReportCallback) {
        VkDebugReportCallbackCreateInfoEXT const cbinfo = {
                .sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
                .flags = VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT,
                .pfnCallback = debugReportCallback,
        };
        result = createDebugReportCallback(mPlatform->getInstance(), &cbinfo, VKALLOC,
                &mDebugCallback);
        ASSERT_POSTCONDITION(result == VK_SUCCESS, "Unable to create Vulkan debug callback.");
    }
#endif
    mTimestamps = std::make_unique<VulkanTimestamps>(mPlatform->getDevice());
    mCommands = std::make_shared<VulkanCommands>(mPlatform->getDevice(),
            mPlatform->getGraphicsQueue(), mPlatform->getGraphicsQueueFamilyIndex());
    mCommands->setObserver(&mPipelineCache);
    mPipelineCache.setDevice(mPlatform->getDevice(), mAllocator);

    // TOOD: move them all to be initialized by constructor
    mStagePool.initialize(mAllocator, mCommands);
    mFramebufferCache.initialize(mPlatform->getDevice());
    mSamplerCache.initialize(mPlatform->getDevice());

    mEmptyTexture = createEmptyTexture(mPlatform->getDevice(), mPlatform->getPhysicalDevice(),
            mContext, mAllocator, mCommands, mStagePool);

    mPipelineCache.setDummyTexture(mEmptyTexture->getPrimaryImageView());
    mBlitter.initialize(mPlatform->getPhysicalDevice(), mPlatform->getDevice(), mAllocator,
            mCommands, mEmptyTexture);
}

VulkanDriver::~VulkanDriver() noexcept = default;

UTILS_NOINLINE
Driver* VulkanDriver::create(VulkanPlatform* platform, VulkanContext const& context,
         Platform::DriverConfig const& driverConfig) noexcept {
    assert_invariant(platform);
    size_t defaultSize = FILAMENT_VULKAN_HANDLE_ARENA_SIZE_IN_MB * 1024U * 1024U;
    Platform::DriverConfig validConfig{
            .handleArenaSize = std::max(driverConfig.handleArenaSize, defaultSize)};
    return new VulkanDriver(platform, context, validConfig);
}

ShaderModel VulkanDriver::getShaderModel() const noexcept {
#if defined(__ANDROID__) || defined(IOS)
    return ShaderModel::MOBILE;
#else
    return ShaderModel::DESKTOP;
#endif
}

void VulkanDriver::terminate() {
    mEmptyTexture.reset();
    mCommands.reset();
    mTimestamps.reset();

    mBlitter.shutdown();

    // Allow the stage pool and disposer to clean up.
    mStagePool.gc();
    mDisposer.reset();

    mStagePool.reset();
    mPipelineCache.destroyCache();
    mFramebufferCache.reset();
    mSamplerCache.reset();

    vmaDestroyAllocator(mAllocator);

    if (mDebugCallback) {
        vkDestroyDebugReportCallbackEXT(mPlatform->getInstance(), mDebugCallback, VKALLOC);
    }
    if (mDebugMessenger) {
        vkDestroyDebugUtilsMessengerEXT(mPlatform->getInstance(), mDebugMessenger, VKALLOC);
    }
    mPlatform->terminate();
}

void VulkanDriver::tick(int) {
    mCommands->updateFences();
}

// Garbage collection should not occur too frequently, only about once per frame. Internally, the
// eviction time of various resources is often measured in terms of an approximate frame number
// rather than the wall clock, because we must wait 3 frames after a DriverAPI-level resource has
// been destroyed for safe destruction, due to outstanding command buffers and triple buffering.
void VulkanDriver::collectGarbage() {
    // Command buffers need to be submitted and completed before other resources can be gc'd. And
    // its gc() function carrys out the *wait*.
    mCommands->gc();
    mStagePool.gc();
    mFramebufferCache.gc();
    mDisposer.gc();
}

void VulkanDriver::beginFrame(int64_t monotonic_clock_ns, uint32_t frameId) {
    // Do nothing.
}

void VulkanDriver::setFrameScheduledCallback(Handle<HwSwapChain> sch,
        FrameScheduledCallback callback, void* user) {
}

void VulkanDriver::setFrameCompletedCallback(Handle<HwSwapChain> sch,
        FrameCompletedCallback callback, void* user) {
}

void VulkanDriver::setPresentationTime(int64_t monotonic_clock_ns) {
}

void VulkanDriver::endFrame(uint32_t frameId) {
    if (mCommands->flush()) {
        collectGarbage();
    }
}

void VulkanDriver::flush(int) {
    mCommands->flush();
}

void VulkanDriver::finish(int dummy) {
    mCommands->flush();
}

void VulkanDriver::createSamplerGroupR(Handle<HwSamplerGroup> sbh, uint32_t count) {
    construct<VulkanSamplerGroup>(sbh, count);
}

void VulkanDriver::createRenderPrimitiveR(Handle<HwRenderPrimitive> rph,
        Handle<HwVertexBuffer> vbh, Handle<HwIndexBuffer> ibh,
        PrimitiveType pt, uint32_t offset,
        uint32_t minIndex, uint32_t maxIndex, uint32_t count) {
    construct<VulkanRenderPrimitive>(rph);
    VulkanDriver::setRenderPrimitiveBuffer(rph, vbh, ibh);
    VulkanDriver::setRenderPrimitiveRange(rph, pt, offset, minIndex, maxIndex, count);
}

void VulkanDriver::destroyRenderPrimitive(Handle<HwRenderPrimitive> rph) {
    if (rph) {
        destruct<VulkanRenderPrimitive>(rph);
    }
}

void VulkanDriver::createVertexBufferR(Handle<HwVertexBuffer> vbh, uint8_t bufferCount,
        uint8_t attributeCount, uint32_t elementCount, AttributeArray attributes) {
    auto vertexBuffer = construct<VulkanVertexBuffer>(vbh, mContext, mStagePool,
            bufferCount, attributeCount, elementCount, attributes);
    mDisposer.createDisposable(vertexBuffer, [this, vbh] () {
        destruct<VulkanVertexBuffer>(vbh);
    });
}

void VulkanDriver::destroyVertexBuffer(Handle<HwVertexBuffer> vbh) {
    if (vbh) {
        auto vertexBuffer = handle_cast<VulkanVertexBuffer*>(vbh);
        mDisposer.removeReference(vertexBuffer);
    }
}

void VulkanDriver::createIndexBufferR(Handle<HwIndexBuffer> ibh, ElementType elementType,
        uint32_t indexCount, BufferUsage usage) {
    auto elementSize = (uint8_t) getElementTypeSize(elementType);
    auto indexBuffer = construct<VulkanIndexBuffer>(ibh, mAllocator, mCommands, mStagePool,
            elementSize, indexCount);
    mDisposer.createDisposable(indexBuffer,
            [this, ibh]() { destructBuffer<VulkanIndexBuffer>(ibh); });
}

void VulkanDriver::destroyIndexBuffer(Handle<HwIndexBuffer> ibh) {
    if (ibh) {
        auto indexBuffer = handle_cast<VulkanIndexBuffer*>(ibh);
        mDisposer.removeReference(indexBuffer);
    }
}

void VulkanDriver::createBufferObjectR(Handle<HwBufferObject> boh, uint32_t byteCount,
        BufferObjectBinding bindingType, BufferUsage usage) {
    auto bufferObject = construct<VulkanBufferObject>(boh, mAllocator, mCommands, mStagePool,
            byteCount, bindingType, usage);
    mDisposer.createDisposable(bufferObject,
            [this, boh]() { destructBuffer<VulkanBufferObject>(boh); });
}

void VulkanDriver::destroyBufferObject(Handle<HwBufferObject> boh) {
    if (boh) {
       auto bufferObject = handle_cast<VulkanBufferObject*>(boh);
       if (bufferObject->bindingType == BufferObjectBinding::UNIFORM) {
           mPipelineCache.unbindUniformBuffer(bufferObject->buffer.getGpuBuffer());
           // Decrement the refcount of the uniform buffer, but schedule it for destruction a few
           // frames in the future. To be safe, we need to assume that the current command buffer is
           // still using it somewhere.
           mDisposer.acquire(bufferObject);
       }
       mDisposer.removeReference(bufferObject);
    }
}

void VulkanDriver::createTextureR(Handle<HwTexture> th, SamplerType target, uint8_t levels,
        TextureFormat format, uint8_t samples, uint32_t w, uint32_t h, uint32_t depth,
        TextureUsage usage) {
    auto vktexture = construct<VulkanTexture>(th, mPlatform->getDevice(),
            mPlatform->getPhysicalDevice(), mContext, mAllocator, mCommands, target, levels, format,
            samples, w, h, depth, usage, mStagePool);
    mDisposer.createDisposable(vktexture, [this, th]() { destruct<VulkanTexture>(th); });
}

void VulkanDriver::createTextureSwizzledR(Handle<HwTexture> th, SamplerType target, uint8_t levels,
        TextureFormat format, uint8_t samples, uint32_t w, uint32_t h, uint32_t depth,
        TextureUsage usage,
        TextureSwizzle r, TextureSwizzle g, TextureSwizzle b, TextureSwizzle a) {
    TextureSwizzle swizzleArray[] = {r, g, b, a};
    const VkComponentMapping swizzleMap = getSwizzleMap(swizzleArray);
    auto vktexture = construct<VulkanTexture>(th, mPlatform->getDevice(),
            mPlatform->getPhysicalDevice(), mContext, mAllocator, mCommands, target, levels, format,
            samples, w, h, depth, usage, mStagePool, swizzleMap);
    mDisposer.createDisposable(vktexture, [this, th]() {
        destruct<VulkanTexture>(th);
    });
}

void VulkanDriver::importTextureR(Handle<HwTexture> th, intptr_t id,
        SamplerType target, uint8_t levels,
        TextureFormat format, uint8_t samples, uint32_t w, uint32_t h, uint32_t depth,
        TextureUsage usage) {
    // not supported in this backend
}

void VulkanDriver::destroyTexture(Handle<HwTexture> th) {
    if (th) {
        auto texture = handle_cast<VulkanTexture*>(th);
        mPipelineCache.unbindImageView(texture->getPrimaryImageView());
        mDisposer.removeReference(texture);
    }
}

void VulkanDriver::createProgramR(Handle<HwProgram> ph, Program&& program) {
    auto vkprogram = construct<VulkanProgram>(ph, mPlatform->getDevice(), program);
    mDisposer.createDisposable(vkprogram, [this, ph] () {
        destruct<VulkanProgram>(ph);
    });
}

void VulkanDriver::destroyProgram(Handle<HwProgram> ph) {
    if (ph) {
        mDisposer.removeReference(handle_cast<VulkanProgram*>(ph));
    }
}

void VulkanDriver::createDefaultRenderTargetR(Handle<HwRenderTarget> rth, int) {
    assert_invariant(mDefaultRenderTarget == nullptr);
    VulkanRenderTarget* renderTarget = construct<VulkanRenderTarget>(rth);
    mDefaultRenderTarget = renderTarget;
    mDisposer.createDisposable(renderTarget, [this, rth] () {
        destruct<VulkanRenderTarget>(rth);
    });
}

void VulkanDriver::createRenderTargetR(Handle<HwRenderTarget> rth,
        TargetBufferFlags targets, uint32_t width, uint32_t height, uint8_t samples,
        MRT color, TargetBufferInfo depth, TargetBufferInfo stencil) {
    UTILS_UNUSED_IN_RELEASE math::vec2<uint32_t> tmin = {std::numeric_limits<uint32_t>::max()};
    UTILS_UNUSED_IN_RELEASE math::vec2<uint32_t> tmax = {0};
    UTILS_UNUSED_IN_RELEASE size_t attachmentCount = 0;

    VulkanAttachment colorTargets[MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT] = {};
    for (int i = 0; i < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; i++) {
        if (color[i].handle) {
            colorTargets[i] = {
                .texture = TexturePointer(handle_cast<VulkanTexture*>(color[i].handle)),
                .level = color[i].level,
                .layer = color[i].layer,
            };
            UTILS_UNUSED_IN_RELEASE VkExtent2D extent = colorTargets[i].getExtent2D();
            tmin = { std::min(tmin.x, extent.width), std::min(tmin.y, extent.height) };
            tmax = { std::max(tmax.x, extent.width), std::max(tmax.y, extent.height) };
            attachmentCount++;
        }
    }

    VulkanAttachment depthStencil[2] = {};
    if (depth.handle) {
        depthStencil[0] = {
            .texture = TexturePointer(handle_cast<VulkanTexture*>(depth.handle)),
            .level = depth.level,
            .layer = depth.layer,
        };
        UTILS_UNUSED_IN_RELEASE VkExtent2D extent = depthStencil[0].getExtent2D();
        tmin = { std::min(tmin.x, extent.width), std::min(tmin.y, extent.height) };
        tmax = { std::max(tmax.x, extent.width), std::max(tmax.y, extent.height) };
        attachmentCount++;
    }

    if (stencil.handle) {
        depthStencil[1] = {
            .texture = TexturePointer(handle_cast<VulkanTexture*>(stencil.handle)),
            .level = stencil.level,
            .layer = stencil.layer,
        };
        UTILS_UNUSED_IN_RELEASE VkExtent2D extent = depthStencil[1].getExtent2D();
        tmin = { std::min(tmin.x, extent.width), std::min(tmin.y, extent.height) };
        tmax = { std::max(tmax.x, extent.width), std::max(tmax.y, extent.height) };
        attachmentCount++;
    }

    // All attachments must have the same dimensions, which must be greater than or equal to the
    // render target dimensions.
    assert_invariant(attachmentCount > 0);
    assert_invariant(tmin == tmax);
    assert_invariant(tmin.x >= width && tmin.y >= height);

    auto renderTarget = construct<VulkanRenderTarget>(rth, mPlatform->getDevice(),
            mPlatform->getPhysicalDevice(), mContext, mAllocator, mCommands, width, height, samples,
            colorTargets, depthStencil, mStagePool);
    mDisposer.createDisposable(renderTarget, [this, rth]() { destruct<VulkanRenderTarget>(rth); });
}

void VulkanDriver::destroyRenderTarget(Handle<HwRenderTarget> rth) {
    if (rth) {
        VulkanRenderTarget* rt = handle_cast<VulkanRenderTarget*>(rth);
        if (UTILS_UNLIKELY(rt == mDefaultRenderTarget)) {
            mDefaultRenderTarget = nullptr;
        }
        mDisposer.removeReference(rt);
    }
}

void VulkanDriver::createFenceR(Handle<HwFence> fh, int) {
    VulkanCommandBuffer const& commandBuffer = mCommands->get();
    construct<VulkanFence>(fh, commandBuffer);
}

void VulkanDriver::createSyncR(Handle<HwSync> sh, int) {
    VulkanCommandBuffer const& commandBuffer = mCommands->get();
    construct<VulkanSync>(sh, commandBuffer);
}

void VulkanDriver::createSwapChainR(Handle<HwSwapChain> sch, void* nativeWindow, uint64_t flags) {
    construct<VulkanSwapChain>(sch, mPlatform, mContext, mAllocator, mCommands, mStagePool,
            nativeWindow, flags);
}

void VulkanDriver::createSwapChainHeadlessR(Handle<HwSwapChain> sch, uint32_t width,
        uint32_t height, uint64_t flags) {
    assert_invariant(width > 0 && height > 0 && "Vulkan requires non-zero swap chain dimensions.");
    construct<VulkanSwapChain>(sch, mPlatform, mContext, mAllocator, mCommands, mStagePool, nullptr,
            flags, VkExtent2D{width, height});
}

void VulkanDriver::createTimerQueryR(Handle<HwTimerQuery> tqh, int) {
    // nothing to do, timer query was constructed in createTimerQueryS
}

Handle<HwVertexBuffer> VulkanDriver::createVertexBufferS() noexcept {
    return allocHandle<VulkanVertexBuffer>();
}

Handle<HwIndexBuffer> VulkanDriver::createIndexBufferS() noexcept {
    return allocHandle<VulkanIndexBuffer>();
}

Handle<HwBufferObject> VulkanDriver::createBufferObjectS() noexcept {
    return allocHandle<VulkanBufferObject>();
}

Handle<HwTexture> VulkanDriver::createTextureS() noexcept {
    return allocHandle<VulkanTexture>();
}

Handle<HwTexture> VulkanDriver::createTextureSwizzledS() noexcept {
    return allocHandle<VulkanTexture>();
}

Handle<HwTexture> VulkanDriver::importTextureS() noexcept {
    return allocHandle<VulkanTexture>();
}

Handle<HwSamplerGroup> VulkanDriver::createSamplerGroupS() noexcept {
    return allocHandle<VulkanSamplerGroup>();
}

Handle<HwRenderPrimitive> VulkanDriver::createRenderPrimitiveS() noexcept {
    return allocHandle<VulkanRenderPrimitive>();
}

Handle<HwProgram> VulkanDriver::createProgramS() noexcept {
    return allocHandle<VulkanProgram>();
}

Handle<HwRenderTarget> VulkanDriver::createDefaultRenderTargetS() noexcept {
    return allocHandle<VulkanRenderTarget>();
}

Handle<HwRenderTarget> VulkanDriver::createRenderTargetS() noexcept {
    return allocHandle<VulkanRenderTarget>();
}

Handle<HwFence> VulkanDriver::createFenceS() noexcept {
    return allocHandle<VulkanFence>();
}

Handle<HwSync> VulkanDriver::createSyncS() noexcept {
    Handle<HwSync> sh = allocHandle<VulkanSync>();
    construct<VulkanSync>(sh);
    return sh;
}

Handle<HwSwapChain> VulkanDriver::createSwapChainS() noexcept {
    return allocHandle<VulkanSwapChain>();
}

Handle<HwSwapChain> VulkanDriver::createSwapChainHeadlessS() noexcept {
    return allocHandle<VulkanSwapChain>();
}

Handle<HwTimerQuery> VulkanDriver::createTimerQueryS() noexcept {
    // The handle must be constructed here, as a synchronous call to getTimerQueryValue might happen
    // before createTimerQueryR is executed.
    Handle<HwTimerQuery> tqh = initHandle<VulkanTimerQuery>(mTimestamps->getNextQuery());
    auto query = handle_cast<VulkanTimerQuery*>(tqh);
    mDisposer.createDisposable(query, [this, tqh] () {
        destruct<VulkanTimerQuery>(tqh);
    });
    return tqh;
}

void VulkanDriver::destroySamplerGroup(Handle<HwSamplerGroup> sbh) {
    if (sbh) {
        // Unlike most of the other "Hw" handles, the sampler buffer is an abstract concept and does
        // not map to any Vulkan objects. To handle destruction, the only thing we need to do is
        // ensure that the next draw call doesn't try to access a zombie sampler buffer. Therefore,
        // simply replace all weak references with null.
        auto* hwsb = handle_cast<VulkanSamplerGroup*>(sbh);
        for (auto& binding : mSamplerBindings) {
            if (binding == hwsb) {
                binding = nullptr;
            }
        }
        destruct<VulkanSamplerGroup>(sbh);
    }
}

void VulkanDriver::destroySwapChain(Handle<HwSwapChain> sch) {
    if (sch) {
        VulkanSwapChain& swapChain = *handle_cast<VulkanSwapChain*>(sch);
        if (mCurrentSwapChain == &swapChain) {
            mCurrentSwapChain = nullptr;
        }
        destruct<VulkanSwapChain>(sch);
    }
}

void VulkanDriver::destroyStream(Handle<HwStream> sh) {
}

void VulkanDriver::destroyTimerQuery(Handle<HwTimerQuery> tqh) {
    if (tqh) {
        mDisposer.removeReference(handle_cast<VulkanTimerQuery*>(tqh));
    }
}

void VulkanDriver::destroySync(Handle<HwSync> sh) {
    destruct<VulkanSync>(sh);
}


Handle<HwStream> VulkanDriver::createStreamNative(void* nativeStream) {
    return {};
}

Handle<HwStream> VulkanDriver::createStreamAcquired() {
    return {};
}

void VulkanDriver::setAcquiredImage(Handle<HwStream> sh, void* image,
        CallbackHandler* handler, StreamCallback cb, void* userData) {
}

void VulkanDriver::setStreamDimensions(Handle<HwStream> sh, uint32_t width, uint32_t height) {
}

int64_t VulkanDriver::getStreamTimestamp(Handle<HwStream> sh) {
    return 0;
}

void VulkanDriver::updateStreams(CommandStream* driver) {
}

void VulkanDriver::destroyFence(Handle<HwFence> fh) {
    destruct<VulkanFence>(fh);
}

FenceStatus VulkanDriver::wait(Handle<HwFence> fh, uint64_t timeout) {
    auto& cmdfence = handle_cast<VulkanFence*>(fh)->fence;

    // Internally we use the VK_INCOMPLETE status to mean "not yet submitted".
    // When this fence gets submitted, its status changes to VK_NOT_READY.
    std::unique_lock<utils::Mutex> lock(cmdfence->mutex);
    if (cmdfence->status.load() == VK_INCOMPLETE) {
        // This will obviously timeout if Filament creates a fence and immediately waits on it
        // without calling endFrame() or commit().
        cmdfence->condition.wait(lock);
    } else {
        lock.unlock();
    }
    VkResult result =
            vkWaitForFences(mPlatform->getDevice(), 1, &cmdfence->fence, VK_TRUE, timeout);
    return result == VK_SUCCESS ? FenceStatus::CONDITION_SATISFIED : FenceStatus::TIMEOUT_EXPIRED;
}

// We create all textures using VK_IMAGE_TILING_OPTIMAL, so our definition of "supported" is that
// the GPU supports the given texture format with non-zero optimal tiling features.
bool VulkanDriver::isTextureFormatSupported(TextureFormat format) {
    VkFormat vkformat = getVkFormat(format);
    // We automatically use an alternative format when the client requests DEPTH24.
    if (format == TextureFormat::DEPTH24) {
        vkformat = mContext.getDepthFormat();
    }
    if (vkformat == VK_FORMAT_UNDEFINED) {
        return false;
    }
    VkFormatProperties info;
    vkGetPhysicalDeviceFormatProperties(mPlatform->getPhysicalDevice(), vkformat, &info);
    return info.optimalTilingFeatures != 0;
}

bool VulkanDriver::isTextureSwizzleSupported() {
    return true;
}

bool VulkanDriver::isTextureFormatMipmappable(TextureFormat format) {
    switch (format) {
        case TextureFormat::DEPTH16:
        case TextureFormat::DEPTH24:
        case TextureFormat::DEPTH32F:
        case TextureFormat::DEPTH24_STENCIL8:
        case TextureFormat::DEPTH32F_STENCIL8:
            return false;
        default:
            return isRenderTargetFormatSupported(format);
    }
}

bool VulkanDriver::isRenderTargetFormatSupported(TextureFormat format) {
    VkFormat vkformat = getVkFormat(format);
    // We automatically use an alternative format when the client requests DEPTH24.
    if (format == TextureFormat::DEPTH24) {
        vkformat = mContext.getDepthFormat();
    }
    if (vkformat == VK_FORMAT_UNDEFINED) {
        return false;
    }
    VkFormatProperties info;
    vkGetPhysicalDeviceFormatProperties(mPlatform->getPhysicalDevice(), vkformat, &info);
    return (info.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
}

bool VulkanDriver::isFrameBufferFetchSupported() {
    return true;
}

bool VulkanDriver::isFrameBufferFetchMultiSampleSupported() {
    return false;
}

bool VulkanDriver::isFrameTimeSupported() {
    return true;
}

bool VulkanDriver::isAutoDepthResolveSupported() {
    return false;
}

bool VulkanDriver::isSRGBSwapChainSupported() {
    // TODO: implement SWAP_CHAIN_CONFIG_SRGB_COLORSPACE
    return false;
}

bool VulkanDriver::isWorkaroundNeeded(Workaround workaround) {
    switch (workaround) {
        case Workaround::SPLIT_EASU: {
            auto const vendorId = mContext.getPhysicalDeviceVendorId();
            // early exit condition is flattened in EASU code
            return vendorId == 0x5143; // Qualcomm
        }
        case Workaround::ALLOW_READ_ONLY_ANCILLARY_FEEDBACK_LOOP:
            // Supporting depth attachment as both sampler and attachment is only possible if we set
            // the depth attachment as read-only (e.g. during SSAO pass), however note that the
            // store-ops for attachments wrt VkRenderPass only has VK_ATTACHMENT_STORE_OP_DONT_CARE
            // and VK_ATTACHMENT_STORE_OP_STORE for versions below 1.3. Only at 1.3 and above do we
            // have a true read-only choice VK_ATTACHMENT_STORE_OP_NONE. That means for < 1.3, we
            // will trigger a validation sync error if we use the depth attachment also as a
            // sampler. See full error below:
            //
            // SYNC-HAZARD-WRITE-AFTER-READ(ERROR / SPEC): msgNum: 929810911 - Validation Error:
            // [ SYNC-HAZARD-WRITE-AFTER-READ ] Object 0: handle = 0x6160000c3680,
            // type = VK_OBJECT_TYPE_RENDER_PASS; | MessageID = 0x376bc9df | vkCmdEndRenderPass:
            // Hazard WRITE_AFTER_READ in subpass 0 for attachment 1 depth aspect during store with
            // storeOp VK_ATTACHMENT_STORE_OP_STORE. Access info (usage:
            // SYNC_LATE_FRAGMENT_TESTS_DEPTH_STENCIL_ATTACHMENT_WRITE, prior_usage:
            // SYNC_FRAGMENT_SHADER_SHADER_STORAGE_READ, read_barriers: VK_PIPELINE_STAGE_2_NONE,
            // command: vkCmdDrawIndexed, seq_no: 177, reset_no: 1)
            //
            // Therefore we apply the existing workaround of an extra blit until a better
            // resolution.
            return false;
        case Workaround::ADRENO_UNIFORM_ARRAY_CRASH:
            return false;
        default:
            return false;
    }
    return false;
}

FeatureLevel VulkanDriver::getFeatureLevel() {
    VkPhysicalDeviceLimits const& limits = mContext.getPhysicalDeviceLimits();

    // If cubemap arrays are not supported, then this is an FL1 device.
    if (!mContext.isImageCubeArraySupported()) {
        return FeatureLevel::FEATURE_LEVEL_1;
    }

    // If the max sampler counts do not meet FL2 standards, then this is an FL1 device.
    const auto& fl2 = FEATURE_LEVEL_CAPS[+FeatureLevel::FEATURE_LEVEL_2];
    if (fl2.MAX_VERTEX_SAMPLER_COUNT < limits.maxPerStageDescriptorSamplers ||
        fl2.MAX_FRAGMENT_SAMPLER_COUNT < limits.maxPerStageDescriptorSamplers) {
        return FeatureLevel::FEATURE_LEVEL_1;
    }

    // If the max sampler counts do not meet FL3 standards, then this is an FL2 device.
    const auto& fl3 = FEATURE_LEVEL_CAPS[+FeatureLevel::FEATURE_LEVEL_3];
    if (fl3.MAX_VERTEX_SAMPLER_COUNT < limits.maxPerStageDescriptorSamplers ||
        fl3.MAX_FRAGMENT_SAMPLER_COUNT < limits.maxPerStageDescriptorSamplers) {
        return FeatureLevel::FEATURE_LEVEL_2;
    }

    return FeatureLevel::FEATURE_LEVEL_3;
}

math::float2 VulkanDriver::getClipSpaceParams() {
    // virtual and physical z-coordinate of clip-space is in [-w, 0]
    // Note: this is actually never used (see: main.vs), but it's a backend API, so we implement it
    // properly.
    return math::float2{ 1.0f, 0.0f };
}

uint8_t VulkanDriver::getMaxDrawBuffers() {
    return MRT::MIN_SUPPORTED_RENDER_TARGET_COUNT; // TODO: query real value
}

size_t VulkanDriver::getMaxUniformBufferSize() {
    // TODO: return the actual size instead of hardcoded value
    // TODO: devices that return less than 32768 should be rejected. This represents only 3%
    //       of android devices.
    return 32768;
}

void VulkanDriver::setVertexBufferObject(Handle<HwVertexBuffer> vbh, uint32_t index,
        Handle<HwBufferObject> boh) {
    auto& vb = *handle_cast<VulkanVertexBuffer*>(vbh);
    auto& bo = *handle_cast<VulkanBufferObject*>(boh);
    assert_invariant(bo.bindingType == BufferObjectBinding::VERTEX);
    vb.buffers[index] = &bo.buffer;
}

void VulkanDriver::updateIndexBuffer(Handle<HwIndexBuffer> ibh, BufferDescriptor&& p,
        uint32_t byteOffset) {
    auto ib = handle_cast<VulkanIndexBuffer*>(ibh);
    ib->buffer.loadFromCpu(p.buffer, byteOffset, p.size);
    mDisposer.acquire(ib);
    scheduleDestroy(std::move(p));
}

void VulkanDriver::updateBufferObject(Handle<HwBufferObject> boh, BufferDescriptor&& bd,
        uint32_t byteOffset) {
    auto bo = handle_cast<VulkanBufferObject*>(boh);
    bo->buffer.loadFromCpu(bd.buffer, byteOffset, bd.size);
    mDisposer.acquire(bo);
    scheduleDestroy(std::move(bd));
}

void VulkanDriver::updateBufferObjectUnsynchronized(Handle<HwBufferObject> boh,
        BufferDescriptor&& bd, uint32_t byteOffset) {
    auto bo = handle_cast<VulkanBufferObject*>(boh);
    // TODO: implement unsynchronized version
    bo->buffer.loadFromCpu(bd.buffer, byteOffset, bd.size);
    mDisposer.acquire(bo);
    scheduleDestroy(std::move(bd));
}

void VulkanDriver::resetBufferObject(Handle<HwBufferObject> boh) {
    // TODO: implement resetBufferObject(). This is equivalent to calling
    // destroyBufferObject() followed by createBufferObject() keeping the same handle.
    // It is actually okay to keep a no-op implementation, the intention here is to "orphan" the
    // buffer (and possibly return it to a pool) and allocate a new one (or get it from a pool),
    // so that no further synchronization with the GPU is needed.
    // This is only useful if updateBufferObjectUnsynchronized() is implemented unsynchronizedly.
}

void VulkanDriver::setMinMaxLevels(Handle<HwTexture> th, uint32_t minLevel, uint32_t maxLevel) {
    handle_cast<VulkanTexture*>(th)->setPrimaryRange(minLevel, maxLevel);
}

void VulkanDriver::update3DImage(
        Handle<HwTexture> th,
        uint32_t level, uint32_t xoffset, uint32_t yoffset, uint32_t zoffset,
        uint32_t width, uint32_t height, uint32_t depth,
        PixelBufferDescriptor&& data) {
    handle_cast<VulkanTexture*>(th)->updateImage(data, width, height, depth,
            xoffset, yoffset, zoffset, level);
    scheduleDestroy(std::move(data));
}

void VulkanDriver::setupExternalImage(void* image) {
}

bool VulkanDriver::getTimerQueryValue(Handle<HwTimerQuery> tqh, uint64_t* elapsedTime) {
    VulkanTimerQuery* vtq = handle_cast<VulkanTimerQuery*>(tqh);
    if (!vtq->isCompleted()) {
        return false;
    }

    auto results = mTimestamps->getResult(vtq);
    uint64_t timestamp0 = results[0];
    uint64_t available0 = results[1];
    uint64_t timestamp1 = results[2];
    uint64_t available1 = results[3];

    if (available0 == 0 || available1 == 0) {
        return false;
    }

    ASSERT_POSTCONDITION(timestamp1 >= timestamp0, "Timestamps are not monotonically increasing.");

    // NOTE: MoltenVK currently writes system time so the following delta will always be zero.
    // However there are plans for implementing this properly. See the following GitHub ticket.
    // https://github.com/KhronosGroup/MoltenVK/issues/773

    float const period = mContext.getPhysicalDeviceLimits().timestampPeriod;
    uint64_t delta = uint64_t(float(timestamp1 - timestamp0) * period);
    *elapsedTime = delta;
    return true;
}

SyncStatus VulkanDriver::getSyncStatus(Handle<HwSync> sh) {
    VulkanSync* sync = handle_cast<VulkanSync*>(sh);
    if (sync->fence == nullptr) {
        return SyncStatus::NOT_SIGNALED;
    }
    VkResult status = sync->fence->status.load(std::memory_order_relaxed);
    switch (status) {
        case VK_SUCCESS: return SyncStatus::SIGNALED;
        case VK_INCOMPLETE: return SyncStatus::NOT_SIGNALED;
        case VK_NOT_READY: return SyncStatus::NOT_SIGNALED;
        case VK_ERROR_DEVICE_LOST: return SyncStatus::ERROR;
        default:
            // NOTE: In theory, the fence status must be one of the above values.
            return SyncStatus::ERROR;
    }
}

void VulkanDriver::setExternalImage(Handle<HwTexture> th, void* image) {
}

void VulkanDriver::setExternalImagePlane(Handle<HwTexture> th, void* image, uint32_t plane) {
}

void VulkanDriver::setExternalStream(Handle<HwTexture> th, Handle<HwStream> sh) {
}

void VulkanDriver::generateMipmaps(Handle<HwTexture> th) { }

bool VulkanDriver::canGenerateMipmaps() {
    return false;
}

void VulkanDriver::updateSamplerGroup(Handle<HwSamplerGroup> sbh,
        BufferDescriptor&& data) {
    auto* sb = handle_cast<VulkanSamplerGroup*>(sbh);

    // FIXME: we shouldn't be using SamplerGroup here, instead the backend should create
    //        a descriptor or any internal data-structure that represents the textures/samplers.
    //        It's preferable to do as much work as possible here.
    //        Here, we emulate the older backend API by re-creating a SamplerGroup from the
    //        passed data.
    SamplerGroup samplerGroup(data.size / sizeof(SamplerDescriptor));
    memcpy(samplerGroup.data(), data.buffer, data.size);
    *sb->sb = std::move(samplerGroup);

    scheduleDestroy(std::move(data));
}

void VulkanDriver::compilePrograms(CallbackHandler* handler,
        CallbackHandler::Callback callback, void* user) {
    scheduleCallback(handler, user, callback);
}

void VulkanDriver::beginRenderPass(Handle<HwRenderTarget> rth, const RenderPassParams& params) {
    VulkanRenderTarget* const rt = handle_cast<VulkanRenderTarget*>(rth);
    const VkExtent2D extent = rt->getExtent();
    assert_invariant(extent.width > 0 && extent.height > 0);

    // Filament has the expectation that the contents of the swap chain are not preserved on the
    // first render pass. Note however that its contents are often preserved on subsequent render
    // passes, due to multiple views.
    TargetBufferFlags discardStart = params.flags.discardStart;
    if (rt->isSwapChain()) {
        VulkanSwapChain* sc = mCurrentSwapChain;
        assert_invariant(sc);
        if (sc->isFirstRenderPass()) {
            discardStart |= TargetBufferFlags::COLOR;
            sc->markFirstRenderPass();
        }
    }

    VulkanAttachment depth = rt->getSamples() == 1 ? rt->getDepth() : rt->getMsaaDepth();
#if FILAMENT_VULKAN_VERBOSE
    if (depth.texture) {
        depth.texture->print();
    }
#endif

    // We need to determine whether the same depth texture is both sampled and set as an attachment.
    // If that's the case, we need to change the layout of the texture to DEPTH_SAMPLER, which is a
    // more general layout. Otherwise, we prefer the DEPTH_ATTACHMENT layout, which is optimal for
    // the non-sampling case.
    bool samplingDepthAttachment = false;
    VkCommandBuffer const cmdbuffer = mCommands->get().cmdbuffer;

    UTILS_NOUNROLL
    for (uint8_t samplerGroupIdx = 0; samplerGroupIdx < Program::SAMPLER_BINDING_COUNT;
            samplerGroupIdx++) {
        VulkanSamplerGroup* vksb = mSamplerBindings[samplerGroupIdx];
        if (!vksb) {
            continue;
        }
        SamplerGroup* sb = vksb->sb.get();
        for (size_t i = 0; i < sb->getSize(); i++) {
            SamplerDescriptor const* boundSampler = sb->data() + i;
            if (UTILS_LIKELY(boundSampler->t)) {
                VulkanTexture* texture = handle_cast<VulkanTexture*>(boundSampler->t);
                if (!any(texture->usage & TextureUsage::DEPTH_ATTACHMENT)) {
                    continue;
                }
                samplingDepthAttachment
                        = depth.texture && texture->getVkImage() == depth.texture->getVkImage();
                if (texture->getPrimaryImageLayout() == VulkanLayout::DEPTH_SAMPLER) {
                    continue;
                }
                VkImageSubresourceRange const subresources{
                        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                        .baseMipLevel = 0,
                        .levelCount = texture->levels,
                        .baseArrayLayer = 0,
                        .layerCount = texture->depth,
                };
                texture->transitionLayout(cmdbuffer, subresources, VulkanLayout::DEPTH_SAMPLER);
                break;
            }
        }
    }
    // currentDepthLayout tracks state of the layout after the (potential) transition in the above block.
    VulkanLayout currentDepthLayout = depth.getLayout();
    VulkanLayout const renderPassDepthLayout = samplingDepthAttachment
                                                       ? VulkanLayout::DEPTH_SAMPLER
                                                       : VulkanLayout::DEPTH_ATTACHMENT;
    VulkanLayout const finalDepthLayout = renderPassDepthLayout;

    TargetBufferFlags clearVal = params.flags.clear;
    TargetBufferFlags discardEndVal = params.flags.discardEnd;
    if (depth.texture) {
        if (params.readOnlyDepthStencil & RenderPassParams::READONLY_DEPTH) {
            discardEndVal &= ~TargetBufferFlags::DEPTH;
            clearVal &= ~TargetBufferFlags::DEPTH;
        }
        if (currentDepthLayout != renderPassDepthLayout) {
            depth.texture->transitionLayout(cmdbuffer,
                    depth.getSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT), renderPassDepthLayout);
            currentDepthLayout = renderPassDepthLayout;
        }
    }

    // Create the VkRenderPass or fetch it from cache.
    VulkanFboCache::RenderPassKey rpkey = {
        .initialColorLayoutMask = 0,
        .initialDepthLayout = currentDepthLayout,
        .renderPassDepthLayout = renderPassDepthLayout,
        .finalDepthLayout = finalDepthLayout,
        .depthFormat = depth.getFormat(),
        .clear = clearVal,
        .discardStart = discardStart,
        .discardEnd = discardEndVal,
        .samples = rt->getSamples(),
        .subpassMask = uint8_t(params.subpassMask),
    };
    for (int i = 0; i < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; i++) {
        const VulkanAttachment& info = rt->getColor(i);
        if (info.texture) {
            rpkey.initialColorLayoutMask |= 1 << i;
            rpkey.colorFormat[i] = info.getFormat();
            if (rpkey.samples > 1 && info.texture->samples == 1) {
                rpkey.needsResolveMask |= (1 << i);
            }
            if (info.texture->getPrimaryImageLayout() != VulkanLayout::COLOR_ATTACHMENT) {
                ((VulkanTexture*) info.texture)->transitionLayout(cmdbuffer,
                        info.getSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
                        VulkanLayout::COLOR_ATTACHMENT);
            }
        } else {
            rpkey.colorFormat[i] = VK_FORMAT_UNDEFINED;
        }
    }

    VkRenderPass renderPass = mFramebufferCache.getRenderPass(rpkey);
    mPipelineCache.bindRenderPass(renderPass, 0);

    // Create the VkFramebuffer or fetch it from cache.
    VulkanFboCache::FboKey fbkey {
        .renderPass = renderPass,
        .width = (uint16_t) extent.width,
        .height = (uint16_t) extent.height,
        .layers = 1,
        .samples = rpkey.samples,
    };
    for (int i = 0; i < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; i++) {
        if (!rt->getColor(i).texture) {
            fbkey.color[i] = VK_NULL_HANDLE;
            fbkey.resolve[i] = VK_NULL_HANDLE;
        } else if (fbkey.samples == 1) {
            fbkey.color[i] = rt->getColor(i).getImageView(VK_IMAGE_ASPECT_COLOR_BIT);
            fbkey.resolve[i] = VK_NULL_HANDLE;
            assert_invariant(fbkey.color[i]);
        } else {
            fbkey.color[i] = rt->getMsaaColor(i).getImageView(VK_IMAGE_ASPECT_COLOR_BIT);
            VulkanTexture* texture = (VulkanTexture*) rt->getColor(i).texture;
            if (texture->samples == 1) {
                fbkey.resolve[i] = rt->getColor(i).getImageView(VK_IMAGE_ASPECT_COLOR_BIT);
                assert_invariant(fbkey.resolve[i]);
            }
            assert_invariant(fbkey.color[i]);
        }
    }
    if (depth.texture) {
        fbkey.depth = depth.getImageView(VK_IMAGE_ASPECT_DEPTH_BIT);
        assert_invariant(fbkey.depth);

        // Vulkan 1.1 does not support multisampled depth resolve, so let's check here
        // and assert if this is requested. (c.f. isAutoDepthResolveSupported)
        // Reminder: Filament's backend API works like this:
        // - If the RT is SS then all attachments must be SS.
        // - If the RT is MS then all SS attachments are auto resolved if not discarded.
        assert_invariant(!(rt->getSamples() > 1 &&
                rt->getDepth().texture->samples == 1 &&
                !any(rpkey.discardEnd & TargetBufferFlags::DEPTH)));
    }
    VkFramebuffer vkfb = mFramebufferCache.getFramebuffer(fbkey);

    // Assign a label to the framebuffer for debugging purposes.
    if (UTILS_UNLIKELY(mContext.isDebugUtilsSupported()) && !mCurrentDebugMarker.empty()) {
        const VkDebugUtilsObjectNameInfoEXT info = {
            VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            nullptr,
            VK_OBJECT_TYPE_FRAMEBUFFER,
            reinterpret_cast<uint64_t>(vkfb),
            mCurrentDebugMarker.c_str(),
        };
        vkSetDebugUtilsObjectNameEXT(mPlatform->getDevice(), &info);
    }

    // The current command buffer now owns a reference to the render target and its attachments.
    // Note that we must acquire parent textures, not sidecars.
    mDisposer.acquire(rt);
    mDisposer.acquire((VulkanTexture const*) rt->getDepth().texture);
    for (int i = 0; i < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; i++) {
        mDisposer.acquire((VulkanTexture const*) rt->getColor(i).texture);
    }

    // Populate the structures required for vkCmdBeginRenderPass.
    VkRenderPassBeginInfo renderPassInfo {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = renderPass,
        .framebuffer = vkfb,

        // The renderArea field constrains the LoadOp, but scissoring does not.
        // Therefore, we do not set the scissor rect here, we only need it in draw().
        .renderArea = { .offset = {}, .extent = extent }
    };

    rt->transformClientRectToPlatform(&renderPassInfo.renderArea);

    VkClearValue clearValues[
            MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT + MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT +
            1] = {};
    if (params.flags.clear != TargetBufferFlags::NONE) {

        // NOTE: clearValues must be populated in the same order as the attachments array in
        // VulkanFboCache::getFramebuffer. Values must be provided regardless of whether Vulkan is
        // actually clearing that particular target.
        for (int i = 0; i < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; i++) {
            if (fbkey.color[i]) {
                VkClearValue &clearValue = clearValues[renderPassInfo.clearValueCount++];
                clearValue.color.float32[0] = params.clearColor.r;
                clearValue.color.float32[1] = params.clearColor.g;
                clearValue.color.float32[2] = params.clearColor.b;
                clearValue.color.float32[3] = params.clearColor.a;
            }
        }
        // Resolve attachments are not cleared but still have entries in the list, so skip over them.
        for (int i = 0; i < MRT::MAX_SUPPORTED_RENDER_TARGET_COUNT; i++) {
            if (rpkey.needsResolveMask & (1u << i)) {
                renderPassInfo.clearValueCount++;
            }
        }
        if (fbkey.depth) {
            VkClearValue &clearValue = clearValues[renderPassInfo.clearValueCount++];
            clearValue.depthStencil = {(float) params.clearDepth, 0};
        }
        renderPassInfo.pClearValues = &clearValues[0];
    }

    vkCmdBeginRenderPass(cmdbuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {
        .x = (float) params.viewport.left,
        .y = (float) params.viewport.bottom,
        .width = (float) params.viewport.width,
        .height = (float) params.viewport.height,
        .minDepth = params.depthRange.near,
        .maxDepth = params.depthRange.far
    };

    rt->transformClientRectToPlatform(&viewport);
    vkCmdSetViewport(cmdbuffer, 0, 1, &viewport);

    mCurrentRenderPass = {
        .renderTarget = rt,
        .renderPass = renderPassInfo.renderPass,
        .params = params,
        .currentSubpass = 0,
    };
}

void VulkanDriver::endRenderPass(int) {
    VkCommandBuffer cmdbuffer = mCommands->get().cmdbuffer;
    vkCmdEndRenderPass(cmdbuffer);

    VulkanRenderTarget* rt = mCurrentRenderPass.renderTarget;
    assert_invariant(rt);

    // Since we might soon be sampling from the render target that we just wrote to, we need a
    // pipeline barrier between framebuffer writes and shader reads. This is a memory barrier rather
    // than an image barrier. If we were to use image barriers here, we would potentially need to
    // issue several of them when considering MRT. This would be very complex to set up and would
    // require more state tracking, so we've chosen to use a memory barrier for simplicity and
    // correctness.

    // NOTE: ideally dstStageMask would merely be VERTEX_SHADER_BIT | FRAGMENT_SHADER_BIT, but this
    // seems to be insufficient on Mali devices. To work around this we are adding a more aggressive
    // TOP_OF_PIPE barrier.

    if (!rt->isSwapChain()) {
        VkMemoryBarrier barrier {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (rt->hasDepth()) {
            barrier.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            srcStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        }
        vkCmdPipelineBarrier(cmdbuffer, srcStageMask,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | // <== For Mali
                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    if (mCurrentRenderPass.currentSubpass > 0) {
        for (uint32_t i = 0; i < VulkanPipelineCache::TARGET_BINDING_COUNT; i++) {
            mPipelineCache.bindInputAttachment(i, {});
        }
        mCurrentRenderPass.currentSubpass = 0;
    }
    mCurrentRenderPass.renderTarget = nullptr;
    mCurrentRenderPass.renderPass = VK_NULL_HANDLE;
}

void VulkanDriver::nextSubpass(int) {
    ASSERT_PRECONDITION(mCurrentRenderPass.currentSubpass == 0,
            "Only two subpasses are currently supported.");

    VulkanRenderTarget* renderTarget = mCurrentRenderPass.renderTarget;
    assert_invariant(renderTarget);
    assert_invariant(mCurrentRenderPass.params.subpassMask);

    vkCmdNextSubpass(mCommands->get().cmdbuffer, VK_SUBPASS_CONTENTS_INLINE);

    mPipelineCache.bindRenderPass(mCurrentRenderPass.renderPass,
            ++mCurrentRenderPass.currentSubpass);

    for (uint32_t i = 0; i < VulkanPipelineCache::TARGET_BINDING_COUNT; i++) {
        if ((1 << i) & mCurrentRenderPass.params.subpassMask) {
            VulkanAttachment subpassInput = renderTarget->getColor(i);
            VkDescriptorImageInfo info = {
                .imageView = subpassInput.getImageView(VK_IMAGE_ASPECT_COLOR_BIT),
                .imageLayout = ImgUtil::getVkLayout(subpassInput.getLayout()),
            };
            mPipelineCache.bindInputAttachment(i, info);
        }
    }
}

void VulkanDriver::setRenderPrimitiveBuffer(Handle<HwRenderPrimitive> rph,
        Handle<HwVertexBuffer> vbh, Handle<HwIndexBuffer> ibh) {
    auto primitive = handle_cast<VulkanRenderPrimitive*>(rph);
    primitive->setBuffers(handle_cast<VulkanVertexBuffer*>(vbh),
            handle_cast<VulkanIndexBuffer*>(ibh));
}

void VulkanDriver::setRenderPrimitiveRange(Handle<HwRenderPrimitive> rph,
        PrimitiveType pt, uint32_t offset,
        uint32_t minIndex, uint32_t maxIndex, uint32_t count) {
    auto& primitive = *handle_cast<VulkanRenderPrimitive*>(rph);
    primitive.setPrimitiveType(pt);
    primitive.offset = offset * primitive.indexBuffer->elementSize;
    primitive.count = count;
    primitive.minIndex = minIndex;
    primitive.maxIndex = maxIndex > minIndex ? maxIndex : primitive.maxVertexCount - 1;
}

void VulkanDriver::makeCurrent(Handle<HwSwapChain> drawSch, Handle<HwSwapChain> readSch) {
    ASSERT_PRECONDITION_NON_FATAL(drawSch == readSch,
                                  "Vulkan driver does not support distinct draw/read swap chains.");
    VulkanSwapChain* swapChain = mCurrentSwapChain = handle_cast<VulkanSwapChain*>(drawSch);

    bool resized = false;
    swapChain->acquire(resized);

    if (resized) {
        mFramebufferCache.reset();
    }

    if (UTILS_LIKELY(mDefaultRenderTarget)) {
        mDefaultRenderTarget->bindToSwapChain(*swapChain);
    }
}

void VulkanDriver::commit(Handle<HwSwapChain> sch) {
    VulkanSwapChain* swapChain = handle_cast<VulkanSwapChain*>(sch);

    if (mCommands->flush()) {
        collectGarbage();
    }

    // Present the backbuffer after the most recent command buffer submission has finished.
    swapChain->present();
}

void VulkanDriver::bindUniformBuffer(uint32_t index, Handle<HwBufferObject> boh) {
    auto* bo = handle_cast<VulkanBufferObject*>(boh);
    const VkDeviceSize offset = 0;
    const VkDeviceSize size = VK_WHOLE_SIZE;
    mPipelineCache.bindUniformBuffer((uint32_t) index, bo->buffer.getGpuBuffer(), offset, size);
}

void VulkanDriver::bindBufferRange(BufferObjectBinding bindingType, uint32_t index,
        Handle<HwBufferObject> boh, uint32_t offset, uint32_t size) {

    assert_invariant(bindingType == BufferObjectBinding::SHADER_STORAGE ||
                     bindingType == BufferObjectBinding::UNIFORM);

    // TODO: implement BufferObjectBinding::SHADER_STORAGE case

    auto* bo = handle_cast<VulkanBufferObject*>(boh);
    mPipelineCache.bindUniformBuffer((uint32_t)index, bo->buffer.getGpuBuffer(), offset, size);
}

void VulkanDriver::unbindBuffer(BufferObjectBinding bindingType, uint32_t index) {
    // TODO: implement unbindBuffer()
}

void VulkanDriver::bindSamplers(uint32_t index, Handle<HwSamplerGroup> sbh) {
    auto* hwsb = handle_cast<VulkanSamplerGroup*>(sbh);
    mSamplerBindings[index] = hwsb;
}

void VulkanDriver::insertEventMarker(char const* string, uint32_t len) {
    constexpr float MARKER_COLOR[] = { 0.0f, 1.0f, 0.0f, 1.0f };
    VkCommandBuffer const cmdbuffer = mCommands->get().cmdbuffer;
    if (mContext.isDebugUtilsSupported()) {
        VkDebugUtilsLabelEXT labelInfo = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
            .pLabelName = string,
            .color = {1, 1, 0, 1},
        };
        vkCmdInsertDebugUtilsLabelEXT(cmdbuffer, &labelInfo);
    } else if (mContext.isDebugMarkersSupported()) {
        VkDebugMarkerMarkerInfoEXT markerInfo = {};
        markerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_MARKER_MARKER_INFO_EXT;
        memcpy(markerInfo.color, &MARKER_COLOR[0], sizeof(MARKER_COLOR));
        markerInfo.pMarkerName = string;
        vkCmdDebugMarkerInsertEXT(cmdbuffer, &markerInfo);
    }
}

void VulkanDriver::pushGroupMarker(char const* string, uint32_t len) {

#if FILAMENT_VULKAN_VERBOSE
    renderPassMarkers.push(std::string(string));
    utils::slog.d << "----> " << string << utils::io::endl;
#endif

    // TODO: Add group marker color to the Driver API
    constexpr float MARKER_COLOR[] = { 0.0f, 1.0f, 0.0f, 1.0f };
    const VkCommandBuffer cmdbuffer = mCommands->get().cmdbuffer;
    if (mContext.isDebugUtilsSupported()) {
        VkDebugUtilsLabelEXT labelInfo = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
            .pLabelName = string,
            .color = {0, 1, 0, 1},
        };
        vkCmdBeginDebugUtilsLabelEXT(cmdbuffer, &labelInfo);
        mCurrentDebugMarker = string;
    } else if (mContext.isDebugMarkersSupported()) {
        VkDebugMarkerMarkerInfoEXT markerInfo = {};
        markerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_MARKER_MARKER_INFO_EXT;
        memcpy(markerInfo.color, &MARKER_COLOR[0], sizeof(MARKER_COLOR));
        markerInfo.pMarkerName = string;
        vkCmdDebugMarkerBeginEXT(cmdbuffer, &markerInfo);
    }
}

void VulkanDriver::popGroupMarker(int) {

#if FILAMENT_VULKAN_VERBOSE
    std::string const& marker = renderPassMarkers.top();
    renderPassMarkers.pop();
    utils::slog.d << "<---- " << marker << utils::io::endl;
#endif

    const VkCommandBuffer cmdbuffer = mCommands->get().cmdbuffer;
    if (mContext.isDebugUtilsSupported()) {
        vkCmdEndDebugUtilsLabelEXT(cmdbuffer);
        mCurrentDebugMarker.clear();
    } else if (mContext.isDebugMarkersSupported()) {
        vkCmdDebugMarkerEndEXT(cmdbuffer);
    }
}

void VulkanDriver::startCapture(int) {

}

void VulkanDriver::stopCapture(int) {

}

void VulkanDriver::readPixels(Handle<HwRenderTarget> src, uint32_t x, uint32_t y,
        uint32_t width, uint32_t height, PixelBufferDescriptor&& pbd) {
    const VkDevice device = mPlatform->getDevice();
    VulkanRenderTarget* srcTarget = handle_cast<VulkanRenderTarget*>(src);
    VulkanTexture* srcTexture = (VulkanTexture*) srcTarget->getColor(0).texture;
    assert_invariant(srcTexture);
    const VkFormat srcFormat = srcTexture->getVkFormat();
    const bool swizzle = srcFormat == VK_FORMAT_B8G8R8A8_UNORM;

    // Create a host visible, linearly tiled image as a staging area.
    VkImageCreateInfo imageInfo {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = srcFormat,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = ImgUtil::getVkLayout(VulkanLayout::UNDEFINED),
    };

    VkImage stagingImage;
    vkCreateImage(device, &imageInfo, VKALLOC, &stagingImage);

#if FILAMENT_VULKAN_VERBOSE
    utils::slog.d << "readPixels created image=" << stagingImage
                  << " to copy from image=" << srcTexture->getVkImage()
                  << " src-layout=" << srcTexture->getLayout(0, 0) << utils::io::endl;
#endif

    VkMemoryRequirements memReqs;
    VkDeviceMemory stagingMemory;
    vkGetImageMemoryRequirements(device, stagingImage, &memReqs);
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = mContext.selectMemoryType(memReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
    };

    vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
    vkBindImageMemory(device, stagingImage, stagingMemory, 0);

    // TODO: don't flush/wait here, this should be asynchronous

    mCommands->flush();
    mCommands->wait();

    // Transition the staging image layout.
    const VkCommandBuffer cmdbuffer = mCommands->get().cmdbuffer;
    ImgUtil::transitionLayout(cmdbuffer, {
        .image = stagingImage,
        .oldLayout = VulkanLayout::UNDEFINED,
        .newLayout = VulkanLayout::TRANSFER_DST,
        .subresources = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });

    const VulkanAttachment srcAttachment = srcTarget->getColor(0);

    VkImageCopy imageCopyRegion = {
        .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = srcAttachment.level,
            .baseArrayLayer = srcAttachment.layer,
            .layerCount = 1,
        },
        .srcOffset = {
            .x = (int32_t) x,
            .y = (int32_t) (srcTarget->getExtent().height - (height + y)),
        },
        .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .extent = {
            .width = width,
            .height = height,
            .depth = 1,
        },
    };

    // Transition the source image layout (which might be the swap chain)

    const VkImageSubresourceRange srcRange
            = srcAttachment.getSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT);
    srcTexture->transitionLayout(cmdbuffer, srcRange, VulkanLayout::TRANSFER_SRC);

    // Perform the copy into the staging area. At this point we know that the src layout is
    // TRANSFER_SRC_OPTIMAL and the staging area is GENERAL.

    UTILS_UNUSED_IN_RELEASE VkExtent2D srcExtent = srcAttachment.getExtent2D();
    assert_invariant(imageCopyRegion.srcOffset.x + imageCopyRegion.extent.width <= srcExtent.width);
    assert_invariant(imageCopyRegion.srcOffset.y + imageCopyRegion.extent.height <= srcExtent.height);

    vkCmdCopyImage(cmdbuffer, srcAttachment.getImage(),
            ImgUtil::getVkLayout(VulkanLayout::TRANSFER_SRC), stagingImage,
            ImgUtil::getVkLayout(VulkanLayout::TRANSFER_DST), 1, &imageCopyRegion);

    // Restore the source image layout. Between driver API calls, color images are always kept in
    // UNDEFINED layout or in their "usage default" layout.

    srcTexture->transitionLayout(cmdbuffer, srcRange, VulkanLayout::COLOR_ATTACHMENT);

    // TODO: don't flush/wait here -- we should do this asynchronously

    // Flush and wait.
    mCommands->flush();
    mCommands->wait();

    VkImageSubresource subResource { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT };
    VkSubresourceLayout subResourceLayout;
    vkGetImageSubresourceLayout(device, stagingImage, &subResource, &subResourceLayout);

    // Map image memory so we can start copying from it.

    const uint8_t* srcPixels;
    vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, (void**) &srcPixels);
    srcPixels += subResourceLayout.offset;

    if (!DataReshaper::reshapeImage(&pbd, getComponentType(srcFormat), getComponentCount(srcFormat),
                srcPixels, subResourceLayout.rowPitch, width, height, swizzle)) {
        utils::slog.e << "Unsupported PixelDataFormat or PixelDataType" << utils::io::endl;
    }

    vkUnmapMemory(device, stagingMemory);
    vkDestroyImage(device, stagingImage, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    scheduleDestroy(std::move(pbd));
}

void VulkanDriver::readBufferSubData(backend::BufferObjectHandle boh,
        uint32_t offset, uint32_t size, backend::BufferDescriptor&& p) {
    // TODO: implement readBufferSubData
    scheduleDestroy(std::move(p));
}

void VulkanDriver::blit(TargetBufferFlags buffers, Handle<HwRenderTarget> dst, Viewport dstRect,
        Handle<HwRenderTarget> src, Viewport srcRect, SamplerMagFilter filter) {
    assert_invariant(mCurrentRenderPass.renderPass == VK_NULL_HANDLE);

    // blit operation only support COLOR0 color buffer
    assert_invariant(
            !(buffers & (TargetBufferFlags::COLOR_ALL & ~TargetBufferFlags::COLOR0)));

    if (UTILS_UNLIKELY(mCurrentRenderPass.renderPass)) {
        utils::slog.e << "Blits cannot be invoked inside a render pass." << utils::io::endl;
        return;
    }

    VulkanRenderTarget* dstTarget = handle_cast<VulkanRenderTarget*>(dst);
    VulkanRenderTarget* srcTarget = handle_cast<VulkanRenderTarget*>(src);

    VkFilter vkfilter = filter == SamplerMagFilter::NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

    // The Y inversion below makes it so that Vk matches GL and Metal.

    const VkExtent2D srcExtent = srcTarget->getExtent();
    const int32_t srcLeft = srcRect.left;
    const int32_t srcTop = srcExtent.height - srcRect.bottom - srcRect.height;
    const int32_t srcRight = srcRect.left + srcRect.width;
    const int32_t srcBottom = srcTop + srcRect.height;
    const VkOffset3D srcOffsets[2] = { { srcLeft, srcTop, 0 }, { srcRight, srcBottom, 1 }};

    const VkExtent2D dstExtent = dstTarget->getExtent();
    const int32_t dstLeft = dstRect.left;
    const int32_t dstTop = dstExtent.height - dstRect.bottom - dstRect.height;
    const int32_t dstRight = dstRect.left + dstRect.width;
    const int32_t dstBottom = dstTop + dstRect.height;
    const VkOffset3D dstOffsets[2] = { { dstLeft, dstTop, 0 }, { dstRight, dstBottom, 1 }};

    if (any(buffers & TargetBufferFlags::DEPTH) && srcTarget->hasDepth() && dstTarget->hasDepth()) {
        mBlitter.blitDepth({dstTarget, dstOffsets, srcTarget, srcOffsets});
    }

    if (any(buffers & TargetBufferFlags::COLOR0)) {
        mBlitter.blitColor({ dstTarget, dstOffsets, srcTarget, srcOffsets, vkfilter, int(0) });
    }
}

void VulkanDriver::draw(PipelineState pipelineState, Handle<HwRenderPrimitive> rph,
        const uint32_t instanceCount) {
    VulkanCommandBuffer const* commands = &mCommands->get();
    VkCommandBuffer cmdbuffer = commands->cmdbuffer;
    const VulkanRenderPrimitive& prim = *handle_cast<VulkanRenderPrimitive*>(rph);

    Handle<HwProgram> programHandle = pipelineState.program;
    RasterState rasterState = pipelineState.rasterState;
    PolygonOffset depthOffset = pipelineState.polygonOffset;
    const Viewport& scissorBox = pipelineState.scissor;

    auto* program = handle_cast<VulkanProgram*>(programHandle);
    mDisposer.acquire(program);
    mDisposer.acquire(prim.indexBuffer);
    mDisposer.acquire(prim.vertexBuffer);

    // If this is a debug build, validate the current shader.
#if !defined(NDEBUG)
    if (program->bundle.vertex == VK_NULL_HANDLE || program->bundle.fragment == VK_NULL_HANDLE) {
        utils::slog.e << "Binding missing shader: " << program->name.c_str() << utils::io::endl;
    }
#endif

    // Update the VK raster state.
    const VulkanRenderTarget* rt = mCurrentRenderPass.renderTarget;

    auto vkraster = mPipelineCache.getCurrentRasterState();
    vkraster.cullMode = getCullMode(rasterState.culling);
    vkraster.frontFace = getFrontFace(rasterState.inverseFrontFaces);
    vkraster.depthBiasEnable = (depthOffset.constant || depthOffset.slope) ? true : false;
    vkraster.depthBiasConstantFactor = depthOffset.constant;
    vkraster.depthBiasSlopeFactor = depthOffset.slope;
    vkraster.blendEnable = rasterState.hasBlending();
    vkraster.srcColorBlendFactor = getBlendFactor(rasterState.blendFunctionSrcRGB);
    vkraster.dstColorBlendFactor = getBlendFactor(rasterState.blendFunctionDstRGB);
    vkraster.colorBlendOp = rasterState.blendEquationRGB;
    vkraster.srcAlphaBlendFactor = getBlendFactor(rasterState.blendFunctionSrcAlpha);
    vkraster.dstAlphaBlendFactor = getBlendFactor(rasterState.blendFunctionDstAlpha);
    vkraster.alphaBlendOp =  rasterState.blendEquationAlpha;
    vkraster.colorWriteMask = (VkColorComponentFlags) (rasterState.colorWrite ? 0xf : 0x0);
    vkraster.depthWriteEnable = rasterState.depthWrite;
    vkraster.depthCompareOp = rasterState.depthFunc;
    vkraster.rasterizationSamples = rt->getSamples();
    vkraster.alphaToCoverageEnable = rasterState.alphaToCoverage;
    vkraster.colorTargetCount = rt->getColorTargetCount(mCurrentRenderPass);
    mPipelineCache.setCurrentRasterState(vkraster);

    // Declare fixed-size arrays that get passed to the pipeCache and to vkCmdBindVertexBuffers.
    VulkanPipelineCache::VertexArray varray = {};
    VkBuffer buffers[MAX_VERTEX_ATTRIBUTE_COUNT] = {};
    VkDeviceSize offsets[MAX_VERTEX_ATTRIBUTE_COUNT] = {};

    // For each attribute, append to each of the above lists.
    const uint32_t bufferCount = prim.vertexBuffer->attributes.size();
    for (uint32_t attribIndex = 0; attribIndex < bufferCount; attribIndex++) {
        Attribute attrib = prim.vertexBuffer->attributes[attribIndex];

        const bool isInteger = attrib.flags & Attribute::FLAG_INTEGER_TARGET;
        const bool isNormalized = attrib.flags & Attribute::FLAG_NORMALIZED;

        VkFormat vkformat = getVkFormat(attrib.type, isNormalized, isInteger);

        // HACK: Re-use the positions buffer as a dummy buffer for disabled attributes. Filament's
        // vertex shaders declare all attributes as either vec4 or uvec4 (the latter for bone
        // indices), and positions are always at least 32 bits per element. Therefore we can assign
        // a dummy type of either R8G8B8A8_UINT or R8G8B8A8_SNORM, depending on whether the shader
        // expects to receive floats or ints.
        if (attrib.buffer == Attribute::BUFFER_UNUSED) {
            vkformat = isInteger ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_SNORM;
            attrib = prim.vertexBuffer->attributes[0];
        }

        const VulkanBuffer* buffer = prim.vertexBuffer->buffers[attrib.buffer];

        // If the vertex buffer is missing a constituent buffer object, skip the draw call.
        // There is no need to emit an error message because this is not explicitly forbidden.
        if (buffer == nullptr) {
            return;
        }

        buffers[attribIndex] = buffer->getGpuBuffer();
        offsets[attribIndex] = attrib.offset;
        varray.attributes[attribIndex] = {
            .location = attribIndex, // matches the GLSL layout specifier
            .binding = attribIndex,  // matches the position within vkCmdBindVertexBuffers
            .format = vkformat,
        };
        varray.buffers[attribIndex] = {
            .binding = attribIndex,
            .stride = attrib.stride,
        };
    }

    // Push state changes to the VulkanPipelineCache instance. This is fast and does not make VK calls.
    mPipelineCache.bindProgram(*program);
    mPipelineCache.bindRasterState(mPipelineCache.getCurrentRasterState());
    mPipelineCache.bindPrimitiveTopology(prim.primitiveTopology);
    mPipelineCache.bindVertexArray(varray);

    // Query the program for the mapping from (SamplerGroupBinding,Offset) to (SamplerBinding),
    // where "SamplerBinding" is the integer in the GLSL, and SamplerGroupBinding is the abstract
    // Filament concept used to form groups of samplers.

    VkDescriptorImageInfo samplerInfo[VulkanPipelineCache::SAMPLER_BINDING_COUNT] = {};
    VulkanPipelineCache::UsageFlags usage;

    UTILS_NOUNROLL
    for (uint8_t samplerGroupIdx = 0; samplerGroupIdx < Program::SAMPLER_BINDING_COUNT; samplerGroupIdx++) {
        const auto& samplerGroup = program->samplerGroupInfo[samplerGroupIdx];
        const auto& samplers = samplerGroup.samplers;
        if (samplers.empty()) {
            continue;
        }
        VulkanSamplerGroup* vksb = mSamplerBindings[samplerGroupIdx];
        if (!vksb) {
            continue;
        }
        SamplerGroup* sb = vksb->sb.get();
        assert_invariant(sb->getSize() == samplers.size());
        size_t samplerIdx = 0;
        for (auto& sampler : samplers) {
            const SamplerDescriptor* boundSampler = sb->data() + samplerIdx;
            samplerIdx++;

            if (UTILS_LIKELY(boundSampler->t)) {
                VulkanTexture* texture = handle_cast<VulkanTexture*>(boundSampler->t);
                mDisposer.acquire(texture);

                // TODO: can this uninitialized check be checked in a higher layer?
                // This fallback path is very flaky because the dummy texture might not have
                // matching characteristics. (e.g. if the missing texture is a 3D texture)
                if (UTILS_UNLIKELY(texture->getPrimaryImageLayout() == VulkanLayout::UNDEFINED)) {
#ifndef NDEBUG
                    utils::slog.w << "Uninitialized texture bound to '" << sampler.name.c_str() << "'";
                    utils::slog.w << " in material '" << program->name.c_str() << "'";
                    utils::slog.w << " at binding point " << +sampler.binding << utils::io::endl;
#endif
                    // Calling get() won't leak here since `texture` is local.
                    texture = mEmptyTexture.get();
                }

                const SamplerParams& samplerParams = boundSampler->s;
                VkSampler vksampler = mSamplerCache.getSampler(samplerParams);

                usage = VulkanPipelineCache::getUsageFlags(sampler.binding, samplerGroup.stageFlags, usage);

                samplerInfo[sampler.binding] = {
                    .sampler = vksampler,
                    .imageView = texture->getPrimaryImageView(),
                    .imageLayout = ImgUtil::getVkLayout(texture->getPrimaryImageLayout())
                };
            }
        }
    }

    mPipelineCache.bindSamplers(samplerInfo, usage);

    // Bind new descriptor sets if they need to change.
    // If descriptor set allocation failed, skip the draw call and bail. No need to emit an error
    // message since the validation layers already do so.
    if (!mPipelineCache.bindDescriptors(cmdbuffer)) {
        return;
    }

    // Set scissoring.
    // clamp left-bottom to 0,0 and avoid overflows
    constexpr int32_t maxvali  = std::numeric_limits<int32_t>::max();
    constexpr uint32_t maxvalu  = std::numeric_limits<int32_t>::max();
    int32_t l = scissorBox.left;
    int32_t b = scissorBox.bottom;
    uint32_t w = std::min(maxvalu, scissorBox.width);
    uint32_t h = std::min(maxvalu, scissorBox.height);
    int32_t r = (l > int32_t(maxvalu - w)) ? maxvali : l + int32_t(w);
    int32_t t = (b > int32_t(maxvalu - h)) ? maxvali : b + int32_t(h);
    l = std::max(0, l);
    b = std::max(0, b);
    assert_invariant(r >= l && t >= b);
    VkRect2D scissor{
            .offset = { l, b },
            .extent = { uint32_t(r - l), uint32_t(t - b) }
    };

    rt->transformClientRectToPlatform(&scissor);
    mPipelineCache.bindScissor(cmdbuffer, scissor);

    // Bind a new pipeline if the pipeline state changed.
    // If allocation failed, skip the draw call and bail. We do not emit an error since the
    // validation layer will already do so.
    if (!mPipelineCache.bindPipeline(cmdbuffer)) {
        return;
    }

    // Next bind the vertex buffers and index buffer. One potential performance improvement is to
    // avoid rebinding these if they are already bound, but since we do not (yet) support subranges
    // it would be rare for a client to make consecutive draw calls with the same render primitive.
    vkCmdBindVertexBuffers(cmdbuffer, 0, bufferCount, buffers, offsets);
    vkCmdBindIndexBuffer(cmdbuffer, prim.indexBuffer->buffer.getGpuBuffer(), 0,
            prim.indexBuffer->indexType);

    // Finally, make the actual draw call. TODO: support subranges
    const uint32_t indexCount = prim.count;
    const uint32_t firstIndex = prim.offset / prim.indexBuffer->elementSize;
    const int32_t vertexOffset = 0;
    const uint32_t firstInstId = 0;
    vkCmdDrawIndexed(cmdbuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstId);
}

void VulkanDriver::dispatchCompute(Handle<HwProgram> program, math::uint3 workGroupCount) {
    // FIXME: implement me
}

void VulkanDriver::beginTimerQuery(Handle<HwTimerQuery> tqh) {
    VulkanTimerQuery* vtq = handle_cast<VulkanTimerQuery*>(tqh);
    mTimestamps->beginQuery(&(mCommands->get()), vtq);
}

void VulkanDriver::endTimerQuery(Handle<HwTimerQuery> tqh) {
    VulkanTimerQuery* vtq = handle_cast<VulkanTimerQuery*>(tqh);
    mTimestamps->endQuery(&(mCommands->get()), vtq);
}

void VulkanDriver::debugCommandBegin(CommandStream* cmds, bool synchronous, const char* methodName) noexcept {
    DriverBase::debugCommandBegin(cmds, synchronous, methodName);
#ifndef NDEBUG
    static const std::set<std::string_view> OUTSIDE_COMMANDS = {
        "loadUniformBuffer",
        "updateBufferObject",
        "updateIndexBuffer",
        "update3DImage",
    };
    static const std::string_view BEGIN_COMMAND = "beginRenderPass";
    static const std::string_view END_COMMAND = "endRenderPass";
    static bool inRenderPass = false; // for debug only
    const std::string_view command{ methodName };
    if (command == BEGIN_COMMAND) {
        assert_invariant(!inRenderPass);
        inRenderPass = true;
    } else if (command == END_COMMAND) {
        assert_invariant(inRenderPass);
        inRenderPass = false;
    } else if (inRenderPass && OUTSIDE_COMMANDS.find(command) != OUTSIDE_COMMANDS.end()) {
        utils::slog.e << command.data() << " issued inside a render pass." << utils::io::endl;
    }
#endif
}

void VulkanDriver::resetState(int) {
}

// explicit instantiation of the Dispatcher
template class ConcreteDispatcher<VulkanDriver>;

} // namespace filament::backend

#pragma clang diagnostic pop

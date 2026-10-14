# Source-owned Vulkan Ganesh backend. No system GL/EGL or host Vulkan library.
# Keep the list explicit so new upstream platform files are reviewed on update.
target_compile_definitions(Skia PUBLIC SK_VULKAN SK_USE_INTERNAL_VULKAN_HEADERS SK_BUILD_FOR_UNIX)
target_sources(Skia PRIVATE
    src/gpu/MutableTextureState.cpp
    src/gpu/ganesh/GrUniformDataManager.cpp
    src/gpu/ganesh/vk/GrVkBackendSemaphore.cpp
    src/gpu/ganesh/vk/GrVkBackendSurface.cpp
    src/gpu/ganesh/vk/GrVkBuffer.cpp
    src/gpu/ganesh/vk/GrVkCaps.cpp
    src/gpu/ganesh/vk/GrVkCommandBuffer.cpp
    src/gpu/ganesh/vk/GrVkCommandPool.cpp
    src/gpu/ganesh/vk/GrVkContextThreadSafeProxy.cpp
    src/gpu/ganesh/vk/GrVkDescriptorPool.cpp
    src/gpu/ganesh/vk/GrVkDescriptorSet.cpp
    src/gpu/ganesh/vk/GrVkDescriptorSetManager.cpp
    src/gpu/ganesh/vk/GrVkDirectContext.cpp
    src/gpu/ganesh/vk/GrVkFramebuffer.cpp
    src/gpu/ganesh/vk/GrVkGpu.cpp
    src/gpu/ganesh/vk/GrVkImage.cpp
    src/gpu/ganesh/vk/GrVkImageView.cpp
    src/gpu/ganesh/vk/GrVkMSAALoadManager.cpp
    src/gpu/ganesh/vk/GrVkOpsRenderPass.cpp
    src/gpu/ganesh/vk/GrVkPipeline.cpp
    src/gpu/ganesh/vk/GrVkPipelineState.cpp
    src/gpu/ganesh/vk/GrVkPipelineStateBuilder.cpp
    src/gpu/ganesh/vk/GrVkPipelineStateCache.cpp
    src/gpu/ganesh/vk/GrVkPipelineStateDataManager.cpp
    src/gpu/ganesh/vk/GrVkRenderPass.cpp
    src/gpu/ganesh/vk/GrVkRenderTarget.cpp
    src/gpu/ganesh/vk/GrVkResourceProvider.cpp
    src/gpu/ganesh/vk/GrVkSampler.cpp
    src/gpu/ganesh/vk/GrVkSamplerYcbcrConversion.cpp
    src/gpu/ganesh/vk/GrVkSecondaryCBDrawContext.cpp
    src/gpu/ganesh/vk/GrVkSemaphore.cpp
    src/gpu/ganesh/vk/GrVkTexture.cpp
    src/gpu/ganesh/vk/GrVkTextureRenderTarget.cpp
    src/gpu/ganesh/vk/GrVkTypesPriv.cpp
    src/gpu/ganesh/vk/GrVkUniformHandler.cpp
    src/gpu/ganesh/vk/GrVkUtil.cpp
    src/gpu/ganesh/vk/GrVkVaryingHandler.cpp
    src/gpu/vk/VulkanExtensions.cpp
    src/gpu/vk/VulkanInterface.cpp
    src/gpu/vk/VulkanMemory.cpp
    src/gpu/vk/VulkanMutableTextureState.cpp
    src/gpu/vk/VulkanPreferredFeatures.cpp
    src/gpu/vk/VulkanUtilsPriv.cpp
)

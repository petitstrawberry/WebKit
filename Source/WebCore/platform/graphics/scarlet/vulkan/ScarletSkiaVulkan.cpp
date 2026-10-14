// Native Skia/Vulkan contract probe. No platform GL, direct SGFX, or CPU surface.
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkString.h>
#include <skia/gpu/ganesh/GrDirectContext.h>
#include <skia/gpu/ganesh/GrContextOptions.h>
#include <skia/gpu/ganesh/SkSurfaceGanesh.h>
#include <skia/gpu/ganesh/vk/GrVkDirectContext.h>
#include <skia/gpu/ganesh/vk/GrVkBackendSurface.h>
#include <skia/gpu/ganesh/vk/GrVkTypes.h>
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/vk/VulkanBackendContext.h>
#include <skia/gpu/vk/VulkanExtensions.h>
#include <skia/gpu/vk/VulkanMemoryAllocator.h>
#include <dlfcn.h>
#include <cstdio>
#include <memory>
#include <vector>
#include <unistd.h>

namespace {
struct Dispatch {
    PFN_vkGetInstanceProcAddr instanceProc { };
    PFN_vkGetDeviceProcAddr deviceProc { };
    VkInstance instance { };
    VkDevice device { };
    template<typename T> T instanceFunction(const char* name) const
    { return reinterpret_cast<T>(instanceProc(instance, name)); }
    template<typename T> T deviceFunction(const char* name) const
    { return reinterpret_cast<T>(deviceProc(device, name)); }
};

// A small coherent allocator for the native ICD's unified memory. Ganesh itself
// pools its vertex/uniform/staging buffers. Unsupported properties fail instead
// of claiming protected/lazy or noncoherent allocations were honored.
class Allocator final : public skgpu::VulkanMemoryAllocator {
    struct Allocation { skgpu::VulkanAlloc info; void* mapped { }; };
public:
    Allocator(Dispatch dispatch, VkPhysicalDevice physical) : m_dispatch(dispatch)
    { dispatch.instanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &m_properties); }
    VkResult allocateImageMemory(VkImage image, uint32_t flags, skgpu::VulkanBackendMemory* memory) override
    {
        VkMemoryRequirements requirements { };
        m_dispatch.deviceFunction<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(m_dispatch.device, image, &requirements);
        return allocate(requirements, 0, flags, memory);
    }
    VkResult allocateBufferMemory(VkBuffer buffer, BufferUsage usage, uint32_t flags, skgpu::VulkanBackendMemory* memory) override
    {
        VkMemoryRequirements requirements { };
        m_dispatch.deviceFunction<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(m_dispatch.device, buffer, &requirements);
        auto required = usage == BufferUsage::kGpuOnly ? 0u : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        return allocate(requirements, required, flags, memory);
    }
    void getAllocInfo(const skgpu::VulkanBackendMemory& memory, skgpu::VulkanAlloc* info) const override
    { *info = reinterpret_cast<Allocation*>(memory)->info; }
    VkResult mapMemory(const skgpu::VulkanBackendMemory& memory, void** pointer) override
    {
        auto& allocation = *reinterpret_cast<Allocation*>(memory);
        if (!(allocation.info.fFlags & skgpu::VulkanAlloc::kMappable_Flag)) return VK_ERROR_MEMORY_MAP_FAILED;
        if (!allocation.mapped) {
            auto result = m_dispatch.deviceFunction<PFN_vkMapMemory>("vkMapMemory")(m_dispatch.device, allocation.info.fMemory, 0, VK_WHOLE_SIZE, 0, &allocation.mapped);
            if (result != VK_SUCCESS) return result;
        }
        *pointer = allocation.mapped;
        return VK_SUCCESS;
    }
    void unmapMemory(const skgpu::VulkanBackendMemory& memory) override
    {
        auto& allocation = *reinterpret_cast<Allocation*>(memory);
        if (allocation.mapped) {
            m_dispatch.deviceFunction<PFN_vkUnmapMemory>("vkUnmapMemory")(m_dispatch.device, allocation.info.fMemory);
            allocation.mapped = nullptr;
        }
    }
    void freeMemory(const skgpu::VulkanBackendMemory& memory) override
    {
        auto* allocation = reinterpret_cast<Allocation*>(memory);
        unmapMemory(memory);
        m_dispatch.deviceFunction<PFN_vkFreeMemory>("vkFreeMemory")(m_dispatch.device, allocation->info.fMemory, nullptr);
        m_bytes -= allocation->info.fSize;
        delete allocation;
    }
    std::pair<uint64_t, uint64_t> totalAllocatedAndUsedMemory() const override { return { m_bytes, m_bytes }; }
private:
    VkResult allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags required, uint32_t flags, skgpu::VulkanBackendMemory* memory)
    {
        *memory = 0;
        if (flags & kProtected_AllocationPropertyFlag) return VK_ERROR_FEATURE_NOT_PRESENT;
        for (uint32_t type = 0; type < m_properties.memoryTypeCount; ++type) {
            auto properties = m_properties.memoryTypes[type].propertyFlags;
            if (!(requirements.memoryTypeBits & (1u << type)) || (properties & required) != required) continue;
            // Coherence is required whenever the chosen memory is host-visible.
            if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
            VkMemoryAllocateInfo info { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = type;
            auto allocation = std::make_unique<Allocation>();
            auto result = m_dispatch.deviceFunction<PFN_vkAllocateMemory>("vkAllocateMemory")(m_dispatch.device, &info, nullptr, &allocation->info.fMemory);
            if (result != VK_SUCCESS) return result;
            allocation->info.fSize = requirements.size;
            if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) allocation->info.fFlags |= skgpu::VulkanAlloc::kMappable_Flag;
            auto* raw = allocation.release();
            *memory = reinterpret_cast<skgpu::VulkanBackendMemory>(raw);
            raw->info.fBackendMemory = *memory;
            m_bytes += requirements.size;
            if (flags & kPersistentlyMapped_AllocationPropertyFlag) {
                void* pointer;
                result = mapMemory(*memory, &pointer);
                if (result != VK_SUCCESS) { freeMemory(*memory); *memory = 0; return result; }
            }
            return VK_SUCCESS;
        }
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    Dispatch m_dispatch;
    VkPhysicalDeviceMemoryProperties m_properties { };
    uint64_t m_bytes { };
};

class Context {
public:
    ~Context()
    {
        surface = nullptr;
        context = nullptr;
        if (exportedHandle >= 0) close(exportedHandle);
        if (sharedImage) dispatch.deviceFunction<PFN_vkDestroyImage>("vkDestroyImage")(dispatch.device, sharedImage, nullptr);
        if (sharedMemory) allocator->freeMemory(sharedMemory);
        allocator = nullptr;
        if (dispatch.device) dispatch.deviceFunction<PFN_vkDestroyDevice>("vkDestroyDevice")(dispatch.device, nullptr);
        if (dispatch.instance) dispatch.instanceFunction<PFN_vkDestroyInstance>("vkDestroyInstance")(dispatch.instance, nullptr);
        if (library) dlclose(library);
    }
    void* library { };
    Dispatch dispatch;
    sk_sp<Allocator> allocator;
    sk_sp<GrDirectContext> context;
    sk_sp<SkSurface> surface;
    VkImage sharedImage { };
    skgpu::VulkanBackendMemory sharedMemory { };
    int32_t exportedHandle { -1 };
};

int probe()
{
    char scalar[kSkStrAppendScalar_MaxSize + 1];
    auto* scalarEnd = SkStrAppendScalar(scalar, 1.25f);
    *scalarEnd = '\0';
    if (SkString(scalar) != SkString("1.25")) {
        std::fprintf(stderr, "SKIA_VULKAN_FAIL scalar conversion\n");
        return 17;
    }
    auto cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus <= 0) { std::fprintf(stderr, "SKIA_VULKAN_FAIL online CPU query\n"); return 16; }
    std::fprintf(stderr, "SKIA_VULKAN_CPUS %ld\n", cpus);
    Context owner;
    owner.library = dlopen("/lib/libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!owner.library) { std::fprintf(stderr, "SKIA_VULKAN_FAIL loader: %s\n", dlerror()); return 1; }
    auto& dispatch = owner.dispatch;
    dispatch.instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(owner.library, "vkGetInstanceProcAddr"));
    if (!dispatch.instanceProc) return 2;
    VkApplicationInfo application { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    application.pApplicationName = "Myrica Skia Vulkan probe";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &application;
    auto result = dispatch.instanceFunction<PFN_vkCreateInstance>("vkCreateInstance")(&info, nullptr, &dispatch.instance);
    if (result != VK_SUCCESS) { std::fprintf(stderr, "SKIA_VULKAN_FAIL instance: %d\n", result); return 3; }
    auto enumerate = dispatch.instanceFunction<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    uint32_t count = 0;
    if (enumerate(dispatch.instance, &count, nullptr) != VK_SUCCESS || !count) return 4;
    std::vector<VkPhysicalDevice> physicals(count);
    if (enumerate(dispatch.instance, &count, physicals.data()) != VK_SUCCESS) return 4;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t queueIndex = 0;
    for (auto candidate : physicals) {
        VkPhysicalDeviceProperties properties { };
        dispatch.instanceFunction<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate, &properties);
        std::fprintf(stderr, "SKIA_VULKAN_DEVICE %s api=%u.%u\n", properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));
        if (properties.apiVersion < VK_API_VERSION_1_1) continue;
        auto queues = dispatch.instanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        count = 0; queues(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        queues(candidate, &count, families.data());
        for (uint32_t i = 0; i < count; ++i) {
            if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { physical = candidate; queueIndex = i; break; }
        }
        if (physical) break;
    }
    if (!physical) { std::fprintf(stderr, "SKIA_VULKAN_FAIL requires Vulkan 1.1 graphics device\n"); return 5; }
    float priority = 1;
    VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = queueIndex; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
    const char* imageExtension = "VK_SGFX_scarlet_image";
    deviceInfo.enabledExtensionCount = 1; deviceInfo.ppEnabledExtensionNames = &imageExtension;
    result = dispatch.instanceFunction<PFN_vkCreateDevice>("vkCreateDevice")(physical, &deviceInfo, nullptr, &dispatch.device);
    if (result != VK_SUCCESS) { std::fprintf(stderr, "SKIA_VULKAN_FAIL device: %d\n", result); return 6; }
    dispatch.deviceProc = dispatch.instanceFunction<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    VkQueue queue;
    dispatch.deviceFunction<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(dispatch.device, queueIndex, 0, &queue);
    skgpu::VulkanBackendContext backend;
    backend.fInstance = dispatch.instance; backend.fPhysicalDevice = physical; backend.fDevice = dispatch.device;
    backend.fQueue = queue; backend.fGraphicsQueueIndex = queueIndex; backend.fMaxAPIVersion = VK_API_VERSION_1_1;
    backend.fGetProc = [dispatch](const char* name, VkInstance instance, VkDevice device) -> PFN_vkVoidFunction {
        auto function = device ? dispatch.deviceProc(device, name) : dispatch.instanceProc(instance, name);
        if (!function) std::fprintf(stderr, "SKIA_VULKAN_MISSING_PROC %s\n", name);
        return function;
    };
    skgpu::VulkanExtensions extensions;
    extensions.init(backend.fGetProc, dispatch.instance, physical, 0, nullptr, 1, &imageExtension);
    backend.fVkExtensions = &extensions;
    owner.allocator = sk_make_sp<Allocator>(dispatch, physical);
    backend.fMemoryAllocator = owner.allocator;
    GrContextOptions options;
    bool stencil = false;
    for (auto format : { VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT }) {
        VkFormatProperties properties { };
        dispatch.instanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical, format, &properties);
        stencil |= properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    options.fAvoidStencilBuffers = !stencil;
    owner.context = GrDirectContexts::MakeVulkan(backend, options);
    if (!owner.context) { std::fprintf(stderr, "SKIA_VULKAN_FAIL Ganesh context\n"); return 7; }
    const auto imageInfo = SkImageInfo::Make(128, 96, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    // Export a bound, untouched Vulkan image before Ganesh can submit it.
    // Wrap the same physical image, so the GPU draw/readback exercises the
    // SGFX sharing path rather than a separate CPU copy of an internal surface.
    VkImageCreateInfo imageCreate { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageCreate.imageType = VK_IMAGE_TYPE_2D;
    imageCreate.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageCreate.extent = { 128, 96, 1 };
    imageCreate.mipLevels = imageCreate.arrayLayers = 1;
    imageCreate.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreate.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreate.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreate.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dispatch.deviceFunction<PFN_vkCreateImage>("vkCreateImage")(dispatch.device, &imageCreate, nullptr, &owner.sharedImage) != VK_SUCCESS) return 11;
    if (owner.allocator->allocateImageMemory(owner.sharedImage, 0, &owner.sharedMemory) != VK_SUCCESS) return 12;
    GrVkImageInfo wrapped;
    owner.allocator->getAllocInfo(owner.sharedMemory, &wrapped.fAlloc);
    if (dispatch.deviceFunction<PFN_vkBindImageMemory>("vkBindImageMemory")(dispatch.device, owner.sharedImage, wrapped.fAlloc.fMemory, wrapped.fAlloc.fOffset) != VK_SUCCESS) return 13;
    if (dispatch.deviceFunction<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue) != VK_SUCCESS) return 14;
    using ExportImage = VkResult (*)(VkDevice, VkImage, int32_t*);
    auto exportImage = dispatch.deviceFunction<ExportImage>("vkGetImageScarletHandleSGFX");
    if (!exportImage || exportImage(dispatch.device, owner.sharedImage, &owner.exportedHandle) != VK_SUCCESS || owner.exportedHandle < 0) return 15;
    wrapped.fImage = owner.sharedImage;
    wrapped.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    wrapped.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    wrapped.fFormat = imageCreate.format;
    wrapped.fImageUsageFlags = imageCreate.usage;
    wrapped.fSampleCount = 1;
    wrapped.fLevelCount = 1;
    wrapped.fCurrentQueueFamily = queueIndex;
    auto renderTarget = GrBackendRenderTargets::MakeVk(128, 96, wrapped);
    owner.surface = SkSurfaces::WrapBackendRenderTarget(owner.context.get(), renderTarget, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, SkColorSpace::MakeSRGB(), nullptr);
    if (!owner.surface) { std::fprintf(stderr, "SKIA_VULKAN_FAIL shared GPU surface\n"); return 8; }
    owner.surface->getCanvas()->clear(SK_ColorWHITE);
    SkPaint paint; paint.setColor(SK_ColorRED);
    owner.surface->getCanvas()->drawRect(SkRect::MakeXYWH(16, 16, 32, 32), paint);
    paint.setColor(SK_ColorBLUE);
    owner.surface->getCanvas()->drawRect(SkRect::MakeXYWH(64, 32, 32, 32), paint);
    owner.context->flush(owner.surface.get());
    if (!owner.context->submit(GrSyncCpu::kYes)) {
        std::fprintf(stderr, "SKIA_VULKAN_FAIL draw submit abandoned=%d\n", owner.context->abandoned());
        return 18;
    }
    std::vector<uint32_t> pixels(128 * 96);
    if (!owner.surface->readPixels(imageInfo, pixels.data(), 128 * 4, 0, 0)) { std::fprintf(stderr, "SKIA_VULKAN_FAIL GPU readback abandoned=%d\n", owner.context->abandoned()); return 9; }
    // RGBA byte order, independent of Skia's native N32 order.
    auto* bytes = reinterpret_cast<const unsigned char*>(pixels.data());
    auto matches = [bytes](int x, int y, unsigned char r, unsigned char g, unsigned char b) {
        auto p = bytes + (y * 128 + x) * 4;
        return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
    };
    if (!matches(0, 0, 255, 255, 255) || !matches(24, 24, 255, 0, 0) || !matches(72, 40, 0, 0, 255)) {
        std::fprintf(stderr, "SKIA_VULKAN_FAIL GPU draw pixel mismatch\n"); return 10;
    }
    std::fprintf(stderr, "SKIA_VULKAN_PASS Ganesh rectangles on shared SGFX GPU image, handle=%d memory=%llu\n", owner.exportedHandle, static_cast<unsigned long long>(owner.allocator->totalAllocatedAndUsedMemory().first));
    return 0;
}
}

extern "C" int scarlet_webview_vulkan_probe() { return probe(); }

// Initialized C++ ELF TLS must be copied for every native thread. This also
// checks that Rust's namespace header and C errno survive C++ TLS writes.
static thread_local uint64_t tlsCounter = 17;
extern "C" uint64_t scarlet_webview_tls_value() { return tlsCounter; }
extern "C" uint64_t scarlet_webview_tls_increment() { return ++tlsCounter; }

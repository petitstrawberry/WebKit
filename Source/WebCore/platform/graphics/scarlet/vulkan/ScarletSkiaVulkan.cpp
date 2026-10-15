// Native Ganesh Vulkan device and owned shared GPU surfaces.
#include "cmakeconfig.h"
#include "ScarletSkiaVulkan.h"
#include <wtf/RedBlackTree.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkString.h>
#include <skia/core/SkYUVAPixmaps.h>
#include <skia/gpu/ganesh/SkImageGanesh.h>
#include <array>
#include <algorithm>
#include <cstring>
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
#include <thread>
#include <cstdlib>
#include <mutex>

namespace WebCore {
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
    struct Allocation { skgpu::VulkanAlloc info; void* mapped { }; bool persistentlyMapped { false }; };
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
        // Ganesh's transfer buffers request a mapping that remains valid until
        // allocation destruction, including across staging-buffer reuse.
        if (allocation.persistentlyMapped)
            return;
        if (allocation.mapped) {
            m_dispatch.deviceFunction<PFN_vkUnmapMemory>("vkUnmapMemory")(m_dispatch.device, allocation.info.fMemory);
            allocation.mapped = nullptr;
        }
    }
    void freeMemory(const skgpu::VulkanBackendMemory& memory) override
    {
        auto* allocation = reinterpret_cast<Allocation*>(memory);
        // Final destruction also retires a persistent mapping, exactly once.
        allocation->persistentlyMapped = false;
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
            allocation->persistentlyMapped = flags & kPersistentlyMapped_AllocationPropertyFlag;
            auto result = m_dispatch.deviceFunction<PFN_vkAllocateMemory>("vkAllocateMemory")(m_dispatch.device, &info, nullptr, &allocation->info.fMemory);
            if (result != VK_SUCCESS) return result;
            allocation->info.fSize = requirements.size;
            if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) allocation->info.fFlags |= skgpu::VulkanAlloc::kMappable_Flag;
            auto* raw = allocation.release();
            *memory = reinterpret_cast<skgpu::VulkanBackendMemory>(raw);
            raw->info.fBackendMemory = *memory;
            m_bytes += requirements.size;
            if (raw->persistentlyMapped) {
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

// Contexts remain thread confined. Only Vulkan device/queue lifetime is shared;
// immutable textures can consequently be promised to the compositor context.
static std::mutex queueMutex;
static PFN_vkQueueSubmit queueSubmit;
static PFN_vkQueueWaitIdle queueWaitIdle;
static PFN_vkDeviceWaitIdle deviceWaitIdle;
static VKAPI_ATTR VkResult VKAPI_CALL synchronizedQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f)
{ std::lock_guard lock(queueMutex); return queueSubmit(q, n, s, f); }
static VKAPI_ATTR VkResult VKAPI_CALL synchronizedQueueWaitIdle(VkQueue q)
{ std::lock_guard lock(queueMutex); return queueWaitIdle(q); }
static VKAPI_ATTR VkResult VKAPI_CALL synchronizedDeviceWaitIdle(VkDevice d)
{ std::lock_guard lock(queueMutex); return deviceWaitIdle(d); }
struct SharedVulkanDevice {
    void* library { };
    Dispatch dispatch;
    VkPhysicalDevice physical { };
    VkQueue queue { };
    uint32_t queueIndex { };
    ~SharedVulkanDevice()
    {
        if (dispatch.device) dispatch.deviceFunction<PFN_vkDestroyDevice>("vkDestroyDevice")(dispatch.device, nullptr);
        if (dispatch.instance) dispatch.instanceFunction<PFN_vkDestroyInstance>("vkDestroyInstance")(dispatch.instance, nullptr);
        if (library) dlclose(library);
    }
};
struct ScarletSkiaVulkan::State {
    ~State()
    {
        checkThread();
        context = nullptr;
        allocator = nullptr;
    }
    std::shared_ptr<SharedVulkanDevice> shared;
    Dispatch dispatch;
    sk_sp<Allocator> allocator;
    sk_sp<GrDirectContext> context;
    VkQueue queue { };
    uint32_t queueIndex { };
    std::thread::id ownerThread { std::this_thread::get_id() };
    void checkThread() const { if (ownerThread != std::this_thread::get_id()) std::abort(); }
};

static int initializeDevice(SharedVulkanDevice& owner)
{
    owner.library = dlopen("/lib/libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!owner.library) { std::fprintf(stderr, "SKIA_VULKAN_FAIL loader: %s\n", dlerror()); return 1; }
    Dispatch& dispatch = owner.dispatch;
    dispatch.instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(owner.library, "vkGetInstanceProcAddr"));
    if (!dispatch.instanceProc) return 2;
    VkApplicationInfo application { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    application.pApplicationName = "WebCore Scarlet Skia Vulkan";
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
    owner.queue = queue;
    owner.queueIndex = queueIndex;
    owner.physical = physical;
    queueSubmit = dispatch.deviceFunction<PFN_vkQueueSubmit>("vkQueueSubmit");
    queueWaitIdle = dispatch.deviceFunction<PFN_vkQueueWaitIdle>("vkQueueWaitIdle");
    deviceWaitIdle = dispatch.deviceFunction<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    return 0;
}
static std::shared_ptr<SharedVulkanDevice> sharedDevice()
{
    static std::mutex mutex;
    static std::weak_ptr<SharedVulkanDevice> device;
    std::lock_guard lock(mutex);
    if (auto existing = device.lock())
        return existing;
    auto created = std::make_shared<SharedVulkanDevice>();
    if (initializeDevice(*created))
        return nullptr;
    device = created;
    return created;
}
template<typename DeviceState>
int initialize(DeviceState& owner)
{
    owner.shared = sharedDevice();
    if (!owner.shared)
        return 1;
    auto dispatch = owner.shared->dispatch;
    owner.dispatch = dispatch;
    auto physical = owner.shared->physical;
    auto queue = owner.shared->queue;
    auto queueIndex = owner.shared->queueIndex;
    owner.queue = queue;
    owner.queueIndex = queueIndex;
    const char* imageExtension = "VK_SGFX_scarlet_image";
    skgpu::VulkanBackendContext backend;
    backend.fInstance = dispatch.instance; backend.fPhysicalDevice = physical; backend.fDevice = dispatch.device;
    backend.fQueue = queue; backend.fGraphicsQueueIndex = queueIndex; backend.fMaxAPIVersion = VK_API_VERSION_1_1;
    backend.fGetProc = [dispatch](const char* name, VkInstance instance, VkDevice device) -> PFN_vkVoidFunction {
        if (!std::strcmp(name, "vkQueueSubmit")) return reinterpret_cast<PFN_vkVoidFunction>(synchronizedQueueSubmit);
        if (!std::strcmp(name, "vkQueueWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(synchronizedQueueWaitIdle);
        if (!std::strcmp(name, "vkDeviceWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(synchronizedDeviceWaitIdle);
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
    // SGFX has 256 shader slots; a graphics pipeline can need two distinct
    // shaders. Leave headroom beyond the cache for construction and command
    // buffers retaining evicted pipelines. This bounds cache retention, not
    // the total live pipelines of an arbitrarily complex recording.
    options.fRuntimeProgramCacheSize = 64;
    bool stencil = false;
    for (auto format : { VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT }) {
        VkFormatProperties properties { };
        dispatch.instanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(physical, format, &properties);
        stencil |= properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    options.fAvoidStencilBuffers = !stencil;
    owner.context = GrDirectContexts::MakeVulkan(backend, options);
    if (!owner.context) { std::fprintf(stderr, "SKIA_VULKAN_FAIL Ganesh context\n"); return 7; }
    return 0;
}
struct ScarletSkiaVulkan::Surface::State {
    std::shared_ptr<ScarletSkiaVulkan> owner;
    sk_sp<SkSurface> surface;
    VkImage image { };
    skgpu::VulkanBackendMemory memory { };
    int32_t exportedHandle { -1 };
    ~State()
    {
        auto& device = *owner->m_state;
        device.checkThread();
        surface = nullptr;
        // Finish commands referencing borrowed storage before freeing it.
        if (device.context) {
            device.context->flush();
            device.context->submit(GrSyncCpu::kYes);
        }
        if (image) device.dispatch.deviceFunction<PFN_vkDestroyImage>("vkDestroyImage")(device.dispatch.device, image, nullptr);
        if (memory) device.allocator->freeMemory(memory);
        if (exportedHandle >= 0) close(exportedHandle);
    }
};
ScarletSkiaVulkan::ScarletSkiaVulkan(std::unique_ptr<State>&& state) : m_state(std::move(state)) { }
ScarletSkiaVulkan::~ScarletSkiaVulkan() = default;
ScarletSkiaVulkan::Surface::Surface(std::unique_ptr<State>&& state) : m_state(std::move(state)) { }
ScarletSkiaVulkan::Surface::~Surface() = default;
std::shared_ptr<ScarletSkiaVulkan> ScarletSkiaVulkan::create()
{
    auto state = std::make_unique<State>();
    if (auto status = initialize(*state)) {
        std::fprintf(stderr, "SCARLET_SKIA_VULKAN_FAIL initialize=%d\n", status);
        return nullptr;
    }
    return std::shared_ptr<ScarletSkiaVulkan>(new ScarletSkiaVulkan(std::move(state)));
}
GrDirectContext* ScarletSkiaVulkan::grContext() const
{
    m_state->checkThread();
    return m_state->context.get();
}
SkSurface& ScarletSkiaVulkan::Surface::skSurface() const
{
    m_state->owner->m_state->checkThread();
    return *m_state->surface;
}
int32_t ScarletSkiaVulkan::Surface::duplicateHandle() const
{
    m_state->owner->m_state->checkThread();
    return dup(m_state->exportedHandle);
}
uint64_t ScarletSkiaVulkan::allocatedBytes() const
{
    m_state->checkThread();
    return m_state->allocator->totalAllocatedAndUsedMemory().first;
}
struct ScarletSkiaVulkan::Submission::State {
    struct Completion {
        bool finished { false };
    };
    std::shared_ptr<ScarletSkiaVulkan> owner;
    // The callback owns only this small record, not the device (no owner cycle).
    std::shared_ptr<Completion> completion { std::make_shared<Completion>() };
    bool submitted { false };
};
ScarletSkiaVulkan::Submission::Submission(std::unique_ptr<State>&& state)
    : m_state(std::move(state)) { }
ScarletSkiaVulkan::Submission::~Submission()
{
    auto& device = *m_state->owner->m_state;
    device.checkThread();
    // Cancellation/resize/shutdown cannot free borrowed output memory early.
    // Regular completion polling never waits here: finished tokens retire first.
    if (m_state->submitted && !m_state->completion->finished && !device.context->abandoned()) {
        device.context->submit(GrSyncCpu::kYes);
        device.context->checkAsyncWorkCompletion();
    }
}
std::unique_ptr<ScarletSkiaVulkan::Submission> ScarletSkiaVulkan::submitAsync(Surface& surface)
{
    m_state->checkThread();
    if (surface.m_state->owner.get() != this || m_state->context->abandoned())
        return nullptr;
    auto state = std::make_unique<Submission::State>();
    state->owner = shared_from_this();
    GrFlushInfo info;
    using Completion = std::shared_ptr<Submission::State::Completion>;
    info.fFinishedContext = new Completion(state->completion);
    info.fFinishedProc = [](void* opaque) {
        std::unique_ptr<Completion> completion(static_cast<Completion*>(opaque));
        (*completion)->finished = true;
    };
    m_state->context->flush(surface.m_state->surface.get(), info);
    state->submitted = m_state->context->submit(GrSyncCpu::kNo) && !m_state->context->abandoned();
    return std::unique_ptr<Submission>(new Submission(std::move(state)));
}
ScarletSkiaVulkan::SubmissionStatus ScarletSkiaVulkan::submissionStatus(Submission& submission)
{
    m_state->checkThread();
    if (submission.m_state->owner.get() != this || !submission.m_state->submitted || m_state->context->abandoned())
        return SubmissionStatus::Failed;
    // Ganesh invokes finished callbacks here, on this context's sole owner.
    m_state->context->checkAsyncWorkCompletion();
    if (m_state->context->abandoned())
        return SubmissionStatus::Failed;
    return submission.m_state->completion->finished ? SubmissionStatus::Complete : SubmissionStatus::Pending;
}
bool ScarletSkiaVulkan::submit(Surface& surface)
{
    m_state->checkThread();
    if (surface.m_state->owner.get() != this || m_state->context->abandoned()) return false;
    m_state->context->flush(surface.m_state->surface.get());
    return m_state->context->submit(GrSyncCpu::kYes) && !m_state->context->abandoned();
}
std::unique_ptr<ScarletSkiaVulkan::Surface> ScarletSkiaVulkan::createSurface(uint32_t width, uint32_t height, const SkSurfaceProps* properties)
{
    m_state->checkThread();
    if (!width || !height || width > static_cast<uint32_t>(m_state->context->maxRenderTargetSize()) || height > static_cast<uint32_t>(m_state->context->maxRenderTargetSize()) || m_state->context->abandoned()) return nullptr;
    auto state = std::make_unique<Surface::State>();
    state->owner = shared_from_this();
    auto& dispatch = m_state->dispatch;
    VkImageCreateInfo imageCreate { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageCreate.imageType = VK_IMAGE_TYPE_2D;
    imageCreate.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageCreate.extent = { width, height, 1 };
    imageCreate.mipLevels = imageCreate.arrayLayers = 1;
    imageCreate.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreate.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreate.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreate.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dispatch.deviceFunction<PFN_vkCreateImage>("vkCreateImage")(dispatch.device, &imageCreate, nullptr, &state->image) != VK_SUCCESS) return nullptr;
    if (m_state->allocator->allocateImageMemory(state->image, 0, &state->memory) != VK_SUCCESS) return nullptr;
    GrVkImageInfo wrapped;
    m_state->allocator->getAllocInfo(state->memory, &wrapped.fAlloc);
    if (dispatch.deviceFunction<PFN_vkBindImageMemory>("vkBindImageMemory")(dispatch.device, state->image, wrapped.fAlloc.fMemory, wrapped.fAlloc.fOffset) != VK_SUCCESS) return nullptr;
    if (synchronizedQueueWaitIdle(m_state->queue) != VK_SUCCESS) return nullptr;
    using ExportImage = VkResult (*)(VkDevice, VkImage, int32_t*);
    auto exportImage = dispatch.deviceFunction<ExportImage>("vkGetImageScarletHandleSGFX");
    if (!exportImage || exportImage(dispatch.device, state->image, &state->exportedHandle) != VK_SUCCESS || state->exportedHandle < 0) return nullptr;
    wrapped.fImage = state->image;
    wrapped.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    wrapped.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    wrapped.fFormat = imageCreate.format;
    wrapped.fImageUsageFlags = imageCreate.usage;
    wrapped.fSampleCount = 1;
    wrapped.fLevelCount = 1;
    wrapped.fCurrentQueueFamily = m_state->queueIndex;
    auto renderTarget = GrBackendRenderTargets::MakeVk(width, height, wrapped);
    state->surface = SkSurfaces::WrapBackendRenderTarget(m_state->context.get(), renderTarget, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, SkColorSpace::MakeSRGB(), properties);
    if (!state->surface) { std::fprintf(stderr, "SKIA_VULKAN_FAIL shared GPU surface\n"); return nullptr; }
    return std::unique_ptr<Surface>(new Surface(std::move(state)));
}

// Exercise decoded I420/NV12 planes as GPU textures, never a CPU-converted RGB
// frame. The scalar reference is an independent BT.601/709 matrix oracle for
// diagnostic readback only; real presentation samples Ganesh's YUV textures.
static int probePlanarYUVGPU(ScarletSkiaVulkan& owner, bool nv12)
{
    const char* format = nv12 ? "NV12" : "I420";
    const int failureBase = nv12 ? 46 : 40;
    constexpr int width = 8;
    constexpr int height = 8;
    auto surface = owner.createSurface(width, height);
    if (!surface) {
        std::fprintf(stderr, "SKIA_VULKAN_FAIL %s shared surface\n", format);
        return failureBase;
    }
    struct Conversion {
        SkYUVColorSpace space;
        const char* name;
        double kr;
        double kb;
        bool limited;
    };
    const Conversion conversions[] {
        { kJPEG_Full_SkYUVColorSpace, "BT601-full", 0.2990, 0.1140, false },
        { kRec601_Limited_SkYUVColorSpace, "BT601-limited", 0.2990, 0.1140, true },
        { kRec709_Full_SkYUVColorSpace, "BT709-full", 0.2126, 0.0722, false },
        { kRec709_Limited_SkYUVColorSpace, "BT709-limited", 0.2126, 0.0722, true },
    };
    const auto readInfo = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    std::array<unsigned char, width * height * 4> readback;
    for (const auto& conversion : conversions) {
        // Black/white/gray, opposed chroma, and clipped out-of-gamut samples.
        // Different values on U and V expose plane-order mistakes. Padding
        // guards expose use of plane width in place of the decoder row stride.
        const std::array<int, 3> samples[] {
            { conversion.limited ? 16 : 0, 128, 128 },
            { conversion.limited ? 235 : 255, 128, 128 },
            { 128, 128, 128 },
            { 96, 64, 192 },
            { 160, 192, 64 },
            { 16, 16, 240 },
            { 235, 240, 16 },
        };
        int fixture = 0;
        for (const auto& sample : samples) {
            const SkYUVAInfo info(SkISize::Make(width, height), nv12 ? SkYUVAInfo::PlaneConfig::kY_UV : SkYUVAInfo::PlaneConfig::kY_U_V,
                SkYUVAInfo::Subsampling::k420, conversion.space);
            const size_t rowBytes[SkYUVAPixmaps::kMaxPlanes] { 16, nv12 ? 16u : 8u, nv12 ? 0u : 8u, 0 };
            auto planes = SkYUVAPixmaps::Allocate(SkYUVAPixmapInfo(info,
                SkYUVAPixmapInfo::DataType::kUnorm8, rowBytes));
            if (!planes.isValid() || planes.numPlanes() != (nv12 ? 2 : 3)) {
                std::fprintf(stderr, "SKIA_VULKAN_FAIL %s plane allocation space=%s fixture=%d\n", format, conversion.name, fixture);
                return failureBase + 1;
            }
            for (int i = 0; i < planes.numPlanes(); ++i) {
                const auto& plane = planes.plane(i);
                const bool interleavedUV = nv12 && i == 1;
                const int planeSize = i ? width / 2 : width;
                if (plane.width() != planeSize || plane.height() != planeSize
                    || plane.info().bytesPerPixel() != (interleavedUV ? 2 : 1)) {
                    std::fprintf(stderr, "SKIA_VULKAN_FAIL %s plane layout space=%s fixture=%d plane=%d\n",
                        format, conversion.name, fixture, i);
                    return failureBase + 1;
                }
                std::memset(plane.writable_addr(), 0xCD, plane.computeByteSize());
                for (int row = 0; row < plane.height(); ++row) {
                    auto* bytes = static_cast<unsigned char*>(plane.writable_addr()) + row * plane.rowBytes();
                    if (interleavedUV) {
                        // NV12: each subsampled pixel carries U followed by V.
                        // Leave the remaining padded row bytes at the guard.
                        for (int column = 0; column < plane.width(); ++column) {
                            bytes[2 * column] = sample[1];
                            bytes[2 * column + 1] = sample[2];
                        }
                    } else
                        std::memset(bytes, sample[i], plane.width());
                }
            }
            auto image = SkImages::TextureFromYUVAPixmaps(owner.grContext(), planes,
                skgpu::Mipmapped::kNo, false, SkColorSpace::MakeSRGB());
            if (!image || !image->isTextureBacked()) {
                std::fprintf(stderr, "SKIA_VULKAN_FAIL %s GPU texture creation space=%s fixture=%d abandoned=%d\n",
                    format, conversion.name, fixture, owner.grContext()->abandoned());
                return failureBase + 2;
            }
            auto* canvas = surface->skSurface().getCanvas();
            canvas->clear(SK_ColorMAGENTA);
            SkPaint paint;
            paint.setBlendMode(SkBlendMode::kSrc);
            canvas->drawImage(image, 0, 0, SkSamplingOptions(SkFilterMode::kNearest), &paint);
            if (!owner.submit(*surface)) {
                std::fprintf(stderr, "SKIA_VULKAN_FAIL %s GPU draw submit space=%s fixture=%d abandoned=%d\n",
                    format, conversion.name, fixture, owner.grContext()->abandoned());
                return failureBase + 3;
            }
            if (!surface->skSurface().readPixels(readInfo, readback.data(), width * 4, 0, 0)) {
                std::fprintf(stderr, "SKIA_VULKAN_FAIL %s GPU readback space=%s fixture=%d\n", format, conversion.name, fixture);
                return failureBase + 4;
            }
            const double y = (sample[0] - (conversion.limited ? 16.0 : 0.0)) / (conversion.limited ? 219.0 : 255.0);
            const double u = (sample[1] - 128.0) / (conversion.limited ? 224.0 : 255.0);
            const double v = (sample[2] - 128.0) / (conversion.limited ? 224.0 : 255.0);
            const double kg = 1.0 - conversion.kr - conversion.kb;
            auto quantize = [](double value) { return static_cast<int>(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5); };
            const int expected[] {
                quantize(y + 2.0 * (1.0 - conversion.kr) * v),
                quantize(y - 2.0 * conversion.kb * (1.0 - conversion.kb) / kg * u - 2.0 * conversion.kr * (1.0 - conversion.kr) / kg * v),
                quantize(y + 2.0 * (1.0 - conversion.kb) * u),
            };
            // Two 8-bit levels cover GPU float/half matrix quantization and
            // UNORM rounding; range/plane/matrix mistakes are much larger.
            for (int pixel = 0; pixel < width * height; ++pixel) {
                auto* actual = readback.data() + pixel * 4;
                if (std::abs(actual[0] - expected[0]) > 2 || std::abs(actual[1] - expected[1]) > 2
                    || std::abs(actual[2] - expected[2]) > 2 || actual[3] != 255) {
                    std::fprintf(stderr, "SKIA_VULKAN_FAIL %s GPU color space=%s fixture=%d pixel=%d yuv=%d,%d,%d expected=%d,%d,%d,255 actual=%d,%d,%d,%d\n",
                        format, conversion.name, fixture, pixel, sample[0], sample[1], sample[2],
                        expected[0], expected[1], expected[2], actual[0], actual[1], actual[2], actual[3]);
                    return failureBase + 5;
                }
            }
            ++fixture;
        }
        std::fprintf(stderr, "SKIA_VULKAN_PASS %s GPU YUV textures space=%s frames=%d checked_pixels=%d padded_planes=yes cpu-rgb-conversion=none\n",
            format, conversion.name, fixture, fixture * width * height);
    }
    return 0;
}


// Probe-only regression using the generic RunLoop scheduled-task node type.
class RemovedTimerProbeNode final : public WTF::RedBlackTree<RemovedTimerProbeNode, unsigned>::ThreadSafeNode {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(RemovedTimerProbeNode);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(RemovedTimerProbeNode);
public:
    explicit RemovedTimerProbeNode(unsigned key) : m_key(key) { }
    unsigned key() const { return m_key; }
private:
    unsigned m_key;
};

static int probeRemovedTimerNodes()
{
    for (unsigned children = 1; children <= 2; ++children) {
        WTF::RedBlackTree<RemovedTimerProbeNode, unsigned> tree;
        auto* retained = new RemovedTimerProbeNode(2);
        tree.insert(retained);
        tree.insert(new RemovedTimerProbeNode(1));
        if (children == 2)
            tree.insert(new RemovedTimerProbeNode(3));
        tree.remove(retained);
        while (auto* node = tree.first()) {
            tree.remove(node);
            delete node;
        }
        // Old checked child links refer to now-zeroed former children.
        // Destroying the retained node must not access those dead objects.
        delete retained;
    }
    WTF::RedBlackTree<RemovedTimerProbeNode, unsigned> tree;
    auto* retained = new RemovedTimerProbeNode(2);
    tree.insert(retained);
    tree.insert(new RemovedTimerProbeNode(1));
    tree.insert(new RemovedTimerProbeNode(3));
    tree.remove(retained);
    tree.insert(retained);
    unsigned expected = 1;
    bool ordered = true;
    for (auto* node = tree.first(); node; node = node->successor())
        ordered &= node->key() == expected++;
    while (auto* node = tree.first()) {
        tree.remove(node);
        delete node;
    }
    if (!ordered || expected != 4) {
        std::fprintf(stderr, "SKIA_VULKAN_FAIL removed timer node reinsertion order\n");
        return 22;
    }
    std::fprintf(stderr, "SKIA_VULKAN_PASS removed timer nodes one-child two-child delayed-destruction reinsertion\n");
    return 0;
}

int probe()
{
    if (auto status = probeRemovedTimerNodes()) return status;
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
    auto owner = ScarletSkiaVulkan::create();
    if (!owner) return 7;
    auto surface = owner->createSurface(128, 96);
    if (!surface) return 8;
    auto retainedSurface = owner->createSurface(160, 112);
    if (!retainedSurface) return 19;
    retainedSurface->skSurface().getCanvas()->clear(SK_ColorGREEN);
    if (!owner->submit(*retainedSurface)) return 20;
    int32_t handle = surface->duplicateHandle();
    if (handle < 0) return 15;
    close(handle);
    const auto imageInfo = SkImageInfo::Make(128, 96, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    surface->skSurface().getCanvas()->clear(SK_ColorWHITE);
    SkPaint paint; paint.setColor(SK_ColorRED);
    surface->skSurface().getCanvas()->drawRect(SkRect::MakeXYWH(16, 16, 32, 32), paint);
    paint.setColor(SK_ColorBLUE);
    surface->skSurface().getCanvas()->drawRect(SkRect::MakeXYWH(64, 32, 32, 32), paint);
    if (!owner->submit(*surface)) {
        std::fprintf(stderr, "SKIA_VULKAN_FAIL draw submit abandoned=%d\n", owner->grContext()->abandoned());
        return 18;
    }
    std::vector<uint32_t> pixels(128 * 96);
    if (!surface->skSurface().readPixels(imageInfo, pixels.data(), 128 * 4, 0, 0)) { std::fprintf(stderr, "SKIA_VULKAN_FAIL GPU readback abandoned=%d\n", owner->grContext()->abandoned()); return 9; }
    // RGBA byte order, independent of Skia's native N32 order.
    auto* bytes = reinterpret_cast<const unsigned char*>(pixels.data());
    auto matches = [bytes](int x, int y, unsigned char r, unsigned char g, unsigned char b) {
        auto p = bytes + (y * 128 + x) * 4;
        return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
    };
    if (!matches(0, 0, 255, 255, 255) || !matches(24, 24, 255, 0, 0) || !matches(72, 40, 0, 0, 255)) {
        std::fprintf(stderr, "SKIA_VULKAN_FAIL GPU draw pixel mismatch\n"); return 10;
    }
    std::fprintf(stderr, "SKIA_VULKAN_PASS Ganesh rectangles on shared SGFX GPU image, handle=%d memory=%llu\n", handle, static_cast<unsigned long long>(owner->allocatedBytes()));
    surface.reset();
    if (!owner->submit(*retainedSurface)) return 21;
    if (auto status = probePlanarYUVGPU(*owner, false)) return status;
    return probePlanarYUVGPU(*owner, true);
}
}
extern "C" int scarlet_skia_vulkan_probe() { return WebCore::probe(); }

// Initialized C++ ELF TLS must be copied for every native thread. This also
// checks that Rust's namespace header and C errno survive C++ TLS writes.
static thread_local uint64_t tlsCounter = 17;
extern "C" uint64_t scarlet_webview_tls_value() { return tlsCounter; }
extern "C" uint64_t scarlet_webview_tls_increment() { return ++tlsCounter; }

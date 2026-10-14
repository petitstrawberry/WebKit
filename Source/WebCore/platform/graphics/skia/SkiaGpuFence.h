#pragma once
#if PLATFORM(SCARLET)
namespace WebCore {
// Backend-independent synchronization contract. The native synchronous path
// completes actual Vulkan work and returns no pending fence object.
class SkiaGpuFence {
public:
    virtual ~SkiaGpuFence() = default;
    virtual void serverWait() = 0;
};
}
#else
#include "GLFence.h"
namespace WebCore { using SkiaGpuFence = GLFence; }
#endif

#pragma once
#include "PlatformDisplay.h"
#if !PLATFORM(SCARLET)
#include "GLContext.h"
#endif
namespace WebCore {
inline bool prepareSkiaContext()
{
#if PLATFORM(SCARLET)
    return PlatformDisplay::sharedDisplay().prepareSkiaContext();
#else
    auto* context = PlatformDisplay::sharedDisplay().skiaGLContext();
    return context && context->makeContextCurrent();
#endif
}
}

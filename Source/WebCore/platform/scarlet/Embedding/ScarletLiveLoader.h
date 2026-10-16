#pragma once

#include "ScarletWebView.h"
#include <string>

namespace WebCore {

class Page;

// All entry points execute on the WebCore owner thread. The strategy owns
// ResourceLoaders; the host owns HTTP transport and clones poll's spans.
void initializeScarletLiveStrategies();
void registerScarletLivePage(Page&, void* viewIdentity);
void unregisterScarletLivePage(Page&);
int32_t pollScarletLiveRequest(Page&, SWVRequest&);
int32_t completeScarletLiveRequest(Page&, const SWVResponse&);
int32_t startScarletLiveResponse(Page&, const SWVResponse&);
int32_t appendScarletLiveResponse(Page&, uint64_t, SWVBytes, bool finished);
uint32_t pendingScarletLiveRequests(Page&);
std::string scarletLiveLastError(Page&);

} // namespace WebCore

// The standalone direct-WebCore runner has no network or browser process.
#pragma once

#include <cstdint>
#include <string>

namespace WebCore {

// Call on the initialized WebCore main thread before constructing any Page.
// Installs an offline loader. Unsupported clipboard/media/blob access fails.
void initializeScarletOfflineStrategies();

// An explicit, immutable HTTPS snapshot profile. This is a byte-backed
// substitute resource transport, never a network connection. The index is
// generated from the audited original response bytes by prepare-index.py.
// Call instead of initializeScarletOfflineStrategies, before creating a Page.
void initializeScarletSnapshotStrategies(const char* resourceRoot, const char* nativeIndex, const char* documentURL);

struct ScarletSnapshotResourceState {
    uint64_t loadedBytes { 0 };
    uint32_t requests { 0 };
    uint32_t loaded { 0 };
    uint32_t blocked { 0 };
    uint32_t failed { 0 };
    uint32_t cancelled { 0 };
    uint32_t pending { 0 };
};

// Deliver one queued batch through real SubresourceLoader policy, buffering,
// CachedResource completion and decoder callbacks. Call between RunLoop cycles
// until loading, stylesheet and font work is actually quiescent. Layout can
// discover more fonts/images, so the caller must recheck after actual layout.
unsigned pumpScarletSnapshotResources();
ScarletSnapshotResourceState scarletSnapshotResourceState();
std::string scarletSnapshotResourceEvidenceJSON();

} // namespace WebCore

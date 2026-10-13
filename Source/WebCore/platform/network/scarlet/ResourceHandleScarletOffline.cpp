// Native offline loader: unsupported URL loads always fail explicitly.
#include "config.h"
#include "ResourceHandle.h"
#include "ResourceHandleInternal.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"

#if PLATFORM(SCARLET)
namespace WebCore {

ResourceHandleInternal::~ResourceHandleInternal() = default;
ResourceHandle::~ResourceHandle() = default;

bool ResourceHandle::start()
{
    // True means a handle now owns completion. It does NOT report load success.
    // The existing zero-delay failure timer calls client()->wasBlocked(this).
    scheduleFailure(BlockedFailure);
    // Preserve initial deferred state. setDefersLoading(false) restarts this
    // pending failure through the existing generic implementation.
    if (d->m_defersLoading)
        d->m_failureTimer.stop();
    return true;
}

void ResourceHandle::cancel()
{
    d->m_failureTimer.stop();
    d->m_scheduledFailureType = NoFailure;
}

void ResourceHandle::platformSetDefersLoading(bool)
{
    // Existing ResourceHandle::setDefersLoading controls a scheduled failure.
    // There is no transport, socket, request thread or successful result here.
}

void ResourceHandle::platformLoadResourceSynchronously(NetworkingContext*,
    const ResourceRequest& request, StoredCredentialsPolicy, SecurityOrigin*,
    ResourceError& error, ResourceResponse& response, Vector<uint8_t>& data)
{
    data.clear();
    response = ResourceResponse { };
    error = ResourceError("ScarletOffline"_s, 1, request.url(),
        "URL loading is disabled in the native offline WebCore milestone"_s);
}

bool ResourceHandle::shouldUseCredentialStorage() { return false; }
void ResourceHandle::didReceiveAuthenticationChallenge(const AuthenticationChallenge&)
{
    scheduleFailure(BlockedFailure);
}
void ResourceHandle::receivedCredential(const AuthenticationChallenge&, const Credential&)
{
    scheduleFailure(BlockedFailure);
}
void ResourceHandle::receivedRequestToContinueWithoutCredential(const AuthenticationChallenge&)
{
    scheduleFailure(BlockedFailure);
}
void ResourceHandle::receivedCancellation(const AuthenticationChallenge&)
{
    cancel();
}
void ResourceHandle::receivedRequestToPerformDefaultHandling(const AuthenticationChallenge&)
{
    scheduleFailure(BlockedFailure);
}
void ResourceHandle::receivedChallengeRejection(const AuthenticationChallenge&)
{
    scheduleFailure(BlockedFailure);
}

} // namespace WebCore
#endif

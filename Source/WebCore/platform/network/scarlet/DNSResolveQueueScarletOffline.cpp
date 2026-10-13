// Explicit offline DNS adapter for the native Scarlet rendering milestone.
#include "config.h"
#include "DNSResolveQueueCurl.h"
#include <wtf/TZoneMallocInlines.h>

#if PLATFORM(SCARLET) && USE(CURL)
namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DNSResolveQueueCurl);

void DNSResolveQueueCurl::updateIsUsingProxy()
{
    m_isUsingProxy = false;
}

void DNSResolveQueueCurl::platformResolve(const String&)
{
    // Prefetch has no result callback. Complete accounting without contacting
    // a resolver or populating any successful cache entry.
    decrementRequestCount();
}

void DNSResolveQueueCurl::resolve(const String&, uint64_t, DNSCompletionHandler&& completionHandler)
{
    completionHandler(makeUnexpected(DNSError::CannotResolve));
}

void DNSResolveQueueCurl::stopResolve(uint64_t)
{
    // resolve() completes synchronously with failure; nothing remains pending.
}

} // namespace WebCore
#endif

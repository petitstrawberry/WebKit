#pragma once
#include <wtf/Forward.h>
namespace WebCore {
class CookieJar;
class StorageNamespaceProvider;
class ResourceRequest;
Ref<CookieJar> createScarletCookieJar();
Ref<StorageNamespaceProvider> createScarletStorageNamespaceProvider();
void closeScarletStorage();
void storeScarletResponseCookie(const ResourceRequest&, const String&);
}

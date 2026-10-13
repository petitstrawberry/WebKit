#pragma once
#include <wtf/Forward.h>
namespace WebCore {
class CookieJar;
class StorageNamespaceProvider;
class DatabaseProvider;
class ResourceRequest;
Ref<CookieJar> createScarletCookieJar();
Ref<StorageNamespaceProvider> createScarletStorageNamespaceProvider();
Ref<DatabaseProvider> createScarletDatabaseProvider();
void closeScarletStorage();
void storeScarletResponseCookie(const ResourceRequest&, const String&);
}

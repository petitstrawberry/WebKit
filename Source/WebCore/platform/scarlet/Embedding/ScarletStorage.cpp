/* Native in-process storage backed by WebKit's cookie database and legacy
 * storage namespace implementation. Network and DOM access share one jar. */
#include "config.h"
#include "ScarletStorage.h"
#include "CookieJar.h"
#include "CookieJarDB.h"
#include "SameSiteInfo.h"
#include "CookieStorageSession.h"
#include "Document.h"
#include <wtf/FileSystem.h>
#include "ResourceRequest.h"
#include "StorageSessionProvider.h"
#include "WebStorageNamespaceProvider.h"
#include <pal/SessionID.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <cstdlib>

namespace WebCore {
using namespace WTF::StringLiterals;

static String storageDirectory()
{
    const char* home = std::getenv("HOME");
    auto path = FileSystem::pathByAppendingComponent(home ? String::fromUTF8(home) : "/root"_s, ".local/share/myrica"_s);
    if (!FileSystem::makeAllDirectories(path))
        return { };
    return path;
}

static CookieJarDB& cookieDatabase()
{
    static NeverDestroyed<CookieJarDB> database([] {
        auto directory = storageDirectory();
        return directory.isEmpty() ? ":memory:"_s : FileSystem::pathByAppendingComponent(directory, "cookies.sqlite"_s);
    }());
    static bool opened = (database.get().open(), true);
    UNUSED_PARAM(opened);
    return database;
}

class ScarletStorageSessionProvider final : public StorageSessionProvider {
public:
    CookieStorageSession* storageSession() const final { return &m_session; }
private:
    mutable CookieStorageSession m_session { PAL::SessionID::defaultSessionID() };
};

class ScarletCookieJar final : public CookieJar {
public:
    ScarletCookieJar() : CookieJar(adoptRef(*new ScarletStorageSessionProvider)) { }
    bool cookiesEnabled(Document& document) final { return document.cookieURL().protocolIsInHTTPFamily() && cookieDatabase().isEnabled(); }
    String cookies(Document& document, const URL& url) const final { return header(document.firstPartyForCookies(), url, false, shouldIncludeSecureCookies(url), sameSiteInfo(document, IsForDOMCookieAccess::Yes)).first; }
    void setCookies(Document& document, const URL& url, const String& value) final {
        if (url.protocolIsInHTTPFamily())
            cookieDatabase().setCookie(document.firstPartyForCookies(), url, value, CookieJarDB::Source::Script);
    }
    std::pair<String, SecureCookiesAccessed> cookieRequestHeaderFieldValue(const URL& firstParty, const SameSiteInfo& info, const URL& url, IncludeSecureCookies secure) const final {
        return header(firstParty, url, true, secure, info);
    }
    std::optional<SHA1::Digest> cookieRequestHeaderFieldValueDigest(const URL& firstParty, const SameSiteInfo& info, const URL& url, IncludeSecureCookies secure) const final {
        auto value = cookieRequestHeaderFieldValue(firstParty, info, url, secure).first.utf8();
        SHA1 hash;
        hash.addBytes(byteCast<uint8_t>(value.span()));
        SHA1::Digest digest;
        hash.computeHash(digest);
        return digest;
    }
    bool getRawCookies(Document& document, const URL& url, Vector<Cookie>& result) const final {
        auto cookies = cookieDatabase().searchCookies(document.firstPartyForCookies(), url, std::nullopt, url.protocolIs("https"_s) ? std::nullopt : std::optional<bool>(false), std::nullopt);
        if (!cookies) return false;
        result = WTF::move(*cookies);
        return true;
    }
    void deleteCookie(const Document&, const URL& url, const String& name, CompletionHandler<void()>&& completion) final {
        cookieDatabase().deleteCookie(url.string(), name);
        completion();
    }
private:
    static std::pair<String, SecureCookiesAccessed> header(const URL& firstParty, const URL& url, bool httpOnly, IncludeSecureCookies secure, const SameSiteInfo& info) {
        auto cookies = cookieDatabase().searchCookies(firstParty, url, httpOnly ? std::nullopt : std::optional<bool>(false), secure == IncludeSecureCookies::Yes ? std::nullopt : std::optional<bool>(false), std::nullopt);
        StringBuilder builder;
        bool accessedSecure = false;
        if (cookies) {
            for (auto& cookie : *cookies) {
                if (httpOnly && !info.isSameSite
                    && (cookie.sameSite == Cookie::SameSitePolicy::Strict
                        || (cookie.sameSite == Cookie::SameSitePolicy::Lax && !(info.isTopSite && info.isSafeHTTPMethod))))
                    continue;
                if (!builder.isEmpty()) builder.append("; "_s);
                builder.append(cookie.name, '=', cookie.value);
                accessedSecure |= cookie.secure;
            }
        }
        return { builder.toString(), accessedSecure ? SecureCookiesAccessed::Yes : SecureCookiesAccessed::No };
    }
};

Ref<CookieJar> createScarletCookieJar()
{
    static NeverDestroyed<Ref<CookieJar>> jar(adoptRef(*new ScarletCookieJar));
    return jar.get().copyRef();
}

void storeScarletResponseCookie(const ResourceRequest& request, const String& value)
{
    if (request.allowCookies())
        cookieDatabase().setCookie(request.firstPartyForCookies(), request.url(), value, CookieJarDB::Source::Network);
}

void closeScarletStorage()
{
    WebKit::WebStorageNamespaceProvider::closeLocalStorage();
}

Ref<StorageNamespaceProvider> createScarletStorageNamespaceProvider()
{
    static NeverDestroyed<Ref<StorageNamespaceProvider>> provider(WebKit::WebStorageNamespaceProvider::create(storageDirectory()));
    return provider.get().copyRef();
}
}

/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "ScarletWidevine.h"

#if ENABLE(ENCRYPTED_MEDIA)

#include "CDMFactory.h"
#include "CDMInstance.h"
#include "CDMInstanceSession.h"
#include "CDMKeySystemConfiguration.h"
#include "CDMPrivate.h"
#include "CDMProxy.h"
#include "CDMRequirement.h"
#include "CDMRestrictions.h"
#include "CDMSessionType.h"
#include "SharedBuffer.h"
#include <cstdlib>
#include <span>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringCommon.h>

namespace WebCore {

namespace {

constexpr auto keySystemClearKeyName = ""_s;

ScarletWidevineBytes toBytes(const CString& text)
{
    return { reinterpret_cast<const uint8_t*>(text.data()), text.length() };
}

ScarletWidevineBytes toBytes(std::span<const uint8_t> span)
{
    return { span.data(), span.size() };
}

String configuredPath(const char* variable, const char* fallback)
{
    if (const char* value = std::getenv(variable); value && *value)
        return String::fromUTF8(value);
    return String::fromUTF8(fallback);
}

uint32_t toSessionType(CDMSessionType sessionType)
{
    switch (sessionType) {
    case CDMSessionType::PersistentUsageRecord:
        return SCARLET_WIDEVINE_SESSION_PERSISTENT_USAGE_RECORD;
    case CDMSessionType::PersistentLicense:
        return SCARLET_WIDEVINE_SESSION_PERSISTENT_LICENSE;
    case CDMSessionType::Temporary:
    default:
        return SCARLET_WIDEVINE_SESSION_TEMPORARY;
    }
}

uint32_t toInitDataType(const String& initDataType)
{
    if (equalLettersIgnoringASCIICase(initDataType, "keyids"_s))
        return SCARLET_WIDEVINE_INIT_DATA_KEYIDS;
    if (equalLettersIgnoringASCIICase(initDataType, "webm"_s))
        return SCARLET_WIDEVINE_INIT_DATA_WEBM;
    return SCARLET_WIDEVINE_INIT_DATA_CENC;
}

CDMKeyStatus toKeyStatus(uint32_t status)
{
    switch (status) {
    case SCARLET_WIDEVINE_KEY_USABLE:
        return CDMKeyStatus::Usable;
    case SCARLET_WIDEVINE_KEY_INTERNAL_ERROR:
        return CDMKeyStatus::InternalError;
    case SCARLET_WIDEVINE_KEY_EXPIRED:
        return CDMKeyStatus::Expired;
    case SCARLET_WIDEVINE_KEY_OUTPUT_RESTRICTED:
        return CDMKeyStatus::OutputRestricted;
    case SCARLET_WIDEVINE_KEY_OUTPUT_DOWNSCALED:
        return CDMKeyStatus::OutputDownscaled;
    case SCARLET_WIDEVINE_KEY_RELEASED:
        return CDMKeyStatus::Released;
    default:
        return CDMKeyStatus::StatusPending;
    }
}

CDMInstanceSession::KeyStatusVector decodeKeyStatuses(const ScarletWidevineBuffer& buffer)
{
    CDMInstanceSession::KeyStatusVector keys;
    if (!buffer.data || buffer.length < 4)
        return keys;

    size_t position = 0;
    auto readUint32 = [&](uint32_t& value) {
        if (position + 4 > buffer.length)
            return false;
        value = static_cast<uint32_t>(buffer.data[position])
            | (static_cast<uint32_t>(buffer.data[position + 1]) << 8)
            | (static_cast<uint32_t>(buffer.data[position + 2]) << 16)
            | (static_cast<uint32_t>(buffer.data[position + 3]) << 24);
        position += 4;
        return true;
    };

    uint32_t count = 0;
    if (!readUint32(count))
        return keys;

    for (uint32_t index = 0; index < count; ++index) {
        uint32_t idLength = 0;
        if (!readUint32(idLength) || position + idLength > buffer.length)
            break;
        auto keyId = SharedBuffer::create(std::span { buffer.data + position, static_cast<size_t>(idLength) });
        position += idLength;
        uint32_t status = 0;
        if (!readUint32(status))
            break;
        keys.append({ WTF::move(keyId), toKeyStatus(status) });
    }
    return keys;
}

} // namespace

class CDMInstanceSessionScarlet;

class CDMInstanceScarlet final : public CDMInstance, public CanMakeWeakPtr<CDMInstanceScarlet> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CDMInstanceScarlet);
public:
    static Ref<CDMInstanceScarlet> create() { return adoptRef(*new CDMInstanceScarlet()); }
    ~CDMInstanceScarlet() final
    {
        if (m_handle)
            scarlet_widevine_destroy(m_handle);
    }

    ImplementationType implementationType() const final { return ImplementationType::Scarlet; }
    void initializeWithConfiguration(const CDMKeySystemConfiguration&, AllowDistinctiveIdentifiers, AllowPersistentState, SuccessCallback&&) final;
    void setServerCertificate(Ref<SharedBuffer>&&, SuccessCallback&& callback) final { callback(SuccessValue::Failed); }
    void setStorageDirectory(const String&) final { }
    const String& keySystem() const final { return m_keySystem; }
    RefPtr<CDMInstanceSession> createSession() final;

    ScarletWidevineHandle* handle()
    {
        if (m_handle)
            return m_handle;

        auto hostPath = configuredPath("MYRICA_WIDEVINE_HOST", "/shared/widevine/widevine-host");
        auto cdmPath = configuredPath("MYRICA_WIDEVINE_CDM", "/shared/widevine/libwidevinecdm.so");
        auto ldPath = configuredPath("MYRICA_WIDEVINE_LD_PATH", "");
        auto hostUtf8 = hostPath.utf8();
        auto cdmUtf8 = cdmPath.utf8();
        auto ldUtf8 = ldPath.utf8();
        m_handle = scarlet_widevine_create(toBytes(hostUtf8), toBytes(cdmUtf8), toBytes(ldUtf8), 4, 1024 * 1024);
        return m_handle;
    }

private:
    CDMInstanceScarlet() = default;

    ScarletWidevineHandle* m_handle { nullptr };
    String m_keySystem { "com.widevine.alpha"_s };
};

class CDMInstanceSessionScarlet final : public CDMInstanceSession {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CDMInstanceSessionScarlet);
public:
    static Ref<CDMInstanceSessionScarlet> create(CDMInstanceScarlet& instance)
    {
        return adoptRef(*new CDMInstanceSessionScarlet(instance));
    }

    void setClient(WeakPtr<CDMInstanceSessionClient>&& client) final { m_client = WTF::move(client); }
    void clearClient() final { m_client = nullptr; }

    void requestLicense(LicenseType sessionType, KeyGroupingStrategy, const String& initDataType, Ref<SharedBuffer>&& initData, LicenseCallback&& callback) final
    {
        RefPtr<CDMInstanceScarlet> instance = m_instance.get();
        auto* handle = instance ? instance->handle() : nullptr;
        if (!handle) {
            callback(SharedBuffer::create(), emptyString(), false, Failed);
            return;
        }

        uint32_t messageType = 0;
        ScarletWidevineBuffer sessionBuffer { nullptr, 0 };
        ScarletWidevineBuffer messageBuffer { nullptr, 0 };
        auto initSpan = initData->span();
        const auto status = scarlet_widevine_create_session(handle, toBytes(initSpan), toSessionType(sessionType), toInitDataType(initDataType), &messageType, &sessionBuffer, &messageBuffer);
        if (status != SCARLET_WIDEVINE_OK) {
            callback(SharedBuffer::create(), emptyString(), false, Failed);
            return;
        }

        String sessionId = String::fromUTF8(std::span { sessionBuffer.data, sessionBuffer.length });
        Ref<SharedBuffer> message = SharedBuffer::create(std::span { messageBuffer.data, messageBuffer.length });
        scarlet_widevine_buffer_free(sessionBuffer);
        scarlet_widevine_buffer_free(messageBuffer);

        m_sessionId = sessionId;
        const bool needsIndividualization = messageType == 3;
        callback(WTF::move(message), sessionId, needsIndividualization, Succeeded);
    }

    void updateLicense(const String& sessionId, LicenseType, Ref<SharedBuffer>&& response, LicenseUpdateCallback&& callback) final
    {
        RefPtr<CDMInstanceScarlet> instance = m_instance.get();
        auto* handle = instance ? instance->handle() : nullptr;
        if (!handle) {
            callback(false, std::nullopt, std::nullopt, std::nullopt, Failed);
            return;
        }

        auto sessionUtf8 = sessionId.utf8();
        auto responseSpan = response->span();
        ScarletWidevineBuffer keysBuffer { nullptr, 0 };
        const auto status = scarlet_widevine_update_session(handle, toBytes(sessionUtf8), toBytes(responseSpan), &keysBuffer);
        if (status != SCARLET_WIDEVINE_OK) {
            callback(false, std::nullopt, std::nullopt, std::nullopt, Failed);
            return;
        }

        auto keys = decodeKeyStatuses(keysBuffer);
        scarlet_widevine_buffer_free(keysBuffer);
        callback(false, WTF::move(keys), std::nullopt, std::nullopt, Succeeded);
    }

    void loadSession(LicenseType, const String&, const String&, LoadSessionCallback&& callback) final
    {
        callback(std::nullopt, std::nullopt, std::nullopt, Failed, SessionLoadFailure::NoSessionData);
    }

    void closeSession(const String& sessionId, CloseSessionCallback&& callback) final
    {
        RefPtr<CDMInstanceScarlet> instance = m_instance.get();
        auto* handle = instance ? instance->handle() : nullptr;
        if (handle) {
            auto sessionUtf8 = sessionId.utf8();
            scarlet_widevine_close_session(handle, toBytes(sessionUtf8));
        }
        m_sessionId = String();
        callback();
    }

    void removeSessionData(const String&, LicenseType, RemoveSessionDataCallback&& callback) final
    {
        callback({ }, nullptr, Succeeded);
    }

    void storeRecordOfKeyUsage(const String&) final { }

private:
    explicit CDMInstanceSessionScarlet(CDMInstanceScarlet& instance)
        : m_instance(instance)
    {
    }

    WeakPtr<CDMInstanceScarlet> m_instance;
    WeakPtr<CDMInstanceSessionClient> m_client;
    String m_sessionId;
};

RefPtr<CDMInstanceSession> CDMInstanceScarlet::createSession()
{
    return CDMInstanceSessionScarlet::create(*this);
}

void CDMInstanceScarlet::initializeWithConfiguration(const CDMKeySystemConfiguration&, AllowDistinctiveIdentifiers allowDistinctiveIdentifiers, AllowPersistentState allowPersistentState, SuccessCallback&& callback)
{
    auto* scarletHandle = handle();
    if (!scarletHandle) {
        callback(SuccessValue::Failed);
        return;
    }

    const uint32_t distinctive = allowDistinctiveIdentifiers == AllowDistinctiveIdentifiers::Yes ? 1 : 0;
    const uint32_t persistent = allowPersistentState == AllowPersistentState::Yes ? 1 : 0;
    if (scarlet_widevine_initialize(scarletHandle, distinctive, persistent, 0) != SCARLET_WIDEVINE_OK) {
        callback(SuccessValue::Failed);
        return;
    }
    callback(SuccessValue::Succeeded);
}

class CDMPrivateScarlet final : public CDMPrivate {
public:
    Vector<String> supportedInitDataTypes() const final
    {
        return { "cenc"_s, "keyids"_s };
    }

    bool supportsConfiguration(const CDMKeySystemConfiguration& configuration) const final
    {
        return configuration.distinctiveIdentifier != CDMRequirement::Required
            && configuration.persistentState != CDMRequirement::Required;
    }

    bool supportsConfigurationWithRestrictions(const CDMKeySystemConfiguration& configuration, const CDMRestrictions& restrictions) const final
    {
        if (restrictions.distinctiveIdentifierDenied && configuration.distinctiveIdentifier == CDMRequirement::Required)
            return false;
        if (restrictions.persistentStateDenied && configuration.persistentState == CDMRequirement::Required)
            return false;
        if (restrictions.deniedSessionTypes.contains(CDMSessionType::Temporary))
            return false;
        return supportsConfiguration(configuration);
    }

    bool supportsSessionTypeWithConfiguration(const CDMSessionType& sessionType, const CDMKeySystemConfiguration&) const final
    {
        return sessionType == CDMSessionType::Temporary;
    }

    Vector<String> supportedRobustnesses() const final
    {
        return { emptyString() };
    }

    CDMRequirement distinctiveIdentifiersRequirement(const CDMKeySystemConfiguration&, const CDMRestrictions&) const final
    {
        return CDMRequirement::NotAllowed;
    }

    CDMRequirement persistentStateRequirement(const CDMKeySystemConfiguration&, const CDMRestrictions&) const final
    {
        return CDMRequirement::NotAllowed;
    }

    bool distinctiveIdentifiersAreUniquePerOriginAndClearable(const CDMKeySystemConfiguration&) const final
    {
        return true;
    }

    RefPtr<CDMInstance> createInstance() final
    {
        return CDMInstanceScarlet::create();
    }

    void loadAndInitialize() final { }

    bool supportsServerCertificates() const final { return false; }

    bool supportsSessions() const final { return true; }

    bool supportsInitData(const String& initDataType, const SharedBuffer&) const final
    {
        return equalLettersIgnoringASCIICase(initDataType, "cenc"_s)
            || equalLettersIgnoringASCIICase(initDataType, "keyids"_s);
    }

    RefPtr<SharedBuffer> sanitizeResponse(const SharedBuffer& response) const final
    {
        return SharedBuffer::create(response.span());
    }

    std::optional<String> sanitizeSessionId(const String& sessionId) const final
    {
        return sessionId;
    }
};

class CDMFactoryScarlet final : public CDMFactory {
public:
    static CDMFactoryScarlet& singleton()
    {
        static NeverDestroyed<CDMFactoryScarlet> factory;
        return factory;
    }

    // The factory is a process-wide singleton, so it never participates in refcounting.
    void ref() const final { }
    void deref() const final { }

    std::unique_ptr<CDMPrivate> createCDM(const String& keySystem, const String&, const CDMPrivateClient&) final
    {
        if (!supportsKeySystem(keySystem))
            return nullptr;
        return makeUnique<CDMPrivateScarlet>();
    }

    bool supportsKeySystem(const String& keySystem) final
    {
        return equalLettersIgnoringASCIICase(keySystem, "com.widevine.alpha"_s);
    }

private:
    CDMFactoryScarlet() = default;
    ~CDMFactoryScarlet() = default;
};

void CDMFactory::platformRegisterFactories(Vector<WeakRef<CDMFactory>>& factories)
{
    factories.append(CDMFactoryScarlet::singleton());
}

Vector<CDMProxyFactory*> CDMProxyFactory::platformRegisterFactories()
{
    // The Scarlet port decrypts through its own bridge instead of CDMProxy.
    return { };
}

} // namespace WebCore

#endif // ENABLE(ENCRYPTED_MEDIA)

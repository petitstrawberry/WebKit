/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "ScarletWidevine.h"

#if ENABLE(ENCRYPTED_MEDIA)

#include "CDMFactory.h"
#include "CDMEncryptionScheme.h"
#include "CDMMediaCapability.h"
#include "CDMInstance.h"
#include "CDMInstanceSession.h"
#include "CDMKeySystemConfiguration.h"
#include "CDMPrivate.h"
#include "CDMProxy.h"
#include "CDMRequirement.h"
#include "CDMMessageType.h"
#include <wtf/HashMap.h>
#include <wtf/Scope.h>
#include "CDMRestrictions.h"
#include "CDMSessionType.h"
#include "MediaKeySystemClient.h"
#include "MediaKeySystemRequest.h"
#include "SharedBuffer.h"
#include <cstdlib>
#include <span>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WeakPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/WorkQueue.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringCommon.h>

namespace WebCore {

class ScarletMediaKeySystemClient final : public RefCounted<ScarletMediaKeySystemClient>, public MediaKeySystemClient {
public:
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }
    void requestMediaKeySystem(MediaKeySystemRequest& request) final
    {
        if (request.keySystem() == "com.widevine.alpha"_s)
            request.allow(String { });
        else
            request.deny("Unsupported key system"_s);
    }
    void cancelMediaKeySystemRequest(MediaKeySystemRequest&) final { }
};

void provideScarletMediaKeySystemTo(Page& page)
{
    provideMediaKeySystemTo(page, adoptRef(*new ScarletMediaKeySystemClient));
}

namespace {

ScarletWidevineBytes toBytes(const UTF8CString& text)
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
        auto keyId = SharedBuffer::create(std::span<const uint8_t> { buffer.data + position, static_cast<size_t>(idLength) });
        position += idLength;
        uint32_t status = 0;
        if (!readUint32(status))
            break;
        keys.append({ WTF::move(keyId), toKeyStatus(status) });
    }
    return keys;
}

} // namespace

// All blocking process/IPC operations are serialized here, away from the Page
// owner. Replies return to the owner loop that created this MediaKeys object.
class ScarletCDMConnection final : public ThreadSafeRefCounted<ScarletCDMConnection> {
public:
    static Ref<ScarletCDMConnection> create() { return adoptRef(*new ScarletCDMConnection); }
    ~ScarletCDMConnection()
    {
        if (m_handle)
            cdm_host_destroy(m_handle);
    }

    void dispatch(Function<void(ScarletCDMConnection&)>&& operation)
    {
        m_queue->dispatch([connection = Ref { *this }, operation = WTF::move(operation)]() mutable {
            operation(connection.get());
        });
    }
    void reply(Function<void()>&& callback) { m_owner->dispatch(WTF::move(callback)); }
    ScarletWidevineHandle* handle() const { return m_handle; }
    bool initialize()
    {
        if (!m_handle)
            m_handle = cdm_host_create(toBytes(m_hostPath), toBytes(m_cdmPath), toBytes(m_ldPath), 4, 1024 * 1024);
        // L3, temporary sessions: never request persistent identifiers/state or
        // hardware-secure codecs from the software CDM.
        return m_handle && cdm_host_initialize(m_handle, 0, 0, 0) == SCARLET_WIDEVINE_OK;
    }

private:
    ScarletCDMConnection()
        : m_queue(WorkQueue::create("WebCore.ScarletCDM"_s))
        , m_owner(RunLoop::currentSingleton())
        , m_hostPath(configuredPath("MYRICA_WIDEVINE_HOST", "/shared/widevine/widevine-host").utf8())
        , m_cdmPath(configuredPath("MYRICA_WIDEVINE_CDM", "/shared/widevine/libwidevinecdm.so").utf8())
        , m_ldPath(configuredPath("MYRICA_WIDEVINE_LD_PATH", "").utf8())
    {
    }

    Ref<WorkQueue> m_queue;
    Ref<RunLoop> m_owner;
    UTF8CString m_hostPath;
    UTF8CString m_cdmPath;
    UTF8CString m_ldPath;
    ScarletWidevineHandle* m_handle { nullptr }; // Accessed only by the serial queue.
};

// Session clients and their timer stay on the Page owner. Only the connection
// and owned reply buffers travel through the serial CDM IPC queue.
class ScarletCDMEvents final : public RefCounted<ScarletCDMEvents>, public CanMakeWeakPtr<ScarletCDMEvents> {
public:
    static Ref<ScarletCDMEvents> create(ScarletCDMConnection& connection) { return adoptRef(*new ScarletCDMEvents(connection)); }
    ~ScarletCDMEvents() { m_timer.stop(); }
    void add(const String& id, const WeakPtr<CDMInstanceSessionClient>& client)
    {
        if (!client || id.isEmpty())
            return;
        m_clients.set(id, client);
        if (!m_timer.isActive())
            m_timer.startRepeating(100_ms);
    }
    void remove(const String& id)
    {
        m_clients.remove(id);
        if (m_clients.isEmpty())
            m_timer.stop();
    }
private:
    explicit ScarletCDMEvents(ScarletCDMConnection& connection)
        : m_connection(connection)
        , m_timer(RunLoop::currentSingleton(), "ScarletCDMEvents::poll"_s, this, &ScarletCDMEvents::poll)
    {
    }
    void poll()
    {
        m_clients.removeIf([](const auto& entry) { return !entry.value; });
        if (m_clients.isEmpty()) { m_timer.stop(); return; }
        if (m_pending)
            return;
        m_pending = true;
        m_connection->dispatch([completion = [protectedThis = Ref { *this }](Vector<ScarletWidevineBuffer>&& events) mutable {
            protectedThis->m_pending = false;
            for (auto buffer : events) {
                auto release = makeScopeExit([&] { cdm_host_buffer_free(buffer); });
                protectedThis->deliver(buffer);
            }
        }](ScarletCDMConnection& connection) mutable {
            Vector<ScarletWidevineBuffer> events;
            for (unsigned count = 0; count < 64; ++count) {
                ScarletWidevineBuffer buffer { nullptr, 0 };
                if (!connection.handle() || cdm_host_take_event(connection.handle(), &buffer) <= 0)
                    break;
                events.append(buffer);
            }
            connection.reply([completion = WTF::move(completion), events = WTF::move(events)]() mutable { completion(WTF::move(events)); });
        });
    }
    void deliver(const ScarletWidevineBuffer& buffer)
    {
        size_t position = 0;
        auto integer = [&]() -> std::optional<uint32_t> {
            if (buffer.length - position < 4)
                return std::nullopt;
            auto* data = buffer.data + position;
            position += 4;
            return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
                | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
        };
        auto kind = integer();
        auto idSize = integer();
        if (!kind || !idSize || *idSize > buffer.length - position)
            return;
        auto id = String::fromUTF8(std::span<const uint8_t> { buffer.data + position, *idSize });
        position += *idSize;
        RefPtr client = m_clients.get(id);
        if (!client)
            return;
        if (*kind == SCARLET_WIDEVINE_EVENT_KEYS) {
            ScarletWidevineBuffer keys { buffer.data + position, buffer.length - position };
            client->updateKeyStatuses(decodeKeyStatuses(keys));
        } else if (*kind == SCARLET_WIDEVINE_EVENT_MESSAGE) {
            auto type = integer();
            auto size = integer();
            if (!type || *type > 3 || !size || *size > buffer.length - position)
                return;
            client->sendMessage(static_cast<CDMMessageType>(*type), SharedBuffer::create(std::span<const uint8_t> { buffer.data + position, *size }));
        }
    }
    Ref<ScarletCDMConnection> m_connection;
    RunLoop::Timer m_timer;
    HashMap<String, WeakPtr<CDMInstanceSessionClient>> m_clients;
    bool m_pending { false };
};

class CDMInstanceSessionScarlet final : public CDMInstanceSession {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CDMInstanceSessionScarlet);
public:
    static Ref<CDMInstanceSessionScarlet> create(ScarletCDMConnection& connection, ScarletCDMEvents& events)
    {
        return adoptRef(*new CDMInstanceSessionScarlet(connection, events));
    }

    ~CDMInstanceSessionScarlet() { m_events->remove(m_sessionId); }
    void setClient(WeakPtr<CDMInstanceSessionClient>&& client) final { m_client = WTF::move(client); m_events->add(m_sessionId, m_client); }
    void clearClient() final { m_events->remove(m_sessionId); m_client = nullptr; }

    void requestLicense(LicenseType sessionType, KeyGroupingStrategy, const String& initDataType, Ref<SharedBuffer>&& initData, LicenseCallback&& callback) final
    {
        if (sessionType != LicenseType::Temporary) {
            callback(SharedBuffer::create(), emptyString(), false, Failed);
            return;
        }
        auto dataType = toInitDataType(initDataType);
        m_connection->dispatch([dataType, initData = WTF::move(initData),
            completion = [protectedThis = Ref { *this }, callback = WTF::move(callback)](int32_t status, uint32_t messageType, ScarletWidevineBuffer session, ScarletWidevineBuffer message) mutable {
                auto sessionId = String::fromUTF8(std::span { session.data, session.length });
                auto buffer = SharedBuffer::create(std::span<const uint8_t> { message.data, message.length });
                cdm_host_buffer_free(session);
                cdm_host_buffer_free(message);
                if (status != SCARLET_WIDEVINE_OK || sessionId.isEmpty()) {
                    callback(SharedBuffer::create(), emptyString(), false, Failed);
                    return;
                }
                protectedThis->m_sessionId = sessionId;
                protectedThis->m_events->add(sessionId, protectedThis->m_client);
                callback(WTF::move(buffer), sessionId, messageType == 3, Succeeded);
            }](ScarletCDMConnection& connection) mutable {
            uint32_t messageType = 0;
            ScarletWidevineBuffer session { nullptr, 0 };
            ScarletWidevineBuffer message { nullptr, 0 };
            auto status = connection.handle() ? cdm_host_create_session(connection.handle(), toBytes(initData->span()), SCARLET_WIDEVINE_SESSION_TEMPORARY, dataType, &messageType, &session, &message) : SCARLET_WIDEVINE_ERROR;
            connection.reply([completion = WTF::move(completion), status, messageType, session, message]() mutable {
                completion(status, messageType, session, message);
            });
        });
    }

    void updateLicense(const String& sessionId, LicenseType, Ref<SharedBuffer>&& response, LicenseUpdateCallback&& callback) final
    {
        m_connection->dispatch([session = sessionId.utf8(), response = WTF::move(response),
            completion = [protectedThis = Ref { *this }, callback = WTF::move(callback)](int32_t status, ScarletWidevineBuffer keys) mutable {
                auto changedKeys = decodeKeyStatuses(keys);
                cdm_host_buffer_free(keys);
                if (status != SCARLET_WIDEVINE_OK) {
                    callback(false, std::nullopt, std::nullopt, std::nullopt, Failed);
                    return;
                }
                callback(false, WTF::move(changedKeys), std::nullopt, std::nullopt, Succeeded);
            }](ScarletCDMConnection& connection) mutable {
            ScarletWidevineBuffer keys { nullptr, 0 };
            auto status = connection.handle() ? cdm_host_update_session(connection.handle(), toBytes(session), toBytes(response->span()), &keys) : SCARLET_WIDEVINE_ERROR;
            connection.reply([completion = WTF::move(completion), status, keys]() mutable { completion(status, keys); });
        });
    }

    void loadSession(LicenseType, const String&, const String&, LoadSessionCallback&& callback) final
    {
        callback(std::nullopt, std::nullopt, std::nullopt, Failed, SessionLoadFailure::NoSessionData);
    }

    void closeSession(const String& sessionId, CloseSessionCallback&& callback) final
    {
        m_connection->dispatch([session = sessionId.utf8(),
            completion = [protectedThis = Ref { *this }, callback = WTF::move(callback)]() mutable {
                protectedThis->m_events->remove(protectedThis->m_sessionId);
                protectedThis->m_sessionId = String();
                callback();
            }](ScarletCDMConnection& connection) mutable {
            if (connection.handle())
                cdm_host_close_session(connection.handle(), toBytes(session));
            connection.reply(WTF::move(completion));
        });
    }

    void removeSessionData(const String&, LicenseType, RemoveSessionDataCallback&& callback) final
    {
        callback({ }, nullptr, Failed);
    }

    void storeRecordOfKeyUsage(const String&) final { }

private:
    explicit CDMInstanceSessionScarlet(ScarletCDMConnection& connection, ScarletCDMEvents& events)
        : m_connection(connection)
        , m_events(events)
    {
    }

    Ref<ScarletCDMConnection> m_connection;
    Ref<ScarletCDMEvents> m_events;
    WeakPtr<CDMInstanceSessionClient> m_client;
    String m_sessionId;
};

class CDMInstanceScarlet final : public CDMInstance {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(CDMInstanceScarlet);
public:
    static Ref<CDMInstanceScarlet> create() { return adoptRef(*new CDMInstanceScarlet); }
    ImplementationType implementationType() const final { return ImplementationType::Scarlet; }
    ScarletWidevineHandle* mediaHandle() const { return m_connection->handle(); }
    void initializeWithConfiguration(const CDMKeySystemConfiguration&, AllowDistinctiveIdentifiers identifiers, AllowPersistentState persistent, SuccessCallback&& callback) final
    {
        if (identifiers == AllowDistinctiveIdentifiers::Yes || persistent == AllowPersistentState::Yes) {
            callback(SuccessValue::Failed);
            return;
        }
        m_connection->dispatch([callback = WTF::move(callback)](ScarletCDMConnection& connection) mutable {
            bool success = connection.initialize();
            connection.reply([callback = WTF::move(callback), success]() mutable {
                callback(success ? SuccessValue::Succeeded : SuccessValue::Failed);
            });
        });
    }
    void setServerCertificate(Ref<SharedBuffer>&&, SuccessCallback&& callback) final { callback(SuccessValue::Failed); }
    void setStorageDirectory(const String&) final { }
    const String& keySystem() const final { return m_keySystem; }
    RefPtr<CDMInstanceSession> createSession() final { return CDMInstanceSessionScarlet::create(m_connection.get(), m_events.get()); }

private:
    CDMInstanceScarlet() = default;
    Ref<ScarletCDMConnection> m_connection { ScarletCDMConnection::create() };
    Ref<ScarletCDMEvents> m_events { ScarletCDMEvents::create(m_connection.get()) };
    String m_keySystem { "com.widevine.alpha"_s };
};

ScarletWidevineHandle* scarletCDMHandle(CDMInstance& instance)
{
    // MediaKeys is published only after WorkQueue initialization completes.
    // The native media session clones the shared state before this call returns.
    if (instance.implementationType() != CDMInstance::ImplementationType::Scarlet)
        return nullptr;
    return static_cast<CDMInstanceScarlet&>(instance).mediaHandle();
}

class CDMPrivateScarlet final : public CDMPrivate {
public:
    Vector<String> supportedInitDataTypes() const final
    {
        return { "cenc"_s, "keyids"_s };
    }

    bool supportsConfiguration(const CDMKeySystemConfiguration& configuration) const final
    {
        auto supportedScheme = [](const auto& capability) {
            return !capability.encryptionScheme || *capability.encryptionScheme == CDMEncryptionScheme::cenc;
        };
        return configuration.distinctiveIdentifier != CDMRequirement::Required
            && configuration.persistentState != CDMRequirement::Required
            && std::ranges::all_of(configuration.audioCapabilities, supportedScheme)
            && std::ranges::all_of(configuration.videoCapabilities, supportedScheme);
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
        // Only advertise the decrypt-only L3 path. Decode inside the CDM and
        // hardware-protected output are not implemented by this port.
        return { emptyString(), "SW_SECURE_CRYPTO"_s };
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
        return keySystem == "com.widevine.alpha"_s;
    }

private:
    friend class NeverDestroyed<CDMFactoryScarlet>;
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

/*
 * Copyright (C) 2023 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SharedMemory.h"
#include <limits>
#include <wtf/Assertions.h>

namespace WebCore {

RefPtr<SharedMemory> SharedMemory::allocate(size_t size)
{
    if (!size || size > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max()))
        return nullptr;
    ScarletSharedMemoryHandle handle(scarlet_webcore_memory_create(size));
    if (!handle) {
        WTFLogAlways("Scarlet WebCore: native shared-memory allocation failed");
        return nullptr;
    }
    void* data = scarlet_webcore_memory_map(handle.value(), size, true);
    if (!data) {
        WTFLogAlways("Scarlet WebCore: native shared-memory mapping failed");
        return nullptr;
    }
    auto memory = adoptRef(*new SharedMemory);
    memory->m_size = size;
    memory->m_data = data;
    memory->m_scarletHandle = WTF::move(handle);
    return memory;
}

RefPtr<SharedMemory> SharedMemory::map(Handle&& handle, Protection protection, CopyOnWrite copyOnWrite)
{
    if (copyOnWrite != CopyOnWrite::No) {
        WTFLogAlways("Scarlet WebCore: copy-on-write shared memory is unsupported");
        return nullptr;
    }
    if (!handle.m_handle || !handle.size() || handle.size() > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max()))
        return nullptr;
    void* data = scarlet_webcore_memory_map(handle.m_handle.value(), handle.size(), protection == Protection::ReadWrite);
    if (!data) {
        WTFLogAlways("Scarlet WebCore: native shared-memory mapping failed");
        return nullptr;
    }
    auto memory = adoptRef(*new SharedMemory);
    memory->m_size = handle.size();
    memory->m_data = data;
    memory->m_scarletHandle = WTF::move(handle.m_handle);
    return memory;
}

SharedMemory::~SharedMemory()
{
    RELEASE_ASSERT_WITH_MESSAGE(!scarlet_webcore_memory_unmap(m_data, m_size), "Could not unmap Scarlet shared memory");
}

std::optional<SharedMemory::Handle> SharedMemory::createHandle(Protection protection)
{
    // Duplicate native handles retain object permissions. Scarlet has no handle
    // rights attenuation API; do not claim to export a read-only capability.
    if (protection == Protection::ReadOnly) {
        WTFLogAlways("Scarlet WebCore: read-only shared-memory handle export is unsupported");
        return std::nullopt;
    }
    auto duplicate = m_scarletHandle.duplicate();
    if (!duplicate)
        return std::nullopt;
    return Handle(WTF::move(duplicate), m_size);
}

} // namespace WebCore

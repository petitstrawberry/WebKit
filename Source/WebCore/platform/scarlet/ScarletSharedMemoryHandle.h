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

#pragma once

#include <cstddef>
#include <utility>
#include <wtf/Assertions.h>

extern "C" {
void* scarlet_webcore_memory_create(size_t);
void* scarlet_webcore_memory_duplicate(const void*);
int scarlet_webcore_memory_close(void*);
void* scarlet_webcore_memory_map(const void*, size_t, bool writable);
int scarlet_webcore_memory_unmap(void*, size_t);
}

namespace WebCore {

// Owns a bridge Box<scarlet_os::Handle>, which owns a native Scarlet shared-memory
// object. This is intentionally not an integer/file descriptor or UnixFileDescriptor.
class ScarletSharedMemoryHandle {
public:
    ScarletSharedMemoryHandle() = default;
    explicit ScarletSharedMemoryHandle(void* handle) : m_handle(handle) { }
    explicit ScarletSharedMemoryHandle(const ScarletSharedMemoryHandle& other)
        : m_handle(other ? scarlet_webcore_memory_duplicate(other.m_handle) : nullptr)
    {
        RELEASE_ASSERT_WITH_MESSAGE(!other || m_handle, "Could not duplicate Scarlet shared-memory handle");
    }
    ScarletSharedMemoryHandle(ScarletSharedMemoryHandle&& other)
        : m_handle(std::exchange(other.m_handle, nullptr)) { }
    ScarletSharedMemoryHandle& operator=(ScarletSharedMemoryHandle&& other)
    {
        if (this != &other) {
            ScarletSharedMemoryHandle previous(std::move(*this));
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }
    ~ScarletSharedMemoryHandle()
    {
        RELEASE_ASSERT_WITH_MESSAGE(!scarlet_webcore_memory_close(m_handle), "Could not close Scarlet shared-memory handle");
    }
    explicit operator bool() const { return m_handle; }
    void* value() const { return m_handle; }
    ScarletSharedMemoryHandle duplicate() const
    {
        return ScarletSharedMemoryHandle(m_handle ? scarlet_webcore_memory_duplicate(m_handle) : nullptr);
    }

private:
    void* m_handle { nullptr };
};

} // namespace WebCore

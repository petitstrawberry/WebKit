/*
 * Copyright (C) 2006, 2010 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Torch Mobile Inc. All rights reserved. (http://www.torchmobile.com/)
 * Copyright (C) 2009 Google, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ScarletBackForwardClient.h"

#include "BackForwardCache.h"
#include "HistoryItem.h"
#include <optional>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>

namespace WebCore {

static HistoryItem* itemForFrame(HistoryItem& item, FrameIdentifier frameID)
{
    if (item.frameID() == frameID)
        return &item;
    for (auto& child : item.children()) {
        if (auto* result = itemForFrame(child, frameID))
            return result;
    }
    return nullptr;
}

static HistoryItem* itemForFrameItem(HistoryItem& item, BackForwardFrameItemIdentifier frameItemID)
{
    if (item.frameItemID() == frameItemID)
        return &item;
    for (auto& child : item.children()) {
        if (auto* result = itemForFrameItem(child, frameItemID))
            return result;
    }
    return nullptr;
}

static bool treeContainsItem(const HistoryItem& tree, const HistoryItem& item)
{
    if (&tree == &item)
        return true;
    for (auto& child : tree.children()) {
        if (treeContainsItem(child, item))
            return true;
    }
    return false;
}

static void removeCachedTree(HistoryItem& item)
{
    for (auto& child : item.children())
        removeCachedTree(child);
    BackForwardCache::singleton().remove(item);
}

// Like the native WebKitLegacy list, this retains HistoryItem trees, not URLs.
// HistoryController updates their state/form/scroll data and owns traversal.
class ScarletBackForwardClient final : public BackForwardClient {
    WTF_MAKE_TZONE_ALLOCATED(ScarletBackForwardClient);
public:
    ScarletBackForwardClient() = default;
    ~ScarletBackForwardClient() final { close(); }

private:
    static constexpr unsigned capacity = 100;

    void addItem(Ref<HistoryItem>&& item) final
    {
        if (m_closed)
            return;

        if (m_current) {
            while (m_entries.size() > *m_current + 1) {
                auto removed = m_entries.takeLast();
                removeCachedTree(removed);
            }
        }

        if (m_entries.size() == capacity) {
            auto removed = WTF::move(m_entries[0]);
            m_entries.removeAt(0);
            --*m_current;
            removeCachedTree(removed);
        }

        m_entries.append(WTF::move(item));
        m_current = m_entries.size() - 1;
    }

    void setChildItem(BackForwardFrameItemIdentifier parentFrameItemID, Ref<HistoryItem>&& item) final
    {
        if (m_closed || !m_current)
            return;
        // Child commits modify the currently selected entry's genuine tree.
        auto* parent = itemForFrameItem(m_entries[*m_current], parentFrameItemID);
        if (!parent)
            return;
        for (auto& child : parent->children()) {
            // HistoryController may already have updated this same retained
            // object before notifying BackForwardClient. Do not set it twice:
            // HistoryItem::setChildItem expects a new, non-target child.
            if (child.ptr() == item.ptr())
                return;
        }
        parent->setChildItem(WTF::move(item));
    }

    void goToItem(HistoryItem& item) final
    {
        if (m_closed)
            return;
        for (unsigned i = 0; i < m_entries.size(); ++i) {
            if (treeContainsItem(m_entries[i], item)) {
                m_current = i;
                return;
            }
        }
    }

    Vector<Ref<HistoryItem>> allItems(FrameIdentifier frameID) final
    {
        Vector<Ref<HistoryItem>> result;
        if (m_closed)
            return result;
        // The controller passes its mainFrame().frameID() by default. Other
        // frame queries resolve within each tree and never return wrong roots.
        for (auto& entry : m_entries) {
            if (auto* item = itemForFrame(entry, frameID))
                result.append(*item);
        }
        return result;
    }

    RefPtr<HistoryItem> itemAtIndex(int offset, FrameIdentifier frameID) final
    {
        if (m_closed || !m_current)
            return nullptr;
        // Capacity is bounded, so these comparisons avoid both signed
        // overflow for INT_MIN/INT_MAX offsets and unsigned cursor wrapping.
        if (offset < -static_cast<int>(*m_current) || offset > static_cast<int>(forwardListCount()))
            return nullptr;
        auto index = static_cast<unsigned>(static_cast<int>(*m_current) + offset);
        return itemForFrame(m_entries[index], frameID);
    }

    unsigned backListCount() const final { return m_current ? *m_current : 0; }
    unsigned forwardListCount() const final { return m_current ? m_entries.size() - *m_current - 1 : 0; }

    bool containsItem(const HistoryItem& item) const final
    {
        if (m_closed)
            return false;
        for (auto& entry : m_entries) {
            if (treeContainsItem(entry, item))
                return true;
        }
        return false;
    }

    void close() final
    {
        if (m_closed)
            return;
        m_closed = true;
        m_current.reset();
        auto entries = WTF::move(m_entries);
        for (auto& item : entries)
            removeCachedTree(item);
    }

    Vector<Ref<HistoryItem>> m_entries;
    std::optional<unsigned> m_current;
    bool m_closed { false };
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(ScarletBackForwardClient);

Ref<BackForwardClient> createScarletBackForwardClient()
{
    return adoptRef(*new ScarletBackForwardClient);
}

} // namespace WebCore

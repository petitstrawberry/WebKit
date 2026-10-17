/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "config.h"
#include "ScarletScrollingCoordinator.h"
#if ENABLE(ASYNC_SCROLLING) && USE(COORDINATED_GRAPHICS)
#include "ScarletLayerTree.h"
#include "ScrollingThread.h"
#include <wtf/MainThread.h>
namespace WebCore {
ScarletScrollInput::ScarletScrollInput(Function<void(const PlatformWheelEvent&, OptionSet<WheelEventProcessingSteps>)>&& dispatchDOM, Function<void()>&& wake)
    : m_wake(WTF::move(wake))
    , m_owner(RunLoop::currentSingleton())
    , m_dispatchDOM(WTF::move(dispatchDOM))
{
    ASSERT(isMainThread());
}
void ScarletScrollInput::setTree(Ref<ScrollingTreeCoordinated>&& tree)
{
    ASSERT(isMainThread());
    std::lock_guard lock(m_mutex);
    m_tree = WTF::move(tree);
}
void ScarletScrollInput::setViewport(const IntSize& size, double scale)
{
    ASSERT(isMainThread());
    std::lock_guard lock(m_mutex);
    m_size = size;
    m_scale = scale;
}
void ScarletScrollInput::setEpoch(uint64_t epoch)
{
    ASSERT(isMainThread());
    std::lock_guard lock(m_mutex);
    m_epoch = epoch;
}
bool ScarletScrollInput::isCurrent(uint64_t epoch)
{
    std::lock_guard lock(m_mutex);
    return m_live && m_epoch == epoch;
}
void ScarletScrollInput::invalidate()
{
    ASSERT(isMainThread());
    {
        std::lock_guard lock(m_mutex);
        m_live = false;
        m_tree = nullptr;
    }
    m_dispatchDOM = nullptr;
}
void ScarletScrollInput::dispatchDOM(const PlatformWheelEvent& event, OptionSet<WheelEventProcessingSteps> steps, uint64_t epoch)
{
    RefPtr<RunLoop> owner;
    {
        std::lock_guard lock(m_mutex);
        if (!m_live || m_epoch != epoch) {
            --m_pending;
            return;
        }
        // The endpoint can outlive its owner thread. Retain the run loop
        // before dropping the live-state lock, then post without that lock:
        // the run-loop wake callback may acquire the UI mailbox mutex.
        owner = &m_owner;
    }
    owner->dispatch([self = shared_from_this(), event, steps, epoch] {
        if (self->isCurrent(epoch) && self->m_dispatchDOM)
            self->m_dispatchDOM(event, steps);
        --self->m_pending;
    });
}
bool ScarletScrollInput::dispatch(const PlatformWheelEvent& event, uint64_t epoch, const IntSize& size, double scale)
{
    RefPtr<ScrollingTreeCoordinated> tree;
    {
        std::lock_guard lock(m_mutex);
        if (!m_live || !m_tree || m_epoch != epoch || m_size != size || m_scale != scale || m_pending >= 256)
            return false;
        tree = m_tree;
        ++m_pending;
    }
    tree->willProcessWheelEvent();
    m_wake();
    // Match EventDispatcher's standard routing. Non-passive DOM listeners keep
    // their cancellation semantics; passive listeners run after the async scroll.
    ScrollingThread::dispatch([self = shared_from_this(), tree = WTF::move(tree), event, epoch] {
        if (!self->isCurrent(epoch)) {
            --self->m_pending;
            return;
        }
        auto steps = tree->determineWheelEventProcessing(event);
        if (steps.contains(WheelEventProcessingSteps::SynchronousScrolling)) {
            tree->willSendEventToMainThread(event);
            self->dispatchDOM(event, steps, epoch);
            tree->waitForEventToBeProcessedByMainThread(event);
            return;
        }
        auto result = tree->handleWheelEvent(event, steps);
        // A scroll update has queued an owner rendering update. Mark that
        // request here as well: a busy owner cannot acknowledge its RunLoop
        // task yet. WebKit's normal display-tick timeout can then desynchronize
        // layer positions without waiting for script to finish.
        if (result.wasHandled)
            tree->didScheduleRenderingUpdate();
        if (result.needsMainThreadProcessing())
            self->dispatchDOM(event, result.steps, epoch);
        else
            --self->m_pending;
    });
    return true;
}
Ref<ScarletScrollingCoordinator> ScarletScrollingCoordinator::create(Page& page, ScarletLayerTree& layerTree)
{
    return adoptRef(*new ScarletScrollingCoordinator(page, layerTree));
}
ScarletScrollingCoordinator::ScarletScrollingCoordinator(Page& page, ScarletLayerTree& layerTree)
    : ThreadedScrollingCoordinator(&page)
{
    auto tree = ScrollingTreeCoordinated::create(*this);
    layerTree.setScrollingTree(tree.copyRef());
    setScrollingTree(WTF::move(tree));
}
void ScarletScrollingCoordinator::didCompletePlatformRenderingUpdate()
{
    downcast<ScrollingTreeCoordinated>(scrollingTree())->didCompletePlatformRenderingUpdate();
}
}
#endif

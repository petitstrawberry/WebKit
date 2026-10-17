/*
 * Copyright (C) 2026 Scarlet contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#if ENABLE(ASYNC_SCROLLING) && USE(COORDINATED_GRAPHICS)
#include "PlatformWheelEvent.h"
#include "ScrollingTreeCoordinated.h"
#include "ThreadedScrollingCoordinator.h"
#include "IntSize.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <wtf/Function.h>
#include <wtf/RunLoop.h>
namespace WebCore {
class ScarletLayerTree;

// Independently owned input endpoint. Only the owner calls/clears m_dispatchDOM;
// foreign threads exchange events and scrolling-tree references under m_mutex.
class ScarletScrollInput final : public std::enable_shared_from_this<ScarletScrollInput> {
public:
    explicit ScarletScrollInput(Function<void(const PlatformWheelEvent&, OptionSet<WheelEventProcessingSteps>)>&&, Function<void()>&& wake);
    bool dispatch(const PlatformWheelEvent&, uint64_t epoch, const IntSize&, double scale);
    void setTree(Ref<ScrollingTreeCoordinated>&&);
    void setViewport(const IntSize&, double);
    void setEpoch(uint64_t);
    void invalidate();
private:
    bool isCurrent(uint64_t);
    void dispatchDOM(const PlatformWheelEvent&, OptionSet<WheelEventProcessingSteps>, uint64_t);
    Function<void()> m_wake; // Thread-safe compositor wake; contains no Page.
    RunLoop& m_owner;
    std::mutex m_mutex;
    RefPtr<ScrollingTreeCoordinated> m_tree;
    Function<void(const PlatformWheelEvent&, OptionSet<WheelEventProcessingSteps>)> m_dispatchDOM;
    IntSize m_size;
    double m_scale { 1 };
    uint64_t m_epoch { 0 };
    bool m_live { true };
    std::atomic<unsigned> m_pending { 0 };
};

class ScarletScrollingCoordinator final : public ThreadedScrollingCoordinator {
public:
    static Ref<ScarletScrollingCoordinator> create(Page&, ScarletLayerTree&);
private:
    ScarletScrollingCoordinator(Page&, ScarletLayerTree&);
    void didCompletePlatformRenderingUpdate() final;
};
}
#endif

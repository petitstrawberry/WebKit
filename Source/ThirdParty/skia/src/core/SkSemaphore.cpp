/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/private/SkSemaphore.h"

#include "include/private/SkFeatures.h" // IWYU pragma: keep

#if defined(SK_BUILD_FOR_MAC) || defined(SK_BUILD_FOR_IOS)
    #include <dispatch/dispatch.h>

    struct SkSemaphore::OSSemaphore {
        dispatch_semaphore_t fSemaphore;

        OSSemaphore()  { fSemaphore = dispatch_semaphore_create(0/*initial count*/); }
        ~OSSemaphore() { dispatch_release(fSemaphore); }

        void signal(int n) { while (n --> 0) { dispatch_semaphore_signal(fSemaphore); } }
        void wait() { dispatch_semaphore_wait(fSemaphore, DISPATCH_TIME_FOREVER); }
    };
#elif defined(SK_BUILD_FOR_WIN)
#include "src/core/SkLeanWindows.h"

    struct SkSemaphore::OSSemaphore {
        HANDLE fSemaphore;

        OSSemaphore()  {
            fSemaphore = CreateSemaphore(nullptr    /*security attributes, optional*/,
                                         0       /*initial count*/,
                                         MAXLONG /*max count*/,
                                         nullptr    /*name, optional*/);
        }
        ~OSSemaphore() { CloseHandle(fSemaphore); }

        void signal(int n) {
            ReleaseSemaphore(fSemaphore, n, nullptr/*returns previous count, optional*/);
        }
        void wait() { WaitForSingleObject(fSemaphore, INFINITE/*timeout in ms*/); }
    };
#elif defined(__scarlet__)
    #include <condition_variable>
    #include <mutex>
    // Native libc++ uses Scarlet's real pthread mutex/condition primitives.
    // Retain posted tokens when a signal precedes the first OS wait.
    struct SkSemaphore::OSSemaphore {
        std::mutex mutex;
        std::condition_variable ready;
        size_t tokens = 0;

        void signal(int n) {
            if (n <= 0) return;
            {
                std::lock_guard lock(mutex);
                tokens += static_cast<size_t>(n);
            }
            ready.notify_all();
        }
        void wait() {
            std::unique_lock lock(mutex);
            ready.wait(lock, [this] { return tokens != 0; });
            --tokens;
        }
    };
#else
    // It's important we test for Mach before this.  This code will compile but not work there.
    #include <errno.h>
    #include <semaphore.h>
    struct SkSemaphore::OSSemaphore {
        sem_t fSemaphore;

        OSSemaphore()  { sem_init(&fSemaphore, 0/*cross process?*/, 0/*initial count*/); }
        ~OSSemaphore() { sem_destroy(&fSemaphore); }

        void signal(int n) { while (n --> 0) { sem_post(&fSemaphore); } }
        void wait() {
            // Try until we're not interrupted.
            while(sem_wait(&fSemaphore) == -1 && errno == EINTR);
        }
    };
#endif

///////////////////////////////////////////////////////////////////////////////

SkSemaphore::~SkSemaphore() {
    delete fOSSemaphore;
}

void SkSemaphore::osSignal(int n) {
    fOSSemaphoreOnce([this] { fOSSemaphore = new OSSemaphore; });
    fOSSemaphore->signal(n);
}

void SkSemaphore::osWait() {
    fOSSemaphoreOnce([this] { fOSSemaphore = new OSSemaphore; });
    fOSSemaphore->wait();
}

bool SkSemaphore::try_wait() {
    int count = fCount.load(std::memory_order_relaxed);
    if (count > 0) {
        return fCount.compare_exchange_weak(count, count-1, std::memory_order_acquire);
    }
    return false;
}

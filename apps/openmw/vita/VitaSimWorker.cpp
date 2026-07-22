#include "VitaSimWorker.h"

#ifdef __vita__

#include <cstdio>
#include <exception>

#include <psp2/kernel/threadmgr.h>

#include "VitaInit.h"

namespace Vita
{
    namespace
    {
        thread_local bool tIsSimThread = false;
    }

    bool isSimThread()
    {
        return tIsSimThread;
    }

    SimWorker::SimWorker()
    {
        breadcrumb("[SimWorker] spawning sim thread");
        mThread = std::thread([this] { loop(); });
    }

    SimWorker::~SimWorker()
    {
        join();
    }

    void SimWorker::run(std::function<void()> work)
    {
        // Release store on mHasWork publishes mWork.
        mWork = std::move(work);
        mHasWork.store(true, std::memory_order_release);
    }

    void SimWorker::finish()
    {
        while (mHasWork.load(std::memory_order_acquire))
            sceKernelDelayThread(10);
    }

    void SimWorker::join()
    {
        if (!mThread.joinable())
            return;
        mJoinRequest.store(true, std::memory_order_release);
        mThread.join();
        breadcrumb("[SimWorker] joined");
    }

    void SimWorker::loop() noexcept
    {
        tIsSimThread = true;
        breadcrumb("[SimWorker] alive");

        while (!mJoinRequest.load(std::memory_order_acquire))
        {
            if (!mHasWork.load(std::memory_order_acquire))
            {
                sceKernelDelayThread(10);
                continue;
            }

            try
            {
                mWork();
            }
            catch (const std::exception& e)
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "[SimWorker] std::exception: %s", e.what());
                breadcrumb(buf);
            }
            catch (...)
            {
                breadcrumb("[SimWorker] non-std exception");
            }

            mWork = nullptr;
            mHasWork.store(false, std::memory_order_release);
        }
    }
}

#endif // __vita__

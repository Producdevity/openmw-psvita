#ifndef OPENMW_VITA_SIMWORKER_H
#define OPENMW_VITA_SIMWORKER_H

#ifdef __vita__

#include <atomic>
#include <functional>
#include <thread>

namespace Vita
{
    /// True when called from the SimWorker thread.
    bool isSimThread();

    /// Runs sim phases off the GL thread (vitaGL is single-threaded).
    /// Atomic polling handshake; condition_variable is unreliable on Vita.
    class SimWorker
    {
    public:
        SimWorker();
        ~SimWorker();

        SimWorker(const SimWorker&) = delete;
        SimWorker& operator=(const SimWorker&) = delete;

        /// Kick work (non-blocking).
        void run(std::function<void()> work);

        /// Block until kicked work completes.
        void finish();

        /// Stop the worker and join its thread. Safe to call more than once.
        void join();

    private:
        void loop() noexcept;

        std::function<void()> mWork;
        std::atomic<bool> mHasWork{ false };
        std::atomic<bool> mJoinRequest{ false };
        std::thread mThread;
    };
}

#endif // __vita__
#endif // OPENMW_VITA_SIMWORKER_H

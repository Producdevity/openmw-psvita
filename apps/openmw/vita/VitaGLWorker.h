#ifndef OPENMW_VITA_GLWORKER_H
#define OPENMW_VITA_GLWORKER_H

#ifdef __vita__

#include <atomic>
#include <functional>
#include <thread>

namespace Vita
{
    /// Owns the GL context; all GL runs here when `vita gl thread` is on.
    /// Same atomic-polling handshake as SimWorker (condvars unreliable).
    class GLWorker
    {
    public:
        GLWorker();
        ~GLWorker();

        GLWorker(const GLWorker&) = delete;
        GLWorker& operator=(const GLWorker&) = delete;

        /// Kick async work (non-blocking).
        void run(std::function<void()> work);

        /// Run work on the GL thread and wait for completion.
        void call(std::function<void()> work);

        /// Block until kicked work completes.
        void finish();

        void join();

    private:
        void loop() noexcept;

        std::thread mThread;
        std::function<void()> mWork;
        std::atomic<bool> mHasWork{ false };
        std::atomic<bool> mJoinRequest{ false };
    };

    /// Created on demand; null until first use.
    GLWorker* getGLWorker();
    void ensureGLWorker();
    void destroyGLWorker();
}

#endif // __vita__
#endif

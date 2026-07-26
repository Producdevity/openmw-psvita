#include "engine.hpp"

#include <cerrno>
#include <chrono>
#include <future>
#include <system_error>
#include <thread>
#ifdef __vita__
#include <malloc.h>
#endif

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgUtil/RenderBin>
#include <osgViewer/Renderer>
#include <osgViewer/ViewerEventHandlers>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>

#ifdef __vita__
#include "vita/VitaInit.h"
#include "vita/VitaMemAudit.h"
#include "vita/VitaSimWorker.h"
#include <components/vita/VitaDialogueText.h>
#include <components/vita/VitaEsmPrefetch.h>
#include <psp2/kernel/processmgr.h>
#define VITA_CRUMB(msg) Vita::breadcrumb(msg)
#else
#define VITA_CRUMB(msg)
#endif

#include <components/misc/rng.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/strings/format.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>
#include <components/files/scancache.hpp>

#include <components/version/version.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/misc/frameratelimiter.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/settings.hpp>
#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/worldimp.hpp"

#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "profile.hpp"

namespace
{
    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    struct ScreenCaptureMessageBox
    {
        void operator()(std::string filePath) const
        {
            if (filePath.empty())
            {
                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                return;
            }

            auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
            std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation()
            : GraphicsOperation("IdentifyOpenGLOperation", false)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
        }

        int getMaxTextureImageUnits() const
        {
            if (mMaxTextureImageUnits == 0)
                throw std::logic_error("mMaxTextureImageUnits is not initialized");
            return mMaxTextureImageUnits;
        }

    private:
        int mMaxTextureImageUnits = 0;
    };

    void reportStats(unsigned frameNumber, osgViewer::Viewer& viewer, std::ostream& stream)
    {
        viewer.getViewerStats()->report(stream, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        viewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(stream, frameNumber);
    }
}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mWorld->getLocalScripts();

    localScripts.startIteration();
    std::pair<ESM::RefId, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext(&script.second.getRefData().getLocals(), script.second);
        mScriptManager->run(script.first, interpreterContext);
    }
}

#ifdef __vita__
// Worker/main frame instrumentation; reported by VitaMemAudit.
extern "C"
{
    uint32_t simprof_script_us = 0, simprof_mech_us = 0, simprof_phys_us = 0, simprof_batches = 0;
    uint32_t mainprof_draw_us = 0, mainprof_swap_us = 0, mainprof_swap_max_us = 0;
    uint32_t mainprof_fence_us = 0, mainprof_frames = 0;
    // This-frame values for [Slow] attribution (averages hide spikes).
    uint32_t mainprof_lastdraw_us = 0, mainprof_lastswap_us = 0, mainprof_lastfence_us = 0;
    uint32_t vitastat_early_in_us = 0, vitastat_early_snd_us = 0;
    uint32_t vitastat_early_lsync_us = 0, vitastat_early_state_us = 0;
    // Defined in fetched-OSG RenderLeaf.cpp.
    extern int vita_draw_replay;
    extern int vita_state_replay;
    // Defined in mwinput/inputmanagerimp.cpp.
    extern uint32_t vitastat_input_cap_us;
    extern uint32_t vitastat_input_bind_us;
    extern uint32_t vitastat_input_mgr_us;
    // Defined in scene.cpp / statemanagerimp.cpp.
    extern uint32_t vitastat_stream_us;
    extern uint32_t vitastat_save_flag;
    // The ~8ms untimed main tail found by the unlocked-fps run (2026-07-24).
    uint32_t tailprof_early_us = 0, tailprof_world_us = 0, tailprof_gui_us = 0;
    uint32_t tailprof_trav_us = 0, tailprof_lua_us = 0, tailprof_frames = 0;
}
#endif

void OMW::Engine::runSimPhases(osg::Timer_t frameStart, unsigned frameNumber, float frametime, bool paused)
{
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

#ifdef __vita__
        const uint64_t simT0 = sceKernelGetProcessTimeWide();
#endif
        {
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!mWindowManager->containsMode(MWGui::GM_MainMenu) || !paused)
                {
                    if (mWorld->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mScriptManager->getGlobalScripts().run();
                    }

                    mWorld->getWorldScene().markCellAsUnchanged();
                }

                if (!paused)
                {
                    double hours = (frametime * mWorld->getTimeManager()->getGameTimeScale()) / 3600.0;
                    mWorld->advanceTime(hours, true);
#ifdef __vita__
                    // rechargeItems iterates every NPC/Creature/Container in
                    // every active cell each frame. Recharge math is
                    // FPS-invariant; throttle to ~4 Hz with accumulated dt.
                    static float s_rechargeAccum = 0.f;
                    s_rechargeAccum += frametime;
                    if (s_rechargeAccum >= 0.25f)
                    {
                        mWorld->rechargeItems(s_rechargeAccum, true);
                        s_rechargeAccum = 0.f;
                    }
#else
                    mWorld->rechargeItems(frametime, true);
#endif
                }
            }
        }

#ifdef __vita__
        const uint64_t simT1 = sceKernelGetProcessTimeWide();
        simprof_script_us += (uint32_t)(simT1 - simT0);
#endif
        // update mechanics
        {
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mMechanicsManager->update(frametime, paused);
            }

            if (mStateManager->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!paused && player.getClass().getCreatureStats(player).isDead())
                    mStateManager->endGame();
            }
        }

#ifdef __vita__
        const uint64_t simT2 = sceKernelGetProcessTimeWide();
        simprof_mech_us += (uint32_t)(simT2 - simT1);
#endif
        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
#ifdef __vita__
                try {
                    mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
                } catch (const std::exception& e) {
                    Vita::breadcrumb(("[PhysCrash] std::exception: " + std::string(e.what())).c_str());
                } catch (...) {
                    Vita::breadcrumb("[PhysCrash] non-std exception caught");
                }
#else
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
#endif
            }
        }

#ifdef __vita__
        simprof_phys_us += (uint32_t)(sceKernelGetProcessTimeWide() - simT2);
        ++simprof_batches;
#endif
}

bool OMW::Engine::frame(unsigned frameNumber, float frametime)
{
    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

    mEnvironment.setFrameDuration(frametime);

#ifdef __vita__
    uint64_t tail0 = 0, tail1 = 0, tail2 = 0;
#endif
    try
    {
#ifdef __vita__
        // Finish overlapped sim before touching game state.
        if (mSimWorker && mSimOverlap && mSimPrimed)
        {
            const uint64_t f0 = sceKernelGetProcessTimeWide();
            mSimWorker->finish();
            const uint32_t fenceUs = (uint32_t)(sceKernelGetProcessTimeWide() - f0);
            mainprof_fence_us += fenceUs;
            mainprof_lastfence_us = fenceUs;
        }
#endif
#ifdef __vita__
        tail0 = sceKernelGetProcessTimeWide();
#endif
        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }
#ifdef __vita__
        const uint64_t earlyIn = sceKernelGetProcessTimeWide();
#endif

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            if (!mWindowManager->isWindowVisible())
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else
                mSoundManager->resumePlayback();

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }
#ifdef __vita__
        const uint64_t earlySnd = sceKernelGetProcessTimeWide();
#endif

        {
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }
#ifdef __vita__
        const uint64_t earlyLua = sceKernelGetProcessTimeWide();
#endif

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }
#ifdef __vita__
        // Early-segment split for [Slow]; sim bootstrap below is the remainder.
        vitastat_early_in_us = (uint32_t)(earlyIn - tail0);
        vitastat_early_snd_us = (uint32_t)(earlySnd - earlyIn);
        vitastat_early_lsync_us = (uint32_t)(earlyLua - earlySnd);
        vitastat_early_state_us = (uint32_t)(sceKernelGetProcessTimeWide() - earlyLua);
#endif

        bool paused = mWorld->getTimeManager()->isPaused();

#ifdef __vita__
        if (mSimWorker && mSimOverlap)
        {
            // Sim already ran during draw; first frame bootstraps here.
            if (!mSimPrimed)
            {
                mSimWorker->run([&] { runSimPhases(frameStart, frameNumber, frametime, paused); });
                mSimWorker->finish();
            }
        }
        else if (mSimWorker)
        {
            // Synchronous: sim on worker, same ordering.
            mSimWorker->run([&] { runSimPhases(frameStart, frameNumber, frametime, paused); });
            mSimWorker->finish();
        }
        else
            runSimPhases(frameStart, frameNumber, frametime, paused);
#else
        runSimPhases(frameStart, frameNumber, frametime, paused);
#endif

#ifdef __vita__
        tail1 = sceKernelGetProcessTimeWide();
        tailprof_early_us += (uint32_t)(tail1 - tail0);
#endif
        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }

#ifdef __vita__
        tail2 = sceKernelGetProcessTimeWide();
        tailprof_world_us += (uint32_t)(tail2 - tail1);
#endif
        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mWindowManager->update(frametime);
        }
#ifdef __vita__
        tailprof_gui_us += (uint32_t)(sceKernelGetProcessTimeWide() - tail2);
#endif
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }

    const bool reportResource = stats->collectStats("resource");

    if (reportResource)
        stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

    mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
        stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);

        stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
    }

    mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

#ifdef __vita__
    const uint64_t tail3 = sceKernelGetProcessTimeWide();
#endif
    mViewer->eventTraversal();
    mViewer->updateTraversal();

    // update focus object for GUI
    {
        ScopedProfile<UserStatsType::Focus> profile(frameStart, frameNumber, *timer, *stats);
#ifdef __vita__
        // Full scene raycast; 10 Hz is enough for tooltips.
        static float s_focusAccum = 0.f;
        s_focusAccum += frametime;
        if (s_focusAccum >= 0.1f)
        {
            s_focusAccum = 0.f;
            mWorld->updateFocusObject();
        }
#else
        mWorld->updateFocusObject();
#endif
    }

    // if there is a separate Lua thread, it starts the update now
#ifdef __vita__
    const uint64_t tail4 = sceKernelGetProcessTimeWide();
    tailprof_trav_us += (uint32_t)(tail4 - tail3);
#endif
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);
#ifdef __vita__
    tailprof_lua_us += (uint32_t)(sceKernelGetProcessTimeWide() - tail4);
    ++tailprof_frames;
    // Slow-frame forensics: name the culprit at the moment of the drop.
    // draw/swap/fence are THIS frame's values; stream = streaming work this
    // frame (scene.cpp); save flags a savegame write this frame.
    {
        static uint64_t s_lastFrameEnd = 0;
        static uint64_t s_lastReport = 0;
        const uint64_t nowUs = sceKernelGetProcessTimeWide();
        if (s_lastFrameEnd != 0)
        {
            const uint32_t frameUs = (uint32_t)(nowUs - s_lastFrameEnd);
            if (frameUs > 40000 && nowUs - s_lastReport > 1000000)
            {
                s_lastReport = nowUs;
                char buf[256];
                snprintf(buf, sizeof(buf),
                    "[Slow] frame=%ums early=%.1f(in=%.1f[c=%.1f b=%.1f m=%.1f] snd=%.1f lsync=%.1f st=%.1f) "
                    "world=%.1f gui=%.1f trav=%.1f lua=%.1f draw=%.1f swap=%.1f fence=%.1f stream=%.1f save=%u",
                    frameUs / 1000, (tail1 - tail0) / 1000.0, vitastat_early_in_us / 1000.0,
                    vitastat_input_cap_us / 1000.0, vitastat_input_bind_us / 1000.0, vitastat_input_mgr_us / 1000.0,
                    vitastat_early_snd_us / 1000.0, vitastat_early_lsync_us / 1000.0,
                    vitastat_early_state_us / 1000.0, (tail2 - tail1) / 1000.0, (tail3 - tail2) / 1000.0,
                    (tail4 - tail3) / 1000.0, (nowUs - tail4) / 1000.0, mainprof_lastdraw_us / 1000.0,
                    mainprof_lastswap_us / 1000.0, mainprof_lastfence_us / 1000.0, vitastat_stream_us / 1000.0,
                    vitastat_save_flag);
                Vita::breadcrumb(buf);
            }
        }
        vitastat_stream_us = 0;
        vitastat_save_flag = 0;
        s_lastFrameEnd = nowUs;
    }
#endif

#ifdef __vita__
    const uint64_t renderStartUs = sceKernelGetProcessTimeWide();
    if (mSimWorker && mSimOverlap && mCullOverlap)
    {
        // Stage C: worker culls N then sims N+1; main draws N-1 meanwhile.
        // Draw lags cull by one frame (adds one frame of latency).
        auto* renderer = static_cast<osgViewer::Renderer*>(mViewer->getCamera()->getRenderer());
        if (renderer->getGraphicsThreadDoesCull())
            renderer->setGraphicsThreadDoesCull(false);
        const bool nextPaused = mWorld->getTimeManager()->isPaused();
        const float nextDt = frametime; // estimate; corrected next frame
        const unsigned nextFrame = frameNumber + 1;
        const bool havePrev = mCullPrimed;
        Vita::setDrawInFlight(true);
        mSimWorker->run([this, renderer, frameStart, nextFrame, nextDt, nextPaused] {
            renderer->cull();
            runSimPhases(frameStart, nextFrame, nextDt, nextPaused);
        });
        mSimPrimed = true;
        mCullPrimed = true;
        if (havePrev)
        {
            const uint64_t t0 = sceKernelGetProcessTimeWide();
            renderer->draw();
            const uint64_t t1 = sceKernelGetProcessTimeWide();
            mViewer->getCamera()->getGraphicsContext()->swapBuffers();
            const uint64_t t2 = sceKernelGetProcessTimeWide();
            mainprof_draw_us += (uint32_t)(t1 - t0);
            mainprof_lastdraw_us = (uint32_t)(t1 - t0);
            const uint32_t swapUs = (uint32_t)(t2 - t1);
            mainprof_lastswap_us = swapUs;
            mainprof_swap_us += swapUs;
            if (swapUs > mainprof_swap_max_us)
                mainprof_swap_max_us = swapUs;
            ++mainprof_frames;
        }
        Vita::setDrawInFlight(false);
    }
    else if (mSimWorker && mSimOverlap)
    {
        // Kick next frame's sim after cull; it overlaps draw+swap.
        // Draw reads cull-cached matrices, so sim writes are safe.
        auto* renderer = static_cast<osgViewer::Renderer*>(mViewer->getCamera()->getRenderer());
        // Else cull() no-ops and draw() blocks forever.
        if (renderer->getGraphicsThreadDoesCull())
            renderer->setGraphicsThreadDoesCull(false);
        renderer->cull();
        const bool nextPaused = mWorld->getTimeManager()->isPaused();
        const float nextDt = frametime; // estimate; corrected next frame
        const unsigned nextFrame = frameNumber + 1;
        Vita::setDrawInFlight(true);
        mSimWorker->run([this, frameStart, nextFrame, nextDt, nextPaused] {
            runSimPhases(frameStart, nextFrame, nextDt, nextPaused);
        });
        mSimPrimed = true;
        {
            const uint64_t t0 = sceKernelGetProcessTimeWide();
            renderer->draw();
            const uint64_t t1 = sceKernelGetProcessTimeWide();
            mViewer->getCamera()->getGraphicsContext()->swapBuffers();
            const uint64_t t2 = sceKernelGetProcessTimeWide();
            mainprof_draw_us += (uint32_t)(t1 - t0);
            mainprof_lastdraw_us = (uint32_t)(t1 - t0);
            const uint32_t swapUs = (uint32_t)(t2 - t1);
            mainprof_lastswap_us = swapUs;
            mainprof_swap_us += swapUs;
            if (swapUs > mainprof_swap_max_us)
                mainprof_swap_max_us = swapUs;
            ++mainprof_frames;
        }
        Vita::setDrawInFlight(false);
    }
    else
        mViewer->renderingTraversals();
    Vita::noteRenderTime(sceKernelGetProcessTimeWide() - renderStartUs);
#else
    mViewer->renderingTraversals();
#endif

    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);

#ifdef __vita__
    Vita::auditFrameStats(*mViewer);
#endif

    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
    : mWindow(nullptr)
    , mEncoding(ToUTF8::WINDOWS_1252)
    , mScreenCaptureOperation(nullptr)
    , mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
    , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    , mStereoManager(nullptr)
    , mSkipMenu(false)
    , mUseSound(true)
    , mCompileAll(false)
    , mCompileAllDialogue(false)
    , mWarningsMode(1)
    , mScriptConsoleMode(false)
    , mActivationDistanceOverride(-1)
    , mGrab(true)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mCfgMgr(configurationManager)
    , mGlMaxTextureImageUnits(0)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

#ifdef __vita__
    // No SDL_INIT_SENSOR: sceMotion polling inside SDL_PumpEvents costs
    // intermittent 8-13ms spikes, and the port doesn't use gyro aiming.
    Uint32 flags = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
#else
    Uint32 flags
        = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
#endif
    if (SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if (SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
    {
        mScreenCaptureOperation->stop();
        mScreenCaptureOperation = nullptr;
    }
    mScreenCaptureHandler = nullptr;

    mMechanicsManager = nullptr;
    mDialogueManager = nullptr;
    mJournal = nullptr;
    mWindowManager = nullptr;
    mScriptManager = nullptr;
    mWorld = nullptr;
    mStereoManager = nullptr;
    mSoundManager = nullptr;
    mInputManager = nullptr;
    mStateManager = nullptr;
    mLuaWorker = nullptr;
#ifdef __vita__
    mSimWorker = nullptr;
#endif
    mLuaManager = nullptr;
    mL10nManager = nullptr;

    mScriptContext = nullptr;

    mUnrefQueue = nullptr;
    mWorkQueue = nullptr;

    mViewer = nullptr;

    mResourceSystem.reset();

    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();

    Log(Debug::Info) << "Quitting peacefully.";
}

// Set data dir

void OMW::Engine::setDataDirs(const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), mResDir / "vfs");
    mFileCollections = Files::Collections(mDataDirs);

#ifdef __vita__
    const auto cachePath = mCfgMgr.getUserConfigPath() / "scan_cache.bin";
    Files::Collections::CollectionsMap cached;
    if (Files::loadScanCache(cachePath, mDataDirs, cached))
        mFileCollections.setCollections(std::move(cached));
#endif
}

// Add BSA archive
void OMW::Engine::addArchive(const std::string& archive)
{
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir(const std::filesystem::path& parResDir)
{
    mResDir = parResDir;
    if (!Version::checkResourcesVersion(mResDir))
        Log(Debug::Error) << "Resources dir " << mResDir
                          << " doesn't match OpenMW binary, the game may work incorrectly.";
}

// Set start cell name
void OMW::Engine::setCell(const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu(bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::createWindow()
{
    const int screen = Settings::video().mScreen;
    const int width = Settings::video().mResolutionX;
    const int height = Settings::video().mResolutionY;
    const Settings::WindowMode windowMode = Settings::video().mWindowMode;
    const bool windowBorder = Settings::video().mWindowBorder;
    const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
    unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);

    if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

#ifdef __vita__
    Uint32 flags = SDL_WINDOW_SHOWN;
#else
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    if (windowMode == Settings::WindowMode::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    else if (windowMode == Settings::WindowMode::WindowedFullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    // Allows for Windows snapping features to properly work in borderless window
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");

    if (!windowBorder)
        flags |= SDL_WINDOW_BORDERLESS;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

    checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
    if (Debug::shouldDebugOpenGL())
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));

    if (antialiasing > 0)
    {
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
    }

    osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow;
    while (!graphicsWindow || !graphicsWindow->valid())
    {
        while (!mWindow)
        {
            mWindow = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
            if (!mWindow)
            {
                // Try with a lower AA
                if (antialiasing > 0)
                {
                    Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                        << antialiasing / 2;
                    antialiasing /= 2;
                    Settings::video().mAntialiasing.set(antialiasing);
                    checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                    continue;
                }
                else
                {
                    std::stringstream error;
                    error << "Failed to create SDL window: " << SDL_GetError();
                    throw std::runtime_error(error.str());
                }
            }
        }

        // Since we use physical resolution internally, we have to create the window with scaled resolution,
        // but we can't get the scale before the window exists, so instead we have to resize aftewards.
        int w, h;
        SDL_GetWindowSize(mWindow, &w, &h);
        int dw, dh;
        SDL_GL_GetDrawableSize(mWindow, &dw, &dh);
        if (dw != w || dh != h)
        {
            SDL_SetWindowSize(mWindow, width / (dw / w), height / (dh / h));
        }

        setWindowIcon();

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
        SDL_GL_GetDrawableSize(mWindow, &traits->width, &traits->height);
        traits->windowName = SDL_GetWindowTitle(mWindow);
        traits->windowDecoration = !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_BORDERLESS);
        traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
        traits->vsync = 0;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

        graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
        if (!graphicsWindow->valid())
            throw std::runtime_error("Failed to create GraphicsContext");

        if (traits->samples < antialiasing)
        {
            Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of "
                                << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
            graphicsWindow->closeImplementation();
            SDL_DestroyWindow(mWindow);
            mWindow = nullptr;
            antialiasing /= 2;
            Settings::video().mAntialiasing.set(antialiasing);
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            continue;
        }

        if (traits->red < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
        if (traits->green < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
        if (traits->blue < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
        if (traits->depth < 24)
            Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";

        traits->alpha = 0; // set to 0 to stop ScreenCaptureHandler reading the alpha channel
    }

    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsWindow);
    camera->setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

    osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
    mViewer->setRealizeOperation(realizeOperations);
    osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
    realizeOperations->add(identifyOp);
    realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());

    if (Debug::shouldDebugOpenGL())
        realizeOperations->add(new Debug::EnableGLDebugOperation());

    realizeOperations->add(mSelectDepthFormatOperation);
    realizeOperations->add(mSelectColorFormatOperation);

    if (Stereo::getStereo())
    {
        Stereo::Settings settings;

        settings.mMultiview = Settings::stereo().mMultiview;
        settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
        settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;

        if (Settings::stereo().mUseCustomView)
        {
            const osg::Vec3 leftEyeOffset(Settings::stereoView().mLeftEyeOffsetX,
                Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ);

            const osg::Quat leftEyeOrientation(Settings::stereoView().mLeftEyeOrientationX,
                Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                Settings::stereoView().mLeftEyeOrientationW);

            const osg::Vec3 rightEyeOffset(Settings::stereoView().mRightEyeOffsetX,
                Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ);

            const osg::Quat rightEyeOrientation(Settings::stereoView().mRightEyeOrientationX,
                Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                Settings::stereoView().mRightEyeOrientationW);

            settings.mCustomView = Stereo::CustomView{
                .mLeft = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = leftEyeOffset,
                        .orientation = leftEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                        .angleRight = Settings::stereoView().mLeftEyeFovRight,
                        .angleUp = Settings::stereoView().mLeftEyeFovUp,
                        .angleDown = Settings::stereoView().mLeftEyeFovDown,
                    },
                },
                .mRight = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = rightEyeOffset,
                        .orientation = rightEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                        .angleRight = Settings::stereoView().mRightEyeFovRight,
                        .angleUp = Settings::stereoView().mRightEyeFovUp,
                        .angleDown = Settings::stereoView().mRightEyeFovDown,
                    },
                },
            };
        }

        if (Settings::stereo().mUseCustomEyeResolution)
            settings.mEyeResolution
                = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);

        realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
    }

    mViewer->realize();
    mGlMaxTextureImageUnits = identifyOp->getMaxTextureImageUnits();

#ifdef __vita__
    // Enable OSG matrix uniforms and vertex attribute aliasing for custom GLSL shaders.
    // Matrix uniforms: OSG provides osg_ModelViewProjectionMatrix, osg_ModelViewMatrix, osg_NormalMatrix.
    // Attribute aliasing: OSG maps glVertexPointer->glVertexAttribPointer(0), etc., so custom shaders
    // receive vertex data through generic attribute slots instead of FFP-only arrays.
    if (auto* gc = mViewer->getCamera()->getGraphicsContext())
    {
        gc->getState()->setUseModelViewAndProjectionUniforms(true);
        gc->getState()->setUseVertexAttributeAliasing(true);
    }
#endif

    mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
        0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
}

void OMW::Engine::setWindowIcon()
{
    std::ifstream windowIconStream;
    const auto windowIcon = mResDir / "openmw.png";
    windowIconStream.open(windowIcon, std::ios_base::in | std::ios_base::binary);
    if (windowIconStream.fail())
        Log(Debug::Error) << "Error: Failed to open " << windowIcon;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    if (!reader)
    {
        Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
        return;
    }
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code "
                          << result.status();
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface.get());
    }
}

void OMW::Engine::prepareEngine()
{
    VITA_CRUMB("prepareEngine() enter");
    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    const bool stereoEnabled = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
    mStereoManager = std::make_unique<Stereo::Manager>(
        mViewer, stereoEnabled, Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    osg::ref_ptr<osg::Group> rootNode(new osg::Group);
    mViewer->setSceneData(rootNode);

    VITA_CRUMB("prepareEngine() createWindow");
#ifdef __vita__
    Vita::logMemoryStatus("Pre-createWindow");
#endif
    createWindow();

    mVFS = std::make_unique<VFS::Manager>();

#ifdef __vita__
    {
        auto cacheDir = mCfgMgr.getUserConfigPath();
        if (mForceRescan)
        {
            Log(Debug::Info) << "Force rescan — clearing VFS caches";
            Files::clearScanCache(cacheDir / "scan_cache.bin");
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec))
                if (entry.path().filename().string().starts_with("vfs_dir_"))
                    std::filesystem::remove(entry.path(), ec);
        }
        VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true,
            &mEncoder.get()->getStatelessEncoder(), cacheDir);
    }

    {
        // Kick ESM reads now; the parser consumes them after GUI bring-up.
        std::vector<std::filesystem::path> toPrefetch;
        for (const std::string& file : mContentFiles)
        {
            if (Misc::getFileExtension(file) == "omwscripts")
                continue;
            const Files::MultiDirCollection& col = mFileCollections.getCollection(Misc::getFileExtension(file));
            if (col.doesExist(file))
                toPrefetch.push_back(col.getPath(file));
        }
        Vita::EsmPrefetch::start(std::move(toPrefetch));
    }
#else
    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());
#endif

    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder());
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);
#ifdef __vita__
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(true); // release CPU-side image after GPU upload
#else
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(
        false); // keep to Off for now to allow better state sharing
#endif
    mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
        Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap,
        static_cast<float>(Settings::general().mAnisotropy));
    mEnvironment.setResourceSystem(*mResourceSystem);

    mWorkQueue = new SceneUtil::WorkQueue(Settings::cells().mPreloadNumThreads);
    mUnrefQueue = std::make_unique<SceneUtil::UnrefQueue>();

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(mCfgMgr.getScreenshotPath(),
            Settings::general().mScreenshotFormat,
            Settings::general().mNotifyOnSavedScreenshot ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                                                         : std::function<void(std::string)>(IgnoreString{})));

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);

    mViewer->addEventHandler(mScreenCaptureHandler);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);

    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    if (!keybinderUserExists)
    {
        const auto input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml");
        if (std::filesystem::exists(input2))
        {
            keybinderUserExists = std::filesystem::copy_file(input2, keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";

    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;

    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }
    // else if it doesn't exist, pass in an empty path

    // gui needs our shaders path before everything else
    mResourceSystem->getSceneManager()->setShaderPath(mResDir / "shaders");

    osg::GLExtensions& exts = SceneUtil::getGLExtensions();
    bool shadersSupported = exts.glslLanguageVersion >= 1.2f;
#ifdef __vita__
    // vitaGL cannot compile OpenMW's GLSL shaders — force fixed-function path
    shadersSupported = false;
#endif

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
        exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    mStereoManager->disableStereoForNode(guiRoot);
    rootNode->addChild(guiRoot);

#ifdef __vita__
    Vita::logMemoryStatus("Post-VFS");
#endif
    VITA_CRUMB("prepareEngine() creating WindowManager");
    mWindowManager = std::make_unique<MWGui::WindowManager>(mWindow, mViewer, guiRoot, mResourceSystem.get(),
        mWorkQueue.get(), mCfgMgr.getLogPath(), mScriptConsoleMode, mTranslationDataStorage, mEncoding, mExportFonts,
        Version::getOpenmwVersionDescription(), shadersSupported, mCfgMgr);
    mEnvironment.setWindowManager(*mWindowManager);

    VITA_CRUMB("prepareEngine() creating InputManager");
    mInputManager = std::make_unique<MWInput::InputManager>(mWindow, mViewer, mScreenCaptureHandler, keybinderUser,
        keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    mEnvironment.setInputManager(*mInputManager);

    // Create sound system
#ifdef __vita__
    Vita::logMemoryStatus("Post-WindowManager");
#endif
    VITA_CRUMB("prepareEngine() creating SoundManager");
    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    // Create the world
    VITA_CRUMB("prepareEngine() creating World");
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
#ifdef __vita__
    if (Settings::general().mVitaLazyDialogue)
    {
        Vita::DialogueText::setEnabled(true);
        Vita::DialogueText::setEncoding(mEncoding);
    }
    Vita::logMemoryStatus("Pre-data-load");
    VITA_CRUMB("prepareEngine() loading data async");
    // Parse chases the prefetch reads started before createWindow.
    listener->loadingOn();
    {
        auto dataLoading = std::async(std::launch::async,
            [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    listener->loadingOff();
    // Coalesce freed slurp buffers before initUI's allocation storm.
    malloc_trim(0);
    Files::saveScanCache(
        mCfgMgr.getUserConfigPath() / "scan_cache.bin", mDataDirs, mFileCollections.getCollections());
    Vita::logMemoryStatus("Post-data-load");
#else
    VITA_CRUMB("prepareEngine() loading data async");
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    if (!mSkipMenu)
    {
        std::string_view logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, true);
    }

    listener->loadingOn();
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    listener->loadingOff();
#endif
    VITA_CRUMB("prepareEngine() data loaded");

    VITA_CRUMB("prepareEngine() world init");
#ifdef __vita__
    Vita::logMemoryStatus("Pre-World::init");
#endif
    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue);
#ifdef __vita__
    Vita::logMemoryStatus("Post-World::init");
#endif
    VITA_CRUMB("prepareEngine() world init done");
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    mWorld->setRandomSeed(mRandomSeed);

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader([gmst, misses = std::set<std::string, Misc::StringUtils::CiComp>()](
                                    std::string_view gmstName) mutable -> const std::string* {
        const ESM::GameSetting* res = gmst->search(gmstName);
        if (res && res->mValue.getType() == ESM::VT_String)
            return &res->mValue.getString();
        if (misses.emplace(gmstName).second)
            Log(Debug::Error) << "GMST " << gmstName << " not found";
        return nullptr;
    });

    mWindowManager->setStore(mWorld->getStore());
#ifdef __vita__
    Vita::logMemoryStatus("Pre-initUI");
#endif
    mWindowManager->initUI();
#ifdef __vita__
    Vita::logMemoryStatus("Post-initUI");
    // UI textures land in the image cache; break down the initUI pool.
    Vita::auditResourceCaches(mResourceSystem.get());
#endif

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);

    // Create script system
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    // Create game mechanics system
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);

    // Create dialog system
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }

#ifdef __vita__
    Vita::logMemoryStatus("Pre-LuaInit");
#endif
    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->init();

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
#ifdef __vita__
    Vita::logMemoryStatus("Post-LuaInit");
#endif
    VITA_CRUMB("prepareEngine() done");
}

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert(!mContentFiles.empty());

    Log(Debug::Info) << "OSG version: " << osgGetVersion();
    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "."
                     << (int)sdlVersion.patch;

    Misc::Rng::init(mRandomSeed);

    Settings::ShaderManager::get().load(mCfgMgr.getUserConfigPath() / "shaders.yaml");

    MWClass::registerClasses();

    // Create encoder
    mEncoder = std::make_unique<ToUTF8::Utf8Encoder>(mEncoding);

    // Setup viewer
    VITA_CRUMB("go() creating viewer");
    mViewer = new osgViewer::Viewer;
    mViewer->setReleaseContextAtEndOfFrameHint(false);

#ifdef __vita__
    // DrawThreadPerContext crashes on launch — vitaGL/SceGxm isn't safe
    // for draw submission from a non-main thread.
    mViewer->setThreadingModel(osgViewer::ViewerBase::SingleThreaded);
    // Bin sort mode A/B via setting.
    if (Settings::general().mVitaStateSortedBins)
        osgUtil::RenderBin::setDefaultRenderBinSortMode(osgUtil::RenderBin::SORT_BY_STATE);
    else
        osgUtil::RenderBin::setDefaultRenderBinSortMode(osgUtil::RenderBin::TRAVERSAL_ORDER);
    // vitaGL is single-threaded; sim moves to a worker instead.
    vita_draw_replay = Settings::general().mVitaDrawReplay ? 1 : 0;
    vita_state_replay = Settings::general().mVitaStateReplay ? 1 : 0;
    if (Settings::general().mVitaSimThread)
    {
        mSimWorker = std::make_unique<Vita::SimWorker>();
        mSimOverlap = Settings::general().mVitaSimOverlap;
        mCullOverlap = Settings::general().mVitaCullOverlap;
        // Nested render loops (loading, video) must consume a pending
        // culled frame first or the queue serves them a stale scene.
        Vita::setDrainDrawHook([this] {
            if (!mCullPrimed)
                return;
            // From sim thread: cull already ran (it precedes sim in the batch);
            // finish() here would deadlock on our own job.
            if (!Vita::isSimThread())
                mSimWorker->finish();
            auto* renderer = static_cast<osgViewer::Renderer*>(mViewer->getCamera()->getRenderer());
            renderer->draw();
            mCullPrimed = false;
        });
    }
#endif

    // Do not try to outsmart the OS thread scheduler (see bug #4785).
    mViewer->setUseConfigureAffinity(false);

    mEnvironment.setFrameRateLimit(Settings::video().mFramerateLimit);

    prepareEngine();

#ifdef _WIN32
    const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
    const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif

    std::filesystem::path path;
    if (statsFile != nullptr)
        path = statsFile;

    std::ofstream stats;
    if (!path.empty())
    {
        stats.open(path, std::ios_base::out);
        if (stats.is_open())
            Log(Debug::Info) << "OSG stats will be written to: " << path;
        else
            Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                << "\": " << std::generic_category().message(errno);
    }

    // Setup profiler
    osg::ref_ptr<Resource::Profiler> statsHandler = new Resource::Profiler(stats.is_open(), *mVFS);

    initStatsHandler(*statsHandler);

    mViewer->addEventHandler(statsHandler);

    osg::ref_ptr<Resource::StatsHandler> resourcesHandler = new Resource::StatsHandler(stats.is_open(), *mVFS);
    mViewer->addEventHandler(resourcesHandler);

    if (stats.is_open())
        Resource::collectStatistics(*mViewer);

    // Start the game
    VITA_CRUMB("go() starting game");
    if (!mSaveGameFile.empty())
    {
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        VITA_CRUMB("go() pushing main menu");
        mWindowManager->pushGuiMode(MWGui::GM_MainMenu);

        if (mVFS->exists(MWSound::titleMusic))
            mSoundManager->streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        else
            Log(Debug::Warning) << "Title music not found";

        std::string_view logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, /*allowSkipping*/ true, /*overrideSounds*/ false);
    }
    else
    {
        mStateManager->newGame(!mNewGame);
    }

    if (!mStartupScript.empty() && mStateManager->getState() == MWState::StateManager::State_Running)
    {
        mWindowManager->executeInConsole(mStartupScript);
    }

    // Start the main rendering loop
    VITA_CRUMB("go() entering main loop");
    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
#ifdef __vita__
    constexpr float kDynFogMin = 1500.f;
    constexpr float kDynFogMinInterior = 700.f;
    constexpr float kDynFogMax = 5000.f;
    constexpr float kDynFogUserMoveThreshold = 300.f;
    constexpr float kDynFogDeadband = 0.5f;
    constexpr float kDynFogEmaAlpha = 0.15f;
    constexpr float kDynFogLerpHz = 4.0f;
    constexpr auto kDynFogAdjustInterval = std::chrono::milliseconds(250);
    float vitaDynFogTarget = Settings::camera().mViewingDistance;
    float vitaDynFogLastWritten = Settings::camera().mViewingDistance;
    bool vitaDynFogWasOn = false;
    // Seed EMA with the configured target period so the first tick isn't biased.
    float vitaDynFogEmaDt = 1.0f / std::max(15.f, Settings::camera().mVitaDynFogTargetFps.get());
    auto vitaDynFogLastAdjust = std::chrono::steady_clock::now();
    bool vitaDynFogWasRunning = false;
    bool vitaDynFogWasInInterior = false;
#endif
    while (!mViewer->done() && !mStateManager->hasQuitRequest())
    {
        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                              .count()
            * timeManager.getSimulationTimeScale();

#ifdef __vita__
        // Cull overlap: worker reads the frame stamp; idle it before advance.
        if (mSimWorker && mCullOverlap && mSimPrimed)
        {
            const uint64_t f0 = sceKernelGetProcessTimeWide();
            mSimWorker->finish();
            const uint32_t fenceUs = (uint32_t)(sceKernelGetProcessTimeWide() - f0);
            mainprof_fence_us += fenceUs;
            mainprof_lastfence_us = fenceUs;
        }
#endif
        mViewer->advance(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();

        if (!frame(frameNumber, static_cast<float>(dt)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        timeManager.updateIsPaused();
        if (!timeManager.isPaused())
        {
            timeManager.setSimulationTime(timeManager.getSimulationTime() + dt);
            timeManager.setRenderingSimulationTime(timeManager.getRenderingSimulationTime() + dt);
        }

        if (stats)
        {
            // The delay is required because rendering happens in parallel to the main thread and stats from there is
            // available with delay.
            constexpr unsigned statsReportDelay = 3;
            if (frameNumber >= statsReportDelay)
            {
                // Viewer frame number can be different from frameNumber because of loading screens which render new
                // frames inside a simulation frame.
                const unsigned currentFrameNumber = mViewer->getFrameStamp()->getFrameNumber();
                for (unsigned i = frameNumber; i <= currentFrameNumber; ++i)
                    reportStats(i - statsReportDelay, *mViewer, stats);
            }
        }

#ifdef __vita__
        {
            double frameDt = std::chrono::duration<double>(
                frameRateLimiter.getLastFrameDuration()).count();
            // Skip cell-load / pause spikes so they don't poison the fps EMA.
            if (frameDt > 0.001 && frameDt < 0.2)
                vitaDynFogEmaDt = (1.0f - kDynFogEmaAlpha) * vitaDynFogEmaDt
                    + kDynFogEmaAlpha * static_cast<float>(frameDt);

            bool dynFogOn = Settings::camera().mVitaDynamicFog;
            const float dynFogTargetFps
                = std::max(15.f, Settings::camera().mVitaDynFogTargetFps.get());
            auto pickFloor = [&]() -> std::pair<float, float> {
                struct Tier { float fps, ext, intr; };
                static constexpr Tier kTable[] = {
                    { 15.f, 2000.f, 1000.f },
                    { 18.f, 1700.f,  850.f },
                    { 20.f, 1500.f,  700.f },
                };
                const Tier* pick = &kTable[2]; // default "20 (Balanced)"
                float best = std::abs(dynFogTargetFps - kTable[2].fps);
                for (const Tier& t : kTable)
                {
                    float d = std::abs(dynFogTargetFps - t.fps);
                    if (d < best) { best = d; pick = &t; }
                }
                return { pick->ext, pick->intr };
            };
            const auto [floorExt, floorInt] = pickFloor();
            const bool isRunning = (mStateManager->getState() == MWBase::StateManager::State_Running);
            bool isInInterior = false;
            float effectiveMin = floorExt;
            if (isRunning)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                MWWorld::CellStore* cell = player.getCell();
                if (cell && !cell->isExterior())
                {
                    isInInterior = true;
                    effectiveMin = floorInt;
                }
            }

            // Snap fog to the floor so it always *grows* from tight toward the
            const bool dynFogJustEnabled = dynFogOn && !vitaDynFogWasOn;
            const bool justLoadedGame = dynFogOn && isRunning && !vitaDynFogWasRunning;
            const bool enteredInterior = dynFogOn && isInInterior && !vitaDynFogWasInInterior;
            if (dynFogJustEnabled || justLoadedGame || enteredInterior)
            {
                vitaDynFogTarget = effectiveMin;
                vitaDynFogLastWritten = effectiveMin;
                Settings::camera().mViewingDistance.set(effectiveMin);
                static const Settings::CategorySettingVector kFilter{ { "Camera", "viewing distance" } };
                const auto changes = Settings::Manager::getPendingChanges(kFilter);
                if (!changes.empty())
                {
                    mWorld->processChangedSettings(changes);
                    Settings::Manager::resetPendingChanges(kFilter);
                }
                vitaDynFogLastAdjust = std::chrono::steady_clock::now();
            }
            vitaDynFogWasOn = dynFogOn;
            vitaDynFogWasRunning = isRunning;
            vitaDynFogWasInInterior = isInInterior;

            if (dynFogOn)
            {
                float current = Settings::camera().mViewingDistance;
                // Compare to what we last wrote, not to target — the lerp lags target naturally.
                if (std::abs(current - vitaDynFogLastWritten) > kDynFogUserMoveThreshold)
                {
                    Settings::camera().mVitaDynamicFog.set(false);
                    vitaDynFogWasOn = false;
                }
                else
                {
                    // Resolve aggression profile each tick (cheap, allows live UI changes)
                    const std::string& aggStr = Settings::camera().mVitaDynFogAggression.get();
                    float shrinkCoef = 50.f, shrinkMaxAbs = 500.f, shrinkLerpHz = 6.f; // aggressive default
                    if (aggStr == "normal")
                    {
                        shrinkCoef = 30.f; shrinkMaxAbs = 300.f; shrinkLerpHz = 4.f;
                    }
                    else if (aggStr == "very aggressive")
                    {
                        shrinkCoef = 80.f; shrinkMaxAbs = 800.f; shrinkLerpHz = 8.f;
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (now - vitaDynFogLastAdjust >= kDynFogAdjustInterval)
                    {
                        vitaDynFogLastAdjust = now;
                        float fps = 1.0f / vitaDynFogEmaDt;
                        float fpsGap = fps - dynFogTargetFps;
                        float step = 0.f;
                        if (fpsGap < -kDynFogDeadband)
                        {
                            // Two-tier shrink: catastrophic snap is fixed (always
                            // saves you), proportional response scales with the
                            // aggression setting.
                            if (fps < 15.f)
                                step = -2000.f;
                            else
                                step = std::clamp(fpsGap * shrinkCoef, -shrinkMaxAbs, -20.f);
                        }
                        else if (fpsGap > kDynFogDeadband)
                            step = std::clamp(fpsGap * 20.f, 20.f, 150.f);
                        const float userMax = std::clamp(
                            Settings::camera().mVitaDynFogMaxDistance.get(), effectiveMin, kDynFogMax);
                        vitaDynFogTarget
                            = std::clamp(vitaDynFogTarget + step, effectiveMin, userMax);
                    }


                    const float targetDelta = vitaDynFogTarget - current;
                    float lerpHz = kDynFogLerpHz;
                    if (targetDelta < -1200.f)
                        lerpHz = 16.f;
                    else if (targetDelta < 0.f && targetDelta > -1200.f)
                        lerpHz = shrinkLerpHz; // proportional shrink uses aggression-tuned rate
                    float lerpT = 1.0f - std::exp(-lerpHz * static_cast<float>(frameDt));
                    float newDist = current + targetDelta * lerpT;
                    if (std::abs(newDist - current) > 0.5f)
                    {
                        Settings::camera().mViewingDistance.set(newDist);
                        vitaDynFogLastWritten = newDist;
                        static const Settings::CategorySettingVector kFilter{
                            { "Camera", "viewing distance" } };
                        const auto changes
                            = Settings::Manager::getPendingChanges(kFilter);
                        if (!changes.empty())
                        {
                            mWorld->processChangedSettings(changes);
                            Settings::Manager::resetPendingChanges(kFilter);
                        }
                    }
                }
            }
        }
#endif
        frameRateLimiter.limit();
    }

    mLuaWorker->join();
#ifdef __vita__
    if (mSimWorker)
        mSimWorker->join();
#endif

    // Save user settings
    Settings::Manager::saveUser(mCfgMgr.getUserConfigPath() / "settings.cfg");
    Settings::ShaderManager::get().save();
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath());

#ifdef __vita__
    // Skip C++ static destructors — vitaGL/OSG/Bullet teardown paths can hang
    // on shutdown, leaving the app stuck instead of returning to LiveArea.
    // Saves above are already complete; nothing else needs to run for a clean
    // exit from the user's perspective.
    sceKernelExitProcess(0);
#endif
}

void OMW::Engine::setCompileAll(bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue(bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode(bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript(const std::filesystem::path& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride(int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode(int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::filesystem::path& savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}

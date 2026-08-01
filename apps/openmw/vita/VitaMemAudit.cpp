#ifdef __vita__

#include "VitaMemAudit.h"
#include "VitaInit.h"
#include "VitaSimWorker.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include <vitaGL.h>

extern "C"
{
    extern uint32_t osgprof_mat_us, osgprof_state_us, osgprof_unif_us, osgprof_draw_us, osgprof_leaves;
    extern uint32_t osgprof_replayable, osgprof_replayed, osgprof_streplayed;
    extern unsigned int cullprof_node, cullprof_group, cullprof_transform, cullprof_geode, cullprof_drawable,
        cullprof_dcull, cullprof_leaves, cullprof_sg, cullprof_xf_bone;
    extern uint32_t vgl_memo_hits, vgl_memo_miss;
    unsigned int cullprof_creplay = 0, cullprof_crep_drop = 0;
    uint32_t cullprof_terr_us = 0;
    extern uint32_t osgapply_calls, osgapply_tex_us, osgapply_mode_us, osgapply_attr_us, osgapply_unif_us;
    extern uint32_t osgapply_push, osgapply_pop;
}

#include <cstdint>

#include <osg/Camera>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/NodeVisitor>
#include <osg/Stats>
#include <osgViewer/Viewer>

#include <components/esm3/loaddial.hpp>
#include <components/esm3/loadinfo.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadscpt.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/lightmanager.hpp>

#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/worldmodel.hpp"

namespace
{
    void auditLog(const char* buf)
    {
        sceClibPrintf("%s\n", buf);
        vitaMemBreadcrumb(buf);
    }

    // String heap bytes beyond SSO (newlib SSO = 15).
    size_t strHeapBytes(const std::string& s)
    {
        return s.capacity() > 15 ? s.capacity() + 1 : 0;
    }

    // malloc + std::list node overhead.
    constexpr size_t kListNodeOverhead = 2 * sizeof(void*) + 8;

    size_t dialInfoBytes(const ESM::DialInfo& info)
    {
        size_t bytes = sizeof(ESM::DialInfo) + kListNodeOverhead;
        bytes += strHeapBytes(info.mSound);
        bytes += strHeapBytes(info.mResponse);
        bytes += strHeapBytes(info.mResultScript);
        bytes += info.mSelects.capacity() * sizeof(ESM::DialogueCondition);
        for (const auto& select : info.mSelects)
            bytes += strHeapBytes(select.mVariable);
        return bytes;
    }

    // Geometry bytes under a node; textures counted by the image audit.
    class GeometryBytesVisitor : public osg::NodeVisitor
    {
    public:
        GeometryBytesVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Drawable& drawable) override
        {
            if (const osg::Geometry* geom = drawable.asGeometry())
            {
                addArray(geom->getVertexArray());
                addArray(geom->getNormalArray());
                addArray(geom->getColorArray());
                addArray(geom->getSecondaryColorArray());
                addArray(geom->getFogCoordArray());
                for (const auto& tc : geom->getTexCoordArrayList())
                    addArray(tc.get());
                for (const auto& va : geom->getVertexAttribArrayList())
                    addArray(va.get());
                for (const auto& ps : geom->getPrimitiveSetList())
                    if (ps)
                        mBytes += ps->getTotalDataSize();
            }
            traverse(drawable);
        }

        size_t mBytes = 0;

    private:
        void addArray(const osg::Array* array)
        {
            if (array)
                mBytes += array->getTotalDataSize();
        }
    };
}

namespace Vita
{
    void auditDialogueStore(const MWWorld::ESMStore& store)
    {
        const auto& dialogues = store.get<ESM::Dialogue>();

        size_t topics = 0;
        size_t infos = 0;
        size_t liveBytes = 0; // Dialogue::mInfo — the list actually used at runtime
        size_t dupBytes = 0; // InfoOrder::mOrderedInfo — load-order copy never freed

        for (auto it = dialogues.begin(); it != dialogues.end(); ++it)
        {
            const ESM::Dialogue& dial = *it;
            ++topics;
            liveBytes += sizeof(ESM::Dialogue) + strHeapBytes(dial.mStringId);
            for (const ESM::DialInfo& info : dial.mInfo)
            {
                ++infos;
                liveBytes += dialInfoBytes(info);
            }
            for (const ESM::DialInfo& info : dial.mInfoOrder.getOrderedInfo())
                dupBytes += dialInfoBytes(info);
        }

        char buf[256];
        snprintf(buf, sizeof(buf),
            "[VitaAudit] dialogue: %u topics, %u infos, live=%uKB, infoOrderDup=%uKB, total=%uMB",
            (unsigned)topics, (unsigned)infos, (unsigned)(liveBytes / 1024), (unsigned)(dupBytes / 1024),
            (unsigned)((liveBytes + dupBytes) / (1024 * 1024)));
        auditLog(buf);

        // OpenMW recompiles from source; vanilla bytecode is dead weight.
        const auto& scripts = store.get<ESM::Script>();
        size_t scriptCount = 0;
        size_t textBytes = 0;
        size_t vanillaBytes = 0;
        size_t varBytes = 0;
        for (auto it = scripts.begin(); it != scripts.end(); ++it)
        {
            ++scriptCount;
            textBytes += strHeapBytes(it->mScriptText);
            vanillaBytes += it->mScriptData.capacity();
            for (const auto& var : it->mVarNames)
                varBytes += sizeof(std::string) + strHeapBytes(var);
        }
        snprintf(buf, sizeof(buf),
            "[VitaAudit] scripts: %u records, sourceText=%uKB, vanillaBytecode=%uKB (unused), varNames=%uKB",
            (unsigned)scriptCount, (unsigned)(textBytes / 1024), (unsigned)(vanillaBytes / 1024),
            (unsigned)(varBytes / 1024));
        auditLog(buf);

        snprintf(buf, sizeof(buf), "[VitaAudit] store counts: cells=%u, lands=%u",
            (unsigned)store.get<ESM::Cell>().getSize(), (unsigned)store.get<ESM::Land>().getSize());
        auditLog(buf);
    }

    void auditWorldModel(MWWorld::WorldModel& worldModel)
    {
        size_t total = 0;
        size_t loaded = 0;
        size_t preloaded = 0;
        size_t refs = 0;

        worldModel.forEachLoadedCellStore([&](MWWorld::CellStore& cell) {
            ++total;
            switch (cell.getState())
            {
                case MWWorld::CellStore::State_Loaded:
                    ++loaded;
                    refs += cell.count();
                    break;
                case MWWorld::CellStore::State_Preloaded:
                    ++preloaded;
                    break;
                default:
                    break;
            }
        });

        char buf[224];
        snprintf(buf, sizeof(buf),
            "[VitaAudit] worldmodel: %u cellstores (%u loaded, %u preloaded), %u live refs resident",
            (unsigned)total, (unsigned)loaded, (unsigned)preloaded, (unsigned)refs);
        auditLog(buf);

        // Evictability breakdown: tally why stores are retained.
        size_t evictable = 0;
        std::map<std::string, size_t> blocked;
        std::string firstDetail;
        worldModel.forEachLoadedCellStore([&](MWWorld::CellStore& cell) {
            std::string why;
            if (cell.isSafeToEvict(&why))
            {
                ++evictable;
                return;
            }
            const std::string key = why.substr(0, why.find(':'));
            if (++blocked[key] == 1 && why.size() > key.size() && firstDetail.empty())
                firstDetail = why;
        });
        std::string summary = "[VitaAudit] evictable=" + std::to_string(evictable);
        for (const auto& [reason, count] : blocked)
            summary += " " + reason + "=" + std::to_string(count);
        if (!firstDetail.empty())
            summary += " e.g. " + firstDetail;
        auditLog(summary.c_str());
    }

    void auditResourceCaches(Resource::ResourceSystem* resourceSystem)
    {
        if (!resourceSystem)
            return;

        size_t imageCount = 0;
        size_t imageBytes = 0;
        resourceSystem->getImageManager()->getObjectCache()->call([&](const auto&, osg::Object* obj) {
            if (const osg::Image* image = dynamic_cast<osg::Image*>(obj))
            {
                ++imageCount;
                imageBytes += image->getTotalSizeInBytesIncludingMipmaps();
            }
        });

        size_t nodeCount = 0;
        GeometryBytesVisitor geomBytes;
        resourceSystem->getSceneManager()->getObjectCache()->call([&](const auto&, osg::Object* obj) {
            if (osg::Node* node = dynamic_cast<osg::Node*>(obj))
            {
                ++nodeCount;
                node->accept(geomBytes);
            }
        });

        size_t keyframeCount = 0;
        resourceSystem->getKeyframeManager()->getObjectCache()->call(
            [&](const auto&, osg::Object*) { ++keyframeCount; });

        char buf[256];
        snprintf(buf, sizeof(buf),
            "[VitaAudit] caches: images=%u (%uKB), sceneTemplates=%u (geom %uKB), keyframes=%u",
            (unsigned)imageCount, (unsigned)(imageBytes / 1024), (unsigned)nodeCount,
            (unsigned)(geomBytes.mBytes / 1024), (unsigned)keyframeCount);
        auditLog(buf);
    }

    namespace
    {
        uint64_t s_renderUsAccum = 0;
        unsigned s_renderSamples = 0;

        // Engine frame numbers lag the viewer's; scan back.
        double msOf(const osg::Stats* stats, const char* name)
        {
            double v = 0.0;
            if (stats)
            {
                const unsigned int latest = stats->getLatestFrameNumber();
                const unsigned int earliest = stats->getEarliestFrameNumber();
                for (unsigned int f = latest;; --f)
                {
                    if (stats->getAttribute(f, name, v))
                        break;
                    if (f == earliest || f == 0)
                        break;
                }
            }
            return v * 1000.0;
        }

        double countOf(const osg::Stats* stats, const char* name)
        {
            double v = 0.0;
            if (stats)
                stats->getAttribute(stats->getLatestFrameNumber() > 0 ? stats->getLatestFrameNumber() - 1 : 0, name, v);
            return v;
        }
    }

    void auditFrameStats(osgViewer::Viewer& viewer)
    {
        constexpr int kReportEveryFrames = 150; // ~5s at 30fps
        static int s_frames = 0;
        static uint64_t s_lastReportUs = 0;
        static bool s_enabled = false;

        osg::Stats* viewerStats = viewer.getViewerStats();
        osg::Stats* camStats = viewer.getCamera() ? viewer.getCamera()->getStats() : nullptr;
        if (!s_enabled && viewerStats && camStats)
        {
            viewerStats->collectStats("engine", true);
            viewerStats->collectStats("update", true);
            camStats->collectStats("rendering", true);
            camStats->collectStats("scene", true);
            s_enabled = true;
        }
        if (!s_enabled)
            return;

        if (++s_frames < kReportEveryFrames)
            return;
        const uint64_t nowUs = sceKernelGetProcessTimeWide();
        const double frameMs
            = s_lastReportUs ? (nowUs - s_lastReportUs) / 1000.0 / static_cast<double>(s_frames) : 0.0;
        s_lastReportUs = nowUs;
        s_frames = 0;

        const double renderMs
            = s_renderSamples ? s_renderUsAccum / 1000.0 / static_cast<double>(s_renderSamples) : 0.0;
        s_renderUsAccum = 0;
        s_renderSamples = 0;

        const unsigned vramFreeMB = static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM) >> 20);
        const unsigned vramTotalMB = static_cast<unsigned>(vglMemTotal(VGL_MEM_VRAM) >> 20);

        char buf[256];
        snprintf(buf, sizeof(buf),
            "[Frame] avg=%.1fms render=%.1f (cull=%.1f draw=%.1f) update=%.1f wait=%.1f | mech=%.1f phys=%.1f "
            "world=%.1f gui=%.1f lua=%.1f script=%.1f input=%.1f sound=%.1f | vram=%u/%uMB free",
            frameMs, renderMs, msOf(camStats, "Cull traversal time taken"),
            msOf(camStats, "Draw traversal time taken"), msOf(viewerStats, "Update traversal time taken"),
            Vita::gWorkerWaitUs / 1000.0 / kReportEveryFrames,
            msOf(viewerStats, "mechanics_time_taken"), msOf(viewerStats, "physics_time_taken"),
            msOf(viewerStats, "world_time_taken"), msOf(viewerStats, "gui_time_taken"),
            msOf(viewerStats, "lua_time_taken"), msOf(viewerStats, "script_time_taken"),
            msOf(viewerStats, "input_time_taken"), msOf(viewerStats, "sound_time_taken"), vramFreeMB, vramTotalMB);
        auditLog(buf);
        Vita::gWorkerWaitUs = 0;
        snprintf(buf, sizeof(buf),
            "[Scene] drawables=%.0f fast=%.0f lights=%.0f bins=%.0f tris=%.0f strips=%.0f lightCb=%u/%.2fms "
            "glmemo=%u/%u",
            countOf(camStats, "Visible number of drawables"), countOf(camStats, "Visible number of fast drawables"),
            countOf(camStats, "Visible number of lights"), countOf(camStats, "Visible number of render bins"),
            countOf(camStats, "Visible number of GL_TRIANGLES"),
            countOf(camStats, "Visible number of GL_TRIANGLE_STRIP"),
            SceneUtil::gLightCbCalls / kReportEveryFrames,
            SceneUtil::gLightCbUs / 1000.0 / kReportEveryFrames,
            vgl_memo_hits / kReportEveryFrames, vgl_memo_miss / kReportEveryFrames);
        SceneUtil::gLightCbCalls = 0;
        SceneUtil::gLightCbUs = 0;
        vgl_memo_hits = vgl_memo_miss = 0;
        auditLog(buf);

        // Per-leaf OSG draw-dispatch cost (probes in RenderLeaf.cpp).
        if (osgprof_leaves > 0)
        {
            snprintf(buf, sizeof(buf), "[OsgProf] repl=%u rep=%u srep=%u leaves=%u mat=%.1f state=%.1f unif=%.1f draw=%.1f us/leaf",
                osgprof_replayable, osgprof_replayed, osgprof_streplayed,
                osgprof_leaves, (double)osgprof_mat_us / osgprof_leaves, (double)osgprof_state_us / osgprof_leaves,
                (double)osgprof_unif_us / osgprof_leaves, (double)osgprof_draw_us / osgprof_leaves);
            auditLog(buf);
        }
        osgprof_mat_us = osgprof_state_us = osgprof_unif_us = osgprof_draw_us = osgprof_leaves = 0;
        osgprof_replayable = osgprof_replayed = osgprof_streplayed = 0;

        // Cull traversal shape: visits per frame by node kind.
        if (cullprof_node + cullprof_group + cullprof_drawable + cullprof_sg > 0)
        {
            snprintf(buf, sizeof(buf),
                "[CullProf] node=%u grp=%u xf=%u bone=%u geode=%u drw=%u dcull=%u leaves=%u crep=%u sg=%u terr=%.2fms cdrop=%u /frame",
                cullprof_node / kReportEveryFrames, cullprof_group / kReportEveryFrames,
                cullprof_transform / kReportEveryFrames, cullprof_xf_bone / kReportEveryFrames,
                cullprof_geode / kReportEveryFrames, cullprof_drawable / kReportEveryFrames,
                cullprof_dcull / kReportEveryFrames, cullprof_leaves / kReportEveryFrames,
                cullprof_creplay / kReportEveryFrames, cullprof_sg / kReportEveryFrames,
                (double)cullprof_terr_us / 1000.0 / kReportEveryFrames, cullprof_crep_drop / kReportEveryFrames);
            auditLog(buf);
        }
        cullprof_node = cullprof_group = cullprof_transform = cullprof_geode = 0;
        cullprof_drawable = cullprof_dcull = cullprof_leaves = cullprof_sg = 0;
        cullprof_xf_bone = cullprof_creplay = cullprof_crep_drop = 0;
        cullprof_terr_us = 0;

        // Inside State::apply: which section eats the state bucket.
        if (osgapply_calls > 0)
        {
            snprintf(buf, sizeof(buf),
                "[OsgApply] calls=%u tex=%.1f mode=%.1f attr=%.1f unif=%.1f us/call push=%u pop=%u",
                osgapply_calls, (double)osgapply_tex_us / osgapply_calls, (double)osgapply_mode_us / osgapply_calls,
                (double)osgapply_attr_us / osgapply_calls, (double)osgapply_unif_us / osgapply_calls, osgapply_push,
                osgapply_pop);
            auditLog(buf);
        }
        osgapply_calls = osgapply_tex_us = osgapply_mode_us = osgapply_attr_us = osgapply_unif_us = 0;
        osgapply_push = osgapply_pop = 0;
    }

    void noteRenderTime(unsigned long long us)
    {
        s_renderUsAccum += us;
        ++s_renderSamples;
    }
}

#endif // __vita__

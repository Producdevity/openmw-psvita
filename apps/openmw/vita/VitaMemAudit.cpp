#ifdef __vita__

#include "VitaMemAudit.h"
#include "VitaInit.h"

#include <cstdio>
#include <cstring>

#include <psp2/kernel/clib.h>

#include <osg/Geometry>
#include <osg/Image>
#include <osg/NodeVisitor>

#include <components/esm3/loaddial.hpp>
#include <components/esm3/loadinfo.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadscpt.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

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
}

#endif // __vita__

#ifndef OPENMW_MWWORLD_CELLPRELOADER_H
#define OPENMW_MWWORLD_CELLPRELOADER_H

#include "positioncellgrid.hpp"

#include <components/sceneutil/workqueue.hpp>

#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <map>
#include <span>

namespace osg
{
    class Stats;
}

namespace Resource
{
    class ResourceSystem;
    class BulletShapeManager;
}

namespace Terrain
{
    class World;
    class View;
}

namespace MWRender
{
    class LandManager;
}

namespace Loading
{
    class Listener;
}

namespace MWWorld
{
    class CellStore;
    class TerrainPreloadItem;

    class CellPreloader
    {
    public:
        CellPreloader(Resource::ResourceSystem* resourceSystem, Resource::BulletShapeManager* bulletShapeManager,
            Terrain::World* terrain, MWRender::LandManager* landManager);
        ~CellPreloader();

        /// Ask a background thread to preload rendering meshes and collision shapes for objects in this cell.
        /// @note The cell itself must be in State_Loaded or State_Preloaded.
        void preload(MWWorld::CellStore& cell, double timestamp, bool urgent = false);

#ifdef __vita__
        void setPlayerContext(const osg::Vec3f& pos, const osg::Vec3f& forwardDir);
#endif

        void notifyLoaded(MWWorld::CellStore* cell);

        void clear();

        /// Removes preloaded cells that have not had a preload request for a while.
        void updateCache(double timestamp);

        /// How long to keep a preloaded cell in cache after it's no longer requested.
        void setExpiryDelay(double expiryDelay);

        /// The minimum number of preloaded cells before unused cells get thrown out.
        void setMinCacheSize(std::size_t value) { mMinCacheSize = value; }

        /// The maximum number of preloaded cells.
        void setMaxCacheSize(std::size_t value) { mMaxCacheSize = value; }

        /// Enables the creation of instances in the preloading thread.
        void setPreloadInstances(bool preload);

        std::size_t getMaxCacheSize() const { return mMaxCacheSize; }

        std::size_t getCacheSize() const { return mPreloadCells.size(); }

#ifdef __vita__
        bool isCellPreloaded(const MWWorld::CellStore& cell) const
        {
            auto pinned = mVitaPinnedPreloads.find(&cell);
            if (pinned != mVitaPinnedPreloads.end())
                return pinned->second.mWorkItem && pinned->second.mWorkItem->isDone();
            auto it = mPreloadCells.find(&cell);
            return it != mPreloadCells.end() && it->second.mWorkItem && it->second.mWorkItem->isDone();
        }
        // Border warming: separate slots, immune to LRU eviction.
        void vitaPreloadPinned(MWWorld::CellStore& cell, double timestamp);
        void vitaReleasePinned(const MWWorld::CellStore* cell);
        void vitaReleaseAllPinned();
#endif

        void setWorkQueue(osg::ref_ptr<SceneUtil::WorkQueue> workQueue);

        void setTerrainPreloadPositions(std::span<const PositionCellGrid> positions);

        void syncTerrainLoad(Loading::Listener& listener);
        void abortTerrainPreloadExcept(const PositionCellGrid* exceptPos);
        bool isTerrainLoaded(const PositionCellGrid& position, double referenceTime) const;
        void setTerrain(Terrain::World* terrain);

        void reportStats(unsigned int frameNumber, osg::Stats& stats) const;

    private:
        void clearAllTasks();

        Resource::ResourceSystem* mResourceSystem;
        Resource::BulletShapeManager* mBulletShapeManager;
        Terrain::World* mTerrain;
        MWRender::LandManager* mLandManager;
        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        double mExpiryDelay;
        std::size_t mMinCacheSize = 0;
        std::size_t mMaxCacheSize = 0;
        bool mPreloadInstances;

        double mLastResourceCacheUpdate;

        struct PreloadEntry
        {
            PreloadEntry(double timestamp, osg::ref_ptr<SceneUtil::WorkItem> workItem)
                : mTimeStamp(timestamp)
                , mWorkItem(std::move(workItem))
            {
            }
            PreloadEntry()
                : mTimeStamp(0.0)
            {
            }

            double mTimeStamp;
            osg::ref_ptr<SceneUtil::WorkItem> mWorkItem;
        };
        typedef std::map<const MWWorld::CellStore*, PreloadEntry> PreloadMap;

        // Cells that are currently being preloaded, or have already finished preloading
        PreloadMap mPreloadCells;
#ifdef __vita__
        PreloadMap mVitaPinnedPreloads;
#endif

        std::vector<osg::ref_ptr<Terrain::View>> mTerrainViews;
        std::vector<PositionCellGrid> mTerrainPreloadPositions;
        osg::ref_ptr<TerrainPreloadItem> mTerrainPreloadItem;
        osg::ref_ptr<SceneUtil::WorkItem> mUpdateCacheItem;

        std::vector<PositionCellGrid> mLoadedTerrainPositions;
        double mLoadedTerrainTimestamp;
        std::size_t mEvicted = 0;
        std::size_t mAdded = 0;
        std::size_t mExpired = 0;
        std::size_t mLoaded = 0;

#ifdef __vita__
        osg::Vec3f mPlayerPos;
        osg::Vec3f mForwardDir;
        bool mHasPlayerContext = false;

        float getCellDistanceSq(const MWWorld::CellStore* cell) const;
        bool isRearHemisphere(const MWWorld::CellStore* cell) const;
#endif
    };

}

#endif

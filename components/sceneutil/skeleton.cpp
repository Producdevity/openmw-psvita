#include "skeleton.hpp"

#include <osg/MatrixTransform>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/lower.hpp>
#ifdef __vita__
#include <components/sceneutil/lightmanager.hpp>
#endif

#include <algorithm>

#ifdef __vita__
extern "C" int cullprof_in_skeleton;

namespace
{
    // Mask drawable-free bone branches out of cull (bit mirrors
    // MWRender::Mask_BoneOnly; cull cameras exclude it, update visitors
    // and the skeleton's own bone update are unaffected). Chains leading
    // to attachments (weapons, torches, VFX) stay fully visible.
    constexpr unsigned int kBoneOnlyMask = 1u << 23;

    bool vitaMarkBoneSubtree(osg::Node* node)
    {
        if (node->asDrawable())
            return true;
        if (dynamic_cast<SceneUtil::LightSource*>(node))
            return true;
        bool renderable = false;
        osg::Group* group = node->asGroup();
        if (group)
        {
            for (unsigned int i = 0; i < group->getNumChildren(); ++i)
                if (vitaMarkBoneSubtree(group->getChild(i)))
                    renderable = true;
        }
        else
            renderable = true; // unknown leaf type: keep visible
        if (node->asTransform())
            node->setNodeMask(renderable ? ~0u : kBoneOnlyMask);
        return renderable;
    }
}
#endif

namespace SceneUtil
{

    class InitBoneCacheVisitor : public osg::NodeVisitor
    {
    public:
        typedef std::vector<osg::MatrixTransform*> TransformPath;
        InitBoneCacheVisitor(std::unordered_map<std::string, TransformPath>& cache)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mCache(cache)
        {
        }

        void apply(osg::MatrixTransform& node) override
        {
            mPath.push_back(&node);
            mCache.emplace(Misc::StringUtils::lowerCase(node.getName()), mPath);
            traverse(node);
            mPath.pop_back();
        }

    private:
        TransformPath mPath;
        std::unordered_map<std::string, TransformPath>& mCache;
    };

    Skeleton::Skeleton()
        : mBoneCacheInit(false)
        , mNeedToUpdateBoneMatrices(true)
        , mActive(Active)
        , mLastFrameNumber(0)
        , mLastCullFrameNumber(0)
    {
    }

    Skeleton::Skeleton(const Skeleton& copy, const osg::CopyOp& copyop)
        : osg::Group(copy, copyop)
        , mBoneCacheInit(false)
        , mNeedToUpdateBoneMatrices(true)
        , mActive(copy.mActive)
        , mLastFrameNumber(0)
        , mLastCullFrameNumber(0)
    {
    }

    Bone* Skeleton::getBone(const std::string& name)
    {
        if (!mBoneCacheInit)
        {
            InitBoneCacheVisitor visitor(mBoneCache);
            accept(visitor);
            mBoneCacheInit = true;
        }

        BoneCache::iterator found = mBoneCache.find(Misc::StringUtils::lowerCase(name));
        if (found == mBoneCache.end())
            return nullptr;

        // find or insert in the bone hierarchy

        if (!mRootBone.get())
        {
            mRootBone = std::make_unique<Bone>();
        }

        Bone* bone = mRootBone.get();
        for (osg::MatrixTransform* matrixTransform : found->second)
        {
            const auto it = std::find_if(bone->mChildren.begin(), bone->mChildren.end(),
                [&](const auto& v) { return v->mNode == matrixTransform; });

            if (it == bone->mChildren.end())
            {
                bone = bone->mChildren.emplace_back(std::make_unique<Bone>()).get();
                mNeedToUpdateBoneMatrices = true;
            }
            else
                bone = it->get();

            bone->mNode = matrixTransform;
        }

        return bone;
    }

    void Skeleton::updateBoneMatrices(unsigned int traversalNumber)
    {
        if (traversalNumber != mLastFrameNumber)
            mNeedToUpdateBoneMatrices = true;

        mLastFrameNumber = traversalNumber;

        if (mNeedToUpdateBoneMatrices)
        {
            if (mRootBone.get())
            {
                for (const auto& child : mRootBone->mChildren)
                    child->update(nullptr);
            }

            mNeedToUpdateBoneMatrices = false;
        }
    }

    void Skeleton::setActive(ActiveType active)
    {
        mActive = active;
    }

    bool Skeleton::getActive() const
    {
        return mActive != Inactive;
    }

    void Skeleton::markDirty()
    {
        mLastFrameNumber = 0;
        mBoneCache.clear();
        mBoneCacheInit = false;
    }

    void Skeleton::traverse(osg::NodeVisitor& nv)
    {
        if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
        {
            if (mActive == Inactive && mLastFrameNumber != 0)
                return;
            if (mActive == SemiActive && mLastFrameNumber != 0 && mLastCullFrameNumber + 3 <= nv.getTraversalNumber())
                return;
        }
        else if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
        {
            mLastCullFrameNumber = nv.getTraversalNumber();
#ifdef __vita__
            // Refresh bone cull masks ~4x/s: attachments (equip, VFX,
            // torches) appear within a beat; skipped branches drop out
            // of the cull walk entirely.
            if (mVitaMaskFrame == 0 || mLastCullFrameNumber - mVitaMaskFrame >= 8)
            {
                mVitaMaskFrame = mLastCullFrameNumber;
                for (unsigned int i = 0; i < getNumChildren(); ++i)
                    vitaMarkBoneSubtree(getChild(i));
            }
            // Census: how much of the cull transform count is bones.
            ++cullprof_in_skeleton;
            osg::Group::traverse(nv);
            --cullprof_in_skeleton;
            return;
#endif
        }

        osg::Group::traverse(nv);
    }

    void Skeleton::childInserted(unsigned int)
    {
        markDirty();
    }

    void Skeleton::childRemoved(unsigned int, unsigned int)
    {
        markDirty();
    }

    Bone::Bone()
        : mNode(nullptr)
    {
    }

    void Bone::update(const osg::Matrixf* parentMatrixInSkeletonSpace)
    {
        if (!mNode)
        {
            Log(Debug::Error) << "Error: Bone without node";
            return;
        }
        if (parentMatrixInSkeletonSpace)
            mMatrixInSkeletonSpace = mNode->getMatrix() * (*parentMatrixInSkeletonSpace);
        else
            mMatrixInSkeletonSpace = mNode->getMatrix();

        for (const auto& child : mChildren)
            child->update(&mMatrixInSkeletonSpace);
    }

}

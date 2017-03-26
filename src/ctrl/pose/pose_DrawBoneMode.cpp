#include <cmath>
#include "util/CollDetect.h"
#include "util/MathUtil.h"
#include "cmnd/ScopedMacro.h"
#include "core/Constant.h"
#include "ctrl/TimeLineUtil.h"
#include "ctrl/CmndName.h"
#include "ctrl/pose/pose_DrawBoneMode.h"
#include "ctrl/bone/bone_Renderer.h"

#include "util/TreeUtil.h"

using namespace core;

namespace ctrl {
namespace pose {

RigidBone::RigidBone(const core::Bone2& aOrigin)
    : TreeNodeBase(this)
    , ptr(&aOrigin)
    , rootPos()
    , angle()
    , length()
    , force()
    , torque()
{
    rootPos = aOrigin.parent() ? aOrigin.parent()->worldPos() : aOrigin.worldPos();
    length = (aOrigin.worldPos() - rootPos).length();
    angle = aOrigin.worldAngle();
}

QVector2D RigidBone::tailPos() const
{
    return rootPos + util::MathUtil::getVectorFromPolarCoord(length, angle);
}

QVector2D RigidBone::dir() const
{
    return util::MathUtil::getVectorFromPolarCoord(length, angle);
}

void RigidBone::updateMotion(int aCentroid)
{
    rootPos += force;
    if (aCentroid == -1)
    {
        angle += torque;
    }
    else if (aCentroid == 0)
    {
        auto rotate = torque;
        auto center = rootPos + 0.5f * dir();
        rootPos = center + util::MathUtil::getRotateVectorRad(rootPos - center, rotate);
        angle += rotate;
    }
    else if (aCentroid == 1)
    {
        auto rotate = torque;
        auto center = tailPos();
        rootPos = center + util::MathUtil::getRotateVectorRad(rootPos - center, rotate);
        angle += rotate;
    }

    force = QVector2D();
    torque = 0.0f;
}

BoneDynamics::BoneDynamics(const core::Bone2& aTopBone)
    : mTopBone(aTopBone)
    , mRigidTopBone()
    , mConduction(0.1f)
{
    mRigidTopBone = util::TreeUtil::createShadow<Bone2, RigidBone>(&mTopBone);
}

BoneDynamics::~BoneDynamics()
{
    deleteAll();
}

QVector<float> BoneDynamics::rotationDifferences() const
{
    using util::MathUtil;
    QVector<float> differences;

    RigidBone::ConstIterator itr(mRigidTopBone);
    while (itr.hasNext())
    {
        auto rigidBone = itr.next();
        float diff = 0.0f;
        if (rigidBone->parent())
        {
            auto rotate = rigidBone->angle - rigidBone->parent()->angle - rigidBone->ptr->localAngle();
            diff = MathUtil::getAngleDifferenceRad(
                        MathUtil::normalizeAngleRad(rigidBone->ptr->rotate()),
                        MathUtil::normalizeAngleRad(rotate));
        }
        differences.push_back(diff);
    }
    return differences;
}

void BoneDynamics::pullBone(RigidBone& aTarget, const QVector2D& aPull, float aPullPos)
{
    auto preRoot = aTarget.rootPos;
    auto preTail = aTarget.tailPos();
    if (aTarget.length >= core::Constant::normalizable())
    {
#if 1
        auto rotateRateLinear = std::abs(2.0f * (aPullPos - 0.5f));
        auto rotateRate = 1.0 - (1.0f - rotateRateLinear) * (1.0f - rotateRateLinear);
        rotateRate = mConduction * rotateRate + (1.0f - mConduction);

        auto normDir = aTarget.dir().normalized();
        auto vertical = normDir * QVector2D::dotProduct(normDir, aPull);
        auto horizontal = aPull - vertical;
        aTarget.force = mConduction * (vertical + (1.0f - rotateRate) * horizontal);

        auto rotate = horizontal * rotateRate;
        auto torque = (rotate.length() / aTarget.length) *
                (aPullPos >= 0.5f ? 1.0f : -1.0f) *
                (util::CollDetect::getCross(normDir, rotate) > 0.0f ? 1.0f : -1.0f);
        aTarget.torque = torque;
        aTarget.updateMotion(aPullPos >= 0.5f ? -1 : 1);
#else
        auto rotateRate = aPullPos;
        auto normDir = aTarget.dir().normalized();
        auto vertical = normDir * QVector2D::dotProduct(normDir, aPull);
        auto horizontal = aPull - vertical;
        aTarget.force = (vertical + (1.0f - rotateRate) * horizontal);

        auto rotate = horizontal * rotateRate;
        auto torque = (rotate.length() / aTarget.length) *
                (util::CollDetect::getCross(normDir, rotate) > 0.0f ? 1.0f : -1.0f);
        aTarget.torque = torque;
        aTarget.updateMotion(-1);
#endif
    }
    else
    {
        aTarget.force = aPull;
        aTarget.updateMotion();
    }
    // update parents
    pullParentBones(aTarget, aTarget.rootPos - preRoot);
    adjustByOriginConstraint(aTarget);

    // update children
    pullChildBonesRecursive(aTarget, aTarget.tailPos() - preTail);

    // adjustment
    for (int i = 0; i < 3; ++i)
    {
        adjustParentBones(aTarget);
        adjustChildBonesRecursive(aTarget);
    }
}

QVector2D BoneDynamics::adjustByOriginConstraint(RigidBone& aTarget)
{
    QVector2D pull;
    if (aTarget.parent())
    {
        pull = adjustByOriginConstraint(*aTarget.parent());
    }
    else
    {
        auto originPos = mTopBone.worldPos();
        pull = originPos - aTarget.rootPos;
    }

    if (aTarget.length >= core::Constant::normalizable())
    {
        auto normDir = aTarget.dir().normalized();
        auto trans = normDir * QVector2D::dotProduct(normDir, pull);
        auto rotate = pull - trans;
        auto torque = (rotate.length() / aTarget.length) *
                (util::CollDetect::getCross(normDir, rotate) < 0.0f ? 1.0f : -1.0f);
        aTarget.force = trans;
        aTarget.torque = torque;
        aTarget.updateMotion(1);
        pull = trans;
    }
    else
    {
        aTarget.force = pull;
        aTarget.updateMotion(1);
    }
    return pull;
}

void BoneDynamics::pullParentBones(RigidBone& aTarget, const QVector2D& aPull)
{
    auto pull = aPull;
    for (auto parent = aTarget.parent(); parent; parent = parent->parent())
    {
        if (parent->length >= core::Constant::normalizable())
        {
            auto normDir = parent->dir().normalized();
            auto trans = normDir * QVector2D::dotProduct(normDir, pull);
            auto rotate = pull - trans;
            auto torque = (rotate.length() / parent->length) *
                    (util::CollDetect::getCross(normDir, rotate) > 0.0f ? 1.0f : -1.0f);
            parent->force = mConduction * trans;
            parent->torque = torque;
            parent->updateMotion(-1);
            pull = mConduction * trans;
        }
        else
        {
            parent->force = mConduction * pull;
            parent->updateMotion(-1);
        }
    }
}

void BoneDynamics::pullChildBonesRecursive(RigidBone& aTarget, const QVector2D& aPull)
{
    for (auto child : aTarget.children())
    {
        auto trans = aPull;
        if (child->length >= core::Constant::normalizable())
        {
            auto normDir = child->dir().normalized();
            trans = normDir * QVector2D::dotProduct(normDir, aPull);
            auto rotate = aPull - trans;
            auto torque = (rotate.length() / child->length) *
                    (util::CollDetect::getCross(normDir, rotate) < 0.0f ? 1.0f : -1.0f);
            child->force = /*mConduction * */trans;
            child->torque = torque;
            child->updateMotion(1);
        }
        else
        {
            child->force = /*mConduction * */trans;
            child->updateMotion(1);
        }
        pullChildBonesRecursive(*child, /*mConduction * */trans);
    }
}

void BoneDynamics::adjustParentBones(RigidBone& aTarget)
{
    auto prev = &aTarget;
    for (auto parent = aTarget.parent(); parent; parent = parent->parent())
    {
        auto pull = prev->rootPos - parent->tailPos();
        if (parent->length >= core::Constant::normalizable())
        {
            auto normDir = parent->dir().normalized();
            auto trans = normDir * QVector2D::dotProduct(normDir, pull);
            auto rotate = pull - trans;
            auto torque = (rotate.length() / parent->length) *
                    (util::CollDetect::getCross(normDir, rotate) > 0.0f ? 1.0f : -1.0f);
            parent->force = trans;
            parent->torque = torque;
        }
        else
        {
            parent->force = pull;
        }
        parent->updateMotion(-1);
        prev = parent;
    }
}

void BoneDynamics::adjustChildBonesRecursive(RigidBone& aTarget)
{
    for (auto child : aTarget.children())
    {
        auto pull = aTarget.tailPos() - child->rootPos;
        if (child->length >= core::Constant::normalizable())
        {
            auto normDir = child->dir().normalized();
            auto trans = normDir * QVector2D::dotProduct(normDir, pull);
            auto rotate = pull - trans;
            auto torque = (rotate.length() / child->length) *
                    (util::CollDetect::getCross(normDir, rotate) < 0.0f ? 1.0f : -1.0f);
            child->force = trans;
            child->torque = torque;
        }
        else
        {
            child->force = pull;
        }
        child->updateMotion(1);
        adjustChildBonesRecursive(*child);
    }
}

void BoneDynamics::deleteAll()
{
    if (mRigidTopBone)
    {
        util::TreeUtil::deleteAll(mRigidTopBone);
        mRigidTopBone = nullptr;
    }
}

void BoneDynamics::updateMotions()
{
    RigidBone::Iterator itr(mRigidTopBone);
    while (itr.hasNext())
    {
        itr.next()->updateMotion();
    }
}

void BoneDynamics::reconnectBones()
{
    auto originPos = mRigidTopBone->tailPos();
    for (auto child : mRigidTopBone->children())
    {
        reconnectBonesRecursive(*child, originPos - child->rootPos);
    }
}

void BoneDynamics::reconnectBonesRecursive(
        RigidBone& aCurrent, const QVector2D& aRootPull)
{
    auto dir = aCurrent.dir();
    auto tailPos = aCurrent.rootPos + dir;
    auto halfLength = 0.5f * aCurrent.length;
    auto count = 1 + aCurrent.children().size();

    QVector2D connectPos = tailPos / count;
    for (auto child : aCurrent.children())
    {
        connectPos += child->rootPos / count;
    }

    auto tailPull = connectPos - tailPos;

    if (aCurrent.length >= core::Constant::normalizable())
    {
        auto normDir = dir.normalized();
        auto rootTrans = normDir * QVector2D::dotProduct(normDir, aRootPull);
        auto rootRotate = aRootPull - rootTrans;
        auto tailTrans = normDir * QVector2D::dotProduct(normDir, tailPull);
        auto tailRotate = tailPull - tailTrans;
        auto rootTorque = (rootRotate.length() / halfLength) *
                (util::CollDetect::getCross(normDir, rootRotate) < 0.0f ? 1.0f : -1.0f);
        auto tailTorque = (tailRotate.length() / halfLength) *
                (util::CollDetect::getCross(normDir, tailRotate) > 0.0f ? 1.0f : -1.0f);
        aCurrent.force = rootTrans + tailTrans;
        aCurrent.torque = rootTorque + tailTorque;
    }
    else
    {
        aCurrent.force = aRootPull + tailPull;
        aCurrent.torque = 0.0f;
    }

    for (auto child : aCurrent.children())
    {
        reconnectBonesRecursive(*child, connectPos - child->rootPos);
    }
}

DrawBoneMode::DrawBoneMode(Project& aProject, const Target& aTarget, KeyOwner& aKey)
    : mProject(aProject)
    , mTarget(*aTarget.node)
    , mTargetMtx(aTarget.mtx)
    , mTargetInvMtx(aTarget.invMtx)
    , mKeyOwner(aKey)
    , mFocuser()
    , mCommandRef()
    , mPullPos()
    , mPullOffset()
    , mPullPosRate()
{
    XC_PTR_ASSERT(mKeyOwner.key);
    mFocuser.setTopBones(mKeyOwner.key->data().topBones());
    mFocuser.setFocusConnector(true);
    mFocuser.setTargetMatrix(mTargetMtx);
}

bool DrawBoneMode::updateCursor(const CameraInfo& aCamera, const AbstractCursor& aCursor)
{
    auto focus = mFocuser.update(aCamera, aCursor.screenPos());
    bool updated = mFocuser.focusChanged();

    if (aCursor.emitsLeftPressedEvent())
    {
        mCommandRef = nullptr;
        mFocuser.clearSelection();
        if (focus && focus->parent())
        {
            mFocuser.select(*focus);
            const QVector2D center = focus->parent()->worldPos();
            const QVector2D tail = focus->worldPos();
            const util::Segment2D seg(center, tail - center);

            const QVector2D cursorPos =
                    (mTargetInvMtx * QVector3D(aCursor.worldPos())).toVector2D();
            mPullPos = util::CollDetect::getPosOnLine(seg, cursorPos);
            mPullOffset = cursorPos - mPullPos;
            mPullPosRate = (mPullPos - seg.start).length() / seg.dir.length();
        }
        updated = true;
    }
    else if (aCursor.emitsLeftDraggedEvent())
    {
        Bone2* selected = mFocuser.selectingBone();

        if (selected && selected->parent())
        {
            const QVector2D cursorPos =
                    (mTargetInvMtx * QVector3D(aCursor.worldPos())).toVector2D();

            auto nextPos = (cursorPos - mPullOffset);
            auto pull = nextPos - mPullPos;
            mPullPos = nextPos;
            pullBone(*selected, pull, mPullPosRate);
        }
        updated = true;
    }
    else if (aCursor.emitsLeftReleasedEvent())
    {
        mCommandRef = nullptr;
        mFocuser.clearSelection();
        updated = true;
    }

    return updated;
}

void DrawBoneMode::renderQt(const RenderInfo& aInfo, QPainter& aPainter)
{
    bone::Renderer renderer(aPainter, aInfo);
    renderer.setAntialiasing(true);
    renderer.setFocusConnector(true);
    renderer.setTargetMatrix(mTargetMtx);

    for (auto bone : mKeyOwner.key->data().topBones())
    {
        renderer.renderBones(bone);
    }
}

void DrawBoneMode::pullBone(Bone2& aTarget, const QVector2D& aPull, float aPullPosRate)
{
    auto& targetRoot = util::TreeUtil::getTreeRoot<Bone2>(aTarget);
    BoneDynamics dynamics(targetRoot);
    {
        RigidBone* rigidTarget = nullptr;
        RigidBone::Iterator itr(&dynamics.rigidTopBone());
        while (itr.hasNext())
        {
            auto rigidBone = itr.next();
            if (rigidBone->ptr == &aTarget)
            {
                rigidTarget = rigidBone;
                break;
            }
        }
        if (rigidTarget)
        {
            for (int i = 0; i < 16; ++i)
            {
                dynamics.pullBone(*rigidTarget, aPull / 16.0f, aPullPosRate);
            }
        }
    }

    // get next rotation values
    QVector<float> nextRots;
    {
        auto diffs = dynamics.rotationDifferences();
        Bone2::ConstIterator itr(&targetRoot);

        for (auto diff : diffs)
        {
            if (!itr.hasNext()) break;
            nextRots.push_back(itr.next()->rotate() + diff);
        }
    }

    // create a command
    {
        cmnd::Stack& stack = mProject.commandStack();
        TimeLine& timeLine = *mTarget.timeLine();
        const int frame = mProject.animator().currentFrame().get();

        // modify
        if (mCommandRef && stack.isModifiable(mCommandRef))
        {
            mCommandRef->modifyValue(nextRots);

            // notify
            TimeLineEvent event;
            event.setType(TimeLineEvent::Type_ChangeKeyValue);
            event.pushTarget(mTarget, TimeKeyType_Pose, frame);
            mProject.onTimeLineModified(event, false);
        }
        else
        {
            cmnd::ScopedMacro macro(stack, CmndName::tr("pull bones of a posing key"));

            // set notifier
            {
                auto notifier = new TimeLineUtil::Notifier(mProject);
                notifier->event().setType(
                            mKeyOwner.owns() ?
                                TimeLineEvent::Type_PushKey :
                                TimeLineEvent::Type_ChangeKeyValue);
                notifier->event().pushTarget(mTarget, TimeKeyType_Pose, frame);
                macro.grabListener(notifier);
            }
            // push key command
            if (mKeyOwner.owns())
            {
                mKeyOwner.pushOwnsKey(stack, timeLine, frame);
            }

            // push command
            mCommandRef = new RotateBones(&targetRoot, nextRots);
            stack.push(mCommandRef);
        }
    }
}

} // namespace pose
} // namespace ctrl

#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_CAMERA_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_CAMERA_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct CameraCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<float> mNearClip{ mIndex, "Camera", "near clip", makeMaxSanitizerFloat(0.005f) };
        SettingValue<bool> mSmallFeatureCulling{ mIndex, "Camera", "small feature culling" };
        SettingValue<float> mSmallFeatureCullingPixelSize{ mIndex, "Camera", "small feature culling pixel size",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<float> mViewingDistance{ mIndex, "Camera", "viewing distance", makeMaxStrictSanitizerFloat(0) };
        SettingValue<float> mFieldOfView{ mIndex, "Camera", "field of view", makeClampSanitizerFloat(1, 179) };
        SettingValue<float> mFirstPersonFieldOfView{ mIndex, "Camera", "first person field of view",
            makeClampSanitizerFloat(1, 179) };
        SettingValue<bool> mReverseZ{ mIndex, "Camera", "reverse z" };
        SettingValue<bool> mVitaDynamicFog{ mIndex, "Camera", "vita dynamic fog" };
        SettingValue<float> mVitaDynFogTargetFps{ mIndex, "Camera", "vita dyn fog target fps",
            makeClampSanitizerFloat(15.f, 30.f) };
        // Fog stops expanding here even when fps allows; spare goes to smoothness.
        SettingValue<float> mVitaDynFogMaxDistance{ mIndex, "Camera", "vita dyn fog max distance",
            makeClampSanitizerFloat(1500.f, 5000.f) };
        // controls how hard the proportional shrink response pulls fog in when fps is below target.
        SettingValue<std::string> mVitaDynFogAggression{ mIndex, "Camera", "vita dyn fog aggression" };
    };
}

#endif

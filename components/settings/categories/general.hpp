#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_GENERAL_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_GENERAL_H

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
    struct GeneralCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<int> mAnisotropy{ mIndex, "General", "anisotropy", makeClampSanitizerInt(0, 16) };
        SettingValue<std::string> mScreenshotFormat{ mIndex, "General", "screenshot format",
            makeEnumSanitizerString({ "jpg", "png", "tga" }) };
        SettingValue<std::string> mTextureMagFilter{ mIndex, "General", "texture mag filter",
            makeEnumSanitizerString({ "nearest", "linear" }) };
        SettingValue<std::string> mTextureMinFilter{ mIndex, "General", "texture min filter",
            makeEnumSanitizerString({ "nearest", "linear" }) };
        SettingValue<std::string> mTextureMipmap{ mIndex, "General", "texture mipmap",
            makeEnumSanitizerString({ "none", "nearest", "linear" }) };
        // Vita-only: caps the maximum edge length to which loose textures
        // get downsampled at load time inside imagemanager.cpp. See the
        // ImageManager::getImage downsample block for the exact caps tied to
        // each preset. "off" disables the cap entirely; "performance" matches
        // the values the Vita port shipped with before the setting existed.
        SettingValue<std::string> mVitaTextureDetail{ mIndex, "General", "vita texture detail",
            makeEnumSanitizerString({ "performance", "balanced", "high", "off" }) };
        // Vita: keep DXT compressed on GPU; false restores decompress path.
        SettingValue<bool> mVitaKeepCompressedTextures{ mIndex, "General", "vita keep compressed textures", true };
        // Vita: VBOs for static geometry; false restores client arrays.
        SettingValue<bool> mVitaStaticGeometryVbo{ mIndex, "General", "vita static geometry vbo", true };
        SettingValue<bool> mVitaCellMerge{ mIndex, "General", "vita cell merge", false };
        SettingValue<bool> mVitaCellFlatten{ mIndex, "General", "vita cell flatten", true };
        SettingValue<bool> mVitaCullOverlap{ mIndex, "General", "vita cull overlap", false };
        SettingValue<bool> mVitaDrawReplay{ mIndex, "General", "vita draw replay", false };
        SettingValue<bool> mVitaStateReplay{ mIndex, "General", "vita state replay", false };
        // Vita: static VBOs in RAM, sparing CDRAM for textures.
        SettingValue<bool> mVitaVboInRam{ mIndex, "General", "vita vbo in ram", false };
        SettingValue<bool> mVitaSyncCellDrain{ mIndex, "General", "vita sync cell drain", true };
        // Vita: heap MB held back from the watchdog budget. Raise to test eviction.
        SettingValue<int> mVitaMemoryReserveMb{ mIndex, "General", "vita memory reserve mb", 40,
            makeClampSanitizerInt(20, 150) };
        // Vita: full cache clear at grid change only above this heap usage.
        SettingValue<int> mVitaFlushThresholdMb{ mIndex, "General", "vita flush threshold mb", 225,
            makeClampSanitizerInt(0, 272) };
        // Vita: sort render bins by state (fewer GL state changes per draw).
        SettingValue<bool> mVitaStateSortedBins{ mIndex, "General", "vita state sorted bins", false };
        // Vita: run scripts/mechanics/physics on a worker thread.
        SettingValue<bool> mVitaSimThread{ mIndex, "General", "vita sim thread", true };
        // Vita: overlap next frame's sim with draw (requires sim thread).
        SettingValue<bool> mVitaSimOverlap{ mIndex, "General", "vita sim overlap", true };
        // Vita: dialogue text stays on disk until first use.
        SettingValue<bool> mVitaLazyDialogue{ mIndex, "General", "vita lazy dialogue", true };
        SettingValue<bool> mNotifyOnSavedScreenshot{ mIndex, "General", "notify on saved screenshot" };
        SettingValue<std::vector<std::string>> mPreferredLocales{ mIndex, "General", "preferred locales" };
        SettingValue<bool> mGmstOverridesL10n{ mIndex, "General", "gmst overrides l10n" };
        SettingValue<std::size_t> mLogBufferSize{ mIndex, "General", "log buffer size" };
        SettingValue<std::size_t> mConsoleHistoryBufferSize{ mIndex, "General", "console history buffer size" };
    };
}

#endif

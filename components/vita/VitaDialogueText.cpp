#include "VitaDialogueText.h"

#ifdef __vita__

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <components/esm3/loadinfo.hpp>

extern "C" void vitaBreadcrumb(const char* msg); // apps/openmw/vita/VitaInit.cpp

namespace Vita::DialogueText
{
    namespace
    {
        // ~800KB worst case; a session touches a small fraction of infos.
        constexpr size_t kMaxCacheEntries = 2048;

        bool sEnabled = false;
        std::unique_ptr<ToUTF8::Utf8Encoder> sEncoder;
        std::map<int, std::string> sPaths;
        std::map<int, std::FILE*> sFiles;
        std::unordered_map<const ESM::DialInfo*, std::string> sCache;
        std::mutex sMutex;
        const std::string sEmpty;

        const std::string& fail(const char* what, const ESM::DialInfo& info)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[LazyDial] %s (file=%d off=%u)", what, (int)info.mResponseFile,
                (unsigned)info.mResponseOffset);
            vitaBreadcrumb(buf);
            return sEmpty;
        }
    }

    void setEnabled(bool enabled)
    {
        sEnabled = enabled;
    }

    bool enabled()
    {
        return sEnabled;
    }

    void setEncoding(ToUTF8::FromType encoding)
    {
        sEncoder = std::make_unique<ToUTF8::Utf8Encoder>(encoding);
    }

    void registerContentFile(int index, const std::filesystem::path& path)
    {
        const std::lock_guard lock(sMutex);
        sPaths[index] = path.string();
    }

    const std::string& fetch(const ESM::DialInfo& info)
    {
        if (info.mResponseFile < 0)
            return info.mResponse; // eager-loaded
        if (info.mResponseSize == 0)
            return sEmpty;

        const std::lock_guard lock(sMutex);

        auto it = sCache.find(&info);
        if (it != sCache.end())
            return it->second;

        std::FILE*& file = sFiles[info.mResponseFile];
        if (!file)
        {
            auto path = sPaths.find(info.mResponseFile);
            if (path == sPaths.end())
                return fail("unknown content file", info);
            file = std::fopen(path->second.c_str(), "rb");
            if (!file)
                return fail("open failed", info);
        }

        std::vector<char> raw(info.mResponseSize);
        if (std::fseek(file, static_cast<long>(info.mResponseOffset), SEEK_SET) != 0
            || std::fread(raw.data(), 1, raw.size(), file) != raw.size())
            return fail("read failed", info);

        // Mirror ESMReader::getStringView: trim at NUL, then convert.
        const size_t len = strnlen(raw.data(), raw.size());
        std::string text;
        if (sEncoder)
            text = std::string(sEncoder->getUtf8(std::string_view(raw.data(), len)));
        else
            text.assign(raw.data(), len);

        if (sCache.size() >= kMaxCacheEntries)
            sCache.clear();
        return sCache.emplace(&info, std::move(text)).first->second;
    }
}

#endif // __vita__

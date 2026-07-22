#ifndef COMPONENTS_VITA_DIALOGUETEXT_H
#define COMPONENTS_VITA_DIALOGUETEXT_H

#ifdef __vita__

#include <filesystem>
#include <string>

#include <components/toutf8/toutf8.hpp>

namespace ESM
{
    struct DialInfo;
}

/// Lazy dialogue response text: INFO text stays on disk at load,
/// fetched (and session-cached) on first use.
namespace Vita::DialogueText
{
    /// Master switch; set before content load.
    void setEnabled(bool enabled);
    bool enabled();

    void setEncoding(ToUTF8::FromType encoding);
    void registerContentFile(int index, const std::filesystem::path& path);

    /// Response text; ref valid until the session cache clears.
    const std::string& fetch(const ESM::DialInfo& info);
}

#endif // __vita__
#endif

#ifndef COMPONENTS_VITA_ESMPREFETCH_H
#define COMPONENTS_VITA_ESMPREFETCH_H

#ifdef __vita__

#include <filesystem>
#include <istream>
#include <memory>
#include <vector>

/// ESM read/parse pipelining: a background thread reads content files
/// into RAM while the main thread parses behind a watermark.
namespace Vita::EsmPrefetch
{
    /// Start reading \a files (load order) on a background thread.
    void start(std::vector<std::filesystem::path> files);

    /// Stream over the prefetched file; reads wait for the watermark.
    /// Null if the file is not prefetched (caller falls back to disk).
    std::unique_ptr<std::istream> takeStream(const std::filesystem::path& path);

    /// Join the reader thread. Cheap once parsing has consumed everything.
    void finish();
}

#endif // __vita__
#endif

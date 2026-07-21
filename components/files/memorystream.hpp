#ifndef OPENMW_COMPONENTS_FILES_MEMORYSTREAM_H
#define OPENMW_COMPONENTS_FILES_MEMORYSTREAM_H

#include <istream>
#include <string>

namespace Files
{

    struct MemBuf : std::streambuf
    {
        MemBuf(char const* buffer, size_t size)
            // a streambuf isn't specific to istreams, so we need a non-const pointer :/
            : bufferStart(const_cast<char*>(buffer))
            , bufferEnd(bufferStart + size)
        {
            this->setg(bufferStart, bufferStart, bufferEnd);
        }

        pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override
        {
            if (dir == std::ios_base::cur)
                setg(bufferStart, gptr() + off, bufferEnd);
            else
                setg(bufferStart, (dir == std::ios_base::beg ? bufferStart : bufferEnd) + off, bufferEnd);

            return gptr() - bufferStart;
        }

        pos_type seekpos(pos_type pos, std::ios_base::openmode which) override
        {
            return seekoff(pos, std::ios_base::beg, which);
        }

    protected:
        char* bufferStart;
        char* bufferEnd;
    };

    /// @brief A variant of std::istream that reads from a constant in-memory buffer.
    struct IMemStream : virtual MemBuf, std::istream
    {
        IMemStream(char const* buffer, size_t size)
            : MemBuf(buffer, size)
            , std::istream(static_cast<std::streambuf*>(this))
        {
        }
    };

    /// @brief IMemStream that owns its buffer.
    struct OwningIMemStream : IMemStream
    {
        explicit OwningIMemStream(std::string&& buffer)
            // The virtual base MemBuf is initialized before members, so the
            // real get area is bound in the body once mBuffer exists.
            : MemBuf(nullptr, 0)
            , IMemStream(nullptr, 0)
            , mBuffer(std::move(buffer))
        {
            bufferStart = mBuffer.data();
            bufferEnd = bufferStart + mBuffer.size();
            setg(bufferStart, bufferStart, bufferEnd);
        }

        const char* bufferData() const { return mBuffer.data(); }
        std::size_t bufferSize() const { return mBuffer.size(); }

    private:
        std::string mBuffer;
    };

}

#endif

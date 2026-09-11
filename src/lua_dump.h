// Bounded dump formatting. Metadata queries remain complete; presentation is capped.
#pragma once
#include <ostream>
#include <streambuf>
#include <string>
#include <algorithm>

class LuaDumpBuffer : public std::streambuf
{
  public:
    static constexpr size_t Limit = 256 * 1024;
    bool Full() const { return text.size() >= Limit - 32; }
    std::string str() const
    {
        return truncated ? text + "\n... <dump truncated>\n" : text;
    }
  protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override
    {
        if (count <= 0) return 0;
        const size_t size = static_cast<size_t>(count);
        const size_t copied = (std::min)(size, Limit - 32 - text.size());
        text.append(data, copied);
        truncated |= copied != size;
        return count;
    }
    int_type overflow(int_type value) override
    {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char character = traits_type::to_char_type(value);
        xsputn(&character, 1);
        return value;
    }
  private:
    std::string text;
    bool truncated = false;
};

class LuaDumpStream : private LuaDumpBuffer, public std::ostream
{
  public:
    LuaDumpStream() : std::ostream(static_cast<LuaDumpBuffer*>(this)) {}
    using LuaDumpBuffer::str;
    using LuaDumpBuffer::Full;
};

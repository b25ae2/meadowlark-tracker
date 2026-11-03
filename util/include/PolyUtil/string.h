#pragma once

#include <algorithm>
#include <cassert>
#include <cstring>
#include <span>
#include <string>

inline void strcpytolower(char* dest, char* src, int maxLength)
{
    int pos = 0;
    while (pos < maxLength - 1)
    {
        if (*src == '\0')
        {
            (*dest) = 0;
            break;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        *(dest++) = static_cast<char>(tolower(*(src++)));
        pos++;
    }

    if (pos == maxLength - 1)
    {
        *(dest) = 0;
    }
}

inline void polystrncpy(char* dest, const char* src, int destSize)
{
    strncpy(dest, src, destSize - 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    dest[destSize - 1] = 0;
}

/**
 * Performs a copy of a string_view and adds a null terminator at the end
 * Assumption is that src.size() < dest.size()
 */
inline void copyString(std::string_view src, std::span<char> dest)
{
    assert(src.size() < dest.size());
    std::copy_n(std::begin(src), src.size(), std::begin(dest));

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    dest[src.size()] = '\0';
}

// Trims string to maxChars length and appends suffix if the string had to be cut.
void trimString(std::string& str, int maxChars, const auto& suffix)
{
    auto len = std::ssize(str);
    if (len > maxChars)
    {
        str.replace(maxChars, len, suffix);
    }
}

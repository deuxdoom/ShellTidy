#include "shelltidy.h"
#include <algorithm>

namespace st
{
std::wstring popupSummary(const std::wstring &text, size_t maxChars, size_t maxLines)
{
    std::wstring out;
    size_t lines = 1;
    for (auto c : text)
    {
        if (c == L'\r')
            continue;
        if (out.size() >= maxChars || (c == L'\n' && lines >= maxLines))
        {
            out += L"…";
            break;
        }
        out += c;
        if (c == L'\n')
            ++lines;
    }
    return trim(out);
}
RECT centeredRect(const RECT &work, int width, int height)
{
    width = std::clamp(width, 1, static_cast<int>(std::max(1L, work.right - work.left)));
    height = std::clamp(height, 1, static_cast<int>(std::max(1L, work.bottom - work.top)));
    LONG left = work.left + (work.right - work.left - width) / 2;
    LONG top = work.top + (work.bottom - work.top - height) / 2;
    return {left, top, left + width, top + height};
}
} // namespace st

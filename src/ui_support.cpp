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
bool eligible(const Entry &e, Action action)
{
    switch (action)
    {
    case Action::Delete:
        return !e.protectedItem && !e.unsupported;
    case Action::Disable:
        return e.switchable && !e.disabled;
    default:
        return e.switchable && e.disabled;
    }
}
std::vector<size_t> actionEntries(const std::vector<Entry> &entries, const std::vector<size_t> &visible,
                                  const std::vector<size_t> &selected, Action action)
{
    // Checked rows hidden by the current search or filter are never acted on. Without a visible
    // checked row, the highlighted rows are used, as in list-oriented shell utilities.
    std::vector<size_t> out;
    for (auto i : visible)
        if (i < entries.size() && entries[i].checked && eligible(entries[i], action))
            out.push_back(i);
    bool anyChecked = std::any_of(visible.begin(), visible.end(),
                                  [&](size_t i) { return i < entries.size() && entries[i].checked; });
    if (!anyChecked)
        for (auto i : selected)
            if (i < entries.size() && eligible(entries[i], action))
                out.push_back(i);
    return out;
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

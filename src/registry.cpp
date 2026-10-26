#include "shelltidy.h"
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

namespace st
{
bool sharedTarget(const Target &t, const Roots &roots)
{
    auto path = lower(t.path), classes = lower(roots.classes);
    if (path.starts_with(classes + L"\\"))
    {
        auto relative = path.substr(classes.size() + 1);
        for (const auto &redirected :
             {L"clsid", L"directshow", L"interface", L"media type", L"mediafoundation"})
            if (relative == redirected || relative.starts_with(std::wstring(redirected) + L"\\"))
                return false;
        return true;
    }
    if (t.hive == Hive::User)
        return true;
    return path == L"software\\clients" || path.starts_with(L"software\\clients\\") ||
           path == lower(roots.registeredApps);
}
Target canonicalTarget(Target t, const Roots &roots)
{
    if (sharedTarget(t, roots))
        t.view = KEY_WOW64_64KEY;
    return t;
}
int compareLabels(const std::wstring &a, const std::wstring &b)
{
    int result = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                                 static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr,
                                 nullptr, 0);
    if (result)
        return result - CSTR_EQUAL;
    auto x = lower(a), y = lower(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}
std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}
std::wstring trim(std::wstring s)
{
    auto a = s.find_first_not_of(L" \t\r\n");
    if (a == s.npos)
        return {};
    return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}
HKEY hiveKey(Hive h)
{
    return h == Hive::User ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}
std::wstring errorText(DWORD code)
{
    wchar_t *p = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&p), 0, nullptr);
    std::wstring s = p ? trim(p) : L"Windows error";
    LocalFree(p);
    return s + L" (" + std::to_wstring(code) + L")";
}
std::wstring describe(const Target &t)
{
    return std::wstring(t.hive == Hive::User ? L"HKCU\\" : L"HKLM\\") + t.path +
           (t.tree ? L" [key]" : L" → " + (t.name.empty() ? L"(Default)" : t.name)) +
           (t.view == KEY_WOW64_32KEY ? L" [32-bit]" : L" [64-bit]");
}
Key openKey(const Target &t, REGSAM access)
{
    HKEY k = nullptr;
    LSTATUS e = RegOpenKeyExW(hiveKey(t.hive), t.path.c_str(), 0, access | t.view, &k);
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
        return {};
    if (e != ERROR_SUCCESS)
        throw Error("RegOpenKeyEx", e);
    return Key(k);
}
std::optional<Value> readValue(const Target &t)
{
    auto k = openKey(t, KEY_QUERY_VALUE);
    if (!k.h)
        return {};
    Value v;
    DWORD size = 0;
    LSTATUS e = RegQueryValueExW(k.h, t.name.c_str(), nullptr, &v.type, nullptr, &size);
    if (e == ERROR_FILE_NOT_FOUND)
        return {};
    if (e != ERROR_SUCCESS)
        throw Error("RegQueryValueEx", e);
    if (size > 8 * 1024 * 1024)
        throw Error("Registry value too large", ERROR_FILE_TOO_LARGE);
    for (int i = 0; i < 4; ++i)
    {
        v.data.resize(size);
        e = RegQueryValueExW(k.h, t.name.c_str(), nullptr, &v.type, v.data.data(), &size);
        if (e == ERROR_SUCCESS)
        {
            v.data.resize(size);
            return v;
        }
        if (e != ERROR_MORE_DATA)
            throw Error("RegQueryValueEx", e);
    }
    throw Error("Registry changed during read", ERROR_RETRY);
}
bool exists(const Target &t)
{
    if (t.tree)
        return openKey(t).h != nullptr;
    return readValue(t).has_value();
}
void writeValue(const Target &t, const Value &v)
{
    HKEY raw = nullptr;
    LSTATUS e = RegCreateKeyExW(hiveKey(t.hive), t.path.c_str(), 0, nullptr, 0, KEY_SET_VALUE | t.view,
                                nullptr, &raw, nullptr);
    if (e != ERROR_SUCCESS)
        throw Error("RegCreateKeyEx", e);
    Key k(raw);
    e = RegSetValueExW(k.h, t.name.c_str(), 0, v.type, v.data.data(), static_cast<DWORD>(v.data.size()));
    if (e != ERROR_SUCCESS)
        throw Error("RegSetValueEx", e);
}
std::wstring textValue(const Value &v)
{
    if (v.type != REG_SZ && v.type != REG_EXPAND_SZ)
        return {};
    if (v.data.size() % sizeof(wchar_t))
        return {};
    std::wstring s(v.data.size() / 2, L'\0');
    if (!s.empty())
        memcpy(s.data(), v.data.data(), v.data.size());
    auto end = s.find(L'\0');
    if (end != s.npos)
    {
        if (s.find_first_not_of(L'\0', end) != s.npos)
            return {};
        s.resize(end);
    }
    return s;
}
Value stringValue(const std::wstring &s, DWORD type)
{
    Value v;
    v.type = type;
    v.data.resize((s.size() + 1) * 2);
    memcpy(v.data.data(), s.c_str(), v.data.size());
    return v;
}
std::vector<std::wstring> subkeys(const Target &t)
{
    auto k = openKey(t);
    std::vector<std::wstring> out;
    if (!k.h)
        return out;
    for (DWORD i = 0;; ++i)
    {
        wchar_t name[256];
        DWORD n = 256;
        LSTATUS e = RegEnumKeyExW(k.h, i, name, &n, nullptr, nullptr, nullptr, nullptr);
        if (e == ERROR_NO_MORE_ITEMS)
            break;
        if (e != ERROR_SUCCESS)
            throw Error("RegEnumKeyEx", e);
        out.emplace_back(name, n);
    }
    return out;
}
std::vector<std::pair<std::wstring, Value>> values(const Target &t)
{
    auto k = openKey(t);
    std::vector<std::pair<std::wstring, Value>> out;
    if (!k.h)
        return out;
    for (DWORD i = 0;; ++i)
    {
        std::wstring name(16384, L'\0');
        DWORD n = static_cast<DWORD>(name.size());
        LSTATUS e = RegEnumValueW(k.h, i, name.data(), &n, nullptr, nullptr, nullptr, nullptr);
        if (e == ERROR_NO_MORE_ITEMS)
            break;
        if (e != ERROR_SUCCESS)
            throw Error("RegEnumValue", e);
        name.resize(n);
        Target v = t;
        v.name = name;
        auto data = readValue(v);
        if (data)
            out.emplace_back(name, std::move(*data));
    }
    return out;
}
static bool executableFile(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring resolveCommand(const std::wstring &input)
{
    if (input.empty() || input.size() > 32767)
        return {};
    DWORD n = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (!n || n > 65536)
        return {};
    std::wstring cmd(n, L'\0');
    if (!ExpandEnvironmentStringsW(input.c_str(), cmd.data(), n))
        return {};
    cmd = trim(cmd.c_str());
    if (cmd.empty())
        return {};
    std::wstring exe;
    if (cmd[0] == L'"')
    {
        auto end = cmd.find(L'"', 1);
        if (end == cmd.npos)
            return {};
        exe = cmd.substr(1, end - 1);
    }
    else
    {
        // An unquoted path containing spaces is ambiguous: only accept an existing executable.
        auto low = lower(cmd);
        auto end = low.find(L".exe");
        if (end != low.npos)
        {
            exe = cmd.substr(0, end + 4);
            if (exe.find(L' ') != exe.npos && !executableFile(exe))
                return {};
        }
        else
        {
            auto end2 = cmd.find_first_of(L" \t");
            exe = cmd.substr(0, end2);
        }
    }
    if (exe.empty() || exe.find_first_of(L"<>|\r\n\"%") != exe.npos)
        return {};
    auto ext = lower(std::filesystem::path(exe).extension().wstring());
    if (ext != L".exe" && ext != L".com")
        return {};
    if (PathIsRelativeW(exe.c_str()))
    {
        // Do not use the process working directory or PATH for diagnostic resolution.
        for (auto hive : {Hive::User, Hive::Machine})
            for (DWORD view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
            {
                auto v =
                    readValue({hive, view, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + exe,
                               L"", false});
                if (v)
                {
                    auto s = textValue(*v);
                    if (!s.empty() && !PathIsRelativeW(s.c_str()))
                        return s;
                }
            }
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        auto p = std::wstring(system) + L"\\" + exe;
        if (executableFile(p))
            return p;
        return {};
    }
    wchar_t full[32768];
    DWORD length = GetFullPathNameW(exe.c_str(), 32768, full, nullptr);
    return length && length < 32768 ? full : L"";
}
} // namespace st

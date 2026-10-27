#include "shelltidy.h"
#include <shlwapi.h>
#include <winver.h>
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
std::wstring targetId(const Target &original, const Roots &roots)
{
    auto t = canonicalTarget(original, roots);
    auto path = lower(t.path), name = lower(t.name);
    return std::to_wstring(static_cast<DWORD>(t.hive)) + L":" + std::to_wstring(t.view) + L":" +
           std::to_wstring(t.tree) + L":" + std::to_wstring(path.size()) + L":" + path + name;
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
    for (int i = 0; i < 4; ++i)
    {
        if (size > 8 * 1024 * 1024)
            throw Error("Registry value too large", ERROR_FILE_TOO_LARGE);
        v.data.resize(size);
        BYTE empty = 0; // A null buffer would turn an empty-value read into another size query.
        e = RegQueryValueExW(k.h, t.name.c_str(), nullptr, &v.type, v.data.empty() ? &empty : v.data.data(),
                             &size);
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
    size_t totalBytes = 0;
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
        {
            totalBytes += name.size() * sizeof(wchar_t) + data->data.size();
            if (totalBytes > 64 * 1024 * 1024)
                throw Error("Registry key values too large", ERROR_FILE_TOO_LARGE);
            out.emplace_back(name, std::move(*data));
        }
    }
    return out;
}
static bool executableFile(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring expandEnvironment(const std::wstring &input)
{
    if (input.empty() || input.size() > 32767 || input.find(L'\0') != input.npos)
        return {};
    DWORD n = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (!n || n > 32768)
        return {};
    std::wstring expanded(n, L'\0');
    DWORD written = ExpandEnvironmentStringsW(input.c_str(), expanded.data(), n);
    if (!written || written > n)
        return {};
    expanded.resize(written - 1);
    return expanded;
}
std::wstring resolveCommand(const std::wstring &input)
{
    auto cmd = trim(expandEnvironment(input));
    if (cmd.empty())
        return {};
    std::wstring exe;
    if (cmd[0] == L'"')
    {
        auto end = cmd.find(L'"', 1);
        if (end == cmd.npos || (end + 1 < cmd.size() && cmd[end + 1] != L' ' && cmd[end + 1] != L'\t'))
            return {};
        exe = cmd.substr(1, end - 1);
    }
    else
    {
        // Follow CreateProcess: try each whitespace boundary and use the first existing executable.
        // When none exists, the first .exe/.com interpretation is the command that can no longer run.
        // '.exe' may also occur in a directory name or a later argument.
        // Network paths are not probed per candidate: an offline server would stall every lookup.
        std::wstring first;
        bool probe = !cmd.starts_with(L"\\\\");
        for (size_t end = 1; end <= cmd.size(); ++end)
        {
            if (end != cmd.size() && cmd[end] != L' ' && cmd[end] != L'\t')
                continue;
            auto candidate = cmd.substr(0, end);
            auto path = std::filesystem::path(candidate);
            auto ext = lower(path.extension().wstring());
            if (probe && ext.empty() && path.is_absolute() && executableFile(candidate + L".exe"))
            {
                exe = candidate + L".exe";
                break;
            }
            if (ext != L".exe" && ext != L".com")
                continue;
            if (probe && path.is_absolute() && executableFile(candidate))
            {
                exe = std::move(candidate);
                break;
            }
            if (first.empty())
                first = std::move(candidate);
        }
        if (exe.empty())
            exe = std::move(first);
    }
    if (exe.empty() || exe.find_first_of(L"<>|\r\n\"%") != exe.npos)
        return {};
    auto ext = lower(std::filesystem::path(exe).extension().wstring());
    if (ext != L".exe" && ext != L".com")
        return {};
    auto path = std::filesystem::path(exe);
    if (!path.is_absolute())
    {
        // Drive-relative, root-relative and directory-relative paths depend on launch context.
        if (path.has_parent_path() || path.has_root_name() || path.has_root_directory())
            return {};
        // Do not use the process working directory or PATH for diagnostic resolution.
        for (auto hive : {Hive::User, Hive::Machine})
            for (DWORD view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
            {
                auto v =
                    readValue({hive, view, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + exe,
                               L"", false});
                if (v)
                {
                    auto s = trim(expandEnvironment(textValue(*v)));
                    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"')
                        s = s.substr(1, s.size() - 2);
                    if (std::filesystem::path(s).is_absolute() && s.find_first_of(L"<>|\r\n\"%") == s.npos &&
                        (lower(std::filesystem::path(s).extension().wstring()) == L".exe" ||
                         lower(std::filesystem::path(s).extension().wstring()) == L".com"))
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
FileState fileState(const std::wstring &path)
{
    if (path.empty())
        return FileState::Unavailable;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES)
        return a & FILE_ATTRIBUTE_DIRECTORY ? FileState::Folder : FileState::Present;
    DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        return FileState::Unavailable;
    // An absent drive letter or network share also reports a missing path. Only a reachable volume
    // root proves that the file itself is gone.
    std::wstring root;
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
        root = path.substr(0, 3);
    else if (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\\\.\\"))
    {
        auto server = path.find(L'\\', 2);
        auto share = server == path.npos ? path.npos : path.find(L'\\', server + 1);
        if (share == path.npos)
            return FileState::Unavailable;
        root = path.substr(0, share + 1);
    }
    else
        return FileState::Unavailable;
    DWORD r = GetFileAttributesW(root.c_str());
    return r != INVALID_FILE_ATTRIBUTES && (r & FILE_ATTRIBUTE_DIRECTORY) ? FileState::Missing
                                                                          : FileState::Unavailable;
}
bool isGuid(const std::wstring &s)
{
    if (s.size() != 38 || s.front() != L'{' || s.back() != L'}')
        return false;
    for (size_t i = 1; i < 37; ++i)
    {
        bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        if (dash ? s[i] != L'-' : !iswxdigit(s[i]))
            return false;
    }
    return true;
}
std::wstring displayText(const std::wstring &raw)
{
    auto s = trim(raw);
    if (s.starts_with(L"@"))
    {
        wchar_t text[1024]{};
        if (FAILED(SHLoadIndirectString(s.c_str(), text, 1024, nullptr)))
            return {};
        s = trim(text);
    }
    // Menu captions carry keyboard accelerators: "열기(&O)" -> "열기", "&Open" -> "Open", "&&" -> "&".
    if (s.size() >= 4 && s.ends_with(L")") && s[s.size() - 4] == L'(' && s[s.size() - 3] == L'&')
        s.resize(s.size() - 4);
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != L'&')
            out += s[i];
        else if (i + 1 < s.size() && s[i + 1] == L'&')
            out += s[++i];
    }
    return trim(out);
}
std::pair<std::wstring, std::wstring> versionText(const std::wstring &path)
{
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size || size > 16 * 1024 * 1024)
        return {};
    Bytes data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Translation
    {
        WORD language, codePage;
    };
    std::vector<std::wstring> blocks;
    Translation *translations = nullptr;
    UINT bytes = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&translations),
                       &bytes) &&
        translations)
        for (UINT i = 0; i < bytes / sizeof(Translation) && i < 8; ++i)
        {
            wchar_t block[16];
            swprintf_s(block, L"%04x%04x", translations[i].language, translations[i].codePage);
            blocks.push_back(block);
        }
    blocks.push_back(L"040904b0");
    blocks.push_back(L"040904e4");
    auto query = [&](const wchar_t *name) -> std::wstring
    {
        for (const auto &block : blocks)
        {
            wchar_t *text = nullptr;
            UINT n = 0;
            auto key = L"\\StringFileInfo\\" + block + L"\\" + name;
            if (VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void **>(&text), &n) && text && n)
                return trim(std::wstring(text, wcsnlen(text, n)));
        }
        return {};
    };
    return {query(L"FileDescription"), query(L"CompanyName")};
}
std::wstring registryPath(const Target &original, const Roots &roots)
{
    auto t = canonicalTarget(original, roots);
    auto path = t.path;
    // A non-shared 32-bit view is physically stored below WOW6432Node.
    if (t.view == KEY_WOW64_32KEY && !sharedTarget(t, roots))
    {
        auto p = lower(path);
        if (p.starts_with(L"software\\classes\\"))
            path.insert(17, L"WOW6432Node\\");
        else if (p.starts_with(L"software\\"))
            path.insert(9, L"WOW6432Node\\");
    }
    return std::wstring(t.hive == Hive::User ? L"HKEY_CURRENT_USER\\" : L"HKEY_LOCAL_MACHINE\\") + path;
}
} // namespace st

#include "shelltidy.h"
#include <shlobj.h>
#include <algorithm>
#include <fstream>

namespace st
{
namespace
{
constexpr size_t MaxBackup = 64 * 1024 * 1024;
struct Writer
{
    Bytes b;
    void u(DWORD n)
    {
        if (b.size() > MaxBackup - 4)
            throw Error("Backup too large", ERROR_FILE_TOO_LARGE);
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<BYTE>(n >> (i * 8)));
    }
    void str(const std::wstring &s)
    {
        if (s.size() > 32768)
            throw Error("Backup string too large", ERROR_INVALID_DATA);
        u(static_cast<DWORD>(s.size()));
        if (s.size() * 2 > MaxBackup - b.size())
            throw Error("Backup too large", ERROR_FILE_TOO_LARGE);
        auto p = reinterpret_cast<const BYTE *>(s.data());
        b.insert(b.end(), p, p + s.size() * 2);
    }
    void val(const Value &v)
    {
        if (v.data.size() > 8 * 1024 * 1024)
            throw Error("Backup value too large", ERROR_FILE_TOO_LARGE);
        u(v.type);
        u(static_cast<DWORD>(v.data.size()));
        if (v.data.size() > MaxBackup - b.size())
            throw Error("Backup too large", ERROR_FILE_TOO_LARGE);
        b.insert(b.end(), v.data.begin(), v.data.end());
    }
};
struct Reader
{
    const Bytes &b;
    size_t pos = 0;
    DWORD u()
    {
        if (pos + 4 > b.size())
            throw Error("Truncated backup", ERROR_INVALID_DATA);
        DWORD n = 0;
        for (int i = 0; i < 4; ++i)
            n |= static_cast<DWORD>(b[pos++]) << (8 * i);
        return n;
    }
    std::wstring str()
    {
        auto n = u();
        if (n > 32768 || pos + n * 2 > b.size())
            throw Error("Invalid backup string", ERROR_INVALID_DATA);
        std::wstring s(n, L'\0');
        if (n)
            memcpy(s.data(), b.data() + pos, n * 2);
        pos += n * 2;
        if (s.find(L'\0') != s.npos)
            throw Error("Embedded null", ERROR_INVALID_DATA);
        return s;
    }
    Value val()
    {
        Value v;
        v.type = u();
        auto n = u();
        if (n > 8 * 1024 * 1024 || pos + n > b.size())
            throw Error("Invalid backup value", ERROR_INVALID_DATA);
        v.data.assign(b.begin() + pos, b.begin() + pos + n);
        pos += n;
        return v;
    }
    DWORD count()
    {
        auto n = u();
        if (n > 100000)
            throw Error("Invalid backup count", ERROR_INVALID_DATA);
        return n;
    }
};
DWORD checksum(const Bytes &b, size_t n)
{
    DWORD h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 16777619u;
    return h;
}
bool under(const std::wstring &path, const std::wstring &root)
{
    return path.starts_with(root + L"\\");
}
bool validPath(const std::wstring &path, bool relative = false)
{
    return (relative && path.empty()) || (!path.empty() && path.front() != L'\\' && path.back() != L'\\' &&
                                          path.find(L"\\\\") == path.npos && path.find(L'\0') == path.npos &&
                                          path.find(L'/') == path.npos && path.find(L"..") == path.npos &&
                                          lower(path).find(L"userchoice") == path.npos);
}
// A reversible switch is a verb's LegacyDisable value or a CLSID in Shell Extensions\Blocked.
bool switchTarget(const Target &t)
{
    auto p = lower(t.path);
    if (t.tree)
        return false;
    if (lower(t.name) == L"legacydisable")
    {
        auto verb = p.rfind(L"\\shell\\");
        return verb != p.npos && p.find(L'\\', verb + 7) == p.npos;
    }
    return p.ends_with(L"\\shell extensions\\blocked") && isGuid(t.name);
}
Value switchValue(const Target &t, const std::wstring &name)
{
    return stringValue(lower(t.name) == L"legacydisable" ? L"" : name.substr(0, 200));
}
void validateSnapshot(const Mutation &m)
{
    if (!validPath(m.target.path) || m.target.name.find(L'\0') != m.target.name.npos ||
        (m.target.hive != Hive::User && m.target.hive != Hive::Machine) ||
        (m.target.view != KEY_WOW64_64KEY && m.target.view != KEY_WOW64_32KEY) || m.phase > 3 ||
        m.keys.size() > 100000 || m.before.size() > 100000)
        throw Error("Invalid mutation snapshot", ERROR_INVALID_DATA);
    std::set<std::wstring> keys, names;
    for (const auto &k : m.keys)
        if (!validPath(k, true) || !keys.insert(lower(k)).second)
            throw Error("Invalid snapshot key", ERROR_INVALID_DATA);
    if (m.target.tree)
    {
        if (m.after || !keys.contains(L""))
            throw Error("Invalid subtree snapshot", ERROR_INVALID_DATA);
    }
    else if (m.before.empty())
    {
        // A created switch did not exist before; restore removes only this exact string.
        if (!m.keys.empty() || !m.after || !switchTarget(m.target) || m.after->type != REG_SZ ||
            *m.after != stringValue(textValue(*m.after)) || textValue(*m.after).size() > 200)
            throw Error("Invalid switch snapshot", ERROR_INVALID_DATA);
    }
    else if (!m.keys.empty() || m.before.size() != 1 || !m.before[0].relative.empty() ||
             m.before[0].name != m.target.name)
        throw Error("Invalid value snapshot", ERROR_INVALID_DATA);
    for (const auto &v : m.before)
    {
        auto id = std::to_wstring(v.relative.size()) + L":" + lower(v.relative) + lower(v.name);
        if (!validPath(v.relative, true) || v.name.find(L'\0') != v.name.npos ||
            (m.target.tree && !keys.contains(lower(v.relative))) || !names.insert(id).second)
            throw Error("Invalid snapshot value", ERROR_INVALID_DATA);
    }
    if (m.after && !m.before.empty())
    {
        const auto &before = m.before[0].value;
        auto old = textValue(before), next = textValue(*m.after);
        if (!lower(m.target.path).ends_with(L"\\openwithlist") || lower(m.target.name) != L"mrulist" ||
            (before.type != REG_SZ && before.type != REG_EXPAND_SZ) || m.after->type != before.type ||
            before != stringValue(old, before.type) || *m.after != stringValue(next, before.type))
            throw Error("Invalid MRU replacement", ERROR_INVALID_DATA);
        size_t position = 0;
        for (auto c : next)
        {
            position = old.find(c, position);
            if (position == old.npos)
                throw Error("MRU replacement must only remove letters", ERROR_INVALID_DATA);
            ++position;
        }
    }
}
void validate(const Target &t, const Roots &roots)
{
    auto p = lower(t.path), c = lower(roots.classes), f = lower(roots.fileExts);
    if ((t.view != KEY_WOW64_64KEY && t.view != KEY_WOW64_32KEY) ||
        (t.hive != Hive::User && t.hive != Hive::Machine) || p.find(L"userchoice") != p.npos ||
        !validPath(t.path) || t.name.find(L'\0') != t.name.npos)
        throw Error("Forbidden registry target", ERROR_ACCESS_DENIED);
    bool ok = false;
    if (under(p, c))
    {
        auto rel = p.substr(c.size() + 1);
        if (t.tree)
        {
            ok = rel.starts_with(L"applications\\") && std::count(rel.begin(), rel.end(), L'\\') == 1;
            ok = ok || rel.find(L"\\shell\\") != rel.npos ||
                 rel.find(L"\\shellex\\contextmenuhandlers\\") != rel.npos ||
                 rel.find(L"\\openwithlist\\") != rel.npos;
            if (rel.starts_with(L"clsid\\") || rel.starts_with(L"packagedcom\\"))
                ok = false;
        }
        else
            ok = p.ends_with(L"\\openwithprogids") ||
                 (switchTarget(t) && !rel.starts_with(L"clsid\\") && !rel.starts_with(L"packagedcom\\"));
    }
    if (t.hive == Hive::User && under(p, f) && !t.tree)
        ok = p.ends_with(L"\\openwithprogids") || p.ends_with(L"\\openwithlist");
    if (!t.tree && p == lower(roots.blocked) && isGuid(t.name) && t.view == KEY_WOW64_64KEY)
        ok = true;
    if (!t.tree && p.starts_with(L"software\\") && p.ends_with(L"\\fileassociations"))
    {
        for (const auto &[n, v] : values({t.hive, t.view, roots.registeredApps, L"", false}))
            if (lower(textValue(v) + L"\\FileAssociations") == p)
                ok = true;
    }
    if (!ok)
        throw Error("Unsupported registry target", ERROR_ACCESS_DENIED);
}
void captureTree(const Target &target, const std::wstring &relative, Mutation &m, unsigned depth = 0,
                 size_t *captured = nullptr)
{
    size_t localBytes = 0;
    if (!captured)
        captured = &localBytes;
    if (depth > 48 || m.before.size() >= 100000 || m.keys.size() >= 100000)
        throw Error("Registry subtree too large", ERROR_FILE_TOO_LARGE);
    auto t = target;
    if (!relative.empty())
        t.path += L"\\" + relative;
    m.keys.push_back(relative);
    *captured += relative.size() * sizeof(wchar_t) + 4;
    for (const auto &[name, value] : values(t))
    {
        *captured += (relative.size() + name.size()) * sizeof(wchar_t) + value.data.size() + 16;
        if (*captured > MaxBackup)
            throw Error("Registry subtree exceeds backup limit", ERROR_FILE_TOO_LARGE);
        m.before.push_back({relative, name, value});
    }
    if (*captured > MaxBackup)
        throw Error("Registry subtree exceeds backup limit", ERROR_FILE_TOO_LARGE);
    for (const auto &child : subkeys(t))
        captureTree(target, relative.empty() ? child : relative + L"\\" + child, m, depth + 1, captured);
}
bool sameTree(const Mutation &a, const Mutation &b)
{
    if (a.keys != b.keys || a.before.size() != b.before.size())
        return false;
    auto sort = [](const SavedValue &x, const SavedValue &y)
    { return lower(x.relative) + L"|" + lower(x.name) < lower(y.relative) + L"|" + lower(y.name); };
    auto x = a.before, y = b.before;
    std::sort(x.begin(), x.end(), sort);
    std::sort(y.begin(), y.end(), sort);
    for (size_t i = 0; i < x.size(); ++i)
        if (lower(x[i].relative) != lower(y[i].relative) || lower(x[i].name) != lower(y[i].name) ||
            x[i].value != y[i].value)
            return false;
    return true;
}
bool matchesBefore(const Mutation &m)
{
    if (m.target.tree)
    {
        if (!exists(m.target))
            return false;
        Mutation now;
        captureTree(m.target, L"", now);
        return sameTree(m, now);
    }
    auto v = readValue(m.target);
    if (m.before.empty())
        return !v; // A switch is created only where no value exists.
    return v && m.before.size() == 1 && *v == m.before[0].value;
}
void deleteTarget(const Target &t)
{
    if (t.tree)
    {
        // Open the parent in the explicit WOW64 view; RegDeleteTree itself has no view argument.
        auto cut = t.path.find_last_of(L'\\');
        auto parent = t;
        parent.path = t.path.substr(0, cut);
        auto k = openKey(parent, KEY_READ | KEY_WRITE | DELETE);
        if (!k.h)
            throw Error("Registry target disappeared", ERROR_FILE_NOT_FOUND);
        LSTATUS e = RegDeleteTreeW(k.h, t.path.substr(cut + 1).c_str());
        if (e != ERROR_SUCCESS)
            throw Error("RegDeleteTree", e);
    }
    else
    {
        auto k = openKey(t, KEY_SET_VALUE);
        if (!k.h)
            throw Error("Registry target disappeared", ERROR_FILE_NOT_FOUND);
        LSTATUS e = RegDeleteValueW(k.h, t.name.c_str());
        if (e != ERROR_SUCCESS)
            throw Error("RegDeleteValue", e);
    }
}
std::wstring entryContext(const Entry &e)
{
    return std::to_wstring(static_cast<int>(e.kind)) + L":" + std::to_wstring(e.command.size()) + L":" +
           e.command + lower(e.executable);
}
using Inventory = std::map<std::wstring, std::set<std::wstring>>;
Inventory allowedNow(const Roots &roots, const std::atomic_bool &cancel)
{
    Inventory out;
    std::set<std::wstring> blocked;
    auto s = scan(roots, cancel);
    if (s.canceled)
        throw Error("Canceled", ERROR_CANCELLED);
    for (const auto &e : s.entries)
    {
        for (const auto &t : e.targets)
        {
            auto id = targetId(t, roots);
            if (e.protectedItem || e.unsupported)
                blocked.insert(id);
            else
                out[id].insert(entryContext(e));
        }
        for (const auto &t : e.switches)
        {
            auto id = targetId(t, roots);
            if (e.switchable)
                out[id].insert(entryContext(e));
            else
                blocked.insert(id);
        }
    }
    for (const auto &id : blocked)
        out.erase(id); // Protection in either WOW64 view wins for a shared physical registration.
    return out;
}
} // namespace
std::wstring backupDirectory()
{
    wchar_t overridePath[32768];
    DWORD n = GetEnvironmentVariableW(L"SHELLTIDY_BACKUP_DIR", overridePath, 32768);
    if (n && n < 32768)
    {
        auto p = std::filesystem::path(overridePath);
        if (!p.is_absolute())
            throw Error("Backup override must be absolute", ERROR_BAD_PATHNAME);
        return p.wstring();
    }
    PWSTR p = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &p);
    if (FAILED(hr))
        throw Error("LocalAppData unavailable", static_cast<DWORD>(hr));
    std::wstring path = std::wstring(p) + L"\\ShellTidy\\Backups";
    CoTaskMemFree(p);
    return path;
}
std::vector<std::wstring> backupFiles()
{
    std::vector<std::wstring> out;
    auto p = backupDirectory();
    if (!std::filesystem::exists(p))
        return out;
    for (const auto &entry : std::filesystem::directory_iterator(p))
        if (entry.is_regular_file() && entry.path().extension() == L".stb")
            out.push_back(entry.path().wstring());
    std::sort(out.rbegin(), out.rend());
    return out;
}
Journal prepare(const std::vector<Entry> &entries, const Roots &roots, Action action)
{
    std::atomic_bool cancel = false;
    auto allowed = allowedNow(roots, cancel);
    Journal j;
    std::set<std::wstring> seen;
    size_t captured = 0;
    for (const auto &e : entries)
    {
        if (action == Action::Delete)
            continue;
        // Disabling creates the first switch; enabling removes every switch that currently exists.
        if (!e.switchable || e.switches.empty() || e.disabled != (action == Action::Enable))
            throw Error("Entry cannot be switched", ERROR_ACCESS_DENIED);
        auto chosen = action == Action::Disable ? std::vector<Target>{e.switches.front()} : e.switches;
        for (const auto &original : chosen)
        {
            auto t = canonicalTarget(original, roots);
            validate(t, roots);
            auto id = targetId(t, roots);
            if (!allowed.contains(id) || !allowed.at(id).contains(entryContext(e)))
                throw Error("Entry changed or is now locked", ERROR_RETRY);
            if (!seen.insert(id).second)
                continue;
            auto v = readValue(t);
            if (action == Action::Disable ? v.has_value() : !v.has_value())
                continue;
            Mutation m;
            m.target = t;
            m.context = entryContext(e);
            if (action == Action::Disable)
                m.after = switchValue(t, e.name);
            else
                m.before.push_back({L"", t.name, *v});
            validateSnapshot(m);
            j.items.push_back(std::move(m));
        }
    }
    for (const auto &e : entries)
    {
        if (action != Action::Delete)
            break;
        if (e.protectedItem || e.unsupported)
            throw Error("Protected entry", ERROR_ACCESS_DENIED);
        for (const auto &original : e.targets)
        {
            auto t = canonicalTarget(original, roots);
            validate(t, roots);
            auto id = targetId(t, roots);
            if (!allowed.contains(id) || !allowed.at(id).contains(entryContext(e)))
                throw Error("Entry changed or is now protected", ERROR_RETRY);
            if (!seen.insert(id).second)
                continue;
            Mutation m;
            m.target = t;
            m.context = entryContext(e);
            if (t.tree)
            {
                if (!exists(t))
                    throw Error("Entry disappeared", ERROR_FILE_NOT_FOUND);
                captureTree(t, L"", m);
            }
            else
            {
                auto v = readValue(t);
                if (!v)
                    throw Error("Entry disappeared", ERROR_FILE_NOT_FOUND);
                m.before.push_back({L"", t.name, *v});
            }
            validateSnapshot(m);
            captured +=
                (m.target.path.size() + m.target.name.size() + m.context.size()) * sizeof(wchar_t) + 64;
            for (const auto &key : m.keys)
                captured += key.size() * sizeof(wchar_t) + 4;
            for (const auto &value : m.before)
                captured += (value.relative.size() + value.name.size()) * sizeof(wchar_t) +
                            value.value.data.size() + 16;
            if (captured > MaxBackup)
                throw Error("Selection exceeds backup limit", ERROR_FILE_TOO_LARGE);
            j.items.push_back(std::move(m));
        }
    }
    // Collapse overlapping keys/values so a parent subtree is backed up and removed only once.
    std::vector<Mutation> unique;
    std::vector<bool> covered(j.items.size(), false);
    for (size_t i = 0; i < j.items.size(); ++i)
        for (size_t k = 0; k < j.items.size(); ++k)
            if (k != i)
            {
                auto &a = j.items[i].target;
                auto &b = j.items[k].target;
                if (b.tree && a.hive == b.hive && a.view == b.view &&
                    (lower(a.path) == lower(b.path) || under(lower(a.path), lower(b.path))))
                {
                    covered[i] = true;
                    break;
                }
            }
    for (size_t i = 0; i < j.items.size(); ++i)
        if (!covered[i])
            unique.push_back(std::move(j.items[i]));
    j.items = std::move(unique);
    std::map<std::wstring, std::pair<Target, std::set<wchar_t>>> mru;
    for (const auto &m : j.items)
        if (!m.target.tree && lower(m.target.path).ends_with(L"\\openwithlist") && m.target.name.size() == 1)
        {
            auto t = m.target;
            t.name = L"MRUList";
            auto &pair = mru[targetId(t, roots)];
            pair.first = t;
            pair.second.insert(m.target.name[0]);
        }
    for (const auto &[id, pair] : mru)
    {
        auto v = readValue(pair.first);
        if (!v)
            continue;
        auto old = textValue(*v);
        if (v->type != REG_SZ && v->type != REG_EXPAND_SZ)
            throw Error("Invalid MRUList type", ERROR_INVALID_DATA);
        std::wstring s;
        for (auto c : old)
            if (!pair.second.contains(c))
                s += c;
        if (s == old)
            continue;
        Mutation m;
        m.target = pair.first;
        m.before.push_back({L"", L"MRUList", *v});
        m.after = stringValue(s, v->type);
        validateSnapshot(m);
        j.items.push_back(std::move(m));
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[96];
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u-%03u-%lu", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    static std::atomic_uint serial = 0;
    wchar_t suffix[20];
    swprintf_s(suffix, L"-%06u", ++serial);
    j.file = backupDirectory() + L"\\" + stamp + suffix + L".stb";
    return j;
}
void saveJournal(const Journal &j)
{
    Writer w;
    w.u(0x42545453);
    w.u(2);
    if (j.items.size() > 100000 || !std::filesystem::path(j.file).is_absolute())
        throw Error("Invalid backup destination or count", ERROR_INVALID_DATA);
    w.u(static_cast<DWORD>(j.items.size()));
    for (const auto &m : j.items)
    {
        validateSnapshot(m);
        w.u(static_cast<DWORD>(m.target.hive));
        w.u(m.target.view);
        w.str(m.target.path);
        w.str(m.target.name);
        w.u(m.target.tree);
        w.u(m.phase);
        w.str(m.context);
        w.u(m.after.has_value());
        if (m.after)
            w.val(*m.after);
        w.u(static_cast<DWORD>(m.keys.size()));
        for (const auto &k : m.keys)
            w.str(k);
        w.u(static_cast<DWORD>(m.before.size()));
        for (const auto &v : m.before)
        {
            w.str(v.relative);
            w.str(v.name);
            w.val(v.value);
        }
        if (w.b.size() > MaxBackup)
            throw Error("Backup too large", ERROR_FILE_TOO_LARGE);
    }
    auto hash = checksum(w.b, w.b.size());
    w.u(hash);
    std::error_code directoryError;
    std::filesystem::create_directories(std::filesystem::path(j.file).parent_path(), directoryError);
    if (directoryError)
        throw Error("Create backup directory", static_cast<DWORD>(directoryError.value()));
    auto temp = j.file + L".tmp";
    {
        Handle f(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (f.h == INVALID_HANDLE_VALUE)
            throw Error("Create backup");
        DWORD done = 0;
        if (!WriteFile(f.h, w.b.data(), static_cast<DWORD>(w.b.size()), &done, nullptr) ||
            done != w.b.size() || !FlushFileBuffers(f.h))
            throw Error("Flush backup");
    }
    if (!MoveFileExW(temp.c_str(), j.file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Error("Commit backup");
}
Journal loadJournal(const std::wstring &file)
{
    Handle f(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr));
    if (f.h == INVALID_HANDLE_VALUE)
        throw Error("Open backup");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f.h, &size) || size.QuadPart < 16 || size.QuadPart > MaxBackup)
        throw Error("Invalid backup size", ERROR_INVALID_DATA);
    Bytes b(static_cast<size_t>(size.QuadPart));
    DWORD done = 0;
    if (!ReadFile(f.h, b.data(), static_cast<DWORD>(b.size()), &done, nullptr) || done != b.size())
        throw Error("Read backup");
    Reader r{b};
    Reader trailer{b, b.size() - 4};
    if (trailer.u() != checksum(b, b.size() - 4))
        throw Error("Backup checksum mismatch", ERROR_INVALID_DATA);
    if (r.u() != 0x42545453)
        throw Error("Invalid backup header", ERROR_INVALID_DATA);
    DWORD version = r.u();
    if (version != 1 && version != 2)
        throw Error("Unsupported backup version", ERROR_INVALID_DATA);
    Journal j;
    j.file = file;
    auto count = r.count();
    for (DWORD i = 0; i < count; ++i)
    {
        Mutation m;
        m.target.hive = static_cast<Hive>(r.u());
        m.target.view = r.u();
        m.target.path = r.str();
        m.target.name = r.str();
        auto tree = r.u();
        if (tree > 1)
            throw Error("Invalid target type", ERROR_INVALID_DATA);
        m.target.tree = tree != 0;
        m.phase = r.u();
        if (m.phase > 3)
            throw Error("Invalid phase", ERROR_INVALID_DATA);
        if (version >= 2)
            m.context = r.str();
        auto replacement = r.u();
        if (replacement > 1)
            throw Error("Invalid replacement flag", ERROR_INVALID_DATA);
        if (replacement)
            m.after = r.val();
        auto n = r.count();
        for (DWORD k = 0; k < n; ++k)
        {
            auto s = r.str();
            if (s.starts_with(L"\\") || s.find(L"..") != s.npos)
                throw Error("Invalid relative key", ERROR_INVALID_DATA);
            m.keys.push_back(std::move(s));
        }
        n = r.count();
        for (DWORD k = 0; k < n; ++k)
        {
            SavedValue v;
            v.relative = r.str();
            v.name = r.str();
            v.value = r.val();
            if (v.relative.starts_with(L"\\") || v.relative.find(L"..") != v.relative.npos)
                throw Error("Invalid relative key", ERROR_INVALID_DATA);
            m.before.push_back(std::move(v));
        }
        validateSnapshot(m);
        j.items.push_back(std::move(m));
    }
    auto hash = r.u();
    if (r.pos != b.size() || hash != checksum(b, b.size() - 4))
        throw Error("Backup checksum mismatch", ERROR_INVALID_DATA);
    return j;
}
Outcome apply(Journal &j, const std::atomic_bool &cancel, const Roots &roots)
{
    Outcome o;
    o.backup = j.file;
    if (cancel)
        return o;
    auto allowed = allowedNow(roots, cancel);
    for (const auto &m : j.items)
        validateSnapshot(m);
    saveJournal(j);
    for (auto &m : j.items)
    {
        if (cancel)
            break;
        if (m.phase != 0)
            continue;
        try
        {
            validate(m.target, roots);
            bool mru = lower(m.target.name) == L"mrulist" && m.after.has_value();
            auto id = targetId(m.target, roots);
            if (!mru && (m.context.empty() || !allowed.contains(id) || !allowed.at(id).contains(m.context)))
                throw Error("Entry is protected or changed", ERROR_RETRY);
            if (!matchesBefore(m))
                throw Error("Registry changed; refresh first", ERROR_RETRY);
            if (mru)
            {
                auto before = textValue(m.before[0].value);
                auto planned = textValue(*m.after);
                std::wstring actual;
                for (auto c : before)
                {
                    auto t = m.target;
                    t.name = std::wstring(1, c);
                    if (planned.find(c) != planned.npos || readValue(t))
                        actual += c;
                }
                m.after = stringValue(actual, m.before[0].value.type);
                if (*m.after == m.before[0].value)
                    continue;
            }
            m.phase = 1;
            saveJournal(j); // No mutation occurs unless this durable write succeeds.
            if (m.after)
                writeValue(m.target, *m.after);
            else
                deleteTarget(m.target);
            auto remaining = m.target.tree ? std::optional<Value>{} : readValue(m.target);
            if (m.target.tree ? exists(m.target) : (m.after ? remaining != m.after : remaining.has_value()))
                throw Error("Mutation verification failed", ERROR_WRITE_FAULT);
            m.phase = 2;
            o.registryChanged = true;
            ++o.success;
            saveJournal(j);
        }
        catch (const Error &e)
        {
            if (m.phase == 1)
                try
                {
                    o.registryChanged = o.registryChanged || !matchesBefore(m);
                }
                catch (...)
                {
                    // The durable intent remains recoverable even when the changed key is unreadable.
                }
            ++o.failed;
            o.detail += describe(m.target) + L"\r\n" + errorText(e.code) + L"\r\n\r\n";
            if (e.code == ERROR_DISK_FULL || e.code == ERROR_WRITE_FAULT)
                break;
        }
    }
    if (cancel)
        o.detail +=
            L"Canceled. Completed changes remain backed up. / 취소됨. 완료된 변경은 백업되어 있습니다.\r\n";
    return o;
}
Outcome restore(Journal &j, const std::atomic_bool &cancel, const Roots &roots)
{
    Outcome o;
    o.backup = j.file;
    for (auto it = j.items.rbegin(); it != j.items.rend(); ++it)
    {
        if (cancel)
            break;
        auto &m = *it;
        if (m.phase != 1 && m.phase != 2)
            continue;
        try
        {
            validateSnapshot(m);
            validate(m.target, roots);
            unsigned conflicts = 0;
            bool changed = false;
            if (m.target.tree)
            {
                // Never overwrite a replacement subtree. Partial recovery only adds missing values.
                for (const auto &v : m.before)
                {
                    auto t = m.target;
                    if (!v.relative.empty())
                        t.path += L"\\" + v.relative;
                    t.name = v.name;
                    t.tree = false;
                    auto current = readValue(t);
                    if (current && *current != v.value)
                        ++conflicts;
                }
                if (conflicts)
                {
                    o.conflicts += conflicts;
                    o.detail += describe(m.target) +
                                L"\r\nConflict: preserved current subtree / 충돌: 현재 하위 키 보존\r\n";
                    continue;
                }
                for (const auto &relative : m.keys)
                {
                    auto t = m.target;
                    if (!relative.empty())
                        t.path += L"\\" + relative;
                    if (!openKey(t).h)
                    {
                        HKEY raw = nullptr;
                        LSTATUS e = RegCreateKeyExW(hiveKey(t.hive), t.path.c_str(), 0, nullptr, 0,
                                                    KEY_WRITE | t.view, nullptr, &raw, nullptr);
                        if (e != ERROR_SUCCESS)
                            throw Error("Restore key", e);
                        RegCloseKey(raw);
                        changed = true;
                        o.registryChanged = true;
                    }
                }
                for (const auto &v : m.before)
                {
                    auto t = m.target;
                    if (!v.relative.empty())
                        t.path += L"\\" + v.relative;
                    t.name = v.name;
                    t.tree = false;
                    if (!readValue(t))
                    {
                        writeValue(t, v.value);
                        changed = true;
                        o.registryChanged = true;
                    }
                }
            }
            else if (m.before.empty())
            {
                // Undo a created switch only while it still holds the value ShellTidy wrote.
                auto current = readValue(m.target);
                if (current && current == m.after)
                {
                    deleteTarget(m.target);
                    changed = true;
                    o.registryChanged = true;
                }
                else if (current)
                {
                    ++o.conflicts;
                    o.detail += describe(m.target) +
                                L"\r\nConflict: preserved current value / 충돌: 현재 값 보존\r\n";
                    continue;
                }
            }
            else
            {
                auto current = readValue(m.target);
                auto &original = m.before[0].value;
                if (current && *current == original)
                {
                } // Already present, including a failed deletion.
                else if (m.after ? current == m.after : !current)
                {
                    writeValue(m.target, original);
                    changed = true;
                    o.registryChanged = true;
                }
                else
                {
                    ++o.conflicts;
                    o.detail += describe(m.target) +
                                L"\r\nConflict: preserved current value / 충돌: 현재 값 보존\r\n";
                    continue;
                }
            }
            m.phase = 3;
            if (changed)
                ++o.success;
            saveJournal(j);
        }
        catch (const Error &e)
        {
            ++o.failed;
            o.detail += describe(m.target) + L"\r\n" + errorText(e.code) + L"\r\n";
        }
    }
    return o;
}
} // namespace st

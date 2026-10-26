#include "shelltidy.h"
#include <algorithm>
#include <shlwapi.h>

namespace st
{
namespace
{
class Scanner
{
    Roots roots;
    const std::atomic_bool &cancel;
    ScanResult result;
    std::map<std::wstring, size_t> groups;
    std::map<std::wstring, std::wstring> commandCache;
    std::set<std::wstring> defaults, defaultPaths;
    std::wstring windows;
    std::wstring get(const Target &t)
    {
        try
        {
            auto v = readValue(t);
            return v ? textValue(*v) : L"";
        }
        catch (const Error &)
        {
            ++result.warnings;
            return {};
        }
    }
    std::vector<std::wstring> children(const Target &t)
    {
        if (cancel)
            return {};
        try
        {
            return subkeys(t);
        }
        catch (const Error &)
        {
            ++result.warnings;
            return {};
        }
    }
    std::vector<std::pair<std::wstring, Value>> vals(const Target &t)
    {
        if (cancel)
            return {};
        try
        {
            return values(t);
        }
        catch (const Error &)
        {
            ++result.warnings;
            return {};
        }
    }
    std::wstring merged(DWORD view, const std::wstring &path, const std::wstring &name = L"")
    {
        // HKCU overrides HKLM, including an explicitly empty value.
        for (auto hive : {Hive::User, Hive::Machine})
            try
            {
                auto v = readValue({hive, view, roots.classes + L"\\" + path, name, false});
                if (v)
                    return textValue(*v);
            }
            catch (const Error &)
            {
                ++result.warnings;
            }
        return {};
    }
    std::wstring command(DWORD view, const std::wstring &prog)
    {
        auto id = std::to_wstring(view) + L":" + lower(prog);
        auto i = commandCache.find(id);
        if (i != commandCache.end())
            return i->second;
        auto cmd = merged(view, prog + L"\\shell\\open\\command");
        if (cmd.empty())
        {
            auto verb = merged(view, prog + L"\\shell");
            if (!verb.empty() && verb.find(L'\\') == verb.npos)
                cmd = merged(view, prog + L"\\shell\\" + verb + L"\\command");
        }
        commandCache[id] = cmd;
        return cmd;
    }
    std::wstring source(const Target &t)
    {
        auto label = std::wstring(t.hive == Hive::User ? L"HKCU" : L"HKLM");
        bool shared = sharedTarget(t, roots);
        return label + (shared ? L" · shared" : (t.view == KEY_WOW64_32KEY ? L" · 32" : L" · 64"));
    }
    std::wstring resolve(const std::wstring &cmd)
    {
        try
        {
            return resolveCommand(cmd);
        }
        catch (const Error &)
        {
            ++result.warnings;
            return {};
        }
    }
    bool system(const std::wstring &p)
    {
        auto s = lower(p);
        return !s.empty() && s.starts_with(windows + L"\\");
    }
    void protect(Entry &e, const std::wstring &prog)
    {
        auto base = lower(std::filesystem::path(e.executable).filename().wstring());
        if (system(e.executable) || defaults.contains(lower(prog)) ||
            (!e.executable.empty() && defaultPaths.contains(lower(e.executable))))
        {
            e.protectedItem = true;
            e.reason =
                L"Windows component or current default application / Windows 구성요소 또는 현재 기본 앱";
        }
        if (base == L"rundll32.exe" || base == L"dllhost.exe" || base == L"explorer.exe" ||
            base == L"cmd.exe" || base == L"powershell.exe")
        {
            e.protectedItem = true;
            e.reason = L"Shared Windows command host / 공유 Windows 명령 호스트";
        }
    }
    void addOpen(const Target &original, const std::wstring &prog, const std::wstring &extension,
                 const std::wstring &app = L"")
    {
        if (cancel)
            return;
        auto t = canonicalTarget(original, roots);
        auto cmd = command(original.view, prog);
        auto exe = resolve(cmd);
        // Only group references which resolve to the same full command and source view.
        auto id = std::to_wstring(static_cast<DWORD>(t.hive)) + L":" + std::to_wstring(t.view) + L":" +
                  (exe.empty() ? lower(prog) : lower(exe) + L"|" + lower(cmd));
        Entry *e;
        if (auto i = groups.find(id); i != groups.end())
            e = &result.entries[i->second];
        else
        {
            Entry row;
            row.kind = Kind::OpenWith;
            row.command = cmd;
            row.executable = exe;
            row.identity = id;
            row.name = exe.empty() ? (app.empty() ? prog : app) : std::filesystem::path(exe).stem().wstring();
            row.source = source(t);
            groups[id] = result.entries.size();
            result.entries.push_back(std::move(row));
            e = &result.entries.back();
        }
        e->progids.insert(prog);
        if (!extension.empty())
            e->extensions.insert(extension);
        if (std::find(e->targets.begin(), e->targets.end(), t) == e->targets.end())
            e->targets.push_back(t);
        protect(*e, prog);
    }
    void associations(Hive hive, DWORD view, const std::wstring &parent, const std::wstring &ext,
                      bool fileExts)
    {
        Target p{hive, view, parent + L"\\OpenWithProgids", L"", false};
        for (const auto &[name, v] : vals(p))
            if (!name.empty())
            {
                auto t = p;
                t.name = name;
                addOpen(t, name, ext);
            }
        p.path = parent + L"\\OpenWithList";
        if (fileExts)
        {
            for (const auto &[name, v] : vals(p))
                if (name.size() == 1 &&
                    ((name[0] >= L'a' && name[0] <= L'z') || (name[0] >= L'A' && name[0] <= L'Z')))
                {
                    auto app = textValue(v);
                    if (!app.empty() && app.find_first_of(L"\\/") == app.npos)
                    {
                        auto t = p;
                        t.name = name;
                        addOpen(t, L"Applications\\" + app, ext, app);
                    }
                }
        }
        else
        {
            for (const auto &app : children(p))
                if (app.find_first_of(L"\\/") == app.npos)
                {
                    auto t = p;
                    t.path += L"\\" + app;
                    t.tree = true;
                    addOpen(t, L"Applications\\" + app, ext, app);
                }
        }
    }
    void menus(Hive hive, DWORD view, const std::wstring &owner, const std::set<std::wstring> &extensions)
    {
        Target shell{hive, view, roots.classes + L"\\" + owner + L"\\shell", L"", true};
        for (const auto &verb : children(shell))
        {
            if (cancel)
                return;
            auto t = shell;
            t.path += L"\\" + verb;
            Entry e;
            e.kind = Kind::Verb;
            e.targets.push_back(t);
            e.extensions = extensions;
            e.progids.insert(owner);
            e.name = get({hive, view, t.path, L"MUIVerb", false});
            if (e.name.empty() || e.name[0] == L'@')
                e.name = get({hive, view, t.path, L"", false});
            if (e.name.empty() || e.name[0] == L'@')
                e.name = verb;
            e.command = get({hive, view, t.path + L"\\command", L"", false});
            e.executable = resolve(e.command);
            e.source = source(t);
            e.identity = describe(t);
            auto modern = get({hive, view, t.path, L"ExplorerCommandHandler", false});
            auto delegate = get({hive, view, t.path + L"\\command", L"DelegateExecute", false});
            if (!modern.empty() || !delegate.empty() ||
                !get({hive, view, t.path, L"SubCommands", false}).empty())
            {
                e.kind = Kind::Modern;
                e.unsupported = true;
                e.reason = L"ExplorerCommand / DelegateExecute / cascading menu: inspection only / 조회 전용";
                if (e.command.empty())
                    e.command = !modern.empty() ? modern : delegate;
            }
            if (lower(verb) == L"open" || lower(verb) == L"openas" || lower(verb) == L"runas" ||
                lower(verb) == L"explore" || defaults.contains(lower(owner)))
            {
                e.protectedItem = true;
                e.reason = L"Default or core shell verb / 기본 또는 핵심 셸 동작";
            }
            protect(e, owner);
            result.entries.push_back(std::move(e));
        }
        Target handlers{hive, view, roots.classes + L"\\" + owner + L"\\shellex\\ContextMenuHandlers", L"",
                        true};
        for (const auto &name : children(handlers))
        {
            if (cancel)
                return;
            auto t = handlers;
            t.path += L"\\" + name;
            Entry e;
            e.kind = Kind::Extension;
            e.name = name;
            e.targets.push_back(t);
            e.extensions = extensions;
            e.progids.insert(owner);
            e.identity = describe(t);
            auto clsid = get({hive, view, t.path, L"", false});
            if (clsid.empty())
                clsid = name;
            auto server = merged(view, L"CLSID\\" + clsid + L"\\InprocServer32");
            if (server.empty())
                server = merged(view, L"CLSID\\" + clsid + L"\\LocalServer32");
            e.command = server;
            e.source = source(t);
            wchar_t expanded[32768];
            if (!server.empty() && ExpandEnvironmentStringsW(server.c_str(), expanded, 32768) > 0)
            {
                std::wstring p = trim(expanded);
                if (p.size() > 1 && p.front() == L'"' && p.back() == L'"')
                    p = p.substr(1, p.size() - 2);
                if (!PathIsRelativeW(p.c_str()) &&
                    lower(std::filesystem::path(p).extension().wstring()) == L".dll")
                    e.executable = p;
                else
                    e.executable = resolve(server);
            }
            e.reason = L"Only the ContextMenuHandlers reference is removed; shared CLSID is preserved / "
                       L"핸들러 참조만 삭제, CLSID 보존";
            protect(e, owner);
            result.entries.push_back(std::move(e));
        }
    }

  public:
    Scanner(const Roots &r, const std::atomic_bool &c) : roots(r), cancel(c)
    {
        wchar_t w[MAX_PATH];
        GetWindowsDirectoryW(w, MAX_PATH);
        windows = lower(w);
    }
    ScanResult run()
    {
        std::map<DWORD, std::map<std::wstring, std::set<std::wstring>>> owners;
        for (DWORD view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
        {
            for (const auto &ext : children({Hive::User, view, roots.fileExts, L"", true}))
                if (ext.starts_with(L"."))
                {
                    auto prog = get(
                        {Hive::User, view, roots.fileExts + L"\\" + ext + L"\\UserChoice", L"ProgId", false});
                    if (prog.empty())
                        prog = merged(view, ext);
                    if (!prog.empty())
                        defaults.insert(lower(prog));
                }
            for (auto hive : {Hive::User, Hive::Machine})
                for (const auto &name : children({hive, view, roots.classes, L"", true}))
                {
                    if (cancel)
                        break;
                    if (name.starts_with(L"."))
                    {
                        auto user = get({Hive::User, view, roots.fileExts + L"\\" + name + L"\\UserChoice",
                                         L"ProgId", false});
                        auto prog = merged(view, name);
                        if (user.empty() && !prog.empty())
                            defaults.insert(lower(prog));
                        if (!prog.empty())
                            owners[view][prog].insert(name);
                        for (const auto &[p, v] :
                             vals({hive, view, roots.classes + L"\\" + name + L"\\OpenWithProgids", L"",
                                   false}))
                            if (!p.empty())
                                owners[view][p].insert(name);
                        owners[view][name].insert(name);
                    }
                    else if (name != L"CLSID" && name != L"Interface" && name != L"TypeLib" &&
                             name != L"AppID" && name != L"Applications")
                        owners[view][name];
                }
            for (const auto &p : defaults)
            {
                auto exe = resolve(command(view, p));
                if (!exe.empty())
                    defaultPaths.insert(lower(exe));
            }
        }
        for (DWORD view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
        {
            for (const auto &ext : children({Hive::User, view, roots.fileExts, L"", true}))
                if (ext.starts_with(L"."))
                    associations(Hive::User, view, roots.fileExts + L"\\" + ext, ext, true);
            for (auto hive : {Hive::User, Hive::Machine})
            {
                for (const auto &name : children({hive, view, roots.classes, L"", true}))
                    if (name.starts_with(L"."))
                        associations(hive, view, roots.classes + L"\\" + name, name, false);
                Target apps{hive, view, roots.classes + L"\\Applications", L"", true};
                for (const auto &app : children(apps))
                {
                    auto t = apps;
                    t.path += L"\\" + app;
                    // Do not delete unrelated Applications subkeys with no open command.
                    if (!get({hive, view, t.path + L"\\shell\\open\\command", L"", false}).empty())
                    {
                        addOpen(t, L"Applications\\" + app, L"", app);
                        auto id =
                            std::to_wstring(static_cast<DWORD>(hive)) + L":" + std::to_wstring(view) + L":";
                        for (const auto &[ext, v] :
                             vals({hive, view, t.path + L"\\SupportedTypes", L"", false}))
                            if (ext.starts_with(L"."))
                                addOpen(t, L"Applications\\" + app, ext, app);
                    }
                    owners[view][L"Applications\\" + app];
                }
                for (const auto &owner : {L"*", L"AllFileSystemObjects", L"Directory",
                                          L"Directory\\Background", L"Folder", L"Drive"})
                    owners[view][owner];
                for (const auto &[owner, exts] : owners[view])
                {
                    if (cancel)
                        break;
                    menus(hive, view, owner, exts);
                }
                // RegisteredApplications capabilities can also advertise Open With candidates.
                for (const auto &[app, v] : vals({hive, view, roots.registeredApps, L"", false}))
                {
                    auto cap = textValue(v);
                    if (cap.empty() || !lower(cap).starts_with(L"software\\"))
                        continue;
                    auto path = cap + L"\\FileAssociations";
                    for (const auto &[ext, p] : vals({hive, view, path, L"", false}))
                    {
                        auto prog = textValue(p);
                        if (ext.starts_with(L".") && !prog.empty())
                            addOpen({hive, view, path, ext, false}, prog, ext, app);
                    }
                }
                // Packaged COM classes are displayed, never offered for deletion.
                Target packaged{hive, view, roots.classes + L"\\PackagedCom\\Package", L"", true};
                for (const auto &package : children(packaged))
                {
                    auto p = packaged;
                    p.path += L"\\" + package + L"\\Class";
                    for (const auto &clsid : children(p))
                    {
                        auto t = p;
                        t.path += L"\\" + clsid;
                        Entry e;
                        e.name = package;
                        e.kind = Kind::Modern;
                        e.unsupported = true;
                        e.identity = describe(t);
                        e.source = hive == Hive::User ? L"HKCU" : L"HKLM";
                        e.source += view == KEY_WOW64_32KEY ? L" · 32" : L" · 64";
                        e.targets.push_back(t);
                        e.command = clsid;
                        e.reason = L"Packaged COM candidate; context-menu participation is not guaranteed. "
                                   L"Inspection only / 패키지 COM 후보, 메뉴 여부 미확인, 조회 전용";
                        result.entries.push_back(std::move(e));
                    }
                }
            }
        }
        // Shared registry branches can be returned in both WOW64 views. Compare physical content.
        std::set<std::wstring> seen;
        std::vector<Entry> unique;
        for (auto &e : result.entries)
        {
            if (cancel)
                break;
            std::wstring key = std::to_wstring(static_cast<int>(e.kind)) + L"|" + e.command;
            for (const auto &t : e.targets)
            {
                key +=
                    L"|" + std::to_wstring(static_cast<int>(t.hive)) + lower(t.path) + L":" + lower(t.name);
                if (!source(t).ends_with(L"shared"))
                    key += L":" + std::to_wstring(t.view);
            }
            if (!seen.insert(key).second)
                continue;
            if (e.protectedItem)
                e.status = Status::Protected;
            else if (e.executable.empty() || e.unsupported)
                e.status = Status::Review;
            else
            {
                DWORD attr = GetFileAttributesW(e.executable.c_str());
                e.status =
                    attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)
                        ? Status::Healthy
                        : (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND
                               ? Status::Missing
                               : Status::Review);
            }
            unique.push_back(std::move(e));
        }
        std::map<std::wstring, std::set<std::wstring>> paths;
        for (const auto &e : unique)
            if (e.kind == Kind::OpenWith && !e.executable.empty())
                paths[lower(e.name)].insert(lower(e.executable));
        for (auto &e : unique)
            if (e.status == Status::Healthy && e.kind == Kind::OpenWith && paths[lower(e.name)].size() > 1)
                e.status = Status::Duplicate;
        std::sort(unique.begin(), unique.end(),
                  [](const Entry &a, const Entry &b) { return lower(a.name) < lower(b.name); });
        result.entries = std::move(unique);
        result.canceled = cancel;
        return std::move(result);
    }
};
} // namespace
ScanResult scan(const Roots &roots, const std::atomic_bool &cancel)
{
    return Scanner(roots, cancel).run();
}
} // namespace st

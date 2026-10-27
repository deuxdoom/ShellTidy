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
    std::map<std::wstring, FileState> states;
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> versions;
    std::set<std::wstring> defaults, defaultPaths, uncertain, checkedChoices, blockedUser, blockedMachine;
    bool defaultsUncertain = false; // Only an unreadable association root protects every entry.
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
    std::vector<std::wstring> children(const Target &t, bool defaultMetadata = false)
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
            if (defaultMetadata)
                defaultsUncertain = true;
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
    // Windows 11 records the effective choice in UserChoiceLatest\ProgId while UserChoice can keep an
    // older ProgID. Every recorded choice and the shell's own answer are protected together.
    // The shell's answer is also asked for extensions without per-user choice keys: packaged apps can be
    // the effective default there even though the class default names another ProgID.
    void currentDefaults(DWORD view, const std::wstring &name, const std::wstring &root, bool extension)
    {
        if (!checkedChoices.insert(std::to_wstring(view) + L":" + lower(root) + L"\\" + lower(name)).second)
            return;
        bool recorded = false, known = false;
        try
        {
            for (const auto &key : {L"\\UserChoice", L"\\UserChoiceLatest\\ProgId"})
                if (auto v = readValue({Hive::User, view, root + L"\\" + name + key, L"ProgId", false}))
                {
                    recorded = true;
                    auto prog = textValue(*v);
                    if (!prog.empty())
                    {
                        known = true;
                        defaults.insert(lower(prog));
                    }
                }
            if (!roots.isolated && view == KEY_WOW64_64KEY)
            {
                wchar_t prog[512]{};
                DWORD n = 512;
                if (SUCCEEDED(
                        AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID, name.c_str(), nullptr, prog, &n)) &&
                    prog[0])
                {
                    known = true;
                    defaults.insert(lower(prog));
                }
            }
            if (recorded || !extension)
            {
                if (recorded && !known)
                    uncertain.insert(lower(name));
                return;
            }
            for (auto hive : {Hive::User, Hive::Machine})
                if (auto v = readValue({hive, view, roots.classes + L"\\" + name, L"", false}))
                {
                    if ((v->type != REG_SZ && v->type != REG_EXPAND_SZ) ||
                        (textValue(*v).empty() && !v->data.empty() && *v != stringValue(L"", v->type)))
                        uncertain.insert(lower(name));
                    if (auto prog = textValue(*v); !prog.empty())
                        defaults.insert(lower(prog));
                    return;
                }
        }
        catch (const Error &)
        {
            ++result.warnings;
            uncertain.insert(lower(name));
        }
    }
    FileState state(const std::wstring &path)
    {
        auto key = lower(path);
        if (auto i = states.find(key); i != states.end())
            return i->second;
        return states[key] = fileState(path);
    }
    bool present(const Target &t)
    {
        try
        {
            return readValue(t).has_value();
        }
        catch (const Error &)
        {
            ++result.warnings;
            return false;
        }
    }
    bool registered(DWORD view, const std::wstring &path)
    {
        for (auto hive : {Hive::User, Hive::Machine})
            try
            {
                if (openKey({hive, view, roots.classes + L"\\" + path, L"", true}).h)
                    return true;
            }
            catch (const Error &)
            {
                ++result.warnings;
                return true; // An unreadable registration is not proof that it is absent.
            }
        return false;
    }
    // Shell Extensions\Blocked applies to every registration of the CLSID (the ShellExView method).
    void blockSwitch(Entry &e, const std::wstring &clsid)
    {
        if (!isGuid(clsid))
            return;
        e.switchable = true;
        e.switches = {{Hive::User, KEY_WOW64_64KEY, roots.blocked, clsid, false},
                      {Hive::Machine, KEY_WOW64_64KEY, roots.blocked, clsid, false}};
        e.disabled = blockedUser.contains(lower(clsid)) || blockedMachine.contains(lower(clsid));
    }
    bool has(const Target &t)
    {
        try
        {
            return readValue(t).has_value();
        }
        catch (const Error &)
        {
            ++result.warnings;
            return true; // A feature whose metadata cannot be read is inspection-only.
        }
    }
    bool delegated(DWORD view, const std::wstring &prog)
    {
        for (auto hive : {Hive::User, Hive::Machine})
            if (has({hive, view, roots.classes + L"\\" + prog + L"\\shell\\open\\command", L"DelegateExecute",
                     false}) ||
                has({hive, view, roots.classes + L"\\" + prog + L"\\shell\\open", L"ExplorerCommandHandler",
                     false}))
                return true;
        return false;
    }
    bool delegatedMenus(Hive hive, DWORD view, const std::wstring &appPath)
    {
        try
        {
            for (const auto &verb : subkeys({hive, view, appPath + L"\\shell", L"", true}))
            {
                auto path = appPath + L"\\shell\\" + verb;
                if (has({hive, view, path, L"ExplorerCommandHandler", false}) ||
                    has({hive, view, path, L"SubCommands", false}) ||
                    has({hive, view, path, L"ExtendedSubCommandsKey", false}) ||
                    has({hive, view, path + L"\\command", L"DelegateExecute", false}))
                    return true;
            }
        }
        catch (const Error &)
        {
            ++result.warnings;
            return true;
        }
        return false;
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
        // An Applications key is removed from its physical hive, even when HKCU shadows HKLM.
        if (t.tree && lower(t.path) == lower(roots.classes + L"\\" + prog) &&
            lower(prog).starts_with(L"applications\\"))
            cmd = get({t.hive, original.view, t.path + L"\\shell\\open\\command", L"", false});
        auto exe = resolve(cmd);
        bool orphan = false;
        if (cmd.empty() && !registered(original.view, prog))
        {
            // Explorer can still locate an unregistered application through App Paths.
            if (lower(prog).starts_with(L"applications\\"))
                exe = resolve(prog.substr(13));
            orphan = exe.empty();
        }
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
            row.orphan = orphan;
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
        if (delegated(original.view, prog) ||
            (t.tree && lower(t.path) == lower(roots.classes + L"\\" + prog) &&
             lower(prog).starts_with(L"applications\\") && delegatedMenus(t.hive, original.view, t.path)))
        {
            e->unsupported = true;
            e->reason = L"Delegated application command: inspection only / 위임 실행 등록: 조회 전용";
        }
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
            e.targets.push_back(canonicalTarget(t, roots));
            e.extensions = extensions;
            e.progids.insert(owner);
            e.name = displayText(get({hive, view, t.path, L"MUIVerb", false}));
            if (e.name.empty())
                e.name = displayText(get({hive, view, t.path, L"", false}));
            if (e.name.empty())
                e.name = verb;
            e.command = get({hive, view, t.path + L"\\command", L"", false});
            e.executable = resolve(e.command);
            e.source = source(t);
            e.identity = describe(t);
            // LegacyDisable hides only this verb and is removed again to enable it (the ShellMenuView
            // method).
            e.switches = {canonicalTarget({hive, view, t.path, L"LegacyDisable", false}, roots)};
            e.switchable = true;
            e.disabled = present(e.switches.front());
            auto modern = get({hive, view, t.path, L"ExplorerCommandHandler", false});
            auto delegate = get({hive, view, t.path + L"\\command", L"DelegateExecute", false});
            if (has({hive, view, t.path, L"ExplorerCommandHandler", false}) ||
                has({hive, view, t.path + L"\\command", L"DelegateExecute", false}) ||
                has({hive, view, t.path, L"SubCommands", false}) ||
                has({hive, view, t.path, L"ExtendedSubCommandsKey", false}))
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
                e.switchable = false;
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
            e.targets.push_back(canonicalTarget(t, roots));
            e.extensions = extensions;
            e.progids.insert(owner);
            e.identity = describe(t);
            auto clsid = get({hive, view, t.path, L"", false});
            if (clsid.empty())
                clsid = name;
            if (auto friendly = displayText(merged(view, L"CLSID\\" + clsid)); !friendly.empty())
                e.name = friendly;
            if (isGuid(clsid) && !registered(view, L"CLSID\\" + clsid))
                e.orphan = true;
            blockSwitch(e, clsid);
            auto server = merged(view, L"CLSID\\" + clsid + L"\\InprocServer32");
            if (server.empty())
                server = merged(view, L"CLSID\\" + clsid + L"\\LocalServer32");
            e.command = server;
            e.source = source(t);
            auto expanded = expandEnvironment(server);
            if (!expanded.empty())
            {
                std::wstring p = trim(expanded);
                if (p.size() > 1 && p.front() == L'"' && p.back() == L'"')
                    p = p.substr(1, p.size() - 2);
                if (std::filesystem::path(p).is_absolute() &&
                    lower(std::filesystem::path(p).extension().wstring()) == L".dll")
                    e.executable = std::filesystem::path(p).lexically_normal().wstring();
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
        // HKCU blocks apply to this user and HKLM blocks to every user; both disable the CLSID here.
        for (const auto &[name, v] : vals({Hive::User, KEY_WOW64_64KEY, roots.blocked, L"", false}))
            blockedUser.insert(lower(name));
        for (const auto &[name, v] : vals({Hive::Machine, KEY_WOW64_64KEY, roots.blocked, L"", false}))
            blockedMachine.insert(lower(name));
        for (DWORD view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
        {
            for (const auto &ext : children({Hive::User, view, roots.fileExts, L"", true}, true))
                if (ext.starts_with(L"."))
                    currentDefaults(view, ext, roots.fileExts, true);
            for (const auto &protocol : children({Hive::User, view, roots.urlAssociations, L"", true}))
                currentDefaults(view, protocol, roots.urlAssociations, false);
            for (auto hive : {Hive::User, Hive::Machine})
                for (const auto &name : children({hive, view, roots.classes, L"", true}, true))
                {
                    if (cancel)
                        break;
                    if (name.starts_with(L"."))
                    {
                        currentDefaults(view, name, roots.fileExts, true);
                        auto prog = merged(view, name);
                        if (!prog.empty())
                            owners[view][prog].insert(name);
                        for (const auto &[p, v] :
                             vals({hive, view, roots.classes + L"\\" + name + L"\\OpenWithProgids", L"",
                                   false}))
                            if (!p.empty())
                                owners[view][p].insert(name);
                        owners[view][name].insert(name);
                        auto perceived = merged(view, name, L"PerceivedType");
                        if (!perceived.empty() && perceived.find_first_of(L"\\/") == perceived.npos)
                            owners[view][L"SystemFileAssociations\\" + perceived].insert(name);
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
                for (const auto &type :
                     children({hive, view, roots.classes + L"\\SystemFileAssociations", L"", true}))
                {
                    auto &exts = owners[view][L"SystemFileAssociations\\" + type];
                    if (type.starts_with(L"."))
                        exts.insert(type);
                }
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
                        e.packaged = true;
                        blockSwitch(e, clsid);
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
        // An unverifiable default protects only the entries related to that extension.
        for (auto &e : result.entries)
        {
            bool unsure = defaultsUncertain;
            for (const auto &x : e.extensions)
                unsure = unsure || uncertain.contains(lower(x));
            if (unsure)
            {
                e.protectedItem = true;
                e.switchable = false;
                e.reason = L"Default application could not be verified / 기본 앱 확인 실패로 삭제 보호";
            }
        }
        // Shared registry branches can be returned in both WOW64 views. Compare physical content.
        std::set<std::wstring> protectedTargets, unsupportedTargets;
        for (const auto &e : result.entries)
            for (const auto &t : e.targets)
            {
                if (e.protectedItem)
                    protectedTargets.insert(targetId(t, roots));
                if (e.unsupported)
                    unsupportedTargets.insert(targetId(t, roots));
            }
        std::set<std::wstring> seen;
        std::vector<Entry> unique;
        for (auto &e : result.entries)
        {
            if (cancel)
                break;
            for (const auto &t : e.targets)
            {
                if (protectedTargets.contains(targetId(t, roots)))
                {
                    e.protectedItem = true;
                    if (e.reason.empty())
                        e.reason = L"Protected in another registry view / 다른 레지스트리 뷰의 보호 등록";
                }
                if (unsupportedTargets.contains(targetId(t, roots)))
                    e.unsupported = true;
            }
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
            else if (e.unsupported)
                e.status = Status::Review;
            else if (e.orphan)
            {
                e.status = Status::Orphan;
                if (e.reason.empty() || e.kind == Kind::Extension)
                    e.reason = L"The referenced ProgID, application or CLSID is not registered / "
                               L"참조 대상 ProgID·앱·CLSID 등록 없음";
            }
            else if (e.executable.empty())
                e.status = Status::Review;
            else
                switch (state(e.executable))
                {
                case FileState::Present:
                    e.status = Status::Healthy;
                    break;
                case FileState::Missing:
                    e.status = Status::Missing;
                    break;
                case FileState::Folder:
                    e.status = Status::Review;
                    break;
                default:
                    e.status = Status::Review;
                    if (e.reason.empty())
                        e.reason = L"Executable location is not reachable (drive, network or access) / "
                                   L"실행 파일 위치에 접근할 수 없음 (드라이브·네트워크·권한)";
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
        reconcileViews(unique, roots);
        for (auto &e : unique)
        {
            if (cancel)
                break;
            if (e.executable.empty() || state(e.executable) != FileState::Present)
                continue;
            auto key = lower(e.executable);
            auto i = versions.find(key);
            if (i == versions.end())
                i = versions.emplace(key, versionText(e.executable)).first;
            e.description = i->second.first;
            e.company = i->second.second;
        }
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
void reconcileViews(std::vector<Entry> &entries, const Roots &roots)
{
    // A shared handler key can point to a 32-bit DLL that exists and a 64-bit DLL that does not.
    // Do not present the shared registration as a missing-file cleanup candidate.
    std::set<std::wstring> healthy;
    for (const auto &e : entries)
        if (e.status == Status::Healthy || e.status == Status::Duplicate)
            for (const auto &t : e.targets)
                healthy.insert(targetId(t, roots));
    for (auto &e : entries)
        if (e.status == Status::Missing || e.status == Status::Orphan)
            for (const auto &t : e.targets)
                if (healthy.contains(targetId(t, roots)))
                {
                    e.status = Status::Review;
                    e.reason = L"Healthy in another 32/64-bit registry view / 다른 32/64비트 뷰에서는 정상";
                    break;
                }
}
} // namespace st

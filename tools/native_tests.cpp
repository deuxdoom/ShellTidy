#include "../src/shelltidy.h"
#include <fstream>
#include <algorithm>
#include <aclapi.h>

namespace st
{
namespace
{
class Tests
{
    std::ofstream output;
    unsigned passed = 0, failed = 0;
    std::wstring base, dir, exe;
    Roots roots;
    std::atomic_bool cancel = false;
    Target target(const std::wstring &path, const std::wstring &name = L"", bool tree = false)
    {
        return {Hive::User, KEY_WOW64_64KEY, path, name, tree};
    }
    void put(const std::wstring &path, const std::wstring &name, const std::wstring &text,
             DWORD type = REG_SZ)
    {
        writeValue(target(path, name), stringValue(text, type));
    }
    void command(const std::wstring &prog, const std::wstring &path)
    {
        put(roots.classes + L"\\" + prog + L"\\shell\\open\\command", L"", L"\"" + path + L"\" \"%1\"");
    }
    void ref(const std::wstring &ext, const std::wstring &prog)
    {
        Value v;
        v.type = REG_NONE;
        writeValue(target(roots.fileExts + L"\\" + ext + L"\\OpenWithProgids", prog), v);
    }
    Entry find(const ScanResult &s, const std::wstring &prog, Kind kind = Kind::OpenWith)
    {
        for (const auto &e : s.entries)
            if (e.kind == kind && e.progids.contains(prog))
                return e;
        throw Error("Test entry not found", ERROR_NOT_FOUND);
    }
    void check(bool condition, const char *label)
    {
        output << (condition ? "PASS " : "FAIL ") << label << '\n';
        output.flush();
        if (condition)
            ++passed;
        else
            ++failed;
    }
    template <class F> void rejects(F f, const char *label)
    {
        bool rejected = false;
        try
        {
            f();
        }
        catch (...)
        {
            rejected = true;
        }
        check(rejected, label);
    }
    void setup()
    {
        wchar_t self[32768];
        GetModuleFileNameW(nullptr, self, 32768);
        exe = self;
        // Normalize separators: a backup override such as "$PWD/build/x" must not leak '/' into fixture
        // paths that resolveCommand returns in canonical backslash form.
        dir = (std::filesystem::path(backupDirectory()).parent_path() /
               (L"Fixtures-" + std::to_wstring(GetCurrentProcessId())))
                  .lexically_normal()
                  .wstring();
        std::filesystem::create_directories(dir);
        base = L"Software\\ShellTidy\\Tests\\Run-" + std::to_wstring(GetCurrentProcessId());
        roots = {base + L"\\Classes",
                 base + L"\\FileExts",
                 true,
                 base + L"\\RegisteredApplications",
                 base + L"\\UrlAssociations",
                 base + L"\\Shell Extensions\\Blocked"};
        auto pic = dir + L"\\Current\\Picvium.exe";
        std::filesystem::create_directories(std::filesystem::path(pic).parent_path());
        std::filesystem::copy_file(exe, pic, std::filesystem::copy_options::overwrite_existing);
        for (const auto &ext : {L".jpg", L".png", L".webp", L".pdf", L".psd"})
        {
            auto current = std::wstring(L"Picvium.Image") + ext,
                 old = std::wstring(L"Picvium.Installed.Image") + ext;
            command(current, pic);
            command(old, dir + L"\\Old\\Picvium.exe");
            ref(ext, current);
            ref(ext, old);
        }
        put(roots.fileExts + L"\\.jpg\\UserChoice", L"ProgId", L"Picvium.Image.jpg");
        command(L"Applications\\marktext.exe", dir + L"\\MarkText\\marktext.exe");
        command(L"Applications\\Obsidian.exe", dir + L"\\Obsidian\\Obsidian.exe");
        command(L"Other.Markdown", exe);
        for (const auto &ext : {L".md", L".markdown", L".novel-extension-123"})
        {
            ref(ext, L"Applications\\marktext.exe");
            ref(ext, L"Applications\\Obsidian.exe");
            ref(ext, L"Other.Markdown");
            put(roots.classes + L"\\Applications\\marktext.exe\\SupportedTypes", ext, L"");
        }
        put(roots.fileExts + L"\\.md\\OpenWithList", L"a", L"marktext.exe");
        put(roots.fileExts + L"\\.md\\OpenWithList", L"b", L"Obsidian.exe");
        put(roots.fileExts + L"\\.md\\OpenWithList", L"c", L"Other.exe");
        put(roots.fileExts + L"\\.md\\OpenWithList", L"MRUList", L"abc");
        put(roots.classes + L"\\*\\shell\\ShellTidyFixture\\command", L"", L"\"" + exe + L"\" \"%1\"");
        put(roots.classes + L"\\Directory\\Background\\shell\\ModernFixture", L"ExplorerCommandHandler",
            L"{00000000-0000-0000-0000-000000000123}");
        put(roots.classes + L"\\*\\shellex\\ContextMenuHandlers\\Fixture", L"",
            L"{00000000-0000-0000-0000-000000000456}");
        put(roots.classes + L"\\CLSID\\{00000000-0000-0000-0000-000000000456}\\InprocServer32", L"",
            dir + L"\\missing.dll");
        command(L"SharedCapabilities.App", dir + L"\\SharedOld.exe");
        put(roots.registeredApps, L"Shared fixture", base + L"\\Capabilities");
        for (int i = 0; i < 65; ++i)
            put(base + L"\\Capabilities\\FileAssociations", L".st" + std::to_wstring(i),
                L"SharedCapabilities.App");
    }
    void reviewTests()
    {
        auto nested = dir + L"\\folder.exe.backup\\Missing.exe";
        check(resolveCommand(nested + L" %1") == nested,
              "an .exe substring in a directory does not truncate the executable path");
        auto com = dir + L"\\Missing.com";
        check(resolveCommand(com + L" --target Other.exe") == com,
              "a .com executable is not confused with an .exe argument");
        check(resolveCommand(L"\"" + exe + L"\"unexpected").empty(),
              "text immediately after a closing executable quote is rejected");
        check(resolveCommand(L"F:relative.exe").empty() && resolveCommand(L"\\root-relative.exe").empty() &&
                  resolveCommand(L".\\relative.exe").empty(),
              "paths depending on the current drive or directory remain unresolved");
        auto unicode = dir + L"\\한글 폴더\\없는 앱.exe";
        check(resolveCommand(L"\"" + unicode + L"\" %1") == unicode,
              "a quoted missing Unicode path can be diagnosed without launching it");
        auto variable = L"SHELLTIDY_TEST_EXPANSION_" + std::to_wstring(GetCurrentProcessId());
        auto large = std::wstring(20000, L'x');
        if (!SetEnvironmentVariableW(variable.c_str(), large.c_str()))
            throw Error("Test environment setup");
        check(expandEnvironment(L"%" + variable + L"%%" + variable + L"%").empty(),
              "oversized environment expansion is rejected before reading a partial buffer");
        SetEnvironmentVariableW(variable.c_str(), nullptr);
        auto emptyTarget = target(base + L"\\ReviewRaw", L"empty");
        Value empty;
        writeValue(emptyTarget, empty);
        check(readValue(emptyTarget) == std::optional<Value>(empty),
              "zero-byte REG_NONE values retain their exact type and bytes");

        command(L"Review.Stale", dir + L"\\OldReview.exe");
        ref(L".review-stale", L"Review.Stale");
        auto selected = find(scan(roots, cancel), L"Review.Stale");
        command(L"Review.Stale", dir + L"\\NewReview.exe");
        rejects([&] { prepare({selected}, roots); },
                "a command changed since selection cannot be prepared for deletion");
        selected = find(scan(roots, cancel), L"Review.Stale");
        auto stale = prepare({selected}, roots);
        command(L"Review.Stale", dir + L"\\ThirdReview.exe");
        auto out = apply(stale, cancel, roots);
        check(out.failed == 1 && out.success == 0 && !out.registryChanged && exists(selected.targets.front()),
              "a referenced command changed after backup is preserved during apply");

        command(L"Review.Once", dir + L"\\Once.exe");
        ref(L".review-once", L"Review.Once");
        selected = find(scan(roots, cancel), L"Review.Once");
        auto once = prepare({selected}, roots);
        saveJournal(once);
        auto saved = loadJournal(once.file);
        check(!saved.items.front().context.empty() &&
                  saved.items.front().context == once.items.front().context,
              "backup v2 preserves the scanned command identity");
        out = apply(once, cancel, roots);
        check(out.success == 1 && out.registryChanged,
              "actual registry mutations set the change notification flag");
        out = apply(once, cancel, roots);
        check(out.success == 0 && out.failed == 0 && !out.registryChanged,
              "applying a completed journal again does not delete twice");
        restore(once, cancel, roots);
        out = apply(once, cancel, roots);
        check(out.success == 0 && exists(selected.targets.front()),
              "a restored journal cannot remove restored registrations on repeated apply");

        // A real version-1 fixture keeps compatibility independent of the current serializer.
        Bytes legacy;
        auto u = [&](DWORD n)
        {
            for (int i = 0; i < 4; ++i)
                legacy.push_back(static_cast<BYTE>(n >> (8 * i)));
        };
        auto str = [&](const std::wstring &s)
        {
            u(static_cast<DWORD>(s.size()));
            auto p = reinterpret_cast<const BYTE *>(s.data());
            legacy.insert(legacy.end(), p, p + s.size() * 2);
        };
        auto legacyTarget = target(roots.fileExts + L"\\.review-legacy\\OpenWithProgids", L"Review.Legacy");
        u(0x42545453);
        u(1);
        u(1);
        u(static_cast<DWORD>(Hive::User));
        u(KEY_WOW64_64KEY);
        str(legacyTarget.path);
        str(legacyTarget.name);
        u(0);
        u(2);
        u(0);
        u(0);
        u(1);
        str(L"");
        str(legacyTarget.name);
        u(REG_NONE);
        u(0);
        DWORD hash = 2166136261u;
        for (auto b : legacy)
            hash = (hash ^ b) * 16777619u;
        u(hash);
        auto legacyFile =
            backupDirectory() + L"\\00000000-legacy-" + std::to_wstring(GetCurrentProcessId()) + L".stb";
        {
            std::ofstream f(std::filesystem::path(legacyFile), std::ios::binary);
            f.write(reinterpret_cast<const char *>(legacy.data()),
                    static_cast<std::streamsize>(legacy.size()));
            if (!f)
                throw Error("Write legacy test backup", ERROR_WRITE_FAULT);
        }
        auto oldBackup = loadJournal(legacyFile);
        auto restored = restore(oldBackup, cancel, roots);
        check(restored.success == 1 && readValue(legacyTarget) == std::optional<Value>(empty),
              "a version-1 backup still restores exact original registration data");

        auto invalid = once;
        invalid.file = once.file + L".invalid.stb";
        invalid.items.front().phase = 2;
        invalid.items.front().before.clear();
        restored = restore(invalid, cancel, roots);
        check(restored.failed == 1 && exists(selected.targets.front()),
              "an empty value snapshot is rejected without indexing missing data");
        rejects([&] { saveJournal(invalid); }, "invalid snapshots cannot be persisted as usable backups");
        auto invalidTree = once;
        invalidTree.items.front().target = target(roots.classes + L"\\*\\shell\\ReviewInvalid", L"", true);
        invalidTree.items.front().phase = 2;
        invalidTree.items.front().before.clear();
        invalidTree.items.front().keys.clear();
        restored = restore(invalidTree, cancel, roots);
        check(restored.failed == 1 && !exists(invalidTree.items.front().target),
              "a subtree snapshot without its root is rejected before creating keys");
        Journal invalidMRU;
        invalidMRU.file = once.file + L".mru.stb";
        Mutation replacement;
        replacement.target = target(roots.fileExts + L"\\.review-mru\\OpenWithList", L"MRUList");
        replacement.before.push_back({L"", L"MRUList", stringValue(L"abc")});
        replacement.after = stringValue(L"xyz");
        replacement.phase = 2;
        invalidMRU.items.push_back(replacement);
        writeValue(replacement.target, *replacement.after);
        rejects([&] { saveJournal(invalidMRU); },
                "an MRU replacement cannot inject new letters into the original list");
        restored = restore(invalidMRU, cancel, roots);
        check(restored.failed == 1 && readValue(replacement.target) == replacement.after,
              "a forged MRU snapshot cannot overwrite the current list during restore");

        auto verbPath = roots.classes + L"\\*\\shell\\ReviewConflict";
        put(verbPath, L"Original", L"original value");
        put(verbPath + L"\\command", L"", L"\"" + dir + L"\\Conflict.exe\" %1");
        auto menuEntry = [&](const std::wstring &name)
        {
            for (const auto &e : scan(roots, cancel).entries)
                if (e.name == name && e.kind != Kind::OpenWith)
                    return e;
            throw Error("Review menu not found", ERROR_NOT_FOUND);
        };
        auto conflict = prepare({menuEntry(L"ReviewConflict")}, roots);
        apply(conflict, cancel, roots);
        put(verbPath + L"\\command", L"", L"new command");
        restored = restore(conflict, cancel, roots);
        check(restored.conflicts == 1 && !restored.detail.empty() &&
                  !readValue(target(verbPath, L"Original")),
              "subtree conflicts preserve new values and provide a detailed conflict location");

        put(roots.classes + L"\\*\\shell\\ReviewEmptyCascade", L"SubCommands", L"");
        check(menuEntry(L"ReviewEmptyCascade").unsupported,
              "even an empty SubCommands registration remains inspection-only");
        put(roots.classes + L"\\*\\shell\\ReviewExtendedCascade", L"ExtendedSubCommandsKey", L"");
        check(menuEntry(L"ReviewExtendedCascade").unsupported,
              "ExtendedSubCommandsKey menus remain inspection-only");
        command(L"Applications\\review-delegate.exe", dir + L"\\Delegate.exe");
        put(roots.classes + L"\\Applications\\review-delegate.exe\\shell\\open\\command", L"DelegateExecute",
            L"");
        auto delegated = find(scan(roots, cancel), L"Applications\\review-delegate.exe");
        check(delegated.unsupported, "delegated Applications registrations remain inspection-only");
        rejects([&] { prepare({delegated}, roots); },
                "delegated application trees cannot be deleted indirectly");
        command(L"Applications\\review-nested.exe", dir + L"\\Nested.exe");
        put(roots.classes + L"\\Applications\\review-nested.exe\\shell\\custom", L"SubCommands", L"");
        check(find(scan(roots, cancel), L"Applications\\review-nested.exe").unsupported,
              "an Applications parent cannot bypass a nested inspection-only menu");

        auto ancestorPath = base + L"\\RestorePermission";
        put(ancestorPath, L"fixture", L"isolated");
        Journal permission;
        Mutation missing;
        missing.target = target(ancestorPath + L"\\Deleted\\Nested", L"value");
        missing.phase = 2;
        permission.items.push_back(missing);
        auto ancestor = openKey(target(ancestorPath), READ_CONTROL | WRITE_DAC | KEY_READ);
        PACL originalAcl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        DWORD aclError = GetSecurityInfo(ancestor.h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, &originalAcl, nullptr, &descriptor);
        if (aclError != ERROR_SUCCESS)
            throw Error("Read isolated ancestor ACL", aclError);
        BYTE everyone[SECURITY_MAX_SID_SIZE];
        DWORD sidSize = sizeof(everyone);
        CreateWellKnownSid(WinWorldSid, nullptr, everyone, &sidSize);
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = KEY_CREATE_SUB_KEY;
        access.grfAccessMode = DENY_ACCESS;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone);
        PACL restrictedAcl = nullptr;
        aclError = SetEntriesInAclW(1, &access, originalAcl, &restrictedAcl);
        if (aclError == ERROR_SUCCESS)
            aclError = SetSecurityInfo(ancestor.h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr,
                                       nullptr, restrictedAcl, nullptr);
        bool elevation = false;
        if (aclError == ERROR_SUCCESS)
        {
            elevation = needsElevation(permission, true);
            aclError = SetSecurityInfo(ancestor.h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr,
                                       nullptr, originalAcl, nullptr);
        }
        LocalFree(restrictedAcl);
        LocalFree(descriptor);
        if (aclError != ERROR_SUCCESS)
            throw Error("Isolated ancestor ACL", aclError);
        check(isElevated() || elevation,
              "restore permission checks find a denied surviving ancestor of a deleted key");
        check(!needsElevation(permission, true),
              "restoring a missing key under a writable user ancestor needs no UAC");

        auto directory = dir + L"\\Directory.exe";
        std::filesystem::create_directories(directory);
        command(L"Review.Directory", directory);
        ref(L".review-directory", L"Review.Directory");
        check(find(scan(roots, cancel), L"Review.Directory").status == Status::Review,
              "a directory named .exe is classified as review rather than a missing executable");
        put(roots.classes + L"\\.review-image", L"PerceivedType", L"review-image");
        auto typeMenu = roots.classes + L"\\SystemFileAssociations\\review-image\\shell\\ReviewTypeMenu";
        put(typeMenu + L"\\command", L"", L"\"" + dir + L"\\MissingTypeApp.exe\" %1");
        auto type = menuEntry(L"ReviewTypeMenu");
        check(type.status == Status::Missing && type.extensions.contains(L".review-image"),
              "SystemFileAssociations menus are discovered with their perceived-type extensions");
        auto typeJournal = prepare({type}, roots);
        out = apply(typeJournal, cancel, roots);
        restored = restore(typeJournal, cancel, roots);
        check(out.success == 1 && restored.success == 1 && readValue(target(typeMenu + L"\\command")),
              "a SystemFileAssociations menu can be deleted and restored in isolation");

        selected = find(scan(roots, cancel), L"Review.Once");
        Value unreadableDefault;
        unreadableDefault.type = REG_BINARY;
        unreadableDefault.data = {1, 2, 3};
        auto choice = target(roots.fileExts + L"\\.review-once\\UserChoice", L"ProgId");
        writeValue(choice, unreadableDefault);
        auto uncertainScan = scan(roots, cancel);
        check(find(uncertainScan, L"Review.Once").protectedItem,
              "unreadable default-app metadata protects entries instead of guessing a safe default");
        check(!find(uncertainScan, L"Review.Directory").protectedItem,
              "an unverifiable default protects only entries related to that extension");
        rejects([&] { prepare({selected}, roots); },
                "a stale selectable entry cannot bypass newly uncertain default-app protection");
        auto key = openKey(choice, KEY_SET_VALUE);
        if (!key.h || RegDeleteValueW(key.h, L"ProgId") != ERROR_SUCCESS)
            throw Error("Clean isolated default fixture", ERROR_WRITE_FAULT);
    }
    std::wstring copyExe(const std::wstring &path)
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        std::filesystem::copy_file(exe, path, std::filesystem::copy_options::overwrite_existing);
        return path;
    }
    std::wstring unusedDrive()
    {
        DWORD used = GetLogicalDrives();
        for (wchar_t letter = L'Z'; letter >= L'D'; --letter)
            if (!(used & (1u << (letter - L'A'))))
                return std::wstring(1, letter) + L":";
        return {};
    }
    Entry named(const ScanResult &s, const std::wstring &name, Kind kind)
    {
        for (const auto &e : s.entries)
            if (e.kind == kind && e.name == name)
                return e;
        throw Error("Named test entry not found", ERROR_NOT_FOUND);
    }
    void classificationTests()
    {
        // Windows 11 keeps the effective choice in UserChoiceLatest\ProgId; UserChoice can be stale.
        command(L"Latest.Current", copyExe(dir + L"\\Latest\\Current.exe"));
        command(L"Latest.Other", copyExe(dir + L"\\Latest\\Other.exe"));
        ref(L".latest-choice", L"Latest.Current");
        ref(L".latest-choice", L"Latest.Other");
        put(roots.fileExts + L"\\.latest-choice\\UserChoice", L"ProgId", L"Latest.Stale");
        put(roots.fileExts + L"\\.latest-choice\\UserChoiceLatest\\ProgId", L"ProgId", L"Latest.Current");
        command(L"Url.Choice", copyExe(dir + L"\\Url\\Browser.exe"));
        ref(L".url-choice", L"Url.Choice");
        put(roots.urlAssociations + L"\\shelltidy-test\\UserChoice", L"ProgId", L"Url.Choice");
        auto s = scan(roots, cancel);
        check(find(s, L"Latest.Current").protectedItem && !find(s, L"Latest.Other").protectedItem,
              "the UserChoiceLatest default is protected even when UserChoice names another ProgID");
        check(find(s, L"Url.Choice").protectedItem, "a protocol default from UrlAssociations is protected");

        auto drive = unusedDrive();
        if (!drive.empty())
        {
            check(fileState(drive + L"\\Apps\\Offline.exe") == FileState::Unavailable,
                  "a path on an absent drive is unavailable rather than missing");
            command(L"Review.Offline", drive + L"\\Apps\\Offline.exe");
            ref(L".review-offline", L"Review.Offline");
        }
        check(fileState(dir + L"\\NoSuchFile.exe") == FileState::Missing &&
                  fileState(exe) == FileState::Present && fileState(dir) == FileState::Folder,
              "file presence distinguishes missing files, files and folders on a reachable volume");
        copyExe(dir + L"\\Space.exe");
        check(resolveCommand(dir + L"\\Space Test\\app.exe %1") == dir + L"\\Space.exe",
              "an unquoted command follows CreateProcess and picks the first existing interpretation");
        put(roots.classes + L"\\*\\shell\\UnquotedFixture\\command", L"", dir + L"\\Old App\\app.exe \"%1\"");
        ref(L".orphan-ref", L"Orphan.NoSuchProgId");
        auto ghost = L"shelltidy-ghost-" + std::to_wstring(GetCurrentProcessId()) + L".exe";
        put(roots.fileExts + L"\\.orphan-list\\OpenWithList", L"a", ghost);
        put(roots.fileExts + L"\\.orphan-list\\OpenWithList", L"MRUList", L"a");
        put(roots.classes + L"\\*\\shellex\\ContextMenuHandlers\\OrphanHandler", L"",
            L"{00000000-0000-0000-0000-000000000789}");
        put(roots.classes + L"\\*\\shell\\AccelFixture", L"MUIVerb", L"Accel &Fixture");
        put(roots.classes + L"\\*\\shell\\AccelFixture\\command", L"", L"\"" + exe + L"\" \"%1\"");
        put(roots.classes + L"\\*\\shellex\\ContextMenuHandlers\\FriendlyKey", L"",
            L"{00000000-0000-0000-0000-0000000000A1}");
        put(roots.classes + L"\\CLSID\\{00000000-0000-0000-0000-0000000000A1}", L"", L"Friendly Handler");
        put(roots.classes + L"\\CLSID\\{00000000-0000-0000-0000-0000000000A1}\\InprocServer32", L"", exe);
        s = scan(roots, cancel);
        if (!drive.empty())
            check(find(s, L"Review.Offline").status == Status::Review,
                  "an application on a disconnected drive is not offered as a missing-file cleanup");
        check(named(s, L"UnquotedFixture", Kind::Verb).status == Status::Missing,
              "an unquoted command with spaces and no runnable interpretation is a missing file");
        check(find(s, L"Orphan.NoSuchProgId").status == Status::Orphan,
              "an OpenWithProgids reference to an unregistered ProgID is reported as not registered");
        auto orphan = find(s, L"Applications\\" + ghost);
        check(orphan.status == Status::Orphan,
              "an OpenWithList application without Applications or App Paths registration is not registered");
        check(named(s, L"OrphanHandler", Kind::Extension).status == Status::Orphan,
              "a context-menu handler whose CLSID is not registered is reported as not registered");
        auto friendly = named(s, L"Friendly Handler", Kind::Extension);
        check(friendly.status == Status::Healthy && !friendly.description.empty() &&
                  !friendly.company.empty(),
              "handlers show the CLSID name and the file description and company");
        check(named(s, L"Accel Fixture", Kind::Verb).status == Status::Healthy,
              "menu captions drop keyboard accelerator markers");
        auto orphanJournal = prepare({orphan}, roots);
        auto out = apply(orphanJournal, cancel, roots);
        check(out.success == 2 && out.failed == 0 &&
                  !readValue(target(roots.fileExts + L"\\.orphan-list\\OpenWithList", L"a")) &&
                  textValue(*readValue(target(roots.fileExts + L"\\.orphan-list\\OpenWithList", L"MRUList")))
                      .empty(),
              "an unregistered Open With reference is deleted together with its MRUList letter");
        auto restored = restore(orphanJournal, cancel, roots);
        check(restored.success == 2 &&
                  readValue(target(roots.fileExts + L"\\.orphan-list\\OpenWithList", L"a")),
              "an unregistered Open With reference can be restored");

        check(displayText(L"열기(&O)") == L"열기" && displayText(L"Open with &Code") == L"Open with Code" &&
                  displayText(L"A && B") == L"A & B" && displayText(L"@shelltidy-missing.dll,-1").empty(),
              "menu text strips accelerators and does not show unresolved @resource strings");

        // The same shared handler key can resolve to a 32-bit DLL that exists and a 64-bit DLL that does not.
        Entry healthy, missing;
        healthy.status = Status::Healthy;
        missing.status = Status::Missing;
        Target shared{Hive::Machine, KEY_WOW64_32KEY,
                      L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\X", L"", true};
        healthy.targets = {shared};
        shared.view = KEY_WOW64_64KEY;
        missing.targets = {shared};
        std::vector<Entry> views{healthy, missing};
        reconcileViews(views);
        check(views[1].status == Status::Review && views[0].status == Status::Healthy,
              "a registration healthy in another WOW64 view is not offered as a missing file");

        std::vector<Entry> rows(4);
        rows[0].checked = rows[1].checked = rows[2].checked = true;
        rows[2].protectedItem = true;
        check(actionEntries(rows, {0, 2, 3}, {3}, Action::Delete) == std::vector<size_t>{0},
              "checked entries hidden by the current view or protected are never acted on");
        check(actionEntries(rows, {3}, {3}, Action::Delete) == std::vector<size_t>{3},
              "without a visible checked row the highlighted rows are used");
        check(registryPath({Hive::Machine, KEY_WOW64_32KEY, L"Software\\Vendor\\App", L"", true}) ==
                  L"HKEY_LOCAL_MACHINE\\Software\\WOW6432Node\\Vendor\\App",
              "registry paths name the physical WOW6432Node location for non-shared 32-bit keys");
    }
    void switchTests()
    {
        auto verbPath = roots.classes + L"\\*\\shell\\SwitchFixture";
        put(verbPath + L"\\command", L"", L"\"" + exe + L"\" \"%1\"");
        auto marker = target(verbPath, L"LegacyDisable");
        auto verb = named(scan(roots, cancel), L"SwitchFixture", Kind::Verb);
        check(verb.switchable && !verb.disabled && verb.switches.size() == 1,
              "a static shell verb offers a reversible LegacyDisable switch");
        auto disable = prepare({verb}, roots, Action::Disable);
        check(disable.items.size() == 1 && disable.items.front().before.empty() &&
                  disable.items.front().after,
              "disabling records a created switch value instead of deleting the verb");
        auto out = apply(disable, cancel, roots);
        check(out.success == 1 && readValue(marker) && exists(target(verbPath, L"", true)),
              "disabling adds LegacyDisable and keeps the verb registration");
        verb = named(scan(roots, cancel), L"SwitchFixture", Kind::Verb);
        check(verb.disabled, "a LegacyDisable verb is shown as disabled");
        rejects([&] { prepare({verb}, roots, Action::Disable); }, "a disabled verb cannot be disabled twice");
        auto enable = prepare({verb}, roots, Action::Enable);
        out = apply(enable, cancel, roots);
        check(out.success == 1 && !readValue(marker), "enabling removes only the LegacyDisable value");
        auto restoredEnable = restore(enable, cancel, roots);
        check(restoredEnable.success == 1 && readValue(marker),
              "restoring an enable operation puts the original switch value back");
        auto restoredDisable = restore(disable, cancel, roots);
        check(restoredDisable.success == 1 && !readValue(marker),
              "restoring a disable operation removes the created switch");

        verb = named(scan(roots, cancel), L"SwitchFixture", Kind::Verb);
        auto conflict = prepare({verb}, roots, Action::Disable);
        apply(conflict, cancel, roots);
        writeValue(marker, stringValue(L"changed by another tool"));
        auto restored = restore(conflict, cancel, roots);
        check(restored.conflicts == 1 && textValue(*readValue(marker)) == L"changed by another tool",
              "a switch changed after disabling is preserved during restore");
        auto k = openKey(marker, KEY_SET_VALUE);
        if (!k.h || RegDeleteValueW(k.h, L"LegacyDisable") != ERROR_SUCCESS)
            throw Error("Clean isolated switch fixture", ERROR_WRITE_FAULT);

        command(L"Switch.Core", exe);
        ref(L".switch-core", L"Switch.Core");
        Entry core;
        for (const auto &e : scan(roots, cancel).entries)
            if (e.kind == Kind::Verb && e.progids.contains(L"Switch.Core"))
                core = e;
        check(core.protectedItem && !core.switchable, "core open verbs cannot be disabled");
        core.switchable = true;
        rejects([&] { prepare({core}, roots, Action::Disable); },
                "a forged switchable core verb is rejected by the fresh scan");

        auto handler = named(scan(roots, cancel), L"Fixture", Kind::Extension);
        check(handler.switchable && handler.switches.size() == 2,
              "a shell extension offers a CLSID block switch");
        auto block = prepare({handler}, roots, Action::Disable);
        auto blocked = target(roots.blocked, L"{00000000-0000-0000-0000-000000000456}");
        out = apply(block, cancel, roots);
        check(out.success == 1 && readValue(blocked) &&
                  readValue(target(roots.classes + L"\\*\\shellex\\ContextMenuHandlers\\Fixture")),
              "blocking a shell extension adds the CLSID to Blocked and keeps the handler registration");
        handler = named(scan(roots, cancel), L"Fixture", Kind::Extension);
        check(handler.disabled, "a blocked shell extension is shown as disabled");
        auto unblock = prepare({handler}, roots, Action::Enable);
        out = apply(unblock, cancel, roots);
        check(out.success == 1 && !readValue(blocked), "unblocking removes the CLSID from Blocked");

        Journal forged;
        forged.file =
            backupDirectory() + L"\\forged-switch-" + std::to_wstring(GetCurrentProcessId()) + L".stb";
        Mutation created;
        created.target = target(roots.fileExts + L"\\.jpg\\OpenWithProgids", L"Injected.ProgId");
        created.after = stringValue(L"");
        forged.items.push_back(created);
        rejects([&] { saveJournal(forged); },
                "a created value outside the switch locations cannot be journaled");
    }
    void runTests()
    {
        setup();
        auto lockName = L"Test-" + std::to_wstring(GetCurrentProcessId());
        {
            bool first = false, second = false;
            Handle a(createInstanceLock(lockName, first));
            Handle b(createInstanceLock(lockName, second));
            check(a.h && b.h && !first && second,
                  "single-instance gate recognizes a second launch for the same user/session");
        }
        {
            bool existing = true;
            Handle a(createInstanceLock(lockName, existing));
            check(a.h && !existing, "single-instance gate is released when all instance handles close");
        }
        auto popup = popupSummary(std::wstring(50000, L'x') + L"\n" + std::wstring(50000, L'y'));
        check(popup.size() <= 341, "popup body is bounded even for very long failure lists");
        auto multiline = popupSummary(L"1\n2\n3\n4\n5\n6\n7\n8");
        check(std::count(multiline.begin(), multiline.end(), L'\n') <= 5,
              "popup body never expands beyond six explicit lines");
        RECT area{-1920, 40, 0, 1080};
        auto centered = centeredRect(area, 944, 608);
        check(centered.left == -1432 && centered.top == 256,
              "startup position centers within monitor work area including negative coordinates");
        auto fitted = centeredRect(area, 4000, 4000);
        check(fitted.left == area.left && fitted.top == area.top && fitted.right == area.right &&
                  fitted.bottom == area.bottom,
              "oversized startup window is constrained to monitor work area");
        check(compareLabels(L"App2", L"App10") < 0 && compareLabels(L"abc", L"ABC") == 0,
              "column collation uses natural numbers and ignores case");
        check(compareLabels(L"가", L"나") < 0 && compareLabels(L"나", L"다") < 0,
              "column collation orders Korean labels in ascending order");
        check(resolveCommand(L"\"" + exe + L"\" \"%1\" --flag") == exe, "quoted path and %1 arguments");
        check(resolveCommand(L"%SystemRoot%\\System32\\notepad.exe %1").find(L"notepad.exe") !=
                  std::wstring::npos,
              "environment expansion without executing commands");
        check(resolveCommand(L"\"unterminated.exe").empty(), "malformed quoted command remains unresolved");
        check(resolveCommand(L"C:\\Missing Path\\Example.exe %1") == L"C:\\Missing Path\\Example.exe",
              "an unquoted path with no runnable interpretation resolves to its intended executable");
        Value malformed;
        malformed.type = REG_SZ;
        malformed.data = {1, 2, 3};
        check(textValue(malformed).empty(), "odd-length REG_SZ is rejected safely");
        auto embedded = stringValue(L"hello");
        embedded.data.insert(embedded.data.end(), {1, 0});
        check(textValue(embedded).empty(), "embedded nonterminal null is rejected safely");
        Value raw;
        raw.type = REG_BINARY;
        raw.data = {0, 1, 0, 255};
        writeValue(target(roots.classes + L"\\*\\shell\\ShellTidyFixture", L"Raw"), raw);
        auto s = scan(roots, cancel);
        auto shared = find(s, L"SharedCapabilities.App");
        check(shared.targets.size() == 65,
              "HKCU Capabilities in both WOW64 views yields exactly 65 physical targets");
        auto legacyShared = shared;
        for (auto &t : legacyShared.targets)
            t.view = KEY_WOW64_32KEY;
        auto sharedJournal = prepare({shared, legacyShared}, roots);
        check(sharedJournal.items.size() == 65,
              "overlapping 32/64-bit selections are deduplicated before backup");
        auto sharedResult = apply(sharedJournal, cancel, roots);
        check(sharedResult.success == 65 && sharedResult.failed == 0,
              "65 shared Capabilities delete once without false ERROR_RETRY");
        auto sharedRestore = restore(sharedJournal, cancel, roots);
        check(sharedRestore.success == 65 && sharedRestore.failed == 0,
              "shared Capabilities restore once with original values intact");
        auto current = find(s, L"Picvium.Image.jpg"), old = find(s, L"Picvium.Installed.Image.jpg");
        check(current.protectedItem, "current UserChoice app is protected across related extensions");
        check(!old.protectedItem && old.executable != current.executable && old.status == Status::Missing,
              "old Picvium path remains individually selectable");
        check(old.extensions.size() == 5 && current.extensions.size() == 5,
              "all five Picvium extensions are grouped by actual command");
        check(old.targets.size() == 5, "shared WOW64 branches do not duplicate registrations");
        rejects([&] { prepare({current}, roots); }, "protected default cannot enter deletion journal");
        auto mark = find(s, L"Applications\\marktext.exe"), obs = find(s, L"Applications\\Obsidian.exe");
        check(mark.extensions.contains(L".novel-extension-123"),
              "extensions are discovered rather than hardcoded");
        Journal j = prepare({old, mark, obs}, roots);
        saveJournal(j);
        auto disk = loadJournal(j.file);
        check(disk.items.size() == j.items.size(), "durable backup round trip preserves exact mutations");
        auto out = apply(j, cancel, roots);
        check(out.success > 0 && out.failed == 0,
              "Picvium MarkText Obsidian selected registrations really delete");
        check(!readValue(target(roots.fileExts + L"\\.jpg\\OpenWithProgids", L"Picvium.Installed.Image.jpg")),
              "only old Picvium OpenWithProgids reference removed");
        check(
            readValue(target(roots.fileExts + L"\\.jpg\\OpenWithProgids", L"Picvium.Image.jpg")).has_value(),
            "current Picvium registration preserved");
        check(textValue(*readValue(target(roots.fileExts + L"\\.md\\OpenWithList", L"MRUList"))) == L"c",
              "MRUList removes selected letters and retains the other app");
        check(readValue(target(roots.fileExts + L"\\.md\\OpenWithProgids", L"Other.Markdown")).has_value(),
              "other Markdown editor preserved");
        check(readValue(target(roots.classes + L"\\Picvium.Installed.Image.jpg\\shell\\open\\command"))
                  .has_value(),
              "shared ProgID command is preserved");
        check(std::filesystem::exists(exe) && std::filesystem::exists(current.executable),
              "application files are never removed");
        auto restored = restore(j, cancel, roots);
        check(restored.success > 0 && restored.failed == 0 && restored.conflicts == 0,
              "last operation restores deleted registrations");
        check(textValue(*readValue(target(roots.fileExts + L"\\.md\\OpenWithList", L"MRUList"))) == L"abc",
              "MRUList restored with exact original type and bytes");
        auto again = restore(j, cancel, roots);
        check(again.success == 0, "restoration is idempotent");
        // A tree snapshot preserves binary types and never replaces later unrelated values.
        s = scan(roots, cancel);
        Entry verb;
        for (const auto &e : s.entries)
            if (e.kind == Kind::Verb && e.name == L"ShellTidyFixture")
                verb = e;
        auto treeJournal = prepare({verb}, roots);
        out = apply(treeJournal, cancel, roots);
        check(out.success == 1, "static verb subtree physically removed");
        put(roots.classes + L"\\*\\shell\\ShellTidyFixture", L"LaterAddition", L"keep me");
        restored = restore(treeJournal, cancel, roots);
        check(readValue(target(roots.classes + L"\\*\\shell\\ShellTidyFixture", L"Raw")) ==
                  std::optional<Value>(raw),
              "binary registry value type and bytes restored");
        check(textValue(*readValue(
                  target(roots.classes + L"\\*\\shell\\ShellTidyFixture", L"LaterAddition"))) == L"keep me",
              "restore preserves unrelated values added later");
        s = scan(roots, cancel);
        old = find(s, L"Picvium.Installed.Image.jpg");
        auto conflict = prepare({old}, roots);
        apply(conflict, cancel, roots);
        auto ct = conflict.items.front().target;
        Value replacement;
        replacement.type = REG_BINARY;
        replacement.data = {7, 8, 9};
        writeValue(ct, replacement);
        restored = restore(conflict, cancel, roots);
        check(restored.conflicts == 1 && readValue(ct) == std::optional<Value>(replacement),
              "restore skips conflicting current value without overwriting");
        // Restore a partial deletion without needing all selected entries to succeed.
        s = scan(roots, cancel);
        mark = find(s, L"Applications\\marktext.exe");
        auto partial = prepare({mark}, roots);
        auto changed = partial.items.front().target;
        writeValue(changed, stringValue(L"concurrent change"));
        out = apply(partial, cancel, roots);
        check(out.failed > 0 && out.success > 0,
              "partial failure reports successful and failed mutations separately");
        restore(partial, cancel, roots);
        s = scan(roots, cancel);
        obs = find(s, L"Applications\\Obsidian.exe");
        auto canceled = prepare({obs}, roots);
        cancel = true;
        out = apply(canceled, cancel, roots);
        cancel = false;
        check(out.success == 0 && exists(obs.targets.front()),
              "cancellation before mutation leaves registration intact");
        auto failure = prepare({obs}, roots);
        auto originalFile = failure.file;
        failure.file = dir + L"\\blocking-file\\no-backup.stb";
        {
            std::ofstream file(std::filesystem::path(dir + L"\\blocking-file"));
            file << "block";
        }
        rejects([&] { apply(failure, cancel, roots); }, "backup failure blocks deletion");
        check(exists(obs.targets.front()), "backup failure preserves all selected registrations");
        // Corrupt backup and forbidden targets never reach registry mutation.
        saveJournal(canceled);
        {
            std::fstream file(std::filesystem::path(canceled.file),
                              std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(20);
            file.put('X');
        }
        rejects([&] { loadJournal(canceled.file); }, "corrupted backup fails checksum validation");
        Journal forbidden;
        forbidden.file = originalFile + L".forbidden.stb";
        Mutation bad;
        bad.target = target(roots.fileExts + L"\\.jpg\\UserChoice", L"ProgId");
        bad.phase = 2;
        bad.before.push_back({L"", L"ProgId", stringValue(L"bad")});
        forbidden.items.push_back(bad);
        auto protectedRestore = restore(forbidden, cancel, roots);
        check(protectedRestore.failed == 1 && textValue(*readValue(bad.target)) == L"Picvium.Image.jpg",
              "UserChoice is never modified even by crafted restore target");
        s = scan(roots, cancel);
        bool modern = false;
        Entry handler;
        for (const auto &e : s.entries)
        {
            if (e.kind == Kind::Modern && e.name == L"ModernFixture")
                modern = e.unsupported;
            if (e.kind == Kind::Extension && e.name == L"Fixture")
                handler = e;
        }
        check(modern, "IExplorerCommand registration is inspection-only");
        auto handlerJournal = prepare({handler}, roots);
        out = apply(handlerJournal, cancel, roots);
        check(out.success == 1 &&
                  readValue(target(roots.classes +
                                   L"\\CLSID\\{00000000-0000-0000-0000-000000000456}\\InprocServer32"))
                      .has_value(),
              "COM deletion removes handler reference and preserves shared CLSID");
        restore(handlerJournal, cancel, roots);
        // Exact key view survives on-disk journal serialization.
        auto viewJournal = prepare({obs}, roots);
        for (auto &m : viewJournal.items)
            m.target.view = KEY_WOW64_32KEY;
        saveJournal(viewJournal);
        auto viewReload = loadJournal(viewJournal.file);
        check(viewReload.items.front().target.view == KEY_WOW64_32KEY,
              "WOW64 view flags preserved in backup");
        // Access-denied test changes only an isolated key ACL and restores it through an existing handle.
        s = scan(roots, cancel);
        obs = find(s, L"Applications\\Obsidian.exe");
        auto denied = prepare({obs}, roots);
        auto deniedTarget = denied.items.front().target;
        if (!deniedTarget.tree)
        {
            auto k = openKey(deniedTarget, READ_CONTROL | WRITE_DAC | KEY_READ);
            PACL oldAcl = nullptr;
            PSECURITY_DESCRIPTOR sd = nullptr;
            DWORD e = GetSecurityInfo(k.h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                      &oldAcl, nullptr, &sd);
            if (e == ERROR_SUCCESS)
            {
                BYTE everyone[SECURITY_MAX_SID_SIZE];
                DWORD sidSize = sizeof(everyone);
                CreateWellKnownSid(WinWorldSid, nullptr, everyone, &sidSize);
                EXPLICIT_ACCESSW access{};
                access.grfAccessPermissions = KEY_SET_VALUE;
                access.grfAccessMode = DENY_ACCESS;
                access.grfInheritance = NO_INHERITANCE;
                access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone);
                PACL acl = nullptr;
                e = SetEntriesInAclW(1, &access, oldAcl, &acl);
                if (e == ERROR_SUCCESS)
                    e = SetSecurityInfo(k.h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                        acl, nullptr);
                if (e == ERROR_SUCCESS)
                {
                    out = apply(denied, cancel, roots);
                    check(out.failed > 0, "access-denied registry mutation is reported");
                    SetSecurityInfo(k.h, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, oldAcl,
                                    nullptr);
                    restore(denied, cancel, roots);
                }
                else
                    check(false, "isolated ACL setup");
                LocalFree(acl);
                LocalFree(sd);
            }
        }
        reviewTests();
        classificationTests();
        switchTests();
        cancel = true;
        auto stopped = scan(roots, cancel);
        cancel = false;
        check(stopped.canceled, "asynchronous scanner respects cancellation");
        std::wstring preflight;
        bool shell = explorerPreflight(preflight);
        output << "INFO Explorer read-only preflight: " << (shell ? "verified" : "unavailable")
               << ". No Explorer process was terminated.\n";
        output << "INFO Real Explorer restart, interactive UAC consent/cancellation, and Windows 10 require "
                  "manual validation.\n";
    }

  public:
    explicit Tests(const std::wstring &path) : output(std::filesystem::path(path), std::ios::trunc)
    {
        if (!output)
            throw Error("Test report cannot be created", ERROR_ACCESS_DENIED);
    }
    int run()
    {
        try
        {
            runTests();
        }
        catch (const Error &e)
        {
            output << "FATAL " << e.what() << " (" << e.code << ")\n";
            ++failed;
        }
        catch (const std::exception &e)
        {
            output << "FATAL " << e.what() << '\n';
            ++failed;
        }
        // Only this run's explicitly named fixture root is ever recursively deleted.
        if (base.starts_with(L"Software\\ShellTidy\\Tests\\Run-"))
        {
            auto t = target(base, L"", true);
            auto k = openKey(t, KEY_READ | KEY_WRITE | DELETE);
            if (k.h)
            {
                RegDeleteTreeW(k.h, nullptr);
                k = Key();
                RegDeleteKeyExW(HKEY_CURRENT_USER, base.c_str(), KEY_WOW64_64KEY, 0);
            }
        }
        if (!dir.empty())
        {
            auto resolved = std::filesystem::absolute(dir);
            auto parent = std::filesystem::absolute(std::filesystem::path(backupDirectory()).parent_path());
            if (resolved.parent_path() == parent && resolved.filename().wstring().starts_with(L"Fixtures-"))
            {
                std::error_code ec;
                std::filesystem::remove_all(resolved, ec);
            }
        }
        output << "\nRESULT " << passed << " passed, " << failed << " failed\n";
        return failed ? 1 : 0;
    }
};
} // namespace
int selfTest(const std::wstring &report)
{
    return Tests(report).run();
}
} // namespace st

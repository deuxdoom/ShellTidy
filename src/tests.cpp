#include "shelltidy.h"
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
        dir = std::filesystem::path(backupDirectory()).parent_path().wstring() + L"\\Fixtures-" +
              std::to_wstring(GetCurrentProcessId());
        std::filesystem::create_directories(dir);
        base = L"Software\\ShellTidy\\Tests\\Run-" + std::to_wstring(GetCurrentProcessId());
        roots = {base + L"\\Classes", base + L"\\FileExts", true, base + L"\\RegisteredApplications"};
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
        check(resolveCommand(L"C:\\Missing Path\\Example.exe %1").empty(),
              "ambiguous missing unquoted path stays review-only");
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

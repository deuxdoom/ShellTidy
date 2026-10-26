#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <filesystem>
#include <optional>
#include <stdexcept>

namespace st
{
using Bytes = std::vector<BYTE>;
struct Error : std::runtime_error
{
    DWORD code;
    explicit Error(const char *what, DWORD c = GetLastError()) : std::runtime_error(what), code(c) {}
};
struct Key
{
    HKEY h = nullptr;
    Key() = default;
    explicit Key(HKEY v) : h(v) {}
    ~Key()
    {
        if (h)
            RegCloseKey(h);
    }
    Key(const Key &) = delete;
    Key &operator=(const Key &) = delete;
    Key(Key &&k) noexcept : h(k.h)
    {
        k.h = nullptr;
    }
    Key &operator=(Key &&k) noexcept
    {
        if (h)
            RegCloseKey(h);
        h = k.h;
        k.h = nullptr;
        return *this;
    }
};
struct Handle
{
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle()
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
enum class Hive : DWORD
{
    User,
    Machine
};
enum class Kind
{
    OpenWith,
    Verb,
    Extension,
    Modern
};
enum class Status
{
    Healthy,
    Missing,
    Duplicate,
    Review,
    Protected
};
struct Value
{
    DWORD type = REG_NONE;
    Bytes data;
    bool operator==(const Value &) const = default;
};
struct Target
{
    Hive hive = Hive::User;
    DWORD view = KEY_WOW64_64KEY;
    std::wstring path, name;
    bool tree = false;
    bool operator==(const Target &) const = default;
};
struct Entry
{
    std::wstring name, command, executable, source, identity, reason;
    Kind kind = Kind::OpenWith;
    Status status = Status::Review;
    bool protectedItem = false, unsupported = false, checked = false;
    std::set<std::wstring> extensions, progids;
    std::vector<Target> targets;
};
struct Roots
{
    std::wstring classes = L"Software\\Classes";
    std::wstring fileExts = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts";
    bool isolated = false;
    std::wstring registeredApps = L"Software\\RegisteredApplications";
};
struct ScanResult
{
    std::vector<Entry> entries;
    unsigned warnings = 0;
    bool canceled = false;
};
struct SavedValue
{
    std::wstring relative, name;
    Value value;
};
struct Mutation
{
    Target target;
    std::vector<SavedValue> before;
    std::vector<std::wstring> keys;
    std::optional<Value> after; // null = delete; otherwise exact replacement (MRUList).
    DWORD phase = 0;            // 0 prepared, 1 durably recorded intent, 2 verified success, 3 restored.
};
struct Journal
{
    std::vector<Mutation> items;
    std::wstring file;
};
struct Outcome
{
    unsigned success = 0, failed = 0, conflicts = 0;
    std::wstring detail, backup;
    bool restartOK = true;
};
std::wstring lower(std::wstring s);
std::wstring trim(std::wstring s);
int compareLabels(const std::wstring &a, const std::wstring &b);
std::wstring popupSummary(const std::wstring &text, size_t maxChars = 340, size_t maxLines = 6);
RECT centeredRect(const RECT &work, int width, int height);
std::wstring textValue(const Value &v);
Value stringValue(const std::wstring &s, DWORD type = REG_SZ);
std::wstring describe(const Target &t);
bool sharedTarget(const Target &t, const Roots &roots = {});
Target canonicalTarget(Target t, const Roots &roots = {});
std::wstring errorText(DWORD code);
HKEY hiveKey(Hive h);
Key openKey(const Target &t, REGSAM access = KEY_READ);
std::optional<Value> readValue(const Target &t);
bool exists(const Target &t);
void writeValue(const Target &t, const Value &v);
std::vector<std::wstring> subkeys(const Target &t);
std::vector<std::pair<std::wstring, Value>> values(const Target &t);
std::wstring resolveCommand(const std::wstring &command);
ScanResult scan(const Roots &roots, const std::atomic_bool &cancel);
std::wstring backupDirectory();
std::vector<std::wstring> backupFiles();
Journal prepare(const std::vector<Entry> &entries, const Roots &roots = {});
void saveJournal(const Journal &journal);
Journal loadJournal(const std::wstring &file);
Outcome apply(Journal &journal, const std::atomic_bool &cancel, const Roots &roots = {});
Outcome restore(Journal &journal, const std::atomic_bool &cancel, const Roots &roots = {});
bool isElevated();
HANDLE createInstanceLock(const std::wstring &suffix, bool &alreadyRunning);
void activateExistingInstance();
Outcome executeElevated(const std::vector<Entry> &entries, bool restoring, const std::wstring &backup,
                        HWND owner);
int elevatedWorker(const std::wstring &pipe);
bool restartExplorer(std::wstring &detail);
bool explorerPreflight(std::wstring &detail);
int selfTest(const std::wstring &report);
int runUI(HINSTANCE instance, int show, bool english, UINT testDpi = 0);
} // namespace st

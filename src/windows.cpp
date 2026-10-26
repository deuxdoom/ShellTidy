#include "shelltidy.h"
#include <shellapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <restartmanager.h>
#include <algorithm>

namespace st
{
namespace
{
std::wstring modulePath()
{
    wchar_t p[32768];
    DWORD n = GetModuleFileNameW(nullptr, p, 32768);
    if (!n || n >= 32768)
        throw Error("Executable path");
    return p;
}
Bytes tokenUser(HANDLE process)
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        throw Error("Read process token");
    Handle token(raw);
    DWORD n = 0;
    GetTokenInformation(token.h, TokenUser, nullptr, 0, &n);
    Bytes b(n);
    if (!GetTokenInformation(token.h, TokenUser, b.data(), n, &n))
        throw Error("Read token user");
    return b;
}
bool sameUser(HANDLE a, HANDLE b)
{
    auto x = tokenUser(a), y = tokenUser(b);
    return EqualSid(reinterpret_cast<TOKEN_USER *>(x.data())->User.Sid,
                    reinterpret_cast<TOKEN_USER *>(y.data())->User.Sid) != FALSE;
}
bool mediumToken(HANDLE token)
{
    DWORD n = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &n);
    Bytes b(n);
    if (!GetTokenInformation(token, TokenIntegrityLevel, b.data(), n, &n))
        return false;
    auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(b.data())->Label.Sid;
    DWORD level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    return level <= SECURITY_MANDATORY_MEDIUM_RID;
}
HANDLE linkedMedium()
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw))
        throw Error("Read launch token");
    Handle token(raw);
    TOKEN_LINKED_TOKEN linked{};
    DWORD n = sizeof(linked);
    if (!GetTokenInformation(token.h, TokenLinkedToken, &linked, n, &n))
        throw Error("Unelevated launch token unavailable");
    if (!mediumToken(linked.LinkedToken))
    {
        CloseHandle(linked.LinkedToken);
        throw Error("Refusing elevated Explorer launch", ERROR_ACCESS_DENIED);
    }
    return linked.LinkedToken;
}
bool launchMedium(const std::wstring &exe, std::wstring args, HANDLE token)
{
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    BOOL ok = token ? CreateProcessWithTokenW(token, 0, exe.c_str(), cmd.data(),
                                              CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                              &si, &pi)
                    : CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                     nullptr, nullptr, &si, &pi);
    if (ok)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok != FALSE;
}
std::wstring explorerPath()
{
    wchar_t w[MAX_PATH];
    GetWindowsDirectoryW(w, MAX_PATH);
    return std::wstring(w) + L"\\explorer.exe";
}
bool validShell()
{
    auto shell = GetShellWindow();
    if (!shell)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(shell, &pid);
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!p.h)
        return false;
    wchar_t path[32768];
    DWORD n = 32768;
    if (!QueryFullProcessImageNameW(p.h, 0, path, &n) || lower(path) != lower(explorerPath()))
        return false;
    DWORD a = 0, b = 0;
    if (!ProcessIdToSessionId(pid, &a) || !ProcessIdToSessionId(GetCurrentProcessId(), &b) || a != b)
        return false;
    try
    {
        return sameUser(p.h, GetCurrentProcess());
    }
    catch (...)
    {
        return false;
    }
}
bool waitShell(unsigned ms)
{
    for (unsigned i = 0; i < ms / 100; ++i)
    {
        if (validShell())
            return true;
        Sleep(100);
    }
    return validShell();
}
struct Packet
{
    DWORD magic = 0x53545031, mode = 0, success = 0, failed = 0, conflicts = 0, length = 0;
};
void pipeIO(HANDLE pipe, void *data, DWORD size, bool write)
{
    BYTE *p = static_cast<BYTE *>(data);
    while (size)
    {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{};
        ov.hEvent = event.h;
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, p, size, &done, &ov) : ReadFile(pipe, p, size, &done, &ov);
        if (!ok)
        {
            if (GetLastError() != ERROR_IO_PENDING)
                throw Error("Elevation pipe I/O");
            if (WaitForSingleObject(event.h, 120000) != WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &ov);
                WaitForSingleObject(event.h, INFINITE);
                throw Error("Elevation worker timeout", ERROR_TIMEOUT);
            }
            if (!GetOverlappedResult(pipe, &ov, &done, FALSE))
                throw Error("Elevation pipe result");
        }
        if (!done)
            throw Error("Elevation worker disconnected", ERROR_BROKEN_PIPE);
        p += done;
        size -= done;
    }
}
void sendPacket(HANDLE pipe, Packet p, const std::wstring &text)
{
    p.length = static_cast<DWORD>(text.size());
    pipeIO(pipe, &p, sizeof(p), true);
    if (p.length)
        pipeIO(pipe, const_cast<wchar_t *>(text.data()), p.length * 2, true);
}
std::pair<Packet, std::wstring> receivePacket(HANDLE pipe)
{
    Packet p;
    pipeIO(pipe, &p, sizeof(p), false);
    if (p.magic != 0x53545031 || p.length > 500000)
        throw Error("Invalid elevation response", ERROR_INVALID_DATA);
    std::wstring text(p.length, L'\0');
    if (p.length)
        pipeIO(pipe, text.data(), p.length * 2, false);
    return {p, text};
}
} // namespace
bool isElevated()
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    TOKEN_ELEVATION e{};
    DWORD n = sizeof(e);
    return GetTokenInformation(token.h, TokenElevation, &e, n, &n) && e.TokenIsElevated;
}
HANDLE createInstanceLock(const std::wstring &suffix, bool &alreadyRunning)
{
    auto user = tokenUser(GetCurrentProcess());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid))
        throw Error("Instance user identity");
    std::wstring name = L"Local\\ShellTidy.Main." + std::wstring(sid) + L"." + suffix;
    LocalFree(sid);
    HANDLE mutex = CreateMutexExW(nullptr, name.c_str(), 0, SYNCHRONIZE);
    DWORD error = GetLastError();
    alreadyRunning = error == ERROR_ALREADY_EXISTS || (!mutex && error == ERROR_ACCESS_DENIED);
    if (!mutex && !alreadyRunning)
        throw Error("Single instance lock", error);
    return mutex;
}
void activateExistingInstance()
{
    for (int i = 0; i < 20; ++i)
    {
        HWND window = FindWindowW(L"ShellTidy.Main", nullptr);
        if (window)
        {
            if (IsIconic(window))
                ShowWindowAsync(window, SW_RESTORE);
            HWND popup = GetLastActivePopup(window);
            if (popup && IsWindowVisible(popup))
                window = popup;
            SetForegroundWindow(window);
            FLASHWINFO flash{sizeof(flash), window, FLASHW_TRAY, 2, 0};
            FlashWindowEx(&flash);
            return;
        }
        Sleep(50);
    }
}
Outcome executeElevated(const std::vector<Entry> &entries, bool restoring, const std::wstring &backup,
                        HWND owner)
{
    Journal j = restoring ? loadJournal(backup) : prepare(entries);
    if (!restoring)
        saveJournal(j);
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t id[40];
    StringFromGUID2(guid, id, 40);
    auto pipeName = std::wstring(L"\\\\.\\pipe\\ShellTidy-") + id;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)",
                                                              SDDL_REVISION_1, &sd, nullptr))
        throw Error("Pipe security");
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    Handle pipe(CreateNamedPipeW(pipeName.c_str(),
                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 1, 65536, 65536, 0, &sa));
    LocalFree(sd);
    if (pipe.h == INVALID_HANDLE_VALUE)
        throw Error("Create elevation pipe");
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED ov{};
    ov.hEvent = event.h;
    BOOL connected = ConnectNamedPipe(pipe.h, &ov);
    DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && connectError != ERROR_IO_PENDING && connectError != ERROR_PIPE_CONNECTED)
        throw Error("Connect elevation pipe", connectError);
    auto exe = modulePath();
    auto args = L"--worker \"" + pipeName + L"\"";
    SHELLEXECUTEINFOW si{sizeof(si)};
    si.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    si.hwnd = owner;
    si.lpVerb = L"runas";
    si.lpFile = exe.c_str();
    si.lpParameters = args.c_str();
    si.nShow = SW_HIDE;
    if (!ShellExecuteExW(&si))
    {
        CancelIoEx(pipe.h, &ov);
        WaitForSingleObject(event.h, 1000);
        throw Error("UAC canceled or denied");
    }
    Handle process(si.hProcess);
    if (connectError == ERROR_IO_PENDING)
    {
        HANDLE handles[] = {event.h, process.h};
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 30000);
        if (wait != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe.h, &ov);
            WaitForSingleObject(event.h, 1000);
            throw Error("Elevation connection failed", ERROR_TIMEOUT);
        }
    }
    ULONG client = 0;
    if (!GetNamedPipeClientProcessId(pipe.h, &client) || client != GetProcessId(process.h))
        throw Error("Unexpected elevation client", ERROR_ACCESS_DENIED);
    Packet request;
    request.mode = restoring ? 1 : 0;
    sendPacket(pipe.h, request, j.file);
    auto [response, message] = receivePacket(pipe.h);
    Outcome out;
    out.success = response.success;
    out.failed = response.failed;
    out.conflicts = response.conflicts;
    out.detail = message;
    out.backup = j.file;
    return out;
}
int elevatedWorker(const std::wstring &name)
{
    if (!name.starts_with(L"\\\\.\\pipe\\ShellTidy-"))
        return 2;
    Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr));
    if (pipe.h == INVALID_HANDLE_VALUE)
        return 3;
    try
    {
        ULONG server = 0;
        if (!GetNamedPipeServerProcessId(pipe.h, &server))
            throw Error("Server identity");
        Handle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server));
        wchar_t path[32768];
        DWORD n = 32768;
        if (!parent.h || !QueryFullProcessImageNameW(parent.h, 0, path, &n) ||
            lower(path) != lower(modulePath()) || !sameUser(parent.h, GetCurrentProcess()))
            throw Error("Elevation requires the same Windows account", ERROR_ACCESS_DENIED);
        auto [request, file] = receivePacket(pipe.h);
        auto j = loadJournal(file);
        std::atomic_bool cancel = false;
        Outcome out;
        if (request.mode == 0)
        {
            for (const auto &m : j.items)
                if (m.phase != 0)
                    throw Error("Invalid request state", ERROR_INVALID_DATA);
            out = st::apply(j, cancel);
        }
        else if (request.mode == 1)
            out = restore(j, cancel);
        else
            throw Error("Invalid worker request", ERROR_INVALID_DATA);
        Packet response;
        response.success = out.success;
        response.failed = out.failed;
        response.conflicts = out.conflicts;
        sendPacket(pipe.h, response, out.detail);
        return 0;
    }
    catch (const Error &e)
    {
        try
        {
            Packet p;
            p.failed = 1;
            sendPacket(pipe.h, p, errorText(e.code));
        }
        catch (...)
        {
        }
        return 4;
    }
    catch (...)
    {
        try
        {
            Packet p;
            p.failed = 1;
            sendPacket(pipe.h, p, L"Elevation worker failed / 관리자 작업 실패");
        }
        catch (...)
        {
        }
        return 5;
    }
}
bool explorerPreflight(std::wstring &detail)
{
    if (!validShell())
    {
        detail = L"Explorer shell identity could not be verified / 현재 사용자의 Explorer 셸을 확인할 수 "
                 L"없습니다.";
        return false;
    }
    if (isElevated())
        try
        {
            Handle token(linkedMedium());
        }
        catch (const Error &e)
        {
            detail = errorText(e.code);
            return false;
        }
    return true;
}
bool restartExplorer(std::wstring &detail)
{
    if (!explorerPreflight(detail))
        return false;
    Handle token(isElevated() ? linkedMedium() : nullptr);
    // Independent unelevated recovery monitor is started before any Explorer is stopped.
    if (!launchMedium(modulePath(), L"--recover-shell", token.h))
    {
        detail =
            L"Recovery monitor could not start / 복구 감시 프로세스 시작 실패: " + errorText(GetLastError());
        return false;
    }
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.h == INVALID_HANDLE_VALUE)
    {
        detail = errorText(GetLastError());
        return false;
    }
    std::vector<RM_UNIQUE_PROCESS> apps;
    std::vector<HANDLE> handles;
    PROCESSENTRY32W item{sizeof(item)};
    if (Process32FirstW(snapshot.h, &item))
        do
        {
            if (_wcsicmp(item.szExeFile, L"explorer.exe"))
                continue;
            DWORD other = 0;
            if (!ProcessIdToSessionId(item.th32ProcessID, &other) || other != session)
                continue;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE,
                                   item.th32ProcessID);
            if (!p)
                continue;
            wchar_t path[32768];
            DWORD n = 32768;
            bool safe = false;
            try
            {
                safe = QueryFullProcessImageNameW(p, 0, path, &n) && lower(path) == lower(explorerPath()) &&
                       sameUser(p, GetCurrentProcess());
            }
            catch (...)
            {
            }
            FILETIME created{}, exit{}, kernel{}, user{};
            if (!safe || !GetProcessTimes(p, &created, &exit, &kernel, &user))
            {
                CloseHandle(p);
                continue;
            }
            apps.push_back({item.th32ProcessID, created});
            handles.push_back(p);
        } while (Process32NextW(snapshot.h, &item));
    if (apps.empty())
    {
        detail = L"No verified Explorer process / 확인된 Explorer 프로세스 없음";
        return false;
    }
    DWORD rm = 0;
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    DWORD result = RmStartSession(&rm, 0, key);
    bool started = result == ERROR_SUCCESS;
    bool forced = false;
    DWORD failed = 0;
    if (started)
    {
        result = RmRegisterResources(rm, 0, nullptr, static_cast<UINT>(apps.size()), apps.data(), 0, nullptr);
        if (result == ERROR_SUCCESS)
            result = RmShutdown(rm, 0, nullptr);
    }
    // The confirmation explicitly describes interruption of file operations and windows.
    for (auto p : handles)
    {
        if (WaitForSingleObject(p, 1500) == WAIT_TIMEOUT)
        {
            if (TerminateProcess(p, 0))
            {
                forced = true;
                WaitForSingleObject(p, 5000);
            }
            else
                failed = GetLastError();
        }
        CloseHandle(p);
    }
    if (started)
    {
        RmRestart(rm, 0, nullptr);
        RmEndSession(rm);
    }
    if (!waitShell(6000))
    {
        for (int attempt = 0; attempt < 3 && !validShell(); ++attempt)
        {
            launchMedium(explorerPath(), L"", token.h);
            waitShell(5000);
        }
    }
    if (!validShell())
    {
        detail = L"Explorer restart failed; recovery monitor is retrying / Explorer 재시작 실패, 복구 감시가 "
                 L"재시도 중입니다.\r\n" +
                 errorText(failed ? failed : result);
        return false;
    }
    if (failed)
    {
        detail = L"Some Explorer windows could not be closed / 일부 Explorer 창을 종료하지 못했습니다. " +
                 errorText(failed);
        return false;
    }
    if (forced)
        detail = L"Explorer restarted (forced fallback) / Explorer 재시작 완료 (강제 종료 대체 경로)";
    return true;
}
int recoverShell()
{
    // This helper is always launched with a verified medium integrity token.
    if (isElevated())
        return 1;
    unsigned absent = 0;
    for (unsigned i = 0; i < 120; ++i)
    {
        Sleep(500);
        if (validShell())
        {
            absent = 0;
            continue;
        }
        if (++absent >= 6)
        {
            launchMedium(explorerPath(), L"", nullptr);
            absent = 0;
        }
    }
    return validShell() ? 0 : 2;
}
} // namespace st

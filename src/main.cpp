#include "shelltidy.h"
#include <shellapi.h>
namespace st
{
int recoverShell();
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try
    {
        if (argc >= 3 && std::wstring(argv[1]) == L"--worker")
            result = st::elevatedWorker(argv[2]);
        else if (argc >= 2 && std::wstring(argv[1]) == L"--recover-shell")
            result = st::recoverShell();
#ifdef SHELLTIDY_SELF_TEST
        // Only the separate test build links the isolated registry tests.
        else if (argc >= 3 && std::wstring(argv[1]) == L"--self-test")
            result = st::selfTest(argv[2]);
#endif
        else
        {
            bool alreadyRunning = false;
            st::Handle instanceLock(st::createInstanceLock(L"UI", alreadyRunning));
            if (alreadyRunning || FindWindowW(L"ShellTidy.Main", nullptr))
            {
                st::activateExistingInstance();
                result = 0;
            }
            else
            {
                bool english = PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_KOREAN;
                UINT dpi = 0;
                for (int i = 1; i < argc; ++i)
                {
                    if (std::wstring(argv[i]) == L"--english")
                        english = true;
                    if (std::wstring(argv[i]) == L"--korean")
                        english = false;
                    if (std::wstring(argv[i]) == L"--test-dpi" && i + 1 < argc)
                        dpi = static_cast<UINT>(_wtoi(argv[++i]));
                }
                result = st::runUI(instance, show, english, dpi);
            }
        }
    }
    catch (const st::Error &e)
    {
        MessageBoxW(nullptr, st::errorText(e.code).c_str(), L"ShellTidy", MB_ICONERROR);
    }
    catch (...)
    {
        MessageBoxW(nullptr, L"ShellTidy could not complete the operation.", L"ShellTidy", MB_ICONERROR);
    }
    LocalFree(argv);
    if (SUCCEEDED(hr))
        CoUninitialize();
    return result;
}

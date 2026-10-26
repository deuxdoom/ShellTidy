#include "shelltidy.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <thread>
#include <memory>
#include <algorithm>

namespace st
{
namespace
{
constexpr UINT ScanDone = WM_APP + 1, WorkDone = WM_APP + 2;
enum
{
    List = 100,
    Search,
    Filter,
    Tab,
    Refresh,
    Delete,
    Restore,
    Backups,
    Restart,
    Details,
    StatusText
};
struct WorkResult
{
    Outcome outcome;
    std::wstring error;
};
class Window
{
    HINSTANCE instance;
    HWND hwnd = nullptr, list = nullptr, search = nullptr, filter = nullptr, tabs = nullptr,
         details = nullptr, status = nullptr;
    HWND refresh = nullptr, remove = nullptr, restoreButton = nullptr, backups = nullptr, restart = nullptr;
    HFONT font = nullptr, titleFont = nullptr, smallFont = nullptr;
    HIMAGELIST rowImages = nullptr;
    std::vector<HIMAGELIST> buttonImages;
    HBRUSH background = CreateSolidBrush(RGB(248, 249, 251));
    UINT dpi = 96, forcedDpi = 0;
    bool english = false, busy = false, closing = false, filling = false;
    std::atomic_bool cancel = false;
    std::thread worker;
    std::vector<Entry> entries;
    std::vector<size_t> visible;
    unsigned warnings = 0;
    int sortColumn = 0;
    bool sortAscending = true;
    const wchar_t *tr(const wchar_t *ko, const wchar_t *en) const
    {
        return english ? en : ko;
    }
    int px(int n) const
    {
        return MulDiv(n, dpi * 4, 96 * 5);
    }
    static std::wstring getText(HWND h)
    {
        int n = GetWindowTextLengthW(h);
        std::wstring s(n + 1, L'\0');
        GetWindowTextW(h, s.data(), n + 1);
        s.resize(n);
        return s;
    }
    const wchar_t *kind(Kind k) const
    {
        switch (k)
        {
        case Kind::OpenWith:
            return tr(L"연결 프로그램", L"Open With");
        case Kind::Verb:
            return tr(L"셸 메뉴", L"Shell verb");
        case Kind::Extension:
            return tr(L"셸 확장", L"Shell extension");
        default:
            return tr(L"최신/패키지 · 조회", L"Modern/package · inspect");
        }
    }
    const wchar_t *state(Status s) const
    {
        switch (s)
        {
        case Status::Healthy:
            return tr(L"정상", L"Healthy");
        case Status::Missing:
            return tr(L"경로 없음", L"File missing");
        case Status::Duplicate:
            return tr(L"중복 의심", L"Possible duplicate");
        case Status::Protected:
            return tr(L"시스템 보호", L"Protected");
        default:
            return tr(L"확인 필요", L"Needs review");
        }
    }
    HWND control(const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0)
    {
        auto h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0,
                                 hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!h)
            throw Error("Create control");
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return h;
    }
    void fonts()
    {
        if (font)
            DeleteObject(font);
        if (titleFont)
            DeleteObject(titleFont);
        if (smallFont)
            DeleteObject(smallFont);
        auto face = english ? L"Segoe UI" : L"Malgun Gothic";
        font = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                           0, CLEARTYPE_QUALITY, 0, face);
        titleFont = CreateFontW(-px(29), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                CLEARTYPE_QUALITY, 0, L"Segoe UI");
        smallFont = CreateFontW(-MulDiv(11, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
        if (list)
        {
            for (auto h : {list, search, filter, tabs, details, status, refresh, remove, restoreButton,
                           backups, restart})
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SendMessageW(details, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), TRUE);
            if (rowImages)
            {
                ListView_SetImageList(list, nullptr, LVSIL_SMALL);
                ImageList_Destroy(rowImages);
            }
            rowImages = ImageList_Create(1, px(30), ILC_COLOR32, 1, 1);
            ListView_SetImageList(list, rowImages, LVSIL_SMALL);
            for (auto image : buttonImages)
                ImageList_Destroy(image);
            buttonImages.clear();
            HWND buttons[] = {refresh, remove, restoreButton, backups};
            for (int i = 0; i < 4; ++i)
            {
                auto images = ImageList_Create(px(18), px(18), ILC_COLOR32 | ILC_MASK, 1, 1);
                auto icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(i + 2), IMAGE_ICON,
                                                          px(18), px(18), LR_DEFAULTCOLOR));
                ImageList_AddIcon(images, icon);
                if (icon)
                    DestroyIcon(icon);
                BUTTON_IMAGELIST config{};
                config.himl = images;
                config.margin = {px(8), 0, px(5), 0};
                config.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
                SendMessageW(buttons[i], BCM_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(&config));
                buttonImages.push_back(images);
            }
        }
    }
    void create()
    {
        dpi = forcedDpi ? forcedDpi : GetDpiForWindow(hwnd);
        fonts();
        auto systemMenu = GetSystemMenu(hwnd, FALSE);
        AppendMenuW(systemMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(systemMenu, MF_STRING, 0x1ff0, tr(L"ShellTidy 정보", L"About ShellTidy"));
        tabs = control(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_FOCUSNEVER, Tab);
        const wchar_t *labels[] = {tr(L"연결 프로그램", L"Open With"), tr(L"우클릭 메뉴", L"Context menus"),
                                   tr(L"전체", L"All entries")};
        for (int i = 0; i < 3; ++i)
        {
            TCITEMW t{};
            t.mask = TCIF_TEXT;
            t.pszText = const_cast<LPWSTR>(labels[i]);
            TabCtrl_InsertItem(tabs, i, &t);
        }
        search = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Search, WS_EX_CLIENTEDGE);
        SendMessageW(
            search, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(tr(L"프로그램, 경로, 확장자 검색", L"Search apps, paths, extensions")));
        SetWindowTextW(search, L"");
        filter = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, Filter);
        for (auto s : {tr(L"전체 상태", L"All statuses"), tr(L"중복 의심", L"Possible duplicates"),
                       tr(L"실행 파일 없음", L"Missing executable"), tr(L"확인 필요", L"Needs review")})
            SendMessageW(filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessageW(filter, CB_SETCURSEL, 0, 0);
        list =
            control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, List, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                                    LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP | LVS_EX_LABELTIP);
        SetWindowTheme(list, L"Explorer", nullptr);
        ListView_SetBkColor(list, RGB(255, 255, 255));
        ListView_SetTextBkColor(list, RGB(255, 255, 255));
        const wchar_t *cols[] = {tr(L"프로그램", L"Program"), tr(L"유형", L"Type"), tr(L"상태", L"Status"),
                                 tr(L"실행 경로", L"Executable path"), tr(L"등록 위치", L"Source")};
        for (int i = 0; i < 5; ++i)
        {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<LPWSTR>(cols[i]);
            c.cx = px(150);
            ListView_InsertColumn(list, i, &c);
        }
        details =
            control(L"EDIT",
                    tr(L"항목을 선택하면 확장자, ProgID와 실제 레지스트리 위치를 볼 수 있습니다.",
                       L"Select an entry to inspect extensions, ProgIDs and exact registry locations."),
                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, Details, WS_EX_CLIENTEDGE);
        status = control(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, StatusText);
        refresh = control(L"BUTTON", tr(L"새로고침", L"Refresh"), WS_TABSTOP | BS_PUSHBUTTON, Refresh);
        remove =
            control(L"BUTTON", tr(L"선택 항목 삭제", L"Delete selected"), WS_TABSTOP | BS_PUSHBUTTON, Delete);
        restoreButton =
            control(L"BUTTON", tr(L"마지막 작업 복원", L"Restore last"), WS_TABSTOP | BS_PUSHBUTTON, Restore);
        backups = control(L"BUTTON", tr(L"백업 목록", L"Backups"), WS_TABSTOP | BS_PUSHBUTTON, Backups);
        restart =
            control(L"BUTTON",
                    tr(L"정리 완료 후 Windows 탐색기 자동 재시작", L"Restart Windows Explorer after changes"),
                    WS_TABSTOP | BS_AUTOCHECKBOX, Restart);
        SendMessageW(restart, BM_SETCHECK, BST_UNCHECKED, 0);
        fonts();
        BOOL dark = FALSE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        layout();
        startScan();
    }
    void move(HWND h, int x, int y, int w, int height)
    {
        MoveWindow(h, px(x), px(y), px(w), px(height), TRUE);
    }
    void layout()
    {
        if (!list)
            return;
        RECT r;
        GetClientRect(hwnd, &r);
        int w = MulDiv(r.right, 96 * 5, dpi * 4), h = MulDiv(r.bottom, 96 * 5, dpi * 4);
        int margin = 24;
        move(tabs, margin, 99, 440, 32);
        move(search, w - 522, 98, 300, 32);
        move(filter, w - 210, 98, 186, 260);
        int listHeight = std::max(120, h - 390);
        move(list, margin, 146, w - margin * 2, listHeight);
        int below = 146 + listHeight;
        move(details, margin, below + 12, w - margin * 2, 112);
        move(status, margin, below + 133, w - margin * 2, 24);
        move(restart, margin, h - 83, w - margin * 2, 26);
        move(refresh, margin, h - 43, 112, 32);
        move(remove, 148, h - 43, 158, 32);
        move(restoreButton, w - 324, h - 43, 172, 32);
        move(backups, w - 140, h - 43, 116, 32);
        ListView_SetColumnWidth(list, 0, px(180));
        ListView_SetColumnWidth(list, 1, px(150));
        ListView_SetColumnWidth(list, 2, px(144));
        ListView_SetColumnWidth(list, 4, px(104));
        ListView_SetColumnWidth(
            list, 3, std::max(px(230), px(w - 48 - 180 - 150 - 144 - 104) - GetSystemMetrics(SM_CXVSCROLL)));
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    void setBusy(bool value)
    {
        busy = value;
        SetWindowTextW(refresh, value ? tr(L"취소", L"Cancel") : tr(L"새로고침", L"Refresh"));
        for (auto h : {remove, restoreButton, backups, search, filter, tabs, restart, list})
            EnableWindow(h, !value);
    }
    void statusText()
    {
        if (busy)
            return;
        unsigned checked = 0;
        for (const auto &e : entries)
            if (e.checked)
                ++checked;
        auto s = std::to_wstring(visible.size()) + tr(L"개 표시 / 전체 ", L" shown / ") +
                 std::to_wstring(entries.size()) + tr(L"개  ·  선택 ", L" total  ·  selected ") +
                 std::to_wstring(checked);
        if (warnings)
            s += tr(L"  ·  읽기 경고 ", L"  ·  read warnings ") + std::to_wstring(warnings);
        SetWindowTextW(status, s.c_str());
        EnableWindow(remove, checked > 0);
    }
    void populate()
    {
        if (!list)
            return;
        size_t selectedEntry = SIZE_MAX;
        int selectedRow = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        if (selectedRow >= 0 && static_cast<size_t>(selectedRow) < visible.size())
            selectedEntry = visible[selectedRow];
        auto q = lower(getText(search));
        int tab = TabCtrl_GetCurSel(tabs);
        int f = static_cast<int>(SendMessageW(filter, CB_GETCURSEL, 0, 0));
        filling = true;
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list);
        visible.clear();
        for (size_t i = 0; i < entries.size(); ++i)
        {
            auto &e = entries[i];
            if (tab == 0 && e.kind != Kind::OpenWith)
                continue;
            if (tab == 1 && e.kind == Kind::OpenWith)
                continue;
            if (f == 1 && e.status != Status::Duplicate)
                continue;
            if (f == 2 && e.status != Status::Missing)
                continue;
            if (f == 3 && e.status != Status::Review)
                continue;
            if (!q.empty())
            {
                std::wstring hay = e.name + L" " + e.executable + L" " + e.command + L" " + e.source;
                for (const auto &x : e.extensions)
                    hay += L" " + x;
                for (const auto &x : e.progids)
                    hay += L" " + x;
                for (const auto &t : e.targets)
                    hay += L" " + t.path;
                if (lower(hay).find(q) == std::wstring::npos)
                    continue;
            }
            visible.push_back(i);
        }
        auto label = [this](const Entry &e) -> std::wstring
        {
            switch (sortColumn)
            {
            case 1:
                return kind(e.kind);
            case 2:
                return state(e.status);
            case 3:
                return e.executable.empty() ? e.command : e.executable;
            case 4:
                return e.source;
            default:
                return e.name;
            }
        };
        std::stable_sort(visible.begin(), visible.end(),
                         [&](size_t a, size_t b)
                         {
                             int cmp = compareLabels(label(entries[a]), label(entries[b]));
                             if (!cmp)
                                 cmp = compareLabels(entries[a].name, entries[b].name);
                             if (!cmp)
                                 cmp = compareLabels(entries[a].executable, entries[b].executable);
                             return sortAscending ? cmp < 0 : cmp > 0;
                         });
        int selection = -1;
        for (int row = 0; row < static_cast<int>(visible.size()); ++row)
        {
            auto &e = entries[visible[row]];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = e.name.data();
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, row, 1, const_cast<LPWSTR>(kind(e.kind)));
            ListView_SetItemText(list, row, 2, const_cast<LPWSTR>(state(e.status)));
            auto path = e.executable.empty() ? e.command : e.executable;
            if (path.empty())
                path = tr(L"해석할 수 없음", L"Unresolved");
            ListView_SetItemText(list, row, 3, path.data());
            ListView_SetItemText(list, row, 4, e.source.data());
            ListView_SetCheckState(list, row, e.checked);
            if (visible[row] == selectedEntry)
            {
                selection = row;
                ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
        auto header = ListView_GetHeader(list);
        for (int column = 0; column < 5; ++column)
        {
            HDITEMW item{};
            item.mask = HDI_FORMAT;
            Header_GetItem(header, column, &item);
            item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (column == sortColumn)
                item.fmt |= sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
            Header_SetItem(header, column, &item);
        }
        if (selection >= 0)
            showDetails(selection);
        else
            SetWindowTextW(
                details, tr(L"항목을 선택하면 확장자, ProgID와 실제 레지스트리 위치를 볼 수 있습니다.",
                            L"Select an entry to inspect extensions, ProgIDs and exact registry locations."));
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        filling = false;
        statusText();
    }
    void startScan()
    {
        if (busy)
            return;
        if (worker.joinable())
            worker.join();
        cancel = false;
        setBusy(true);
        SetWindowTextW(status, tr(L"레지스트리 검색 중… UI는 계속 사용할 수 있습니다.",
                                  L"Scanning registry… You can cancel or close the window."));
        worker = std::thread(
            [this]
            {
                auto r = std::make_unique<ScanResult>();
                try
                {
                    *r = scan({}, cancel);
                }
                catch (...)
                {
                    ++r->warnings;
                }
                PostMessageW(hwnd, ScanDone, 0, reinterpret_cast<LPARAM>(r.release()));
            });
    }
    struct PopupData
    {
        std::wstring copy;
    };
    static HRESULT CALLBACK popupCallback(HWND dialog, UINT notice, WPARAM button, LPARAM, LONG_PTR data)
    {
        if (notice == TDN_CREATED)
        {
            RECT r;
            GetWindowRect(dialog, &r);
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &info);
            auto centered = centeredRect(info.rcWork, r.right - r.left, r.bottom - r.top);
            SetWindowPos(dialog, nullptr, centered.left, centered.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (notice == TDN_BUTTON_CLICKED && button == 6001)
        {
            auto context = reinterpret_cast<PopupData *>(data);
            auto size = (context->copy.size() + 1) * sizeof(wchar_t);
            auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
            if (memory)
            {
                auto text = GlobalLock(memory);
                if (text)
                {
                    memcpy(text, context->copy.c_str(), size);
                    GlobalUnlock(memory);
                    if (OpenClipboard(dialog))
                    {
                        EmptyClipboard();
                        if (SetClipboardData(CF_UNICODETEXT, memory))
                            memory = nullptr;
                        CloseClipboard();
                    }
                }
                if (memory)
                    GlobalFree(memory);
            }
            return S_FALSE;
        }
        return S_OK;
    }
    void message(const std::wstring &text, bool error = false, const std::wstring &full = L"")
    {
        auto content = popupSummary(text);
        PopupData context{full.empty() ? text : full};
        TASKDIALOG_BUTTON buttons[] = {{IDOK, tr(L"확인", L"OK")}, {6001, tr(L"상세 복사", L"Copy details")}};
        TASKDIALOGCONFIG c{sizeof(c)};
        c.hwndParent = hwnd;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.cxWidth = 280;
        c.pszWindowTitle = L"ShellTidy";
        c.pszMainInstruction =
            error ? tr(L"작업 결과를 확인하세요", L"Review the operation result") : L"ShellTidy";
        c.pszContent = content.c_str();
        c.pszMainIcon = error ? TD_WARNING_ICON : TD_INFORMATION_ICON;
        c.pButtons = buttons;
        c.cButtons = 2;
        c.nDefaultButton = IDOK;
        c.pfCallback = popupCallback;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(&context);
        TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
    }
    bool confirm(const std::wstring &title, const std::wstring &summary, const std::wstring &expanded)
    {
        auto warning = std::wstring(tr(
            L"선택한 레지스트리 등록만 변경하며 앱 파일과 문서는 보존합니다.",
            L"Only the selected registry registrations are changed. App files and documents are preserved."));
        if (SendMessageW(restart, BM_GETCHECK, 0, 0) == BST_CHECKED)
            warning += tr(L"\n\nExplorer가 자동 재시작됩니다. 파일 복사·이동 작업 및 열린 탐색기 창이 중단될 "
                          L"수 있으니 먼저 완료해 주세요.",
                          L"\n\nExplorer will restart automatically. File copies, moves and open Explorer "
                          L"windows can be interrupted. Finish those operations first.");
        TASKDIALOG_BUTTON buttons[] = {{IDYES, tr(L"진행", L"Continue")},
                                       {IDCANCEL, tr(L"취소", L"Cancel")},
                                       {6001, tr(L"대상 목록 복사", L"Copy targets")}};
        PopupData context{expanded};
        TASKDIALOGCONFIG c{sizeof(c)};
        c.hwndParent = hwnd;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.cxWidth = 280;
        c.pszWindowTitle = L"ShellTidy";
        c.pszMainInstruction = title.c_str();
        auto content = popupSummary(summary, 140, 3) + L"\n\n" + warning;
        c.pszContent = content.c_str();
        c.pButtons = buttons;
        c.cButtons = 3;
        c.pfCallback = popupCallback;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(&context);
        c.nDefaultButton = IDCANCEL;
        c.pszMainIcon = TD_WARNING_ICON;
        int selected = 0;
        TaskDialogIndirect(&c, &selected, nullptr, nullptr);
        return selected == IDYES;
    }
    bool needsElevation(const Journal &j)
    {
        if (isElevated())
            return false;
        for (const auto &m : j.items)
        {
            try
            {
                auto t = m.target;
                if (t.tree)
                {
                    auto cut = t.path.find_last_of(L'\\');
                    t.path = t.path.substr(0, cut);
                }
                auto key = openKey(t, KEY_WRITE | (m.target.tree ? DELETE : 0));
            }
            catch (const Error &e)
            {
                if (e.code == ERROR_ACCESS_DENIED)
                    return true;
                throw;
            }
        }
        return false;
    }
    void work(bool restoring, std::wstring file = {})
    {
        if (busy)
            return;
        std::vector<Entry> selected;
        for (const auto &e : entries)
            if (e.checked)
                selected.push_back(e);
        if (!restoring && selected.empty())
            return;
        std::wstring locations;
        unsigned count = 0;
        unsigned userCount = 0, machineCount = 0;
        std::set<std::wstring> counted;
        auto countTarget = [&](const Target &original)
        {
            auto t = canonicalTarget(original);
            if (counted.insert(lower(describe(t))).second)
            {
                ++count;
                if (t.hive == Hive::User)
                    ++userCount;
                else
                    ++machineCount;
            }
        };
        try
        {
            if (restoring)
            {
                auto j = loadJournal(file);
                for (const auto &m : j.items)
                    if (m.phase == 1 || m.phase == 2)
                    {
                        countTarget(m.target);
                        locations += describe(m.target) + L"\n";
                    }
                if (!count)
                {
                    message(tr(L"이 백업에는 복원할 변경이 없습니다.",
                               L"This backup has no pending changes to restore."));
                    return;
                }
            }
            else
                for (const auto &e : selected)
                {
                    locations += e.name + L"\n";
                    for (const auto &t : e.targets)
                    {
                        countTarget(t);
                        locations += L"  " + describe(t) + L"\n";
                    }
                }
        }
        catch (const Error &e)
        {
            message(errorText(e.code), true);
            return;
        }
        auto summary = restoring
                           ? std::filesystem::path(file).filename().wstring() + L"\n"
                           : std::to_wstring(selected.size()) + tr(L"개 선택 항목\n", L" selected entries\n");
        summary += std::to_wstring(count) + tr(L"개 레지스트리 대상", L" registry targets");
        summary += L"\nHKCU " + std::to_wstring(userCount) + L" · HKLM " + std::to_wstring(machineCount);
        if (!confirm(restoring ? tr(L"백업한 등록을 복원할까요?", L"Restore the backed-up registrations?")
                               : tr(L"선택한 등록을 실제로 삭제할까요?",
                                    L"Permanently remove these registrations?"),
                     summary, locations))
            return;
        bool autoRestart = SendMessageW(restart, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (autoRestart)
        {
            std::wstring detail;
            if (!explorerPreflight(detail))
            {
                message(detail, true);
                return;
            }
        }
        if (worker.joinable())
            worker.join();
        cancel = false;
        setBusy(true);
        SetWindowTextW(status, restoring ? tr(L"충돌 검사 및 복원 중…", L"Checking conflicts and restoring…")
                                         : tr(L"백업 및 선택 등록 삭제 중…",
                                              L"Backing up and deleting selected registrations…"));
        worker = std::thread(
            [this, restoring, file, selected = std::move(selected), autoRestart]
            {
                auto result = std::make_unique<WorkResult>();
                try
                {
                    auto j = restoring ? loadJournal(file) : prepare(selected);
                    if (cancel)
                        throw Error("Canceled", ERROR_CANCELLED);
                    if (needsElevation(j))
                        result->outcome = executeElevated(selected, restoring, file, hwnd);
                    else
                        result->outcome = restoring ? restore(j, cancel) : st::apply(j, cancel);
                    auto &o = result->outcome;
                    if (o.success)
                    {
                        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
                        if (autoRestart)
                        {
                            std::wstring d;
                            o.restartOK = restartExplorer(d);
                            if (!d.empty())
                                o.detail += L"\r\n" + d;
                        }
                    }
                }
                catch (const Error &e)
                {
                    result->error = errorText(e.code);
                }
                catch (const std::exception &)
                {
                    result->error =
                        tr(L"파일 또는 백업 작업에 실패했습니다. 작업한 항목의 백업을 확인하세요.",
                           L"A file or backup operation failed. Check the backup for any completed changes.");
                }
                PostMessageW(hwnd, WorkDone, 0, reinterpret_cast<LPARAM>(result.release()));
            });
    }
    void chooseBackup(bool last)
    {
        try
        {
            auto files = backupFiles();
            if (files.empty())
            {
                message(tr(L"저장된 백업이 없습니다.", L"No saved backups."));
                return;
            }
            if (last)
            {
                for (const auto &f : files)
                {
                    auto j = loadJournal(f);
                    if (std::any_of(j.items.begin(), j.items.end(),
                                    [](const Mutation &m) { return m.phase == 1 || m.phase == 2; }))
                    {
                        work(true, f);
                        return;
                    }
                }
                message(tr(L"복원할 마지막 작업이 없습니다.", L"No pending operation to restore."));
                return;
            }
            HMENU menu = CreatePopupMenu();
            size_t n = std::min<size_t>(files.size(), 30);
            for (size_t i = 0; i < n; ++i)
                AppendMenuW(menu, MF_STRING, 1000 + i, std::filesystem::path(files[i]).filename().c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2000, tr(L"백업 폴더 열기", L"Open backup folder"));
            RECT r;
            GetWindowRect(backups, &r);
            int choice =
                TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN, r.right, r.top, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (choice >= 1000 && choice < 1000 + static_cast<int>(n))
                work(true, files[choice - 1000]);
            else if (choice == 2000)
                ShellExecuteW(hwnd, L"open", backupDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        catch (const Error &e)
        {
            message(errorText(e.code), true);
        }
        catch (...)
        {
            message(tr(L"백업 목록을 읽을 수 없습니다.", L"Could not read the backup list."), true);
        }
    }
    void showDetails(int row)
    {
        if (row < 0 || row >= static_cast<int>(visible.size()))
            return;
        auto &e = entries[visible[row]];
        std::wstring s = e.name + L"  |  " + state(e.status) + L"\r\n";
        s += tr(L"명령: ", L"Command: ") + e.command + L"\r\n";
        s += tr(L"확장자: ", L"Extensions: ");
        for (const auto &ext : e.extensions)
            s += ext + L" ";
        s += L"\r\nProgID: ";
        for (const auto &p : e.progids)
            s += p + L" ";
        s += L"\r\n";
        if (!e.reason.empty())
            s += e.reason + L"\r\n";
        for (const auto &t : e.targets)
            s += describe(t) + L"\r\n";
        SetWindowTextW(details, s.c_str());
    }
    LRESULT notify(NMHDR *n)
    {
        if (n->idFrom == Tab && n->code == TCN_SELCHANGE)
        {
            populate();
            return 0;
        }
        if (n->idFrom != List)
            return 0;
        if (n->code == LVN_COLUMNCLICK && !busy)
        {
            auto click = reinterpret_cast<NMLISTVIEW *>(n);
            if (click->iSubItem == sortColumn)
                sortAscending = !sortAscending;
            else
            {
                sortColumn = click->iSubItem;
                sortAscending = true;
            }
            populate();
            return 0;
        }
        if (n->code == LVN_ITEMCHANGING && !filling)
        {
            auto v = reinterpret_cast<NMLISTVIEW *>(n);
            if (v->iItem >= 0 && v->iItem < static_cast<int>(visible.size()) && (v->uChanged & LVIF_STATE) &&
                ((v->uNewState ^ v->uOldState) & LVIS_STATEIMAGEMASK))
            {
                auto &e = entries[visible[v->iItem]];
                if ((e.protectedItem || e.unsupported) &&
                    (v->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2))
                    return TRUE;
            }
        }
        if (n->code == LVN_ITEMCHANGED && !filling)
        {
            auto v = reinterpret_cast<NMLISTVIEW *>(n);
            if (v->iItem >= 0 && v->iItem < static_cast<int>(visible.size()))
            {
                entries[visible[v->iItem]].checked = ListView_GetCheckState(list, v->iItem) != FALSE;
                if (v->uNewState & LVIS_SELECTED)
                    showDetails(v->iItem);
                statusText();
            }
        }
        if (n->code == NM_CUSTOMDRAW)
        {
            auto d = reinterpret_cast<NMLVCUSTOMDRAW *>(n);
            if (d->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (d->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && d->nmcd.dwItemSpec < visible.size())
            {
                auto &e = entries[visible[d->nmcd.dwItemSpec]];
                if (e.protectedItem || e.unsupported)
                    d->clrText = RGB(118, 124, 134);
                else if (e.status == Status::Missing)
                    d->clrText = RGB(155, 64, 48);
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    }
    void paint()
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(dc, &ps.rcPaint, background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(27, 36, 49));
        auto old = SelectObject(dc, titleFont);
        RECT title{px(24), px(18), r.right - px(24), px(59)};
        DrawTextW(dc, L"ShellTidy", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, font);
        SetTextColor(dc, RGB(91, 103, 119));
        RECT sub{px(25), px(62), r.right - px(24), px(88)};
        DrawTextW(dc,
                  tr(L"불필요한 연결 프로그램과 우클릭 메뉴를 정리합니다.",
                     L"Clean up unwanted Open With registrations and context menus."),
                  -1, &sub, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == 0x1ff0)
            {
                std::wstring licenses;
                for (int id : {101, 102})
                {
                    auto resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
                    if (!resource)
                        continue;
                    auto raw = static_cast<const char *>(LockResource(LoadResource(instance, resource)));
                    auto size = static_cast<int>(SizeofResource(instance, resource));
                    auto count = MultiByteToWideChar(CP_UTF8, 0, raw, size, nullptr, 0);
                    std::wstring value(count, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, raw, size, value.data(), count);
                    licenses += value + L"\n\n";
                }
                message(L"ShellTidy 0.0.1\nhttps://github.com/deuxdoom/ShellTidy\n\n" + licenses);
                return 0;
            }
            break;
        case WM_CREATE:
            create();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED:
            if (!forcedDpi)
            {
                dpi = HIWORD(w);
                fonts();
                auto r = reinterpret_cast<RECT *>(l);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                layout();
            }
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto p = reinterpret_cast<MINMAXINFO *>(l);
            p->ptMinTrackSize = {px(1040), px(660)};
            return 0;
        }
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(w), RGB(248, 249, 251));
            SetTextColor(reinterpret_cast<HDC>(w), RGB(50, 61, 76));
            return reinterpret_cast<LRESULT>(background);
        case WM_NOTIFY:
            return notify(reinterpret_cast<NMHDR *>(l));
        case WM_COMMAND:
        {
            int id = LOWORD(w);
            if (id == Search && HIWORD(w) == EN_CHANGE)
                populate();
            else if (id == Filter && HIWORD(w) == CBN_SELCHANGE)
                populate();
            else if (id == Refresh)
            {
                if (busy)
                {
                    cancel = true;
                    SetWindowTextW(status, tr(L"현재 안전한 단계까지 마친 후 취소합니다…",
                                              L"Canceling after the current safe step…"));
                }
                else
                    startScan();
            }
            else if (id == Delete)
                work(false);
            else if (id == Restore)
                chooseBackup(true);
            else if (id == Backups)
                chooseBackup(false);
            return 0;
        }
        case ScanDone:
        {
            if (worker.joinable())
                worker.join();
            std::unique_ptr<ScanResult> r(reinterpret_cast<ScanResult *>(l));
            setBusy(false);
            if (!r->canceled)
            {
                entries = std::move(r->entries);
                warnings = r->warnings;
            }
            populate();
            if (closing)
                DestroyWindow(hwnd);
            return 0;
        }
        case WorkDone:
        {
            if (worker.joinable())
                worker.join();
            std::unique_ptr<WorkResult> r(reinterpret_cast<WorkResult *>(l));
            setBusy(false);
            auto &o = r->outcome;
            std::wstring text = r->error;
            std::wstring full;
            if (text.empty())
            {
                text = tr(L"변경 성공: ", L"Changes completed: ") + std::to_wstring(o.success) +
                       tr(L"\n실패: ", L"\nFailed: ") + std::to_wstring(o.failed) +
                       tr(L"\n복원 충돌: ", L"\nRestore conflicts: ") + std::to_wstring(o.conflicts);
                if (!o.backup.empty())
                    text += tr(L"\n백업이 저장되었습니다.", L"\nBackup saved.");
                if (o.failed || o.conflicts || !o.restartOK)
                    text += tr(L"\n상세 복사에서 전체 오류와 위치를 확인하세요.",
                               L"\nCopy details for full errors and locations.");
                full = text + L"\n\n" + o.backup + L"\n\n" + o.detail;
            }
            if (!closing)
                message(text, !r->error.empty() || o.failed || o.conflicts || !o.restartOK, full);
            for (auto &e : entries)
                e.checked = false;
            if (closing)
                DestroyWindow(hwnd);
            else
                startScan();
            return 0;
        }
        case WM_CLOSE:
            if (busy)
            {
                closing = true;
                cancel = true;
                SetWindowTextW(status, tr(L"안전하게 작업을 마친 후 종료합니다…",
                                          L"Finishing the current safe step before closing…"));
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, m, w, l);
    }
    static LRESULT CALLBACK dispatch(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        auto self = reinterpret_cast<Window *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE)
        {
            self = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        try
        {
            return self->proc(m, w, l);
        }
        catch (const Error &e)
        {
            MessageBoxW(h, errorText(e.code).c_str(), L"ShellTidy", MB_ICONERROR);
            return m == WM_CREATE ? -1 : 0;
        }
        catch (...)
        {
            MessageBoxW(h, L"Unexpected error / 오류가 발생했습니다.", L"ShellTidy", MB_ICONERROR);
            return m == WM_CREATE ? -1 : 0;
        }
    }

  public:
    Window(HINSTANCE i, bool en, UINT d) : instance(i), forcedDpi(d), english(en) {}
    ~Window()
    {
        cancel = true;
        if (worker.joinable())
            worker.join();
        if (rowImages)
            ImageList_Destroy(rowImages);
        for (auto images : buttonImages)
            ImageList_Destroy(images);
        if (font)
            DeleteObject(font);
        if (titleFont)
            DeleteObject(titleFont);
        if (smallFont)
            DeleteObject(smallFont);
        DeleteObject(background);
    }
    int run(int show)
    {
        WNDCLASSEXW c{sizeof(c)};
        c.lpfnWndProc = dispatch;
        c.hInstance = instance;
        c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        c.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        if (!c.hIcon)
            c.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        c.lpszClassName = L"ShellTidy.Main";
        RegisterClassExW(&c);
        dpi = forcedDpi ? forcedDpi : GetDpiForSystem();
        RECT r{0, 0, px(1180), px(760)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        auto h = CreateWindowExW(0, c.lpszClassName, L"ShellTidy", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr,
                                 nullptr, instance, this);
        if (!h)
            return 1;
        POINT pointer{};
        GetCursorPos(&pointer);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST), &monitor);
        for (int pass = 0; pass < 2; ++pass)
        {
            RECT size{0, 0, px(1180), px(760)};
            AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
            auto position = centeredRect(monitor.rcWork, size.right - size.left, size.bottom - size.top);
            SetWindowPos(h, nullptr, position.left, position.top, position.right - position.left,
                         position.bottom - position.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(h, show);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(h, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return static_cast<int>(msg.wParam);
    }
};
} // namespace
int runUI(HINSTANCE instance, int show, bool english, UINT testDpi)
{
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&cc);
    Window window(instance, english, testDpi);
    return window.run(show);
}
} // namespace st

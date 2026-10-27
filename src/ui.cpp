#include "shelltidy.h"
#include <commctrl.h>
#include <commdlg.h>
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
constexpr UINT_PTR SearchTimer = 1;
enum : int
{
    List = 100,
    Search,
    TypeFilter,
    StatusFilter,
    Toolbar,
    Details,
    // Commands shared by the menu bar, toolbar, keyboard shortcuts and the row menu.
    CmdRefresh = 200,
    CmdDelete,
    CmdDisable,
    CmdEnable,
    CmdRestoreLast,
    CmdRestoreFrom,
    CmdCleanBackups,
    CmdOpenBackups,
    CmdSave,
    CmdExit,
    CmdCheck,
    CmdUncheck,
    CmdCheckProblems,
    CmdUncheckAll,
    CmdSelectAll,
    CmdCopy,
    CmdFind,
    CmdProperties,
    CmdRegistry,
    CmdLocation,
    CmdHideProtected,
    CmdHideMicrosoft,
    CmdShowPackaged,
    CmdShowDetails,
    CmdAutoRestart,
    CmdAbout,
    CmdBackupFirst = 1000
};
constexpr int Columns = 10;
constexpr int ColumnWidths[Columns] = {200, 116, 116, 84, 300, 120, 170, 140, 96, 380};
struct WorkResult
{
    Outcome outcome;
    std::wstring error;
};
class Window
{
    HINSTANCE instance;
    HWND hwnd = nullptr, toolbar = nullptr, list = nullptr, search = nullptr, typeFilter = nullptr,
         statusFilter = nullptr, details = nullptr;
    HFONT font = nullptr;
    HIMAGELIST rowImages = nullptr, toolImages = nullptr;
    // Windows 11 light surfaces: layered background, white content and hairline borders.
    HBRUSH background = CreateSolidBrush(RGB(243, 243, 243)), surface = CreateSolidBrush(RGB(255, 255, 255)),
           border = CreateSolidBrush(RGB(224, 224, 224));
    UINT dpi = 96, forcedDpi = 0;
    bool english = false, busy = false, closing = false, filling = false;
    bool hideProtected = false, hideMicrosoft = false, showPackaged = false, showDetails = true,
         autoRestart = false;
    std::atomic_bool cancel = false;
    std::thread worker;
    std::vector<Entry> entries;
    std::vector<size_t> visible;
    unsigned warnings = 0;
    int sortColumn = 2;
    bool sortAscending = true;
    RECT listFrame{}, detailsFrame{}, statusFrame{};
    POINT menuAnchor{};
    std::wstring statusLine;
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
    static std::wstring number(size_t n)
    {
        auto s = std::to_wstring(n);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
            s.insert(static_cast<size_t>(i), L",");
        return s;
    }
    std::wstring kindText(const Entry &e) const
    {
        if (e.packaged)
            return tr(L"패키지 COM", L"Packaged COM");
        switch (e.kind)
        {
        case Kind::OpenWith:
            return tr(L"연결 프로그램", L"Open With");
        case Kind::Verb:
            return tr(L"셸 메뉴", L"Shell verb");
        case Kind::Extension:
            return tr(L"셸 확장", L"Shell extension");
        default:
            return tr(L"최신 메뉴", L"Modern menu");
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
        case Status::Orphan:
            return tr(L"등록 없음", L"Not registered");
        case Status::Duplicate:
            return tr(L"중복 의심", L"Possible duplicate");
        case Status::Protected:
            return tr(L"시스템 보호", L"Protected");
        default:
            return tr(L"확인 필요", L"Needs review");
        }
    }
    static int rank(Status s)
    {
        switch (s)
        {
        case Status::Missing:
            return 0;
        case Status::Orphan:
            return 1;
        case Status::Review:
            return 2;
        case Status::Duplicate:
            return 3;
        case Status::Healthy:
            return 4;
        default:
            return 5;
        }
    }
    static bool problem(const Entry &e)
    {
        return e.status == Status::Missing || e.status == Status::Orphan;
    }
    static bool checkable(const Entry &e)
    {
        return eligible(e, Action::Delete) || e.switchable;
    }
    std::wstring switchText(const Entry &e) const
    {
        if (e.disabled)
            return tr(L"사용 안 함", L"Disabled");
        return e.switchable ? tr(L"사용", L"Enabled") : L"";
    }
    std::wstring pathText(const Entry &e) const
    {
        auto path = e.executable.empty() ? e.command : e.executable;
        if (!path.empty())
            return path;
        if (e.orphan && !e.progids.empty())
            return std::wstring(tr(L"(등록 없음) ", L"(not registered) ")) + *e.progids.begin();
        return tr(L"해석할 수 없음", L"Unresolved");
    }
    static std::wstring extensionText(const Entry &e)
    {
        std::wstring s;
        size_t n = 0;
        for (const auto &x : e.extensions)
        {
            if (++n > 12)
            {
                s += L" …";
                break;
            }
            s += (s.empty() ? L"" : L" ") + x;
        }
        return s;
    }
    std::wstring registryText(const Entry &e) const
    {
        if (e.targets.empty())
            return {};
        const auto &t = e.targets.front();
        auto s = registryPath(t);
        if (!t.tree)
            s += L" → " + (t.name.empty() ? std::wstring(tr(L"(기본값)", L"(Default)")) : t.name);
        if (e.targets.size() > 1)
            s += L"  (+" + number(e.targets.size() - 1) + L")";
        return s;
    }
    const wchar_t *columnName(int column) const
    {
        const wchar_t *ko[] = {L"이름",   L"유형", L"상태", L"사용 여부", L"명령 / 파일 경로",
                               L"확장자", L"설명", L"회사", L"등록 위치", L"레지스트리 키"};
        const wchar_t *en[] = {L"Name",       L"Type",        L"Status",  L"Enabled", L"Command / file path",
                               L"Extensions", L"Description", L"Company", L"Source",  L"Registry key"};
        return english ? en[column] : ko[column];
    }
    std::wstring cell(const Entry &e, int column) const
    {
        switch (column)
        {
        case 0:
            return e.name;
        case 1:
            return kindText(e);
        case 2:
            return state(e.status);
        case 3:
            return switchText(e);
        case 4:
            return pathText(e);
        case 5:
            return extensionText(e);
        case 6:
            return e.description;
        case 7:
            return e.company;
        case 8:
            return e.source;
        default:
            return registryText(e);
        }
    }
    HWND control(const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0)
    {
        auto h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0,
                                 hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!h)
            throw Error("Create control");
        if (font)
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return h;
    }
    void fonts()
    {
        auto face = english ? L"Segoe UI" : L"Malgun Gothic";
        auto next = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
        for (auto h : {toolbar, search, typeFilter, statusFilter, list, details})
            if (h)
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        if (font)
            DeleteObject(font);
        font = next;
        if (list)
        {
            // A one-pixel image list sets a comfortable row height without drawing icons.
            ListView_SetImageList(list, nullptr, LVSIL_SMALL);
            if (rowImages)
                ImageList_Destroy(rowImages);
            rowImages = ImageList_Create(1, px(30), ILC_COLOR32, 1, 1);
            ListView_SetImageList(list, rowImages, LVSIL_SMALL);
        }
        if (toolbar)
        {
            int size = MulDiv(16, dpi, 96);
            auto images = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 6, 1);
            for (int id = 2; id <= 7; ++id)
            {
                auto icon = static_cast<HICON>(
                    LoadImageW(instance, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
                ImageList_AddIcon(images, icon);
                if (icon)
                    DestroyIcon(icon);
            }
            SendMessageW(toolbar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
            if (toolImages)
                ImageList_Destroy(toolImages);
            toolImages = images;
            SendMessageW(toolbar, TB_SETPADDING, 0, MAKELPARAM(px(14), px(10)));
            SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
        }
    }
    void buildMenu()
    {
        auto item = [this](HMENU m, int id, const wchar_t *ko, const wchar_t *en)
        { AppendMenuW(m, MF_STRING, static_cast<UINT_PTR>(id), tr(ko, en)); };
        auto file = CreatePopupMenu(), edit = CreatePopupMenu(), view = CreatePopupMenu(),
             options = CreatePopupMenu(), help = CreatePopupMenu();
        item(file, CmdSave, L"선택 항목 보고서 저장(&S)…\tCtrl+S", L"&Save Selected Items…\tCtrl+S");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        item(file, CmdRestoreLast, L"마지막 작업 복원(&R)", L"&Restore Last Operation");
        item(file, CmdRestoreFrom, L"백업에서 복원(&B)…", L"Restore from &Backup…");
        item(file, CmdCleanBackups, L"복원이 끝난 백업 정리(&C)…", L"&Clean Up Finished Backups…");
        item(file, CmdOpenBackups, L"백업 폴더 열기(&O)", L"&Open Backup Folder");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        item(file, CmdExit, L"끝내기(&X)", L"E&xit");
        item(edit, CmdDelete, L"삭제(&D)\tDel", L"&Delete\tDel");
        item(edit, CmdDisable, L"사용 안 함(&I)\tF7", L"D&isable\tF7");
        item(edit, CmdEnable, L"사용(&E)\tF8", L"&Enable\tF8");
        AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
        item(edit, CmdCheck, L"선택 행 체크(&K)", L"Chec&k Selected Rows");
        item(edit, CmdUncheck, L"선택 행 체크 해제(&U)", L"&Uncheck Selected Rows");
        item(edit, CmdCheckProblems, L"문제 항목 모두 체크(&P)", L"Check All &Problem Entries");
        item(edit, CmdUncheckAll, L"모든 체크 해제(&N)", L"Clear All Check&marks");
        AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
        item(edit, CmdSelectAll, L"모두 선택(&A)\tCtrl+A", L"Select &All\tCtrl+A");
        item(edit, CmdCopy, L"항목 정보 복사(&C)\tCtrl+C", L"&Copy Entry Details\tCtrl+C");
        item(edit, CmdFind, L"찾기(&F)\tCtrl+F", L"&Find\tCtrl+F");
        item(view, CmdRefresh, L"새로고침(&R)\tF5", L"&Refresh\tF5");
        AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
        item(view, CmdHideProtected, L"보호 항목 숨기기(&H)", L"&Hide Protected Entries");
        item(view, CmdHideMicrosoft, L"Microsoft 항목 숨기기(&M)", L"Hide &Microsoft Entries");
        item(view, CmdShowPackaged, L"패키지 COM 후보 표시(&P)", L"Show &Packaged COM Candidates");
        item(view, CmdShowDetails, L"하단 정보 창(&D)", L"&Details Pane");
        AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
        item(view, CmdProperties, L"속성(&T)\tAlt+Enter", L"Proper&ties\tAlt+Enter");
        item(view, CmdRegistry, L"레지스트리 편집기에서 열기(&G)", L"Open in Re&gistry Editor");
        item(view, CmdLocation, L"파일 위치 열기(&L)", L"Open File &Location");
        item(options, CmdAutoRestart, L"변경 후 Windows 탐색기 자동 재시작(&R)",
             L"&Restart Windows Explorer After Changes");
        item(help, CmdAbout, L"ShellTidy 정보(&A)", L"&About ShellTidy");
        auto bar = CreateMenu();
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), tr(L"파일(&F)", L"&File"));
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), tr(L"편집(&E)", L"&Edit"));
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), tr(L"보기(&V)", L"&View"));
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(options), tr(L"옵션(&O)", L"&Options"));
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), tr(L"도움말(&H)", L"&Help"));
        SetMenu(hwnd, bar);
    }
    void createToolbar()
    {
        toolbar = CreateWindowExW(
            0, TOOLBARCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS |
                TBSTYLE_TRANSPARENT | CCS_NODIVIDER | CCS_NORESIZE | CCS_NOPARENTALIGN,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(Toolbar)), instance, nullptr);
        if (!toolbar)
            throw Error("Create toolbar");
        SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS | TBSTYLE_EX_DOUBLEBUFFER);
        struct
        {
            int command, image;
            const wchar_t *text;
        } items[] = {
            {CmdRefresh, 0, tr(L"새로고침", L"Refresh")}, {0, 0, nullptr},
            {CmdDelete, 1, tr(L"삭제", L"Delete")},       {CmdDisable, 4, tr(L"사용 안 함", L"Disable")},
            {CmdEnable, 5, tr(L"사용", L"Enable")},       {0, 0, nullptr},
            {CmdRestoreLast, 2, tr(L"복원", L"Restore")}, {CmdRestoreFrom, 3, tr(L"백업", L"Backups")}};
        std::vector<TBBUTTON> buttons;
        for (const auto &item : items)
        {
            TBBUTTON b{};
            if (!item.command)
                b.fsStyle = BTNS_SEP;
            else
            {
                b.iBitmap = item.image;
                b.idCommand = item.command;
                b.fsState = TBSTATE_ENABLED;
                b.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
                b.iString = reinterpret_cast<INT_PTR>(item.text);
            }
            buttons.push_back(b);
        }
        SendMessageW(toolbar, TB_ADDBUTTONSW, buttons.size(), reinterpret_cast<LPARAM>(buttons.data()));
    }
    void create()
    {
        dpi = forcedDpi ? forcedDpi : GetDpiForWindow(hwnd);
        buildMenu();
        createToolbar();
        search = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Search, WS_EX_CLIENTEDGE);
        SendMessageW(search, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(tr(L"검색 (Ctrl+F)", L"Search (Ctrl+F)")));
        typeFilter = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, TypeFilter);
        for (auto s : {tr(L"모든 유형", L"All types"), tr(L"연결 프로그램", L"Open With"),
                       tr(L"우클릭 메뉴", L"Context menus")})
            SendMessageW(typeFilter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessageW(typeFilter, CB_SETCURSEL, 0, 0);
        statusFilter = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, StatusFilter);
        for (auto s :
             {tr(L"모든 상태", L"All statuses"), tr(L"문제 항목", L"Problems"),
              tr(L"경로 없음", L"File missing"), tr(L"등록 없음", L"Not registered"),
              tr(L"중복 의심", L"Possible duplicates"), tr(L"확인 필요", L"Needs review"),
              tr(L"정상", L"Healthy"), tr(L"시스템 보호", L"Protected"), tr(L"사용 안 함", L"Disabled")})
            SendMessageW(statusFilter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessageW(statusFilter, CB_SETCURSEL, 0, 0);
        list = control(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, List);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                                    LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
                                                    LVS_EX_HEADERDRAGDROP);
        SetWindowTheme(list, L"Explorer", nullptr);
        ListView_SetCallbackMask(list, LVIS_STATEIMAGEMASK);
        for (int i = 0; i < Columns; ++i)
        {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<LPWSTR>(columnName(i));
            c.cx = px(ColumnWidths[i]);
            ListView_InsertColumn(list, i, &c);
        }
        details = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, Details);
        fonts();
        BOOL dark = FALSE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        layout();
        showDetailsText();
        startScan();
    }
    void layout()
    {
        if (!list)
            return;
        RECT r;
        GetClientRect(hwnd, &r);
        int pad = px(12), gap = px(8);
        SIZE bar{};
        SendMessageW(toolbar, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&bar));
        int buttonHeight = HIWORD(SendMessageW(toolbar, TB_GETBUTTONSIZE, 0, 0));
        MoveWindow(statusFilter, 0, 0, px(150), px(320), FALSE);
        RECT field;
        GetWindowRect(statusFilter, &field);
        int fieldHeight = field.bottom - field.top;
        int row = std::max(buttonHeight, fieldHeight) + px(14);
        MoveWindow(toolbar, px(6), (row - buttonHeight) / 2, bar.cx, buttonHeight, TRUE);
        int x = r.right - pad, y = (row - fieldHeight) / 2;
        MoveWindow(statusFilter, x - px(150), y, px(150), px(320), TRUE);
        x -= px(150) + gap;
        MoveWindow(typeFilter, x - px(130), y, px(130), px(320), TRUE);
        x -= px(130) + gap;
        int searchWidth = std::clamp(x - (px(6) + static_cast<int>(bar.cx) + gap * 2), px(100), px(300));
        MoveWindow(search, x - searchWidth, y, searchWidth, fieldHeight, TRUE);
        int statusHeight = px(28);
        statusFrame = {0, r.bottom - statusHeight, r.right, r.bottom};
        int bottom = statusFrame.top - px(8);
        if (showDetails)
        {
            detailsFrame = {pad, bottom - px(104), r.right - pad, bottom};
            bottom = detailsFrame.top - gap;
            MoveWindow(details, detailsFrame.left + px(8), detailsFrame.top + px(6),
                       detailsFrame.right - detailsFrame.left - px(10),
                       detailsFrame.bottom - detailsFrame.top - px(12), TRUE);
        }
        ShowWindow(details, showDetails ? SW_SHOWNA : SW_HIDE);
        listFrame = {pad, row, r.right - pad, std::max(row + px(80), bottom)};
        MoveWindow(list, listFrame.left + 1, listFrame.top + 1, listFrame.right - listFrame.left - 2,
                   listFrame.bottom - listFrame.top - 2, TRUE);
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    void setStatus(const std::wstring &text)
    {
        statusLine = text;
        InvalidateRect(hwnd, &statusFrame, TRUE);
    }
    std::vector<size_t> selectedEntries() const
    {
        std::vector<size_t> out;
        for (int row = -1; (row = ListView_GetNextItem(list, row, LVNI_SELECTED)) >= 0;)
            if (static_cast<size_t>(row) < visible.size())
                out.push_back(visible[row]);
        return out;
    }
    int currentRow() const
    {
        int row = ListView_GetNextItem(list, -1, LVNI_FOCUSED | LVNI_SELECTED);
        if (row < 0)
            row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        return row >= 0 && static_cast<size_t>(row) < visible.size() ? row : -1;
    }
    bool can(Action action) const
    {
        return !busy && !actionEntries(entries, visible, selectedEntries(), action).empty();
    }
    void refreshButtons()
    {
        for (auto [id, action] :
             {std::pair{CmdDelete, Action::Delete}, std::pair{CmdDisable, Action::Disable},
              std::pair{CmdEnable, Action::Enable}})
            SendMessageW(toolbar, TB_ENABLEBUTTON, id, MAKELONG(can(action), 0));
        for (int id : {CmdRestoreLast, CmdRestoreFrom})
            SendMessageW(toolbar, TB_ENABLEBUTTON, id, MAKELONG(!busy, 0));
    }
    void setBusy(bool value)
    {
        busy = value;
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_TEXT;
        info.pszText = const_cast<LPWSTR>(value ? tr(L"취소", L"Cancel") : tr(L"새로고침", L"Refresh"));
        SendMessageW(toolbar, TB_SETBUTTONINFOW, CmdRefresh, reinterpret_cast<LPARAM>(&info));
        SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
        layout();
        refreshButtons();
    }
    void statusText()
    {
        refreshButtons();
        if (busy)
            return;
        size_t missing = 0, orphan = 0, disabled = 0, checked = 0;
        for (const auto &e : entries)
        {
            missing += e.status == Status::Missing;
            orphan += e.status == Status::Orphan;
            disabled += e.disabled;
            checked += e.checked;
        }
        auto s = std::wstring(tr(L"표시 ", L"Shown ")) + number(visible.size()) + L" / " +
                 number(entries.size()) + tr(L"     경로 없음 ", L"     Missing ") + number(missing) +
                 tr(L"  ·  등록 없음 ", L"  ·  Not registered ") + number(orphan) +
                 tr(L"  ·  사용 안 함 ", L"  ·  Disabled ") + number(disabled) +
                 tr(L"     체크 ", L"     Checked ") + number(checked);
        if (warnings)
            s += tr(L"  ·  읽기 경고 ", L"  ·  Read warnings ") + number(warnings);
        setStatus(s);
    }
    bool shown(const Entry &e, const std::wstring &q, int type, int status) const
    {
        if ((type == 1 && e.kind != Kind::OpenWith) || (type == 2 && e.kind == Kind::OpenWith))
            return false;
        switch (status)
        {
        case 1:
            if (!problem(e))
                return false;
            break;
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        {
            const Status order[] = {Status::Missing, Status::Orphan,  Status::Duplicate,
                                    Status::Review,  Status::Healthy, Status::Protected};
            if (e.status != order[status - 2])
                return false;
            break;
        }
        case 8:
            if (!e.disabled)
                return false;
            break;
        }
        if (hideProtected && (e.status == Status::Protected || (e.unsupported && !e.switchable)))
            return false;
        if (hideMicrosoft && lower(e.company).starts_with(L"microsoft"))
            return false;
        if (!showPackaged && e.packaged)
            return false;
        if (q.empty())
            return true;
        std::wstring hay = e.name + L" " + e.executable + L" " + e.command + L" " + e.source + L" " +
                           e.description + L" " + e.company;
        for (const auto &x : e.extensions)
            hay += L" " + x;
        for (const auto &x : e.progids)
            hay += L" " + x;
        for (const auto &t : e.targets)
            hay += L" " + t.path;
        return lower(hay).find(q) != std::wstring::npos;
    }
    void populate()
    {
        if (!list)
            return;
        auto chosen = selectedEntries();
        std::set<size_t> keep(chosen.begin(), chosen.end());
        size_t focused = SIZE_MAX;
        if (int row = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
            row >= 0 && static_cast<size_t>(row) < visible.size())
            focused = visible[row];
        auto q = lower(getText(search));
        int type = static_cast<int>(SendMessageW(typeFilter, CB_GETCURSEL, 0, 0));
        int status = static_cast<int>(SendMessageW(statusFilter, CB_GETCURSEL, 0, 0));
        visible.clear();
        for (size_t i = 0; i < entries.size(); ++i)
            if (shown(entries[i], q, type, status))
                visible.push_back(i);
        std::stable_sort(visible.begin(), visible.end(),
                         [&](size_t a, size_t b)
                         {
                             const auto &x = entries[a], &y = entries[b];
                             int cmp = sortColumn == 2
                                           ? rank(x.status) - rank(y.status)
                                           : compareLabels(cell(x, sortColumn), cell(y, sortColumn));
                             if (!cmp)
                                 cmp = compareLabels(x.name, y.name);
                             if (!cmp)
                                 cmp = compareLabels(x.executable, y.executable);
                             return sortAscending ? cmp < 0 : cmp > 0;
                         });
        filling = true;
        ListView_SetItemCountEx(list, static_cast<int>(visible.size()), LVSICF_NOSCROLL);
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        for (int row = 0; row < static_cast<int>(visible.size()); ++row)
        {
            if (keep.contains(visible[row]))
                ListView_SetItemState(list, row, LVIS_SELECTED, LVIS_SELECTED);
            if (visible[row] == focused)
                ListView_SetItemState(list, row, LVIS_FOCUSED, LVIS_FOCUSED);
        }
        filling = false;
        auto header = ListView_GetHeader(list);
        for (int column = 0; column < Columns; ++column)
        {
            HDITEMW item{};
            item.mask = HDI_FORMAT;
            Header_GetItem(header, column, &item);
            item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (column == sortColumn)
                item.fmt |= sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
            Header_SetItem(header, column, &item);
        }
        InvalidateRect(list, nullptr, FALSE);
        showDetailsText();
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
        setStatus(tr(L"레지스트리 검색 중…  목록은 계속 볼 수 있으며 F5로 취소합니다.",
                     L"Scanning the registry…  The list stays usable; press F5 to cancel."));
        worker = std::thread(
            [this]
            {
                HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                auto r = std::make_unique<ScanResult>();
                try
                {
                    *r = scan({}, cancel);
                }
                catch (...)
                {
                    ++r->warnings;
                }
                if (SUCCEEDED(com))
                    CoUninitialize();
                PostMessageW(hwnd, ScanDone, 0, reinterpret_cast<LPARAM>(r.release()));
            });
    }
    struct PopupData
    {
        std::wstring copy;
    };
    static void copyText(HWND owner, const std::wstring &value)
    {
        auto size = (value.size() + 1) * sizeof(wchar_t);
        auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!memory)
            return;
        auto text = GlobalLock(memory);
        if (text)
        {
            memcpy(text, value.c_str(), size);
            GlobalUnlock(memory);
            if (OpenClipboard(owner))
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
            copyText(dialog, context->copy);
            return S_FALSE;
        }
        return S_OK;
    }
    // Popups never scroll: the body is bounded and the full text is available through the copy button.
    void message(const std::wstring &text, bool error = false, const std::wstring &full = L"",
                 const std::wstring &title = L"", size_t maxChars = 340, size_t maxLines = 6)
    {
        auto content = popupSummary(text, maxChars, maxLines);
        PopupData context{full.empty() ? text : full};
        TASKDIALOG_BUTTON buttons[] = {{IDOK, tr(L"확인", L"OK")}, {6001, tr(L"상세 복사", L"Copy details")}};
        TASKDIALOGCONFIG c{sizeof(c)};
        c.hwndParent = hwnd;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.cxWidth = 300;
        c.pszWindowTitle = L"ShellTidy";
        auto instruction =
            !title.empty()
                ? title
                : std::wstring(error ? tr(L"작업 결과를 확인하세요", L"Review the operation result")
                                     : L"ShellTidy");
        c.pszMainInstruction = instruction.c_str();
        c.pszContent = content.c_str();
        c.pszMainIcon = error ? TD_WARNING_ICON : TD_INFORMATION_ICON;
        c.pButtons = buttons;
        c.cButtons = 2;
        c.nDefaultButton = IDOK;
        c.pfCallback = popupCallback;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(&context);
        TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
    }
    bool confirm(const std::wstring &title, const std::wstring &content, const std::wstring &copy)
    {
        TASKDIALOG_BUTTON buttons[] = {{IDYES, tr(L"진행", L"Continue")},
                                       {IDCANCEL, tr(L"취소", L"Cancel")},
                                       {6001, tr(L"대상 목록 복사", L"Copy targets")}};
        PopupData context{copy};
        TASKDIALOGCONFIG c{sizeof(c)};
        c.hwndParent = hwnd;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.cxWidth = 300;
        c.pszWindowTitle = L"ShellTidy";
        c.pszMainInstruction = title.c_str();
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
    std::vector<Target> actionTargets(const Entry &e, Action action) const
    {
        if (action == Action::Delete)
            return e.targets;
        if (action == Action::Disable)
            return e.switches.empty() ? std::vector<Target>{} : std::vector<Target>{e.switches.front()};
        return e.switches;
    }
    void work(Action action, bool restoring = false, const std::wstring &file = {})
    {
        if (busy)
            return;
        std::vector<Entry> selected;
        size_t hiddenChecked = 0;
        if (!restoring)
        {
            for (auto i : actionEntries(entries, visible, selectedEntries(), action))
                selected.push_back(entries[i]);
            std::set<size_t> current(visible.begin(), visible.end());
            for (size_t i = 0; i < entries.size(); ++i)
                hiddenChecked += entries[i].checked && !current.contains(i);
            if (selected.empty())
            {
                message(
                    tr(L"이 작업을 적용할 수 있는 항목을 체크하거나 선택하세요. 보호 항목과 조회 전용 항목은 "
                       L"제외됩니다.",
                       L"Check or select entries this action applies to. Protected and inspection-only "
                       L"entries are excluded."));
                return;
            }
        }
        std::wstring locations;
        unsigned count = 0, userCount = 0, machineCount = 0;
        std::set<std::wstring> counted;
        auto countTarget = [&](const Target &original)
        {
            auto t = canonicalTarget(original);
            if (counted.insert(lower(describe(t))).second)
            {
                ++count;
                ++(t.hive == Hive::User ? userCount : machineCount);
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
                    for (const auto &t : actionTargets(e, action))
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
        auto summary = restoring ? std::filesystem::path(file).filename().wstring() + L"\n"
                                 : number(selected.size()) + tr(L"개 항목\n", L" entries\n");
        summary += number(count) + tr(L"개 레지스트리 위치", L" registry locations");
        summary += L"\nHKCU " + number(userCount) + L" · HKLM " + number(machineCount);
        std::wstring title, warning;
        if (restoring)
        {
            title = tr(L"백업한 등록을 복원할까요?", L"Restore the backed-up registrations?");
            warning = tr(L"현재 값과 충돌하는 항목은 덮어쓰지 않고 건너뜁니다.",
                         L"Entries that conflict with current values are skipped, never overwritten.");
        }
        else if (action == Action::Delete)
        {
            title = tr(L"선택한 등록을 실제로 삭제할까요?", L"Permanently remove these registrations?");
            warning = tr(L"선택한 레지스트리 등록만 삭제하며 앱 파일과 문서는 보존합니다. 삭제 전에 자동으로 "
                         L"백업합니다.",
                         L"Only the selected registry registrations are removed. App files and documents are "
                         L"preserved, and a backup is saved first.");
        }
        else
        {
            title = action == Action::Disable
                        ? tr(L"선택한 메뉴를 사용 안 함으로 바꿀까요?", L"Disable the selected menus?")
                        : tr(L"선택한 메뉴를 다시 사용할까요?", L"Enable the selected menus again?");
            warning =
                tr(L"등록은 지우지 않고 사용 안 함 표시(LegacyDisable 값 또는 Shell Extensions\\Blocked의 "
                   L"CLSID)만 바꿉니다. 셸 확장은 같은 CLSID를 쓰는 모든 메뉴에 함께 적용됩니다.",
                   L"Registrations are kept; only the disable marker (a LegacyDisable value or the CLSID "
                   L"in Shell Extensions\\Blocked) changes. A shell extension switch applies to every "
                   L"menu that uses the same CLSID.");
        }
        if (hiddenChecked)
            warning += tr(L"\n\n목록에 보이지 않는 체크 항목 ", L"\n\n") + number(hiddenChecked) +
                       tr(L"개는 이번 작업에서 제외합니다.",
                          L" checked entries hidden by the current view are not included.");
        if (autoRestart)
            warning += tr(L"\n\nExplorer가 자동 재시작됩니다. 파일 복사·이동 작업 및 열린 탐색기 창이 중단될 "
                          L"수 있으니 먼저 완료해 주세요.",
                          L"\n\nExplorer will restart automatically. File copies, moves and open Explorer "
                          L"windows can be interrupted. Finish those operations first.");
        if (!confirm(title, popupSummary(summary, 140, 3) + L"\n\n" + warning, locations))
            return;
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
        setStatus(restoring ? tr(L"충돌 검사 및 복원 중…", L"Checking conflicts and restoring…")
                            : tr(L"백업 후 변경 중…", L"Backing up and applying changes…"));
        worker = std::thread(
            [this, restoring, file, action, selected = std::move(selected), autoRestart = autoRestart]
            {
                HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                auto result = std::make_unique<WorkResult>();
                try
                {
                    auto j = restoring ? loadJournal(file) : prepare(selected, {}, action);
                    if (cancel)
                        throw Error("Canceled", ERROR_CANCELLED);
                    if (j.items.empty())
                        result->outcome.detail = tr(L"변경할 내용이 없습니다.", L"Nothing needed to change.");
                    else if (needsElevation(j, restoring))
                        result->outcome = executeElevated(j, restoring, hwnd);
                    else
                        result->outcome = restoring ? restore(j, cancel) : st::apply(j, cancel);
                    auto &o = result->outcome;
                    if (o.registryChanged)
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
                if (SUCCEEDED(com))
                    CoUninitialize();
                PostMessageW(hwnd, WorkDone, 0, reinterpret_cast<LPARAM>(result.release()));
            });
    }
    void chooseBackup(bool last)
    {
        if (busy)
            return;
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
                        work(Action::Delete, true, f);
                        return;
                    }
                }
                message(tr(L"복원할 마지막 작업이 없습니다.", L"No pending operation to restore."));
                return;
            }
            HMENU menu = CreatePopupMenu();
            size_t n = std::min<size_t>(files.size(), 30);
            for (size_t i = 0; i < n; ++i)
                AppendMenuW(menu, MF_STRING, CmdBackupFirst + i,
                            std::filesystem::path(files[i]).filename().c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, CmdOpenBackups, tr(L"백업 폴더 열기", L"Open backup folder"));
            int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, menuAnchor.x, menuAnchor.y, 0,
                                        hwnd, nullptr);
            DestroyMenu(menu);
            if (choice >= CmdBackupFirst && choice < CmdBackupFirst + static_cast<int>(n))
                work(Action::Delete, true, files[choice - CmdBackupFirst]);
            else if (choice == CmdOpenBackups)
                openBackupFolder();
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
    void openBackupFolder()
    {
        try
        {
            auto folder = backupDirectory();
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            ShellExecuteW(hwnd, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        catch (const Error &e)
        {
            message(errorText(e.code), true);
        }
    }
    void cleanBackups()
    {
        if (busy)
            return;
        std::vector<std::wstring> finished;
        std::wstring names;
        try
        {
            for (const auto &f : backupFiles())
                try
                {
                    auto j = loadJournal(f);
                    if (std::none_of(j.items.begin(), j.items.end(),
                                     [](const Mutation &m) { return m.phase == 1 || m.phase == 2; }))
                    {
                        finished.push_back(f);
                        names += std::filesystem::path(f).filename().wstring() + L"\n";
                    }
                }
                catch (...)
                {
                    // Unreadable backups are kept for manual inspection.
                }
        }
        catch (...)
        {
            message(tr(L"백업 목록을 읽을 수 없습니다.", L"Could not read the backup list."), true);
            return;
        }
        if (finished.empty())
        {
            message(
                tr(L"정리할 백업이 없습니다. 복원할 변경이 남은 백업과 읽을 수 없는 백업은 유지합니다.",
                   L"Nothing to clean up. Backups with restorable changes and unreadable backups are kept."));
            return;
        }
        if (!confirm(
                tr(L"복원이 끝난 백업을 삭제할까요?", L"Delete finished backups?"),
                number(finished.size()) +
                    tr(L"개 백업에는 복원할 변경이 남아 있지 않습니다. 복원할 변경이 남은 백업은 유지합니다.",
                       L" backups have nothing left to restore. Backups with restorable changes are kept."),
                names))
            return;
        unsigned failed = 0;
        for (const auto &f : finished)
            failed += !DeleteFileW(f.c_str());
        message(tr(L"삭제한 백업: ", L"Deleted backups: ") + number(finished.size() - failed) +
                    tr(L"\n실패: ", L"\nFailed: ") + number(failed),
                failed != 0);
    }
    std::wstring entryDetails(const Entry &e) const
    {
        std::wstring s = e.name + L"  |  " + kindText(e) + L"  |  " + state(e.status);
        if (!switchText(e).empty())
            s += L"  |  " + switchText(e);
        s += L"\r\n";
        s += tr(L"명령: ", L"Command: ") + e.command + L"\r\n";
        if (!e.executable.empty())
            s += tr(L"실행 파일: ", L"File: ") + e.executable + L"\r\n";
        if (!e.description.empty() || !e.company.empty())
            s += tr(L"설명: ", L"Description: ") + e.description + L"  |  " + tr(L"회사: ", L"Company: ") +
                 e.company + L"\r\n";
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
        for (const auto &t : e.switches)
            s += tr(L"사용 안 함 표시: ", L"Disable marker: ") + describe(t) + L"\r\n";
        return s;
    }
    void showDetailsText()
    {
        if (!showDetails || !details)
            return;
        int row = currentRow();
        SetWindowTextW(details, row >= 0
                                    ? entryDetails(entries[visible[row]]).c_str()
                                    : tr(L"항목을 선택하면 명령, 확장자, ProgID와 실제 레지스트리 위치를 "
                                         L"볼 수 있습니다. 더블클릭하면 속성을 엽니다.",
                                         L"Select an entry to see its command, extensions, ProgIDs and exact "
                                         L"registry locations. Double-click for properties."));
    }
    void properties()
    {
        int row = currentRow();
        if (row < 0)
            return;
        const auto &e = entries[visible[row]];
        auto text = std::wstring(tr(L"유형: ", L"Type: ")) + kindText(e) + L"  ·  " + state(e.status);
        if (!switchText(e).empty())
            text += L"  ·  " + switchText(e);
        text += L"\n" + std::wstring(tr(L"경로: ", L"Path: ")) + pathText(e).substr(0, 120);
        if (!e.description.empty())
            text += L"\n" + std::wstring(tr(L"설명: ", L"Description: ")) + e.description.substr(0, 80);
        if (!e.company.empty())
            text += L"\n" + std::wstring(tr(L"회사: ", L"Company: ")) + e.company.substr(0, 80);
        if (!e.extensions.empty())
            text += L"\n" + std::wstring(tr(L"확장자: ", L"Extensions: ")) + extensionText(e);
        text += L"\n" + std::wstring(tr(L"등록 위치: ", L"Source: ")) + e.source + L"  ·  " +
                number(e.targets.size()) + tr(L"개 레지스트리 대상", L" registry targets");
        if (!e.reason.empty())
            text += L"\n" + e.reason.substr(0, 160);
        message(text, false, entryDetails(e), e.name.substr(0, 80), 900, 12);
    }
    void copyRows()
    {
        std::wstring text;
        for (auto i : selectedEntries())
            text += entryDetails(entries[i]) + L"\r\n";
        if (!text.empty())
            copyText(hwnd, text);
    }
    static std::wstring csvField(const std::wstring &s)
    {
        std::wstring out = L"\"";
        for (auto c : s)
            out += c == L'"' ? L"\"\"" : std::wstring(1, c);
        return out + L"\"";
    }
    void saveReport()
    {
        auto rows = selectedEntries();
        if (rows.empty())
            rows = visible;
        if (rows.empty())
            return;
        wchar_t file[4096] = L"ShellTidy-report";
        std::wstring filter = std::wstring(tr(L"텍스트 파일 (*.txt)", L"Text file (*.txt)")) + L'\0' +
                              L"*.txt" + L'\0' + tr(L"CSV 파일 (*.csv)", L"CSV file (*.csv)") + L'\0' +
                              L"*.csv" + L'\0' + L'\0';
        OPENFILENAMEW o{sizeof(o)};
        o.hwndOwner = hwnd;
        o.lpstrFilter = filter.c_str();
        o.lpstrFile = file;
        o.nMaxFile = static_cast<DWORD>(std::size(file));
        o.lpstrDefExt = L"txt";
        o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetSaveFileNameW(&o))
            return;
        bool csv = lower(std::filesystem::path(file).extension().wstring()) == L".csv";
        std::wstring text;
        if (csv)
        {
            for (int column = 0; column < Columns; ++column)
                text += (column ? L"," : L"") + csvField(columnName(column));
            text += L"\r\n";
            for (auto i : rows)
            {
                for (int column = 0; column < Columns; ++column)
                    text += (column ? L"," : L"") + csvField(cell(entries[i], column));
                text += L"\r\n";
            }
        }
        else
            for (auto i : rows)
            {
                text += L"==================================================\r\n";
                for (int column = 0; column < Columns; ++column)
                    text += std::wstring(columnName(column)) + L": " + cell(entries[i], column) + L"\r\n";
                text += entryDetails(entries[i]) + L"\r\n";
            }
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                    nullptr, nullptr);
        std::string bytes = "\xEF\xBB\xBF";
        auto at = bytes.size();
        bytes.resize(at + static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), bytes.data() + at, n,
                            nullptr, nullptr);
        Handle f(CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD done = 0;
        if (f.h == INVALID_HANDLE_VALUE ||
            !WriteFile(f.h, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) ||
            done != bytes.size())
            message(errorText(GetLastError()), true);
    }
    void openRegistry()
    {
        int row = currentRow();
        if (row < 0 || entries[visible[row]].targets.empty())
            return;
        // Regedit reopens LastKey; keep the localized "Computer" root name it already uses.
        Target last{Hive::User, KEY_WOW64_64KEY,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit", L"LastKey", false};
        std::wstring prefix = L"Computer";
        try
        {
            if (auto v = readValue(last))
            {
                auto s = textValue(*v);
                auto cut = s.find(L'\\');
                auto root = s.substr(0, cut);
                if (!root.empty() && !lower(root).starts_with(L"hkey_"))
                    prefix = root;
            }
            writeValue(last,
                       stringValue(prefix + L"\\" + registryPath(entries[visible[row]].targets.front())));
        }
        catch (const Error &e)
        {
            message(errorText(e.code), true);
            return;
        }
        ShellExecuteW(hwnd, L"open", L"regedit.exe", L"-m", nullptr, SW_SHOWNORMAL);
    }
    void openLocation()
    {
        int row = currentRow();
        if (row < 0)
            return;
        auto path = entries[visible[row]].executable;
        if (path.empty())
        {
            message(
                tr(L"실행 파일 경로를 해석할 수 없습니다.", L"The executable path could not be resolved."));
            return;
        }
        if (fileState(path) == FileState::Present)
            if (auto pidl = ILCreateFromPathW(path.c_str()))
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
                return;
            }
        // A missing file opens the closest folder that still exists.
        for (auto folder = std::filesystem::path(path).parent_path(); !folder.empty();)
        {
            if (fileState(folder.wstring()) == FileState::Folder)
            {
                ShellExecuteW(hwnd, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            auto parent = folder.parent_path();
            if (parent == folder)
                break;
            folder = parent;
        }
        message(tr(L"파일 위치에 접근할 수 없습니다.", L"The file location is not reachable."), true);
    }
    void redraw()
    {
        InvalidateRect(list, nullptr, FALSE);
        statusText();
    }
    void toggle(int row)
    {
        if (row < 0 || static_cast<size_t>(row) >= visible.size())
            return;
        auto &e = entries[visible[row]];
        if (!checkable(e))
            return;
        e.checked = !e.checked;
        ListView_RedrawItems(list, row, row);
        statusText();
    }
    void toggleSelected()
    {
        int row = currentRow();
        if (row < 0)
            return;
        bool value = !entries[visible[row]].checked;
        for (auto i : selectedEntries())
            if (checkable(entries[i]))
                entries[i].checked = value;
        redraw();
    }
    void checkRows(bool value)
    {
        for (auto i : selectedEntries())
            if (checkable(entries[i]))
                entries[i].checked = value;
        redraw();
    }
    void updateMenu(HMENU m)
    {
        auto rows = selectedEntries();
        auto enable = [&](int id, bool on)
        { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
        auto check = [&](int id, bool on)
        { CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
        enable(CmdDelete, can(Action::Delete));
        enable(CmdDisable, can(Action::Disable));
        enable(CmdEnable, can(Action::Enable));
        for (int id : {CmdRestoreLast, CmdRestoreFrom, CmdCleanBackups})
            enable(id, !busy);
        for (int id : {CmdCheck, CmdUncheck, CmdCopy, CmdProperties, CmdRegistry})
            enable(id, !rows.empty());
        int row = currentRow();
        enable(CmdLocation, row >= 0 && !entries[visible[row]].executable.empty());
        enable(CmdSave, !visible.empty());
        enable(CmdCheckProblems, !visible.empty());
        check(CmdHideProtected, hideProtected);
        check(CmdHideMicrosoft, hideMicrosoft);
        check(CmdShowPackaged, showPackaged);
        check(CmdShowDetails, showDetails);
        check(CmdAutoRestart, autoRestart);
    }
    void rowMenu(LPARAM position)
    {
        int row = currentRow();
        if (row < 0)
            return;
        POINT point{static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
        if (point.x == -1 && point.y == -1)
        {
            RECT item{};
            ListView_GetItemRect(list, row, &item, LVIR_BOUNDS);
            point = {item.left + px(24), item.bottom};
            ClientToScreen(list, &point);
        }
        HMENU menu = CreatePopupMenu();
        if (!menu)
            throw Error("Create list menu");
        auto item = [&](int id, const wchar_t *ko, const wchar_t *en)
        { AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(id), tr(ko, en)); };
        item(CmdDelete, L"삭제\tDel", L"Delete\tDel");
        item(CmdDisable, L"사용 안 함\tF7", L"Disable\tF7");
        item(CmdEnable, L"사용\tF8", L"Enable\tF8");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(CmdCheck, L"선택 행 체크", L"Check selected rows");
        item(CmdUncheck, L"선택 행 체크 해제", L"Uncheck selected rows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(CmdRegistry, L"레지스트리 편집기에서 열기", L"Open in Registry Editor");
        item(CmdLocation, L"파일 위치 열기", L"Open file location");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        item(CmdCopy, L"항목 정보 복사\tCtrl+C", L"Copy entry details\tCtrl+C");
        item(CmdProperties, L"속성\tAlt+Enter", L"Properties\tAlt+Enter");
        updateMenu(menu);
        auto choice =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (choice)
            command(choice);
    }
    void about()
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
        message(L"ShellTidy 0.0.2\nhttps://github.com/deuxdoom/ShellTidy\n\n" + licenses);
    }
    void command(int id)
    {
        switch (id)
        {
        case CmdRefresh:
            if (busy)
            {
                cancel = true;
                setStatus(tr(L"현재 안전한 단계까지 마친 후 취소합니다…",
                             L"Canceling after the current safe step…"));
            }
            else
                startScan();
            break;
        case CmdDelete:
            work(Action::Delete);
            break;
        case CmdDisable:
            work(Action::Disable);
            break;
        case CmdEnable:
            work(Action::Enable);
            break;
        case CmdRestoreLast:
            chooseBackup(true);
            break;
        case CmdRestoreFrom:
            chooseBackup(false);
            break;
        case CmdCleanBackups:
            cleanBackups();
            break;
        case CmdOpenBackups:
            openBackupFolder();
            break;
        case CmdSave:
            saveReport();
            break;
        case CmdExit:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case CmdCheck:
        case CmdUncheck:
            checkRows(id == CmdCheck);
            break;
        case CmdCheckProblems:
            for (auto i : visible)
                if (problem(entries[i]) && eligible(entries[i], Action::Delete))
                    entries[i].checked = true;
            redraw();
            break;
        case CmdUncheckAll:
            for (auto &e : entries)
                e.checked = false;
            redraw();
            break;
        case CmdSelectAll:
            SetFocus(list);
            ListView_SetItemState(list, -1, LVIS_SELECTED, LVIS_SELECTED);
            break;
        case CmdCopy:
            copyRows();
            break;
        case CmdFind:
            SetFocus(search);
            SendMessageW(search, EM_SETSEL, 0, -1);
            break;
        case CmdProperties:
            properties();
            break;
        case CmdRegistry:
            openRegistry();
            break;
        case CmdLocation:
            openLocation();
            break;
        case CmdHideProtected:
            hideProtected = !hideProtected;
            populate();
            break;
        case CmdHideMicrosoft:
            hideMicrosoft = !hideMicrosoft;
            populate();
            break;
        case CmdShowPackaged:
            showPackaged = !showPackaged;
            populate();
            break;
        case CmdShowDetails:
            showDetails = !showDetails;
            layout();
            showDetailsText();
            break;
        case CmdAutoRestart:
            autoRestart = !autoRestart;
            break;
        case CmdAbout:
            about();
            break;
        }
    }
    bool shortcut(const MSG &msg)
    {
        if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN)
            return false;
        bool ctrl = GetKeyState(VK_CONTROL) < 0, inList = GetFocus() == list;
        if (msg.message == WM_SYSKEYDOWN)
        {
            if (msg.wParam == VK_RETURN && inList)
            {
                command(CmdProperties);
                return true;
            }
            return false;
        }
        int id = 0;
        if (msg.wParam == VK_F5)
            id = CmdRefresh;
        else if (msg.wParam == VK_F7)
            id = CmdDisable;
        else if (msg.wParam == VK_F8)
            id = CmdEnable;
        else if (msg.wParam == VK_DELETE && inList)
            id = CmdDelete;
        else if (ctrl && msg.wParam == 'F')
            id = CmdFind;
        else if (ctrl && msg.wParam == 'S')
            id = CmdSave;
        else if (ctrl && inList && msg.wParam == 'A')
            id = CmdSelectAll;
        else if (ctrl && inList && msg.wParam == 'C')
            id = CmdCopy;
        if (id)
            command(id);
        return id != 0;
    }
    LRESULT notify(NMHDR *n)
    {
        if (n->hwndFrom == toolbar && n->code == TBN_GETINFOTIPW)
        {
            auto tip = reinterpret_cast<NMTBGETINFOTIPW *>(n);
            const wchar_t *text = nullptr;
            switch (tip->iItem)
            {
            case CmdRefresh:
                text = tr(L"새로고침 / 검색 취소 (F5)", L"Refresh / cancel scan (F5)");
                break;
            case CmdDelete:
                text = tr(L"체크하거나 선택한 등록 삭제 (Del)",
                          L"Delete checked or selected registrations (Del)");
                break;
            case CmdDisable:
                text = tr(L"등록은 두고 메뉴만 사용 안 함 (F7)",
                          L"Disable the menu, keep the registration (F7)");
                break;
            case CmdEnable:
                text = tr(L"사용 안 함을 해제 (F8)", L"Enable the menu again (F8)");
                break;
            case CmdRestoreLast:
                text = tr(L"마지막 작업 복원", L"Restore the last operation");
                break;
            case CmdRestoreFrom:
                text = tr(L"백업 목록에서 복원", L"Restore from the backup list");
                break;
            }
            if (text)
                wcsncpy_s(tip->pszText, tip->cchTextMax, text, _TRUNCATE);
            return 0;
        }
        if (n->idFrom != List)
            return 0;
        switch (n->code)
        {
        case LVN_GETDISPINFOW:
        {
            auto &item = reinterpret_cast<NMLVDISPINFOW *>(n)->item;
            if (item.iItem < 0 || static_cast<size_t>(item.iItem) >= visible.size())
                return 0;
            const auto &e = entries[visible[item.iItem]];
            if (item.mask & LVIF_TEXT)
                wcsncpy_s(item.pszText, item.cchTextMax, cell(e, item.iSubItem).c_str(), _TRUNCATE);
            if (item.mask & LVIF_IMAGE)
                item.iImage = 0;
            if (item.mask & LVIF_STATE)
            {
                // No checkbox for rows that can be neither deleted nor switched.
                item.state = INDEXTOSTATEIMAGEMASK(checkable(e) ? (e.checked ? 2 : 1) : 0);
                item.stateMask = LVIS_STATEIMAGEMASK;
            }
            return 0;
        }
        case LVN_ODFINDITEMW:
        {
            // Type-ahead search over the virtual list by name.
            auto find = reinterpret_cast<NMLVFINDITEMW *>(n);
            if (!(find->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !find->lvfi.psz || visible.empty())
                return -1;
            auto prefix = lower(find->lvfi.psz);
            for (size_t k = 0; k < visible.size(); ++k)
            {
                size_t row = (static_cast<size_t>(std::max(find->iStart, 0)) + k) % visible.size();
                if (lower(entries[visible[row]].name).starts_with(prefix))
                    return static_cast<LRESULT>(row);
            }
            return -1;
        }
        case LVN_COLUMNCLICK:
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
        case NM_CLICK:
        case NM_DBLCLK:
        {
            auto activate = reinterpret_cast<NMITEMACTIVATE *>(n);
            LVHITTESTINFO hit{};
            hit.pt = activate->ptAction;
            ListView_HitTest(list, &hit);
            if (hit.iItem >= 0 && (hit.flags & LVHT_ONITEMSTATEICON))
                toggle(hit.iItem);
            else if (n->code == NM_DBLCLK && activate->iItem >= 0)
                properties();
            return 0;
        }
        case LVN_KEYDOWN:
            if (reinterpret_cast<NMLVKEYDOWN *>(n)->wVKey == VK_SPACE)
                toggleSelected();
            return 0;
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED:
            if (!filling)
            {
                showDetailsText();
                statusText();
            }
            return 0;
        case NM_CUSTOMDRAW:
        {
            auto d = reinterpret_cast<NMLVCUSTOMDRAW *>(n);
            if (d->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (d->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && d->nmcd.dwItemSpec < visible.size())
            {
                const auto &e = entries[visible[d->nmcd.dwItemSpec]];
                if (problem(e))
                    d->clrTextBk = RGB(253, 231, 233);
                if (e.disabled)
                    d->clrText = RGB(138, 138, 138);
                else if (e.status == Status::Protected || (e.unsupported && !e.switchable))
                    d->clrText = RGB(112, 112, 112);
            }
            return CDRF_DODEFAULT;
        }
        }
        return 0;
    }
    void paint()
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FrameRect(dc, &listFrame, border);
        if (showDetails)
        {
            FillRect(dc, &detailsFrame, surface);
            FrameRect(dc, &detailsFrame, border);
        }
        RECT line{statusFrame.left, statusFrame.top, statusFrame.right, statusFrame.top + 1};
        FillRect(dc, &line, border);
        RECT text = statusFrame;
        text.left += px(16);
        text.right -= px(16);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(96, 96, 96));
        auto old = SelectObject(dc, font);
        DrawTextW(dc, statusLine.c_str(), -1, &text,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_CREATE:
            create();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED:
            if (!forcedDpi)
            {
                UINT previous = dpi;
                dpi = HIWORD(w);
                fonts();
                for (int i = 0; i < Columns; ++i)
                    ListView_SetColumnWidth(list, i, MulDiv(ListView_GetColumnWidth(list, i), dpi, previous));
                auto r = reinterpret_cast<RECT *>(l);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                layout();
            }
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto p = reinterpret_cast<MINMAXINFO *>(l);
            p->ptMinTrackSize = {px(1000), px(560)};
            return 0;
        }
        case WM_PAINT:
            paint();
            return 0;
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(l) == details)
            {
                SetBkColor(reinterpret_cast<HDC>(w), RGB(255, 255, 255));
                SetTextColor(reinterpret_cast<HDC>(w), RGB(50, 50, 50));
                return reinterpret_cast<LRESULT>(surface);
            }
            break;
        case WM_NOTIFY:
            return notify(reinterpret_cast<NMHDR *>(l));
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(w) == list)
            {
                rowMenu(l);
                return 0;
            }
            break;
        case WM_INITMENUPOPUP:
            updateMenu(reinterpret_cast<HMENU>(w));
            return 0;
        case WM_TIMER:
            if (w == SearchTimer)
            {
                KillTimer(hwnd, SearchTimer);
                populate();
                return 0;
            }
            break;
        case WM_COMMAND:
        {
            int id = LOWORD(w);
            if (id == Search && HIWORD(w) == EN_CHANGE)
                SetTimer(hwnd, SearchTimer, 180, nullptr); // Filter once typing pauses.
            else if ((id == TypeFilter || id == StatusFilter) && HIWORD(w) == CBN_SELCHANGE)
                populate();
            else if (id == IDOK && GetFocus() == list)
                properties();
            else if (id >= CmdRefresh && id <= CmdAbout)
            {
                RECT button{};
                if (reinterpret_cast<HWND>(l) == toolbar &&
                    SendMessageW(toolbar, TB_GETRECT, id, reinterpret_cast<LPARAM>(&button)))
                {
                    menuAnchor = {button.left, button.bottom};
                    ClientToScreen(toolbar, &menuAnchor);
                }
                else
                    GetCursorPos(&menuAnchor);
                command(id);
            }
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
                text = tr(L"변경 성공: ", L"Changes completed: ") + number(o.success) +
                       tr(L"\n실패: ", L"\nFailed: ") + number(o.failed) +
                       tr(L"\n복원 충돌: ", L"\nRestore conflicts: ") + number(o.conflicts);
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
                setStatus(tr(L"안전하게 작업을 마친 후 종료합니다…",
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
        if (toolImages)
            ImageList_Destroy(toolImages);
        if (font)
            DeleteObject(font);
        for (auto brush : {background, surface, border})
            DeleteObject(brush);
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
        c.hbrBackground = background;
        c.lpszClassName = L"ShellTidy.Main";
        RegisterClassExW(&c);
        dpi = forcedDpi ? forcedDpi : GetDpiForSystem();
        RECT r{0, 0, px(1180), px(760)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
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
            AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
            auto position = centeredRect(monitor.rcWork, size.right - size.left, size.bottom - size.top);
            SetWindowPos(h, nullptr, position.left, position.top, position.right - position.left,
                         position.bottom - position.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(h, show);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (!shortcut(msg) && !IsDialogMessageW(h, &msg))
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
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&cc);
    Window window(instance, english, testDpi);
    return window.run(show);
}
} // namespace st

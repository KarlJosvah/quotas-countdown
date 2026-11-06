// Countdown Widget - Win32 + SQLite, C++17, Windows 10+
//
//  * Floating window (drag anywhere on empty space, minimize, maximize, resize)
//  * Closing the window only hides it into the system tray
//  * Right-click on the tray icon -> menu with "Quit" (the only way to exit)
//  * Create / Read (live) / Update (label) / Delete countdowns, stored in SQLite
//  * Notifications at 30 min, 15 min and at expiry (tray balloon = Win10 toast)

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

// SQLite: by default use winsqlite3 (ships with Windows 10, header is in the SDK).
// Define USE_BUNDLED_SQLITE (see CMakeLists.txt) to compile sqlite3.c yourself.
#if !defined(USE_BUNDLED_SQLITE) && __has_include(<winsqlite/winsqlite3.h>)
  #include <winsqlite/winsqlite3.h>
  #ifdef _MSC_VER
    #pragma comment(lib, "winsqlite3.lib")
  #endif
#else
  #include "sqlite3.h"
#endif

#ifdef _MSC_VER
  #pragma comment(lib, "comctl32.lib")
  #pragma comment(lib, "user32.lib")
  #pragma comment(lib, "gdi32.lib")
  #pragma comment(lib, "shell32.lib")
  #pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// ------------------------------------------------------------------ constants
static const wchar_t kMainClass[] = L"CountdownWidgetMain";
static const wchar_t kDlgClass[]  = L"CountdownWidgetDlg";
static const wchar_t kAppName[]   = L"Countdown Widget";

enum : int { ID_LIST = 100, ID_BTN_NEW, ID_BTN_EDIT, ID_BTN_DEL, ID_SEARCH,
             CMD_SHOW = 200, CMD_TOPMOST, CMD_QUIT, CMD_SEARCH };
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER (0x1500 + 1)
#endif
enum : UINT { WM_TRAY = WM_APP + 1, WM_SHOWAPP = WM_APP + 2 };
static const UINT_PTR TICK_ID = 1;
// list columns: [toggle] [label] [start] [ends] [remaining] [reset button]
enum : int { COL_TOGGLE = 0, COL_LABEL, COL_START, COL_END, COL_REMAIN, COL_RESET, COL_COUNT };
static const int64_t  RESET_SECONDS = 7LL * 86400;      // Reset restarts the countdown with exactly 7 days
static const int64_t  EPOCH_DIFF = 11644473600LL;   // 1601 -> 1970 in seconds

// ---------------------------------------------------------------------- model
struct Countdown {
    int64_t      id = 0;
    std::wstring label;
    int64_t      start = 0;      // UTC epoch seconds
    int64_t      duration = 0;   // seconds
    bool         n30 = false, n15 = false, n0 = false;   // notifications already sent
    bool         checked = false;                         // the per-row boolean toggle (default false)
    std::wstring shown;          // text currently displayed in the "Remaining" cell
    int64_t end() const { return start + duration; }
};

static std::vector<Countdown> g_items;

// -------------------------------------------------------------------- globals
static HINSTANCE g_hInst;
static HWND      g_hwnd, g_list, g_search, g_count, g_btnNew, g_btnEdit, g_btnDel;
static std::wstring g_filter;          // lower-cased search text
static std::vector<int> g_view;        // list row -> index in g_items (rows hidden by the filter are absent)
static HFONT     g_font;
static HICON     g_iconBig, g_iconSmall;
static NOTIFYICONDATAW g_nid;
static bool      g_topmost = false;
static UINT      g_dpi = 96;
static UINT      g_wmTaskbarCreated;
static sqlite3*  g_db;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

// ------------------------------------------------------------------ time utils
static int64_t FileTimeToEpoch(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000000ULL) - EPOCH_DIFF;
}
static int64_t NowEpoch() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return FileTimeToEpoch(ft);
}
static int64_t LocalToEpoch(const SYSTEMTIME& local) {
    SYSTEMTIME utc{}; FILETIME ft{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) utc = local;
    SystemTimeToFileTime(&utc, &ft);
    return FileTimeToEpoch(ft);
}
static SYSTEMTIME EpochToLocal(int64_t e) {
    ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)(e + EPOCH_DIFF) * 10000000ULL;
    FILETIME ft; ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME utc{}, loc{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc);
    return loc;
}
static std::wstring FmtDateTime(int64_t e) {
    SYSTEMTIME s = EpochToLocal(e);
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02d %02d:%02d", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute);
    return b;
}
static std::wstring FmtDur(int64_t s) {
    int64_t d = s / 86400; s %= 86400;
    wchar_t b[64];
    if (d > 0) swprintf(b, 64, L"%lldd %02lld:%02lld:%02lld", (long long)d, (long long)(s / 3600), (long long)((s % 3600) / 60), (long long)(s % 60));
    else       swprintf(b, 64, L"%02lld:%02lld:%02lld", (long long)(s / 3600), (long long)((s % 3600) / 60), (long long)(s % 60));
    return b;
}
static std::wstring RemainText(const Countdown& c, int64_t now) {
    if (now < c.start) return L"Starts in " + FmtDur(c.start - now);
    int64_t rem = c.end() - now;
    if (rem <= 0) return L"Refreshed";
    return FmtDur(rem);
}

// ------------------------------------------------------------------- database
static void DbExec(const char* sql) { sqlite3_exec(g_db, sql, nullptr, nullptr, nullptr); }

static bool DbOpen() {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return false;
    std::wstring dir = std::wstring(appdata) + L"\\CountdownWidget";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\timers.db";
    if (sqlite3_open16(path.c_str(), &g_db) != SQLITE_OK) return false;
    DbExec("CREATE TABLE IF NOT EXISTS timers("
           " id INTEGER PRIMARY KEY AUTOINCREMENT,"
           " label TEXT NOT NULL,"
           " start INTEGER NOT NULL,"
           " duration INTEGER NOT NULL,"
           " n30 INTEGER NOT NULL DEFAULT 0,"
           " n15 INTEGER NOT NULL DEFAULT 0,"
           " n0  INTEGER NOT NULL DEFAULT 0,"
           " checked INTEGER NOT NULL DEFAULT 0);"
           "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value INTEGER);");
    // databases created by an older version lack the column; this fails harmlessly if it already exists
    DbExec("ALTER TABLE timers ADD COLUMN checked INTEGER NOT NULL DEFAULT 0;");
    return true;
}

static void DbLoad() {
    g_items.clear();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare16_v2(g_db, L"SELECT id,label,start,duration,n30,n15,n0,checked FROM timers", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            Countdown c;
            c.id = sqlite3_column_int64(st, 0);
            const wchar_t* t = (const wchar_t*)sqlite3_column_text16(st, 1);
            c.label = t ? t : L"";
            c.start = sqlite3_column_int64(st, 2);
            c.duration = sqlite3_column_int64(st, 3);
            c.n30 = sqlite3_column_int(st, 4) != 0;
            c.n15 = sqlite3_column_int(st, 5) != 0;
            c.n0  = sqlite3_column_int(st, 6) != 0;
            c.checked = sqlite3_column_int(st, 7) != 0;
            g_items.push_back(std::move(c));
        }
    }
    sqlite3_finalize(st);
}

static int64_t DbInsert(const Countdown& c) {
    sqlite3_stmt* st = nullptr; int64_t id = 0;
    if (sqlite3_prepare16_v2(g_db, L"INSERT INTO timers(label,start,duration,n30,n15,n0) VALUES(?,?,?,?,?,?)", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text16(st, 1, c.label.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, c.start);
        sqlite3_bind_int64(st, 3, c.duration);
        sqlite3_bind_int(st, 4, c.n30);
        sqlite3_bind_int(st, 5, c.n15);
        sqlite3_bind_int(st, 6, c.n0);
        if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(g_db);
    }
    sqlite3_finalize(st);
    return id;
}
static void DbUpdateLabel(const Countdown& c) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare16_v2(g_db, L"UPDATE timers SET label=? WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text16(st, 1, c.label.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, c.id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}
static void DbUpdateFlags(const Countdown& c) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare16_v2(g_db, L"UPDATE timers SET n30=?,n15=?,n0=? WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, c.n30);
        sqlite3_bind_int(st, 2, c.n15);
        sqlite3_bind_int(st, 3, c.n0);
        sqlite3_bind_int64(st, 4, c.id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}
static void DbUpdateChecked(const Countdown& c) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare16_v2(g_db, L"UPDATE timers SET checked=? WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, c.checked);
        sqlite3_bind_int64(st, 2, c.id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}
static void DbUpdateRestart(const Countdown& c) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare16_v2(g_db, L"UPDATE timers SET start=?,duration=?,n30=0,n15=0,n0=0 WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, c.start);
        sqlite3_bind_int64(st, 2, c.duration);
        sqlite3_bind_int64(st, 3, c.id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}
static void DbDelete(int64_t id) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare16_v2(g_db, L"DELETE FROM timers WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}
static int GetSetting(const char* key, int def) {
    sqlite3_stmt* st = nullptr; int v = def;
    if (sqlite3_prepare_v2(g_db, "SELECT value FROM settings WHERE key=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}
static void SetSetting(const char* key, int v) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO settings(key,value) VALUES(?,?)", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, v);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

// ----------------------------------------------------------------- icon (GDI)
static HICON MakeClockIcon(int sz) {
    HDC scr = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(scr);
    HBITMAP color = CreateCompatibleBitmap(scr, sz, sz);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    RECT r{ 0, 0, sz, sz };
    const int pw = std::max(1, sz / 14);

    HGDIOBJ old = SelectObject(dc, color);
    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    HBRUSH face = CreateSolidBrush(RGB(0, 120, 215));
    HPEN rim = CreatePen(PS_SOLID, pw, RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(dc, face), op = SelectObject(dc, rim);
    Ellipse(dc, 1, 1, sz - 1, sz - 1);
    HPEN hand = CreatePen(PS_SOLID, std::max(1, sz / 11), RGB(255, 255, 255));
    SelectObject(dc, hand);
    MoveToEx(dc, sz / 2, sz / 2, nullptr); LineTo(dc, sz / 2, sz / 5);
    MoveToEx(dc, sz / 2, sz / 2, nullptr); LineTo(dc, sz * 7 / 10, sz / 2);
    SelectObject(dc, op); SelectObject(dc, ob);

    SelectObject(dc, mask);                      // 1 = transparent, 0 = opaque
    FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(dc, GetStockObject(BLACK_BRUSH));
    SelectObject(dc, GetStockObject(BLACK_PEN));
    Ellipse(dc, 1, 1, sz - 1, sz - 1);
    SelectObject(dc, old);

    ICONINFO ii{}; ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON ico = CreateIconIndirect(&ii);
    DeleteObject(face); DeleteObject(rim); DeleteObject(hand);
    DeleteObject(color); DeleteObject(mask);
    DeleteDC(dc); ReleaseDC(nullptr, scr);
    return ico;
}

// --------------------------------------------------------------------- tray
static void TrayAdd() {
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_iconSmall;
    wcscpy_s(g_nid.szTip, kAppName);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayRemove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

// On Windows 10 a tray balloon is displayed as a regular toast and kept in the Action Center.
static void Notify(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, _countof(n.szInfoTitle), title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, _countof(n.szInfo), text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ----------------------------------------------------------------- list view
static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
static bool Matches(const Countdown& c) { return g_filter.empty() || Lower(c.label).find(g_filter) != std::wstring::npos; }

// With a filter active, list rows no longer equal indices in g_items; these convert between the two.
static int ListToItem(int row) { return (row >= 0 && row < (int)g_view.size()) ? g_view[row] : -1; }
static int ItemToList(int idx) {
    for (int r = 0; r < (int)g_view.size(); ++r) if (g_view[r] == idx) return r;
    return -1;
}
static int SelIndex() { return ListToItem(ListView_GetNextItem(g_list, -1, LVNI_SELECTED)); }   // index in g_items

static void UpdateButtons() {
    BOOL has = SelIndex() >= 0;
    EnableWindow(g_btnEdit, has);
    EnableWindow(g_btnDel, has);
}

static void RefreshRow(int row, int64_t now) {
    int idx = ListToItem(row);
    if (idx < 0) return;
    Countdown& c = g_items[idx];
    std::wstring t = RemainText(c, now);
    if (t != c.shown) {
        c.shown = t;
        ListView_SetItemText(g_list, row, COL_REMAIN, (LPWSTR)c.shown.c_str());
    }
}

// "N items" for what is shown; "N of M items" when the search filter hides some.
static void UpdateCount() {
    if (!g_count) return;
    size_t v = g_view.size(), total = g_items.size();
    std::wstring t = std::to_wstring(v);
    if (v == total) t += (v == 1 ? L" item" : L" items");
    else            t += L" of " + std::to_wstring(total) + L" items";
    SetWindowTextW(g_count, t.c_str());
}

static void Rebuild(int64_t selectId = 0) {
    std::stable_sort(g_items.begin(), g_items.end(), [](const Countdown& a, const Countdown& b) {
        return a.end() != b.end() ? a.end() < b.end() : a.id < b.id;
    });
    int64_t now = NowEpoch();
    int sel = -1;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    g_view.clear();
    for (size_t i = 0; i < g_items.size(); ++i) if (Matches(g_items[i])) g_view.push_back((int)i);
    for (int row = 0; row < (int)g_view.size(); ++row) {
        Countdown& c = g_items[g_view[row]];
        c.shown.clear();
        LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = row;
        it.pszText = (LPWSTR)L""; it.lParam = (LPARAM)c.id;      // column 0 is the toggle, painted in custom draw
        ListView_InsertItem(g_list, &it);
        std::wstring s = FmtDateTime(c.start), e = FmtDateTime(c.end());
        ListView_SetItemText(g_list, row, COL_LABEL, (LPWSTR)c.label.c_str());
        ListView_SetItemText(g_list, row, COL_START, (LPWSTR)s.c_str());
        ListView_SetItemText(g_list, row, COL_END,   (LPWSTR)e.c_str());
        RefreshRow(row, now);
        if (c.id == selectId) sel = row;
    }
    if (sel >= 0) {
        ListView_SetItemState(g_list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, sel, FALSE);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    UpdateButtons();
    UpdateCount();
}

// ------------------------------------------------------------ create/edit dialog
struct DlgData {
    bool edit = false;
    std::wstring label;
    int64_t start = 0, duration = 0;
    bool ok = false;
    HWND eLabel = nullptr, dDate = nullptr, dTime = nullptr, eDays = nullptr, ud = nullptr, dDur = nullptr;
};

static HWND Ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void BuildDlg(HWND h, DlgData& d) {
    Ctl(h, L"STATIC", L"Label", SS_LEFT, 14, 18, 90, 20);
    d.eLabel = Ctl(h, L"EDIT", d.label.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, 110, 14, 250, 24, 0, WS_EX_CLIENTEDGE);
    SendMessageW(d.eLabel, EM_SETLIMITTEXT, 100, 0);

    Ctl(h, L"STATIC", L"Start", SS_LEFT, 14, 58, 90, 20);
    d.dDate = Ctl(h, DATETIMEPICK_CLASSW, L"", WS_TABSTOP | DTS_SHORTDATEFORMAT, 110, 54, 120, 24);
    d.dTime = Ctl(h, DATETIMEPICK_CLASSW, L"", WS_TABSTOP | DTS_UPDOWN, 238, 54, 122, 24);
    DateTime_SetFormat(d.dDate, L"yyyy-MM-dd");
    DateTime_SetFormat(d.dTime, L"HH':'mm':'ss");

    Ctl(h, L"STATIC", L"Duration", SS_LEFT, 14, 98, 90, 20);
    d.eDays = Ctl(h, L"EDIT", L"0", WS_TABSTOP | ES_NUMBER | ES_CENTER, 110, 94, 56, 24, 0, WS_EX_CLIENTEDGE);
    d.ud = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                           0, 0, 0, 0, h, nullptr, g_hInst, nullptr);
    SendMessageW(d.ud, UDM_SETBUDDY, (WPARAM)d.eDays, 0);
    SendMessageW(d.ud, UDM_SETRANGE32, 0, 3650);
    Ctl(h, L"STATIC", L"days", SS_LEFT, 172, 98, 36, 20);
    d.dDur = Ctl(h, DATETIMEPICK_CLASSW, L"", WS_TABSTOP | DTS_UPDOWN, 214, 94, 76, 24);
    DateTime_SetFormat(d.dDur, L"HH':'mm");
    Ctl(h, L"STATIC", L"(hh:mm)", SS_LEFT, 296, 98, 64, 20);

    Ctl(h, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 190, 138, 80, 26, IDOK);
    Ctl(h, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 280, 138, 80, 26, IDCANCEL);

    // initial values
    SYSTEMTIME st; if (d.edit) st = EpochToLocal(d.start); else GetLocalTime(&st);
    DateTime_SetSystemtime(d.dDate, GDT_VALID, &st);
    DateTime_SetSystemtime(d.dTime, GDT_VALID, &st);
    SYSTEMTIME ds; GetLocalTime(&ds);
    ds.wHour = (WORD)((d.duration % 86400) / 3600);
    ds.wMinute = (WORD)((d.duration % 3600) / 60);
    ds.wSecond = 0; ds.wMilliseconds = 0;
    DateTime_SetSystemtime(d.dDur, GDT_VALID, &ds);
    SendMessageW(d.ud, UDM_SETPOS32, 0, (LPARAM)(d.duration / 86400));

    if (d.edit) {   // update = label only
        for (HWND c : { d.dDate, d.dTime, d.eDays, d.ud, d.dDur }) EnableWindow(c, FALSE);
    }
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool Collect(HWND h, DlgData& d) {
    wchar_t buf[256]; GetWindowTextW(d.eLabel, buf, 256);
    d.label = Trim(buf);
    if (d.label.empty()) d.label = L"Untitled";
    if (d.edit) return true;

    SYSTEMTIME dt{}, tm{}, du{};
    DateTime_GetSystemtime(d.dDate, &dt);
    DateTime_GetSystemtime(d.dTime, &tm);
    DateTime_GetSystemtime(d.dDur, &du);
    SYSTEMTIME local = dt;
    local.wHour = tm.wHour; local.wMinute = tm.wMinute; local.wSecond = tm.wSecond; local.wMilliseconds = 0;
    d.start = LocalToEpoch(local);

    wchar_t nb[16]; GetWindowTextW(d.eDays, nb, 16);
    long days = wcstol(nb, nullptr, 10);
    days = std::max(0L, std::min(3650L, days));
    d.duration = (int64_t)days * 86400 + du.wHour * 3600 + du.wMinute * 60;
    if (d.duration <= 0) {
        MessageBoxW(h, L"The duration must be greater than zero.", kAppName, MB_ICONWARNING);
        return false;
    }
    return true;
}

static void CloseDlg(HWND h) {
    HWND owner = GetWindow(h, GW_OWNER);
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(h);
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    DlgData* d = (DlgData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE:
        d = (DlgData*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
        BuildDlg(h, *d);
        return 0;
    case WM_COMMAND:
        if (!d) break;
        if (LOWORD(w) == IDOK) { if (Collect(h, *d)) { d->ok = true; CloseDlg(h); } return 0; }
        if (LOWORD(w) == IDCANCEL) { CloseDlg(h); return 0; }
        break;
    case WM_CLOSE:
        CloseDlg(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool RunDlg(HWND owner, DlgData& d) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof wc };
        wc.lpfnWndProc = DlgProc; wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDlgClass;
        wc.hIcon = g_iconBig; wc.hIconSm = g_iconSmall;
        RegisterClassExW(&wc);
        registered = true;
    }
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
    RECT r{ 0, 0, S(374), S(178) };
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int w = r.right - r.left, h = r.bottom - r.top;
    RECT or_{}; GetWindowRect(owner, &or_);
    int x = or_.left + ((or_.right - or_.left) - w) / 2;
    int y = or_.top + ((or_.bottom - or_.top) - h) / 2;

    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(ex, kDlgClass, d.edit ? L"Rename countdown" : L"New countdown", style,
                               x, y, w, h, owner, nullptr, g_hInst, &d);
    if (!dlg) { EnableWindow(owner, TRUE); return false; }
    ShowWindow(dlg, SW_SHOW);
    SetFocus(d.eLabel);
    SendMessageW(d.eLabel, EM_SETSEL, 0, -1);

    MSG msg;
    while (IsWindow(dlg)) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) { PostQuitMessage((int)msg.wParam); break; }   // app is quitting
        if (got < 0) break;
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return d.ok;
}

// ---------------------------------------------------------------- CRUD actions
static void NewCountdown() {
    DlgData d;
    if (!RunDlg(g_hwnd, d)) return;
    Countdown c;
    c.label = d.label; c.start = d.start; c.duration = d.duration;
    c.n30 = d.duration <= 30 * 60;     // no "30 min left" alert if the timer is shorter than that
    c.n15 = d.duration <= 15 * 60;
    c.id = DbInsert(c);
    if (!c.id) { MessageBoxW(g_hwnd, L"Could not save to the database.", kAppName, MB_ICONERROR); return; }
    g_items.push_back(c);
    if (!Matches(c)) { g_filter.clear(); SetWindowTextW(g_search, L""); }   // don't hide the item that was just created
    Rebuild(c.id);
}

static void EditCountdown() {
    int i = SelIndex();
    if (i < 0 || i >= (int)g_items.size()) return;
    DlgData d; d.edit = true;
    d.label = g_items[i].label; d.start = g_items[i].start; d.duration = g_items[i].duration;
    if (!RunDlg(g_hwnd, d)) return;
    g_items[i].label = d.label;
    DbUpdateLabel(g_items[i]);
    Rebuild(g_items[i].id);
}

static void DeleteSelected() {
    int i = SelIndex();
    if (i < 0 || i >= (int)g_items.size()) return;
    std::wstring msg = L"Delete \"" + g_items[i].label + L"\"?\nThe whole countdown will be removed.";
    if (MessageBoxW(g_hwnd, msg.c_str(), L"Delete countdown", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    DbDelete(g_items[i].id);
    g_items.erase(g_items.begin() + i);
    Rebuild();
}

// Restart a countdown now, for exactly 7 days. Notification flags are cleared so the alerts fire again.
static void ResetItem(int row) {
    int i = ListToItem(row);
    if (i < 0) return;
    Countdown& c = g_items[i];
    c.start = NowEpoch();
    c.duration = RESET_SECONDS;
    c.n30 = c.n15 = c.n0 = false;
    DbUpdateRestart(c);
    int64_t id = c.id;
    Rebuild(id);                       // re-sorts (end time changed); 'c' must not be used after this
}

// The list view has no real buttons, so the last cell is painted like one and clicks are hit-tested.
static void DrawResetButton(HDC dc, int item) {
    RECT cell;
    if (!ListView_GetSubItemRect(g_list, item, COL_RESET, LVIR_BOUNDS, &cell)) return;
    FillRect(dc, &cell, GetSysColorBrush(COLOR_WINDOW));
    RECT b = cell; InflateRect(&b, -S(4), -S(2));
    DrawFrameControl(dc, &b, DFC_BUTTON, DFCS_BUTTONPUSH);
    HGDIOBJ old = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    DrawTextW(dc, L"Reset 7d", -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

// Paint the boolean toggle (a check box) in the first column.
static void DrawToggle(HDC dc, int item) {
    int idx = ListToItem(item);
    if (idx < 0) return;
    RECT cell;
    if (!ListView_GetSubItemRect(g_list, item, COL_TOGGLE, LVIR_LABEL, &cell)) return;
    FillRect(dc, &cell, GetSysColorBrush(COLOR_WINDOW));
    int side = S(15);
    RECT b{ cell.left + ((cell.right - cell.left) - side) / 2, cell.top + ((cell.bottom - cell.top) - side) / 2, 0, 0 };
    b.right = b.left + side; b.bottom = b.top + side;
    DrawFrameControl(dc, &b, DFC_BUTTON, DFCS_BUTTONCHECK | (g_items[idx].checked ? DFCS_CHECKED : 0));
}

static void ToggleItem(int row) {
    int i = ListToItem(row);
    if (i < 0) return;
    g_items[i].checked = !g_items[i].checked;
    DbUpdateChecked(g_items[i]);
    ListView_RedrawItems(g_list, row, row);
}

static COLORREF RowColor(int i) {   // i = index in g_items; CLR_INVALID = leave default colour
    if (i < 0 || i >= (int)g_items.size()) return CLR_INVALID;
    int64_t now = NowEpoch(), rem = g_items[i].end() - now;
    if (now < g_items[i].start) return RGB(120, 120, 120);
    if (rem <= 0)               return RGB(200, 0, 0);
    if (rem <= 15 * 60)         return RGB(210, 100, 0);
    if (rem <= 30 * 60)         return RGB(160, 120, 0);
    return CLR_INVALID;
}

// ------------------------------------------------------------- window helpers
static void SaveSettings() {
    if (!IsIconic(g_hwnd) && !IsZoomed(g_hwnd)) {
        RECT r; GetWindowRect(g_hwnd, &r);
        SetSetting("x", r.left); SetSetting("y", r.top);
        SetSetting("w", r.right - r.left); SetSetting("h", r.bottom - r.top);
    }
    SetSetting("topmost", g_topmost ? 1 : 0);
}

static void ShowMain() {
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

static void SetTopmost(bool on) {
    g_topmost = on;
    SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetSetting("topmost", on ? 1 : 0);
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_SHOW, L"Show widget");
    AppendMenuW(m, MF_STRING | (g_topmost ? MF_CHECKED : 0), CMD_TOPMOST, L"Always on top");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_QUIT, L"Quit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);                       // required so the menu closes when clicking elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void ShowListMenu(bool hasItem) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_BTN_NEW, L"New countdown\tCtrl+N");
    AppendMenuW(m, MF_STRING | (hasItem ? 0 : MF_GRAYED), ID_BTN_EDIT, L"Rename\tF2");
    AppendMenuW(m, MF_STRING | (hasItem ? 0 : MF_GRAYED), ID_BTN_DEL, L"Delete\tDel");
    POINT pt; GetCursorPos(&pt);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

static void Layout() {
    if (!g_list || !g_search || !g_count) return;
    RECT rc; GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(8), bh = S(28), bw = S(90), sh = S(24);
    MoveWindow(g_search, m, m, W - 2 * m, sh, TRUE);                 // search box on top
    int cy = m + sh + S(3), ch = S(18);
    MoveWindow(g_count, m + S(2), cy, W - 2 * m - S(2), ch, TRUE);   // item-count label between search and table
    int ly = cy + ch + S(3);
    MoveWindow(g_list, m, ly, W - 2 * m, H - 2 * m - bh - ly, TRUE);
    int y = H - m - bh;
    MoveWindow(g_btnNew,  m,                 y, bw, bh, TRUE);
    MoveWindow(g_btnEdit, m + (bw + m),      y, bw, bh, TRUE);
    MoveWindow(g_btnDel,  m + 2 * (bw + m),  y, bw, bh, TRUE);

    int ct = S(34), cr = S(90);                        // fixed-width toggle (left) and Reset (right) columns
    int total = W - 2 * m - S(6) - GetSystemMetrics(SM_CXVSCROLL) - ct - cr;
    int c0 = total * 28 / 100, c1 = total * 22 / 100, c2 = total * 22 / 100;
    ListView_SetColumnWidth(g_list, COL_TOGGLE, ct);
    ListView_SetColumnWidth(g_list, COL_LABEL, c0);
    ListView_SetColumnWidth(g_list, COL_START, c1);
    ListView_SetColumnWidth(g_list, COL_END, c2);
    ListView_SetColumnWidth(g_list, COL_REMAIN, std::max(S(60), total - c0 - c1 - c2));
    ListView_SetColumnWidth(g_list, COL_RESET, cr);
}

static void OnSearchChanged() {
    wchar_t buf[256]; GetWindowTextW(g_search, buf, 256);
    g_filter = Lower(buf);
    int si = SelIndex();
    Rebuild(si >= 0 ? g_items[si].id : 0);          // keep the selection if it is still visible
}

// ----------------------------------------------------------------- tick / alerts
static void OnTick() {
    int64_t now = NowEpoch();
    bool visible = IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd);
    for (size_t i = 0; i < g_items.size(); ++i) {
        Countdown& c = g_items[i];
        int64_t rem = c.end() - now;
        bool dirty = false;
        if (rem <= 0) {
            if (!c.n0) {
                Notify(L"Refreshed!", L"\"" + c.label + L"\" has refreshed.");
                c.n0 = c.n15 = c.n30 = true; dirty = true;
                if (!c.checked) {                       // timeout automatically turns the checkbox on
                    c.checked = true;
                    DbUpdateChecked(c);
                    int row = ItemToList((int)i);
                    if (visible && row >= 0) ListView_RedrawItems(g_list, row, row);
                }
            }
        } else if (rem <= 15 * 60) {
            if (!c.n15) {
                Notify(L"15 minutes left", L"\"" + c.label + L"\" refreshes in 15 minutes.");
                c.n15 = c.n30 = true; dirty = true;
            }
        } else if (rem <= 30 * 60) {
            if (!c.n30) {
                Notify(L"30 minutes left", L"\"" + c.label + L"\" refreshes in 30 minutes.");
                c.n30 = true; dirty = true;
            }
        }
        if (dirty) DbUpdateFlags(c);
    }
    if (visible)
        for (int row = 0; row < (int)g_view.size(); ++row) RefreshRow(row, now);
}

// --------------------------------------------------------------------- main proc
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_wmTaskbarCreated && msg != 0) { TrayAdd(); return 0; }   // explorer.exe restarted

    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_SEARCH, g_hInst, nullptr);
        SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_search, EM_SETLIMITTEXT, 100, 0);
        SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search label...  (Ctrl+F)");
        g_count = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                  0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        SendMessageW(g_count, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_LIST, g_hInst, nullptr);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        const wchar_t* heads[COL_COUNT] = { L"", L"Label", L"Start", L"Ends", L"Remaining", L"" };
        for (int i = 0; i < COL_COUNT; ++i) {
            LVCOLUMNW col{}; col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = (LPWSTR)heads[i]; col.cx = S(100); col.iSubItem = i;
            ListView_InsertColumn(g_list, i, &col);
        }
        g_btnNew  = Ctl(hwnd, L"BUTTON", L"New",    WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, ID_BTN_NEW);
        g_btnEdit = Ctl(hwnd, L"BUTTON", L"Rename", WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, ID_BTN_EDIT);
        g_btnDel  = Ctl(hwnd, L"BUTTON", L"Delete", WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, ID_BTN_DEL);
        TrayAdd();
        Rebuild();
        SetTimer(hwnd, TICK_ID, 250, nullptr);
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(560);
        mm->ptMinTrackSize.y = S(220);
        return 0;
    }
    case WM_NCHITTEST: {                 // drag the widget from any empty client area
        LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
        if (r == HTCLIENT) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &p);
            if (ChildWindowFromPoint(hwnd, p) == hwnd) return HTCAPTION;
        }
        return r;
    }
    case WM_TIMER:
        if (wp == TICK_ID) OnTick();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_NEW:  NewCountdown();    break;
        case ID_BTN_EDIT: EditCountdown();   break;
        case ID_BTN_DEL:  DeleteSelected();  break;
        case CMD_SHOW:    ShowMain();        break;
        case ID_SEARCH:   if (HIWORD(wp) == EN_CHANGE) OnSearchChanged(); break;
        case CMD_SEARCH:  SetFocus(g_search); SendMessageW(g_search, EM_SETSEL, 0, -1); break;
        case CMD_TOPMOST: SetTopmost(!g_topmost); break;
        case CMD_QUIT:    SaveSettings(); DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->hwndFrom != g_list) break;
        switch (nh->code) {
        case NM_CUSTOMDRAW: {
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lp;
            switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:     return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
            case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                int i = (int)cd->nmcd.dwItemSpec;                    // list row
                if (cd->iSubItem == COL_TOGGLE) { DrawToggle(cd->nmcd.hdc, (int)i); return CDRF_SKIPDEFAULT; }
                if (cd->iSubItem == COL_RESET)  { DrawResetButton(cd->nmcd.hdc, (int)i); return CDRF_SKIPDEFAULT; }
                COLORREF col = RowColor(ListToItem(i));
                if (col != CLR_INVALID) cd->clrText = col;
                return CDRF_NEWFONT;
            }
            }
            return CDRF_DODEFAULT;
        }
        case NM_CLICK: {
            LVHITTESTINFO ht{}; ht.pt = ((NMITEMACTIVATE*)lp)->ptAction;
            ListView_SubItemHitTest(g_list, &ht);
            if (ht.iItem >= 0 && ht.iSubItem == COL_RESET)  ResetItem(ht.iItem);
            if (ht.iItem >= 0 && ht.iSubItem == COL_TOGGLE) ToggleItem(ht.iItem);
            return 0;
        }
        case LVN_ITEMCHANGED: UpdateButtons(); return 0;
        case NM_DBLCLK: {
            LVHITTESTINFO ht{}; ht.pt = ((NMITEMACTIVATE*)lp)->ptAction;
            ListView_SubItemHitTest(g_list, &ht);
            if (ht.iItem >= 0 && ht.iSubItem != COL_RESET && ht.iSubItem != COL_TOGGLE) EditCountdown();   // button cells must not open rename
            return 0;
        }
        case NM_RCLICK: {
            int it = ((NMITEMACTIVATE*)lp)->iItem;
            if (it >= 0) ListView_SetItemState(g_list, it, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ShowListMenu(it >= 0);
            return 0;
        }
        case LVN_KEYDOWN: {
            WORD k = ((NMLVKEYDOWN*)lp)->wVKey;
            if (k == VK_DELETE) DeleteSelected();
            else if (k == VK_F2) EditCountdown();
            else if (k == VK_INSERT) NewCountdown();
            return 0;
        }
        }
        break;
    }
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: ShowMain(); break;
        case WM_RBUTTONUP:     ShowTrayMenu(); break;   // the only route to "Quit"
        }
        return 0;

    case WM_SHOWAPP:        // second instance was launched
        ShowMain();
        return 0;

    case WM_CLOSE: {        // closing = hide into the tray
        SaveSettings();
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TICK_ID);
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------------ entry
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nShow) {
    g_hInst = hInst;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CountdownWidget.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kMainClass, nullptr);
        if (other) PostMessageW(other, WM_SHOWAPP, 0, 0);
        return 0;
    }

    SetProcessDPIAware();
    HDC sdc = GetDC(nullptr); g_dpi = (UINT)GetDeviceCaps(sdc, LOGPIXELSX); ReleaseDC(nullptr, sdc);

    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    if (!DbOpen()) {
        MessageBoxW(nullptr, L"Could not open the SQLite database in %APPDATA%\\CountdownWidget.", kAppName, MB_ICONERROR);
        return 1;
    }
    DbLoad();
    g_topmost = GetSetting("topmost", 0) != 0;
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_iconBig   = MakeClockIcon(GetSystemMetrics(SM_CXICON));
    g_iconSmall = MakeClockIcon(GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kMainClass;
    wc.hIcon = g_iconBig; wc.hIconSm = g_iconSmall;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(g_topmost ? WS_EX_TOPMOST : 0, kMainClass, kAppName, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(600), S(340), nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // restore last position/size if it is still on a connected monitor
    RECT saved{ GetSetting("x", 0), GetSetting("y", 0), 0, 0 };
    int sw = GetSetting("w", 0), sh = GetSetting("h", 0);
    if (sw > 0 && sh > 0) {
        saved.right = saved.left + sw; saved.bottom = saved.top + sh;
        if (MonitorFromRect(&saved, MONITOR_DEFAULTTONULL))
            SetWindowPos(hwnd, nullptr, saved.left, saved.top, sw, sh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Start hidden: the window is created without WS_VISIBLE and is not shown here, so the app
    // begins in the system tray. Left-click the tray icon (or launch the exe again) to open it.
    (void)nShow;

    ACCEL acc[2] = {};
    acc[0].fVirt = FCONTROL | FVIRTKEY; acc[0].key = 'N'; acc[0].cmd = ID_BTN_NEW;   // Ctrl+N = new countdown
    acc[1].fVirt = FCONTROL | FVIRTKEY; acc[1].key = 'F'; acc[1].cmd = CMD_SEARCH;   // Ctrl+F = focus search
    HACCEL accel = CreateAcceleratorTableW(acc, 2);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(hwnd, accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    sqlite3_close(g_db);
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}

/* gui-win32.h — native Win32 GUI for hp-supplies (Windows)
 *
 * Included by src/main.c (same translation unit) when building on Windows
 * without -DHP_GUI_GTK. Replaces the GTK3 GUI so the Windows build only
 * depends on libusb plus system DLLs that ship with Windows 7 SP1
 * (msvcrt.dll, user32/gdi32/comctl32/ws2_32/setupapi/advapi32/cfgmgr32).
 * A single 32-bit binary therefore runs on Windows 7 SP1 through Windows 11,
 * 32-bit and 64-bit (WoW64), with no runtime to install.
 *
 * Functionality mirrors the GTK GUI: product/state line (green/amber/red),
 * usage frame, up to 4 supply cards with color-coded progress bars,
 * 10 s auto-refresh checkbox, refresh button, status line, worker thread
 * (CreateThread + PostMessage instead of GThread + gdk idle).
 * All UI strings stay UTF-8 (identical to the GTK version) and are
 * converted to UTF-16 on the fly via u8w(), so the binary is not tied to
 * the ANSI code page of the machine.
 *
 * License: MIT
 */
#ifndef HP_GUI_WIN32_H
#define HP_GUI_WIN32_H

#include <windows.h>
#include <commctrl.h>

/* ---- window geometry ---- */
#define HPW_W 800
#define HPW_H 660

#define HP_TIMER_AUTO  1
#define HP_WM_DONE     (WM_APP + 1)

/* control ids */
#define HP_ID_PRODUCT  101
#define HP_ID_STATE    102
#define HP_ID_USAGEFR  103
#define HP_ID_USAGEBIG 104
#define HP_ID_USAGESUB 105
#define HP_ID_CHK      120
#define HP_ID_BTN      121
#define HP_ID_STATUS   122
#define HP_ID_CARD(c, k) (200 + (c) * 10 + (k))
#define HP_IS_CARD(id)   ((id) >= 200 && (id) < 240)

/* card control slots */
#define HP_K_GROUP  0
#define HP_K_DOT    1
#define HP_K_NAME   2
#define HP_K_BRAND  3
#define HP_K_BAR    4
#define HP_K_PCT    5
#define HP_K_DETAIL 6
#define HP_K_META   7
#define HP_K_MAX    8

/* palette (mirrors css_apply in main.c) */
#define HP_COL_OK    RGB(0x2e, 0x8b, 0x57)
#define HP_COL_WARN  RGB(0xd1, 0x8f, 0x00)
#define HP_COL_ERR   RGB(0xd9, 0x30, 0x25)
#define HP_COL_BLUE  RGB(0x1a, 0x73, 0xe8)
#define HP_COL_MUTED RGB(0x5f, 0x63, 0x68)
#define HP_COL_FG    RGB(0x20, 0x21, 0x24)
#define HP_COL_BARBG RGB(0xe9, 0xe9, 0xe9)

/* per-toner colors: K / C / M / Y (dot label + progress fill, mirrors CSS) */
static const COLORREF hp_dot_col[4] = {
    RGB(0x9a, 0xa0, 0xa6),   /* K */
    RGB(0x00, 0xae, 0xef),   /* C */
    RGB(0xec, 0x00, 0x8c),   /* M */
    RGB(0xf4, 0xb4, 0x00)    /* Y */
};

/* ---- state ---- */
static HWND hp_win;
static HWND hp_product, hp_state, hp_usagefrm, hp_usage_big, hp_usage_sub;
static HWND hp_card[4][HP_K_MAX];
static HWND hp_chk, hp_btn, hp_status;
static HFONT hp_ft_h1, hp_ft_state, hp_ft_big, hp_ft_dot, hp_ft_main, hp_ft_small;
static COLORREF hp_state_col = HP_COL_FG, hp_status_col = HP_COL_FG;
static volatile LONG hp_busy = 0;
static uint16_t hp_vid, hp_pid;
static char *hp_host;
static int hp_shown = 0;

static void start_refresh(void);                                /* fwd */
static void apply_snapshot_to_controls(const Snapshot *snap);   /* fwd */

/* ---- helpers ---- */

/* UTF-8 → UTF-16 */
static wchar_t *u8w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, n);
    return w;
}

static void settext(HWND h, const char *utf8)
{
    wchar_t *w = u8w(utf8);
    if (w) {
        SetWindowTextW(h, w);
        free(w);
    }
}

/* 1234567 → "1,234,567" (deterministic, locale-independent) */
static void hp_fmt_grouped(long v, char *out, size_t n)
{
    char tmp[24];
    int neg, len, start, digits, commas, cnt, i;
    char *p;
    snprintf(tmp, sizeof tmp, "%ld", v);
    neg = (tmp[0] == '-');
    len = (int)strlen(tmp);
    start = neg ? 1 : 0;
    digits = len - start;
    commas = digits ? (digits - 1) / 3 : 0;
    if ((size_t)len + commas + 1 > n) { snprintf(out, n, "%ld", v); return; }
    p = out + len + commas;
    *p-- = '\0';
    cnt = 0;
    for (i = len - 1; i >= start; i--) {
        *p-- = tmp[i];
        if (++cnt % 3 == 0 && i > start) *p-- = ',';
    }
    if (neg) *p = '-';
}

static HFONT mkfont(int pt, int weight, int dpi)
{
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, FALSE, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

static HWND mkctrl(const wchar_t *cls, const char *utf8, DWORD style, int id,
                   HFONT f)
{
    wchar_t *w = u8w(utf8);
    /* 创建时不可见: 先定位后显示, 避免初始 (0,0) 位置的绘制残留 */
    HWND h = CreateWindowExW(0, cls, w, WS_CHILD | style,
                             0, 0, 10, 10, hp_win, (HMENU)(INT_PTR)id,
                             NULL, NULL);
    free(w);
    if (h && f) SendMessageW(h, WM_SETFONT, (WPARAM)f, MAKELPARAM(TRUE, 0));
    return h;
}

static void set_status(const char *utf8, COLORREF col)
{
    /* 错误信息可能含 \n, STATIC 不换行, 规整成空格 */
    char flat[512];
    size_t j = 0;
    for (size_t i = 0; utf8 && utf8[i] && j + 1 < sizeof flat; i++)
        flat[j++] = (utf8[i] == '\n') ? ' ' : utf8[i];
    flat[j] = '\0';
    hp_status_col = col;
    settext(hp_status, flat);
}

/* ---- snapshot → controls (mirrors apply_snapshot_to_ui in main.c) ---- */

static void apply_snapshot_to_controls(const Snapshot *snap)
{
    char t[220];
    if (snap->error[0]) {
        set_status(snap->error, HP_COL_ERR);
        return;
    }

    snprintf(t, sizeof t, "%s  ·  S/N %s", snap->model, snap->serial);
    settext(hp_product, t);

    /* machine state line: firmware LocString or mapped category + alerts */
    {
        const char *base = snap->status_loc[0] ? snap->status_loc
                         : status_cat_cn(snap->status_cat);
        int level = snapshot_alert_level(snap);
        int has_warn = FALSE;
        size_t len = 0;
        char buf[2048];
        if (level == 2) {
            snprintf(buf, sizeof buf, "⚠ 打印机错误: %s",
                     base[0] ? base : "请查看打印机面板");
            hp_state_col = HP_COL_ERR;
        } else {
            if (base[0]) len = strlen(base);
            memcpy(buf, base, len);
            for (int i = 0; i < snap->nalerts; i++) {
                const Alert *a = &snap->alerts[i];
                const char *mc, *txt;
                if (!strcmp(a->severity, "Info")) continue;
                mc = marker_color_cn(a->color);
                txt = a->loc[0] ? a->loc : alert_id_cn(a->id);
                len += (size_t)snprintf(buf + len, sizeof buf - len,
                                        "%s%s%s%s", len ? "    " : "",
                                        mc, mc[0] ? " " : "", txt);
                has_warn = TRUE;
            }
            if (!len) memcpy(buf, "正常", 7), buf[7] = '\0', len = 6;
            hp_state_col = has_warn ? HP_COL_WARN : HP_COL_OK;
        }
        settext(hp_state, buf);
    }

    hp_fmt_grouped(snap->total, t, sizeof t);
    settext(hp_usage_big, t);
    {
        char m[24], c[24];
        hp_fmt_grouped(snap->mono, m, sizeof m);
        hp_fmt_grouped(snap->color, c, sizeof c);
        snprintf(t, sizeof t, "黑白 %s 页    彩色 %s 页    卡纸 %d 次    进纸失败 %d 次",
                 m, c, snap->jams, snap->mispicks);
        settext(hp_usage_sub, t);
    }

    for (int i = 0; i < 4; i++) {
        BOOL show = i < snap->nsupplies;
        for (int k = 0; k < HP_K_MAX; k++)
            ShowWindow(hp_card[i][k], show ? SW_SHOWNA : SW_HIDE);
        if (!show) continue;

        Supply s = snap->supplies[i];
        snprintf(t, sizeof t, "%s  %s", color_cn(s.label, s.family), s.product);
        settext(hp_card[i][HP_K_NAME], t);

        settext(hp_card[i][HP_K_BRAND],
                !strncmp(s.brand, "clone", 5) ? "兼容耗材"
                : !strcmp(s.brand, "HP")      ? "原装 HP" : "");

        SendMessageW(hp_card[i][HP_K_BAR], PBM_SETRANGE32, 0, MAKELPARAM(0, 100));
        SendMessageW(hp_card[i][HP_K_BAR], PBM_SETPOS,
                     (WPARAM)(s.level >= 0 ? (s.level < 0 ? 0 :
                              s.level > 100 ? 100 : s.level) : 0), 0);
        SendMessageW(hp_card[i][HP_K_BAR], PBM_SETBARCOLOR, 0, hp_dot_col[i]);

        char lvl[16];
        if (s.level >= 0) snprintf(lvl, sizeof lvl, "%d%%", s.level);
        else snprintf(lvl, sizeof lvl, "—");
        settext(hp_card[i][HP_K_PCT], lvl);

        const char *statecn =
            !strcmp(s.qty, "veryLow") ? "严重不足" :
            !strcmp(s.qty, "low")     ? "不足"     :
            !strcmp(s.qty, "ok")      ? "正常"     :
            !strcmp(s.state, "used")  ? "已用尽"   :
            !strcmp(s.state, "new")   ? "全新"     :
                                        (s.qty[0] ? s.qty : s.state);
        {
            char p1[24], p2[24];
            hp_fmt_grouped(s.printed, p1, sizeof p1);
            hp_fmt_grouped(s.est_remaining, p2, sizeof p2);
            snprintf(t, sizeof t, "状态: %s    本支已打印 %s 页    预计还能打 %s 页",
                     statecn, p1, p2);
            settext(hp_card[i][HP_K_DETAIL], t);
        }
        snprintf(t, sizeof t, "装机 %s    标称 %d 页    S/N %s",
                 s.installed[0] ? s.installed : "?", s.yield, s.serial);
        settext(hp_card[i][HP_K_META], t);
    }
    set_status("已连接（USB LEDM）", HP_COL_FG);
}

/* ---- worker thread (mirrors job_worker/start_refresh in main.c) ---- */

static DWORD WINAPI job_worker(LPVOID arg)
{
    (void)arg;
    Snapshot *snap = (Snapshot *)calloc(1, sizeof *snap);
    if (snap) snapshot_build(hp_vid, hp_pid, hp_host, snap);
    PostMessageW(hp_win, HP_WM_DONE, 0, (LPARAM)snap);
    return 0;
}

static void start_refresh(void)
{
    if (InterlockedCompareExchange(&hp_busy, 1, 0) != 0) return;
    EnableWindow(hp_btn, FALSE);
    set_status("查询中…", HP_COL_FG);
    settext(hp_state, "查询中…");
    HANDLE t = CreateThread(NULL, 0, job_worker, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    } else {                      /* thread creation failed: recover */
        EnableWindow(hp_btn, TRUE);
        InterlockedExchange(&hp_busy, 0);
    }
}

/* ---- layout ---- */

static void relayout(int cw, int ch)
{
    const int M = 14, GAP = 10;
    int y = M;
    MoveWindow(hp_product, M, y, cw - 2 * M, 26, TRUE); y += 28;
    MoveWindow(hp_state, M, y, cw - 2 * M, 42, TRUE);   y += 46;

    /* 分组框标题占顶部 ~20px, 子控件下移避免不透明静态控件盖住标题 */
    int uh = 96;
    MoveWindow(hp_usagefrm, M, y, cw - 2 * M, uh, TRUE);
    MoveWindow(hp_usage_big, M + 12, y + 24, 300, 40, TRUE);
    MoveWindow(hp_usage_sub, M + 12, y + 68, cw - 2 * M - 24, 20, TRUE);
    y += uh + GAP;

    int footH = 30;
    int gridH = ch - y - footH - GAP - 4;
    if (gridH < 2 * 96 + GAP) gridH = 2 * 96 + GAP;
    int cardW = (cw - 2 * M - GAP) / 2;
    int cardH = (gridH - GAP) / 2;

    for (int i = 0; i < 4; i++) {
        int cx = M + (i % 2) * (cardW + GAP);
        int cy = y + (i / 2) * (cardH + GAP);
        MoveWindow(hp_card[i][HP_K_GROUP], cx, cy, cardW, cardH, TRUE);
        MoveWindow(hp_card[i][HP_K_DOT],    cx + 10, cy + 8,  22, 22, TRUE);
        MoveWindow(hp_card[i][HP_K_NAME],   cx + 34, cy + 10, cardW - 130, 18, TRUE);
        MoveWindow(hp_card[i][HP_K_BRAND],  cx + cardW - 92, cy + 11, 82, 16, TRUE);
        MoveWindow(hp_card[i][HP_K_BAR],    cx + 10, cy + 36, cardW - 74, 16, TRUE);
        MoveWindow(hp_card[i][HP_K_PCT],    cx + cardW - 60, cy + 35, 52, 18, TRUE);
        MoveWindow(hp_card[i][HP_K_DETAIL], cx + 10, cy + 60, cardW - 20, 16, TRUE);
        MoveWindow(hp_card[i][HP_K_META],   cx + 10, cy + 82, cardW - 20, 15, TRUE);
    }

    int fy = ch - footH - 4;
    MoveWindow(hp_chk, M, fy + 4, 150, 22, TRUE);
    MoveWindow(hp_status, M + 158, fy - 2, cw - 2 * M - 158 - 96, 30, TRUE);
    MoveWindow(hp_btn, cw - M - 88, fy, 88, 27, TRUE);
}

/* 布局到位后一次性显示全部控件并整窗重擦 */
static void show_all_controls(void)
{
    ShowWindow(hp_product, SW_SHOWNA);
    ShowWindow(hp_state, SW_SHOWNA);
    ShowWindow(hp_usagefrm, SW_SHOWNA);
    ShowWindow(hp_usage_big, SW_SHOWNA);
    ShowWindow(hp_usage_sub, SW_SHOWNA);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < HP_K_MAX; k++)
            ShowWindow(hp_card[i][k], SW_SHOWNA);
    ShowWindow(hp_chk, SW_SHOWNA);
    ShowWindow(hp_btn, SW_SHOWNA);
    ShowWindow(hp_status, SW_SHOWNA);
    InvalidateRect(hp_win, NULL, TRUE);
}

/* ---- construction ---- */

static void build_controls(void)
{
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);

    hp_ft_h1    = mkfont(11, FW_SEMIBOLD, dpi);
    hp_ft_state = mkfont(10, FW_SEMIBOLD, dpi);
    hp_ft_big   = mkfont(22, FW_BOLD,     dpi);
    hp_ft_dot   = mkfont(13, FW_BOLD,     dpi);
    hp_ft_main  = mkfont(9,  FW_NORMAL,   dpi);
    hp_ft_small = mkfont(8,  FW_NORMAL,   dpi);

    hp_product = mkctrl(L"STATIC", "未连接", 0, HP_ID_PRODUCT, hp_ft_h1);
    hp_state   = mkctrl(L"STATIC", "—", 0, HP_ID_STATE, hp_ft_state);
    hp_usagefrm = mkctrl(L"BUTTON", "总打印页数",
                         BS_GROUPBOX, HP_ID_USAGEFR, hp_ft_main);
    hp_usage_big = mkctrl(L"STATIC", "—", 0, HP_ID_USAGEBIG, hp_ft_big);
    hp_usage_sub = mkctrl(L"STATIC", "—", 0, HP_ID_USAGESUB, hp_ft_main);

    const char *dots[4] = { "●", "●", "●", "●" };
    for (int i = 0; i < 4; i++) {
        hp_card[i][HP_K_GROUP] = mkctrl(L"BUTTON", "",
                                        BS_GROUPBOX, HP_ID_CARD(i, HP_K_GROUP),
                                        NULL);
        hp_card[i][HP_K_DOT] = mkctrl(L"STATIC", dots[i], 0,
                                      HP_ID_CARD(i, HP_K_DOT), hp_ft_dot);
        hp_card[i][HP_K_NAME] = mkctrl(L"STATIC", "", 0,
                                       HP_ID_CARD(i, HP_K_NAME), hp_ft_main);
        hp_card[i][HP_K_BRAND] = mkctrl(L"STATIC", "", 0,
                                        HP_ID_CARD(i, HP_K_BRAND), hp_ft_small);
        hp_card[i][HP_K_BAR] = mkctrl(PROGRESS_CLASSW, NULL, PBS_SMOOTH,
                                      HP_ID_CARD(i, HP_K_BAR), NULL);
        SendMessageW(hp_card[i][HP_K_BAR], PBM_SETBKCOLOR, 0, HP_COL_BARBG);
        hp_card[i][HP_K_PCT] = mkctrl(L"STATIC", "—", 0,
                                      HP_ID_CARD(i, HP_K_PCT), hp_ft_main);
        hp_card[i][HP_K_DETAIL] = mkctrl(L"STATIC", "", 0,
                                         HP_ID_CARD(i, HP_K_DETAIL), hp_ft_main);
        hp_card[i][HP_K_META] = mkctrl(L"STATIC", "", 0,
                                       HP_ID_CARD(i, HP_K_META), hp_ft_small);
    }

    hp_chk = mkctrl(L"BUTTON", "每10秒自动刷新", BS_AUTOCHECKBOX,
                    HP_ID_CHK, hp_ft_main);
    hp_btn = mkctrl(L"BUTTON", "刷新", BS_PUSHBUTTON, HP_ID_BTN, hp_ft_main);
    hp_status = mkctrl(L"STATIC", "准备…", 0, HP_ID_STATUS, hp_ft_main);
}

/* ---- window procedure ---- */

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        hp_win = w;
        build_controls();
        return 0;
    case WM_SIZE:
        relayout(LOWORD(lp), HIWORD(lp));
        if (!hp_shown) {
            show_all_controls();
            hp_shown = 1;
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND h = (HWND)lp;
        int id = GetDlgCtrlID(h);
        COLORREF c = HP_COL_FG;
        if (h == hp_state) c = hp_state_col;
        else if (h == hp_status) c = hp_status_col;
        else if (id == HP_ID_USAGEBIG) c = HP_COL_BLUE;
        else if (HP_IS_CARD(id)) {
            int i = (id - 200) / 10, k = (id - 200) % 10;
            if (k == HP_K_DOT)   c = hp_dot_col[i];
            else if (k == HP_K_BRAND) c = HP_COL_BLUE;
            else if (k == HP_K_META)  c = HP_COL_MUTED;
        }
        SetTextColor(dc, c);
        SetBkMode(dc, TRANSPARENT);
        /* 必须返回实体背景刷: NULL_BRUSH 不擦底, 文字更新后旧字残留与新字
         * 叠成乱码(实测); 刷子颜色与窗口背景一致, 视觉等同透明 */
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            if (LOWORD(wp) == HP_ID_BTN) {
                start_refresh();
            } else if (LOWORD(wp) == HP_ID_CHK) {
                if (SendMessageW(hp_chk, BM_GETCHECK, 0, 0) == BST_CHECKED)
                    SetTimer(w, HP_TIMER_AUTO, 10000, NULL);
                else
                    KillTimer(w, HP_TIMER_AUTO);
            }
        }
        return 0;
    case WM_TIMER:
        if (wp == HP_TIMER_AUTO) start_refresh();
        return 0;
    case HP_WM_DONE: {
        Snapshot *snap = (Snapshot *)lp;
        if (snap) {
            apply_snapshot_to_controls(snap);
            free(snap);
        }
        EnableWindow(hp_btn, TRUE);
        InterlockedExchange(&hp_busy, 0);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 620;
        mmi->ptMinTrackSize.y = 520;
        return 0;
    }
    case WM_DESTROY:
        snapshot_shutdown();          /* mirrors GTK on_delete */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* ---- entry point (same signature as the GTK gui_run) ---- */

static int gui_run(uint16_t vid, uint16_t pid, const char *host)
{
    hp_vid = vid;
    hp_pid = pid;
    hp_host = _strdup(host ? host : "");
    if (!hp_host) hp_host = _strdup("");

    /* 单 exe 合并: 本程序是控制台子系统, 资源管理器双击启动时系统会新建
     * 一个控制台。若该控制台仅承载本进程 (GetConsoleProcessList<=1),
     * 判定为双击启动 → 隐藏它再进 GUI; 从终端启动则共享已有控制台,
     * 保持不动, CLI 语义 (阻塞/退出码/输出) 完整保留。 */
    {
        HWND cw = GetConsoleWindow();
        if (cw) {
            DWORD list[64], cnt = GetConsoleProcessList(list, 64);
            if (cnt <= 1) ShowWindow(cw, SW_HIDE);
        }
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES |
                                             ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    HINSTANCE inst = GetModuleHandleW(NULL);
    HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!icon) icon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hbrBackground = (HBRUSH)(1 + COLOR_BTNFACE);
    wc.lpszClassName = L"hp_supplies_w32";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassExW(&wc)) return 1;

    wchar_t *title = u8w(APP_NAME);
    HWND w = CreateWindowExW(0, wc.lpszClassName, title ? title : L"hp-supplies",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             HPW_W, HPW_H, NULL, NULL, wc.hInstance, NULL);
    free(title);
    if (!w) return 1;

    ShowWindow(w, SW_SHOWNORMAL);
    UpdateWindow(w);
    start_refresh();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    free(hp_host);
    return 0;
}

#endif /* HP_GUI_WIN32_H */

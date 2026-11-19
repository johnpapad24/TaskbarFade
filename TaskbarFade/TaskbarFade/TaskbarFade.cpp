// TaskbarFade.cpp
// Windows 10 taskbar: translucent while the mouse is away, opaque while it hovers.
// Lives in the system tray (click the icon for options).
//
// The taskbar keeps one colour in both states; only the opacity changes. By default that colour is the
// Start menu's. Choose the opacity for each state, the colour, an optional blur and how faded the icons
// get from the tray menu. Settings are remembered. Exiting (or pausing) puts the taskbar back the way
// Windows had it. It stays in its hover look while the Start menu, Search or notifications are open.
// While opaque it hands the taskbar back to Explorer, so you see Windows' own look (your blur, transparency
// and colours as you had them set) instead of a solid colour.
//
// Uses the undocumented SetWindowCompositionAttribute API (the same one TranslucentTB uses),
// limited to accent states 1-3, which every Windows 10 build supports. Icons are faded with ordinary
// layered-window alpha on the taskbar window; while that is active the taskbar colour is drawn by a
// separate click-through window right behind it, so icon opacity and taskbar opacity are independent.
//
// Build with MSVC (Start menu > "x64 Native Tools Command Prompt for VS", cd to this folder):
//   cl /O2 /EHsc TaskbarFade.cpp /link /SUBSYSTEM:WINDOWS
// Build with MinGW-w64:
//   g++ -O2 -mwindows -static TaskbarFade.cpp -o TaskbarFade.exe -luser32 -lshell32 -ladvapi32 -lcomdlg32 -lgdi32
// In the Visual Studio IDE, add this file to an Empty Project or a Console App project; both link.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00                   // Windows 10
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS") // GUI app: no console window, entry point is WinMain
#endif

// ---- Undocumented API plumbing ----------------------------------------------
struct ACCENT_POLICY { int State; int Flags; unsigned Color; int Anim; };
struct WCA_DATA { int Attr; void* Data; ULONG Size; };
enum { ACCENT_OFF = 0, ACCENT_SOLID = 1, ACCENT_CLEAR = 2, ACCENT_BLUR = 3 };
static BOOL(WINAPI* pSetWCA)(HWND, WCA_DATA*);

// ---- Tweak to taste ----------------------------------------------------------
static const COLORREF  kStartDark = RGB(31, 31, 31), kStartLight = RGB(230, 230, 230);   // Start menu greys when Windows isn't colouring it
static const ULONGLONG kLingerMs = 250;                                          // stay up briefly after the mouse leaves

// ---- State -------------------------------------------------------------------
static const UINT WM_TRAY = WM_APP + 1;
enum { ID_TOGGLE = 1, ID_BLUR, ID_AUTOSTART, ID_EXIT, ID_COLOR_AUTO, ID_COLOR_PICK, ID_COLOR_SAMPLE, ID_FLYOUT, ID_NATIVE, ID_AWAY = 200, ID_HOVER = 400, ID_ICON = 600 };
static UINT            gTaskbarCreated;
static NOTIFYICONDATAW gNid;
static int      gAway = 40, gHover = 100;     // taskbar opacity in percent: mouse away / mouse over the taskbar
static int      gIconAway = 50;               // icon opacity in percent while the mouse is away (icons are 100% on hover)
static COLORREF gAutoTint = kStartDark;       // the Start menu colour, kept up to date by UpdateTint()
static COLORREF gCustom = RGB(32, 32, 32);    // colour picked from the menu
static bool gUseCustom = false, gBlur = false, gPaused = false, gFlyout = true, gNative = true;
static int  gMix = 255, gApplied = -1;        // fade position: 0 = "mouse away" look, 255 = "mouse on taskbar" look; last values pushed
static ULONGLONG gLastOver = 0;

// ---- Settings (registry) -----------------------------------------------------
static const wchar_t* kAppKey = L"Software\\TaskbarFade";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kThemeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
static const wchar_t* kAccentKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent";

static bool ReadDword(const wchar_t* key, const wchar_t* name, DWORD* out) {
    DWORD n = sizeof * out;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, out, &n) == ERROR_SUCCESS;
}
static DWORD Load(const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = 0;
    return ReadDword(key, name, &v) ? v : def;
}
static void Save(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, name, REG_DWORD, &v, sizeof v);
}
static bool AutoStart() {
    DWORD n = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"TaskbarFade", RRF_RT_REG_SZ, nullptr, nullptr, &n) == ERROR_SUCCESS;
}
static void SetAutoStart(bool on) {
    if (!on) { RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"TaskbarFade"); return; }
    wchar_t path[MAX_PATH + 2] = L"\"";
    GetModuleFileNameW(nullptr, path + 1, MAX_PATH);
    lstrcatW(path, L"\"");
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"TaskbarFade", REG_SZ, path, (DWORD)((lstrlenW(path) + 1) * sizeof(wchar_t)));
}
static void UpdateTint() {                    // work out the Start menu colour
    DWORD v = 0;
    if (Load(kThemeKey, L"SystemUsesLightTheme", 0))              // Light mode (1903+): Windows never colours Start
        gAutoTint = kStartLight;
    else if (Load(kThemeKey, L"ColorPrevalence", 0) == 1 && ReadDword(kAccentKey, L"StartColorMenu", &v))
        gAutoTint = v & 0xFFFFFF;                                 // "Show accent color on Start, taskbar...": stored as 0xAABBGGRR
    else
        gAutoTint = kStartDark;
}
static COLORREF CurrentTint() { return gUseCustom ? gCustom : gAutoTint; }
static int ToAlpha(int percent) { return percent * 255 / 100; }              // 0-100% -> 0-255

// ---- Taskbar handling --------------------------------------------------------
// One record per taskbar window (the main one plus one per extra monitor).
struct Bar {
    HWND wnd;                     // Shell_TrayWnd / Shell_SecondaryTrayWnd
    ACCENT_POLICY orig;           // the accent Windows had set before we touched it
    bool haveOrig;
    bool faded;                   // we gave the taskbar window an alpha (that is how the icons are faded)
    bool layeredByUs;             // ... and we had to add WS_EX_LAYERED to do it
    // The "back window" sits right behind the taskbar and supplies its colour while the icons are faded:
    HWND back;
    bool backBlur;                // true: per-pixel layered window over a blur accent; false: plain constant-alpha fill
    int  bgAlpha;                 // opacity wanted for it (0-255)
    COLORREF paintedTint;         // plain mode: colour it was last painted with
    RECT rect;                    // where it was last placed
    HDC mdc; HBITMAP dib; void* bits; int sw, sh;     // blur mode: the surface pushed to the window ...
    int drawnAlpha; COLORREF drawnTint;               // ... and what that surface currently shows
    bool nativeOn;                // the taskbar currently shows its original look (the "opaque form")
    ULONGLONG nativeAt;           // when we last handed the taskbar back to Explorer
    bool covering;                // the cover is fading out underneath Explorer's own look
};
static Bar gBar[16];
static int gBarN = 0;
static BOOL(WINAPI* pGetWCA)(HWND, WCA_DATA*);   // GetWindowCompositionAttribute (may be missing)

static int Bars(HWND* out, int max) {         // main taskbar + one per extra monitor
    int n = 0;
    if (HWND h = FindWindowW(L"Shell_TrayWnd", nullptr)) out[n++] = h;
    for (HWND h = nullptr; n < max && (h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)); )
        out[n++] = h;
    return n;
}

static void ApplyPolicy(HWND wnd, ACCENT_POLICY p) {
    WCA_DATA d = { 19, &p, sizeof p };                                // 19 = WCA_ACCENT_POLICY
    pSetWCA(wnd, &d);
}

static void SetAccent(HWND wnd, int state, int alpha) {
    // GradientColor is 0xAABBGGRR, the same byte order as a COLORREF.
    ApplyPolicy(wnd, { state, 2, ((unsigned)alpha << 24) | (unsigned)(CurrentTint() & 0xFFFFFF), 0 });
}

static void Capture(Bar& b) {                 // remember how Windows has set this taskbar up, so it can be put back
    ACCENT_POLICY p = {};
    WCA_DATA d = { 19, &p, sizeof p };
    if (pGetWCA && pGetWCA(b.wnd, &d) && p.State >= ACCENT_SOLID && p.State <= 4) {   // state 0 = nothing useful to restore
        b.orig = p;
        b.haveOrig = true;
    }
}

static void RedrawNative(Bar& b) {            // make Explorer re-apply its own taskbar look (this is what TranslucentTB does for "normal")
    SendMessageTimeoutW(b.wnd, WM_DWMCOMPOSITIONCHANGED, 1, 0, SMTO_ABORTIFHUNG, 200, nullptr);
}

static int NativeAlpha() {                    // roughly how opaque Windows' own look is, so handing over to it isn't a jump
    for (int i = 0; i < gBarN; i++)
        if (gBar[i].haveOrig) return gBar[i].orig.State == ACCENT_SOLID ? 255 : (int)((gBar[i].orig.Color >> 24) & 0xFF);
    return 217;                               // typical for the Windows 10 taskbar when its look couldn't be read
}

static void DropBack(Bar& b) {                // destroy the back window and its surface
    if (b.back) { DestroyWindow(b.back); b.back = nullptr; }
    if (b.mdc) { DeleteDC(b.mdc); b.mdc = nullptr; }
    if (b.dib) { DeleteObject(b.dib); b.dib = nullptr; }
    b.bits = nullptr;
    b.sw = b.sh = 0;
    b.drawnAlpha = -1;
    b.covering = false;
}

static void Refresh() {                       // keep gBar in step with the real taskbar windows (monitors come and go, Explorer restarts)
    HWND cur[16];
    int n = Bars(cur, 16);
    for (int i = gBarN - 1; i >= 0; i--) {
        bool alive = false;
        for (int j = 0; j < n; j++) if (cur[j] == gBar[i].wnd) alive = true;
        if (!alive) {
            DropBack(gBar[i]);
            gBar[i] = gBar[--gBarN];
        }
    }
    for (int j = 0; j < n; j++) {
        bool known = false;
        for (int i = 0; i < gBarN; i++) if (gBar[i].wnd == cur[j]) known = true;
        if (!known && gBarN < 16) {
            Bar& b = gBar[gBarN++];
            b = Bar();
            b.wnd = cur[j];
            b.drawnAlpha = -1;
            RedrawNative(b);                  // first sight: have Explorer apply its own look (clears anything left behind by an earlier run) ...
            Capture(b);                       // ... and record it, as a safety net for restoring
        }
    }
}

static void ClearFade(Bar& b) {               // undo SetFade: back to a normal, non-layered taskbar window
    if (!b.faded) return;
    SetLayeredWindowAttributes(b.wnd, 0, 255, LWA_ALPHA);
    if (b.layeredByUs) {
        LONG_PTR ex = GetWindowLongPtrW(b.wnd, GWL_EXSTYLE);
        SetWindowLongPtrW(b.wnd, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_LAYERED);
        RedrawWindow(b.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        b.layeredByUs = false;
    }
    b.faded = false;
}

static void SetFade(Bar& b, int alpha) {      // alpha of the taskbar window itself; only its icons are drawn on it in this mode
    LONG_PTR ex = GetWindowLongPtrW(b.wnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) {
        SetWindowLongPtrW(b.wnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        b.layeredByUs = true;
    }
    SetLayeredWindowAttributes(b.wnd, 0, (BYTE)alpha, LWA_ALPHA);
    b.faded = true;
}

static HWND MakeBack(bool blur) {             // click-through window that supplies the taskbar colour behind the icons
    HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        L"TaskbarFadeBack", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    // Plain mode uses a constant-alpha fill. Blur mode must not: it draws its own per-pixel alpha with
    // UpdateLayeredWindow (see BlurSurface), and the two ways of layering a window can't be mixed.
    if (w && !blur) SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
    return w;
}

static void BlurSurface(Bar& b, int w, int h) {   // taskbar colour at bgAlpha (premultiplied), pushed to the blur-mode back window
    if (w <= 0 || h <= 0) return;
    if (!b.dib || b.sw != w || b.sh != h) {
        if (b.mdc) { DeleteDC(b.mdc); b.mdc = nullptr; }
        if (b.dib) { DeleteObject(b.dib); b.dib = nullptr; }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;                        // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        b.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &b.bits, nullptr, 0);
        b.mdc = CreateCompatibleDC(nullptr);
        if (!b.dib || !b.mdc) return;
        SelectObject(b.mdc, b.dib);
        b.sw = w;
        b.sh = h;
    }
    // Where this surface is see-through (alpha < 255) the blur accent shows, so the result is the blurred
    // desktop mixed with the taskbar colour: 0% is pure blur, 100% is solid colour.
    COLORREF c = CurrentTint();
    unsigned a = (unsigned)b.bgAlpha;
    unsigned px = (a << 24) | ((GetRValue(c) * a / 255) << 16) | ((GetGValue(c) * a / 255) << 8) | (GetBValue(c) * a / 255);
    unsigned* p = (unsigned*)b.bits;
    for (int i = w * h; i > 0; i--) *p++ = px;
    POINT src = { 0, 0 };
    SIZE size = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(b.back, nullptr, nullptr, &size, b.mdc, &src, 0, &bf, ULW_ALPHA);
    b.drawnAlpha = b.bgAlpha;
    b.drawnTint = c;
}

static void SyncBack(Bar& b) {                // keep the back window glued directly behind the taskbar
    if (!b.back) return;
    RECT r = {};
    // (pure blur at 0% still has something to draw; while Explorer's own look is shown the cover rests hidden)
    bool nothingToDraw = (!b.backBlur && b.bgAlpha == 0) || (b.nativeOn && !b.covering);
    if (nothingToDraw || !IsWindowVisible(b.wnd) || !GetWindowRect(b.wnd, &r)) {
        ShowWindow(b.back, SW_HIDE);          // fully clear, or the taskbar is hidden
        return;
    }
    int w = r.right - r.left, h = r.bottom - r.top;
    if (b.backBlur && (b.drawnAlpha != b.bgAlpha || b.drawnTint != CurrentTint() || b.sw != w || b.sh != h))
        BlurSurface(b, w, h);
    if (!IsWindowVisible(b.back) || !EqualRect(&r, &b.rect) || GetWindow(b.wnd, GW_HWNDNEXT) != b.back) {
        SetWindowPos(b.back, b.wnd, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        b.rect = r;
    }
}

static void ApplyBackAlpha(Bar& b) {          // push bgAlpha to the back window
    if (!b.back) return;
    if (!b.backBlur) SetLayeredWindowAttributes(b.back, 0, (BYTE)b.bgAlpha, LWA_ALPHA);
    SyncBack(b);                              // (blur mode redraws its surface here; also places, shows or hides the window)
}

static void StepCover(Bar& b) {               // every tick: fade the cover out underneath Explorer's own look
    if (!b.covering || !b.back) { b.covering = false; return; }
    b.bgAlpha -= 40;                          // about 6 ticks, ~150 ms
    if (b.bgAlpha <= 0) { b.bgAlpha = 0; b.covering = false; }
    ApplyBackAlpha(b);                        // (hides the cover again once it reaches 0)
}

static void PaintBar(Bar& b, int bg, int icon, bool native) {   // bg = taskbar opacity, icon = icon opacity (both 0-255)
    if (native) {                             // the opaque form: hand the taskbar back to Explorer so it draws its own look
        if (!b.nativeOn) {
            ULONGLONG now = GetTickCount64();
            if (now - b.nativeAt >= 500) {    // Explorer doesn't like being poked too often, so don't flap
                // Explorer's look goes on FIRST, while our cover is still underneath, so there is never a see-through gap.
                RedrawNative(b);
                b.nativeOn = true;
                b.nativeAt = now;
                // Icons fully opaque. The layered style stays on: taking it off repaints the whole taskbar and flashes.
                if (b.faded) SetLayeredWindowAttributes(b.wnd, 0, 255, LWA_ALPHA);
                if (b.back) {                 // then fade the cover out, starting where "Explorer's look over the cover" is no darker than what was showing
                    int an = NativeAlpha();
                    b.bgAlpha = (an < 255 && bg > an) ? 255 - (255 - bg) * 255 / (255 - an) : 0;
                    b.covering = b.bgAlpha > 0;
                    ApplyBackAlpha(b);
                }
            }
        }
        if (b.nativeOn) return;               // Explorer owns the look while this is on
    }
    else if (b.nativeOn) {
        Capture(b);                           // leaving it: note the look Explorer has by now
        b.nativeOn = false;
        b.covering = false;
    }
    if (gIconAway >= 100) {                   // icons aren't faded: the taskbar's own accent does it all (blur + colour included)
        SetAccent(b.wnd, bg >= 255 ? ACCENT_SOLID : (gBlur ? ACCENT_BLUR : ACCENT_CLEAR), bg);
        DropBack(b);
        ClearFade(b);
        return;
    }
    // Icons are faded: the taskbar window keeps only its icons (no background of its own) and is faded as a
    // whole, while a separate window right behind it paints the colour. That makes the two opacities independent.
    if (b.back && b.backBlur != gBlur) DropBack(b);                // blur needs another kind of window, so rebuild it when toggled
    if (!b.back) {
        b.back = MakeBack(gBlur);
        b.backBlur = gBlur;
        b.paintedTint = CurrentTint();
        b.rect = RECT();
        b.drawnAlpha = -1;
        if (b.back && gBlur) SetAccent(b.back, ACCENT_BLUR, 0);   // blur only: the colour is mixed in by the surface
    }
    if (!b.back) {                            // couldn't create it: fall back to the taskbar's own tint
        SetAccent(b.wnd, ACCENT_CLEAR, bg);
        SetFade(b, icon);
        return;
    }
    b.bgAlpha = bg;
    if (!b.backBlur && b.paintedTint != CurrentTint()) { b.paintedTint = CurrentTint(); InvalidateRect(b.back, nullptr, TRUE); }
    ApplyBackAlpha(b);                        // the cover goes up first ...
    SetAccent(b.wnd, ACCENT_CLEAR, 0);        // ... and only then does the taskbar lose its own background, so there is no gap
    SetFade(b, icon);
}

static void RestoreBar(Bar& b) {              // put one taskbar back the way Windows had it
    DropBack(b);
    ClearFade(b);
    b.nativeOn = false;
    if (b.haveOrig) ApplyPolicy(b.wnd, b.orig);   // safety net first, in case Explorer ignores the message below
    RedrawNative(b);                              // then make Explorer re-apply its own look
}

static void RestoreAll() {                    // used on Exit and when paused
    for (int i = 0; i < gBarN; i++) RestoreBar(gBar[i]);
    gApplied = -1;
}

static bool ShellFlyoutOpen() {               // Start, Search, notifications (Action Center) and similar flyouts take the focus while open
    static HWND lastFg = nullptr;
    static bool lastResult = false;
    HWND fg = GetForegroundWindow();
    if (fg == lastFg) return lastResult;      // only look up the owning program when the foreground window changes
    lastFg = fg;
    lastResult = false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    HANDLE ph = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (ph) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(ph, 0, path, &n)) {
            const wchar_t* name = path;
            for (const wchar_t* c = path; *c; c++) if (*c == L'\\') name = c + 1;
            static const wchar_t* shell[] = { L"StartMenuExperienceHost.exe", L"ShellExperienceHost.exe",
                                              L"SearchUI.exe", L"SearchApp.exe", L"SearchHost.exe" };
            for (const wchar_t* s : shell) if (!lstrcmpiW(name, s)) lastResult = true;
        }
        CloseHandle(ph);
    }
    return lastResult;
}

static void Tick() {
    Refresh();

    POINT pt;
    GetCursorPos(&pt);
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < gBarN; i++) {
        RECT r;
        if (GetWindowRect(gBar[i].wnd, &r) && PtInRect(&r, pt)) gLastOver = now;
    }
    if (gFlyout && ShellFlyoutOpen()) gLastOver = now;           // Start/Search/notifications open: keep the "mouse on taskbar" look

    // gMix animates between the "mouse away" look (0) and the "mouse on taskbar" look (255).
    int target = (now - gLastOver < kLingerMs) ? 255 : 0;
    if (gMix < target)      gMix = (target - gMix <= 64) ? target : gMix + 64;   // fade in quickly
    else if (gMix > target) gMix = (gMix - target <= 16) ? target : gMix - 16;   // fade out gently

    int bgAway = ToAlpha(gAway);
    int bgHover = gNative ? NativeAlpha() : ToAlpha(gHover);     // while Windows' own look is used, approximate it
    if (bgHover < bgAway) bgHover = bgAway;                       // hovering never makes it more see-through
    int iconAway = ToAlpha(gIconAway);                            // icons are fully opaque on hover
    int bg = bgAway + (bgHover - bgAway) * gMix / 255;
    int icon = iconAway + (255 - iconAway) * gMix / 255;

    bool native = gNative && gMix == 255;                         // fully "mouse on taskbar": show the original look
    int key = bg | (icon << 8) | (native ? 1 << 16 : 0);
    for (int i = 0; i < gBarN; i++) StepCover(gBar[i]);           // finish handing over to Explorer's look, smoothly
    static int ticks = 0;
    if (key != gApplied || ++ticks % 4 == 0) {    // also re-assert ~10x/s: Explorer resets the taskbar when Start/Search open
        for (int i = 0; i < gBarN; i++) PaintBar(gBar[i], bg, icon, native);
        gApplied = key;
    }
    else if (gIconAway < 100) {
        for (int i = 0; i < gBarN; i++) SyncBack(gBar[i]);           // keep the back window glued to the taskbar between repaints
    }
}

static LRESULT CALLBACK BackProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {               // plain mode: paint the whole back window in the taskbar colour
        RECT r;
        GetClientRect(h, &r);
        HBRUSH br = CreateSolidBrush(CurrentTint());
        FillRect((HDC)wp, &r, br);
        DeleteObject(br);
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- Tray icon + window ------------------------------------------------------
static void PickColor(HWND owner) {
    static COLORREF custom[16];
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = owner;
    cc.rgbResult = gCustom;
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    SetForegroundWindow(owner);
    if (ChooseColorW(&cc)) {
        gCustom = cc.rgbResult;
        gUseCustom = true;
        Save(L"CustomColor", gCustom);
        Save(L"UseCustom", 1);
        gApplied = -1;
    }
}

static void SampleColor() {                   // use the colour of the pixel under the mouse, e.g. on the open Start menu
    POINT pt;
    GetCursorPos(&pt);
    HDC dc = GetDC(nullptr);
    COLORREF c = GetPixel(dc, pt.x, pt.y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID) return;
    gCustom = c;
    gUseCustom = true;
    Save(L"CustomColor", gCustom);
    Save(L"UseCustom", 1);
    gApplied = -1;
    MessageBeep(MB_OK);
}

static HMENU PercentMenu(int first, int current, int idBase) {      // first..100% in 10% steps
    HMENU sub = CreatePopupMenu();
    for (int p = first; p <= 100; p += 10) {
        wchar_t text[32];
        wsprintfW(text, L"%d", p);
        lstrcatW(text, p == 0 ? L"% (clear)" : (p == 100 ? L"% (solid)" : L"%"));
        AppendMenuW(sub, MF_STRING | (p == current ? MF_CHECKED : 0), idBase + p, text);
    }
    return sub;
}

static void ShowMenu(HWND hwnd) {
    HMENU color = CreatePopupMenu();
    AppendMenuW(color, MF_STRING | (gUseCustom ? 0 : MF_CHECKED), ID_COLOR_AUTO, L"Match Start menu");
    AppendMenuW(color, MF_STRING | (gUseCustom ? MF_CHECKED : 0), ID_COLOR_PICK, L"Custom color...");
    AppendMenuW(color, MF_STRING, ID_COLOR_SAMPLE, L"Sample color from screen (5 seconds)...");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (gPaused ? 0 : MF_CHECKED), ID_TOGGLE, L"Enabled");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)PercentMenu(0, gAway, ID_AWAY), L"Taskbar opacity when mouse is away");

    AppendMenuW(m, MF_POPUP | (gNative ? MF_GRAYED : 0), (UINT_PTR)PercentMenu(50, gHover, ID_HOVER), L"Taskbar opacity when mouse is over it");   // unused while Windows' own look is shown
    AppendMenuW(m, MF_STRING | (gNative ? MF_CHECKED : 0), ID_NATIVE, L"Opaque look: keep the original taskbar");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)PercentMenu(0, gIconAway, ID_ICON), L"Icon opacity when mouse is away");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)color, L"Taskbar color");
    AppendMenuW(m, MF_STRING | (gBlur ? MF_CHECKED : 0), ID_BLUR, L"Blur mixed with taskbar color");
    AppendMenuW(m, MF_STRING | (gFlyout ? MF_CHECKED : 0), ID_FLYOUT, L"Opaque while Start, Search or notifications are open");
    AppendMenuW(m, MF_STRING | (AutoStart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);                // required so the menu closes when you click elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);                           // also destroys the submenus
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == gTaskbarCreated) {             // Explorer restarted: put the tray icon back
        Shell_NotifyIconW(NIM_ADD, &gNid);
        gApplied = -1;
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == 2) { KillTimer(h, 2); SampleColor(); }          // the "sample color from screen" countdown ended
        else if (!gPaused) Tick();
        return 0;
    case WM_SETTINGCHANGE:                    // user switched Windows between light and dark
        if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) { UpdateTint(); gApplied = -1; }
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) ShowMenu(h);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_TOGGLE) { gPaused = !gPaused; if (gPaused) RestoreAll(); else gApplied = -1; }
        else if (id == ID_BLUR) { gBlur = !gBlur; Save(L"Blur", gBlur); gApplied = -1; }
        else if (id == ID_FLYOUT) { gFlyout = !gFlyout; Save(L"FlyoutOpaque", gFlyout); }
        else if (id == ID_NATIVE) { gNative = !gNative; Save(L"NativeOpaque", gNative); gApplied = -1; }
        else if (id == ID_AUTOSTART)  SetAutoStart(!AutoStart());
        else if (id == ID_COLOR_AUTO) { gUseCustom = false; Save(L"UseCustom", 0); gApplied = -1; }
        else if (id == ID_COLOR_PICK) PickColor(h);
        else if (id == ID_COLOR_SAMPLE) SetTimer(h, 2, 5000, nullptr);
        else if (id == ID_EXIT)       DestroyWindow(h);
        else if (id >= ID_AWAY && id <= ID_AWAY + 100) { gAway = id - ID_AWAY;   Save(L"AwayPercent", gAway); }
        else if (id >= ID_HOVER && id <= ID_HOVER + 100) { gHover = id - ID_HOVER; Save(L"HoverPercent", gHover); }
        else if (id >= ID_ICON && id <= ID_ICON + 100) { gIconAway = id - ID_ICON; Save(L"IconAwayPercent", gIconAway); gApplied = -1; }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(h, 1);
        RestoreAll();
        Shell_NotifyIconW(NIM_DELETE, &gNid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int) {
    CreateMutexW(nullptr, TRUE, L"TaskbarFade.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;             // already running

    HMODULE user32 = GetModuleHandleW(L"user32");

    // DPI awareness, so the cursor and taskbar rectangles use the same coordinates.
    // Per-monitor v2 needs Windows 10 1703+, per-monitor v1 needs 1607+, and 1507/1511 only have the old call.
    auto setDpiCtx = (BOOL(WINAPI*)(HANDLE))GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (!(setDpiCtx && (setDpiCtx((HANDLE)(INT_PTR)-4) || setDpiCtx((HANDLE)(INT_PTR)-3))))
        SetProcessDPIAware();

    pSetWCA = (decltype(pSetWCA))GetProcAddress(user32, "SetWindowCompositionAttribute");
    pGetWCA = (decltype(pGetWCA))GetProcAddress(user32, "GetWindowCompositionAttribute");
    if (!pSetWCA) {
        MessageBoxW(nullptr, L"SetWindowCompositionAttribute isn't available on this version of Windows.", L"TaskbarFade", MB_ICONERROR);
        return 1;
    }

    gAway = (int)Load(kAppKey, L"AwayPercent", 40);
    gHover = (int)Load(kAppKey, L"HoverPercent", 100);
    gIconAway = (int)Load(kAppKey, L"IconAwayPercent", 50);
    if (gAway > 100)     gAway = 100;
    if (gHover > 100)    gHover = 100;
    if (gIconAway > 100) gIconAway = 100;
    gBlur = Load(kAppKey, L"Blur", 0) != 0;
    gFlyout = Load(kAppKey, L"FlyoutOpaque", 1) != 0;
    gNative = Load(kAppKey, L"NativeOpaque", 1) != 0;
    gCustom = Load(kAppKey, L"CustomColor", gCustom);
    gUseCustom = Load(kAppKey, L"UseCustom", 0) != 0;
    UpdateTint();
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"TaskbarFadeWnd";
    RegisterClassW(&wc);
    WNDCLASSW bc = {};
    bc.lpfnWndProc = BackProc;
    bc.hInstance = hi;
    bc.lpszClassName = L"TaskbarFadeBack";
    RegisterClassW(&bc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"TaskbarFade", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);

    gNid.cbSize = sizeof gNid;
    gNid.hWnd = hwnd;
    gNid.uID = 1;
    gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gNid.uCallbackMessage = WM_TRAY;
    gNid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(gNid.szTip, L"TaskbarFade");
    Shell_NotifyIconW(NIM_ADD, &gNid);

    SetTimer(hwnd, 1, 25, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// A Visual Studio "Console App" project links against main() instead of WinMain()
// (error LNK2001: unresolved external symbol main), so forward to WinMain.
int main() {
    FreeConsole();                            // drops the console window if this was built as a console app
    return WinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineA(), SW_SHOWDEFAULT);
}

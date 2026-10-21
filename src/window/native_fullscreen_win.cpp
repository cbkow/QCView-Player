#include "native_fullscreen_win.h"

#ifdef Q_OS_WIN

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QOperatingSystemVersion>
#include <QSet>
#include <QWindow>
#include <QtLogging>

namespace qcv {

namespace {

struct SavedState {
    WINDOWPLACEMENT placement{};
    LONG_PTR        style    = 0;
    LONG_PTR        exStyle  = 0;
};

// Module-local map of HWND → saved state. WindowManager only ever has
// one UI window in our app, but this keeps the helper general.
QHash<HWND, SavedState> &savedStates()
{
    static QHash<HWND, SavedState> s;
    return s;
}

HWND hwndOf(QWindow *window)
{
    if (!window) return nullptr;
    return reinterpret_cast<HWND>(window->winId());
}

} // namespace

namespace {
// Pre-Compact window flags, keyed by HWND, while Compact Mode holds.
QHash<HWND, Qt::WindowFlags> &compactSavedFlags()
{
    static QHash<HWND, Qt::WindowFlags> s;
    return s;
}

// Compact Mode's window flags (2026-10-21, after the same finding in
// minNotes). Qt 6.9+ owns the chromeless frame through
// ExpandedClientAreaHint: its WM_NCCALCSIZE handler hands the whole top
// edge to the client — a caption-less WS_THICKFRAME window otherwise
// keeps Windows' PAINTED 8 px top border above the content, and
// stripping WS_CAPTION with SetWindowLongPtr (the previous recipe) left
// Qt's cached frame margins stale — calls DwmExtendFrameIntoClientArea
// with -1 margins so the DWM shadow and the Windows 11 rounded corners
// stay, and keeps the invisible left / right / bottom borders for edge
// resize and snap. CustomizeWindowHint WITHOUT the title and button
// hints: Qt draws neither caption buttons nor a title in its layered
// title-bar child (the CompactStrip has its own exit; the viewport sits
// where the buttons would be) and the style carries no WS_CAPTION /
// WS_MINMAXBOX, as before. NoTitleBarBackgroundHint keeps that child
// transparent. The flags go through QWindow::setFlags so Qt applies the
// style on the EXISTING HWND (no recreation: the D3D11 child keeps its
// parent) and recomputes its margins from the WM_NCCALCSIZE that
// SWP_FRAMECHANGED triggers.
const Qt::WindowFlags kCompactFlags = Qt::Window | Qt::CustomizeWindowHint
    | Qt::WindowSystemMenuHint | Qt::ExpandedClientAreaHint
    | Qt::NoTitleBarBackgroundHint;

// Windows 10: the whole window rect is client (2026-10-21, 2.5.8 field
// report). Qt's WM_NCCALCSIZE for the expanded client area keeps the
// resize-border band (SM_CXSIZEFRAME + SM_CXPADDEDBORDER, 8 px at 96
// dpi) on the left, right and bottom as NON-client. Windows 11 draws
// that band invisible; Windows 10's DWM paints it in the frame colour
// (white in light mode, the accent when "show accent colour on title
// bars and window borders" is on), so Compact Mode showed a white
// border on three sides and none on top. Here, on Windows 10 only, an
// application-level native event filter answers WM_NCCALCSIZE for the
// Compact window first (QWindowsContext::windowsProc runs the filters
// before its own handler) and leaves rgrc[0] untouched: client = window
// rect, nothing left for DWM to paint. Edge resize keeps working — Qt's
// WM_NCHITTEST for the hint measures the band from the client geometry,
// which now reaches the window edge — and the thick frame still gives
// snap and the shadow. Maximized, Windows pushes the frame past the
// monitor edge, so the band is cut on all four sides, as Qt does.
// QCV_COMPACT_FULLCLIENT=1 forces the path on Windows 11 (mechanics can
// be exercised there), =0 keeps Qt's margins on Windows 10.
// Every Windows version since the Windows 11 light-mode measurement
// below (the band shows there too: 2 px of it lie inside the visible
// bounds). QCV_COMPACT_FULLCLIENT=0 goes back to Qt's margins.
bool compactFullClient()
{
    static const bool v = [] {
        if (qEnvironmentVariableIsSet("QCV_COMPACT_FULLCLIENT"))
            return qEnvironmentVariableIntValue("QCV_COMPACT_FULLCLIENT") != 0;
        return true;
    }();
    return v;
}

class CompactFullClientFilter : public QAbstractNativeEventFilter {
public:
    QSet<HWND> hwnds;
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *result) override
    {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message != WM_NCCALCSIZE || !msg->wParam) return false;
        if (!hwnds.contains(msg->hwnd)) return false;
        if (IsZoomed(msg->hwnd)) {
            const UINT dpi = GetDpiForWindow(msg->hwnd);
            const int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                             + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            auto *ncp = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
            ncp->rgrc[0].left   += border;
            ncp->rgrc[0].top    += border;
            ncp->rgrc[0].right  -= border;
            ncp->rgrc[0].bottom -= border;
        }
        *result = 0;
        return true;
    }
};

CompactFullClientFilter &fullClientFilter()
{
    static CompactFullClientFilter *f = [] {
        auto *p = new CompactFullClientFilter;
        QCoreApplication::instance()->installNativeEventFilter(p);
        return p;
    }();
    return *f;
}

// Windows 11 draws a 1 px border line around every top-level window,
// dark when the OS colour scheme is dark (Qt asks for that through
// DWMWA_USE_IMMERSIVE_DARK_MODE) and light otherwise, which is a pale
// outline around the Compact window on a light-mode desktop (chris,
// 2026-10-21). DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE removes it while
// Compact holds; DWMWA_COLOR_DEFAULT gives the system line back. The
// attribute exists from Windows 11 (build 22000) and fails harmlessly
// on Windows 10, whose frame is handled by the full-client path.
// Measured on Windows 11 in light mode (2026-10-21), with Qt's margins:
// the left, right and bottom showed 2 px of the non-client resize band
// inside the visible bounds, painted by DWM in the frame colour (white
// in light mode, near-black in dark mode, hence unseen before); with
// the border line removed one of those pixels stayed white even after
// DWMWA_CAPTION_COLOR painted the frame dark. Only the full-client path
// is clean (content to the window edge). Windows 11 rounds the corners
// of a frameless-looking window only when asked, so the corner
// preference goes with it; the border line is removed; everything is
// put back to the system defaults on exit. The attributes are Windows
// 11 (22000+) and fail harmlessly on Windows 10.
void setCompactBorderLine(HWND hwnd, bool on)
{
    // Literal values: the SDK names (DWMWA_BORDER_COLOR enumerator,
    // DWMWA_COLOR_NONE / _DEFAULT macros) are only in 10.0.22000+ headers.
    constexpr DWORD kCornerPrefAttr   = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr DWORD kBorderColorAttr  = 34;   // DWMWA_BORDER_COLOR
    constexpr DWORD kCaptionColorAttr = 35;   // DWMWA_CAPTION_COLOR
    constexpr COLORREF kColorDefault = 0xFFFFFFFF;
    constexpr COLORREF kColorNone    = 0xFFFFFFFE;
    constexpr COLORREF kWindowBg     = RGB(0x10, 0x10, 0x10);   // Main.qml color
    constexpr DWORD kCornerDefault = 0;       // DWMWCP_DEFAULT
    constexpr DWORD kCornerRound   = 2;       // DWMWCP_ROUND
    const COLORREF border  = on ? kColorNone  : kColorDefault;
    const COLORREF caption = on ? kWindowBg   : kColorDefault;
    const DWORD    corner  = on ? kCornerRound : kCornerDefault;
    DwmSetWindowAttribute(hwnd, kBorderColorAttr,  &border,  sizeof(border));
    DwmSetWindowAttribute(hwnd, kCaptionColorAttr, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, kCornerPrefAttr,   &corner,  sizeof(corner));
}

bool applyCompactFlags(QWindow *window, HWND hwnd, bool on)
{
    auto &map = compactSavedFlags();
    if (on) {
        if (map.contains(hwnd)) return true;
        map.insert(hwnd, window->flags());
        // Registered BEFORE setFlags: the SWP_FRAMECHANGED inside it is
        // what sends the WM_NCCALCSIZE the filter must answer.
        if (compactFullClient()) fullClientFilter().hwnds.insert(hwnd);
        window->setFlags(kCompactFlags);
        setCompactBorderLine(hwnd, true);
    } else {
        auto it = map.find(hwnd);
        if (it == map.end()) return true;
        const Qt::WindowFlags saved = it.value();
        map.erase(it);
        if (compactFullClient()) fullClientFilter().hwnds.remove(hwnd);
        window->setFlags(saved);
        setCompactBorderLine(hwnd, false);
    }
    // The hint changes QWindow::safeAreaMargins() (0 <-> the title-bar
    // height) but the Windows plugin posts no SafeAreaMarginsChange, so
    // the QML SafeArea would hold the old value until the next geometry
    // change: the in-window MenuBar, which insets by it, came back 31 px
    // taller for a frame after leaving Compact. Tell the Quick window now.
    QEvent safeAreaChange(QEvent::SafeAreaMarginsChange);
    QCoreApplication::sendEvent(window, &safeAreaChange);
    qInfo("setCompactBorderlessWin: %p %s (flags 0x%x, full client %d)", hwnd,
          on ? "expanded client area" : "restored", unsigned(window->flags()),
          int(on && compactFullClient()));
    return true;
}
} // namespace

bool enterBorderlessFullscreenWin(QWindow *window)
{
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    if (savedStates().contains(hwnd)) return true;   // already in FS

    // Ask Win32 for the monitor this window currently sits on, in
    // PHYSICAL pixels. We deliberately avoid QScreen::geometry() * dpr:
    // that assumes one global scale factor, which breaks on mixed-DPI
    // multi-monitor layouts (Qt's logical virtual-desktop layout is not
    // a uniform scaling of the physical layout under Per-Monitor-V2).
    // GetMonitorInfo and SetWindowPos both speak the same physical
    // virtual-desktop space, so rcMonitor is exactly what we want — the
    // FULL monitor rect (includes the taskbar area), on every monitor
    // and DPI combination.
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) {
        qWarning("enterBorderlessFullscreenWin: GetMonitorInfo failed (GLE=%lu)",
                 GetLastError());
        return false;
    }
    const RECT &r = mi.rcMonitor;
    const int sx = r.left;
    const int sy = r.top;
    const int sw = r.right  - r.left;
    const int sh = r.bottom - r.top;

    // Compact's expanded client area goes FIRST: while the hint is set
    // Qt keeps answering WM_NCCALCSIZE with the three invisible borders
    // subtracted, which would inset the fullscreen client by 8 px on the
    // left, right and bottom. The style and placement saved below then
    // describe the framed window; exitFullscreen() in Main.qml re-applies
    // Compact after the restore.
    applyCompactFlags(window, hwnd, false);

    SavedState s{};
    s.placement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &s.placement)) {
        qWarning("enterBorderlessFullscreenWin: GetWindowPlacement failed (GLE=%lu)",
                 GetLastError());
        return false;
    }
    s.style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    s.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    savedStates().insert(hwnd, s);

    // Strip the native chrome (caption, thick resize border, system
    // menu, min/max). Keep WS_VISIBLE + WS_CLIPSIBLINGS + WS_CLIPCHILDREN
    // so child HWNDs (our D3D11 surface) still render. We do NOT
    // touch the window class or recreate the HWND — Qt keeps its
    // handle, and our child HWND keeps its parent.
    const LONG_PTR stripStyle = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU
                              | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER
                              | WS_DLGFRAME;
    SetWindowLongPtrW(hwnd, GWL_STYLE, (s.style & ~stripStyle) | WS_POPUP);
    // Add WS_EX_TOPMOST so the window draws above the taskbar
    // (Windows' taskbar is a system topmost window — only another
    // topmost window can sit visually above it). Strip
    // WS_EX_WINDOWEDGE / WS_EX_CLIENTEDGE to kill the residual frame
    // bevel some themes still draw.
    const LONG_PTR stripExStyle = WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE
                                | WS_EX_STATICEDGE | WS_EX_DLGMODALFRAME;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      (s.exStyle & ~stripExStyle) | WS_EX_TOPMOST);
    SetWindowPos(hwnd, HWND_TOPMOST,
                  sx, sy, sw, sh,
                  SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    qInfo("enterBorderlessFullscreenWin: %p → topmost screen rect (%d,%d %dx%d)",
          hwnd, sx, sy, sw, sh);
    return true;
}

bool exitBorderlessFullscreenWin(QWindow *window)
{
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    auto it = savedStates().find(hwnd);
    if (it == savedStates().end()) return false;
    const SavedState s = it.value();
    savedStates().erase(it);

    SetWindowLongPtrW(hwnd, GWL_STYLE,   s.style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, s.exStyle);
    SetWindowPos(hwnd, HWND_NOTOPMOST,
                  0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // Restore the prior placement (position + size + show state) in
    // one go — WINDOWPLACEMENT captures the windowed coords even if
    // the window had been maximized at save time.
    SetWindowPlacement(hwnd, &s.placement);
    qInfo("exitBorderlessFullscreenWin: %p restored", hwnd);
    return true;
}

bool setCompactBorderlessWin(QWindow *window, bool on)
{
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    if (savedStates().contains(hwnd)) return false;   // in fullscreen: later
    return applyCompactFlags(window, hwnd, on);
}

bool isBorderlessFullscreenWin(QWindow *window)
{
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    return savedStates().contains(hwnd);
}

} // namespace qcv

#endif // Q_OS_WIN

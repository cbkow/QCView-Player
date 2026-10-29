// Native borderless fullscreen for Windows — bypasses Qt's
// setVisibility(ApplicationWindow.FullScreen), which on Windows
// transitions the HWND into a fullscreen mode that disrupts Qt's
// internal HWND attributes and breaks the child HWND parent
// relationship we rely on for the D3D11 player surface (the
// "viewport detaches off-screen" symptom).
//
// Instead we keep the window in its current Qt visibility mode
// (Windowed) and:
//   1. Save the pre-FS WINDOWPLACEMENT + ex-style.
//   2. Set WS_EX_TOPMOST so the window covers the taskbar.
//   3. Resize to screen.geometry() (full virtual-screen, not just
//      availableGeometry which excludes the taskbar).
//
// Window stays in Qt::Windowed visibility throughout. Qt has nothing
// to transition. The child HWND's parent doesn't move. Panels hide
// via the QML `inFullscreen` binding driven by the `borderlessFs`
// flag the QML toggles when this helper returns true.
//
// Restoring on exit puts the window back exactly where it was —
// position, size, ex-style.

#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN
class QWindow;

namespace qcv {

bool enterBorderlessFullscreenWin(QWindow *window);
bool exitBorderlessFullscreenWin(QWindow *window);
bool isBorderlessFullscreenWin(QWindow *window);

// Compact Mode's window: Qt's expanded client area at the current
// frame (ExpandedClientAreaHint + NoTitleBarBackgroundHint, no caption
// buttons) — no title bar, no painted top border, the DWM shadow and
// corners, edge resize and snap through the invisible borders — or the
// framed flags back. Refused (false) while the window is in borderless
// fullscreen; a no-op (true) when already as asked.
bool setCompactBorderlessWin(QWindow *window, bool on);

// Always on Top (the pin): SetWindowPos HWND_TOPMOST / HWND_NOTOPMOST and
// nothing else — no move, no size, no activation, no SWP_FRAMECHANGED
// (Compact Mode's expanded client area must not be re-measured).
void setAlwaysOnTopWin(QWindow *window, bool on);

// A popup / tooltip window about to be shown goes to the top of the
// topmost band. Qt makes every Qt::Popup and Qt::ToolTip window
// WS_EX_TOPMOST once, when it is created, and re-shows it with
// ShowWindow alone; a pinned (or borderless-fullscreen) main window
// that was raised within that band after the popup's creation — the
// pin's re-assert after a Compact toggle, or just the user activating
// the window again — then covers every menu, context menu and tooltip
// opened before it (measured 2026-10-29). Called from the application
// event filter on QEvent::Show.
void raisePopupWindowWin(QWindow *popup);

} // namespace qcv

#endif // Q_OS_WIN

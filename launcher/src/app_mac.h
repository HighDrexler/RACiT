// macOS only, for app.cpp: the few Win32 calls the launcher's screens and window procedure
// use, done with SDL2. app.cpp keeps one window procedure for both systems; on a Mac its
// main loop (the end of app.cpp) turns SDL's events into the same messages -- WM_KEYDOWN,
// WM_CHAR, the mouse buttons and wheel, WM_SIZE, WM_TIMER, WM_CLOSE -- and posted messages
// (a finished job, the game's exit) arrive through the queue below.
#pragma once
#ifndef __APPLE__
#error "app_mac.h is the macOS stand-in for <windows.h>"
#endif
#include <SDL2/SDL.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

#include "util.h"

typedef void* HWND;                 // the SDL_Window
typedef void* HANDLE;               // the game: a ChildProcess
typedef void* HINSTANCE;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef unsigned UINT;
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef long LONG;
typedef int BOOL;
typedef unsigned long long ULONGLONG;
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define INFINITE 0xFFFFFFFFu

enum : UINT {
    WM_DESTROY = 0x0002, WM_SIZE = 0x0005, WM_PAINT = 0x000F, WM_CLOSE = 0x0010, WM_ERASEBKGND = 0x0014,
    WM_GETMINMAXINFO = 0x0024, WM_KEYDOWN = 0x0100, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104, WM_TIMER = 0x0113,
    WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
    WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020A, WM_XBUTTONDOWN = 0x020B, WM_XBUTTONUP = 0x020C,
    WM_DPICHANGED = 0x02E0, WM_APP = 0x8000,
};
enum : int {
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
    VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
};
enum { XBUTTON1 = 1, XBUTTON2 = 2 };
enum { SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_MINIMIZE = 6, SW_RESTORE = 9 };
enum { SWP_NOZORDER = 4, SWP_NOACTIVATE = 16, CF_UNICODETEXT = 13, COINIT_APARTMENTTHREADED = 2 };

#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xFFFF))
#define GET_X_LPARAM(l) ((int)(short)LOWORD(l))
#define GET_Y_LPARAM(l) ((int)(short)HIWORD(l))
#define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#define MAKEWPARAM(a, b) ((WPARAM)(((uint32_t)(WORD)(a)) | ((uint32_t)(WORD)(b)) << 16))
#define MAKELPARAM(a, b) ((LPARAM)(((uint32_t)(WORD)(a)) | ((uint32_t)(WORD)(b)) << 16))

struct POINT { LONG x, y; };
struct RECT { LONG left, top, right, bottom; };
struct MINMAXINFO { POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize; };
struct PAINTSTRUCT { int unused; };
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };

namespace mac {
struct Msg { UINT m; WPARAM w; LPARAM l; };
inline std::mutex queue_lock;
inline std::deque<Msg> queue;
inline Uint32 wake_event = 0;       // SDL_RegisterEvents: wakes the main loop for a posted message
inline bool dirty = true;           // InvalidateRect: paint on the next turn of the loop
inline bool quit = false;           // PostQuitMessage
inline std::wstring clipboard;      // GetClipboardData's text, while it is read
inline SDL_Window* win(HWND h) { return (SDL_Window*)h; }
}

inline ULONGLONG GetTickCount64()
{
    return (ULONGLONG)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void GetLocalTime(SYSTEMTIME* st)
{
    const auto now = std::chrono::system_clock::now();
    const time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm lt; localtime_r(&t, &lt);
    st->wYear = (WORD)(lt.tm_year + 1900); st->wMonth = (WORD)(lt.tm_mon + 1); st->wDayOfWeek = (WORD)lt.tm_wday; st->wDay = (WORD)lt.tm_mday;
    st->wHour = (WORD)lt.tm_hour; st->wMinute = (WORD)lt.tm_min; st->wSecond = (WORD)lt.tm_sec;
    st->wMilliseconds = (WORD)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
}

inline BOOL PostMessageW(HWND, UINT m, WPARAM w, LPARAM l)
{
    { std::lock_guard<std::mutex> g(mac::queue_lock); mac::queue.push_back({ m, w, l }); }
    if (mac::wake_event) { SDL_Event e{}; e.type = mac::wake_event; SDL_PushEvent(&e); }
    return TRUE;
}
inline BOOL InvalidateRect(HWND, const void*, BOOL) { mac::dirty = true; return TRUE; }
inline void BeginPaint(HWND, PAINTSTRUCT*) {}
inline void EndPaint(HWND, PAINTSTRUCT*) {}
inline LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
inline BOOL DestroyWindow(HWND h) { return PostMessageW(h, WM_DESTROY, 0, 0); }
inline void PostQuitMessage(int) { mac::quit = true; }
inline BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }   // SDL sizes the window itself

inline BOOL ShowWindow(HWND h, int how)
{
    if (!h) return FALSE;
    if (how == SW_HIDE) SDL_HideWindow(mac::win(h));
    else if (how == SW_MINIMIZE) SDL_MinimizeWindow(mac::win(h));
    else { SDL_ShowWindow(mac::win(h)); SDL_RestoreWindow(mac::win(h)); }
    return TRUE;
}
inline BOOL SetForegroundWindow(HWND h) { if (h) SDL_RaiseWindow(mac::win(h)); return TRUE; }
inline HWND g_mac_window_for_focus = nullptr;
inline HWND GetForegroundWindow()
{
    HWND h = g_mac_window_for_focus;
    return h && (SDL_GetWindowFlags(mac::win(h)) & SDL_WINDOW_INPUT_FOCUS) ? h : nullptr;
}
inline BOOL IsIconic(HWND h) { return h && (SDL_GetWindowFlags(mac::win(h)) & SDL_WINDOW_MINIMIZED) ? TRUE : FALSE; }
inline BOOL IsWindowVisible(HWND h) { return h && !(SDL_GetWindowFlags(mac::win(h)) & SDL_WINDOW_HIDDEN) ? TRUE : FALSE; }

// The clipboard, for Paste in a text field.
inline BOOL OpenClipboard(HWND)
{
    char* t = SDL_GetClipboardText();
    mac::clipboard = t ? widen(t) : std::wstring();
    if (t) SDL_free(t);
    return TRUE;
}
inline HANDLE GetClipboardData(UINT) { return mac::clipboard.empty() ? nullptr : (HANDLE)mac::clipboard.c_str(); }
inline void* GlobalLock(HANDLE h) { return h; }
inline BOOL GlobalUnlock(HANDLE) { return TRUE; }
inline BOOL CloseClipboard() { mac::clipboard.clear(); return TRUE; }

// The game's process (Store::launch hands back a ChildProcess).
inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return child_wait((ChildProcess*)h, ms == INFINITE ? -1 : (int)ms) ? 0 : 258; }
inline BOOL GetExitCodeProcess(HANDLE h, DWORD* code) { *code = (DWORD)child_exit_code((ChildProcess*)h); return TRUE; }
inline BOOL TerminateProcess(HANDLE h, UINT code) { child_terminate((ChildProcess*)h, (int)code); return TRUE; }
inline BOOL CloseHandle(HANDLE h) { child_free((ChildProcess*)h); return TRUE; }

inline long CoInitializeEx(void*, int) { return 0; }
inline void CoUninitialize() {}

/*
 * Windows backend: low-level keyboard/mouse hooks (WH_KEYBOARD_LL,
 * WH_MOUSE_LL) on a dedicated thread with its own message loop.
 *
 * OBS is per-monitor DPI aware, so hook coordinates, GetCursorPos and
 * monitor rectangles are all in physical pixels, matching Display Capture.
 */
#include "input-hook.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>

namespace ts::platform {

namespace {

std::thread g_thread;
std::atomic<DWORD> g_threadId{0};
HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;
std::bitset<256> g_down; /* only touched on the hook thread */

/* The synthetic LCtrl that Windows sends before RAlt on AltGr layouts. */
constexpr DWORD kAltGrFakeCtrlScanCode = 0x21D;

bool pressed(int vk)
{
	return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

uint32_t currentMods()
{
	uint32_t mods = 0;
	const bool altGr = pressed(VK_RMENU);

	if (pressed(VK_LSHIFT) || pressed(VK_RSHIFT))
		mods |= TS_MOD_SHIFT;
	/* With AltGr held, LCtrl is (almost always) the fake one. */
	if (pressed(VK_RCONTROL) || (!altGr && pressed(VK_LCONTROL)))
		mods |= TS_MOD_CTRL;
	if (pressed(VK_LMENU))
		mods |= TS_MOD_ALT;
	if (altGr)
		mods |= TS_MOD_ALTGR;
	if (pressed(VK_LWIN) || pressed(VK_RWIN))
		mods |= TS_MOD_META;
	return mods;
}

void utf16ToUtf8(const wchar_t *src, char *dst, size_t size)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)size, nullptr, nullptr);
	if (n <= 0)
		dst[0] = 0;
}

void setName(ts_key_event &ev, const char *name)
{
	snprintf(ev.name, sizeof(ev.name), "%s", name);
}

void classify(DWORD vk, ts_key_event &ev)
{
	ev.kind = TS_KEY_IGNORED;
	ev.name[0] = 0;

	if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
		ev.kind = TS_KEY_PRINTABLE;
		ev.name[0] = (char)vk;
		ev.name[1] = 0;
		return;
	}
	if (vk >= VK_F1 && vk <= VK_F24) {
		ev.kind = TS_KEY_FUNCTION;
		snprintf(ev.name, sizeof(ev.name), "F%lu", (unsigned long)(vk - VK_F1 + 1));
		return;
	}
	if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
		ev.kind = TS_KEY_PRINTABLE;
		snprintf(ev.name, sizeof(ev.name), "Num %lu", (unsigned long)(vk - VK_NUMPAD0));
		return;
	}

	switch (vk) {
	case VK_SHIFT:
	case VK_LSHIFT:
	case VK_RSHIFT:
	case VK_CONTROL:
	case VK_LCONTROL:
	case VK_RCONTROL:
	case VK_MENU:
	case VK_LMENU:
	case VK_RMENU:
	case VK_LWIN:
	case VK_RWIN:
		ev.kind = TS_KEY_MODIFIER;
		setName(ev, "Modifier");
		return;
	case VK_SPACE:
		ev.kind = TS_KEY_PRINTABLE;
		setName(ev, "Space");
		return;
	case VK_MULTIPLY:
		ev.kind = TS_KEY_PRINTABLE;
		setName(ev, "Num *");
		return;
	case VK_ADD:
		ev.kind = TS_KEY_PRINTABLE;
		setName(ev, "Num +");
		return;
	case VK_SUBTRACT:
		ev.kind = TS_KEY_PRINTABLE;
		setName(ev, "Num -");
		return;
	case VK_DECIMAL:
		ev.kind = TS_KEY_PRINTABLE;
		setName(ev, "Num .");
		return;
	case VK_DIVIDE:
		ev.kind = TS_KEY_PRINTABLE;
		setName(ev, "Num /");
		return;
	case VK_ESCAPE:
		ev.kind = TS_KEY_ESCAPE;
		setName(ev, "Esc");
		return;
	case VK_RETURN:
		ev.kind = TS_KEY_EDITING;
		setName(ev, "Enter");
		return;
	case VK_TAB:
		ev.kind = TS_KEY_EDITING;
		setName(ev, "Tab");
		return;
	case VK_BACK:
		ev.kind = TS_KEY_EDITING;
		setName(ev, "Backspace");
		return;
	case VK_DELETE:
		ev.kind = TS_KEY_EDITING;
		setName(ev, "Delete");
		return;
	case VK_INSERT:
		ev.kind = TS_KEY_EDITING;
		setName(ev, "Insert");
		return;
	case VK_LEFT:
		ev.kind = TS_KEY_NAVIGATION;
		setName(ev, "Left");
		return;
	case VK_RIGHT:
		ev.kind = TS_KEY_NAVIGATION;
		setName(ev, "Right");
		return;
	case VK_UP:
		ev.kind = TS_KEY_NAVIGATION;
		setName(ev, "Up");
		return;
	case VK_DOWN:
		ev.kind = TS_KEY_NAVIGATION;
		setName(ev, "Down");
		return;
	case VK_HOME:
		ev.kind = TS_KEY_NAVIGATION;
		setName(ev, "Home");
		return;
	case VK_END:
		ev.kind = TS_KEY_NAVIGATION;
		setName(ev, "End");
		return;
	case VK_PRIOR:
		ev.kind = TS_KEY_NAVIGATION;
		setName(ev, "PageUp");
		return;
	case VK_NEXT:
		ev.kind = TS_KEY_NAVIGATION;
		setName(ev, "PageDown");
		return;
	case VK_SNAPSHOT:
		ev.kind = TS_KEY_FUNCTION;
		setName(ev, "PrtSc");
		return;
	case VK_PAUSE:
		ev.kind = TS_KEY_FUNCTION;
		setName(ev, "Pause");
		return;
	default:
		break;
	}

	/* Punctuation (VK_OEM_*): ask the active layout which character the
	 * key produces so "Ctrl + ñ" or "Ctrl + ´" show up correctly. */
	UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF; /* high bit = dead key */
	if (ch >= 0x20) {
		wchar_t wide[2] = {(wchar_t)ch, 0};
		CharUpperW(wide);
		ev.kind = TS_KEY_PRINTABLE;
		utf16ToUtf8(wide, ev.name, sizeof(ev.name));
	}
}

LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION) {
		const auto *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
		const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
		const DWORD vk = k->vkCode & 0xFF;

		if (!(vk == VK_LCONTROL && k->scanCode == kAltGrFakeCtrlScanCode)) {
			ts_key_event ev = {};
			ev.down = down;
			ev.repeat = down && g_down.test(vk);
			ev.mods = currentMods();
			g_down.set(vk, down);
			classify(vk, ev);
			InputHook::instance().dispatchKey(ev);
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION) {
		const auto *m = reinterpret_cast<const MSLLHOOKSTRUCT *>(lParam);
		MouseEvent ev = {};
		bool relevant = true;

		switch (wParam) {
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
			ev.button = MouseButton::Left;
			ev.down = wParam == WM_LBUTTONDOWN;
			break;
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
			ev.button = MouseButton::Right;
			ev.down = wParam == WM_RBUTTONDOWN;
			break;
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
			ev.button = MouseButton::Middle;
			ev.down = wParam == WM_MBUTTONDOWN;
			break;
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
			ev.button = MouseButton::Other;
			ev.down = wParam == WM_XBUTTONDOWN;
			break;
		default:
			relevant = false; /* moves and wheel are polled / ignored */
			break;
		}

		if (relevant) {
			ev.x = m->pt.x;
			ev.y = m->pt.y;
			ev.mods = currentMods();
			InputHook::instance().dispatchMouse(ev);
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

void hookThread(std::promise<bool> ready)
{
	MSG msg;
	/* Force creation of the thread message queue before signaling, so
	 * stop() can always post WM_QUIT to it. */
	PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
	g_threadId = GetCurrentThreadId();

	HINSTANCE module = GetModuleHandleW(nullptr);
	g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, module, 0);
	g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, module, 0);

	if (!g_keyboardHook || !g_mouseHook) {
		obs_log(LOG_WARNING, "SetWindowsHookEx failed: %lu", GetLastError());
		if (g_keyboardHook)
			UnhookWindowsHookEx(g_keyboardHook);
		if (g_mouseHook)
			UnhookWindowsHookEx(g_mouseHook);
		g_keyboardHook = g_mouseHook = nullptr;
		ready.set_value(false);
		return;
	}
	ready.set_value(true);

	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	UnhookWindowsHookEx(g_keyboardHook);
	UnhookWindowsHookEx(g_mouseHook);
	g_keyboardHook = g_mouseHook = nullptr;
}

BOOL CALLBACK monitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
	auto *out = reinterpret_cast<std::vector<Monitor> *>(data);
	MONITORINFOEXW info = {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, &info))
		return TRUE;

	char device[64];
	utf16ToUtf8(info.szDevice, device, sizeof(device));
	const char *shortName = device;
	if (strncmp(device, "\\\\.\\", 4) == 0)
		shortName += 4;

	Monitor m;
	m.name = shortName;
	m.x = info.rcMonitor.left;
	m.y = info.rcMonitor.top;
	m.width = info.rcMonitor.right - info.rcMonitor.left;
	m.height = info.rcMonitor.bottom - info.rcMonitor.top;
	m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
	out->push_back(m);
	return TRUE;
}

} // namespace

bool start()
{
	g_down.reset();
	std::promise<bool> ready;
	std::future<bool> result = ready.get_future();
	g_thread = std::thread(hookThread, std::move(ready));

	if (!result.get()) {
		g_thread.join();
		return false;
	}
	return true;
}

void stop()
{
	if (!g_thread.joinable())
		return;
	PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
	g_thread.join();
}

bool cursorPosition(int &x, int &y)
{
	POINT p;
	if (!GetCursorPos(&p))
		return false;
	x = p.x;
	y = p.y;
	return true;
}

bool caretPosition(int &x, int &y)
{
	HWND foreground = GetForegroundWindow();
	if (!foreground)
		return false;

	GUITHREADINFO info = {};
	info.cbSize = sizeof(info);
	if (!GetGUIThreadInfo(GetWindowThreadProcessId(foreground, nullptr), &info) || !info.hwndCaret)
		return false;

	const RECT &r = info.rcCaret;
	if (r.right <= r.left && r.bottom <= r.top)
		return false;

	POINT p = {r.left, (r.top + r.bottom) / 2};
	if (!ClientToScreen(info.hwndCaret, &p))
		return false;
	x = p.x;
	y = p.y;
	return true;
}

void prepareCaretTracking() {}

std::vector<Monitor> monitors()
{
	std::vector<Monitor> out;
	EnumDisplayMonitors(nullptr, nullptr, monitorEnumProc, reinterpret_cast<LPARAM>(&out));
	return out;
}

const char *name()
{
	return "windows";
}

} // namespace ts::platform

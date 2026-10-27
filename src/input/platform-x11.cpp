/*
 * Linux backend (X11 only).
 *
 * Uses the XRecord extension on a dedicated connection. Wayland sessions do
 * not allow global input capture: there the hook reports failure and the
 * features degrade gracefully (keystrokes disabled, zoom on hotkey only).
 */
#include "input-hook.hpp"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/record.h>
#include <X11/keysym.h>

#include <obs-module.h>
#include <plugin-support.h>
#include <obs-nix-platform.h>

#include <bitset>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace ts::platform {

namespace {

std::thread g_thread;
Display *g_ctrlDisplay = nullptr; /* creates/disables the record context */
Display *g_dataDisplay = nullptr; /* blocked in XRecordEnableContext */
XRecordContext g_context = 0;
std::bitset<256> g_down;

/* Connection for queries (cursor, keysyms, monitors). */
std::mutex g_queryMutex;
Display *g_queryDisplay = nullptr;

Display *queryDisplay()
{
	if (!g_queryDisplay)
		g_queryDisplay = XOpenDisplay(nullptr);
	return g_queryDisplay;
}

uint32_t modsFromState(unsigned int state)
{
	uint32_t mods = 0;
	if (state & ShiftMask)
		mods |= TS_MOD_SHIFT;
	if (state & ControlMask)
		mods |= TS_MOD_CTRL;
	if (state & Mod1Mask)
		mods |= TS_MOD_ALT;
	if (state & Mod4Mask)
		mods |= TS_MOD_META;
	if (state & Mod5Mask) /* ISO_Level3_Shift (AltGr) on standard layouts */
		mods |= TS_MOD_ALTGR;
	return mods;
}

void setName(ts_key_event &ev, ts_key_kind kind, const char *name)
{
	ev.kind = kind;
	snprintf(ev.name, sizeof(ev.name), "%s", name);
}

void encodeUtf8(uint32_t cp, char *out, size_t size)
{
	if (size < 5) {
		out[0] = 0;
		return;
	}
	if (cp < 0x80) {
		out[0] = (char)cp;
		out[1] = 0;
	} else if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		out[2] = 0;
	} else if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		out[3] = 0;
	} else {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		out[4] = 0;
	}
}

void classify(KeySym sym, ts_key_event &ev)
{
	ev.kind = TS_KEY_IGNORED;
	ev.name[0] = 0;

	if (sym >= XK_F1 && sym <= XK_F24) {
		ev.kind = TS_KEY_FUNCTION;
		snprintf(ev.name, sizeof(ev.name), "F%lu", (unsigned long)(sym - XK_F1 + 1));
		return;
	}
	if (sym >= XK_KP_0 && sym <= XK_KP_9) {
		ev.kind = TS_KEY_PRINTABLE;
		snprintf(ev.name, sizeof(ev.name), "Num %lu", (unsigned long)(sym - XK_KP_0));
		return;
	}

	switch (sym) {
	case XK_Shift_L:
	case XK_Shift_R:
	case XK_Control_L:
	case XK_Control_R:
	case XK_Alt_L:
	case XK_Alt_R:
	case XK_Meta_L:
	case XK_Meta_R:
	case XK_Super_L:
	case XK_Super_R:
	case XK_ISO_Level3_Shift:
	case XK_Mode_switch:
		return setName(ev, TS_KEY_MODIFIER, "Modifier");
	case XK_space:
		return setName(ev, TS_KEY_PRINTABLE, "Space");
	case XK_Escape:
		return setName(ev, TS_KEY_ESCAPE, "Esc");
	case XK_Return:
	case XK_KP_Enter:
		return setName(ev, TS_KEY_EDITING, "Enter");
	case XK_Tab:
	case XK_ISO_Left_Tab:
		return setName(ev, TS_KEY_EDITING, "Tab");
	case XK_BackSpace:
		return setName(ev, TS_KEY_EDITING, "Backspace");
	case XK_Delete:
	case XK_KP_Delete:
		return setName(ev, TS_KEY_EDITING, "Delete");
	case XK_Insert:
	case XK_KP_Insert:
		return setName(ev, TS_KEY_EDITING, "Insert");
	case XK_Left:
	case XK_KP_Left:
		return setName(ev, TS_KEY_NAVIGATION, "Left");
	case XK_Right:
	case XK_KP_Right:
		return setName(ev, TS_KEY_NAVIGATION, "Right");
	case XK_Up:
	case XK_KP_Up:
		return setName(ev, TS_KEY_NAVIGATION, "Up");
	case XK_Down:
	case XK_KP_Down:
		return setName(ev, TS_KEY_NAVIGATION, "Down");
	case XK_Home:
	case XK_KP_Home:
		return setName(ev, TS_KEY_NAVIGATION, "Home");
	case XK_End:
	case XK_KP_End:
		return setName(ev, TS_KEY_NAVIGATION, "End");
	case XK_Prior:
	case XK_KP_Prior:
		return setName(ev, TS_KEY_NAVIGATION, "PageUp");
	case XK_Next:
	case XK_KP_Next:
		return setName(ev, TS_KEY_NAVIGATION, "PageDown");
	case XK_Print:
		return setName(ev, TS_KEY_FUNCTION, "PrtSc");
	case XK_Pause:
		return setName(ev, TS_KEY_FUNCTION, "Pause");
	case XK_KP_Multiply:
		return setName(ev, TS_KEY_PRINTABLE, "Num *");
	case XK_KP_Add:
		return setName(ev, TS_KEY_PRINTABLE, "Num +");
	case XK_KP_Subtract:
		return setName(ev, TS_KEY_PRINTABLE, "Num -");
	case XK_KP_Decimal:
		return setName(ev, TS_KEY_PRINTABLE, "Num .");
	case XK_KP_Divide:
		return setName(ev, TS_KEY_PRINTABLE, "Num /");
	default:
		break;
	}

	/* Latin-1 keysyms equal their code point; Unicode keysyms are
	 * 0x01000000 + code point. */
	uint32_t cp = 0;
	if ((sym >= 0x20 && sym <= 0x7E) || (sym >= 0xA0 && sym <= 0xFF))
		cp = (uint32_t)sym;
	else if (sym >= 0x01000100 && sym <= 0x0110FFFF)
		cp = (uint32_t)(sym - 0x01000000);

	if (cp) {
		KeySym lower, upper;
		XConvertCase(sym, &lower, &upper);
		if (upper != sym && upper >= 0x20 && upper <= 0xFF)
			cp = (uint32_t)upper;
		ev.kind = TS_KEY_PRINTABLE;
		encodeUtf8(cp, ev.name, sizeof(ev.name));
	}
}

void recordCallback(XPointer, XRecordInterceptData *data)
{
	if (data->category == XRecordFromServer && data->data) {
		const xEvent *event = reinterpret_cast<const xEvent *>(data->data);
		const int type = event->u.u.type & 0x7F;
		const unsigned int detail = event->u.u.detail;
		const unsigned int state = event->u.keyButtonPointer.state;

		if (type == KeyPress || type == KeyRelease) {
			ts_key_event ev = {};
			ev.down = type == KeyPress;
			ev.repeat = ev.down && g_down.test(detail & 0xFF);
			ev.mods = modsFromState(state);
			g_down.set(detail & 0xFF, ev.down);

			KeySym sym = NoSymbol;
			{
				std::lock_guard<std::mutex> lock(g_queryMutex);
				if (Display *d = queryDisplay())
					sym = XkbKeycodeToKeysym(d, (KeyCode)detail, 0, 0);
			}
			classify(sym, ev);
			InputHook::instance().dispatchKey(ev);
		} else if ((type == ButtonPress || type == ButtonRelease) && detail >= 1 && detail <= 3) {
			/* Buttons 4-7 are the scroll wheel. */
			MouseEvent ev = {};
			ev.button = detail == 1 ? MouseButton::Left
						: (detail == 3 ? MouseButton::Right : MouseButton::Middle);
			ev.down = type == ButtonPress;
			ev.x = event->u.keyButtonPointer.rootX;
			ev.y = event->u.keyButtonPointer.rootY;
			ev.mods = modsFromState(state);
			InputHook::instance().dispatchMouse(ev);
		}
	}
	XRecordFreeData(data);
}

} // namespace

bool start()
{
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL) {
		obs_log(LOG_WARNING, "global input capture is only available on X11 sessions");
		return false;
	}

	g_ctrlDisplay = XOpenDisplay(nullptr);
	g_dataDisplay = XOpenDisplay(nullptr);
	if (!g_ctrlDisplay || !g_dataDisplay) {
		stop();
		return false;
	}

	int major = 0, minor = 0;
	if (!XRecordQueryVersion(g_ctrlDisplay, &major, &minor)) {
		obs_log(LOG_WARNING, "XRecord extension not available");
		stop();
		return false;
	}

	XRecordRange *range = XRecordAllocRange();
	range->device_events.first = KeyPress;
	range->device_events.last = ButtonRelease;
	XRecordClientSpec clients = XRecordAllClients;
	g_context = XRecordCreateContext(g_ctrlDisplay, 0, &clients, 1, &range, 1);
	XFree(range);
	if (!g_context) {
		stop();
		return false;
	}
	XSync(g_ctrlDisplay, False);

	g_down.reset();
	g_thread = std::thread([] {
		if (!XRecordEnableContext(g_dataDisplay, g_context, recordCallback, nullptr))
			obs_log(LOG_WARNING, "XRecordEnableContext failed");
	});
	return true;
}

void stop()
{
	if (g_context && g_ctrlDisplay) {
		XRecordDisableContext(g_ctrlDisplay, g_context);
		XFlush(g_ctrlDisplay);
	}
	if (g_thread.joinable())
		g_thread.join();
	if (g_context && g_ctrlDisplay)
		XRecordFreeContext(g_ctrlDisplay, g_context);
	g_context = 0;
	if (g_dataDisplay)
		XCloseDisplay(g_dataDisplay);
	if (g_ctrlDisplay)
		XCloseDisplay(g_ctrlDisplay);
	g_dataDisplay = g_ctrlDisplay = nullptr;
}

bool cursorPosition(int &x, int &y)
{
	std::lock_guard<std::mutex> lock(g_queryMutex);
	Display *d = queryDisplay();
	if (!d)
		return false;

	Window root, child;
	int rootX, rootY, winX, winY;
	unsigned int mask;
	if (!XQueryPointer(d, DefaultRootWindow(d), &root, &child, &rootX, &rootY, &winX, &winY, &mask))
		return false;
	x = rootX;
	y = rootY;
	return true;
}

bool caretPosition(int &, int &)
{
	/* AT-SPI support could be added here; the zoom falls back to the
	 * mouse position. */
	return false;
}

void prepareCaretTracking() {}

std::vector<Monitor> monitors()
{
	std::vector<Monitor> out;
	std::lock_guard<std::mutex> lock(g_queryMutex);
	Display *d = queryDisplay();
	if (!d)
		return out;

	int count = 0;
	XRRMonitorInfo *infos = XRRGetMonitors(d, DefaultRootWindow(d), True, &count);
	for (int i = 0; i < count; i++) {
		Monitor m;
		char *atomName = XGetAtomName(d, infos[i].name);
		m.name = atomName ? atomName : "Monitor";
		if (atomName)
			XFree(atomName);
		m.x = infos[i].x;
		m.y = infos[i].y;
		m.width = infos[i].width;
		m.height = infos[i].height;
		m.primary = infos[i].primary != 0;
		out.push_back(m);
	}
	if (infos)
		XRRFreeMonitors(infos);

	if (out.empty()) {
		Screen *s = DefaultScreenOfDisplay(d);
		out.push_back({"Screen", 0, 0, WidthOfScreen(s), HeightOfScreen(s), true});
	}
	return out;
}

const char *name()
{
	return "linux";
}

} // namespace ts::platform

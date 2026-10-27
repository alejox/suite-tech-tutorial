/*
 * macOS backend.
 *
 *  - Keyboard/mouse: a listen-only CGEventTap running on its own CFRunLoop
 *    thread. Requires the "Input Monitoring" permission for OBS
 *    (System Settings > Privacy & Security > Input Monitoring).
 *  - Text caret: Accessibility API (AXSelectedTextRange + AXBoundsForRange).
 *    Requires the "Accessibility" permission. Electron/Chromium apps such as
 *    VS Code only expose their text fields after AXManualAccessibility is set
 *    on the application element, which we do once per process.
 *
 * All coordinates are global display points with a top-left origin, which is
 * exactly the space CGDisplayBounds uses, so normalized mapping onto a
 * ScreenCaptureKit display capture works regardless of the Retina scale.
 */
#include "input-hook.hpp"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>

#include <obs-module.h>
#include <plugin-support.h>

#include <bitset>
#include <cstdio>
#include <future>
#include <mutex>
#include <set>
#include <thread>

namespace ts::platform {

namespace {

std::thread g_thread;
CFRunLoopRef g_runLoop = nullptr;
CFMachPortRef g_tap = nullptr;
std::bitset<128> g_down;

std::mutex g_axMutex;
std::set<pid_t> g_axEnabledPids;

uint32_t modsFromFlags(CGEventFlags flags)
{
	uint32_t mods = 0;
	if (flags & kCGEventFlagMaskShift)
		mods |= TS_MOD_SHIFT;
	if (flags & kCGEventFlagMaskControl)
		mods |= TS_MOD_CTRL;
	if (flags & kCGEventFlagMaskAlternate)
		mods |= TS_MOD_ALT;
	if (flags & kCGEventFlagMaskCommand)
		mods |= TS_MOD_META;
	return mods;
}

struct KeyInfo {
	int code;
	ts_key_kind kind;
	const char *name;
};

/* Positional names (ANSI layout). Shortcut labels on macOS are
 * conventionally shown this way, e.g. "⌘ ⇧ P". */
const KeyInfo kKeys[] = {
	{kVK_ANSI_A, TS_KEY_PRINTABLE, "A"},
	{kVK_ANSI_B, TS_KEY_PRINTABLE, "B"},
	{kVK_ANSI_C, TS_KEY_PRINTABLE, "C"},
	{kVK_ANSI_D, TS_KEY_PRINTABLE, "D"},
	{kVK_ANSI_E, TS_KEY_PRINTABLE, "E"},
	{kVK_ANSI_F, TS_KEY_PRINTABLE, "F"},
	{kVK_ANSI_G, TS_KEY_PRINTABLE, "G"},
	{kVK_ANSI_H, TS_KEY_PRINTABLE, "H"},
	{kVK_ANSI_I, TS_KEY_PRINTABLE, "I"},
	{kVK_ANSI_J, TS_KEY_PRINTABLE, "J"},
	{kVK_ANSI_K, TS_KEY_PRINTABLE, "K"},
	{kVK_ANSI_L, TS_KEY_PRINTABLE, "L"},
	{kVK_ANSI_M, TS_KEY_PRINTABLE, "M"},
	{kVK_ANSI_N, TS_KEY_PRINTABLE, "N"},
	{kVK_ANSI_O, TS_KEY_PRINTABLE, "O"},
	{kVK_ANSI_P, TS_KEY_PRINTABLE, "P"},
	{kVK_ANSI_Q, TS_KEY_PRINTABLE, "Q"},
	{kVK_ANSI_R, TS_KEY_PRINTABLE, "R"},
	{kVK_ANSI_S, TS_KEY_PRINTABLE, "S"},
	{kVK_ANSI_T, TS_KEY_PRINTABLE, "T"},
	{kVK_ANSI_U, TS_KEY_PRINTABLE, "U"},
	{kVK_ANSI_V, TS_KEY_PRINTABLE, "V"},
	{kVK_ANSI_W, TS_KEY_PRINTABLE, "W"},
	{kVK_ANSI_X, TS_KEY_PRINTABLE, "X"},
	{kVK_ANSI_Y, TS_KEY_PRINTABLE, "Y"},
	{kVK_ANSI_Z, TS_KEY_PRINTABLE, "Z"},
	{kVK_ANSI_0, TS_KEY_PRINTABLE, "0"},
	{kVK_ANSI_1, TS_KEY_PRINTABLE, "1"},
	{kVK_ANSI_2, TS_KEY_PRINTABLE, "2"},
	{kVK_ANSI_3, TS_KEY_PRINTABLE, "3"},
	{kVK_ANSI_4, TS_KEY_PRINTABLE, "4"},
	{kVK_ANSI_5, TS_KEY_PRINTABLE, "5"},
	{kVK_ANSI_6, TS_KEY_PRINTABLE, "6"},
	{kVK_ANSI_7, TS_KEY_PRINTABLE, "7"},
	{kVK_ANSI_8, TS_KEY_PRINTABLE, "8"},
	{kVK_ANSI_9, TS_KEY_PRINTABLE, "9"},
	{kVK_ANSI_Minus, TS_KEY_PRINTABLE, "-"},
	{kVK_ANSI_Equal, TS_KEY_PRINTABLE, "="},
	{kVK_ANSI_LeftBracket, TS_KEY_PRINTABLE, "["},
	{kVK_ANSI_RightBracket, TS_KEY_PRINTABLE, "]"},
	{kVK_ANSI_Backslash, TS_KEY_PRINTABLE, "\\"},
	{kVK_ANSI_Semicolon, TS_KEY_PRINTABLE, ";"},
	{kVK_ANSI_Quote, TS_KEY_PRINTABLE, "'"},
	{kVK_ANSI_Comma, TS_KEY_PRINTABLE, ","},
	{kVK_ANSI_Period, TS_KEY_PRINTABLE, "."},
	{kVK_ANSI_Slash, TS_KEY_PRINTABLE, "/"},
	{kVK_ANSI_Grave, TS_KEY_PRINTABLE, "`"},
	{kVK_ISO_Section, TS_KEY_PRINTABLE, "§"},
	{kVK_Space, TS_KEY_PRINTABLE, "Space"},
	{kVK_ANSI_Keypad0, TS_KEY_PRINTABLE, "Num 0"},
	{kVK_ANSI_Keypad1, TS_KEY_PRINTABLE, "Num 1"},
	{kVK_ANSI_Keypad2, TS_KEY_PRINTABLE, "Num 2"},
	{kVK_ANSI_Keypad3, TS_KEY_PRINTABLE, "Num 3"},
	{kVK_ANSI_Keypad4, TS_KEY_PRINTABLE, "Num 4"},
	{kVK_ANSI_Keypad5, TS_KEY_PRINTABLE, "Num 5"},
	{kVK_ANSI_Keypad6, TS_KEY_PRINTABLE, "Num 6"},
	{kVK_ANSI_Keypad7, TS_KEY_PRINTABLE, "Num 7"},
	{kVK_ANSI_Keypad8, TS_KEY_PRINTABLE, "Num 8"},
	{kVK_ANSI_Keypad9, TS_KEY_PRINTABLE, "Num 9"},
	{kVK_ANSI_KeypadDecimal, TS_KEY_PRINTABLE, "Num ."},
	{kVK_ANSI_KeypadMultiply, TS_KEY_PRINTABLE, "Num *"},
	{kVK_ANSI_KeypadPlus, TS_KEY_PRINTABLE, "Num +"},
	{kVK_ANSI_KeypadMinus, TS_KEY_PRINTABLE, "Num -"},
	{kVK_ANSI_KeypadDivide, TS_KEY_PRINTABLE, "Num /"},
	{kVK_ANSI_KeypadEquals, TS_KEY_PRINTABLE, "Num ="},
	{kVK_ANSI_KeypadEnter, TS_KEY_EDITING, "Enter"},
	{kVK_Return, TS_KEY_EDITING, "Enter"},
	{kVK_Tab, TS_KEY_EDITING, "Tab"},
	{kVK_Delete, TS_KEY_EDITING, "Backspace"},
	{kVK_ForwardDelete, TS_KEY_EDITING, "Delete"},
	{kVK_Escape, TS_KEY_ESCAPE, "Esc"},
	{kVK_LeftArrow, TS_KEY_NAVIGATION, "Left"},
	{kVK_RightArrow, TS_KEY_NAVIGATION, "Right"},
	{kVK_UpArrow, TS_KEY_NAVIGATION, "Up"},
	{kVK_DownArrow, TS_KEY_NAVIGATION, "Down"},
	{kVK_Home, TS_KEY_NAVIGATION, "Home"},
	{kVK_End, TS_KEY_NAVIGATION, "End"},
	{kVK_PageUp, TS_KEY_NAVIGATION, "PageUp"},
	{kVK_PageDown, TS_KEY_NAVIGATION, "PageDown"},
	{kVK_F1, TS_KEY_FUNCTION, "F1"},
	{kVK_F2, TS_KEY_FUNCTION, "F2"},
	{kVK_F3, TS_KEY_FUNCTION, "F3"},
	{kVK_F4, TS_KEY_FUNCTION, "F4"},
	{kVK_F5, TS_KEY_FUNCTION, "F5"},
	{kVK_F6, TS_KEY_FUNCTION, "F6"},
	{kVK_F7, TS_KEY_FUNCTION, "F7"},
	{kVK_F8, TS_KEY_FUNCTION, "F8"},
	{kVK_F9, TS_KEY_FUNCTION, "F9"},
	{kVK_F10, TS_KEY_FUNCTION, "F10"},
	{kVK_F11, TS_KEY_FUNCTION, "F11"},
	{kVK_F12, TS_KEY_FUNCTION, "F12"},
	{kVK_F13, TS_KEY_FUNCTION, "F13"},
	{kVK_F14, TS_KEY_FUNCTION, "F14"},
	{kVK_F15, TS_KEY_FUNCTION, "F15"},
	{kVK_F16, TS_KEY_FUNCTION, "F16"},
	{kVK_F17, TS_KEY_FUNCTION, "F17"},
	{kVK_F18, TS_KEY_FUNCTION, "F18"},
	{kVK_F19, TS_KEY_FUNCTION, "F19"},
	{kVK_F20, TS_KEY_FUNCTION, "F20"},
	{kVK_Command, TS_KEY_MODIFIER, "Modifier"},
	{kVK_RightCommand, TS_KEY_MODIFIER, "Modifier"},
	{kVK_Shift, TS_KEY_MODIFIER, "Modifier"},
	{kVK_RightShift, TS_KEY_MODIFIER, "Modifier"},
	{kVK_Option, TS_KEY_MODIFIER, "Modifier"},
	{kVK_RightOption, TS_KEY_MODIFIER, "Modifier"},
	{kVK_Control, TS_KEY_MODIFIER, "Modifier"},
	{kVK_RightControl, TS_KEY_MODIFIER, "Modifier"},
	{kVK_Function, TS_KEY_MODIFIER, "Modifier"},
};

void classify(int code, ts_key_event &ev)
{
	ev.kind = TS_KEY_IGNORED;
	ev.name[0] = 0;
	for (const KeyInfo &k : kKeys) {
		if (k.code == code) {
			ev.kind = k.kind;
			snprintf(ev.name, sizeof(ev.name), "%s", k.name);
			return;
		}
	}
}

CGEventRef tapCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void *)
{
	if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
		if (g_tap)
			CGEventTapEnable(g_tap, true);
		return event;
	}

	const uint32_t mods = modsFromFlags(CGEventGetFlags(event));

	if (type == kCGEventKeyDown || type == kCGEventKeyUp) {
		const int code = (int)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
		ts_key_event ev = {};
		ev.down = type == kCGEventKeyDown;
		ev.repeat = ev.down && CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
		ev.mods = mods;
		if (code >= 0 && code < (int)g_down.size()) {
			ev.repeat = ev.repeat || (ev.down && g_down.test((size_t)code));
			g_down.set((size_t)code, ev.down);
		}
		classify(code, ev);
		InputHook::instance().dispatchKey(ev);
		return event;
	}

	MouseEvent ev = {};
	switch (type) {
	case kCGEventLeftMouseDown:
	case kCGEventLeftMouseUp:
		ev.button = MouseButton::Left;
		ev.down = type == kCGEventLeftMouseDown;
		break;
	case kCGEventRightMouseDown:
	case kCGEventRightMouseUp:
		ev.button = MouseButton::Right;
		ev.down = type == kCGEventRightMouseDown;
		break;
	case kCGEventOtherMouseDown:
	case kCGEventOtherMouseUp:
		ev.button = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) == 2 ? MouseButton::Middle
											       : MouseButton::Other;
		ev.down = type == kCGEventOtherMouseDown;
		break;
	default:
		return event;
	}

	const CGPoint p = CGEventGetLocation(event);
	ev.x = (int)p.x;
	ev.y = (int)p.y;
	ev.mods = mods;
	InputHook::instance().dispatchMouse(ev);
	return event;
}

void tapThread(std::promise<bool> ready)
{
	const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
				 CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
				 CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
				 CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp);

	g_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, mask,
				 tapCallback, nullptr);
	if (!g_tap) {
		obs_log(LOG_WARNING, "CGEventTapCreate failed. Grant OBS the 'Input Monitoring' "
				     "permission in System Settings > Privacy & Security and restart OBS.");
		ready.set_value(false);
		return;
	}

	CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_tap, 0);
	g_runLoop = CFRunLoopGetCurrent();
	CFRetain(g_runLoop);
	CFRunLoopAddSource(g_runLoop, source, kCFRunLoopCommonModes);
	CGEventTapEnable(g_tap, true);
	ready.set_value(true);

	CFRunLoopRun();

	CGEventTapEnable(g_tap, false);
	CFRunLoopRemoveSource(g_runLoop, source, kCFRunLoopCommonModes);
	CFRelease(source);
	CFRelease(g_tap);
	g_tap = nullptr;
}

/* Ask Chromium/Electron apps to build their accessibility tree. */
void enableManualAccessibility(AXUIElementRef systemWide)
{
	CFTypeRef app = nullptr;
	if (AXUIElementCopyAttributeValue(systemWide, kAXFocusedApplicationAttribute, &app) != kAXErrorSuccess || !app)
		return;

	pid_t pid = 0;
	if (AXUIElementGetPid((AXUIElementRef)app, &pid) == kAXErrorSuccess) {
		std::lock_guard<std::mutex> lock(g_axMutex);
		if (g_axEnabledPids.insert(pid).second)
			AXUIElementSetAttributeValue((AXUIElementRef)app, CFSTR("AXManualAccessibility"),
						     kCFBooleanTrue);
	}
	CFRelease(app);
}

} // namespace

bool start()
{
	if (!CGPreflightListenEventAccess()) {
		/* Shows the system prompt the first time. */
		CGRequestListenEventAccess();
	}

	g_down.reset();
	std::promise<bool> ready;
	std::future<bool> result = ready.get_future();
	g_thread = std::thread(tapThread, std::move(ready));
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
	if (g_runLoop) {
		CFRunLoopStop(g_runLoop);
		CFRunLoopWakeUp(g_runLoop);
	}
	g_thread.join();
	if (g_runLoop) {
		CFRelease(g_runLoop);
		g_runLoop = nullptr;
	}
}

bool cursorPosition(int &x, int &y)
{
	CGEventRef event = CGEventCreate(nullptr);
	if (!event)
		return false;
	const CGPoint p = CGEventGetLocation(event);
	CFRelease(event);
	x = (int)p.x;
	y = (int)p.y;
	return true;
}

void prepareCaretTracking()
{
	static std::once_flag prompted;
	std::call_once(prompted, [] {
		const void *keys[] = {kAXTrustedCheckOptionPrompt};
		const void *values[] = {kCFBooleanTrue};
		CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
							     &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks);
		if (!AXIsProcessTrustedWithOptions(options))
			obs_log(LOG_INFO, "text caret tracking needs the 'Accessibility' "
					  "permission; falling back to the mouse position");
		CFRelease(options);
	});
}

bool caretPosition(int &x, int &y)
{
	if (!AXIsProcessTrusted())
		return false;

	AXUIElementRef systemWide = AXUIElementCreateSystemWide();
	AXUIElementSetMessagingTimeout(systemWide, 0.05f);
	enableManualAccessibility(systemWide);

	bool ok = false;
	CFTypeRef focused = nullptr;
	CFTypeRef range = nullptr;
	CFTypeRef bounds = nullptr;

	if (AXUIElementCopyAttributeValue(systemWide, kAXFocusedUIElementAttribute, &focused) == kAXErrorSuccess &&
	    focused &&
	    AXUIElementCopyAttributeValue((AXUIElementRef)focused, kAXSelectedTextRangeAttribute, &range) ==
		    kAXErrorSuccess &&
	    range &&
	    AXUIElementCopyParameterizedAttributeValue((AXUIElementRef)focused, kAXBoundsForRangeParameterizedAttribute,
						       range, &bounds) == kAXErrorSuccess &&
	    bounds) {
		CGRect rect;
		if (AXValueGetValue((AXValueRef)bounds, (AXValueType)kAXValueCGRectType, &rect) &&
		    (rect.size.height > 0 || rect.size.width > 0)) {
			x = (int)rect.origin.x;
			y = (int)(rect.origin.y + rect.size.height / 2);
			ok = true;
		}
	}

	if (bounds)
		CFRelease(bounds);
	if (range)
		CFRelease(range);
	if (focused)
		CFRelease(focused);
	CFRelease(systemWide);
	return ok;
}

std::vector<Monitor> monitors()
{
	std::vector<Monitor> out;
	CGDirectDisplayID ids[16];
	uint32_t count = 0;
	if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess)
		return out;

	for (uint32_t i = 0; i < count; i++) {
		const CGRect b = CGDisplayBounds(ids[i]);
		Monitor m;
		char name[64];
		snprintf(name, sizeof(name), "Display %u", (unsigned)ids[i]);
		m.name = name;
		m.x = (int)b.origin.x;
		m.y = (int)b.origin.y;
		m.width = (int)b.size.width;
		m.height = (int)b.size.height;
		m.primary = CGDisplayIsMain(ids[i]) != 0;
		out.push_back(m);
	}
	return out;
}

const char *name()
{
	return "macos";
}

} // namespace ts::platform

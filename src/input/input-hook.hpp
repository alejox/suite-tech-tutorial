/*
 * Global input hook shared by every feature of the suite.
 *
 * A single OS-level hook is started when the first listener registers and
 * stopped when the last one leaves, so nothing is captured while no
 * Tech Tutorial Suite source/filter exists.
 *
 * Callbacks run on the hook thread: keep them short (copy the data, return).
 * On Windows a slow low-level hook gets silently removed by the OS, and on
 * macOS a slow event tap gets disabled.
 */
#pragma once

#include "core/shortcut-filter.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ts {

enum class MouseButton { Left, Right, Middle, Other };

struct MouseEvent {
	MouseButton button;
	bool down;
	int x; /* global screen coordinates (same space as Monitor) */
	int y;
	uint32_t mods; /* TS_MOD_* */
};

struct Monitor {
	std::string name;
	int x;
	int y;
	int width;
	int height;
	bool primary;
};

using KeyHandler = std::function<void(const ts_key_event &)>;
using MouseHandler = std::function<void(const MouseEvent &)>;

class InputHook {
public:
	static InputHook &instance();

	/* Returns a listener id (> 0) or 0 if the platform hook failed. */
	int addListener(KeyHandler onKey, MouseHandler onMouse);
	/* Blocks until no callback of this listener is running. */
	void removeListener(int id);

	bool cursorPosition(int &x, int &y);

	/* Text caret tracking is comparatively expensive (accessibility IPC),
	 * so it only runs while someone asks for it, and only right after
	 * keyboard activity. */
	void requestCaretTracking(bool enable);
	/* Last known caret position and when it was sampled (os_gettime_ns). */
	bool caretPosition(int &x, int &y, uint64_t &timestampNs);

	std::vector<Monitor> monitors();

	/* "windows", "macos", "linux" — also sent to the overlay. */
	const char *platformName() const;
	bool isMacOS() const;
	bool running() const;

	/* Called on module unload: drops every listener and stops the hook. */
	void shutdown();

	/* Used by the platform backends. */
	void dispatchKey(const ts_key_event &ev);
	void dispatchMouse(const MouseEvent &ev);

private:
	InputHook() = default;
	struct Impl;
	static Impl &impl();
};

namespace platform {
/* Implemented once per OS in platform-*.cpp. */
bool start();
void stop();
bool cursorPosition(int &x, int &y);
/* May block for a few ms; only called from the caret worker thread. */
bool caretPosition(int &x, int &y);
void prepareCaretTracking(); /* e.g. ask for Accessibility permission */
std::vector<Monitor> monitors();
const char *name();
} // namespace platform

} // namespace ts

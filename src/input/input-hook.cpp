#include "input-hook.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace ts {

namespace {

struct Listener {
	int id;
	KeyHandler onKey;
	MouseHandler onMouse;
};

/* Caret is only polled for this long after the last key press. */
constexpr uint64_t kCaretActiveWindowNs = 1500ull * 1000000ull;
constexpr auto kCaretPollInterval = std::chrono::milliseconds(70);

} // namespace

struct InputHook::Impl {
	std::mutex lifecycleMutex; /* serializes platform start/stop */
	std::mutex listenersMutex; /* held while dispatching */
	std::vector<Listener> listeners;
	int nextId = 1;
	std::atomic<bool> running{false};

	std::atomic<uint64_t> lastKeyNs{0};

	std::mutex caretMutex;
	std::condition_variable caretCv;
	std::thread caretThread;
	int caretRequests = 0;
	bool caretStop = false;
	bool caretValid = false;
	int caretX = 0;
	int caretY = 0;
	uint64_t caretTs = 0;

	void caretLoop()
	{
		std::unique_lock<std::mutex> lock(caretMutex);
		while (!caretStop) {
			const uint64_t now = os_gettime_ns();
			const uint64_t lastKey = lastKeyNs.load();
			const bool typing = lastKey && now - lastKey < kCaretActiveWindowNs;

			if (caretRequests > 0 && typing) {
				lock.unlock();
				int x = 0, y = 0;
				const bool ok = platform::caretPosition(x, y);
				lock.lock();
				caretValid = ok;
				if (ok) {
					caretX = x;
					caretY = y;
					caretTs = os_gettime_ns();
				}
				caretCv.wait_for(lock, kCaretPollInterval);
			} else {
				caretCv.wait_for(lock, std::chrono::milliseconds(250));
			}
		}
	}
};

InputHook &InputHook::instance()
{
	static InputHook hook;
	return hook;
}

InputHook::Impl &InputHook::impl()
{
	static Impl impl;
	return impl;
}

int InputHook::addListener(KeyHandler onKey, MouseHandler onMouse)
{
	Impl &d = impl();
	std::lock_guard<std::mutex> life(d.lifecycleMutex);

	if (!d.running) {
		if (!platform::start()) {
			obs_log(LOG_WARNING, "could not start the %s input hook", platform::name());
			return 0;
		}
		d.running = true;
		obs_log(LOG_INFO, "%s input hook started", platform::name());
	}

	std::lock_guard<std::mutex> lock(d.listenersMutex);
	const int id = d.nextId++;
	d.listeners.push_back({id, std::move(onKey), std::move(onMouse)});
	return id;
}

void InputHook::removeListener(int id)
{
	Impl &d = impl();
	if (id <= 0)
		return;

	std::lock_guard<std::mutex> life(d.lifecycleMutex);
	bool empty;
	{
		std::lock_guard<std::mutex> lock(d.listenersMutex);
		for (auto it = d.listeners.begin(); it != d.listeners.end(); ++it) {
			if (it->id == id) {
				d.listeners.erase(it);
				break;
			}
		}
		empty = d.listeners.empty();
	}

	/* Stopping joins the hook thread, which may be waiting on
	 * listenersMutex inside dispatch: never stop while holding it. */
	if (empty && d.running) {
		platform::stop();
		d.running = false;
		obs_log(LOG_INFO, "input hook stopped");
	}
}

bool InputHook::cursorPosition(int &x, int &y)
{
	return platform::cursorPosition(x, y);
}

void InputHook::requestCaretTracking(bool enable)
{
	Impl &d = impl();
	bool startThread = false;
	{
		std::lock_guard<std::mutex> lock(d.caretMutex);
		d.caretRequests += enable ? 1 : -1;
		if (d.caretRequests < 0)
			d.caretRequests = 0;
		startThread = enable && !d.caretThread.joinable();
		if (startThread)
			d.caretStop = false;
	}

	if (enable)
		platform::prepareCaretTracking();
	if (startThread)
		d.caretThread = std::thread([&d] { d.caretLoop(); });
	d.caretCv.notify_all();
}

bool InputHook::caretPosition(int &x, int &y, uint64_t &timestampNs)
{
	Impl &d = impl();
	std::lock_guard<std::mutex> lock(d.caretMutex);
	if (!d.caretValid)
		return false;
	x = d.caretX;
	y = d.caretY;
	timestampNs = d.caretTs;
	return true;
}

std::vector<Monitor> InputHook::monitors()
{
	return platform::monitors();
}

const char *InputHook::platformName() const
{
	return platform::name();
}

bool InputHook::isMacOS() const
{
	return std::strcmp(platform::name(), "macos") == 0;
}

bool InputHook::running() const
{
	return impl().running;
}

void InputHook::shutdown()
{
	Impl &d = impl();
	{
		std::lock_guard<std::mutex> life(d.lifecycleMutex);
		{
			std::lock_guard<std::mutex> lock(d.listenersMutex);
			d.listeners.clear();
		}
		if (d.running) {
			platform::stop();
			d.running = false;
		}
	}

	{
		std::lock_guard<std::mutex> lock(d.caretMutex);
		d.caretStop = true;
		d.caretRequests = 0;
	}
	d.caretCv.notify_all();
	if (d.caretThread.joinable())
		d.caretThread.join();
}

void InputHook::dispatchKey(const ts_key_event &ev)
{
	Impl &d = impl();
	if (ev.down && ev.kind != TS_KEY_MODIFIER) {
		d.lastKeyNs = os_gettime_ns();
		d.caretCv.notify_all();
	}

	std::lock_guard<std::mutex> lock(d.listenersMutex);
	for (auto &l : d.listeners)
		if (l.onKey)
			l.onKey(ev);
}

void InputHook::dispatchMouse(const MouseEvent &ev)
{
	Impl &d = impl();
	std::lock_guard<std::mutex> lock(d.listenersMutex);
	for (auto &l : d.listeners)
		if (l.onMouse)
			l.onMouse(ev);
}

} // namespace ts

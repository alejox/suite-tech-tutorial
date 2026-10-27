/*
 * "Keystroke Visualizer" source.
 *
 * Native side: captures keys through the shared input hook, runs them
 * through the shortcut filter (plain typing is dropped right there, it never
 * leaves this process), and forwards the remaining shortcuts as JSON events.
 *
 * Rendering side: a private obs-browser source showing the bundled Vue
 * overlay (data/overlay/index.html). Events are pushed with obs-browser's
 * `javascript_event` proc, so they only reach *our* overlay instance and no
 * other browser source in the scene collection. Appearance settings are
 * injected as CSS custom properties.
 */
#include "keystroke-source.hpp"

#include "core/shortcut-filter.h"
#include "input/input-hook.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace ts {

namespace {

constexpr const char *kSourceId = "tech_suite_keystroke_overlay";
constexpr const char *kEventShortcut = "techsuite:shortcut";
constexpr size_t kMaxQueued = 32;

std::string colorToCss(long long abgr)
{
	char buf[16];
	const unsigned v = (unsigned)abgr;
	snprintf(buf, sizeof(buf), "#%02x%02x%02x", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
	return buf;
}

/* Only allow known identifiers into the CSS we inject. */
std::string safeIdent(const char *value, const char *fallback)
{
	if (!value || !*value)
		return fallback;
	for (const char *p = value; *p; p++) {
		const char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
			return fallback;
	}
	return value;
}

class KeystrokeOverlay {
public:
	KeystrokeOverlay(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		pauseHotkey_ = obs_hotkey_register_source(source, "TechSuite.Keystrokes.Pause",
							  obs_module_text("Keystrokes.Hotkey.Pause"), onPauseHotkey,
							  this);
		update(settings);

		listener_ = InputHook::instance().addListener([this](const ts_key_event &ev) { onKey(ev); },
							      [this](const MouseEvent &ev) { onMouse(ev); });
	}

	~KeystrokeOverlay()
	{
		InputHook::instance().removeListener(listener_);
		obs_hotkey_unregister(pauseHotkey_);
		obs_source_release(browser_);
	}

	void update(obs_data_t *settings)
	{
		width_ = (uint32_t)obs_data_get_int(settings, "width");
		height_ = (uint32_t)obs_data_get_int(settings, "height");

		ts_shortcut_opts opts;
		ts_shortcut_opts_default(&opts, InputHook::instance().isMacOS());
		opts.alt_is_command = obs_data_get_bool(settings, "alt_is_command");
		opts.show_function_keys = obs_data_get_bool(settings, "show_function_keys");
		opts.show_escape = obs_data_get_bool(settings, "show_escape");
		opts.show_editing_keys = obs_data_get_bool(settings, "show_editing_keys");
		opts.show_navigation_keys = obs_data_get_bool(settings, "show_navigation_keys");
		opts.show_mouse_combos = obs_data_get_bool(settings, "show_mouse_combos");
		{
			std::lock_guard<std::mutex> lock(mutex_);
			opts_ = opts;
		}

		char css[1024];
		snprintf(css, sizeof(css),
			 ":root{--ts-theme:%s;--ts-position:%s;--ts-key-style:%s;--ts-accent:%s;"
			 "--ts-font-size:%dpx;--ts-duration:%d;--ts-max:%d;--ts-platform:%s;}\n"
			 "body{background-color:rgba(0,0,0,0);margin:0;overflow:hidden;}",
			 safeIdent(obs_data_get_string(settings, "theme"), "glass").c_str(),
			 safeIdent(obs_data_get_string(settings, "position"), "bottom-center").c_str(),
			 safeIdent(obs_data_get_string(settings, "key_style"), "auto").c_str(),
			 colorToCss(obs_data_get_int(settings, "accent_color")).c_str(),
			 (int)obs_data_get_int(settings, "font_size"), (int)obs_data_get_int(settings, "duration_ms"),
			 (int)obs_data_get_int(settings, "max_items"), InputHook::instance().platformName());

		obs_data_t *browserSettings = obs_data_create();
		char *page = obs_module_file("overlay/index.html");
		obs_data_set_bool(browserSettings, "is_local_file", true);
		obs_data_set_string(browserSettings, "local_file", page ? page : "");
		obs_data_set_int(browserSettings, "width", width_);
		obs_data_set_int(browserSettings, "height", height_);
		obs_data_set_bool(browserSettings, "fps_custom", true);
		obs_data_set_int(browserSettings, "fps", 60);
		obs_data_set_bool(browserSettings, "shutdown", false);
		obs_data_set_bool(browserSettings, "restart_when_active", false);
		obs_data_set_bool(browserSettings, "reroute_audio", true);
		obs_data_set_string(browserSettings, "css", css);
		bfree(page);

		if (!browser_) {
			browser_ = obs_source_create_private("browser_source", "tech-suite-keystroke-overlay",
							     browserSettings);
			if (!browser_)
				obs_log(LOG_WARNING, "obs-browser is not available: the "
						     "keystroke overlay cannot be displayed");
		} else {
			obs_source_update(browser_, browserSettings);
		}
		obs_data_release(browserSettings);
	}

	void tick()
	{
		std::deque<std::string> events;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			events.swap(queue_);
		}
		if (!browser_)
			return;

		proc_handler_t *ph = obs_source_get_proc_handler(browser_);
		for (const std::string &json : events) {
			calldata_t cd;
			calldata_init(&cd);
			calldata_set_string(&cd, "eventName", kEventShortcut);
			calldata_set_string(&cd, "jsonString", json.c_str());
			proc_handler_call(ph, "javascript_event", &cd);
			calldata_free(&cd);
		}
	}

	void render()
	{
		if (browser_)
			obs_source_video_render(browser_);
	}

	void enumSources(obs_source_enum_proc_t cb, void *param)
	{
		if (browser_)
			cb(source_, browser_, param);
	}

	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }

	void sendPreview()
	{
		static const char *const demos[][4] = {
			{"Ctrl", "Shift", "P", nullptr},
			{"Meta", "K", nullptr, nullptr},
			{"Alt", "Shift", "F", nullptr},
			{"F5", nullptr, nullptr, nullptr},
		};
		const auto &demo = demos[previewIndex_++ % 4];
		ts_shortcut sc = {};
		for (const char *key : demo) {
			if (!key)
				break;
			snprintf(sc.keys[sc.count++], TS_KEY_NAME_MAX, "%s", key);
		}
		push(sc);
	}

private:
	void push(const ts_shortcut &sc)
	{
		char json[512];
		std::lock_guard<std::mutex> lock(mutex_);
		if (!ts_shortcut_to_json(&sc, InputHook::instance().platformName(), ++seq_, json, sizeof(json)))
			return;
		if (queue_.size() >= kMaxQueued)
			queue_.pop_front();
		queue_.emplace_back(json);
	}

	/* Hook thread. */
	void onKey(const ts_key_event &ev)
	{
		if (paused_)
			return;
		ts_shortcut sc;
		ts_shortcut_opts opts;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			opts = opts_;
		}
		if (ts_shortcut_from_event(&ev, &opts, &sc))
			push(sc);
	}

	void onMouse(const MouseEvent &mouse)
	{
		if (paused_ || !mouse.down)
			return;
		ts_key_event ev = {};
		ev.down = true;
		ev.kind = TS_KEY_MOUSE;
		ev.mods = mouse.mods;
		const char *name = mouse.button == MouseButton::Left     ? "Click"
				   : mouse.button == MouseButton::Right  ? "Right Click"
				   : mouse.button == MouseButton::Middle ? "Middle Click"
									 : "Mouse";
		snprintf(ev.name, sizeof(ev.name), "%s", name);
		onKey(ev);
	}

	static void onPauseHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
	{
		auto *self = static_cast<KeystrokeOverlay *>(data);
		if (pressed) {
			self->paused_ = !self->paused_;
			obs_log(LOG_INFO, "keystroke capture %s", self->paused_ ? "paused" : "resumed");
		}
	}

	obs_source_t *source_;
	obs_source_t *browser_ = nullptr;
	obs_hotkey_id pauseHotkey_ = OBS_INVALID_HOTKEY_ID;
	int listener_ = 0;
	std::atomic<uint32_t> width_{800};
	std::atomic<uint32_t> height_{200};
	std::atomic<bool> paused_{false};
	unsigned previewIndex_ = 0;

	std::mutex mutex_;
	ts_shortcut_opts opts_{};
	std::deque<std::string> queue_;
	uint64_t seq_ = 0;
};

/* ------------------------------------------------------------------------ */

const char *getName(void *)
{
	return obs_module_text("Keystrokes.Name");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	return new KeystrokeOverlay(settings, source);
}

void destroy(void *data)
{
	delete static_cast<KeystrokeOverlay *>(data);
}

void update(void *data, obs_data_t *settings)
{
	static_cast<KeystrokeOverlay *>(data)->update(settings);
}

void videoTick(void *data, float)
{
	static_cast<KeystrokeOverlay *>(data)->tick();
}

void videoRender(void *data, gs_effect_t *)
{
	static_cast<KeystrokeOverlay *>(data)->render();
}

uint32_t getWidth(void *data)
{
	return static_cast<KeystrokeOverlay *>(data)->width();
}

uint32_t getHeight(void *data)
{
	return static_cast<KeystrokeOverlay *>(data)->height();
}

void enumSources(void *data, obs_source_enum_proc_t cb, void *param)
{
	static_cast<KeystrokeOverlay *>(data)->enumSources(cb, param);
}

bool onPreview(obs_properties_t *, obs_property_t *, void *data)
{
	static_cast<KeystrokeOverlay *>(data)->sendPreview();
	return false;
}

obs_properties_t *getProperties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	/* Appearance */
	obs_properties_t *look = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(look, "theme", obs_module_text("Keystrokes.Theme"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Keystrokes.Theme.Glass"), "glass");
	obs_property_list_add_string(p, obs_module_text("Keystrokes.Theme.Light"), "light");
	obs_property_list_add_string(p, obs_module_text("Keystrokes.Theme.Neon"), "neon");
	obs_property_list_add_string(p, obs_module_text("Keystrokes.Theme.Minimal"), "minimal");

	p = obs_properties_add_list(look, "position", obs_module_text("Keystrokes.Position"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	for (const char *pos :
	     {"bottom-center", "bottom-left", "bottom-right", "top-center", "top-left", "top-right", "center"}) {
		char key[64];
		snprintf(key, sizeof(key), "Keystrokes.Position.%s", pos);
		obs_property_list_add_string(p, obs_module_text(key), pos);
	}

	p = obs_properties_add_list(look, "key_style", obs_module_text("Keystrokes.KeyStyle"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Keystrokes.KeyStyle.Auto"), "auto");
	obs_property_list_add_string(p, obs_module_text("Keystrokes.KeyStyle.Symbols"), "symbols");
	obs_property_list_add_string(p, obs_module_text("Keystrokes.KeyStyle.Text"), "text");

	obs_properties_add_color(look, "accent_color", obs_module_text("Keystrokes.Accent"));
	p = obs_properties_add_int_slider(look, "font_size", obs_module_text("Keystrokes.FontSize"), 12, 96, 1);
	obs_property_int_set_suffix(p, " px");
	p = obs_properties_add_int_slider(look, "duration_ms", obs_module_text("Keystrokes.Duration"), 500, 8000, 100);
	obs_property_int_set_suffix(p, " ms");
	obs_properties_add_int_slider(look, "max_items", obs_module_text("Keystrokes.MaxItems"), 1, 8, 1);
	obs_properties_add_int(look, "width", obs_module_text("Keystrokes.Width"), 100, 4096, 1);
	obs_properties_add_int(look, "height", obs_module_text("Keystrokes.Height"), 50, 2160, 1);
	obs_properties_add_group(props, "group_look", obs_module_text("Keystrokes.Group.Look"), OBS_GROUP_NORMAL, look);

	/* What to show */
	obs_properties_t *filter = obs_properties_create();
	obs_properties_add_bool(filter, "show_function_keys", obs_module_text("Keystrokes.ShowFunction"));
	obs_properties_add_bool(filter, "show_escape", obs_module_text("Keystrokes.ShowEscape"));
	obs_properties_add_bool(filter, "show_editing_keys", obs_module_text("Keystrokes.ShowEditing"));
	obs_properties_add_bool(filter, "show_navigation_keys", obs_module_text("Keystrokes.ShowNavigation"));
	obs_properties_add_bool(filter, "show_mouse_combos", obs_module_text("Keystrokes.ShowMouse"));
	p = obs_properties_add_bool(filter, "alt_is_command", obs_module_text("Keystrokes.AltIsCommand"));
	obs_property_set_long_description(p, obs_module_text("Keystrokes.AltIsCommand.Description"));
	obs_properties_add_text(filter, "privacy_info", obs_module_text("Keystrokes.PrivacyInfo"), OBS_TEXT_INFO);
	obs_properties_add_group(props, "group_filter", obs_module_text("Keystrokes.Group.Filter"), OBS_GROUP_NORMAL,
				 filter);

	obs_properties_add_button2(props, "preview", obs_module_text("Keystrokes.Preview"), onPreview, data);
	obs_properties_add_text(props, "permissions_info", obs_module_text("Permissions.Info"), OBS_TEXT_INFO);
	return props;
}

void getDefaults(obs_data_t *settings)
{
	const bool mac = InputHook::instance().isMacOS();
	ts_shortcut_opts opts;
	ts_shortcut_opts_default(&opts, mac);

	obs_data_set_default_string(settings, "theme", "glass");
	obs_data_set_default_string(settings, "position", "bottom-center");
	obs_data_set_default_string(settings, "key_style", "auto");
	obs_data_set_default_int(settings, "accent_color", 0xFFFFC04A); /* ABGR: #4ac0ff */
	obs_data_set_default_int(settings, "font_size", 30);
	obs_data_set_default_int(settings, "duration_ms", 1800);
	obs_data_set_default_int(settings, "max_items", 4);
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 240);

	obs_data_set_default_bool(settings, "show_function_keys", opts.show_function_keys);
	obs_data_set_default_bool(settings, "show_escape", opts.show_escape);
	obs_data_set_default_bool(settings, "show_editing_keys", opts.show_editing_keys);
	obs_data_set_default_bool(settings, "show_navigation_keys", opts.show_navigation_keys);
	obs_data_set_default_bool(settings, "show_mouse_combos", opts.show_mouse_combos);
	obs_data_set_default_bool(settings, "alt_is_command", opts.alt_is_command);
}

} // namespace

void registerKeystrokeSource()
{
	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = getName;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.video_tick = videoTick;
	info.video_render = videoRender;
	info.get_width = getWidth;
	info.get_height = getHeight;
	info.enum_active_sources = enumSources;
	info.enum_all_sources = enumSources;
	info.get_properties = getProperties;
	info.get_defaults = getDefaults;
	obs_register_source(&info);
}

} // namespace ts

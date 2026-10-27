/*
 * "Auto-Zoom & Smart Cursor" video filter.
 *
 * Add it to a Display Capture (or any source showing a whole monitor). It
 * zooms smoothly into where you click or type, follows the cursor while
 * zoomed, zooms back out when you stop, and can draw a cursor spotlight and
 * click ripples. Everything happens live on the GPU, nothing is recorded.
 */
#include "auto-zoom-filter.hpp"

#include "core/zoom-logic.h"
#include "input/input-hook.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ts {

namespace {

constexpr const char *kFilterId = "tech_suite_auto_zoom_filter";
constexpr double kRippleDuration = 0.45;
constexpr uint64_t kCaretMaxAgeNs = 400ull * 1000000ull;

enum class RegionMode : long long { Monitor = 0, Manual = 1 };

struct Region {
	double x = 0, y = 0, w = 0, h = 0;
	bool valid() const { return w > 0 && h > 0; }
	bool toNormalized(int gx, int gy, double &nx, double &ny) const
	{
		if (!valid())
			return false;
		nx = (gx - x) / w;
		ny = (gy - y) / h;
		return nx >= 0.0 && nx <= 1.0 && ny >= 0.0 && ny <= 1.0;
	}
};

struct Config {
	Region region;
	bool zoomOnClick = true;
	bool zoomOnTyping = true;
	bool useCaret = true;
	ts_zoom_params zoom{};

	bool showHalo = false;
	vec4 haloColor{};
	float haloRadius = 40.0f;

	bool showRipples = true;
	vec4 rippleColor{};
	float rippleSize = 36.0f;
};

struct PendingClick {
	int x;
	int y;
};

std::string monitorKey(const Monitor &m)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%d,%d,%d,%d", m.x, m.y, m.width, m.height);
	return buf;
}

Region primaryMonitorRegion()
{
	Region r;
	auto mons = InputHook::instance().monitors();
	for (const Monitor &m : mons) {
		if (m.primary || !r.valid())
			r = {(double)m.x, (double)m.y, (double)m.width, (double)m.height};
		if (m.primary)
			break;
	}
	return r;
}

class AutoZoomFilter {
public:
	AutoZoomFilter(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		char *path = obs_module_file("effects/auto-zoom.effect");
		obs_enter_graphics();
		effect_ = gs_effect_create_from_file(path, nullptr);
		obs_leave_graphics();
		bfree(path);
		if (!effect_)
			obs_log(LOG_ERROR, "failed to load auto-zoom.effect");
		else
			cacheParams();

		ts_zoom_init(&state_);
		update(settings);

		toggleHotkey_ = obs_hotkey_register_source(source, "TechSuite.AutoZoom.Toggle",
							   obs_module_text("AutoZoom.Hotkey.Toggle"), onToggleHotkey,
							   this);
		holdHotkey_ = obs_hotkey_register_source(source, "TechSuite.AutoZoom.Hold",
							 obs_module_text("AutoZoom.Hotkey.Hold"), onHoldHotkey, this);

		listener_ = InputHook::instance().addListener([this](const ts_key_event &ev) { onKey(ev); },
							      [this](const MouseEvent &ev) { onMouse(ev); });
		InputHook::instance().requestCaretTracking(true);
	}

	~AutoZoomFilter()
	{
		InputHook::instance().requestCaretTracking(false);
		InputHook::instance().removeListener(listener_);
		obs_hotkey_unregister(toggleHotkey_);
		obs_hotkey_unregister(holdHotkey_);

		if (effect_) {
			obs_enter_graphics();
			gs_effect_destroy(effect_);
			obs_leave_graphics();
		}
	}

	void update(obs_data_t *settings)
	{
		Config c;

		const auto mode = (RegionMode)obs_data_get_int(settings, "region_mode");
		if (mode == RegionMode::Manual) {
			c.region.x = (double)obs_data_get_int(settings, "region_x");
			c.region.y = (double)obs_data_get_int(settings, "region_y");
			c.region.w = (double)obs_data_get_int(settings, "region_w");
			c.region.h = (double)obs_data_get_int(settings, "region_h");
		} else {
			int x, y, w, h;
			const char *key = obs_data_get_string(settings, "monitor");
			if (key && sscanf(key, "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
				c.region = {(double)x, (double)y, (double)w, (double)h};
			else
				c.region = primaryMonitorRegion();
		}

		c.zoomOnClick = obs_data_get_bool(settings, "zoom_on_click");
		c.zoomOnTyping = obs_data_get_bool(settings, "zoom_on_typing");
		c.useCaret = obs_data_get_bool(settings, "use_caret");

		ts_zoom_params_default(&c.zoom);
		c.zoom.zoom_level = obs_data_get_double(settings, "zoom_level");
		c.zoom.hold_time = obs_data_get_double(settings, "hold_time");
		/* 1..10 slider -> 3..18 rad/s */
		c.zoom.speed = 3.0 + ((double)obs_data_get_int(settings, "speed") - 1.0) * (15.0 / 9.0);
		c.zoom.safe_margin = (double)obs_data_get_int(settings, "safe_margin") / 100.0;
		c.zoom.follow_cursor = obs_data_get_bool(settings, "follow_cursor");

		c.showHalo = obs_data_get_bool(settings, "show_halo");
		vec4_from_rgba(&c.haloColor, (uint32_t)obs_data_get_int(settings, "halo_color"));
		c.haloColor.w = (float)obs_data_get_int(settings, "halo_opacity") / 100.0f;
		c.haloRadius = (float)obs_data_get_int(settings, "halo_radius");

		c.showRipples = obs_data_get_bool(settings, "show_ripples");
		vec4_from_rgba(&c.rippleColor, (uint32_t)obs_data_get_int(settings, "ripple_color"));
		c.rippleSize = (float)obs_data_get_int(settings, "ripple_size");

		std::lock_guard<std::mutex> lock(mutex_);
		config_ = c;
	}

	void tick(float seconds)
	{
		std::vector<PendingClick> clicks;
		bool typed;
		Config c;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			clicks.swap(pendingClicks_);
			typed = pendingTyping_;
			pendingTyping_ = false;
			c = config_;
		}

		const double dt = seconds;
		double cx = -1.0, cy = -1.0;
		int gx, gy;
		const bool cursorInside = InputHook::instance().cursorPosition(gx, gy) &&
					  c.region.toNormalized(gx, gy, cx, cy);
		cursorX_ = cx;
		cursorY_ = cy;
		cursorInside_ = cursorInside;

		const bool autoZoom = enabled_ && c.region.valid();

		for (const PendingClick &click : clicks) {
			double nx, ny;
			if (!c.region.toNormalized(click.x, click.y, nx, ny))
				continue;
			if (c.showRipples) {
				rippleX_ = nx;
				rippleY_ = ny;
				rippleAge_ = 0.0;
			}
			if (autoZoom && c.zoomOnClick)
				ts_zoom_activity(&state_, &c.zoom, nx, ny);
		}

		if (typed && autoZoom && c.zoomOnTyping) {
			double nx, ny;
			int kx, ky;
			uint64_t ts = 0;
			if (c.useCaret && InputHook::instance().caretPosition(kx, ky, ts) &&
			    os_gettime_ns() - ts < kCaretMaxAgeNs && c.region.toNormalized(kx, ky, nx, ny))
				ts_zoom_activity(&state_, &c.zoom, nx, ny);
			else if (cursorInside)
				ts_zoom_activity(&state_, &c.zoom, cx, cy);
		}

		if (holdRequested_ != state_.manual) {
			const double hx = cursorInside ? cx : 0.5;
			const double hy = cursorInside ? cy : 0.5;
			ts_zoom_set_manual(&state_, &c.zoom, holdRequested_, hx, hy);
		}
		if (!enabled_ && !state_.manual && state_.active)
			ts_zoom_reset(&state_);

		ts_zoom_tick(&state_, &c.zoom, dt, cursorInside, cx, cy);

		if (rippleAge_ < kRippleDuration)
			rippleAge_ += dt;

		renderConfig_ = c;
	}

	void render()
	{
		obs_source_t *target = obs_filter_get_target(source_);
		const uint32_t width = target ? obs_source_get_base_width(target) : 0;
		const uint32_t height = target ? obs_source_get_base_height(target) : 0;
		const Config &c = renderConfig_;

		const bool rippleVisible = c.showRipples && rippleAge_ < kRippleDuration;
		const bool haloVisible = c.showHalo && cursorInside_ && c.haloColor.w > 0.0f;

		if (!effect_ || !width || !height || (ts_zoom_is_idle(&state_) && !rippleVisible && !haloVisible)) {
			obs_source_skip_video_filter(source_);
			return;
		}

		double scale, cx, cy;
		ts_zoom_get_view(&state_, &scale, &cx, &cy);

		if (!obs_source_process_filter_begin(source_, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			return;

		vec2 v;
		vec2_set(&v, (float)(1.0 / scale), (float)(1.0 / scale));
		gs_effect_set_vec2(pUvScale_, &v);
		vec2_set(&v, (float)(cx - 0.5 / scale), (float)(cy - 0.5 / scale));
		gs_effect_set_vec2(pUvOffset_, &v);
		vec2_set(&v, (float)width, (float)height);
		gs_effect_set_vec2(pSourceSize_, &v);

		vec4 transparent;
		vec4_zero(&transparent);

		vec2_set(&v, (float)cursorX_, (float)cursorY_);
		gs_effect_set_vec2(pHaloPos_, &v);
		gs_effect_set_float(pHaloRadius_, c.haloRadius);
		gs_effect_set_vec4(pHaloColor_, haloVisible ? &c.haloColor : &transparent);

		/* Every uniform must be set on every draw, even when unused. */
		const double t = rippleVisible ? rippleAge_ / kRippleDuration : 1.0;
		const double ease = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
		vec4 rippleColor = c.rippleColor;
		rippleColor.w *= (float)(1.0 - t);
		vec2_set(&v, (float)rippleX_, (float)rippleY_);
		gs_effect_set_vec2(pRipplePos_, &v);
		gs_effect_set_float(pRippleRadius_, (float)(c.rippleSize * (0.25 + 0.75 * ease)));
		gs_effect_set_float(pRippleWidth_, 3.0f);
		gs_effect_set_vec4(pRippleColor_, rippleVisible ? &rippleColor : &transparent);

		obs_source_process_filter_end(source_, effect_, width, height);
	}

	void zoomNow()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		int x, y;
		if (InputHook::instance().cursorPosition(x, y))
			pendingClicks_.push_back({x, y});
	}

private:
	void cacheParams()
	{
		pUvScale_ = gs_effect_get_param_by_name(effect_, "uv_scale");
		pUvOffset_ = gs_effect_get_param_by_name(effect_, "uv_offset");
		pSourceSize_ = gs_effect_get_param_by_name(effect_, "source_size");
		pHaloPos_ = gs_effect_get_param_by_name(effect_, "halo_pos");
		pHaloRadius_ = gs_effect_get_param_by_name(effect_, "halo_radius");
		pHaloColor_ = gs_effect_get_param_by_name(effect_, "halo_color");
		pRipplePos_ = gs_effect_get_param_by_name(effect_, "ripple_pos");
		pRippleRadius_ = gs_effect_get_param_by_name(effect_, "ripple_radius");
		pRippleWidth_ = gs_effect_get_param_by_name(effect_, "ripple_width");
		pRippleColor_ = gs_effect_get_param_by_name(effect_, "ripple_color");
	}

	/* Hook thread: only record what happened, the video thread decides. */
	void onKey(const ts_key_event &ev)
	{
		if (!ev.down || ev.kind == TS_KEY_MODIFIER || ev.kind == TS_KEY_IGNORED)
			return;
		std::lock_guard<std::mutex> lock(mutex_);
		pendingTyping_ = true;
	}

	void onMouse(const MouseEvent &ev)
	{
		if (!ev.down || (ev.button != MouseButton::Left && ev.button != MouseButton::Right))
			return;
		std::lock_guard<std::mutex> lock(mutex_);
		if (pendingClicks_.size() < 16)
			pendingClicks_.push_back({ev.x, ev.y});
	}

	static void onToggleHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
	{
		auto *self = static_cast<AutoZoomFilter *>(data);
		if (pressed)
			self->enabled_ = !self->enabled_;
	}

	static void onHoldHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
	{
		static_cast<AutoZoomFilter *>(data)->holdRequested_ = pressed;
	}

	obs_source_t *source_;
	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *pUvScale_ = nullptr, *pUvOffset_ = nullptr, *pSourceSize_ = nullptr;
	gs_eparam_t *pHaloPos_ = nullptr, *pHaloRadius_ = nullptr, *pHaloColor_ = nullptr;
	gs_eparam_t *pRipplePos_ = nullptr, *pRippleRadius_ = nullptr, *pRippleWidth_ = nullptr,
		    *pRippleColor_ = nullptr;

	obs_hotkey_id toggleHotkey_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id holdHotkey_ = OBS_INVALID_HOTKEY_ID;
	int listener_ = 0;

	/* Shared with the hook thread / UI thread. */
	std::mutex mutex_;
	Config config_;
	std::vector<PendingClick> pendingClicks_;
	bool pendingTyping_ = false;
	std::atomic<bool> enabled_{true};
	std::atomic<bool> holdRequested_{false};

	/* Graphics thread only (tick + render). */
	ts_zoom_state state_{};
	Config renderConfig_;
	double cursorX_ = -1.0, cursorY_ = -1.0;
	bool cursorInside_ = false;
	double rippleX_ = 0.0, rippleY_ = 0.0;
	double rippleAge_ = kRippleDuration;
};

/* ------------------------------------------------------------------------ */

const char *getName(void *)
{
	return obs_module_text("AutoZoom.Name");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	return new AutoZoomFilter(settings, source);
}

void destroy(void *data)
{
	delete static_cast<AutoZoomFilter *>(data);
}

void update(void *data, obs_data_t *settings)
{
	static_cast<AutoZoomFilter *>(data)->update(settings);
}

void videoTick(void *data, float seconds)
{
	static_cast<AutoZoomFilter *>(data)->tick(seconds);
}

void videoRender(void *data, gs_effect_t *)
{
	static_cast<AutoZoomFilter *>(data)->render();
}

bool onRegionModeChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool manual = (RegionMode)obs_data_get_int(settings, "region_mode") == RegionMode::Manual;
	obs_property_set_visible(obs_properties_get(props, "monitor"), !manual);
	for (const char *key : {"region_x", "region_y", "region_w", "region_h"})
		obs_property_set_visible(obs_properties_get(props, key), manual);
	return true;
}

bool onTestZoom(obs_properties_t *, obs_property_t *, void *data)
{
	static_cast<AutoZoomFilter *>(data)->zoomNow();
	return false;
}

obs_properties_t *getProperties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	/* Screen region */
	obs_properties_t *region = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(region, "region_mode", obs_module_text("AutoZoom.RegionMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("AutoZoom.RegionMode.Monitor"), (long long)RegionMode::Monitor);
	obs_property_list_add_int(mode, obs_module_text("AutoZoom.RegionMode.Manual"), (long long)RegionMode::Manual);
	obs_property_set_modified_callback(mode, onRegionModeChanged);

	obs_property_t *monitor = obs_properties_add_list(region, "monitor", obs_module_text("AutoZoom.Monitor"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(monitor, obs_module_text("AutoZoom.Monitor.Primary"), "");
	int index = 1;
	for (const Monitor &m : InputHook::instance().monitors()) {
		char label[160];
		snprintf(label, sizeof(label), "%d. %s — %dx%d @ %d,%d%s", index++, m.name.c_str(), m.width, m.height,
			 m.x, m.y, m.primary ? " ★" : "");
		obs_property_list_add_string(monitor, label, monitorKey(m).c_str());
	}

	obs_properties_add_int(region, "region_x", obs_module_text("AutoZoom.Region.X"), -32768, 32768, 1);
	obs_properties_add_int(region, "region_y", obs_module_text("AutoZoom.Region.Y"), -32768, 32768, 1);
	obs_properties_add_int(region, "region_w", obs_module_text("AutoZoom.Region.Width"), 1, 32768, 1);
	obs_properties_add_int(region, "region_h", obs_module_text("AutoZoom.Region.Height"), 1, 32768, 1);
	obs_properties_add_group(props, "group_region", obs_module_text("AutoZoom.Group.Region"), OBS_GROUP_NORMAL,
				 region);

	/* Zoom behavior */
	obs_properties_t *zoom = obs_properties_create();
	obs_properties_add_bool(zoom, "zoom_on_click", obs_module_text("AutoZoom.OnClick"));
	obs_properties_add_bool(zoom, "zoom_on_typing", obs_module_text("AutoZoom.OnTyping"));
	obs_property_t *caret = obs_properties_add_bool(zoom, "use_caret", obs_module_text("AutoZoom.UseCaret"));
	obs_property_set_long_description(caret, obs_module_text("AutoZoom.UseCaret.Description"));
	obs_properties_add_bool(zoom, "follow_cursor", obs_module_text("AutoZoom.FollowCursor"));
	obs_property_t *p =
		obs_properties_add_float_slider(zoom, "zoom_level", obs_module_text("AutoZoom.Level"), 1.1, 4.0, 0.05);
	obs_property_float_set_suffix(p, "x");
	p = obs_properties_add_float_slider(zoom, "hold_time", obs_module_text("AutoZoom.HoldTime"), 0.3, 10.0, 0.1);
	obs_property_float_set_suffix(p, " s");
	obs_properties_add_int_slider(zoom, "speed", obs_module_text("AutoZoom.Speed"), 1, 10, 1);
	p = obs_properties_add_int_slider(zoom, "safe_margin", obs_module_text("AutoZoom.SafeMargin"), 0, 40, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_button2(zoom, "test_zoom", obs_module_text("AutoZoom.Test"), onTestZoom, data);
	obs_properties_add_group(props, "group_zoom", obs_module_text("AutoZoom.Group.Zoom"), OBS_GROUP_NORMAL, zoom);

	/* Smart cursor */
	obs_properties_t *cursor = obs_properties_create();
	obs_properties_add_bool(cursor, "show_ripples", obs_module_text("AutoZoom.Ripples"));
	obs_properties_add_color_alpha(cursor, "ripple_color", obs_module_text("AutoZoom.RippleColor"));
	p = obs_properties_add_int_slider(cursor, "ripple_size", obs_module_text("AutoZoom.RippleSize"), 10, 150, 1);
	obs_property_int_set_suffix(p, " px");
	obs_properties_add_bool(cursor, "show_halo", obs_module_text("AutoZoom.Halo"));
	obs_properties_add_color(cursor, "halo_color", obs_module_text("AutoZoom.HaloColor"));
	p = obs_properties_add_int_slider(cursor, "halo_opacity", obs_module_text("AutoZoom.HaloOpacity"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(cursor, "halo_radius", obs_module_text("AutoZoom.HaloRadius"), 10, 200, 1);
	obs_property_int_set_suffix(p, " px");
	obs_properties_add_group(props, "group_cursor", obs_module_text("AutoZoom.Group.Cursor"), OBS_GROUP_NORMAL,
				 cursor);

	obs_properties_add_text(props, "permissions_info", obs_module_text("Permissions.Info"), OBS_TEXT_INFO);
	return props;
}

void getDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "region_mode", (long long)RegionMode::Monitor);
	obs_data_set_default_string(settings, "monitor", "");
	obs_data_set_default_int(settings, "region_x", 0);
	obs_data_set_default_int(settings, "region_y", 0);
	obs_data_set_default_int(settings, "region_w", 1920);
	obs_data_set_default_int(settings, "region_h", 1080);

	obs_data_set_default_bool(settings, "zoom_on_click", true);
	obs_data_set_default_bool(settings, "zoom_on_typing", true);
	obs_data_set_default_bool(settings, "use_caret", true);
	obs_data_set_default_bool(settings, "follow_cursor", true);
	obs_data_set_default_double(settings, "zoom_level", 2.0);
	obs_data_set_default_double(settings, "hold_time", 2.0);
	obs_data_set_default_int(settings, "speed", 4);
	obs_data_set_default_int(settings, "safe_margin", 20);

	obs_data_set_default_bool(settings, "show_ripples", true);
	obs_data_set_default_int(settings, "ripple_color", 0xE6FFC04A); /* ABGR: soft blue */
	obs_data_set_default_int(settings, "ripple_size", 40);
	obs_data_set_default_bool(settings, "show_halo", false);
	obs_data_set_default_int(settings, "halo_color", 0xFF4AD8FF); /* ABGR: warm yellow */
	obs_data_set_default_int(settings, "halo_opacity", 25);
	obs_data_set_default_int(settings, "halo_radius", 45);
}

} // namespace

void registerAutoZoomFilter()
{
	obs_source_info info = {};
	info.id = kFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = getName;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.video_tick = videoTick;
	info.video_render = videoRender;
	info.get_properties = getProperties;
	info.get_defaults = getDefaults;
	obs_register_source(&info);
}

} // namespace ts

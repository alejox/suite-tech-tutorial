/*
 * Auto-zoom camera logic (platform and OBS independent).
 *
 * Coordinates are normalized to the captured image: (0,0) top-left,
 * (1,1) bottom-right. The camera is described by a zoom factor (>= 1) and a
 * view center. All motion goes through critically damped springs, which gives
 * the smooth "Screen Studio" feel without overshoot and stays stable even if a
 * frame takes much longer than usual.
 */
#pragma once

struct ts_spring {
	double pos;
	double vel;
};

/* Critically damped spring, exact closed-form integration.
 * `omega` is the angular frequency in rad/s (higher = snappier). */
void ts_spring_step(struct ts_spring *s, double target, double omega, double dt);

struct ts_zoom_params {
	double zoom_level;  /* zoom factor when zoomed in, e.g. 2.0 */
	double hold_time;   /* seconds without activity before zooming out */
	double speed;       /* spring angular frequency (rad/s) */
	double safe_margin; /* 0..0.45, fraction of the view kept as a margin
			       before the camera pans to follow the cursor */
	bool follow_cursor; /* pan while zoomed when the cursor nears an edge */
};

struct ts_zoom_state {
	struct ts_spring zoom;
	struct ts_spring cx;
	struct ts_spring cy;
	double target_cx;
	double target_cy;
	double idle_time;
	bool active; /* zoomed in, or zooming in */
	bool manual; /* held by a hotkey: ignores hold_time */
};

void ts_zoom_params_default(struct ts_zoom_params *p);
void ts_zoom_init(struct ts_zoom_state *st);

/* A click or a key press happened at (x, y). */
void ts_zoom_activity(struct ts_zoom_state *st, const struct ts_zoom_params *p, double x, double y);

/* Hold-to-zoom hotkey. */
void ts_zoom_set_manual(struct ts_zoom_state *st, const struct ts_zoom_params *p, bool on, double x, double y);

/* Force zoom-out (e.g. when auto-zoom gets disabled). */
void ts_zoom_reset(struct ts_zoom_state *st);

void ts_zoom_tick(struct ts_zoom_state *st, const struct ts_zoom_params *p, double dt, bool cursor_valid,
		  double cursor_x, double cursor_y);

/* Current camera, clamped so the view never leaves the image. */
void ts_zoom_get_view(const struct ts_zoom_state *st, double *scale, double *cx, double *cy);

/* True when the camera is (practically) showing the whole image and is
 * not about to move, so rendering can be skipped entirely. */
bool ts_zoom_is_idle(const struct ts_zoom_state *st);

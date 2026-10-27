#include "zoom-logic.h"

#include <cmath>

void ts_spring_step(struct ts_spring *s, double target, double omega, double dt)
{
	if (dt <= 0.0)
		return;
	if (omega <= 0.0) {
		s->pos = target;
		s->vel = 0.0;
		return;
	}

	/* y(t) = (y0 + (v0 + w*y0) t) e^(-w t)
	 * v(t) = (v0 - w (v0 + w*y0) t) e^(-w t) */
	const double y0 = s->pos - target;
	const double v0 = s->vel;
	const double c = v0 + omega * y0;
	const double e = exp(-omega * dt);

	s->pos = target + (y0 + c * dt) * e;
	s->vel = (v0 - omega * c * dt) * e;
}

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static double clamp_center(double c, double zoom)
{
	if (zoom <= 1.0)
		return 0.5;
	const double half = 0.5 / zoom;
	return clampd(c, half, 1.0 - half);
}

void ts_zoom_params_default(struct ts_zoom_params *p)
{
	p->zoom_level = 2.0;
	p->hold_time = 2.0;
	p->speed = 8.0;
	p->safe_margin = 0.2;
	p->follow_cursor = true;
}

void ts_zoom_init(struct ts_zoom_state *st)
{
	st->zoom.pos = 1.0;
	st->zoom.vel = 0.0;
	st->cx.pos = st->cy.pos = 0.5;
	st->cx.vel = st->cy.vel = 0.0;
	st->target_cx = st->target_cy = 0.5;
	st->idle_time = 0.0;
	st->active = false;
	st->manual = false;
}

static double zoom_level(const struct ts_zoom_params *p)
{
	return p->zoom_level < 1.0 ? 1.0 : p->zoom_level;
}

/* Pans the target view (at the target zoom) the minimum amount needed so
 * that (x, y) sits inside its safe area. */
static void keep_visible(struct ts_zoom_state *st, const struct ts_zoom_params *p, double x, double y)
{
	const double zoom = zoom_level(p);
	const double half = 0.5 / zoom;
	const double margin = clampd(p->safe_margin, 0.0, 0.45);
	const double lim = half * (1.0 - 2.0 * margin);

	if (x - st->target_cx > lim)
		st->target_cx = x - lim;
	else if (st->target_cx - x > lim)
		st->target_cx = x + lim;

	if (y - st->target_cy > lim)
		st->target_cy = y - lim;
	else if (st->target_cy - y > lim)
		st->target_cy = y + lim;

	st->target_cx = clamp_center(st->target_cx, zoom);
	st->target_cy = clamp_center(st->target_cy, zoom);
}

static void zoom_in_at(struct ts_zoom_state *st, const struct ts_zoom_params *p, double x, double y)
{
	const double zoom = zoom_level(p);

	x = clampd(x, 0.0, 1.0);
	y = clampd(y, 0.0, 1.0);

	if (!st->active) {
		st->active = true;
		st->target_cx = clamp_center(x, zoom);
		st->target_cy = clamp_center(y, zoom);

		/* Fully zoomed out the center is invisible: jump straight to the
		 * point so the camera dives into it instead of sliding sideways. */
		if (st->zoom.pos < 1.02) {
			st->cx.pos = st->target_cx;
			st->cy.pos = st->target_cy;
			st->cx.vel = st->cy.vel = 0.0;
		}
	} else {
		keep_visible(st, p, x, y);
	}
}

void ts_zoom_activity(struct ts_zoom_state *st, const struct ts_zoom_params *p, double x, double y)
{
	st->idle_time = 0.0;
	zoom_in_at(st, p, x, y);
}

void ts_zoom_set_manual(struct ts_zoom_state *st, const struct ts_zoom_params *p, bool on, double x, double y)
{
	st->manual = on;
	st->idle_time = 0.0;
	if (on)
		zoom_in_at(st, p, x, y);
}

void ts_zoom_reset(struct ts_zoom_state *st)
{
	st->active = false;
	st->manual = false;
}

void ts_zoom_tick(struct ts_zoom_state *st, const struct ts_zoom_params *p, double dt, bool cursor_valid,
		  double cursor_x, double cursor_y)
{
	if (dt <= 0.0)
		return;

	st->idle_time += dt;
	if (st->active && !st->manual && st->idle_time >= p->hold_time)
		st->active = false;

	if (st->active && p->follow_cursor && cursor_valid && cursor_x >= 0.0 && cursor_x <= 1.0 && cursor_y >= 0.0 &&
	    cursor_y <= 1.0)
		keep_visible(st, p, cursor_x, cursor_y);

	const double target_zoom = st->active ? zoom_level(p) : 1.0;

	ts_spring_step(&st->zoom, target_zoom, p->speed, dt);
	ts_spring_step(&st->cx, st->target_cx, p->speed, dt);
	ts_spring_step(&st->cy, st->target_cy, p->speed, dt);

	if (st->zoom.pos < 1.0) {
		st->zoom.pos = 1.0;
		if (st->zoom.vel < 0.0)
			st->zoom.vel = 0.0;
	}
}

void ts_zoom_get_view(const struct ts_zoom_state *st, double *scale, double *cx, double *cy)
{
	const double zoom = st->zoom.pos < 1.0 ? 1.0 : st->zoom.pos;
	*scale = zoom;
	*cx = clamp_center(st->cx.pos, zoom);
	*cy = clamp_center(st->cy.pos, zoom);
}

bool ts_zoom_is_idle(const struct ts_zoom_state *st)
{
	return !st->active && st->zoom.pos < 1.0005 && fabs(st->zoom.vel) < 1e-3;
}

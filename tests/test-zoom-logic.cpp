#include "core/zoom-logic.h"

#include <cmath>
#include <cstdio>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

#define NEAR(a, b, eps) (fabs((a) - (b)) < (eps))

static void run(struct ts_zoom_state *st, const struct ts_zoom_params *p, double seconds, double cx, double cy)
{
	for (double t = 0; t < seconds; t += 1.0 / 60.0)
		ts_zoom_tick(st, p, 1.0 / 60.0, true, cx, cy);
}

int main(void)
{
	/* Spring: converges, no overshoot, stable with huge dt. */
	{
		struct ts_spring s = {0.0, 0.0};
		double max = 0.0;
		for (int i = 0; i < 600; i++) {
			ts_spring_step(&s, 1.0, 10.0, 1.0 / 60.0);
			if (s.pos > max)
				max = s.pos;
		}
		CHECK(NEAR(s.pos, 1.0, 1e-6));
		CHECK(max <= 1.0 + 1e-9);

		struct ts_spring big = {0.0, 0.0};
		ts_spring_step(&big, 1.0, 10.0, 100.0);
		CHECK(NEAR(big.pos, 1.0, 1e-9));
		CHECK(std::isfinite(big.vel));
	}

	struct ts_zoom_params p;
	ts_zoom_params_default(&p);
	p.hold_time = 1.0;

	/* Idle at start. */
	{
		struct ts_zoom_state st;
		ts_zoom_init(&st);
		CHECK(ts_zoom_is_idle(&st));
		run(&st, &p, 1.0, 0.5, 0.5);
		CHECK(ts_zoom_is_idle(&st));
	}

	/* Click zooms in on the point, then zooms out after hold_time. */
	{
		struct ts_zoom_state st;
		double scale, cx, cy;
		ts_zoom_init(&st);
		ts_zoom_activity(&st, &p, 0.4, 0.6);
		run(&st, &p, 0.9, 0.4, 0.6);
		ts_zoom_get_view(&st, &scale, &cx, &cy);
		CHECK(scale > 1.9 && scale <= 2.0 + 1e-6);
		CHECK(NEAR(cx, 0.4, 1e-3) && NEAR(cy, 0.6, 1e-3));
		CHECK(!ts_zoom_is_idle(&st));

		run(&st, &p, 3.0, 0.4, 0.6);
		ts_zoom_get_view(&st, &scale, &cx, &cy);
		CHECK(NEAR(scale, 1.0, 1e-3));
		CHECK(ts_zoom_is_idle(&st));
	}

	/* Clicking near a corner never shows outside the image. */
	{
		struct ts_zoom_state st;
		double scale, cx, cy;
		ts_zoom_init(&st);
		for (int i = 0; i < 50; i++) {
			if (i == 0)
				ts_zoom_activity(&st, &p, 0.99, 0.01);
			ts_zoom_tick(&st, &p, 1.0 / 60.0, true, 0.99, 0.01);
			ts_zoom_get_view(&st, &scale, &cx, &cy);
			CHECK(cx + 0.5 / scale <= 1.0 + 1e-9);
			CHECK(cy - 0.5 / scale >= -1e-9);
		}
		CHECK(NEAR(cx, 0.75, 1e-2) && NEAR(cy, 0.25, 1e-2));
	}

	/* Continuous activity keeps it zoomed; follow pans toward the cursor. */
	{
		struct ts_zoom_params lp = p;
		lp.hold_time = 10.0;
		struct ts_zoom_state st;
		double scale, cx, cy;
		ts_zoom_init(&st);
		ts_zoom_activity(&st, &lp, 0.3, 0.5);
		for (int i = 0; i < 5; i++) {
			run(&st, &lp, 0.5, 0.3, 0.5);
			ts_zoom_activity(&st, &lp, 0.3, 0.5);
		}
		ts_zoom_get_view(&st, &scale, &cx, &cy);
		CHECK(scale > 1.99);

		/* Cursor inside the safe area: no pan. */
		run(&st, &lp, 0.5, 0.35, 0.55);
		ts_zoom_get_view(&st, &scale, &cx, &cy);
		CHECK(NEAR(cx, 0.3, 1e-3));

		/* Cursor beyond the safe area: pans just enough. */
		run(&st, &lp, 2.0, 0.6, 0.5);
		ts_zoom_get_view(&st, &scale, &cx, &cy);
		/* half view = 0.25, margin 20% -> limit 0.15 */
		CHECK(NEAR(cx, 0.45, 2e-3));
	}

	/* Manual hold ignores hold_time. */
	{
		struct ts_zoom_state st;
		ts_zoom_init(&st);
		ts_zoom_set_manual(&st, &p, true, 0.5, 0.5);
		run(&st, &p, 5.0, 0.5, 0.5);
		CHECK(st.active);
		ts_zoom_set_manual(&st, &p, false, 0.5, 0.5);
		run(&st, &p, 3.0, 0.5, 0.5);
		CHECK(ts_zoom_is_idle(&st));
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("zoom logic: all tests passed\n");
	return 0;
}

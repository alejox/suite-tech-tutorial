#include "core/shortcut-filter.h"

#include <cstdio>
#include <cstring>

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static struct ts_key_event key(enum ts_key_kind kind, uint32_t mods, const char *name)
{
	struct ts_key_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.down = true;
	ev.kind = kind;
	ev.mods = mods;
	snprintf(ev.name, sizeof(ev.name), "%s", name);
	return ev;
}

static const char *shown(const struct ts_key_event *ev, const struct ts_shortcut_opts *o)
{
	static char buf[128];
	struct ts_shortcut sc;
	if (!ts_shortcut_from_event(ev, o, &sc))
		return NULL;
	ts_shortcut_to_string(&sc, buf, sizeof(buf));
	return buf;
}

#define EXPECT_SHOWN(ev, opts, str)                                          \
	do {                                                                 \
		struct ts_key_event e_ = (ev);                               \
		const char *s_ = shown(&e_, (opts));                         \
		if (!s_ || strcmp(s_, (str)) != 0) {                         \
			fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, (str), \
				s_ ? s_ : "(hidden)");                       \
			failures++;                                          \
		}                                                            \
	} while (0)

#define EXPECT_HIDDEN(ev, opts)                                              \
	do {                                                                 \
		struct ts_key_event e_ = (ev);                               \
		const char *s_ = shown(&e_, (opts));                         \
		if (s_) {                                                    \
			fprintf(stderr, "%s:%d: expected hidden, got \"%s\"\n", __FILE__, __LINE__, s_); \
			failures++;                                          \
		}                                                            \
	} while (0)

int main(void)
{
	struct ts_shortcut_opts pc, mac;
	ts_shortcut_opts_default(&pc, false);
	ts_shortcut_opts_default(&mac, true);

	/* Plain typing is never shown: this is the password guarantee. */
	EXPECT_HIDDEN(key(TS_KEY_PRINTABLE, 0, "A"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_PRINTABLE, TS_MOD_SHIFT, "A"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_PRINTABLE, 0, "Space"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_PRINTABLE, TS_MOD_SHIFT, "1"), &mac);

	/* AltGr / Option produce characters (@, #, €) on many layouts. */
	EXPECT_HIDDEN(key(TS_KEY_PRINTABLE, TS_MOD_ALTGR, "2"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_PRINTABLE, TS_MOD_ALTGR | TS_MOD_ALT, "Q"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_PRINTABLE, TS_MOD_ALT, "2"), &mac);
	EXPECT_HIDDEN(key(TS_KEY_PRINTABLE, TS_MOD_ALT | TS_MOD_SHIFT, "2"), &mac);

	/* Real shortcuts, in canonical order. */
	EXPECT_SHOWN(key(TS_KEY_PRINTABLE, TS_MOD_CTRL | TS_MOD_SHIFT, "P"), &pc, "Ctrl + Shift + P");
	EXPECT_SHOWN(key(TS_KEY_PRINTABLE, TS_MOD_META | TS_MOD_SHIFT, "P"), &mac, "Shift + Meta + P");
	EXPECT_SHOWN(key(TS_KEY_PRINTABLE, TS_MOD_ALT, "F"), &pc, "Alt + F");
	EXPECT_SHOWN(key(TS_KEY_PRINTABLE, TS_MOD_CTRL, "Space"), &pc, "Ctrl + Space");
	EXPECT_SHOWN(key(TS_KEY_PRINTABLE, TS_MOD_ALTGR | TS_MOD_CTRL, "C"), &pc, "Ctrl + C");

	/* Option + non-printable is safe on macOS (word navigation). */
	EXPECT_SHOWN(key(TS_KEY_NAVIGATION, TS_MOD_ALT, "Left"), &mac, "Alt + Left");

	/* Special keys without modifiers follow the options. */
	EXPECT_SHOWN(key(TS_KEY_FUNCTION, 0, "F5"), &pc, "F5");
	EXPECT_SHOWN(key(TS_KEY_ESCAPE, 0, "Esc"), &pc, "Esc");
	EXPECT_HIDDEN(key(TS_KEY_EDITING, 0, "Enter"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_EDITING, 0, "Backspace"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_NAVIGATION, 0, "Up"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_EDITING, TS_MOD_SHIFT, "Tab"), &pc);
	EXPECT_SHOWN(key(TS_KEY_FUNCTION, TS_MOD_SHIFT, "F5"), &pc, "Shift + F5");
	EXPECT_SHOWN(key(TS_KEY_EDITING, TS_MOD_CTRL, "Enter"), &pc, "Ctrl + Enter");

	struct ts_shortcut_opts all = pc;
	all.show_editing_keys = true;
	all.show_navigation_keys = true;
	all.show_function_keys = false;
	EXPECT_SHOWN(key(TS_KEY_EDITING, 0, "Enter"), &all, "Enter");
	EXPECT_SHOWN(key(TS_KEY_NAVIGATION, TS_MOD_SHIFT, "Up"), &all, "Shift + Up");
	EXPECT_HIDDEN(key(TS_KEY_FUNCTION, 0, "F5"), &all);

	/* Modifier-only, ignored keys, key-ups and auto-repeat. */
	EXPECT_HIDDEN(key(TS_KEY_MODIFIER, TS_MOD_CTRL, "Shift"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_IGNORED, TS_MOD_CTRL, "CapsLock"), &pc);
	{
		struct ts_key_event up = key(TS_KEY_PRINTABLE, TS_MOD_CTRL, "C");
		up.down = false;
		EXPECT_HIDDEN(up, &pc);
		struct ts_key_event rep = key(TS_KEY_PRINTABLE, TS_MOD_CTRL, "C");
		rep.repeat = true;
		EXPECT_HIDDEN(rep, &pc);
	}

	/* Mouse combos. */
	EXPECT_SHOWN(key(TS_KEY_MOUSE, TS_MOD_META, "Click"), &mac, "Meta + Click");
	EXPECT_HIDDEN(key(TS_KEY_MOUSE, 0, "Click"), &pc);
	EXPECT_HIDDEN(key(TS_KEY_MOUSE, TS_MOD_SHIFT, "Click"), &pc);

	/* JSON escaping. */
	{
		struct ts_shortcut sc = {};
		char json[256];
		sc.count = 2;
		strcpy(sc.keys[0], "Ctrl");
		strcpy(sc.keys[1], "\"\\");
		CHECK(ts_shortcut_to_json(&sc, "windows", 7, json, sizeof(json)));
		CHECK(strcmp(json, "{\"keys\":[\"Ctrl\",\"\\\"\\\\\"],\"platform\":\"windows\",\"seq\":7}") == 0);
		CHECK(!ts_shortcut_to_json(&sc, "windows", 7, json, 10));
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("shortcut filter: all tests passed\n");
	return 0;
}

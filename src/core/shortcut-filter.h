/*
 * Shortcut filter: decides which key presses are safe to show on stream.
 *
 * The rule of thumb is "show shortcuts, never show text". A key press is only
 * turned into an on-screen combo when it cannot be part of something the user
 * is typing (passwords, tokens, chat messages...). This module is plain C++ with
 * no OBS or OS dependencies so it can be unit tested.
 */
#pragma once

#include <cstddef>
#include <cstdint>

/* Modifier bits. ALTGR is reported separately from ALT because AltGr (and
 * macOS Option) is used to type characters such as @ # € on many layouts. */
enum ts_modifier {
	TS_MOD_SHIFT = 1 << 0,
	TS_MOD_CTRL = 1 << 1,
	TS_MOD_ALT = 1 << 2,
	TS_MOD_META = 1 << 3, /* Cmd on macOS, Win on Windows, Super on Linux */
	TS_MOD_ALTGR = 1 << 4,
};

enum ts_key_kind {
	TS_KEY_IGNORED = 0, /* caps lock, num lock, unknown keys... never shown */
	TS_KEY_PRINTABLE,   /* letters, digits, punctuation, space */
	TS_KEY_MODIFIER,    /* shift, ctrl, alt, meta, altgr */
	TS_KEY_FUNCTION,    /* F1..F24 */
	TS_KEY_ESCAPE,
	TS_KEY_EDITING,    /* enter, tab, backspace, delete, insert */
	TS_KEY_NAVIGATION, /* arrows, home, end, page up/down */
	TS_KEY_MOUSE,      /* synthetic: mouse click reported as a key */
};

#define TS_KEY_NAME_MAX 32

struct ts_key_event {
	bool down;
	bool repeat; /* OS auto-repeat */
	enum ts_key_kind kind;
	/* Authoritative modifier state at the time of the event, *excluding*
	 * the key itself if it is a modifier. Coming from the OS on every event
	 * means a lost key-up can never leave a modifier "stuck" (which would
	 * otherwise leak plain typing as fake shortcuts). */
	uint32_t mods;
	char name[TS_KEY_NAME_MAX]; /* UTF-8 label: "P", "F5", "Enter", "Up"... */
};

struct ts_shortcut_opts {
	/* Alt/Option + printable key counts as a shortcut. Off by default on
	 * macOS where Option+key types characters. */
	bool alt_is_command;
	bool show_function_keys; /* F1..F24 without modifiers */
	bool show_escape;
	bool show_editing_keys;    /* Enter, Tab, Backspace... without modifiers */
	bool show_navigation_keys; /* arrows, Home, End... without modifiers */
	bool show_mouse_combos;    /* Ctrl + Click, Cmd + Click... */
};

#define TS_SHORTCUT_MAX_KEYS 6

struct ts_shortcut {
	size_t count;
	char keys[TS_SHORTCUT_MAX_KEYS][TS_KEY_NAME_MAX];
};

void ts_shortcut_opts_default(struct ts_shortcut_opts *opts, bool is_macos);

/* Returns true and fills `out` when `ev` should be shown on screen. */
bool ts_shortcut_from_event(const struct ts_key_event *ev, const struct ts_shortcut_opts *opts,
			    struct ts_shortcut *out);

/* "Ctrl + Shift + P" style label (mostly for logging/tests). */
void ts_shortcut_to_string(const struct ts_shortcut *sc, char *buf, size_t size);

/* Writes `{"keys":["Ctrl","P"]}`-style JSON, escaping key names. Returns
 * false if the buffer is too small. */
bool ts_shortcut_to_json(const struct ts_shortcut *sc, const char *platform, uint64_t seq, char *buf, size_t size);

#include "shortcut-filter.h"

#include <cstdio>
#include <cstring>

void ts_shortcut_opts_default(struct ts_shortcut_opts *opts, bool is_macos)
{
	opts->alt_is_command = !is_macos;
	opts->show_function_keys = true;
	opts->show_escape = true;
	opts->show_editing_keys = false;
	opts->show_navigation_keys = false;
	opts->show_mouse_combos = true;
}

static void push_key(struct ts_shortcut *out, const char *name)
{
	if (out->count >= TS_SHORTCUT_MAX_KEYS)
		return;
	snprintf(out->keys[out->count], TS_KEY_NAME_MAX, "%s", name);
	out->count++;
}

static bool shown_without_modifiers(enum ts_key_kind kind, const struct ts_shortcut_opts *opts)
{
	switch (kind) {
	case TS_KEY_FUNCTION:
		return opts->show_function_keys;
	case TS_KEY_ESCAPE:
		return opts->show_escape;
	case TS_KEY_EDITING:
		return opts->show_editing_keys;
	case TS_KEY_NAVIGATION:
		return opts->show_navigation_keys;
	default:
		return false;
	}
}

bool ts_shortcut_from_event(const struct ts_key_event *ev, const struct ts_shortcut_opts *opts, struct ts_shortcut *out)
{
	out->count = 0;

	if (!ev->down || ev->repeat || !ev->name[0])
		return false;
	if (ev->kind == TS_KEY_IGNORED || ev->kind == TS_KEY_MODIFIER)
		return false;

	uint32_t mods = ev->mods;

	/* AltGr is a text-entry modifier (@, #, €, [...]). Anything typed with
	 * it is text unless an unambiguous command modifier is also held. */
	if (mods & TS_MOD_ALTGR)
		mods &= ~(uint32_t)(TS_MOD_ALT | TS_MOD_ALTGR);

	const bool printable = ev->kind == TS_KEY_PRINTABLE;
	bool command = (mods & (TS_MOD_CTRL | TS_MOD_META)) != 0;

	if (!command && (mods & TS_MOD_ALT))
		command = opts->alt_is_command || !printable;

	if (ev->kind == TS_KEY_MOUSE) {
		if (!opts->show_mouse_combos || !command)
			return false;
	} else if (printable) {
		/* Shift + letter is just an uppercase letter: never shown. */
		if (!command)
			return false;
	} else if (!command && !(mods & TS_MOD_SHIFT)) {
		if (!shown_without_modifiers(ev->kind, opts))
			return false;
	} else if (!command) {
		/* Shift + non-printable (Shift+Tab, Shift+F5, Shift+Arrow).
		 * Safe, but respect the per-category toggles. */
		if (!shown_without_modifiers(ev->kind, opts) && ev->kind != TS_KEY_FUNCTION)
			return false;
	}

	/* Canonical order used by most OS shortcut hints. */
	if (mods & TS_MOD_CTRL)
		push_key(out, "Ctrl");
	if (mods & TS_MOD_ALT)
		push_key(out, "Alt");
	if (mods & TS_MOD_SHIFT)
		push_key(out, "Shift");
	if (mods & TS_MOD_META)
		push_key(out, "Meta");
	push_key(out, ev->name);
	return true;
}

void ts_shortcut_to_string(const struct ts_shortcut *sc, char *buf, size_t size)
{
	size_t len = 0;
	if (!size)
		return;
	buf[0] = 0;
	for (size_t i = 0; i < sc->count && len < size; i++) {
		int n = snprintf(buf + len, size - len, "%s%s", i ? " + " : "", sc->keys[i]);
		if (n < 0)
			break;
		len += (size_t)n;
	}
}

static bool append(char *buf, size_t size, size_t *len, const char *s, size_t n)
{
	if (*len + n + 1 > size)
		return false;
	memcpy(buf + *len, s, n);
	*len += n;
	buf[*len] = 0;
	return true;
}

static bool append_json_string(char *buf, size_t size, size_t *len, const char *s)
{
	if (!append(buf, size, len, "\"", 1))
		return false;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		char esc[8];
		if (*p == '"' || *p == '\\') {
			esc[0] = '\\';
			esc[1] = (char)*p;
			if (!append(buf, size, len, esc, 2))
				return false;
		} else if (*p < 0x20) {
			snprintf(esc, sizeof(esc), "\\u%04x", *p);
			if (!append(buf, size, len, esc, 6))
				return false;
		} else if (!append(buf, size, len, (const char *)p, 1)) {
			return false;
		}
	}
	return append(buf, size, len, "\"", 1);
}

bool ts_shortcut_to_json(const struct ts_shortcut *sc, const char *platform, uint64_t seq, char *buf, size_t size)
{
	size_t len = 0;
	char tmp[64];

	if (!size)
		return false;
	buf[0] = 0;

	if (!append(buf, size, &len, "{\"keys\":[", 9))
		return false;
	for (size_t i = 0; i < sc->count; i++) {
		if (i && !append(buf, size, &len, ",", 1))
			return false;
		if (!append_json_string(buf, size, &len, sc->keys[i]))
			return false;
	}
	if (!append(buf, size, &len, "],\"platform\":", 13))
		return false;
	if (!append_json_string(buf, size, &len, platform ? platform : "unknown"))
		return false;
	int n = snprintf(tmp, sizeof(tmp), ",\"seq\":%llu}", (unsigned long long)seq);
	return n > 0 && append(buf, size, &len, tmp, (size_t)n);
}

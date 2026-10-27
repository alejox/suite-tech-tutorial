// Turns the normalized key names sent by the native plugin
// ("Ctrl", "Shift", "Meta", "P", "Up"...) into what is drawn on the keycaps.

const MODIFIERS = new Set(['Ctrl', 'Alt', 'Shift', 'Meta'])

const SYMBOLS = {
  Ctrl: '⌃',
  Alt: '⌥',
  Shift: '⇧',
  Enter: '↵',
  Tab: '⇥',
  Backspace: '⌫',
  Delete: '⌦',
  Esc: '⎋',
  Space: '␣',
  PageUp: '⇞',
  PageDown: '⇟',
  Home: '↖',
  End: '↘'
}

// Arrows read better as arrows in every style.
const ALWAYS = { Up: '↑', Down: '↓', Left: '←', Right: '→' }

const TEXT = {
  macos: { Alt: 'Option', Meta: 'Cmd', Ctrl: 'Control' },
  windows: { Meta: 'Win' },
  linux: { Meta: 'Super' }
}

export function useSymbols(style, platform) {
  if (style === 'symbols') return true
  if (style === 'text') return false
  return platform === 'macos'
}

export function isModifier(name) {
  return MODIFIERS.has(name)
}

export function keyLabel(name, { symbols, platform }) {
  if (ALWAYS[name]) return ALWAYS[name]
  if (symbols) {
    if (name === 'Meta') return platform === 'macos' ? '⌘' : platform === 'windows' ? '⊞' : '◆'
    if (SYMBOLS[name]) return SYMBOLS[name]
  }
  return TEXT[platform]?.[name] ?? name
}

// macOS orders modifiers ⌃ ⌥ ⇧ ⌘; the plugin already sends Ctrl, Alt, Shift,
// Meta which matches, so we only need a stable identity for de-duplication.
export function comboId(keys) {
  return keys.join('+')
}

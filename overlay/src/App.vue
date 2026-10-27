<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { comboId, isModifier, keyLabel, useSymbols } from './keys.js'

// Settings arrive from the native plugin as CSS custom properties
// (injected through the browser source "css" field).
const config = reactive({
  theme: 'glass',
  position: 'bottom-center',
  keyStyle: 'auto',
  accent: '#4ac0ff',
  fontSize: 30,
  duration: 1800,
  max: 4,
  platform: detectPlatform()
})

const items = ref([])
let uid = 0
let timer = 0
let configTimer = 0
let demoTimer = 0

function detectPlatform() {
  const p = (navigator.userAgentData?.platform || navigator.platform || '').toLowerCase()
  if (p.includes('mac')) return 'macos'
  if (p.includes('win')) return 'windows'
  return 'linux'
}

function readConfig() {
  const css = getComputedStyle(document.documentElement)
  const str = (name, fallback) => css.getPropertyValue(name).trim() || fallback
  const num = (name, fallback) => {
    const v = parseFloat(str(name, ''))
    return Number.isFinite(v) ? v : fallback
  }
  config.theme = str('--ts-theme', config.theme)
  config.position = str('--ts-position', config.position)
  config.keyStyle = str('--ts-key-style', config.keyStyle)
  config.accent = str('--ts-accent', config.accent)
  config.fontSize = num('--ts-font-size', config.fontSize)
  config.duration = num('--ts-duration', config.duration)
  config.max = Math.max(1, Math.round(num('--ts-max', config.max)))
  config.platform = str('--ts-platform', config.platform)
}

const symbols = computed(() => useSymbols(config.keyStyle, config.platform))

function label(name) {
  return keyLabel(name, { symbols: symbols.value, platform: config.platform })
}

function show(keys) {
  if (!Array.isArray(keys) || keys.length === 0) return
  const id = comboId(keys)
  const now = performance.now()
  const last = items.value[items.value.length - 1]

  // Same shortcut again (e.g. Cmd+Z Cmd+Z Cmd+Z): bump a counter instead of
  // stacking duplicates.
  if (last && last.id === id && last.expires > now) {
    last.count++
    last.expires = now + config.duration
    last.bump++
    return
  }

  items.value.push({ uid: ++uid, id, keys, count: 1, bump: 0, expires: now + config.duration })
  while (items.value.length > config.max) items.value.shift()
}

function prune() {
  const now = performance.now()
  if (items.value.some((i) => i.expires <= now)) {
    items.value = items.value.filter((i) => i.expires > now)
  }
}

function onShortcut(event) {
  const detail = event.detail || {}
  if (detail.platform) config.platform = detail.platform
  show(detail.keys)
}

// Outside OBS (opening the file in a browser, `npm run dev`) show a demo
// loop so the design can be worked on without the plugin.
const DEMO = [
  ['Meta', 'Shift', 'P'],
  ['Ctrl', 'C'],
  ['Ctrl', 'V'],
  ['Meta', 'Z'],
  ['Meta', 'Z'],
  ['Alt', 'Shift', 'F'],
  ['F5'],
  ['Ctrl', 'Click'],
  ['Meta', 'Left']
]

onMounted(() => {
  readConfig()
  window.addEventListener('techsuite:shortcut', onShortcut)
  timer = window.setInterval(prune, 100)
  configTimer = window.setInterval(readConfig, 1000)

  if (!window.obsstudio) {
    let i = 0
    demoTimer = window.setInterval(() => show(DEMO[i++ % DEMO.length]), 900)
  }
})

onBeforeUnmount(() => {
  window.removeEventListener('techsuite:shortcut', onShortcut)
  window.clearInterval(timer)
  window.clearInterval(configTimer)
  window.clearInterval(demoTimer)
})

const stageClass = computed(() => [`theme-${config.theme}`, `pos-${config.position}`])
const stageStyle = computed(() => ({
  '--accent': config.accent,
  '--fs': `${config.fontSize}px`
}))
</script>

<template>
  <div class="stage" :class="stageClass" :style="stageStyle">
    <TransitionGroup name="combo" tag="div" class="stack">
      <div v-for="item in items" :key="item.uid" class="combo">
        <div :key="item.bump" class="keys" :class="{ bump: item.bump > 0 }">
          <template v-for="(k, i) in item.keys" :key="i">
            <span v-if="i > 0 && !symbols" class="plus">+</span>
            <kbd class="key" :class="{ mod: isModifier(k), wide: label(k).length > 1 }">{{ label(k) }}</kbd>
          </template>
        </div>
        <span v-if="item.count > 1" :key="'c' + item.count" class="count">×{{ item.count }}</span>
      </div>
    </TransitionGroup>
  </div>
</template>

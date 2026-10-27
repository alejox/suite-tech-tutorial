import { test } from 'node:test'
import assert from 'node:assert/strict'
import { keyLabel, useSymbols, comboId, isModifier } from './keys.js'

test('auto style uses symbols only on macOS', () => {
  assert.equal(useSymbols('auto', 'macos'), true)
  assert.equal(useSymbols('auto', 'windows'), false)
  assert.equal(useSymbols('symbols', 'linux'), true)
  assert.equal(useSymbols('text', 'macos'), false)
})

test('labels', () => {
  const mac = { symbols: true, platform: 'macos' }
  const win = { symbols: false, platform: 'windows' }
  assert.equal(keyLabel('Meta', mac), '⌘')
  assert.equal(keyLabel('Shift', mac), '⇧')
  assert.equal(keyLabel('P', mac), 'P')
  assert.equal(keyLabel('Meta', win), 'Win')
  assert.equal(keyLabel('Ctrl', win), 'Ctrl')
  assert.equal(keyLabel('Up', win), '↑')
  assert.equal(keyLabel('Alt', { symbols: false, platform: 'macos' }), 'Option')
})

test('helpers', () => {
  assert.equal(comboId(['Ctrl', 'Shift', 'P']), 'Ctrl+Shift+P')
  assert.equal(isModifier('Shift'), true)
  assert.equal(isModifier('P'), false)
})

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { viteSingleFile } from 'vite-plugin-singlefile'

// Everything (JS + CSS) is inlined into one index.html so the OBS browser
// source can load it from disk with no server and no network access.
export default defineConfig({
  plugins: [vue(), viteSingleFile()],
  base: './',
  build: {
    outDir: '../data/overlay',
    emptyOutDir: true,
    target: 'es2020'
  }
})

/* 壁纸层：全窗背景 + 模糊/暗角，由 main.ts applyWallpaper 驱动 */

import type { WallpaperState } from '../../../shared/types'

let layer: HTMLElement | null = null
let dimEl: HTMLElement | null = null
let currentState: WallpaperState | null = null

function ensureLayer(): { layer: HTMLElement; dim: HTMLElement } {
  if (layer && dimEl) return { layer, dim: dimEl }
  layer = document.createElement('div')
  layer.id = 'wallpaper'
  layer.setAttribute('aria-hidden', 'true')
  dimEl = document.createElement('div')
  dimEl.className = 'wallpaper-dim'
  layer.append(dimEl)
  document.body.prepend(layer)
  return { layer, dim: dimEl }
}

/** 把壁纸状态应用到 DOM / CSS 变量 */
export function applyWallpaper(state: WallpaperState): void {
  currentState = state
  const { layer: wall, dim } = ensureLayer()
  const { prefs, hasImage } = state
  const dataUrl = document.documentElement.dataset.theme === 'light'
    ? state.dataUrlLight
    : state.dataUrlDark
  document.documentElement.dataset.wallpaper = hasImage && dataUrl ? 'on' : 'off'
  document.documentElement.style.setProperty('--wall-blur', `${prefs.blur}px`)
  document.documentElement.style.setProperty('--wall-dim', `${prefs.dim / 100}`)
  document.documentElement.style.setProperty('--wall-panel-opacity', `${prefs.opacity / 100}`)

  if (hasImage && dataUrl) {
    wall.style.backgroundImage = `url("${dataUrl}")`
    wall.classList.add('visible')
  } else {
    wall.style.backgroundImage = ''
    wall.classList.remove('visible')
  }
  dim.style.setProperty('--wall-dim', `${prefs.dim / 100}`)
}

/** 主题切换时复用当前状态，自动换用 Light / Dark 对应背景。 */
export function refreshWallpaperTheme(): void {
  if (currentState) applyWallpaper(currentState)
}

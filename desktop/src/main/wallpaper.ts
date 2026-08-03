import fs from 'node:fs'
import path from 'node:path'
import { app, dialog, BrowserWindow } from 'electron'
import type { WallpaperPrefs, WallpaperState } from '../shared/types'
import { readPrefs, writePrefs } from './prefs'

const MAX_BYTES = 15 * 1024 * 1024
const ALLOWED_EXT = new Set(['.jpg', '.jpeg', '.png', '.webp', '.bmp'])

export const DEFAULT_WALLPAPER_PREFS: WallpaperPrefs = {
  blur: 0,
  dim: 32,
  opacity: 35,
}

function wallpaperDir(): string {
  const dir = path.join(app.getPath('userData'), 'wallpaper')
  fs.mkdirSync(dir, { recursive: true })
  return dir
}

function currentImagePath(): string | null {
  let dir: string
  try { dir = wallpaperDir() } catch { return null }
  let names: string[]
  try { names = fs.readdirSync(dir) } catch { return null }
  for (const name of names) {
    if (name.startsWith('current.') && ALLOWED_EXT.has(path.extname(name).toLowerCase())) {
      return path.join(dir, name)
    }
  }
  return null
}

function mimeFor(ext: string): string {
  switch (ext.toLowerCase()) {
    case '.jpg':
    case '.jpeg':
      return 'image/jpeg'
    case '.png':
      return 'image/png'
    case '.webp':
      return 'image/webp'
    case '.bmp':
      return 'image/bmp'
    default:
      return 'application/octet-stream'
  }
}

function clampPrefs(input: Partial<WallpaperPrefs> | undefined): WallpaperPrefs {
  const src = { ...DEFAULT_WALLPAPER_PREFS, ...(input ?? {}) }
  return {
    blur: Math.min(40, Math.max(0, Math.round(Number(src.blur) || 0))),
    dim: Math.min(80, Math.max(0, Math.round(Number(src.dim) || 0))),
    opacity: Math.min(100, Math.max(8, Math.round(Number(src.opacity) || 35))),
  }
}

function readStoredPrefs(): WallpaperPrefs {
  const raw = readPrefs() as { wallpaper?: Partial<WallpaperPrefs> }
  return clampPrefs(raw.wallpaper)
}

function writeStoredPrefs(prefs: WallpaperPrefs): void {
  // 与 orb.ts 的 saveOrbPos 共用同一原子写路径，避免两套读写把对方的键覆盖掉（M17）
  writePrefs((existing) => {
    existing.wallpaper = prefs
    return existing
  })
}

function toDataUrl(filePath: string): string | null {
  try {
    const buf = fs.readFileSync(filePath)
    if (buf.byteLength > MAX_BYTES) return null
    const ext = path.extname(filePath)
    return `data:${mimeFor(ext)};base64,${buf.toString('base64')}`
  } catch {
    return null
  }
}

export function getWallpaperState(): WallpaperState {
  const prefs = readStoredPrefs()
  const img = currentImagePath()
  if (!img) return { hasImage: false, dataUrl: null, prefs }
  const dataUrl = toDataUrl(img)
  return { hasImage: Boolean(dataUrl), dataUrl, prefs }
}

export function setWallpaperPrefs(partial: Partial<WallpaperPrefs>): WallpaperState {
  const prefs = clampPrefs({ ...readStoredPrefs(), ...partial })
  writeStoredPrefs(prefs)
  return getWallpaperState()
}

export async function pickWallpaper(win: BrowserWindow | null): Promise<WallpaperState> {
  const options: Electron.OpenDialogOptions = {
    title: '选择壁纸',
    properties: ['openFile'],
    filters: [{ name: '图片', extensions: ['jpg', 'jpeg', 'png', 'webp', 'bmp'] }],
  }
  const result = win
    ? await dialog.showOpenDialog(win, options)
    : await dialog.showOpenDialog(options)
  if (result.canceled || !result.filePaths[0]) return getWallpaperState()

  const src = result.filePaths[0]
  const ext = path.extname(src).toLowerCase()
  if (!ALLOWED_EXT.has(ext)) {
    throw new Error('仅支持 jpg / png / webp / bmp')
  }
  const stat = fs.statSync(src)
  if (stat.size > MAX_BYTES) {
    throw new Error('图片过大（上限 15MB）')
  }

  const dir = wallpaperDir()
  // 清掉旧 current.*
  let oldNames: string[]
  try { oldNames = fs.readdirSync(dir) } catch { oldNames = [] }
  for (const name of oldNames) {
    if (name.startsWith('current.')) {
      try { fs.unlinkSync(path.join(dir, name)) } catch { /* ignore */ }
    }
  }
  const dest = path.join(dir, `current${ext}`)
  fs.copyFileSync(src, dest)
  return getWallpaperState()
}

export function clearWallpaper(): WallpaperState {
  let dir: string
  try { dir = wallpaperDir() } catch { return getWallpaperState() }
  let names: string[]
  try { names = fs.readdirSync(dir) } catch { names = [] }
  for (const name of names) {
    if (name.startsWith('current.')) {
      try { fs.unlinkSync(path.join(dir, name)) } catch { /* ignore */ }
    }
  }
  return getWallpaperState()
}

/** 小球面板壁纸操作弹窗：返回用户选择的操作 */
export async function showWallpaperDialog(win: BrowserWindow | null): Promise<'pick' | 'clear' | 'cancel'> {
  const buttons = ['取消', '选择壁纸…', '清除壁纸']
  const result = win
    ? await dialog.showMessageBox(win, {
        type: 'question',
        buttons,
        defaultId: 1,
        cancelId: 0,
        title: '壁纸',
        message: '壁纸设置',
        detail: '选择一张桌面壁纸作为悬浮球背景，或清除当前壁纸。',
      })
    : await dialog.showMessageBox({
        type: 'question',
        buttons,
        defaultId: 1,
        cancelId: 0,
        title: '壁纸',
        message: '壁纸设置',
        detail: '选择一张桌面壁纸作为悬浮球背景，或清除当前壁纸。',
      })
  if (result.response === 1) return 'pick'
  if (result.response === 2) return 'clear'
  return 'cancel'
}

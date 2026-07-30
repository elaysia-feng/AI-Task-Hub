import fs from 'node:fs'
import path from 'node:path'
import { app, dialog, BrowserWindow } from 'electron'
import type { WallpaperPrefs, WallpaperState } from '../shared/types'

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

function prefsPath(): string {
  return path.join(app.getPath('userData'), 'ui-preferences.json')
}

function currentImagePath(): string | null {
  const dir = wallpaperDir()
  for (const name of fs.readdirSync(dir)) {
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
  try {
    const raw = JSON.parse(fs.readFileSync(prefsPath(), 'utf-8')) as { wallpaper?: Partial<WallpaperPrefs> }
    return clampPrefs(raw.wallpaper)
  } catch {
    return { ...DEFAULT_WALLPAPER_PREFS }
  }
}

function writeStoredPrefs(prefs: WallpaperPrefs): void {
  let existing: Record<string, unknown> = {}
  try {
    existing = JSON.parse(fs.readFileSync(prefsPath(), 'utf-8')) as Record<string, unknown>
  } catch {
    existing = {}
  }
  existing.wallpaper = prefs
  fs.writeFileSync(prefsPath(), JSON.stringify(existing, null, 2), 'utf-8')
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
  for (const name of fs.readdirSync(dir)) {
    if (name.startsWith('current.')) fs.unlinkSync(path.join(dir, name))
  }
  const dest = path.join(dir, `current${ext}`)
  fs.copyFileSync(src, dest)
  return getWallpaperState()
}

export function clearWallpaper(): WallpaperState {
  const dir = wallpaperDir()
  for (const name of fs.readdirSync(dir)) {
    if (name.startsWith('current.')) {
      try {
        fs.unlinkSync(path.join(dir, name))
      } catch {
        // ignore
      }
    }
  }
  return getWallpaperState()
}

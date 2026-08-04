import fs from 'node:fs'
import path from 'node:path'
import { app, dialog, BrowserWindow, nativeImage } from 'electron'
import type {
  WallpaperPrefs,
  WallpaperPresetMeta,
  WallpaperSelection,
  WallpaperState,
} from '../shared/types'
import { readPrefs, writePrefs } from './prefs'
import { RESOURCES_DIR } from './config'

const MAX_BYTES = 15 * 1024 * 1024
const ALLOWED_EXT = new Set(['.jpg', '.jpeg', '.png', '.webp', '.bmp'])

export const DEFAULT_WALLPAPER_PREFS: WallpaperPrefs = {
  blur: 0,
  dim: 32,
  opacity: 35,
}

export const WALLPAPER_PRESETS: WallpaperPresetMeta[] = [
  { id: 'default', name: 'AI 看板娘', lightFile: 'themes/default/wallpaper-light.png', darkFile: 'themes/default/wallpaper-dark.png' },
  { id: 'rei-ayanami', name: '绫波丽', lightFile: 'themes/rei-ayanami/wallpaper-light.png', darkFile: 'themes/rei-ayanami/wallpaper-dark.png' },
  { id: 'tomo-ebizuka', name: '海老塚智', lightFile: 'themes/tomo-ebizuka/wallpaper-light.png', darkFile: 'themes/tomo-ebizuka/wallpaper-dark.png' },
  { id: 'elaina', name: '伊蕾娜', lightFile: 'themes/elaina/wallpaper-light.png', darkFile: 'themes/elaina/wallpaper-dark.png' },
  { id: 'mutsumi-wakaba', name: '若叶睦', lightFile: 'themes/mutsumi-wakaba/wallpaper-light.png', darkFile: 'themes/mutsumi-wakaba/wallpaper-dark.png' },
  { id: 'sakiko-togawa', name: '丰川祥子', lightFile: 'themes/sakiko-togawa/wallpaper-light.png', darkFile: 'themes/sakiko-togawa/wallpaper-dark.png' },
  { id: 'yui-hirasawa', name: '平泽唯', lightFile: 'themes/yui-hirasawa/wallpaper-light.png', darkFile: 'themes/yui-hirasawa/wallpaper-dark.png' },
  { id: 'mio-akiyama', name: '秋山澪', lightFile: 'themes/mio-akiyama/wallpaper-light.png', darkFile: 'themes/mio-akiyama/wallpaper-dark.png' },
  { id: 'ritsu-tainaka', name: '田井中律', lightFile: 'themes/ritsu-tainaka/wallpaper-light.png', darkFile: 'themes/ritsu-tainaka/wallpaper-dark.png' },
  { id: 'tsumugi-kotobuki', name: '琴吹紬', lightFile: 'themes/tsumugi-kotobuki/wallpaper-light.png', darkFile: 'themes/tsumugi-kotobuki/wallpaper-dark.png' },
  { id: 'azusa-nakano', name: '中野梓', lightFile: 'themes/azusa-nakano/wallpaper-light.png', darkFile: 'themes/azusa-nakano/wallpaper-dark.png' },
]

let cachedPresetWallpaper: {
  presetId: string
  dataUrlLight: string | null
  dataUrlDark: string | null
} | null = null
let cachedPresetPreviews: WallpaperPresetMeta[] | null = null

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

function readSelection(): WallpaperSelection {
  const raw = readPrefs().wallpaperSelection as Record<string, unknown> | undefined
  if (raw?.source === 'preset' && typeof raw.presetId === 'string') {
    if (WALLPAPER_PRESETS.some((preset) => preset.id === raw.presetId)) {
      return { source: 'preset', presetId: raw.presetId }
    }
  }
  if (raw?.source === 'custom') return { source: 'custom' }
  // 兼容旧版：已有 current.* 时继续作为本地壁纸使用。
  if (!raw && currentImagePath()) return { source: 'custom' }
  // 新安装默认展示新版 AI 看板娘；用户主动清除后会持久化 source=none。
  if (!raw) return { source: 'preset', presetId: 'default' }
  return { source: 'none' }
}

function writeSelection(selection: WallpaperSelection): void {
  writePrefs((existing) => {
    existing.wallpaperSelection = selection
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
  let selection = readSelection()
  let lightPath: string | null = null
  let darkPath: string | null = null
  if (selection.source === 'custom') {
    lightPath = currentImagePath()
    darkPath = lightPath
    if (!lightPath) {
      selection = { source: 'none' }
      writeSelection(selection)
    }
  } else if (selection.source === 'preset') {
    const presetId = selection.presetId
    const preset = WALLPAPER_PRESETS.find((item) => item.id === presetId)
    if (preset) {
      lightPath = path.join(RESOURCES_DIR, preset.lightFile)
      darkPath = path.join(RESOURCES_DIR, preset.darkFile)
    }
  }
  let dataUrlLight: string | null
  let dataUrlDark: string | null
  if (selection.source === 'preset') {
    const pair = presetWallpaperData(selection.presetId, lightPath, darkPath)
    dataUrlLight = pair.dataUrlLight
    dataUrlDark = pair.dataUrlDark
  } else {
    dataUrlLight = lightPath ? toDataUrl(lightPath) : null
    dataUrlDark = darkPath ? toDataUrl(darkPath) : null
  }
  return {
    hasImage: Boolean(dataUrlLight || dataUrlDark),
    dataUrlLight: dataUrlLight ?? dataUrlDark,
    dataUrlDark: dataUrlDark ?? dataUrlLight,
    prefs,
    selection,
    presets: presetPreviews(),
  }
}

function presetPreviews(): WallpaperPresetMeta[] {
  if (cachedPresetPreviews) return cachedPresetPreviews
  cachedPresetPreviews = WALLPAPER_PRESETS.map((preset) => {
    const icon = nativeImage.createFromPath(path.join(RESOURCES_DIR, 'presets', `${preset.id}.png`))
    return {
      ...preset,
      previewDataUrl: icon.isEmpty() ? null : icon.resize({ width: 96, height: 96 }).toDataURL(),
    }
  })
  return cachedPresetPreviews
}

function presetWallpaperData(
  presetId: string,
  lightPath: string | null,
  darkPath: string | null,
): { dataUrlLight: string | null; dataUrlDark: string | null } {
  if (cachedPresetWallpaper?.presetId === presetId) return cachedPresetWallpaper
  cachedPresetWallpaper = {
    presetId,
    dataUrlLight: lightPath ? toDataUrl(lightPath) : null,
    dataUrlDark: darkPath ? toDataUrl(darkPath) : null,
  }
  return cachedPresetWallpaper
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
  writeSelection({ source: 'custom' })
  return getWallpaperState()
}

export function setWallpaperPreset(presetId: string): WallpaperState {
  const valid = WALLPAPER_PRESETS.some((preset) => preset.id === presetId)
  if (!valid) throw new Error('未知的内置壁纸')
  writeSelection({ source: 'preset', presetId })
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
  writeSelection({ source: 'none' })
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

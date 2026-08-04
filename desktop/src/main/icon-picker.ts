import fs from 'node:fs'
import path from 'node:path'
import { app, dialog, BrowserWindow, nativeImage } from 'electron'
import type { UserIconPrefs, UserIconPresetMeta, UserIconState } from '../shared/types'
import { readPrefs, writePrefs } from './prefs'
import { RESOURCES_DIR } from './config'
import type { TrayHandle } from './tray'

/**
 * 应用图标偏好：用户可选内置预设（粉发少女默认）或本地自定义图片。
 *
 * 生效面：
 *   - 悬浮球球面（渲染端 --orb-face / --orb-face-size）
 *   - 窗口 / 任务栏图标（win.setIcon）
 *   - 托盘图标（tray.setImage，32×32）
 *   - 打包图标（apply-user-icon.mjs / packaging.ts 用 getUserIconSourcePath 重跑 make-icon.ps1）
 *
 * 持久化：ui-preferences.json 的 icon 键 + {userData}/icon/current.<ext>。
 */

const MAX_BYTES = 5 * 1024 * 1024
const ALLOWED_EXT = new Set(['.jpg', '.jpeg', '.png', '.webp', '.bmp'])

/**
 * 内置预设注册表（可扩展）：
 * 往 desktop/resources/presets/ 丢一张正方形 PNG，再在数组里加一项即可。
 * file 为相对 RESOURCES_DIR 的路径；默认预设沿用 resources/anime-head.png（与 make-icon.ps1 同源）。
 */
export const ICON_PRESETS: UserIconPresetMeta[] = [
  { id: 'default', name: '粉发少女（默认）', file: 'anime-head.png' },
]

export const DEFAULT_ICON_PREFS = { source: 'preset', presetId: 'default' } as const

let getWindow: () => BrowserWindow | null = () => null
let trayHandle: TrayHandle | null = null

export function bindIconWindow(getter: () => BrowserWindow | null): void {
  getWindow = getter
}

export function setTrayHandle(handle: TrayHandle | null): void {
  trayHandle = handle
}

/* ---------- 持久化 ---------- */

function iconDir(): string {
  const dir = path.join(app.getPath('userData'), 'icon')
  fs.mkdirSync(dir, { recursive: true })
  return dir
}

function currentImagePath(): string | null {
  let dir: string
  try {
    dir = iconDir()
  } catch {
    return null
  }
  let names: string[]
  try {
    names = fs.readdirSync(dir)
  } catch {
    return null
  }
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

function clampPrefs(input: Record<string, unknown> | undefined): UserIconPrefs {
  if (input?.source === 'custom') return { source: 'custom' }
  const id = typeof input?.presetId === 'string' ? input.presetId : DEFAULT_ICON_PREFS.presetId
  return {
    source: 'preset',
    presetId: ICON_PRESETS.some((p) => p.id === id) ? id : DEFAULT_ICON_PREFS.presetId,
  }
}

function readStoredPrefs(): UserIconPrefs {
  return clampPrefs(readPrefs().icon as Record<string, unknown> | undefined)
}

function writeStoredPrefs(prefs: UserIconPrefs): void {
  // 与 orb.ts / wallpaper.ts 共用同一原子写路径，避免各套读写把对方的键覆盖掉（M17）
  writePrefs((existing) => {
    existing.icon = prefs
    return existing
  })
}

/* ---------- 源图解析 ---------- */

function sourcePathFor(prefs: UserIconPrefs): string | null {
  if (prefs.source === 'custom') return currentImagePath()
  const preset = ICON_PRESETS.find((p) => p.id === prefs.presetId)
  if (!preset) return null
  const p = path.join(RESOURCES_DIR, preset.file)
  return fs.existsSync(p) ? p : null
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

/* ---------- 状态读取 ---------- */

export function getUserIconState(): UserIconState {
  let prefs = readStoredPrefs()
  // 自愈：custom 但本地文件缺失（被清缓存/搬目录）→ 回落默认预设并写回
  if (prefs.source === 'custom' && !currentImagePath()) {
    prefs = { ...DEFAULT_ICON_PREFS }
    writeStoredPrefs(prefs)
  }
  const src = sourcePathFor(prefs)
  const dataUrl = src ? toDataUrl(src) : null
  return { prefs, dataUrl, presets: ICON_PRESETS }
}

/** 打包用：仅当用户设了自定义图标且文件存在时返回源图绝对路径（默认预设走 make-icon.ps1 原有逻辑） */
export function getUserIconSourcePath(): string | null {
  const prefs = readStoredPrefs()
  if (prefs.source !== 'custom') return null
  const p = currentImagePath()
  return p && fs.existsSync(p) ? p : null
}

/* ---------- 生效（窗口 / 托盘 / 渲染端推送） ---------- */

function applyToWindow(dataUrl: string | null): void {
  const win = getWindow()
  if (!win || win.isDestroyed() || !dataUrl) return
  const img = nativeImage.createFromDataURL(dataUrl)
  if (!img.isEmpty()) win.setIcon(img)
}

function applyToTray(prefs: UserIconPrefs, dataUrl: string | null): void {
  const tray = trayHandle?.tray
  if (!tray) return
  // 默认预设沿用 make-icon.ps1 生成的圆形 tray.png（保持托盘圆形头像设计）；
  // 自定义图运行时缩成 32×32 应用（方形托盘图标属 Windows 常规表现）
  if (prefs.source === 'preset' && prefs.presetId === 'default') {
    const p = path.join(RESOURCES_DIR, 'tray.png')
    if (fs.existsSync(p)) {
      const img = nativeImage.createFromPath(p)
      if (!img.isEmpty()) {
        tray.setImage(img)
        return
      }
    }
  }
  if (!dataUrl) return
  const img = nativeImage.createFromDataURL(dataUrl).resize({ width: 32, height: 32 })
  if (!img.isEmpty()) tray.setImage(img)
}

function notifyRenderer(): void {
  const win = getWindow()
  if (!win || win.isDestroyed()) return
  try {
    win.webContents.send('icon:changed', getUserIconState())
  } catch {
    // 页面尚未就绪，渲染端 bootstrap 会拉一次全量状态
  }
}

/** 变更入口统一出口：写状态 → 窗口/托盘 → 推送渲染端 → 返回新状态 */
function commit(): UserIconState {
  const state = getUserIconState()
  applyToWindow(state.dataUrl)
  applyToTray(state.prefs, state.dataUrl)
  notifyRenderer()
  return state
}

/** 启动 / 窗口重建时调用：把持久化图标应用到窗口与托盘 */
export function applyPersistedIcon(): void {
  const state = getUserIconState()
  applyToWindow(state.dataUrl)
  applyToTray(state.prefs, state.dataUrl)
}

/* ---------- 变更入口 ---------- */

function clearCurrent(): void {
  let dir: string
  try {
    dir = iconDir()
  } catch {
    return
  }
  let names: string[] = []
  try {
    names = fs.readdirSync(dir)
  } catch {
    return
  }
  for (const name of names) {
    if (name.startsWith('current.')) {
      try {
        fs.unlinkSync(path.join(dir, name))
      } catch {
        /* ignore */
      }
    }
  }
}

export async function pickUserIcon(win: BrowserWindow | null): Promise<UserIconState> {
  const options: Electron.OpenDialogOptions = {
    title: '选择应用图标',
    properties: ['openFile'],
    filters: [{ name: '图片', extensions: ['png', 'jpg', 'jpeg', 'webp', 'bmp'] }],
  }
  const result = win
    ? await dialog.showOpenDialog(win, options)
    : await dialog.showOpenDialog(options)
  if (result.canceled || !result.filePaths[0]) return getUserIconState()

  const src = result.filePaths[0]
  const ext = path.extname(src).toLowerCase()
  if (!ALLOWED_EXT.has(ext)) {
    throw new Error('仅支持 png / jpg / jpeg / webp / bmp')
  }
  const stat = fs.statSync(src)
  if (stat.size > MAX_BYTES) {
    throw new Error('图片过大（上限 5MB）')
  }

  const dir = iconDir()
  clearCurrent()
  const dest = path.join(dir, `current${ext}`)
  fs.copyFileSync(src, dest)

  writeStoredPrefs({ source: 'custom' })
  return commit()
}

export function setUserIconPreset(presetId: string): UserIconState {
  const valid = ICON_PRESETS.some((p) => p.id === presetId)
  clearCurrent()
  writeStoredPrefs({
    source: 'preset',
    presetId: valid ? presetId : DEFAULT_ICON_PREFS.presetId,
  })
  return commit()
}

export function resetUserIcon(): UserIconState {
  clearCurrent()
  writeStoredPrefs({ ...DEFAULT_ICON_PREFS })
  return commit()
}

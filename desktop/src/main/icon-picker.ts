import fs from 'node:fs'
import path from 'node:path'
import { app, dialog, BrowserWindow, nativeImage } from 'electron'
import type { UserIconPrefs, UserIconPresetMeta, UserIconState } from '../shared/types'
import { readPrefs, writePrefs } from './prefs'
import { RESOURCES_DIR } from './config'
import type { TrayHandle } from './tray'

/**
 * 应用图标偏好：用户可选内置角色预设（AI 看板娘默认）或本地自定义图片。
 *
 * 生效面：
 *   - 标题栏图标（渲染端 --titlebar-icon / --titlebar-icon-size）
 *   - 悬浮球球面（渲染端 --orb-face / --orb-face-size）
 *   - 窗口 / 任务栏图标（win.setIcon）
 *   - 托盘图标（tray.setImage，32×32）
 *   - 打包图标（apply-user-icon.mjs / packaging.ts 用 getUserIconSourcePath 重跑 make-icon.ps1）
 *
 * 持久化：ui-preferences.json 的 icon 键 + {userData}/icon/current.<ext>。
 */

const MAX_BYTES = 5 * 1024 * 1024
const UI_ICON_MAX_EDGE = 256
const ALLOWED_EXT = new Set(['.jpg', '.jpeg', '.png', '.webp', '.bmp'])

/**
 * 内置预设注册表（可扩展）：
 * 往 desktop/resources/presets/ 丢一张正方形 PNG，再在数组里加一项即可。
 * file 为相对 RESOURCES_DIR 的路径；resources/anime-head.png 与默认预设保持同源。
 */
export const ICON_PRESETS: UserIconPresetMeta[] = [
  { id: 'default', name: 'AI 看板娘（默认）', file: 'presets/default.png' },
  { id: 'rei-ayanami', name: '绫波丽', file: 'presets/rei-ayanami.png' },
  { id: 'tomo-ebizuka', name: '海老塚智', file: 'presets/tomo-ebizuka.png' },
  { id: 'elaina', name: '伊蕾娜', file: 'presets/elaina.png' },
  { id: 'mutsumi-wakaba', name: '若叶睦', file: 'presets/mutsumi-wakaba.png' },
  { id: 'sakiko-togawa', name: '丰川祥子', file: 'presets/sakiko-togawa.png' },
  { id: 'yui-hirasawa', name: '平泽唯', file: 'presets/yui-hirasawa.png' },
  { id: 'mio-akiyama', name: '秋山澪', file: 'presets/mio-akiyama.png' },
  { id: 'ritsu-tainaka', name: '田井中律', file: 'presets/ritsu-tainaka.png' },
  { id: 'tsumugi-kotobuki', name: '琴吹紬', file: 'presets/tsumugi-kotobuki.png' },
  { id: 'azusa-nakano', name: '中野梓', file: 'presets/azusa-nakano.png' },
]

export const DEFAULT_ICON_PREFS = { source: 'preset', presetId: 'default' } as const

let getWindow: () => BrowserWindow | null = () => null
let trayHandle: TrayHandle | null = null
let cachedPresetPreviews: UserIconPresetMeta[] | null = null
const cachedPresetDataUrls = new Map<string, string | null>()

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

function toUiDataUrl(filePath: string): string | null {
  const icon = nativeImage.createFromPath(filePath)
  if (icon.isEmpty()) return null

  const { width, height } = icon.getSize()
  const scale = Math.min(1, UI_ICON_MAX_EDGE / Math.max(width, height))
  const resized = scale < 1
    ? icon.resize({
        width: Math.max(1, Math.round(width * scale)),
        height: Math.max(1, Math.round(height * scale)),
      })
    : icon
  return resized.toDataURL()
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
  const dataUrl = src
    ? prefs.source === 'preset'
      ? presetDataUrl(prefs.presetId, src)
      : toUiDataUrl(src)
    : null
  return { prefs, dataUrl, presets: presetPreviews() }
}

function presetDataUrl(presetId: string, filePath: string): string | null {
  if (!cachedPresetDataUrls.has(presetId)) {
    cachedPresetDataUrls.set(presetId, toUiDataUrl(filePath))
  }
  return cachedPresetDataUrls.get(presetId) ?? null
}

function presetPreviews(): UserIconPresetMeta[] {
  if (cachedPresetPreviews) return cachedPresetPreviews
  cachedPresetPreviews = ICON_PRESETS.map((preset) => {
    const icon = nativeImage.createFromPath(path.join(RESOURCES_DIR, preset.file))
    return {
      ...preset,
      previewDataUrl: icon.isEmpty() ? null : icon.resize({ width: 96, height: 96 }).toDataURL(),
    }
  })
  return cachedPresetPreviews
}

/** 应用内打包用：返回当前预设或自定义图标的源图绝对路径。 */
export function getUserIconSourcePath(): string | null {
  const prefs = readStoredPrefs()
  const p = sourcePathFor(prefs)
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

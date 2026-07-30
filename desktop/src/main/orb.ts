import fs from 'node:fs'
import path from 'node:path'
import { BrowserWindow, app, screen } from 'electron'

/** 小球收起尺寸（整窗就是球，可拖满屏） */
export const ORB_SIZE = 44
/** 悬停展开时窗口尺寸（球在右下角） */
export const ORB_PANEL_W = 300
export const ORB_PANEL_H = 380

const PANEL_MIN = { width: 880, height: 560 }
const PANEL_DEFAULT = { width: 1020, height: 660 }

export type WindowMode = 'panel' | 'orb'

let mode: WindowMode = 'panel'
let getWindow: () => BrowserWindow | null = () => null
let panelBounds: Electron.Rectangle | null = null
let dragState: { sx: number; sy: number; wx: number; wy: number } | null = null
let orbExpanded = false

export function bindOrbWindow(getter: () => BrowserWindow | null): void {
  getWindow = getter
}

export function getWindowMode(): WindowMode {
  return mode
}

function prefsPath(): string {
  return path.join(app.getPath('userData'), 'ui-preferences.json')
}

function loadOrbPos(): { x: number; y: number } | null {
  try {
    const raw = JSON.parse(fs.readFileSync(prefsPath(), 'utf-8')) as { orb?: { x?: number; y?: number } }
    if (typeof raw.orb?.x === 'number' && typeof raw.orb?.y === 'number') {
      return { x: raw.orb.x, y: raw.orb.y }
    }
  } catch {
    /* first run */
  }
  return null
}

function saveOrbPos(x: number, y: number): void {
  try {
    let existing: Record<string, unknown> = {}
    try {
      existing = JSON.parse(fs.readFileSync(prefsPath(), 'utf-8')) as Record<string, unknown>
    } catch {
      /* empty */
    }
    existing.orb = { x, y }
    fs.writeFileSync(prefsPath(), JSON.stringify(existing, null, 2), 'utf-8')
  } catch {
    /* ignore */
  }
}

function notifyMode(win: BrowserWindow): void {
  win.webContents.send('ui:mode', mode)
}

function defaultOrbPos(): { x: number; y: number } {
  const { workArea } = screen.getPrimaryDisplay()
  return {
    x: Math.round(workArea.x + workArea.width - ORB_SIZE - 16),
    y: Math.round(workArea.y + workArea.height - ORB_SIZE - 16),
  }
}

/** 限位：按「当前窗口矩形」夹在最近显示器工作区内 */
function clampBounds(x: number, y: number, w: number, h: number): Electron.Rectangle {
  const display = screen.getDisplayNearestPoint({ x: x + w / 2, y: y + h / 2 })
  const { workArea } = display
  const minX = workArea.x + 2
  const minY = workArea.y + 2
  const maxX = workArea.x + workArea.width - w - 2
  const maxY = workArea.y + workArea.height - h - 2
  return {
    x: Math.round(Math.min(Math.max(x, minX), Math.max(minX, maxX))),
    y: Math.round(Math.min(Math.max(y, minY), Math.max(minY, maxY))),
    width: w,
    height: h,
  }
}

/** 以球的屏幕坐标为锚，换算窗口左上角（球贴在窗口右下） */
function boundsFromBallAnchor(
  ballRight: number,
  ballBottom: number,
  w: number,
  h: number,
): Electron.Rectangle {
  return clampBounds(ballRight - w, ballBottom - h, w, h)
}

function currentBallAnchor(win: BrowserWindow): { right: number; bottom: number } {
  const b = win.getBounds()
  return { right: b.x + b.width, bottom: b.y + b.height }
}

export function enterOrbMode(): void {
  const win = getWindow()
  if (!win || win.isDestroyed()) return

  if (mode === 'panel') {
    const b = win.getBounds()
    // 只保存像样的主面板尺寸，避免把残缺尺寸当恢复目标
    if (b.width >= PANEL_MIN.width && b.height >= PANEL_MIN.height) {
      panelBounds = b
    }
  }

  mode = 'orb'
  orbExpanded = false
  dragState = null

  win.setResizable(true)
  win.setMinimumSize(ORB_SIZE, ORB_SIZE)
  win.setMaximumSize(ORB_PANEL_W, ORB_PANEL_H)

  const saved = loadOrbPos()
  const fallback = defaultOrbPos()
  const candidate = saved ?? fallback
  let bounds = clampBounds(candidate.x, candidate.y, ORB_SIZE, ORB_SIZE)
  const { workArea } = screen.getDisplayNearestPoint({
    x: bounds.x + ORB_SIZE / 2,
    y: bounds.y + ORB_SIZE / 2,
  })
  if (
    bounds.x + ORB_SIZE < workArea.x + 8 ||
    bounds.y + ORB_SIZE < workArea.y + 8 ||
    bounds.x > workArea.x + workArea.width - 8 ||
    bounds.y > workArea.y + workArea.height - 8
  ) {
    bounds = clampBounds(fallback.x, fallback.y, ORB_SIZE, ORB_SIZE)
  }

  win.setAlwaysOnTop(true, 'screen-saver')
  win.setSkipTaskbar(true)
  // 必须在仍可 resize 时改尺寸，否则 Windows 上 setBounds 常被忽略
  win.setBounds(bounds)
  win.setResizable(false)
  win.setOpacity(1)
  if (!win.isVisible()) win.show()
  else win.showInactive()
  win.moveTop()
  notifyMode(win)
  setTimeout(() => {
    if (!win.isDestroyed() && mode === 'orb') notifyMode(win)
  }, 50)
}

export function enterPanelMode(): void {
  const win = getWindow()
  if (!win || win.isDestroyed()) return

  if (mode === 'orb') {
    const b = win.getBounds()
    // 存球的位置用窗口左上角（收起尺寸）
    saveOrbPos(
      orbExpanded ? b.x + b.width - ORB_SIZE : b.x,
      orbExpanded ? b.y + b.height - ORB_SIZE : b.y,
    )
  }

  mode = 'panel'
  orbExpanded = false
  dragState = null

  // 先允许缩放，再改 max/min/bounds（顺序很重要）
  win.setResizable(true)
  win.setAlwaysOnTop(false)
  win.setSkipTaskbar(false)
  win.setMaximumSize(10000, 10000)
  win.setMinimumSize(PANEL_MIN.width, PANEL_MIN.height)

  const fallback = {
    ...defaultCenteredPanel(),
    width: PANEL_DEFAULT.width,
    height: PANEL_DEFAULT.height,
  }
  const raw = panelBounds ?? fallback
  const restored = clampBounds(
    raw.x,
    raw.y,
    Math.max(raw.width, PANEL_MIN.width),
    Math.max(raw.height, PANEL_MIN.height),
  )

  win.setBounds(restored)
  // 再强制一次尺寸，防止仍停在小球/展开球的残缺大小
  win.setSize(restored.width, restored.height)
  win.setPosition(restored.x, restored.y)

  if (win.isMinimized()) win.restore()
  win.setOpacity(1)
  win.show()
  win.focus()
  win.moveTop()
  notifyMode(win)
  setTimeout(() => {
    if (!win.isDestroyed() && mode === 'panel') notifyMode(win)
  }, 50)
}

function defaultCenteredPanel(): { x: number; y: number } {
  const { workArea } = screen.getPrimaryDisplay()
  return {
    x: Math.round(workArea.x + (workArea.width - PANEL_DEFAULT.width) / 2),
    y: Math.round(workArea.y + (workArea.height - PANEL_DEFAULT.height) / 2),
  }
}

/** 悬停展开/收起任务面板（只在 orb 模式） */
export function setOrbPanelExpanded(expanded: boolean): void {
  const win = getWindow()
  if (!win || win.isDestroyed() || mode !== 'orb') return
  if (orbExpanded === expanded) return

  const anchor = currentBallAnchor(win)
  orbExpanded = expanded
  const w = expanded ? ORB_PANEL_W : ORB_SIZE
  const h = expanded ? ORB_PANEL_H : ORB_SIZE
  // resizable=false 时 Windows 常忽略 setBounds，先临时放开再锁回
  win.setResizable(true)
  win.setBounds(boundsFromBallAnchor(anchor.right, anchor.bottom, w, h))
  win.setResizable(false)
}

export function isOrbMode(): boolean {
  return mode === 'orb'
}

export function startOrbDrag(screenX: number, screenY: number): void {
  const win = getWindow()
  if (!win || win.isDestroyed() || mode !== 'orb') return
  // 拖动时先收起面板，窗口变小才好拖满屏
  if (orbExpanded) setOrbPanelExpanded(false)
  const [wx, wy] = win.getPosition()
  dragState = { sx: screenX, sy: screenY, wx, wy }
}

export function moveOrbDrag(screenX: number, screenY: number): void {
  const win = getWindow()
  if (!dragState || !win || win.isDestroyed() || mode !== 'orb') return
  const w = win.getBounds().width
  const h = win.getBounds().height
  const next = clampBounds(
    dragState.wx + (screenX - dragState.sx),
    dragState.wy + (screenY - dragState.sy),
    w,
    h,
  )
  win.setPosition(next.x, next.y)
}

export function endOrbDrag(): void {
  const win = getWindow()
  if (win && !win.isDestroyed() && mode === 'orb') {
    const b = win.getBounds()
    saveOrbPos(b.x, b.y)
  }
  dragState = null
}

/** 托盘：面板 ↔ 小球（不再把窗口 hide 掉，避免「球没了」） */
export function toggleOrbVisibility(): void {
  const win = getWindow()
  if (!win || win.isDestroyed()) return
  if (mode === 'panel') enterOrbMode()
  else enterPanelMode()
}

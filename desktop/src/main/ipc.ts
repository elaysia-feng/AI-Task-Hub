import { app, BrowserWindow, dialog, ipcMain, shell } from 'electron'
import path from 'node:path'
import os from 'node:os'
import { apiClient } from './api-client'
import { openTaskTarget } from './launcher'
import { buildExeWithConfirm } from './packaging'
import { resolveAllowedPath } from './path-util'
import {
  clearWallpaper,
  getWallpaperState,
  pickWallpaper,
  setWallpaperPrefs,
  showWallpaperDialog,
} from './wallpaper'
import type { BackendManager } from './backend'
import type { UpdateManager } from './updater'
import type { WallpaperPrefs } from '../shared/types'

// Allowed path prefixes for shell:open-path (user-controlled directories only)
const ALLOWED_OPEN_PREFIXES = [
  os.homedir(),
  path.join(os.homedir(), 'Desktop'),
  path.join(os.homedir(), 'Documents'),
  path.join(os.homedir(), 'Downloads'),
]

function sanitizeCoord(v: unknown): number {
  const n = Number(v)
  if (!Number.isFinite(n)) return 0
  return Math.max(-10000, Math.min(10000, n))
}

/** realpath 解析 junction/symlink + NTFS 大小写不敏感 + 拒绝 URL 协议，见 path-util（M18/M19） */
function isAllowedOpenPath(input: string): boolean {
  return resolveAllowedPath(input, ALLOWED_OPEN_PREFIXES) !== null
}

export interface OrbIpc {
  enterOrb: () => void
  enterPanel: () => void
  setPanelExpanded: (expanded: boolean) => void
  dragStart: (screenX: number, screenY: number) => void
  dragMove: (screenX: number, screenY: number) => void
  dragEnd: () => void
  isOrb: () => boolean
}

/** 注册渲染进程可调用的全部 IPC 通道 */
export function registerIpcHandlers(
  getWindow: () => BrowserWindow | null,
  backend: BackendManager,
  updater: UpdateManager,
  orb?: OrbIpc,
): void {
  ipcMain.handle('tasks:queue', () => apiClient.listTasks('queue'))
  ipcMain.handle('tasks:history', () => apiClient.listTasks('history'))

  ipcMain.handle('tasks:open', async (_event, id: number) => {
    try {
      const task = await apiClient.getTask(id)
      await openTaskTarget(task)
      await apiClient.markViewed(id)
      return { ok: true }
    } catch (err) {
      // 任务已删除 / 后端离线等场景不产生未处理 rejection，把错误带回渲染层
      console.warn('[ipc] tasks:open failed:', err)
      return { ok: false, err: err instanceof Error ? err.message : String(err) }
    }
  })

  ipcMain.handle('tasks:ignore', (_event, id: number) => apiClient.markIgnored(id))
  ipcMain.handle('tasks:delete', (_event, id: number) => apiClient.deleteTask(id))
  ipcMain.handle('tasks:clear', () => apiClient.clearTasks())
  ipcMain.handle('tasks:read-all', () => apiClient.readAllTasks())
  ipcMain.handle('backend:status', () => backend.current)

  ipcMain.handle('update:check', () => updater.check())
  ipcMain.on('update:install', () => updater.quitAndInstall())

  ipcMain.handle('server:status', () => apiClient.getServerStatus())
  ipcMain.handle('integrations:status', () => apiClient.getIntegrations())
  ipcMain.handle('integrations:install-claude', () => apiClient.installClaude())
  ipcMain.handle('integrations:install-codex', () => apiClient.installCodex())
  ipcMain.handle('tasks:events', (_event, taskId: number) => {
    const id = Number(taskId)
    if (!Number.isInteger(id) || id <= 0) return null
    return apiClient.getTaskEvents(id)
  })
  ipcMain.handle('shell:open-path', (_event, target: string) => {
    if (!isAllowedOpenPath(target)) {
      return { err: 'Disallowed path: must be under user home directory' }
    }
    return shell.openPath(target)
  })

  ipcMain.handle('wallpaper:get', () => getWallpaperState())
  ipcMain.handle('wallpaper:pick', () => pickWallpaper(getWindow()))
  ipcMain.handle('wallpaper:clear', () => clearWallpaper())
  ipcMain.handle('wallpaper:set-prefs', (_event, prefs: Partial<WallpaperPrefs>) =>
    setWallpaperPrefs(prefs),
  )
  ipcMain.handle('wallpaper:dialog', () => showWallpaperDialog(getWindow()))

  ipcMain.on('window:minimize', () => getWindow()?.minimize())
  ipcMain.on('window:close', () => {
    // 关闭 = 收起为小球（同一窗口）
    orb?.enterOrb()
  })
  ipcMain.on('window:show-main', () => {
    orb?.enterPanel()
  })
  ipcMain.on('window:collapse-orb', () => {
    orb?.enterOrb()
  })

  ipcMain.handle('orb:set-panel', (_event, expanded: boolean) => {
    orb?.setPanelExpanded(Boolean(expanded))
  })
  ipcMain.on('orb:drag-start', (_event, screenX: number, screenY: number) => {
    orb?.dragStart(sanitizeCoord(screenX), sanitizeCoord(screenY))
  })
  ipcMain.on('orb:drag-move', (_event, screenX: number, screenY: number) => {
    orb?.dragMove(sanitizeCoord(screenX), sanitizeCoord(screenY))
  })
  ipcMain.on('orb:drag-end', () => {
    orb?.dragEnd()
  })

  ipcMain.handle('packaging:build-exe', () => buildExeWithConfirm(getWindow()))
  // 渲染层据此隐藏「应用内打包」按钮：安装版（app.asar）内没有打包所需源码文件
  ipcMain.handle('app:is-packaged', () => app.isPackaged)
}

import { BrowserWindow, dialog, ipcMain, shell } from 'electron'
import { apiClient } from './api-client'
import { openTaskTarget } from './launcher'
import { buildExeWithConfirm } from './packaging'
import {
  clearWallpaper,
  getWallpaperState,
  pickWallpaper,
  setWallpaperPrefs,
} from './wallpaper'
import type { BackendManager } from './backend'
import type { UpdateManager } from './updater'
import type { WallpaperPrefs } from '../shared/types'

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
    const queue = await apiClient.listTasks('queue')
    const task = queue.find((t) => t.id === id)
    if (task) await openTaskTarget(task)
    await apiClient.markViewed(id)
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
  ipcMain.handle('tasks:events', (_event, taskId: number) => apiClient.getTaskEvents(taskId))
  ipcMain.handle('shell:open-path', (_event, target: string) => shell.openPath(target))

  ipcMain.handle('wallpaper:get', () => getWallpaperState())
  ipcMain.handle('wallpaper:pick', () => pickWallpaper(getWindow()))
  ipcMain.handle('wallpaper:clear', () => clearWallpaper())
  ipcMain.handle('wallpaper:set-prefs', (_event, prefs: Partial<WallpaperPrefs>) =>
    setWallpaperPrefs(prefs),
  )

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
    orb?.dragStart(Number(screenX), Number(screenY))
  })
  ipcMain.on('orb:drag-move', (_event, screenX: number, screenY: number) => {
    orb?.dragMove(Number(screenX), Number(screenY))
  })
  ipcMain.on('orb:drag-end', () => {
    orb?.dragEnd()
  })

  ipcMain.handle('packaging:build-exe', () => buildExeWithConfirm(getWindow()))
}

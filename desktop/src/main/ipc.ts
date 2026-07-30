import { BrowserWindow, ipcMain, shell } from 'electron'
import { apiClient } from './api-client'
import { openTaskTarget } from './launcher'
import type { BackendManager } from './backend'
import type { UpdateManager } from './updater'

/** 注册渲染进程可调用的全部 IPC 通道 */
export function registerIpcHandlers(
  getWindow: () => BrowserWindow | null,
  backend: BackendManager,
  updater: UpdateManager,
): void {
  ipcMain.handle('tasks:queue', () => apiClient.listTasks('queue'))
  ipcMain.handle('tasks:history', () => apiClient.listTasks('history'))

  ipcMain.handle('tasks:open', async (_event, id: number) => {
    const queue = await apiClient.listTasks('queue')
    const task = queue.find((t) => t.id === id)
    if (task) await openTaskTarget(task)
    // 点击即视为已读：无论唤起成功与否都移出未读队列
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

  ipcMain.on('window:minimize', () => getWindow()?.minimize())
  ipcMain.on('window:close', () => getWindow()?.hide())
}

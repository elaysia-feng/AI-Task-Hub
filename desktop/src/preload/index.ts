import { contextBridge, ipcRenderer } from 'electron'
import type { AihubApi, BackendStatus, ServerMessage, UpdateState } from '../shared/types'

const api: AihubApi = {
  getQueue: () => ipcRenderer.invoke('tasks:queue'),
  getHistory: () => ipcRenderer.invoke('tasks:history'),
  openTask: (id) => ipcRenderer.invoke('tasks:open', id),
  ignoreTask: (id) => ipcRenderer.invoke('tasks:ignore', id),
  deleteTask: (id) => ipcRenderer.invoke('tasks:delete', id),
  clearTasks: () => ipcRenderer.invoke('tasks:clear'),
  readAllTasks: () => ipcRenderer.invoke('tasks:read-all'),
  getBackendStatus: () => ipcRenderer.invoke('backend:status'),

  onTaskChanged: (cb) => {
    const listener = (_event: Electron.IpcRendererEvent, msg: ServerMessage): void => cb(msg)
    ipcRenderer.on('task:changed', listener)
    return () => ipcRenderer.removeListener('task:changed', listener)
  },

  onBackendStatus: (cb) => {
    const listener = (_event: Electron.IpcRendererEvent, status: BackendStatus): void => cb(status)
    ipcRenderer.on('backend:status', listener)
    return () => ipcRenderer.removeListener('backend:status', listener)
  },

  checkUpdates: () => ipcRenderer.invoke('update:check'),
  installUpdate: () => ipcRenderer.send('update:install'),
  onUpdateStatus: (cb) => {
    const listener = (_event: Electron.IpcRendererEvent, state: UpdateState): void => cb(state)
    ipcRenderer.on('update:status', listener)
    return () => ipcRenderer.removeListener('update:status', listener)
  },

  getServerStatus: () => ipcRenderer.invoke('server:status'),
  getIntegrations: () => ipcRenderer.invoke('integrations:status'),
  installClaude: () => ipcRenderer.invoke('integrations:install-claude'),
  installCodex: () => ipcRenderer.invoke('integrations:install-codex'),
  getTaskEvents: (taskId) => ipcRenderer.invoke('tasks:events', taskId),
  openPath: (target) => ipcRenderer.invoke('shell:open-path', target),

  getWallpaper: () => ipcRenderer.invoke('wallpaper:get'),
  pickWallpaper: () => ipcRenderer.invoke('wallpaper:pick'),
  clearWallpaper: () => ipcRenderer.invoke('wallpaper:clear'),
  setWallpaperPrefs: (prefs) => ipcRenderer.invoke('wallpaper:set-prefs', prefs),

  minimizeWindow: () => ipcRenderer.send('window:minimize'),
  closeWindow: () => ipcRenderer.send('window:close'),
  showMainWindow: () => ipcRenderer.send('window:show-main'),
  collapseToOrb: () => ipcRenderer.send('window:collapse-orb'),
  setOrbPanelExpanded: (expanded) => ipcRenderer.invoke('orb:set-panel', expanded),
  startOrbDrag: (screenX, screenY) => ipcRenderer.send('orb:drag-start', screenX, screenY),
  moveOrbDrag: (screenX, screenY) => ipcRenderer.send('orb:drag-move', screenX, screenY),
  endOrbDrag: () => ipcRenderer.send('orb:drag-end'),
  onUiMode: (cb) => {
    const listener = (_event: Electron.IpcRendererEvent, mode: 'panel' | 'orb'): void => cb(mode)
    ipcRenderer.on('ui:mode', listener)
    return () => ipcRenderer.removeListener('ui:mode', listener)
  },

  buildExe: () => ipcRenderer.invoke('packaging:build-exe'),
  onPackagingStatus: (cb) => {
    const listener = (
      _event: Electron.IpcRendererEvent,
      s: { state: string; message: string },
    ): void => cb(s)
    ipcRenderer.on('packaging:status', listener)
    return () => ipcRenderer.removeListener('packaging:status', listener)
  },
}

contextBridge.exposeInMainWorld('aihub', api)

import { contextBridge, ipcRenderer } from 'electron'
import type {
  AihubApi,
  BackendStatus,
  ServerMessage,
  TaskClearScope,
  UpdateState,
  UserIconState,
  WallpaperPrefs,
} from '../shared/types'

const api: AihubApi = {
  getTaskPage: (status, limit?: number, offset?: number) =>
    ipcRenderer.invoke('tasks:page', status, limit, offset),
  getTasksSummary: () => ipcRenderer.invoke('tasks:summary'),
  openTask: (id) => ipcRenderer.invoke('tasks:open', id),
  ignoreTask: (id) => ipcRenderer.invoke('tasks:ignore', id),
  deleteTask: (id) => ipcRenderer.invoke('tasks:delete', id),
  clearTasks: (scope?: TaskClearScope) => ipcRenderer.invoke('tasks:clear', scope),
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
  setWallpaperPreset: (presetId) => ipcRenderer.invoke('wallpaper:set-preset', presetId),
  clearWallpaper: () => ipcRenderer.invoke('wallpaper:clear'),
  setWallpaperPrefs: (prefs) => {
    const allowed = ['blur', 'dim', 'opacity'] as const
    const unknown = Object.keys(prefs).filter((k) => !(allowed as readonly string[]).includes(k))
    if (unknown.length > 0) console.warn('[preload] setWallpaperPrefs: unknown fields', unknown)
    const filtered: Partial<WallpaperPrefs> = {}
    for (const k of allowed) {
      if (k in prefs) (filtered as Record<string, unknown>)[k] = prefs[k as keyof WallpaperPrefs]
    }
    // 注意：不要在这里 .catch() 吞错——调用方（settings.ts pushPrefs）依赖 reject 来走错误分支，
    // 吞错会把 resolve 值变成 undefined，导致 applyWallpaper(undefined) 解构抛 TypeError。
    return ipcRenderer.invoke('wallpaper:set-prefs', filtered)
  },
  showWallpaperDialog: () => ipcRenderer.invoke('wallpaper:dialog'),

  getUserIcon: () => ipcRenderer.invoke('icon:get'),
  pickUserIcon: () => ipcRenderer.invoke('icon:pick'),
  setUserIconPreset: (presetId) => ipcRenderer.invoke('icon:set-preset', presetId),
  resetUserIcon: () => ipcRenderer.invoke('icon:reset'),
  onIconChanged: (cb) => {
    const listener = (_event: Electron.IpcRendererEvent, state: UserIconState): void => cb(state)
    ipcRenderer.on('icon:changed', listener)
    return () => ipcRenderer.removeListener('icon:changed', listener)
  },

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
  isPackaged: () => ipcRenderer.invoke('app:is-packaged'),
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

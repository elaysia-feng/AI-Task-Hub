import { contextBridge, ipcRenderer } from 'electron'
import type { AihubApi, BackendStatus, ServerMessage } from '../shared/types'

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

  minimizeWindow: () => ipcRenderer.send('window:minimize'),
  closeWindow: () => ipcRenderer.send('window:close'),
}

contextBridge.exposeInMainWorld('aihub', api)

import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { app, BrowserWindow, shell } from 'electron'
import { BackendManager } from './backend'
import { registerIpcHandlers } from './ipc'
import { notifyTaskChanged } from './notification'
import { createTray } from './tray'
import { TaskSocket } from './ws-client'
import { RESOURCES_DIR } from './config'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

let mainWindow: BrowserWindow | null = null
let isQuitting = false

const backend = new BackendManager()
const socket = new TaskSocket()

// 单实例：重复启动时聚焦已有窗口
const gotLock = app.requestSingleInstanceLock()
if (!gotLock) {
  app.quit()
} else {
  app.on('second-instance', () => showMainWindow())
}

function showMainWindow(): void {
  if (!mainWindow) return
  if (mainWindow.isMinimized()) mainWindow.restore()
  mainWindow.show()
  mainWindow.focus()
}

function createMainWindow(): BrowserWindow {
  const win = new BrowserWindow({
    width: 1020,
    height: 660,
    minWidth: 880,
    minHeight: 560,
    frame: false,
    show: false,
    resizable: true,
    backgroundColor: '#0e0f13',
    icon: path.join(RESOURCES_DIR, 'icon.png'),
    webPreferences: {
      preload: path.join(__dirname, '../preload/index.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  })

  win.once('ready-to-show', () => win.show())
  // 关闭按钮 → 最小化到托盘，真正退出走托盘菜单
  win.on('close', (event) => {
    if (!isQuitting) {
      event.preventDefault()
      win.hide()
    }
  })
  win.on('closed', () => {
    mainWindow = null
  })
  // 渲染进程内的外链一律交给系统浏览器
  win.webContents.setWindowOpenHandler(({ url }) => {
    void shell.openExternal(url)
    return { action: 'deny' }
  })

  if (process.env.ELECTRON_RENDERER_URL) {
    void win.loadURL(process.env.ELECTRON_RENDERER_URL)
  } else {
    void win.loadFile(path.join(__dirname, '../renderer/index.html'))
  }
  return win
}

app.whenReady().then(() => {
  app.setAppUserModelId('com.ai-task-hub.desktop')

  mainWindow = createMainWindow()
  registerIpcHandlers(() => mainWindow, backend)

  backend.onStatusChange((status) => {
    mainWindow?.webContents.send('backend:status', status)
  })
  backend.start()

  socket.onMessage((msg) => {
    mainWindow?.webContents.send('task:changed', msg)
    if (msg.type === 'task_changed') {
      notifyTaskChanged(
        msg.task,
        msg.eventType,
        () => Boolean(mainWindow?.isVisible() && mainWindow.isFocused()),
        showMainWindow,
      )
    }
  })
  socket.connect()

  createTray({
    onShow: showMainWindow,
    onQuit: () => {
      isQuitting = true
      app.quit()
    },
  })

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      mainWindow = createMainWindow()
    } else {
      showMainWindow()
    }
  })
})

app.on('before-quit', () => {
  isQuitting = true
  backend.stop()
  socket.close()
})

app.on('window-all-closed', () => {
  // 托盘常驻：所有窗口关闭时不退出（macOS 除外也不退，靠托盘菜单退出）
  if (process.platform === 'darwin') return
})

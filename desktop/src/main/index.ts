import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { app, BrowserWindow, Notification, shell } from 'electron'
import { apiClient } from './api-client'
import { BackendManager } from './backend'
import { registerIpcHandlers } from './ipc'
import { notifyTaskChanged } from './notification'
import {
  ORB_SIZE,
  bindOrbWindow,
  endOrbDrag,
  enterOrbMode,
  enterPanelMode,
  isOrbMode,
  moveOrbDrag,
  setOrbPanelExpanded,
  startOrbDrag,
  toggleOrbVisibility,
} from './orb'
import { createTray, type TrayHandle } from './tray'
import { UpdateManager } from './updater'
import { TaskSocket } from './ws-client'
import { RESOURCES_DIR } from './config'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

let mainWindow: BrowserWindow | null = null
let isQuitting = false

const backend = new BackendManager()
const socket = new TaskSocket()
const updater = new UpdateManager()

const gotLock = app.requestSingleInstanceLock()
if (!gotLock) {
  // 已有实例在跑：把焦点交给旧进程后退出（看起来像「双击没反应」就是这个）
  app.quit()
} else {
  app.on('second-instance', () => {
    focusMainWindow()
  })
}

function focusMainWindow(): void {
  if (!mainWindow || mainWindow.isDestroyed()) {
    mainWindow = createMainWindow()
    bindOrbWindow(() => mainWindow)
  }
  enterPanelMode()
  const win = mainWindow
  if (!win || win.isDestroyed()) return
  if (win.isMinimized()) win.restore()
  win.show()
  win.focus()
  win.moveTop()
  // 短暂置顶，避免藏在其他窗口后面让人以为没打开
  win.setAlwaysOnTop(true)
  setTimeout(() => {
    if (!win.isDestroyed() && !isOrbMode()) win.setAlwaysOnTop(false)
  }, 800)
}

function showMainWindow(): void {
  focusMainWindow()
}

function createMainWindow(): BrowserWindow {
  const win = new BrowserWindow({
    width: 1020,
    height: 660,
    // 最小尺寸在 panel/orb 模式切换时动态设置，这里不能锁 880，否则收不成小球
    minWidth: ORB_SIZE,
    minHeight: ORB_SIZE,
    frame: false,
    show: false,
    resizable: true,
    transparent: true,
    backgroundColor: '#00000000',
    hasShadow: true,
    icon: path.join(RESOURCES_DIR, 'icon.png'),
    webPreferences: {
      preload: path.join(__dirname, '../preload/index.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  })

  win.once('ready-to-show', () => {
    win.show()
    win.webContents.send('ui:mode', 'panel')
  })

  win.on('close', (event) => {
    if (!isQuitting) {
      event.preventDefault()
      enterOrbMode()
    }
  })
  win.on('closed', () => {
    mainWindow = null
  })
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
  bindOrbWindow(() => mainWindow)

  registerIpcHandlers(() => mainWindow, backend, updater, {
    enterOrb: enterOrbMode,
    enterPanel: enterPanelMode,
    setPanelExpanded: setOrbPanelExpanded,
    dragStart: startOrbDrag,
    dragMove: moveOrbDrag,
    dragEnd: endOrbDrag,
    isOrb: isOrbMode,
  })

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
        () => Boolean(mainWindow?.isVisible() && mainWindow.isFocused() && !isOrbMode()),
        showMainWindow,
      )
    }
    refreshTrayUnread()
  })
  socket.connect()

  const trayHandle = createTray({
    onShow: showMainWindow,
    onQuit: () => {
      isQuitting = true
      app.quit()
    },
    onInstallUpdate: () => updater.quitAndInstall(),
    onToggleOrb: toggleOrbVisibility,
  })

  let trayRefreshTimer: NodeJS.Timeout | null = null
  const refreshTrayUnread = (): void => {
    if (trayRefreshTimer) clearTimeout(trayRefreshTimer)
    trayRefreshTimer = setTimeout(() => void updateTrayTooltip(trayHandle), 300)
  }

  void updateTrayTooltip(trayHandle)

  updater.onStateChange((state) => {
    mainWindow?.webContents.send('update:status', state)
    if (state.state === 'downloaded' && state.version) {
      trayHandle.setUpdateReady(state.version)
      new Notification({
        title: 'AI Task Hub 更新就绪',
        body: `v${state.version} 已下载完成，托盘菜单可重启安装`,
      }).show()
    }
  })
  updater.start()

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      mainWindow = createMainWindow()
      bindOrbWindow(() => mainWindow)
    } else {
      showMainWindow()
    }
  })
})

app.on('before-quit', () => {
  isQuitting = true
  backend.stop()
  socket.close()
  updater.stop()
})

async function updateTrayTooltip(trayHandle: TrayHandle): Promise<void> {
  try {
    const queue = await apiClient.listTasks('queue')
    const running = queue.filter((t) => t.status === 'RUNNING').length
    const unread = queue.filter((t) => t.status === 'COMPLETED_UNREAD' || t.status === 'FAILED_UNREAD').length
    const needsInput = queue.filter((t) => t.status === 'NEEDS_INPUT').length
    const parts: string[] = []
    if (running) parts.push(`${running} 执行中`)
    if (unread) parts.push(`${unread} 未读`)
    if (needsInput) parts.push(`${needsInput} 待输入`)
    trayHandle.tray.setToolTip(parts.length ? `AI Task Hub — ${parts.join(' · ')}` : 'AI Task Hub')
  } catch {
    // offline
  }
}

app.on('window-all-closed', () => {
  if (process.platform === 'darwin') return
})

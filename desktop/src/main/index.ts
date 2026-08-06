import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { app, BrowserWindow, crashReporter, Notification, shell } from 'electron'
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
import { killAllTrackedChildren } from './launcher'
import {
  applyPersistedIcon,
  bindIconWindow,
  setTrayHandle,
} from './icon-picker'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

/**
 * Chromium 缓存不应放在 Roaming userData：该目录可能被同步软件或并发开发实例占用，
 * 进而触发 Cache / GPUCache 的 WinError 5。业务偏好仍留在 userData，只迁移浏览器会话数据。
 */
function configureChromiumSessionData(): void {
  const appDirName = app.isPackaged ? 'AI Task Hub' : 'ai-task-hub-desktop'
  const localBase = process.env.LOCALAPPDATA?.trim() || app.getPath('temp')
  const sessionDataDir = path.join(localBase, appDirName, 'Chromium')
  try {
    fs.mkdirSync(sessionDataDir, { recursive: true })
    const legacyLocalStorage = path.join(app.getPath('userData'), 'Local Storage')
    const localStorage = path.join(sessionDataDir, 'Local Storage')
    if (fs.existsSync(legacyLocalStorage) && !fs.existsSync(localStorage)) {
      fs.cpSync(legacyLocalStorage, localStorage, { recursive: true })
    }
    app.setPath('sessionData', sessionDataDir)
  } catch (err) {
    console.warn('[main] Chromium 会话目录迁移失败，继续使用默认目录:', err)
  }
}

// Global exception handlers to prevent silent crashes
process.on('uncaughtException', (err) => {
  console.error('[main] Uncaught exception:', err)
})
process.on('unhandledRejection', (reason) => {
  console.error('[main] Unhandled rejection:', reason)
})

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
  configureChromiumSessionData()
  app.on('second-instance', () => {
    focusMainWindow()
  })
}

function focusMainWindow(): void {
  if (!mainWindow || mainWindow.isDestroyed()) {
    mainWindow = createMainWindow()
    bindOrbWindow(() => mainWindow)
    bindIconWindow(() => mainWindow)
    applyPersistedIcon()
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
  win.webContents.on('will-navigate', (event, url) => {
    // 精确前缀：仅放行本地开发服务器（localhost:/127.0.0.1:）。startsWith('http://localhost')
    // 会误匹配 http://localhost.evil.com 等伪造域名（review MEDIUM）
    if (
      !url.startsWith('app://') &&
      !url.startsWith('file://') &&
      !url.startsWith('http://localhost:') &&
      !url.startsWith('http://127.0.0.1:') &&
      !url.startsWith('https://')
    ) {
      event.preventDefault()
    }
  })
  win.webContents.on('render-process-gone', (_event, details) => {
    if (details.reason === 'clean-exit') return
    console.error(`[main] Renderer process gone: ${details.reason} (exit ${details.exitCode})`)
    // 渲染崩溃 = 黑窗口：延迟自动重载恢复，而不是把用户晾在黑屏上（M20）
    if (!win.isDestroyed()) {
      setTimeout(() => {
        if (!win.isDestroyed()) win.webContents.reload()
      }, 300)
    }
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

  // 本地崩溃转储：无上报服务器，写盘供诊断（M19）
  crashReporter.start({
    productName: 'AI Task Hub',
    companyName: 'AI Task Hub',
    uploadToServer: false,
  })

  mainWindow = createMainWindow()
  bindOrbWindow(() => mainWindow)
  bindIconWindow(() => mainWindow)

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
    if (status === 'online') socket.connect(true) // fresh：后端恢复上线，重置重连退避计数
    else socket.close()
  })
  backend.start()

  socket.onMessage((msg) => {
    mainWindow?.webContents.send('task:changed', msg)
    if (msg.type === 'task_changed') {
      notifyTaskChanged(
        msg.task,
        msg.eventType,
        showMainWindow,
      )
    }
    refreshTrayUnread()
  })
  const trayHandle = createTray({
    onShow: showMainWindow,
    onQuit: () => {
      isQuitting = true
      app.quit()
    },
    onInstallUpdate: () => updater.quitAndInstall(),
    onToggleOrb: toggleOrbVisibility,
  })
  setTrayHandle(trayHandle)
  applyPersistedIcon()

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
      const notif = new Notification({
        title: 'AI Task Hub 更新就绪',
        body: `v${state.version} 已下载完成，托盘菜单可重启安装`,
      })
      // 点通知直接拉出主面板（M21）
      notif.on('click', () => showMainWindow())
      notif.show()
    }
  })
  updater.start()

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      mainWindow = createMainWindow()
      bindOrbWindow(() => mainWindow)
      bindIconWindow(() => mainWindow)
      applyPersistedIcon()
    } else {
      showMainWindow()
    }
  })
})

app.on('before-quit', () => {
  isQuitting = true
  killAllTrackedChildren()
  backend.stop()
  socket.close()
  updater.stop()
})

async function updateTrayTooltip(trayHandle: TrayHandle): Promise<void> {
  try {
    // summary 一条请求拿全 6 种状态准确计数，托盘 tooltip 不随分页截断
    const counts = await apiClient.getTasksSummary()
    const running = counts.RUNNING ?? 0
    const unread = (counts.COMPLETED_UNREAD ?? 0) + (counts.FAILED_UNREAD ?? 0)
    const needsInput = counts.NEEDS_INPUT ?? 0
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

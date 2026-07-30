import path from 'node:path'
import { app, Menu, nativeImage, Tray } from 'electron'
import { RESOURCES_DIR } from './config'

export interface TrayCallbacks {
  onShow: () => void
  onQuit: () => void
}

export function createTray(callbacks: TrayCallbacks): Tray {
  const iconPath = path.join(RESOURCES_DIR, 'tray.png')
  const icon = nativeImage.createFromPath(iconPath)
  const tray = new Tray(icon.isEmpty() ? nativeImage.createEmpty() : icon)

  tray.setToolTip('AI Task Hub')
  tray.setContextMenu(
    Menu.buildFromTemplate([
      { label: '打开主面板', click: callbacks.onShow },
      { type: 'separator' },
      { label: '退出', click: callbacks.onQuit },
    ]),
  )
  tray.on('click', callbacks.onShow)
  return tray
}

/** macOS 上退出前清理（Windows 由系统回收） */
export function destroyTray(tray: Tray | null): void {
  if (tray && process.platform !== 'win32') tray.destroy()
  void app
}

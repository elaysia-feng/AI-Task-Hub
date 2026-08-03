import path from 'node:path'
import { Menu, nativeImage, Tray, type MenuItemConstructorOptions } from 'electron'
import { RESOURCES_DIR } from './config'

export interface TrayCallbacks {
  onShow: () => void
  onQuit: () => void
  onInstallUpdate: () => void
  onToggleOrb?: () => void
}

export interface TrayHandle {
  tray: Tray
  /** 有已下载更新时注入「重启安装」菜单项，无则恢复基础菜单 */
  setUpdateReady(version: string | null): void
}

export function createTray(callbacks: TrayCallbacks): TrayHandle {
  const iconPath = path.join(RESOURCES_DIR, 'tray.png')
  const icon = nativeImage.createFromPath(iconPath)
  const tray = new Tray(icon.isEmpty() ? nativeImage.createEmpty() : icon)

  tray.setToolTip('AI Task Hub')
  tray.on('click', callbacks.onShow)
  tray.on('double-click', callbacks.onShow)

  const buildMenu = (updateVersion: string | null): void => {
    const items: MenuItemConstructorOptions[] = [
      { label: '打开主面板', click: callbacks.onShow },
      { label: '切换悬浮球 / 主面板', click: () => callbacks.onToggleOrb?.() },
    ]
    if (updateVersion) {
      items.push(
        { type: 'separator' },
        { label: `重启安装更新 (v${updateVersion})`, click: callbacks.onInstallUpdate },
      )
    }
    items.push({ type: 'separator' }, { label: '退出', click: callbacks.onQuit })
    tray.setContextMenu(Menu.buildFromTemplate(items))
  }
  buildMenu(null)

  return { tray, setUpdateReady: buildMenu }
}

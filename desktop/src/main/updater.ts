import { app } from 'electron'
import { autoUpdater } from 'electron-updater'
import type { UpdateState } from '../shared/types'

const CHECK_INTERVAL_MS = 4 * 60 * 60 * 1000

type StateListener = (state: UpdateState) => void

/**
 * 自动更新（GitHub Releases）：就绪后查一次 + 每 4h 轮询，自动下载。
 * 仅打包环境生效；开发态全部 no-op，避免 electron-vite dev 下报错。
 */
export class UpdateManager {
  private listeners = new Set<StateListener>()
  private timer: NodeJS.Timeout | null = null
  downloadedVersion: string | null = null

  onStateChange(listener: StateListener): void {
    this.listeners.add(listener)
  }

  get enabled(): boolean {
    return app.isPackaged
  }

  start(): void {
    if (!this.enabled) return
    autoUpdater.autoDownload = true
    autoUpdater.on('checking-for-update', () => this.emit({ state: 'checking' }))
    autoUpdater.on('update-available', (info) => this.emit({ state: 'available', version: info.version }))
    autoUpdater.on('update-not-available', () => this.emit({ state: 'not-available' }))
    autoUpdater.on('download-progress', (p) =>
      this.emit({ state: 'downloading', percent: Math.round(p.percent) }),
    )
    autoUpdater.on('update-downloaded', (info) => {
      this.downloadedVersion = info.version
      this.emit({ state: 'downloaded', version: info.version })
    })
    autoUpdater.on('error', (err) => this.emit({ state: 'error', message: err.message }))
    void this.check()
    this.timer = setInterval(() => void this.check(), CHECK_INTERVAL_MS)
  }

  async check(): Promise<void> {
    if (!this.enabled) return
    try {
      await autoUpdater.checkForUpdates()
    } catch (err) {
      this.emit({ state: 'error', message: err instanceof Error ? err.message : String(err) })
    }
  }

  quitAndInstall(): void {
    if (this.downloadedVersion) autoUpdater.quitAndInstall()
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer)
  }

  private emit(state: UpdateState): void {
    for (const listener of this.listeners) listener(state)
  }
}

import { spawn, type ChildProcess } from 'node:child_process'
import fs from 'node:fs'
import { BACKEND_PYTHON, HEALTH_URL, REPO_ROOT } from './config'
import type { BackendStatus } from '../shared/types'

const POLL_INTERVAL_MS = 2000
const HEALTH_TIMEOUT_MS = 1500
const RESPAWN_COOLDOWN_MS = 30_000

type StatusListener = (status: BackendStatus) => void

/**
 * 后端生命周期管理：
 * 探测 FastAPI 健康状态 → 未运行则尝试用仓库 venv 自动拉起 → 持续轮询。
 */
export class BackendManager {
  private status: BackendStatus = 'connecting'
  private listeners = new Set<StatusListener>()
  private child: ChildProcess | null = null
  private timer: NodeJS.Timeout | null = null
  private lastSpawnAt = 0

  onStatusChange(listener: StatusListener): void {
    this.listeners.add(listener)
  }

  get current(): BackendStatus {
    return this.status
  }

  start(): void {
    void this.tick()
  }

  stop(): void {
    if (this.timer) clearTimeout(this.timer)
    // 不 kill 后端进程：Adapter 上报不应依赖桌面端存活
  }

  private setStatus(next: BackendStatus): void {
    if (this.status === next) return
    this.status = next
    for (const listener of this.listeners) listener(next)
  }

  private async tick(): Promise<void> {
    const healthy = await this.checkHealth()
    if (healthy) {
      this.setStatus('online')
    } else {
      if (this.status === 'online') this.setStatus('offline')
      // 后端中途退出也要能再次拉起，用冷却时间避免高频重启
      if (Date.now() - this.lastSpawnAt > RESPAWN_COOLDOWN_MS) {
        this.lastSpawnAt = Date.now()
        this.trySpawnBackend()
      }
    }
    this.timer = setTimeout(() => void this.tick(), POLL_INTERVAL_MS)
  }

  private async checkHealth(): Promise<boolean> {
    try {
      const res = await fetch(HEALTH_URL, { signal: AbortSignal.timeout(HEALTH_TIMEOUT_MS) })
      return res.ok
    } catch {
      return false
    }
  }

  /** 开发模式下用仓库内 .venv 自动拉起事件服务 */
  private trySpawnBackend(): void {
    if (!fs.existsSync(BACKEND_PYTHON)) {
      console.warn(`[backend] 未找到 Python: ${BACKEND_PYTHON}，请手动启动事件服务`)
      return
    }
    try {
      this.child = spawn(BACKEND_PYTHON, ['-m', 'app.main'], {
        cwd: REPO_ROOT,
        stdio: 'ignore',
        windowsHide: true,
      })
      this.child.unref()
      this.child.on('error', (err) => console.warn('[backend] 启动失败:', err.message))
      console.log('[backend] 已拉起本地事件服务')
    } catch (err) {
      console.warn('[backend] 启动失败:', err)
    }
  }
}

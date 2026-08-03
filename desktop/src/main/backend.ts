import { spawn, type ChildProcess } from 'node:child_process'
import fs from 'node:fs'
import { BACKEND_CMD, BACKEND_CWD, HEALTH_URL } from './config'
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
  private stopped = false

  onStatusChange(listener: StatusListener): void {
    this.listeners.add(listener)
  }

  get current(): BackendStatus {
    return this.status
  }

  start(): void {
    this.stopped = false
    void this.tick()
  }

  stop(): void {
    this.stopped = true
    if (this.timer) clearTimeout(this.timer)
    if (this.child) {
      this.child.kill()
      this.child = null
    }
  }

  private setStatus(next: BackendStatus): void {
    if (this.status === next) return
    this.status = next
    for (const listener of this.listeners) listener(next)
  }

  private async tick(): Promise<void> {
    if (this.stopped) return
    try {
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
    } catch (err) {
      // 任何意外异常都不允许打断轮询：记日志并继续排下一次 tick
      console.error('[backend] tick error:', err)
    }
    if (!this.stopped) this.timer = setTimeout(() => void this.tick(), POLL_INTERVAL_MS)
  }

  private async checkHealth(): Promise<boolean> {
    try {
      const res = await fetch(HEALTH_URL, { signal: AbortSignal.timeout(HEALTH_TIMEOUT_MS) })
      return res.ok
    } catch {
      return false
    }
  }

  /** 自动拉起事件服务（开发态 venv python / 打包态内嵌 exe，由 BACKEND_CMD 决定） */
  private trySpawnBackend(): void {
    if (!fs.existsSync(BACKEND_CMD.exe)) {
      console.warn(`[backend] 未找到后端可执行文件: ${BACKEND_CMD.exe}，请手动启动事件服务`)
      this.setStatus('offline')
      return
    }
    try {
      // 覆盖前先杀掉旧实例，避免重复拉起时遗留孤儿进程（M15）
      if (this.child) {
        this.child.kill()
        this.child = null
      }
      const child = spawn(BACKEND_CMD.exe, BACKEND_CMD.args, {
        cwd: BACKEND_CWD,
        stdio: 'ignore',
        windowsHide: true,
        env: { ...process.env, PYTHONIOENCODING: 'utf-8' },
      })
      this.child = child
      child.unref()
      child.on('error', (err) => {
        console.warn('[backend] 启动失败:', err.message)
        // 通知 renderer，避免用户一直停留在 "connecting"（LOW：spawn 错误仅 console.warn）
        this.setStatus('offline')
      })
      console.log('[backend] 已拉起本地事件服务')
    } catch (err) {
      console.warn('[backend] 启动失败:', err)
      this.setStatus('offline')
    }
  }
}

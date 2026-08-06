import { spawn, type ChildProcess } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { app } from 'electron'
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
        // 后端中途退出也要能再次拉起，用冷却时间避免高频重启。
        // lastSpawnAt 由 trySpawnBackend 在实际拉起时置位：可执行文件缺失时不应消耗
        // 冷却时间，否则「首次启动/用户补装」会被拖慢 30 秒（review MEDIUM）
        if (!this.child && Date.now() - this.lastSpawnAt > RESPAWN_COOLDOWN_MS) {
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

  /**
   * 子进程 stdout/stderr 重定向到日志文件，避免后端在日志初始化前的启动崩溃
   * （导入错误 / 端口占用 / 未捕获异常）全部丢失、无从诊断（review HIGH）。
   * 路径放在 userData/logs，与后端自身日志（backend.log）同目录。
   */
  private childLogPath(): string {
    return path.join(app.getPath('userData'), 'logs', 'backend-console.log')
  }

  /** 自动拉起事件服务（开发态 venv python / 打包态内嵌 exe，由 BACKEND_CMD 决定） */
  private trySpawnBackend(): void {
    if (!fs.existsSync(BACKEND_CMD.exe)) {
      console.warn(`[backend] 未找到后端可执行文件: ${BACKEND_CMD.exe}，请手动启动事件服务`)
      this.setStatus('offline')
      return
    }
    // 已有子进程时等待其健康检查或退出，避免启动较慢时每 30 秒重复拉起。
    if (this.child) return
    this.lastSpawnAt = Date.now() // 仅在真正尝试拉起时消耗冷却时间
    const logPath = this.childLogPath()
    let outFd = -1
    let errFd = -1
    try {
      fs.mkdirSync(path.dirname(logPath), { recursive: true })
      outFd = fs.openSync(logPath, 'a')
      errFd = fs.openSync(logPath, 'a')
      const child = spawn(BACKEND_CMD.exe, BACKEND_CMD.args, {
        cwd: BACKEND_CWD,
        stdio: ['ignore', outFd, errFd],
        windowsHide: true,
        env: { ...process.env, PYTHONIOENCODING: 'utf-8' },
      })
      this.child = child
      child.unref()
      child.on('error', (err) => {
        console.warn('[backend] 启动失败:', err.message)
        if (this.child === child) this.child = null
        // 通知 renderer，避免用户一直停留在 "connecting"（LOW：spawn 错误仅 console.warn）
        this.setStatus('offline')
      })
      child.once('exit', (code, signal) => {
        try {
          fs.closeSync(outFd)
          fs.closeSync(errFd)
        } catch {
          /* 句柄可能已被进程退出清理 */
        }
        if (this.child !== child) return
        this.child = null
        if (this.stopped) return
        console.warn(`[backend] 本地事件服务已退出: code=${code ?? '-'} signal=${signal ?? '-'}，日志见 ${logPath}`)
        this.setStatus('offline')
      })
      console.log(`[backend] 后端进程已启动，等待健康检查（stdout/stderr → ${logPath}）`)
    } catch (err) {
      if (outFd >= 0) {
        try { fs.closeSync(outFd) } catch { /* ignore */ }
      }
      if (errFd >= 0) {
        try { fs.closeSync(errFd) } catch { /* ignore */ }
      }
      console.warn('[backend] 启动失败:', err)
      this.setStatus('offline')
    }
  }
}

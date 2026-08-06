import { spawn } from 'node:child_process'
import os from 'node:os'
import path from 'node:path'
import { shell } from 'electron'
import type { HubTask } from '../shared/types'
import { isSafeExternalUrl, resolveAllowedPath } from './path-util'

// Whitelist of allowed launch targets (user data dirs and repo roots)
const ALLOWED_LAUNCH_PREFIXES: string[] = [
  os.homedir(),
  path.join(os.homedir(), 'Desktop'),
  path.join(os.homedir(), 'Documents'),
  path.join(os.homedir(), 'Projects'),
]

/** Track child processes spawned for lifecycle management */
const childProcesses = new Map<number, NodeJS.Timeout>()

/**
 * 点击任务后的唤起逻辑：不同来源对应不同打开方式。
 * 桌面端采用"点击即视为已读"，由调用方在成功后标记 VIEWED。
 */
export async function openTaskTarget(task: HubTask): Promise<void> {
  if (task.source === 'CHATGPT') {
    // 只放行 http/https，拦截 file://、javascript: 等危险协议（M16）
    if (task.openUrl && isSafeExternalUrl(task.openUrl)) {
      await shell.openExternal(task.openUrl)
      return
    }
    await shell.openExternal('https://chatgpt.com')
    return
  }

  const cwd = validatePath(task.projectPath) ?? os.homedir()
  const command = buildResumeCommand(task)
  await openTerminal(cwd, command)
}

/**
 * Validate that a path is under an allowed prefix to prevent command injection。
 * realpath 解析 junction/symlink、NTFS 大小写不敏感，见 path-util.resolveAllowedPath（M18/M19）。
 */
function validatePath(inputPath: string | null | undefined): string | null {
  return resolveAllowedPath(inputPath, ALLOWED_LAUNCH_PREFIXES)
}

/** Restore lifecycle tracking for a spawned child */
function trackChild(child: ReturnType<typeof spawn>, delayMs = 5_000): void {
  // child.pid 在 'spawn' 事件前是 undefined：此刻登记会让键互相覆盖，先启动的子进程
  // 失去自动回收定时器。推迟到 spawn 完成（pid 已确定）再登记（review HIGH: pid! 断言）
  const pid = child.pid
  if (pid === undefined) {
    child.once('spawn', () => trackChild(child, delayMs))
    return
  }
  const timer = setTimeout(() => {
    child.kill()
    childProcesses.delete(pid)
  }, delayMs)
  childProcesses.set(pid, timer)
  child.once('exit', () => {
    clearTimeout(timer)
    childProcesses.delete(pid)
  })
}

/** Kill all tracked children on shutdown */
export function killAllTrackedChildren(): void {
  for (const [pid, timer] of childProcesses) {
    clearTimeout(timer)
    try { process.kill(pid) } catch { /* ignore */ }
  }
  childProcesses.clear()
}

/** 恢复对应 AI 会话的命令；无会话 ID 时仅打开终端进入项目目录 */
function buildResumeCommand(task: HubTask): string[] | null {
  if (!task.externalTaskId) return null
  switch (task.source) {
    case 'CLAUDE_CODE':
      return ['claude', '--resume', task.externalTaskId]
    case 'CODEX':
      return ['codex', 'resume', task.externalTaskId]
    default:
      return null
  }
}

/** 优先使用 Windows Terminal，缺失时回退到 conhost cmd */
async function openTerminal(cwd: string, command: string[] | null): Promise<void> {
  const args = command ? ['-d', cwd, 'cmd', '/k', ...command] : ['-d', cwd]
  const launched = await trySpawn('wt.exe', args)
  if (launched) return

  const fallback = command
    ? spawn('cmd', ['/c', 'start', '""', '/d', cwd, 'cmd', '/k', ...command], {
        detached: true,
        stdio: 'ignore',
      })
    : spawn('cmd', ['/c', 'start', '""', '/d', cwd, 'cmd'], { detached: true, stdio: 'ignore' })
  trackChild(fallback)
  fallback.unref()
}

function trySpawn(file: string, args: string[]): Promise<boolean> {
  return new Promise((resolve) => {
    try {
      const child = spawn(file, args, { detached: true, stdio: 'ignore' })
      child.on('error', () => resolve(false))
      child.on('spawn', () => {
        trackChild(child)
        child.unref()
        resolve(true)
      })
    } catch {
      resolve(false)
    }
  })
}

/** 主进程（通知文案）与渲染进程（UI 展示）共用的文案映射 */

export const SOURCE_LABELS: Record<string, string> = {
  CHATGPT: 'ChatGPT',
  CLAUDE_CODE: 'Claude Code',
  CODEX: 'Codex',
  OTHER: '其他',
}

export const EVENT_LABELS: Record<string, string> = {
  TASK_STARTED: '开始执行',
  TASK_NEEDS_INPUT: '等待输入',
  TASK_COMPLETED: '已完成',
  TASK_FAILED: '执行失败',
  TASK_VIEWED: '已查看',
  TASK_IGNORED: '已忽略',
}

export const STATUS_LABELS: Record<string, string> = {
  RUNNING: '执行中',
  NEEDS_INPUT: '等待输入',
  COMPLETED_UNREAD: '已完成',
  FAILED_UNREAD: '执行失败',
  VIEWED: '已查看',
  IGNORED: '已忽略',
}

const NOISE_PREFIXES = ['<task-notification', '<task-', '<teammate', '<agent-', 'system:']
const LOG_LINE_RE = /^\d{1,2}:\d{2}:\d{2}\b/
const CODE_START_RE =
  /^(if\s*\(|function\s|const\s|let\s|var\s|import\s|from\s|def\s|class\s|export\s|#include)/
const GENERIC_WAIT_RE = /waiting for your input|needs your permission|claude code 等待/i
const XML_TAG_RE =
  /<(summary|title|message|description|subject|task|content)[^>]*>([\s\S]*?)<\/\1>/i

function isNoiseTitle(text: string): boolean {
  const t = text.trim()
  if (!t) return true
  const low = t.toLowerCase()
  if (NOISE_PREFIXES.some((p) => low.startsWith(p))) return true
  if (LOG_LINE_RE.test(t) || t.slice(0, 80).toLowerCase().includes('[vite]')) return true
  if (CODE_START_RE.test(t)) return true
  if (GENERIC_WAIT_RE.test(t) && t.length < 80) return true
  if ((t.match(/</g) ?? []).length >= 2 && t.replace(/<[^>]+>/g, '').trim().length < 8) return true
  return false
}

function extractFromMarkup(text: string): string | null {
  const match = XML_TAG_RE.exec(text)
  if (!match) return null
  const inner = match[2].replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim()
  if (!inner || inner.length < 4 || isNoiseTitle(inner)) return null
  return inner.length > 50 ? `${inner.slice(0, 50)}…` : inner
}

function projectTitle(projectPath?: string | null): string | null {
  if (!projectPath) return null
  const name = projectPath.replace(/[/\\]+$/, '').split(/[/\\]/).pop()
  return name ? `${name} 会话` : null
}

function shorten(text: string, limit = 50): string {
  const cleaned = text.trim().replace(/\s+/g, ' ')
  return cleaned.length > limit ? `${cleaned.slice(0, limit)}…` : cleaned
}

/**
 * 任务展示标题：对齐 Codex「用户提问作主题」。
 * 过滤 OMC 注入 / 日志 / 代码噪声；存量烂标题也能在 UI 层自愈。
 */
export function displayTitle(task: {
  title?: string | null
  contentPreview?: string | null
  projectPath?: string | null
}): string {
  const rawTitle = (task.title ?? '').trim()
  if (rawTitle && !isNoiseTitle(rawTitle)) return rawTitle

  const fromMarkup = extractFromMarkup(rawTitle)
  if (fromMarkup) return fromMarkup

  const preview = (task.contentPreview ?? '').trim().replace(/\s+/g, ' ')
  if (preview && !isNoiseTitle(preview)) return shorten(preview)

  const previewMarkup = extractFromMarkup(preview)
  if (previewMarkup) return previewMarkup

  return projectTitle(task.projectPath) ?? '(无标题任务)'
}

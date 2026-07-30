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

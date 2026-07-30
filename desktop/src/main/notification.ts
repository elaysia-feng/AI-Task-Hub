import { Notification } from 'electron'
import type { HubTask } from '../shared/types'

const SOURCE_LABELS: Record<string, string> = {
  CHATGPT: 'ChatGPT',
  CLAUDE_CODE: 'Claude Code',
  CODEX: 'Codex',
  OTHER: '其他',
}

const EVENT_LABELS: Record<string, string> = {
  TASK_COMPLETED: '任务完成',
  TASK_FAILED: '任务失败',
  TASK_NEEDS_INPUT: '等待输入',
}

/** 系统桌面通知：仅在窗口不可见时弹出，避免打扰 */
export function notifyTaskChanged(
  task: HubTask,
  eventType: string,
  isWindowVisible: () => boolean,
  onClick: () => void,
): void {
  if (isWindowVisible()) return
  const eventLabel = EVENT_LABELS[eventType]
  if (!eventLabel || !Notification.isSupported()) return

  const sourceLabel = SOURCE_LABELS[task.source] ?? task.source
  const notification = new Notification({
    title: `[${sourceLabel}] ${eventLabel}`,
    body: task.title ?? task.contentPreview ?? '',
    silent: false,
  })
  notification.on('click', onClick)
  notification.show()
}

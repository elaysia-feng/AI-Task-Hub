import { nativeImage, Notification } from 'electron'
import type { HubTask } from '../shared/types'
import { displayTitle } from '../shared/labels'
import { getUserIconState } from './icon-picker'

const SOURCE_LABELS: Record<string, string> = {
  CHATGPT: 'ChatGPT',
  CLAUDE_CODE: 'Claude Code',
  CODEX: 'Codex',
  OTHER: '其他',
}

const EVENT_PRESENTATION: Record<string, { mark: string; label: string }> = {
  TASK_COMPLETED: { mark: '✓', label: '任务已完成' },
  TASK_FAILED: { mark: '!', label: '任务执行失败' },
  TASK_NEEDS_INPUT: { mark: '?', label: '任务等待输入' },
}

/** 系统桌面通知：关键状态始终通知，由 Windows 负责最终卡片样式。 */
export function notifyTaskChanged(
  task: HubTask,
  eventType: string,
  onClick: () => void,
): void {
  const presentation = EVENT_PRESENTATION[eventType]
  if (!presentation || !Notification.isSupported()) return

  const sourceLabel = SOURCE_LABELS[task.source] ?? task.source
  const projectName = task.projectPath?.replace(/[/\\]+$/, '').split(/[/\\]/).pop()
  const iconState = getUserIconState()
  const icon = iconState.dataUrl ? nativeImage.createFromDataURL(iconState.dataUrl) : undefined
  const notification = new Notification({
    id: `task-${task.id}-${eventType}`,
    groupId: `tasks-${task.source}`,
    groupTitle: `${sourceLabel} 任务`,
    title: `${presentation.mark} ${sourceLabel} · ${presentation.label}`,
    body: projectName ? `${displayTitle(task)}\n${projectName}` : displayTitle(task),
    icon: icon && !icon.isEmpty() ? icon : undefined,
    silent: false,
    urgency: 'normal',
    actions: [{ type: 'button', text: '打开任务中心' }],
  })
  notification.on('click', onClick)
  notification.on('action', onClick)
  notification.show()
}

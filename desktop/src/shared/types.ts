/** 主进程 / 预加载 / 渲染进程共享类型，与服务端 camelCase API 对齐 */

export type TaskStatus =
  | 'RUNNING'
  | 'NEEDS_INPUT'
  | 'COMPLETED_UNREAD'
  | 'FAILED_UNREAD'
  | 'VIEWED'
  | 'IGNORED'

export type TaskSource = 'CHATGPT' | 'CLAUDE_CODE' | 'CODEX' | 'OTHER'

export interface HubTask {
  id: number
  source: TaskSource
  externalTaskId: string | null
  eventType: string
  title: string | null
  contentPreview: string | null
  projectPath: string | null
  openTarget: string | null
  openUrl: string | null
  status: TaskStatus
  createdAt: string
  completedAt: string | null
  viewedAt: string | null
}

export interface TaskChangedMessage {
  type: 'task_changed'
  eventType: string
  task: HubTask
}

export interface TaskDeletedMessage {
  type: 'task_deleted'
  taskId: number
}

export interface TasksClearedMessage {
  type: 'tasks_cleared'
  deleted: number
}

export interface TasksReadAllMessage {
  type: 'tasks_read_all'
  count: number
}

export type ServerMessage = TaskChangedMessage | TaskDeletedMessage | TasksClearedMessage | TasksReadAllMessage

export type BackendStatus = 'connecting' | 'online' | 'offline'

export type TaskView = 'queue' | 'history'

/** preload 通过 contextBridge 暴露给渲染进程的 API（window.aihub） */
export interface AihubApi {
  getQueue(): Promise<HubTask[]>
  getHistory(): Promise<HubTask[]>
  openTask(id: number): Promise<void>
  ignoreTask(id: number): Promise<void>
  deleteTask(id: number): Promise<void>
  clearTasks(): Promise<number>
  readAllTasks(): Promise<number>
  getBackendStatus(): Promise<BackendStatus>
  onTaskChanged(cb: (msg: ServerMessage) => void): () => void
  onBackendStatus(cb: (status: BackendStatus) => void): () => void
  minimizeWindow(): void
  closeWindow(): void
}

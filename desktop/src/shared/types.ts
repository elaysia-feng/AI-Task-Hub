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

/** 各状态任务总数（summary 端点，服务端缺失状态补 0） */
export type TaskStatusSummary = Record<TaskStatus, number>

/** 任务列表分页结果：服务端返回 hasMore 供前端翻页，避免历史无限增长时一次载入全表 */
export interface TaskListResult {
  tasks: HubTask[]
  hasMore: boolean
}
export type TaskLoadState = 'loading' | 'ready' | 'error'
export type TaskSort = 'newest' | 'oldest'

export interface UpdateState {
  state: 'checking' | 'available' | 'not-available' | 'downloading' | 'downloaded' | 'error'
  version?: string
  percent?: number
  message?: string
}

export interface TaskEventRecord {
  id: number
  taskId: number
  eventType: string
  occurredAt: string
  payload: Record<string, unknown>
}

export interface ServerStatus {
  status: string
  version: string
  uptimeSec: number
  db: { ok: boolean; host: string; port: number; database: string }
  tasks: number | null
  events: number | null
  logFile: string
}

export interface IntegrationsStatus {
  claudeCode: { installed: boolean; settingsPath: string }
  codex: {
    installed: boolean
    configPath: string
    forwardTarget: boolean
    exeRunning: boolean
    stale: boolean
    processCount: number
  }
  chatgpt: {
    installed: boolean
    lastHeartbeat: number | null
    version: string | null
    extensionDir: string
  }
  backend: { version: string; python: string }
}

export interface InstallResult {
  success: boolean
  changed: boolean
  error?: string
  forwardTarget?: boolean
}

/** 本地壁纸外观偏好（blur/dim 为强度，opacity 为面板不透明度百分比） */
export interface WallpaperPrefs {
  blur: number
  dim: number
  opacity: number
}

export interface WallpaperState {
  hasImage: boolean
  dataUrl: string | null
  prefs: WallpaperPrefs
}

/** 应用图标偏好：内置预设（粉发少女默认）或本地自定义图片 */
export type UserIconPrefs =
  | { source: 'preset'; presetId: string }
  | { source: 'custom' }

export interface UserIconPresetMeta {
  id: string
  name: string
  /** 相对 resources/ 的路径（默认 anime-head.png，扩展放 presets/xxx.png） */
  file: string
}

export interface UserIconState {
  prefs: UserIconPrefs
  /** 当前生效图标的数据 URL（预设或自定义图），异常时为 null */
  dataUrl: string | null
  presets: UserIconPresetMeta[]
}

/** preload 通过 contextBridge 暴露给渲染进程的 API（window.aihub） */
export interface AihubApi {
  /** 按单个状态（种类）分页拉取；每个状态一条独立分页流 */
  getTaskPage(status: TaskStatus, limit?: number, offset?: number): Promise<TaskListResult>
  /** 各状态任务总数，供状态 chip/标题显示准确计数 */
  getTasksSummary(): Promise<TaskStatusSummary>
  openTask(id: number): Promise<void>
  ignoreTask(id: number): Promise<void>
  deleteTask(id: number): Promise<void>
  clearTasks(): Promise<number>
  readAllTasks(): Promise<number>
  getBackendStatus(): Promise<BackendStatus>
  onTaskChanged(cb: (msg: ServerMessage) => void): () => void
  onBackendStatus(cb: (status: BackendStatus) => void): () => void
  checkUpdates(): Promise<void>
  installUpdate(): void
  onUpdateStatus(cb: (state: UpdateState) => void): () => void
  getServerStatus(): Promise<ServerStatus>
  getIntegrations(): Promise<IntegrationsStatus>
  installClaude(): Promise<InstallResult>
  installCodex(): Promise<InstallResult>
  getTaskEvents(taskId: number): Promise<TaskEventRecord[]>
  openPath(target: string): Promise<void>
  getWallpaper(): Promise<WallpaperState>
  pickWallpaper(): Promise<WallpaperState>
  clearWallpaper(): Promise<WallpaperState>
  setWallpaperPrefs(prefs: Partial<WallpaperPrefs>): Promise<WallpaperState>
  showWallpaperDialog(): Promise<'pick' | 'clear' | 'cancel'>
  getUserIcon(): Promise<UserIconState>
  pickUserIcon(): Promise<UserIconState>
  setUserIconPreset(presetId: string): Promise<UserIconState>
  resetUserIcon(): Promise<UserIconState>
  onIconChanged(cb: (state: UserIconState) => void): () => void
  minimizeWindow(): void
  closeWindow(): void
  /** 显示主面板 */
  showMainWindow(): void
  /** 收起为悬浮球（同一窗口） */
  collapseToOrb(): void
  /** 小球模式：展开/收起悬停面板（改窗口尺寸）；返回面板展开方向 */
  setOrbPanelExpanded(expanded: boolean): Promise<'up' | 'down' | null>
  /** 悬浮球拖动 */
  startOrbDrag(screenX: number, screenY: number): void
  moveOrbDrag(screenX: number, screenY: number): void
  endOrbDrag(): void
  /** 主进程切换 panel / orb 模式 */
  onUiMode(cb: (mode: 'panel' | 'orb') => void): () => void
  /** 设置页：确认后生成本地 exe 安装包 */
  buildExe(): Promise<{ ok: boolean; cancelled?: boolean; message: string; distDir?: string; missing?: 'nsis' | 'backend' | 'python' }>
  onPackagingStatus(cb: (s: { state: string; message: string }) => void): () => void
  /** 是否为打包版（安装版内无源码仓库，不能应用内打包，UI 据此隐藏按钮） */
  isPackaged(): Promise<boolean>
}

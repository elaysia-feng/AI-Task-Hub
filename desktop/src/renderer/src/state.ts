import type {
  BackendStatus,
  HubTask,
  TaskLoadState,
  TaskSort,
  TaskSource,
  TaskStatus,
  UpdateState,
} from '../../shared/types'

export type View = 'queue' | 'history' | 'settings'

/** 全部任务状态（与后端 shared/constants.py ALL_STATUSES 对齐） */
export const ALL_STATUSES: TaskStatus[] = [
  'RUNNING',
  'NEEDS_INPUT',
  'COMPLETED_UNREAD',
  'FAILED_UNREAD',
  'VIEWED',
  'IGNORED',
]

/** 队列页展示的状态（未读队列） */
export const QUEUE_STATUSES: TaskStatus[] = ALL_STATUSES.slice(0, 4)
/** 历史页展示的状态 */
export const HISTORY_STATUSES: TaskStatus[] = ['VIEWED', 'IGNORED']

function zeroSummary(): Record<TaskStatus, number> {
  const summary = {} as Record<TaskStatus, number>
  for (const s of ALL_STATUSES) summary[s] = 0
  return summary
}

export interface AppState {
  view: View
  /** 已加载的队列任务（RUNNING/NEEDS_INPUT/COMPLETED_UNREAD/FAILED_UNREAD 各页合并） */
  queue: HubTask[]
  /** 已加载的历史任务（VIEWED/IGNORED 各页合并） */
  history: HubTask[]
  /** 每个种类（状态）独立分页：该状态是否还有下一页 */
  bucketHasMore: Partial<Record<TaskStatus, boolean>>
  /** 各状态任务总数（summary 端点，服务端准确值），供 chip/标题计数 */
  statusCounts: Record<TaskStatus, number>
  backend: BackendStatus
  taskLoadState: TaskLoadState
  selectedTaskId: number | null
  search: string
  sourceFilter: TaskSource | 'all'
  statusFilter: TaskStatus | 'all'
  sort: TaskSort
  updateState: UpdateState | null
}

export const state: AppState = {
  view: 'queue',
  queue: [],
  history: [],
  bucketHasMore: {},
  statusCounts: zeroSummary(),
  backend: 'connecting',
  taskLoadState: 'loading',
  selectedTaskId: null,
  search: '',
  sourceFilter: 'all',
  statusFilter: 'all',
  sort: 'newest',
  updateState: null,
}
// NOTE: Object.freeze only prevents modification of the array contents.
// Property reassignment (state.queue = newQueue) bypasses freeze since the
// state object itself is not frozen. To enforce immutability, the full
// state object would need to be replaced with a read-only Proxy.
Object.freeze(state.queue)
Object.freeze(state.history)

/** 状态变更后由 emit 触发整页重渲染（渲染函数读取本模块 state） */
const listeners = new Set<() => void>()

export function subscribe(fn: () => void): void {
  listeners.add(fn)
}

export function emit(): void {
  for (const fn of listeners) fn()
}

export function findTask(id: number): HubTask | undefined {
  return state.queue.find((t) => t.id === id) ?? state.history.find((t) => t.id === id)
}

/** 某状态（种类）当前已加载的任务数；每个种类独立分页，loadMore 用它当 offset。 */
export function loadedTaskCount(status: TaskStatus): number {
  const base = QUEUE_STATUSES.includes(status) ? state.queue : state.history
  return base.filter((t) => t.status === status).length
}

/** 当前视图下经搜索/来源过滤后的任务集 */
export function filteredTasks(): HubTask[] {
  const base = state.view === 'history' ? state.history : state.queue
  const keyword = state.search.trim().toLowerCase()
  const filtered = base.filter((t) => {
    if (state.sourceFilter !== 'all' && t.source !== state.sourceFilter) return false
    if (state.statusFilter !== 'all' && t.status !== state.statusFilter) return false
    if (
      keyword &&
      !(t.title ?? '').toLowerCase().includes(keyword) &&
      !(t.projectPath ?? '').toLowerCase().includes(keyword) &&
      !(t.contentPreview ?? '').toLowerCase().includes(keyword)
    ) {
      return false
    }
    return true
  })
  const direction = state.sort === 'newest' ? -1 : 1
  return [...filtered].sort((a, b) => direction * (taskTime(a) - taskTime(b)))
}

function taskTime(task: HubTask): number {
  const value = Date.parse(task.viewedAt ?? task.completedAt ?? task.createdAt)
  return Number.isNaN(value) ? 0 : value
}

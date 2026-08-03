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

export interface AppState {
  view: View
  queue: HubTask[]
  history: HubTask[]
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

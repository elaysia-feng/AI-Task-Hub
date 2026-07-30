import type { BackendStatus, HubTask, TaskSource, UpdateState } from '../../shared/types'

export type View = 'queue' | 'history' | 'settings'

export interface AppState {
  view: View
  queue: HubTask[]
  history: HubTask[]
  backend: BackendStatus
  selectedTaskId: number | null
  search: string
  sourceFilter: TaskSource | 'all'
  updateState: UpdateState | null
}

export const state: AppState = {
  view: 'queue',
  queue: [],
  history: [],
  backend: 'connecting',
  selectedTaskId: null,
  search: '',
  sourceFilter: 'all',
  updateState: null,
}

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
  return base.filter((t) => {
    if (state.sourceFilter !== 'all' && t.source !== state.sourceFilter) return false
    if (!keyword) return true
    return (
      (t.title ?? '').toLowerCase().includes(keyword) ||
      (t.projectPath ?? '').toLowerCase().includes(keyword) ||
      (t.contentPreview ?? '').toLowerCase().includes(keyword)
    )
  })
}

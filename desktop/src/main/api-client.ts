import { API_BASE } from './config'
import type {
  HubTask,
  InstallResult,
  IntegrationsStatus,
  ServerStatus,
  TaskClearScope,
  TaskEventRecord,
  TaskListResult,
  TaskSnapshot,
  TaskStatus,
  TaskStatusSummary,
} from '../shared/types'

const TIMEOUT_MS = 3000
const INTEGRATIONS_TIMEOUT_MS = 10000

async function request<T>(path: string, init?: RequestInit, timeoutMs = TIMEOUT_MS): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    ...init,
    signal: AbortSignal.timeout(timeoutMs),
    headers: { 'Content-Type': 'application/json', ...init?.headers },
  })
  if (!res.ok) throw new Error(`${init?.method ?? 'GET'} ${path} → ${res.status}`)
  return (await res.json()) as T
}

export const apiClient = {
  async listTasksByStatus(status: TaskStatus, limit?: number, offset?: number): Promise<TaskListResult> {
    const params = new URLSearchParams({ status })
    if (limit !== undefined) params.set('limit', String(limit))
    if (offset !== undefined) params.set('offset', String(offset))
    const data = await request<{ tasks: HubTask[]; hasMore: boolean }>(`/api/tasks?${params}`)
    return { tasks: data.tasks, hasMore: data.hasMore }
  },

  async getTasksSummary(): Promise<TaskStatusSummary> {
    const data = await request<{ counts: TaskStatusSummary }>('/api/tasks/summary')
    return data.counts
  },

  getTaskSnapshot(limit?: number): Promise<TaskSnapshot> {
    const params = new URLSearchParams()
    if (limit !== undefined) params.set('limit', String(limit))
    const query = params.size > 0 ? '?' + params.toString() : ''
    return request('/api/tasks/snapshot' + query)
  },

  async getTask(id: number): Promise<HubTask> {
    const data = await request<{ task: HubTask }>(`/api/tasks/${id}`)
    return data.task
  },

  markViewed(id: number): Promise<void> {
    return request(`/api/tasks/${id}/view`, { method: 'POST' })
  },

  markIgnored(id: number): Promise<void> {
    return request(`/api/tasks/${id}/ignore`, { method: 'POST' })
  },

  deleteTask(id: number): Promise<void> {
    return request(`/api/tasks/${id}`, { method: 'DELETE' })
  },

  async clearTasks(scope: TaskClearScope = 'all'): Promise<number> {
    // 后端自 303f6b6 起要求 confirm=true 防误删；漏带会 400（一键清理「用不了」的根因）
    // scope=queue/history 按 tab 独立清理（后端 0839… 起支持），默认 all 全清
    // URLSearchParams 负责编码，避免 scope 直拼进 URL 造成参数注入（review MEDIUM）
    const params = new URLSearchParams({ confirm: 'true', scope })
    const data = await request<{ success: boolean; deleted: number }>(
      `/api/tasks?${params}`,
      { method: 'DELETE' },
    )
    return data.deleted
  },

  async readAllTasks(): Promise<number> {
    const data = await request<{ success: boolean; count: number }>('/api/tasks/read-all', { method: 'POST' })
    return data.count
  },

  async getTaskEvents(taskId: number): Promise<TaskEventRecord[]> {
    const data = await request<{ events: TaskEventRecord[] }>(`/api/tasks/${taskId}/events`)
    return data.events
  },

  getServerStatus(): Promise<ServerStatus> {
    return request('/api/status')
  },

  getIntegrations(): Promise<IntegrationsStatus> {
    // Windows 首次进程体检可能略慢，单独放宽；普通任务 API 仍维持 3 秒超时。
    return request('/api/integrations/status', undefined, INTEGRATIONS_TIMEOUT_MS)
  },

  installClaude(): Promise<InstallResult> {
    return request('/api/integrations/claude-code/install', { method: 'POST' })
  },

  installCodex(): Promise<InstallResult> {
    return request('/api/integrations/codex/install', { method: 'POST' })
  },
}

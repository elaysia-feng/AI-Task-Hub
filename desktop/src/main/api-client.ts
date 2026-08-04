import { API_BASE } from './config'
import type {
  HubTask,
  InstallResult,
  IntegrationsStatus,
  ServerStatus,
  TaskEventRecord,
  TaskListResult,
  TaskStatus,
  TaskStatusSummary,
} from '../shared/types'

const TIMEOUT_MS = 3000

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    ...init,
    signal: AbortSignal.timeout(TIMEOUT_MS),
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

  async clearTasks(): Promise<number> {
    const data = await request<{ success: boolean; deleted: number }>('/api/tasks', { method: 'DELETE' })
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
    return request('/api/integrations/status')
  },

  installClaude(): Promise<InstallResult> {
    return request('/api/integrations/claude-code/install', { method: 'POST' })
  },

  installCodex(): Promise<InstallResult> {
    return request('/api/integrations/codex/install', { method: 'POST' })
  },
}

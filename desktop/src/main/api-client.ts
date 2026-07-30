import { API_BASE } from './config'
import type { HubTask, TaskView } from '../shared/types'

const TIMEOUT_MS = 3000

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    ...init,
    signal: AbortSignal.timeout(TIMEOUT_MS),
    headers: { 'Content-Type': 'application/json' },
  })
  if (!res.ok) throw new Error(`${init?.method ?? 'GET'} ${path} → ${res.status}`)
  return (await res.json()) as T
}

export const apiClient = {
  async listTasks(view: TaskView): Promise<HubTask[]> {
    const data = await request<{ tasks: HubTask[] }>(`/api/tasks?view=${view}`)
    return data.tasks
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
}

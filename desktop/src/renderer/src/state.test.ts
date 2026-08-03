import { beforeEach, describe, expect, it } from 'vitest'
import type { HubTask } from '../../shared/types'
import { filteredTasks, state } from './state'

function makeTask(partial: Partial<HubTask>): HubTask {
  return {
    id: 1,
    source: 'OTHER',
    externalTaskId: null,
    eventType: 'TASK_COMPLETED',
    title: null,
    contentPreview: null,
    projectPath: null,
    openTarget: null,
    openUrl: null,
    status: 'COMPLETED_UNREAD',
    createdAt: new Date().toISOString(),
    completedAt: null,
    viewedAt: null,
    ...partial,
  }
}

describe('filteredTasks', () => {
  beforeEach(() => {
    state.view = 'queue'
    state.search = ''
    state.sourceFilter = 'all'
    state.statusFilter = 'all'
    state.sort = 'newest'
    state.queue = [
      makeTask({ id: 1, source: 'CODEX', title: '修复登录页样式', projectPath: 'D:/proj/web', createdAt: '2026-07-30T10:00:00' }),
      makeTask({ id: 2, source: 'CHATGPT', title: '解释快排', projectPath: 'D:/proj/algo', status: 'NEEDS_INPUT', createdAt: '2026-07-30T12:00:00' }),
      makeTask({ id: 3, source: 'CLAUDE_CODE', title: '重构 API 层', projectPath: 'D:/proj/web', createdAt: '2026-07-30T11:00:00' }),
    ]
    state.history = [makeTask({ id: 9, source: 'CODEX', title: '历史任务', status: 'VIEWED' })]
  })

  it('默认返回当前视图全部任务', () => {
    expect(filteredTasks().map((t) => t.id)).toEqual([2, 3, 1])
  })

  it('历史视图取 history 列表', () => {
    state.view = 'history'
    expect(filteredTasks().map((t) => t.id)).toEqual([9])
  })

  it('按来源过滤', () => {
    state.sourceFilter = 'CODEX'
    expect(filteredTasks().map((t) => t.id)).toEqual([1])
  })

  it('搜索命中标题与路径（大小写不敏感）', () => {
    state.search = 'web'
    expect(filteredTasks().map((t) => t.id)).toEqual([3, 1])
    state.search = '快排'
    expect(filteredTasks().map((t) => t.id)).toEqual([2])
    state.search = 'WEB'
    expect(filteredTasks().map((t) => t.id)).toEqual([3, 1])
  })

  it('搜索与来源叠加', () => {
    state.search = 'web'
    state.sourceFilter = 'CLAUDE_CODE'
    expect(filteredTasks().map((t) => t.id)).toEqual([3])
  })

  it('无匹配返回空', () => {
    state.search = '不存在的词'
    expect(filteredTasks()).toEqual([])
  })

  it('按状态过滤并支持旧任务优先', () => {
    state.statusFilter = 'COMPLETED_UNREAD'
    state.sort = 'oldest'
    expect(filteredTasks().map((t) => t.id)).toEqual([1, 3])
  })

  it('历史页最新优先使用查看时间', () => {
    state.view = 'history'
    state.history = [
      makeTask({
        id: 8,
        status: 'VIEWED',
        createdAt: '2026-07-29T10:00:00',
        viewedAt: '2026-07-30T13:00:00',
      }),
      makeTask({
        id: 9,
        status: 'VIEWED',
        createdAt: '2026-07-30T12:00:00',
        viewedAt: '2026-07-30T12:30:00',
      }),
    ]
    expect(filteredTasks().map((t) => t.id)).toEqual([8, 9])
  })
})

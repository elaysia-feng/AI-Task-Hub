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
    state.queue = [
      makeTask({ id: 1, source: 'CODEX', title: '修复登录页样式', projectPath: 'D:/proj/web' }),
      makeTask({ id: 2, source: 'CHATGPT', title: '解释快排', projectPath: 'D:/proj/algo' }),
      makeTask({ id: 3, source: 'CLAUDE_CODE', title: '重构 API 层', projectPath: 'D:/proj/web' }),
    ]
    state.history = [makeTask({ id: 9, source: 'CODEX', title: '历史任务', status: 'VIEWED' })]
  })

  it('默认返回当前视图全部任务', () => {
    expect(filteredTasks().map((t) => t.id)).toEqual([1, 2, 3])
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
    expect(filteredTasks().map((t) => t.id)).toEqual([1, 3])
    state.search = '快排'
    expect(filteredTasks().map((t) => t.id)).toEqual([2])
    state.search = 'WEB'
    expect(filteredTasks().map((t) => t.id)).toEqual([1, 3])
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
})

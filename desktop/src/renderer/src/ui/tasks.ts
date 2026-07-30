/* 任务视图：待处理 / 历史 + 搜索筛选 + 右侧详情面板（事件时间线 + 原始载荷） */

import type { HubTask, TaskSource, TaskStatus } from '../../../shared/types'
import { EVENT_LABELS, SOURCE_LABELS, STATUS_LABELS } from '../../../shared/labels'
import { emit, filteredTasks, findTask, state } from '../state'
import { h, showToast, svgIcon, type IconName } from './dom'
import { formatRelativeTime } from '../time'

const QUEUE_SECTIONS: Array<{ status: TaskStatus; title: string; color: string }> = [
  { status: 'NEEDS_INPUT', title: '等待输入', color: 'var(--st-input)' },
  { status: 'COMPLETED_UNREAD', title: '已完成', color: 'var(--st-done)' },
  { status: 'FAILED_UNREAD', title: '失败', color: 'var(--st-fail)' },
]

const SOURCE_OPTIONS: Array<{ value: TaskSource | 'all'; label: string }> = [
  { value: 'all', label: '全部来源' },
  { value: 'CHATGPT', label: 'ChatGPT' },
  { value: 'CLAUDE_CODE', label: 'Claude Code' },
  { value: 'CODEX', label: 'Codex' },
  { value: 'OTHER', label: '其他' },
]

export function renderTasksView(container: HTMLElement): void {
  const title = state.view === 'history' ? '历史' : '待处理'

  const clearBtn = makeClearButton()
  clearBtn.disabled = state.queue.length + state.history.length === 0
  const readAllBtn = makeReadAllButton()
  readAllBtn.disabled = unreadCount() === 0

  container.append(
    h('div', 'content-header', [
      h('h1', '', [title]),
      h('span', 'summary', [summaryText()]),
      h('div', 'header-actions', [readAllBtn, clearBtn]),
    ]),
    makeFilterBar(),
  )

  if (state.backend === 'offline') {
    container.append(
      h('div', 'offline-banner', ['本地事件服务未连接，等待自动恢复…恢复后任务会自动同步。']),
    )
  }

  const selected = state.selectedTaskId !== null ? findTask(state.selectedTaskId) : undefined
  const listArea = h('div', 'list-area')
  if (state.view === 'queue') renderQueue(listArea)
  else renderHistory(listArea)

  if (selected) {
    const body = h('div', 'tasks-split', [listArea, makeDetailPane(selected)])
    container.append(body)
  } else {
    container.append(listArea)
  }
}

/* ---------- 过滤栏 ---------- */

function makeFilterBar(): HTMLElement {
  const search = h('input', 'filter-search') as HTMLInputElement
  search.type = 'search'
  search.placeholder = '搜索标题 / 路径 / 摘要…'
  search.value = state.search
  search.oninput = () => {
    state.search = search.value
    emit()
  }

  const select = h('select', 'filter-source') as HTMLSelectElement
  for (const opt of SOURCE_OPTIONS) {
    const el = h('option', '', [opt.label]) as HTMLOptionElement
    el.value = opt.value
    if (state.sourceFilter === opt.value) el.selected = true
    select.append(el)
  }
  select.onchange = () => {
    state.sourceFilter = select.value as TaskSource | 'all'
    emit()
  }

  return h('div', 'filter-bar', [search, select])
}

/* ---------- 摘要与按钮 ---------- */

function summaryText(): string {
  if (state.view === 'history') {
    return state.history.length ? `共 ${state.history.length} 条记录` : ''
  }
  if (state.queue.length === 0) return ''
  const needsInput = state.queue.filter((t) => t.status === 'NEEDS_INPUT').length
  return needsInput > 0
    ? `${state.queue.length} 个任务 · ${needsInput} 个等待你的输入`
    : `${state.queue.length} 个任务待查看`
}

function unreadCount(): number {
  return state.queue.filter((t) => t.status === 'COMPLETED_UNREAD' || t.status === 'FAILED_UNREAD').length
}

/* 一键已读：非破坏性操作，单击即执行；不动等待输入的任务 */
function makeReadAllButton(): HTMLButtonElement {
  const btn = h('button', 'btn read-all-btn', [svgIcon('check'), '一键已读'])
  btn.title = '将所有已完成/失败的未读任务标记为已读'
  btn.onclick = async () => {
    btn.disabled = true
    try {
      const count = await window.aihub.readAllTasks()
      showToast(count > 0 ? `已将 ${count} 个任务标记为已读` : '没有未读任务', 'var(--st-done)')
    } catch {
      showToast('操作失败，请确认事件服务已连接', 'var(--st-fail)')
      btn.disabled = false
    }
  }
  return btn
}

/* 一键清理：第一次点击进入确认态，3.2s 内再次点击才执行，避免误删 */
function makeClearButton(): HTMLButtonElement {
  const btn = h('button', 'btn danger clear-btn', [svgIcon('trash'), '一键清理'])
  let armed = false
  let timer = 0

  const reset = (): void => {
    armed = false
    window.clearTimeout(timer)
    btn.classList.remove('armed')
    btn.replaceChildren(svgIcon('trash'), '一键清理')
  }

  btn.onclick = async () => {
    if (!armed) {
      const total = state.queue.length + state.history.length
      armed = true
      btn.classList.add('armed')
      btn.replaceChildren(svgIcon('trash'), `确认清空全部 ${total} 个任务？`)
      timer = window.setTimeout(reset, 3200)
      return
    }
    reset()
    btn.disabled = true
    try {
      const deleted = await window.aihub.clearTasks()
      showToast(`已清空 ${deleted} 个任务`, 'var(--st-done)')
    } catch {
      showToast('清理失败，请确认事件服务已连接', 'var(--st-fail)')
      btn.disabled = false
    }
  }
  return btn
}

/* ---------- 列表 ---------- */

function renderQueue(container: HTMLElement): void {
  const tasks = filteredTasks()
  if (tasks.length === 0) {
    container.append(
      state.queue.length === 0
        ? emptyState('check', '全部处理完毕', '新的 AI 任务完成时会实时推送到这里')
        : emptyState('inboxArt', '没有匹配的任务', '调整搜索关键词或来源筛选试试'),
    )
    return
  }

  for (const section of QUEUE_SECTIONS) {
    const sectionTasks = tasks.filter((t) => t.status === section.status)
    if (sectionTasks.length === 0) continue

    const dot = h('span', 'dot')
    dot.style.background = section.color
    dot.style.boxShadow = `0 0 6px ${section.color}`
    container.append(
      h('div', 'section-header', [dot, section.title, h('span', 'count', [String(sectionTasks.length)])]),
    )

    const grid = h('div', 'card-grid')
    for (const task of sectionTasks) grid.append(queueCard(task))
    container.append(grid)
  }
}

function renderHistory(container: HTMLElement): void {
  const tasks = filteredTasks()
  if (tasks.length === 0) {
    container.append(
      state.history.length === 0
        ? emptyState('inboxArt', '暂无历史任务', '已查看和已忽略的任务会保留在这里')
        : emptyState('inboxArt', '没有匹配的任务', '调整搜索关键词或来源筛选试试'),
    )
    return
  }

  const grid = h('div', 'card-grid')
  for (const task of tasks) {
    const deleteBtn = h('button', 'btn danger', [svgIcon('trash'), '删除'])
    deleteBtn.onclick = async (e) => {
      e.stopPropagation()
      if (state.selectedTaskId === task.id) state.selectedTaskId = null
      await window.aihub.deleteTask(task.id)
    }
    grid.append(buildCard(task, [deleteBtn]))
  }
  container.append(grid)
}

function queueCard(task: HubTask): HTMLElement {
  const openBtn = h('button', 'btn primary', [svgIcon('external'), '打开'])
  openBtn.onclick = async (e) => {
    e.stopPropagation()
    openBtn.disabled = true
    try {
      await window.aihub.openTask(task.id)
    } catch {
      showToast('打开任务失败，请确认后端已连接', 'var(--st-fail)')
      openBtn.disabled = false
    }
  }

  const ignoreBtn = h('button', 'btn', [svgIcon('ignore'), '忽略'])
  ignoreBtn.onclick = async (e) => {
    e.stopPropagation()
    await window.aihub.ignoreTask(task.id)
  }

  return buildCard(task, [ignoreBtn, openBtn])
}

function buildCard(task: HubTask, actions: HTMLElement[]): HTMLElement {
  const srcPill = h('span', 'src-pill', [h('span', 'dot'), SOURCE_LABELS[task.source] ?? task.source])
  srcPill.dataset.source = task.source

  const top = h('div', 'card-top', [
    srcPill,
    h('span', 'card-time', [formatRelativeTime(task.completedAt ?? task.createdAt)]),
  ])

  const title = h('div', 'card-title', [task.title ?? '(无标题任务)'])
  const children: HTMLElement[] = [top, title]

  if (task.contentPreview) {
    children.push(h('div', 'card-preview', [task.contentPreview]))
  }

  const pathEl = h('span', 'card-path', [task.projectPath ?? ''])
  pathEl.title = task.projectPath ?? ''
  const footer = h('div', 'card-footer', [pathEl, h('div', 'card-actions', actions)])
  children.push(footer)

  const card = h('article', 'card', children)
  card.dataset.status = task.status
  if (state.selectedTaskId === task.id) card.classList.add('selected')
  card.onclick = () => {
    state.selectedTaskId = state.selectedTaskId === task.id ? null : task.id
    emit()
  }

  const chip = h('span', 'status-chip', [STATUS_LABELS[task.status] ?? task.status])
  top.append(chip)
  return card
}

function emptyState(icon: IconName, title: string, desc: string): HTMLElement {
  return h('div', 'empty', [
    h('div', 'art', [svgIcon(icon)]),
    h('div', 'title', [title]),
    h('div', 'desc', [desc]),
  ])
}

/* ---------- 详情面板 ---------- */

function makeDetailPane(task: HubTask): HTMLElement {
  const closeBtn = h('button', 'win-btn detail-close', [svgIcon('close')])
  closeBtn.title = '关闭详情'
  closeBtn.onclick = () => {
    state.selectedTaskId = null
    emit()
  }

  const openBtn = h('button', 'btn primary', [svgIcon('external'), '打开对话'])
  openBtn.onclick = async () => {
    openBtn.disabled = true
    try {
      await window.aihub.openTask(task.id)
    } catch {
      showToast('打开失败，请确认后端已连接', 'var(--st-fail)')
      openBtn.disabled = false
    }
  }

  const timelineBox = h('div', 'timeline', [h('div', 'timeline-loading', ['加载事件时间线…'])])

  const pane = h('aside', 'detail-pane', [
    h('div', 'detail-head', [
      h('span', 'src-pill', [h('span', 'dot'), SOURCE_LABELS[task.source] ?? task.source]),
      h('span', 'status-chip', [STATUS_LABELS[task.status] ?? task.status]),
      closeBtn,
    ]),
    h('div', 'detail-title', [task.title ?? '(无标题任务)']),
    h('div', 'detail-kv', [
      h('span', 'k', ['项目路径']),
      h('span', 'v path', [task.projectPath ?? '—']),
      h('span', 'k', ['创建时间']),
      h('span', 'v', [formatRelativeTime(task.createdAt)]),
      h('span', 'k', ['完成时间']),
      h('span', 'v', [task.completedAt ? formatRelativeTime(task.completedAt) : '—']),
      ...(task.openUrl
        ? [h('span', 'k', ['会话链接']), h('span', 'v path', [task.openUrl])]
        : []),
    ]),
    h('div', 'detail-actions', [openBtn]),
    h('div', 'detail-section-title', ['事件时间线']),
    timelineBox,
  ])

  const srcPill = pane.querySelector('.src-pill') as HTMLElement
  srcPill.dataset.source = task.source

  const taskId = task.id
  window.aihub
    .getTaskEvents(taskId)
    .then((events) => {
      if (state.selectedTaskId !== taskId) return
      timelineBox.textContent = ''
      if (events.length === 0) {
        timelineBox.append(h('div', 'timeline-loading', ['暂无事件记录']))
        return
      }
      for (const ev of events) {
        timelineBox.append(
          h('div', 'timeline-item', [
            h('span', 'timeline-dot'),
            h('div', 'timeline-body', [
              h('div', 'timeline-row', [
                h('span', 'timeline-label', [EVENT_LABELS[ev.eventType] ?? ev.eventType]),
                h('span', 'timeline-time', [formatRelativeTime(ev.occurredAt)]),
              ]),
              h('details', 'timeline-payload', [
                h('summary', '', ['原始载荷']),
                h('pre', '', [JSON.stringify(ev.payload, null, 2)]),
              ]),
            ]),
          ]),
        )
      }
    })
    .catch(() => {
      if (state.selectedTaskId !== taskId) return
      timelineBox.textContent = ''
      timelineBox.append(h('div', 'timeline-loading', ['事件加载失败（后端离线？）']))
    })

  return pane
}

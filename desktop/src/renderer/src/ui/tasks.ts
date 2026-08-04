/* 任务视图：待处理 / 历史 + 搜索筛选 + 右侧详情面板（事件时间线 + 原始载荷） */

import type { HubTask, TaskSource, TaskStatus } from '../../../shared/types'
import { EVENT_LABELS, SOURCE_LABELS, STATUS_LABELS, displayTitle } from '../../../shared/labels'
import { HISTORY_STATUSES, QUEUE_STATUSES, emit, filteredTasks, state } from '../state'
import { h, showToast, svgIcon, type IconName } from './dom'
import { formatRelativeTime } from '../time'

type ReloadTasks = () => Promise<boolean>
/** 每种状态（种类）独立翻页：loadMore(status) 追加该状态下一页 */
type LoadMore = (status: TaskStatus) => Promise<boolean>

const QUEUE_SECTIONS: Array<{ status: TaskStatus; title: string; color: string }> = [
  { status: 'RUNNING', title: '执行中', color: 'var(--st-run)' },
  { status: 'NEEDS_INPUT', title: '等待输入', color: 'var(--st-input)' },
  { status: 'COMPLETED_UNREAD', title: '已完成', color: 'var(--st-done)' },
  { status: 'FAILED_UNREAD', title: '失败', color: 'var(--st-fail)' },
]

const HISTORY_SECTIONS: Array<{ status: TaskStatus; title: string; color: string }> = [
  { status: 'VIEWED', title: '已查看', color: 'var(--st-done)' },
  { status: 'IGNORED', title: '已忽略', color: 'var(--text-muted)' },
]

/** 某状态服务端准确总数（summary），缺失时退回已加载数 */
function statusCount(status: TaskStatus): number {
  return state.statusCounts[status] ?? 0
}

/** 某视图下全部状态的准确总数 */
function viewTotal(view: 'queue' | 'history'): number {
  const statuses = view === 'history' ? HISTORY_STATUSES : QUEUE_STATUSES
  return statuses.reduce((sum, s) => sum + statusCount(s), 0)
}

const SOURCE_OPTIONS: Array<{ value: TaskSource | 'all'; label: string }> = [
  { value: 'all', label: '全部来源' },
  { value: 'CHATGPT', label: 'ChatGPT' },
  { value: 'CLAUDE_CODE', label: 'Claude Code' },
  { value: 'CODEX', label: 'Codex' },
  { value: 'OTHER', label: '其他' },
]

const VIEW_STATUS_OPTIONS: Record<'queue' | 'history', Array<{ value: TaskStatus | 'all'; label: string }>> = {
  queue: [
    { value: 'all', label: '全部' },
    { value: 'RUNNING', label: '执行中' },
    { value: 'NEEDS_INPUT', label: '等待输入' },
    { value: 'COMPLETED_UNREAD', label: '已完成' },
    { value: 'FAILED_UNREAD', label: '失败' },
  ],
  history: [
    { value: 'all', label: '全部' },
    { value: 'VIEWED', label: '已查看' },
    { value: 'IGNORED', label: '已忽略' },
  ],
}

export function renderTasksView(container: HTMLElement, reloadTasks: ReloadTasks, loadMore: LoadMore): void {
  const title = state.view === 'history' ? '历史' : '待处理'

  const clearBtn = makeClearButton(reloadTasks)
  clearBtn.disabled = state.queue.length + state.history.length === 0
  const readAllBtn = makeReadAllButton(reloadTasks)
  readAllBtn.disabled = unreadCount() === 0

  const summary = summaryText()
  const headerKids: Array<string | HTMLElement> = [h('h1', '', [title])]
  if (summary) headerKids.push(h('span', 'summary', [summary]))
  headerKids.push(h('div', 'header-actions', [readAllBtn, clearBtn]))

  container.append(
    h('div', 'view-chrome', [
      h('div', 'content-header', headerKids),
      makeStatusFilters(),
      makeFilterBar(),
    ]),
  )

  if (state.backend === 'offline') {
    const offline = h('div', 'offline-banner', ['本地事件服务未连接，等待自动恢复…恢复后任务会自动同步。'])
    offline.setAttribute('role', 'status')
    offline.setAttribute('aria-live', 'polite')
    container.append(offline)
  } else if (state.taskLoadState === 'error' && state.queue.length + state.history.length > 0) {
    const stale = h('div', 'offline-banner stale-banner', ['任务刷新失败，当前显示的是上次缓存结果。'])
    stale.setAttribute('role', 'status')
    stale.setAttribute('aria-live', 'polite')
    container.append(stale)
  }

  if (state.taskLoadState === 'loading') {
    container.append(makeLoadingState())
    return
  }
  if (state.taskLoadState === 'error' && state.queue.length + state.history.length === 0) {
    container.append(makeErrorState(reloadTasks))
    return
  }

  const visibleTasks = filteredTasks()
  const selected =
    state.selectedTaskId !== null
      ? visibleTasks.find((task) => task.id === state.selectedTaskId)
      : undefined
  const listArea = h('div', 'list-area')
  if (state.view === 'queue') renderQueue(listArea, reloadTasks, loadMore)
  else renderHistory(listArea, reloadTasks, loadMore)

  if (selected) {
    const body = h('div', 'tasks-split', [listArea, makeDetailPane(selected, reloadTasks)])
    container.append(body)
  } else {
    container.append(listArea)
  }
}

/* ---------- 过滤栏 ---------- */

function makeStatusFilters(): HTMLElement {
  const view = state.view === 'history' ? 'history' : 'queue'
  const buttons = VIEW_STATUS_OPTIONS[view].map((option) => {
    // 用服务端准确总数（summary），未翻完页前数字也不再跳动
    const count = option.value === 'all' ? viewTotal(view) : statusCount(option.value)
    const button = h('button', 'filter-chip', [
      h('span', 'filter-chip-dot'),
      option.label,
      h('span', 'filter-chip-count', [String(count)]),
    ])
    button.dataset.status = option.value
    button.classList.toggle('active', state.statusFilter === option.value)
    button.setAttribute('aria-pressed', String(state.statusFilter === option.value))
    button.onclick = () => {
      if (state.statusFilter === option.value) return
      state.statusFilter = option.value
      state.selectedTaskId = null
      emit()
    }
    return button
  })
  const filters = h('div', 'status-filters', buttons)
  filters.setAttribute('aria-label', '按任务状态筛选')
  return filters
}

function makeFilterBar(): HTMLElement {
  const search = h('input', 'filter-search') as HTMLInputElement
  search.type = 'search'
  search.placeholder = '搜索标题 / 路径 / 摘要…'
  search.setAttribute('aria-label', '搜索任务')
  search.setAttribute('aria-keyshortcuts', 'Control+K')
  search.value = state.search
  const applySearch = (): void => {
    state.search = search.value
    if (state.selectedTaskId !== null && !filteredTasks().some((task) => task.id === state.selectedTaskId)) {
      state.selectedTaskId = null
    }
    emit()
    focusSearch(state.search.length)
  }
  search.oninput = (event) => {
    state.search = search.value
    if ((event as InputEvent).isComposing) return
    applySearch()
  }
  search.addEventListener('compositionend', applySearch)

  const clearSearch = h('button', 'search-clear', [svgIcon('close')])
  clearSearch.title = '清空搜索'
  clearSearch.setAttribute('aria-label', '清空搜索')
  clearSearch.classList.toggle('hidden', !state.search)
  clearSearch.onclick = () => {
    state.search = ''
    state.selectedTaskId = null
    emit()
    focusSearch(0)
  }

  const searchBox = h('div', 'search-control', [
    svgIcon('search'),
    search,
    clearSearch,
    h('kbd', state.search ? 'search-shortcut hidden' : 'search-shortcut', ['Ctrl K']),
  ])

  const select = h('select', 'filter-source') as HTMLSelectElement
  select.setAttribute('aria-label', '按来源筛选')
  for (const opt of SOURCE_OPTIONS) {
    const el = h('option', '', [opt.label]) as HTMLOptionElement
    el.value = opt.value
    if (state.sourceFilter === opt.value) el.selected = true
    select.append(el)
  }
  select.onchange = () => {
    state.sourceFilter = select.value as TaskSource | 'all'
    state.selectedTaskId = null
    emit()
  }

  const sort = h('select', 'filter-source filter-sort') as HTMLSelectElement
  sort.setAttribute('aria-label', '任务排序')
  for (const [value, label] of [
    ['newest', '最新优先'],
    ['oldest', '最早优先'],
  ] as const) {
    const option = h('option', '', [label]) as HTMLOptionElement
    option.value = value
    option.selected = state.sort === value
    sort.append(option)
  }
  sort.onchange = () => {
    state.sort = sort.value as 'newest' | 'oldest'
    emit()
  }

  return h('div', 'filter-bar', [
    searchBox,
    filterField('来源', select),
    filterField('排序', sort),
  ])
}

function filterField(label: string, control: HTMLElement): HTMLElement {
  return h('label', 'filter-field', [h('span', '', [label]), control])
}

function focusSearch(cursor: number): void {
  const next = document.querySelector('.filter-search') as HTMLInputElement | null
  if (!next) return
  next.focus({ preventScroll: true })
  next.setSelectionRange(cursor, cursor)
}

/* ---------- 摘要与按钮 ---------- */

function summaryText(): string {
  if (state.view === 'history') {
    const total = viewTotal('history')
    return total ? `共 ${total} 条记录` : ''
  }
  const total = viewTotal('queue')
  if (total === 0) return ''
  const needsInput = statusCount('NEEDS_INPUT')
  return needsInput > 0
    ? `${total} 个任务 · ${needsInput} 个等待你的输入`
    : `${total} 个任务待查看`
}

function unreadCount(): number {
  return statusCount('COMPLETED_UNREAD') + statusCount('FAILED_UNREAD')
}

function actionToast(message: string, refreshed: boolean): void {
  showToast(
    refreshed ? message : `${message}，但列表刷新失败`,
    refreshed ? 'var(--st-done)' : 'var(--st-input)',
  )
}

/* 一键已读：非破坏性操作，单击即执行；不动等待输入的任务 */
function makeReadAllButton(reloadTasks: ReloadTasks): HTMLButtonElement {
  const btn = h('button', 'btn read-all-btn', [svgIcon('check'), '一键已读'])
  btn.title = '将所有已完成/失败的未读任务标记为已读'
  btn.onclick = async () => {
    btn.disabled = true
    try {
      const count = await window.aihub.readAllTasks()
      const refreshed = await reloadTasks()
      actionToast(count > 0 ? `已将 ${count} 个任务标记为已读` : '没有未读任务', refreshed)
    } catch {
      showToast('操作失败，请确认事件服务已连接', 'var(--st-fail)')
    } finally {
      // IPC 成功但 reloadTasks() 返回 false 时也要恢复按钮（M21）
      btn.disabled = false
    }
  }
  return btn
}

/* 一键清理：第一次点击进入确认态，3.2s 内再次点击才执行，避免误删 */
function makeClearButton(reloadTasks: ReloadTasks): HTMLButtonElement {
  const btn = h('button', 'btn danger clear-btn', [svgIcon('trash'), '一键清理'])
  let armed = false
  let timer = 0

  const reset = (): void => {
    armed = false
    window.clearTimeout(timer)
    timer = 0
    btn.classList.remove('armed')
    btn.replaceChildren(svgIcon('trash'), '一键清理')
  }

  btn.onclick = async () => {
    if (!armed) {
      window.clearTimeout(timer) // clear any stale timer from previous renders
      const total = viewTotal('queue') + viewTotal('history')
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
      state.selectedTaskId = null
      const refreshed = await reloadTasks()
      actionToast(`已清空 ${deleted} 个任务`, refreshed)
    } catch {
      showToast('清理失败，请确认事件服务已连接', 'var(--st-fail)')
    } finally {
      btn.disabled = false
    }
  }
  return btn
}

/* ---------- 列表 ---------- */

function renderQueue(container: HTMLElement, reloadTasks: ReloadTasks, loadMore: LoadMore): void {
  const tasks = filteredTasks()
  if (tasks.length === 0) {
    container.append(
      state.queue.length === 0
        ? emptyState('check', '全部处理完毕', '新的 AI 任务完成时会实时推送到这里')
        : emptyState('inboxArt', '没有匹配的任务', '调整搜索关键词或来源筛选试试'),
    )
    return
  }

  // 每个种类（状态）独立分页：各区块自己的「加载更多」
  for (const section of QUEUE_SECTIONS) {
    renderSection(container, section, tasks, reloadTasks, loadMore, queueCard)
  }
}

function renderHistory(container: HTMLElement, reloadTasks: ReloadTasks, loadMore: LoadMore): void {
  const tasks = filteredTasks()
  if (tasks.length === 0) {
    container.append(
      state.history.length === 0
        ? emptyState('inboxArt', '暂无历史任务', '已查看和已忽略的任务会保留在这里')
        : emptyState('inboxArt', '没有匹配的任务', '调整搜索关键词或来源筛选试试'),
    )
    return
  }

  const historyCard = (task: HubTask, reload: ReloadTasks): HTMLElement => {
    const deleteBtn = h('button', 'btn danger', [svgIcon('trash'), '删除'])
    deleteBtn.onclick = async (e) => {
      e.stopPropagation()
      deleteBtn.disabled = true
      try {
        await window.aihub.deleteTask(task.id)
        if (state.selectedTaskId === task.id) state.selectedTaskId = null
        const refreshed = await reload()
        actionToast('任务已删除', refreshed)
      } catch {
        showToast('删除失败，请确认事件服务已连接', 'var(--st-fail)')
      } finally {
        deleteBtn.disabled = false
      }
    }
    return buildCard(task, [deleteBtn])
  }

  // 历史也按状态分区：已查看 / 已忽略，各自独立翻页
  for (const section of HISTORY_SECTIONS) {
    renderSection(container, section, tasks, reloadTasks, loadMore, historyCard)
  }
}

/** 渲染一个状态区块：标题（服务端准确计数）+ 卡片网格 + 该状态独立的「加载更多」 */
function renderSection(
  container: HTMLElement,
  section: { status: TaskStatus; title: string; color: string },
  tasks: HubTask[],
  reloadTasks: ReloadTasks,
  loadMore: LoadMore,
  card: (task: HubTask, reloadTasks: ReloadTasks) => HTMLElement,
): void {
  const sectionTasks = tasks.filter((t) => t.status === section.status)
  if (sectionTasks.length === 0) return

  const dot = h('span', 'dot')
  dot.style.background = section.color
  dot.style.boxShadow = `0 0 6px ${section.color}`
  container.append(
    h('div', 'section-header', [dot, section.title, h('span', 'count', [String(statusCount(section.status))])]),
  )

  const grid = h('div', 'card-grid')
  for (const task of sectionTasks) grid.append(card(task, reloadTasks))
  container.append(grid)

  if (state.bucketHasMore[section.status]) container.append(makeLoadMoreButton(section.status, loadMore))
}

function makeLoadMoreButton(status: TaskStatus, loadMore: LoadMore): HTMLElement {
  const btn = h('button', 'btn load-more-btn', ['加载更多'])
  btn.title = '加载更多该分类任务'
  btn.onclick = async () => {
    btn.disabled = true
    btn.textContent = '加载中…'
    try {
      const ok = await loadMore(status)
      if (!ok) showToast('加载失败，请确认事件服务已连接', 'var(--st-fail)')
    } finally {
      if (!btn.isConnected) return // 翻页后整页重渲染，旧按钮已脱离 DOM
      btn.disabled = false
      btn.textContent = '加载更多'
    }
  }
  return btn
}

function queueCard(task: HubTask, reloadTasks: ReloadTasks): HTMLElement {
  const openBtn = h('button', 'btn primary', [svgIcon('external'), '打开'])
  openBtn.onclick = async (e) => {
    e.stopPropagation()
    openBtn.disabled = true
    try {
      await window.aihub.openTask(task.id)
      const refreshed = await reloadTasks()
      if (!refreshed) actionToast('对话已打开', false)
    } catch {
      showToast('打开任务失败，请确认后端已连接', 'var(--st-fail)')
    } finally {
      openBtn.disabled = false
    }
  }

  const ignoreBtn = h('button', 'btn', [svgIcon('ignore'), '忽略'])
  ignoreBtn.onclick = async (e) => {
    e.stopPropagation()
    ignoreBtn.disabled = true
    try {
      await window.aihub.ignoreTask(task.id)
      const refreshed = await reloadTasks()
      actionToast('任务已移入历史', refreshed)
    } catch {
      showToast('忽略失败，请确认事件服务已连接', 'var(--st-fail)')
    } finally {
      ignoreBtn.disabled = false
    }
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

  const title = h('div', 'card-title', [displayTitle(task)])
  const detailChildren: HTMLElement[] = [top, title]

  if (task.contentPreview) {
    detailChildren.push(h('div', 'card-preview', [task.contentPreview]))
  }

  const detailTrigger = h('button', 'card-detail-trigger', detailChildren)
  detailTrigger.setAttribute('aria-label', `查看任务详情：${displayTitle(task)}`)
  detailTrigger.onclick = () => {
    state.selectedTaskId = state.selectedTaskId === task.id ? null : task.id
    emit()
  }

  const pathEl = h('span', 'card-path', [task.projectPath ?? ''])
  pathEl.title = task.projectPath ?? ''
  const footer = h('div', 'card-footer', [pathEl, h('div', 'card-actions', actions)])

  const card = h('article', 'card', [detailTrigger, footer])
  card.dataset.status = task.status
  if (state.selectedTaskId === task.id) card.classList.add('selected')

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

function makeLoadingState(): HTMLElement {
  const loading = h('div', 'skeleton-grid', [
    ...Array.from({ length: 4 }, () =>
      h('div', 'skeleton-card', [
        h('span', 'skeleton-line short'),
        h('span', 'skeleton-line title'),
        h('span', 'skeleton-line'),
        h('span', 'skeleton-line medium'),
      ]),
    ),
  ])
  loading.setAttribute('aria-label', '正在加载任务')
  loading.setAttribute('aria-busy', 'true')
  loading.setAttribute('role', 'status')
  loading.setAttribute('aria-live', 'polite')
  return loading
}

function makeErrorState(reloadTasks: ReloadTasks): HTMLElement {
  const retry = h('button', 'btn primary', [svgIcon('refresh'), '重新连接'])
  retry.onclick = () => {
    retry.disabled = true
    void reloadTasks()
  }
  const error = h('div', 'empty error-state', [
    h('div', 'art', [svgIcon('offline')]),
    h('div', 'title', ['暂时无法读取任务']),
    h('div', 'desc', ['检查 MySQL 与本地事件服务后重试，已有数据不会被删除']),
    retry,
  ])
  error.setAttribute('role', 'alert')
  return error
}

/* ---------- 详情面板 ---------- */

function makeDetailPane(task: HubTask, reloadTasks: ReloadTasks): HTMLElement {
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
      const refreshed = await reloadTasks()
      if (!refreshed) actionToast('对话已打开', false)
    } catch {
      showToast('打开失败，请确认后端已连接', 'var(--st-fail)')
    } finally {
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
    h('div', 'detail-title', [displayTitle(task)]),
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

  // Version counter to cancel stale async responses when pane is unmounted
  const taskId = task.id

  window.aihub
    .getTaskEvents(taskId)
    .then((events) => {
      // Discard if task changed or pane was unmounted (DOM node no longer in document)
      if (state.selectedTaskId !== taskId) return
      if (!pane.isConnected) return
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
      // Discard if task changed or pane was unmounted
      if (state.selectedTaskId !== taskId) return
      if (!pane.isConnected) return
      timelineBox.textContent = ''
      timelineBox.append(h('div', 'timeline-loading', ['事件加载失败（后端离线？）']))
    })

  return pane
}

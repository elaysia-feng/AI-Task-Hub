import './styles.css'
import type { BackendStatus, HubTask, TaskStatus, TaskView } from '../../shared/types'
import { formatRelativeTime } from './time'

/* ---------- 图标（内联 SVG） ---------- */

const ICONS = {
  logo: '<svg viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="2.4" stroke-linecap="round"><circle cx="12" cy="5" r="2.2" fill="white" stroke="none"/><circle cx="5" cy="18" r="2.2" fill="white" stroke="none"/><circle cx="19" cy="18" r="2.2" fill="white" stroke="none"/><path d="M12 7.5 6 16M12 7.5 18 16M7 18h10"/></svg>',
  minus: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><path d="M2 6h8"/></svg>',
  close: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><path d="M2.5 2.5 9.5 9.5M9.5 2.5 2.5 9.5"/></svg>',
  check: '<svg viewBox="0 0 24 24" fill="none" stroke="#a78bfa" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12.5 9.5 18 20 6.5"/></svg>',
  inbox: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 13h4l2 3h6l2-3h4"/><path d="M5 6h14l2 7v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5z"/></svg>',
  inboxArt: '<svg viewBox="0 0 24 24" fill="none" stroke="#a78bfa" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 13h4l2 3h6l2-3h4"/><path d="M5 6h14l2 7v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5z"/></svg>',
  history: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 3v5h5"/><path d="M3.05 13a9 9 0 1 0 .5-4.5L3 8"/><path d="M12 7v5l3.5 2"/></svg>',
  external: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 2.5H2.5v7h7V7.5M7 2h3v3M10 2 5.5 6.5"/></svg>',
  trash: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M1.5 3h9M4.5 3V1.8h3V3M2.8 3l.6 7.2h5.2L9.2 3"/></svg>',
  ignore: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><circle cx="6" cy="6" r="4.6"/><path d="M3 3l6 6"/></svg>',
  sun: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>',
  moon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>',
}

/* ---------- 主题 ---------- */

type Theme = 'dark' | 'light'
const THEME_STORAGE_KEY = 'aihub-theme'
let themeToggleBtn: HTMLButtonElement

function currentTheme(): Theme {
  return (localStorage.getItem(THEME_STORAGE_KEY) as Theme) || 'dark'
}

function applyTheme(theme: Theme): void {
  document.documentElement.dataset.theme = theme
  if (themeToggleBtn) {
    themeToggleBtn.replaceChildren(svgIcon(theme === 'dark' ? 'sun' : 'moon'))
    themeToggleBtn.title = theme === 'dark' ? '切换到浅色模式' : '切换到深色模式'
  }
}

function toggleTheme(): void {
  const next: Theme = currentTheme() === 'dark' ? 'light' : 'dark'
  localStorage.setItem(THEME_STORAGE_KEY, next)
  applyTheme(next)
}

/* ---------- 常量 ---------- */

const SOURCE_LABELS: Record<string, string> = {
  CHATGPT: 'ChatGPT',
  CLAUDE_CODE: 'Claude Code',
  CODEX: 'Codex',
  OTHER: '其他',
}

const STATUS_LABELS: Record<string, string> = {
  RUNNING: '运行中',
  NEEDS_INPUT: '等待输入',
  COMPLETED_UNREAD: '已完成',
  FAILED_UNREAD: '失败',
  VIEWED: '已查看',
  IGNORED: '已忽略',
}

const QUEUE_SECTIONS: Array<{ status: TaskStatus; title: string; color: string }> = [
  { status: 'NEEDS_INPUT', title: '等待输入', color: 'var(--st-input)' },
  { status: 'COMPLETED_UNREAD', title: '已完成', color: 'var(--st-done)' },
  { status: 'FAILED_UNREAD', title: '失败', color: 'var(--st-fail)' },
]

const TOAST_ACCENTS: Record<string, string> = {
  TASK_COMPLETED: 'var(--st-done)',
  TASK_FAILED: 'var(--st-fail)',
  TASK_NEEDS_INPUT: 'var(--st-input)',
}

const BACKEND_LABELS: Record<BackendStatus, string> = {
  online: '已连接',
  connecting: '连接中',
  offline: '未连接',
}

const VIEW_META: Record<TaskView, { title: string; icon: keyof typeof ICONS }> = {
  queue: { title: '待处理', icon: 'inbox' },
  history: { title: '历史', icon: 'history' },
}

/* ---------- 应用状态 ---------- */

interface AppState {
  view: TaskView
  queue: HubTask[]
  history: HubTask[]
  backend: BackendStatus
}

const state: AppState = {
  view: 'queue',
  queue: [],
  history: [],
  backend: 'connecting',
}

/* ---------- DOM 辅助 ---------- */

function h<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string,
  children?: Array<Node | string>,
): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag)
  if (className) el.className = className
  if (children) el.append(...children)
  return el
}

function svgIcon(name: keyof typeof ICONS): HTMLElement {
  const span = document.createElement('span')
  span.innerHTML = ICONS[name]
  return span.firstElementChild as HTMLElement
}

/* ---------- 渲染：外壳 ---------- */

const root = document.getElementById('app')!
let backendPill: HTMLElement
let queueBadge: HTMLElement
let contentEl: HTMLElement

function renderShell(): void {
  const minimizeBtn = h('button', 'win-btn', [svgIcon('minus')])
  minimizeBtn.title = '最小化'
  minimizeBtn.onclick = () => window.aihub.minimizeWindow()

  const closeBtn = h('button', 'win-btn close', [svgIcon('close')])
  closeBtn.title = '隐藏到托盘'
  closeBtn.onclick = () => window.aihub.closeWindow()

  backendPill = h('div', 'backend-pill connecting', [h('span', 'dot'), h('span', 'label')])

  themeToggleBtn = h('button', 'win-btn theme-toggle')
  themeToggleBtn.onclick = toggleTheme

  const titlebar = h('header', 'titlebar', [
    h('div', 'logo', [svgIcon('logo')]),
    h('span', 'app-name', ['AI Task Hub']),
    h('span', 'app-sub', ['多 AI 平台任务中心']),
    h('div', 'drag-fill'),
    backendPill,
    themeToggleBtn,
    minimizeBtn,
    closeBtn,
  ])

  const queueNav = navItem('queue', [queueBadge = h('span', 'badge zero', ['0'])])
  const historyNav = navItem('history')
  const sidebar = h('aside', 'sidebar', [
    h('div', 'nav-label', ['任务']),
    queueNav,
    historyNav,
    h('div', 'foot', ['Claude Code · Codex · ChatGPT', document.createElement('br'), '事件驱动 · 本地运行']),
  ])

  contentEl = h('main', 'content')
  root.append(titlebar, h('div', 'body-row', [sidebar, contentEl]))
}

function navItem(view: TaskView, extra?: HTMLElement[]): HTMLElement {
  const meta = VIEW_META[view]
  const item = h('button', 'nav-item', [svgIcon(meta.icon), meta.title, ...(extra ?? [])])
  item.dataset.view = view
  if (state.view === view) item.classList.add('active')
  item.onclick = () => switchView(view)
  return item
}

function switchView(view: TaskView): void {
  if (state.view === view) return
  state.view = view
  document.querySelectorAll('.nav-item').forEach((item) => {
    item.classList.toggle('active', (item as HTMLElement).dataset.view === view)
  })
  renderContent()
}

function updateBackendPill(): void {
  backendPill.className = `backend-pill ${state.backend}`
  backendPill.querySelector('.label')!.textContent = BACKEND_LABELS[state.backend]
}

/* ---------- 渲染：内容 ---------- */

function renderContent(): void {
  contentEl.textContent = ''

  const tasks = state.view === 'queue' ? state.queue : state.history
  const clearBtn = makeClearButton()
  clearBtn.disabled = state.queue.length + state.history.length === 0
  const readAllBtn = makeReadAllButton()
  readAllBtn.disabled = unreadCount() === 0
  const header = h('div', 'content-header', [
    h('h1', '', [VIEW_META[state.view].title]),
    h('span', 'summary', [summaryText()]),
    h('div', 'header-actions', [readAllBtn, clearBtn]),
  ])
  contentEl.append(header)

  if (state.backend === 'offline') {
    contentEl.append(
      h('div', 'offline-banner', ['本地事件服务未连接，等待自动恢复…恢复后任务会自动同步。']),
    )
  }

  if (state.view === 'queue') renderQueue()
  else renderHistory(tasks)

  const count = state.queue.length
  queueBadge.textContent = String(count)
  queueBadge.classList.toggle('zero', count === 0)
}

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

function renderQueue(): void {
  if (state.queue.length === 0) {
    contentEl.append(emptyState('check', '全部处理完毕', '新的 AI 任务完成时会实时推送到这里'))
    return
  }

  for (const section of QUEUE_SECTIONS) {
    const tasks = state.queue.filter((t) => t.status === section.status)
    if (tasks.length === 0) continue

    const dot = h('span', 'dot')
    dot.style.background = section.color
    dot.style.boxShadow = `0 0 6px ${section.color}`
    contentEl.append(
      h('div', 'section-header', [dot, section.title, h('span', 'count', [String(tasks.length)])]),
    )

    const grid = h('div', 'card-grid')
    for (const task of tasks) grid.append(queueCard(task))
    contentEl.append(grid)
  }
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

  const card = buildCard(task, [ignoreBtn, openBtn])
  card.onclick = () => openBtn.click()
  return card
}

function renderHistory(tasks: HubTask[]): void {
  if (tasks.length === 0) {
    contentEl.append(emptyState('inboxArt', '暂无历史任务', '已查看和已忽略的任务会保留在这里'))
    return
  }

  const grid = h('div', 'card-grid')
  for (const task of tasks) {
    const deleteBtn = h('button', 'btn danger', [svgIcon('trash'), '删除'])
    deleteBtn.onclick = async (e) => {
      e.stopPropagation()
      await window.aihub.deleteTask(task.id)
    }
    grid.append(buildCard(task, [deleteBtn]))
  }
  contentEl.append(grid)
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

  const chip = h('span', 'status-chip', [STATUS_LABELS[task.status] ?? task.status])
  top.append(chip)
  return card
}

function emptyState(icon: keyof typeof ICONS, title: string, desc: string): HTMLElement {
  return h('div', 'empty', [
    h('div', 'art', [svgIcon(icon)]),
    h('div', 'title', [title]),
    h('div', 'desc', [desc]),
  ])
}

/* ---------- Toast ---------- */

let toastRoot: HTMLElement

function showToast(text: string, accent?: string): void {
  const toast = h('div', 'toast', [h('span', 'dot'), h('span', '', [text])])
  if (accent) toast.style.setProperty('--toast-accent', accent)
  toastRoot.append(toast)
  setTimeout(() => {
    toast.classList.add('leaving')
    toast.addEventListener('animationend', () => toast.remove(), { once: true })
  }, 3600)
}

/* ---------- 数据 ---------- */

async function reload(): Promise<void> {
  try {
    const [queue, history] = await Promise.all([window.aihub.getQueue(), window.aihub.getHistory()])
    state.queue = queue
    state.history = history
  } catch {
    // 后端暂不可达时保留现有数据，由 backend:status 事件提示
  }
  renderContent()
}

/* ---------- 启动 ---------- */

async function bootstrap(): Promise<void> {
  applyTheme(currentTheme()) // 先落主题再渲染，避免闪烁
  renderShell()
  applyTheme(currentTheme()) // 补齐按钮图标
  toastRoot = h('div', 'toast-root')
  document.body.append(toastRoot)
  updateBackendPill()
  renderContent()

  state.backend = await window.aihub.getBackendStatus()
  updateBackendPill()
  await reload()

  window.aihub.onBackendStatus((status) => {
    const recovered = status === 'online' && state.backend !== 'online'
    state.backend = status
    updateBackendPill()
    renderContent()
    if (recovered) void reload()
  })

  window.aihub.onTaskChanged((msg) => {
    if (msg.type === 'task_changed') {
      const label = STATUS_LABELS[msg.task.status] ?? ''
      const accent = TOAST_ACCENTS[msg.eventType]
      if (accent) {
        showToast(`${SOURCE_LABELS[msg.task.source]} · ${label}：${msg.task.title ?? ''}`, accent)
      }
    }
    void reload()
  })
}

void bootstrap()

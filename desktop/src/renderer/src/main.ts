import './styles.css'
import type { BackendStatus } from '../../shared/types'
import { SOURCE_LABELS, STATUS_LABELS, displayTitle } from '../../shared/labels'
import { emit, state, subscribe } from './state'
import { h, showToast, svgIcon, type IconName } from './ui/dom'
import { renderTasksView } from './ui/tasks'
import { renderSettingsView } from './ui/settings'
import { applyWallpaper } from './ui/wallpaper'
import { applyUiMode } from './orb'

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

const NAV_ITEMS: Array<{ view: 'queue' | 'history' | 'settings'; title: string; icon: IconName }> = [
  { view: 'queue', title: '待处理', icon: 'inbox' },
  { view: 'history', title: '历史', icon: 'history' },
  { view: 'settings', title: '设置', icon: 'gear' },
]

/* ---------- 渲染：外壳 ---------- */

const root = document.getElementById('app')!
let backendPill: HTMLElement
let queueBadge: HTMLElement
let contentEl: HTMLElement

function renderShell(): void {
  const minimizeBtn = h('button', 'win-btn', [svgIcon('minus')])
  minimizeBtn.title = '最小化'
  minimizeBtn.onclick = () => window.aihub.minimizeWindow()

  const orbBtn = h('button', 'win-btn orb-toggle', [svgIcon('orb')])
  orbBtn.title = '收起为悬浮球'
  orbBtn.onclick = () => window.aihub.collapseToOrb()

  const closeBtn = h('button', 'win-btn close', [svgIcon('close')])
  closeBtn.title = '收起为悬浮球'
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
    orbBtn,
    minimizeBtn,
    closeBtn,
  ])

  const sidebar = h('aside', 'sidebar', [
    h('div', 'nav-label', ['任务']),
    ...NAV_ITEMS.map((item) => navItem(item)),
    h('div', 'foot', ['Claude Code · Codex · ChatGPT', document.createElement('br'), '事件驱动 · 本地运行']),
  ])

  contentEl = h('main', 'content')
  root.append(titlebar, h('div', 'body-row', [sidebar, contentEl]))
}

function navItem(item: { view: 'queue' | 'history' | 'settings'; title: string; icon: IconName }): HTMLElement {
  const extra = item.view === 'queue' ? [(queueBadge = h('span', 'badge zero', ['0']))] : []
  const nav = h('button', 'nav-item', [svgIcon(item.icon), item.title, ...extra])
  nav.dataset.view = item.view
  if (state.view === item.view) nav.classList.add('active')
  nav.onclick = () => {
    if (state.view === item.view) return
    state.view = item.view
    state.selectedTaskId = null
    document.querySelectorAll('.nav-item').forEach((el) => {
      el.classList.toggle('active', (el as HTMLElement).dataset.view === item.view)
    })
    emit()
  }
  return nav
}

function updateBackendPill(): void {
  backendPill.className = `backend-pill ${state.backend}`
  backendPill.querySelector('.label')!.textContent = BACKEND_LABELS[state.backend]
}

/* ---------- 渲染：内容分发 ---------- */

function renderContent(): void {
  contentEl.textContent = ''
  if (state.view === 'settings') renderSettingsView(contentEl)
  else renderTasksView(contentEl)

  const count = state.queue.length
  if (queueBadge) {
    queueBadge.textContent = String(count)
    queueBadge.classList.toggle('zero', count === 0)
  }
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
  emit()
}

/* ---------- 启动 ---------- */

const WIZARD_SEEN_KEY = 'aihub-wizard-seen'

async function bootstrap(): Promise<void> {
  document.documentElement.dataset.mode = 'panel'
  applyTheme(currentTheme()) // 先落主题再渲染，避免闪烁
  // 首次运行落到设置页（接入向导），之后记住
  if (!localStorage.getItem(WIZARD_SEEN_KEY)) {
    state.view = 'settings'
    localStorage.setItem(WIZARD_SEEN_KEY, '1')
  }
  renderShell()
  applyTheme(currentTheme()) // 补齐按钮图标
  try {
    applyWallpaper(await window.aihub.getWallpaper())
  } catch {
    // 主进程未就绪时忽略，设置页可重试
  }
  updateBackendPill()
  subscribe(renderContent)
  renderContent()

  state.backend = await window.aihub.getBackendStatus()
  updateBackendPill()
  await reload()

  window.aihub.onUiMode((mode) => {
    applyUiMode(mode)
  })

  window.aihub.onBackendStatus((status) => {
    const recovered = status === 'online' && state.backend !== 'online'
    state.backend = status
    updateBackendPill()
    emit()
    if (recovered) void reload()
  })

  window.aihub.onTaskChanged((msg) => {
    if (msg.type === 'task_changed') {
      const label = STATUS_LABELS[msg.task.status] ?? ''
      const accent = TOAST_ACCENTS[msg.eventType]
      if (accent) {
        showToast(`${SOURCE_LABELS[msg.task.source]} · ${label}：${displayTitle(msg.task)}`, accent)
      }
    }
    void reload()
  })

  window.aihub.onUpdateStatus((s) => {
    state.updateState = s
    if (s.state === 'available') showToast(`发现新版本 v${s.version}，后台下载中…`, 'var(--brand)')
    if (s.state === 'downloaded') showToast(`v${s.version} 已就绪，托盘菜单或设置页可重启安装`, 'var(--st-done)')
    if (s.state === 'error') showToast('更新检查失败，稍后自动重试', 'var(--st-fail)')
    if (state.view === 'settings') emit()
  })

  window.aihub.onPackagingStatus((s) => {
    if (s.state === 'running') showToast(s.message, 'var(--brand)')
    if (s.state === 'done') showToast(s.message, 'var(--st-done)')
    if (s.state === 'error') showToast(s.message, 'var(--st-fail)')
  })
}

void bootstrap()

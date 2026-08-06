import './styles.css'
import type { BackendStatus, HubTask, TaskStatus } from '../../shared/types'
import { SOURCE_LABELS, STATUS_LABELS, displayTitle } from '../../shared/labels'
import {
  ALL_STATUSES,
  HISTORY_STATUSES,
  QUEUE_STATUSES,
  emit,
  loadedTaskCount,
  state,
  subscribe,
} from './state'
import { h, showToast, svgIcon, type IconName } from './ui/dom'
import { renderTasksView } from './ui/tasks'
import { renderSettingsView } from './ui/settings'
import { applyWallpaper, refreshWallpaperTheme } from './ui/wallpaper'
import { applyIcon } from './ui/icon'
import { applyUiMode } from './orb'

/* ---------- 主题 ---------- */

type Theme = 'dark' | 'light'
const THEME_STORAGE_KEY = 'aihub-theme'
let themeToggleBtn: HTMLButtonElement

function currentTheme(): Theme {
  const stored = localStorage.getItem(THEME_STORAGE_KEY)
  return stored === 'light' || stored === 'dark' ? stored : 'dark'
}

function applyTheme(theme: Theme): void {
  document.documentElement.dataset.theme = theme
  refreshWallpaperTheme()
  if (themeToggleBtn) {
    themeToggleBtn.replaceChildren(svgIcon(theme === 'dark' ? 'sun' : 'moon'))
    themeToggleBtn.title = theme === 'dark' ? '切换到浅色模式' : '切换到深色模式'
  }
}

function toggleTheme(): void {
  const next: Theme = currentTheme() === 'dark' ? 'light' : 'dark'
  try {
    localStorage.setItem(THEME_STORAGE_KEY, next)
  } catch {
    // quota exceeded → keep current theme
    return
  }
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
  minimizeBtn.setAttribute('aria-label', '最小化')
  minimizeBtn.onclick = () => window.aihub.minimizeWindow()

  const orbBtn = h('button', 'win-btn orb-toggle', [svgIcon('orb')])
  orbBtn.title = '收起为悬浮球'
  orbBtn.setAttribute('aria-label', '收起为悬浮球')
  orbBtn.onclick = () => window.aihub.collapseToOrb()

  const closeBtn = h('button', 'win-btn close', [svgIcon('close')])
  // 与 orb-toggle 的 tooltip 区分，title 与 aria-label 统一为「关闭到悬浮球」
  closeBtn.title = '关闭到悬浮球'
  closeBtn.setAttribute('aria-label', '关闭到悬浮球')
  closeBtn.onclick = () => window.aihub.closeWindow()

  backendPill = h('div', 'backend-pill connecting', [h('span', 'dot'), h('span', 'label')])

  themeToggleBtn = h('button', 'win-btn theme-toggle')
  themeToggleBtn.setAttribute('aria-label', '切换明暗主题')
  themeToggleBtn.onclick = toggleTheme

  const logoBtn = h('button', 'logo', [svgIcon('logo')])
  logoBtn.type = 'button'
  logoBtn.title = '选择应用图标'
  logoBtn.setAttribute('aria-label', '选择应用图标')
  logoBtn.onclick = async () => {
    logoBtn.disabled = true
    try {
      applyIcon(await window.aihub.pickUserIcon())
    } catch (err) {
      showToast(err instanceof Error ? err.message : '选择图标失败', 'var(--st-fail)')
    } finally {
      logoBtn.disabled = false
    }
  }

  const titlebar = h('header', 'titlebar', [
    logoBtn,
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
    state.statusFilter = 'all'
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
  else renderTasksView(contentEl, reload, loadMore)

  const count = state.queue.length
  if (queueBadge) {
    queueBadge.textContent = String(count)
    queueBadge.classList.toggle('zero', count === 0)
  }
}

/* ---------- 数据 ---------- */

const PAGE_SIZE = 100
let reloadRequestId = 0
let reloadTimer: ReturnType<typeof setTimeout> | undefined

/** 把 reload 合入防抖窗口：批量操作（如 read-all 标记 50 条）会连发多个 task_changed，
 *  每次都触发 7 路 API 调用属浪费，收口为一次（review MEDIUM） */
function scheduleReload(): void {
  if (reloadTimer !== undefined) clearTimeout(reloadTimer)
  reloadTimer = setTimeout(() => {
    reloadTimer = undefined
    void reload().catch((err) => console.error('[main] reload failed:', err))
  }, 250)
}

async function reload(): Promise<boolean> {
  const requestId = ++reloadRequestId
  if (state.queue.length + state.history.length === 0) {
    state.taskLoadState = 'loading'
    emit()
  }
  try {
    // 按种类独立分页：summary + 6 种状态各拉第一页，互不影响 offset/hasMore。
    // 每页上限 PAGE_SIZE，避免无限增长时一次载入全表（内存尖峰已修）。
    const [summary, ...pages] = await Promise.all([
      window.aihub.getTasksSummary(),
      ...ALL_STATUSES.map((status) => window.aihub.getTaskPage(status, PAGE_SIZE, 0)),
    ])
    if (requestId !== reloadRequestId) return true
    state.statusCounts = summary
    const byStatus = new Map<TaskStatus, HubTask[]>()
    pages.forEach((page, i) => byStatus.set(ALL_STATUSES[i], page.tasks))
    for (const status of ALL_STATUSES) {
      state.bucketHasMore[status] = pages[ALL_STATUSES.indexOf(status)].hasMore
    }
    state.queue = QUEUE_STATUSES.flatMap((status) => byStatus.get(status) ?? [])
    state.history = HISTORY_STATUSES.flatMap((status) => byStatus.get(status) ?? [])
    state.taskLoadState = 'ready'
    const current = state.view === 'history' ? state.history : state.queue
    if (state.selectedTaskId !== null && !current.some((task) => task.id === state.selectedTaskId)) {
      state.selectedTaskId = null
    }
  } catch {
    if (requestId !== reloadRequestId) return true
    state.taskLoadState = 'error'
    emit()
    return false
  }
  emit()
  return true
}

/** 某种类（状态）「加载更多」：追加该状态下一页（offset=该状态已加载数）。
 *  期间若发生新 reload（requestId 变化）则丢弃本次结果防竞态；
 *  追加前按 id 去重，兜住并发下可能的重复返回。 */
async function loadMore(status: TaskStatus): Promise<boolean> {
  if (!state.bucketHasMore[status] || state.taskLoadState !== 'ready') return true
  const requestId = reloadRequestId
  try {
    const page = await window.aihub.getTaskPage(status, PAGE_SIZE, loadedTaskCount(status))
    if (requestId !== reloadRequestId) return true
    const target = QUEUE_STATUSES.includes(status) ? 'queue' : 'history'
    const existing = state[target]
    const known = new Set(existing.map((task) => task.id))
    const fresh = page.tasks.filter((task) => !known.has(task.id))
    state[target] = [...existing, ...fresh]
    state.bucketHasMore = { ...state.bucketHasMore, [status]: page.hasMore }
  } catch {
    return false
  }
  emit()
  return true
}

// SPA 单例，监听器永久有效；如未来支持多实例需加 cleanup
function bindKeyboardShortcuts(): void {
  document.addEventListener('keydown', (event) => {
    const target = event.target as HTMLElement | null
    const editing =
      target instanceof HTMLInputElement ||
      target instanceof HTMLTextAreaElement ||
      target instanceof HTMLSelectElement

    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
      if (state.view === 'settings') return
      event.preventDefault()
      ;(document.querySelector('.filter-search') as HTMLInputElement | null)?.focus()
      return
    }
    if (event.key === '/' && !editing && state.view !== 'settings') {
      event.preventDefault()
      ;(document.querySelector('.filter-search') as HTMLInputElement | null)?.focus()
      return
    }
    if (event.key !== 'Escape') return
    if (state.selectedTaskId !== null) {
      state.selectedTaskId = null
      emit()
    } else if (state.search) {
      state.search = ''
      emit()
    }
  })
}

/* ---------- 启动 ---------- */

const WIZARD_SEEN_KEY = 'aihub-wizard-seen'

async function bootstrap(): Promise<void> {
  document.documentElement.dataset.mode = 'panel'
  applyTheme(currentTheme()) // 先落主题再渲染，避免闪烁
  // 首次运行落到设置页（接入向导），之后记住
  if (!localStorage.getItem(WIZARD_SEEN_KEY)) {
    state.view = 'settings'
    try {
      localStorage.setItem(WIZARD_SEEN_KEY, '1')
    } catch {
      // localStorage full/quota exceeded → continue with in-memory flag
    }
  }
  renderShell()
  applyTheme(currentTheme()) // 补齐按钮图标
  try {
    applyWallpaper(await window.aihub.getWallpaper())
  } catch {
    // 主进程未就绪时忽略，设置页可重试
  }
  try {
    applyIcon(await window.aihub.getUserIcon())
  } catch {
    // 主进程未就绪时忽略，onIconChanged 兜底
  }
  window.aihub.onIconChanged((state) => applyIcon(state))
  updateBackendPill()
  subscribe(renderContent)
  bindKeyboardShortcuts()
  renderContent()

  state.backend = await window.aihub.getBackendStatus()
  updateBackendPill()
  if (state.backend === 'online') await reload()
  else {
    state.taskLoadState = 'error'
    emit()
  }

  window.aihub.onUiMode((mode) => {
    applyUiMode(mode)
  })

  window.aihub.onBackendStatus((status) => {
    const recovered = status === 'online' && state.backend !== 'online'
    state.backend = status
    if (status === 'offline' && state.taskLoadState === 'loading') state.taskLoadState = 'error'
    updateBackendPill()
    emit()
    if (recovered) void reload().catch((err) => console.error('[main] reload failed:', err))
  })

  window.aihub.onTaskChanged((msg) => {
    if (msg.type === 'task_changed') {
      const label = STATUS_LABELS[msg.task.status] ?? ''
      const accent = TOAST_ACCENTS[msg.eventType]
      if (accent) {
        showToast(`${SOURCE_LABELS[msg.task.source]} · ${label}：${displayTitle(msg.task)}`, accent, `task:${msg.task.id}:${msg.eventType}`)
      }
    }
    scheduleReload()
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

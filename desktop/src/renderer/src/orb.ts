import './orb.css'
import type { HubTask, TaskStatus, WallpaperState } from '../../shared/types'
import { SOURCE_LABELS, STATUS_LABELS, displayTitle } from '../../shared/labels'
import { state, subscribe } from './state'
import { BRAND_MARK_SVG } from './ui/dom'
import { applyWallpaper } from './ui/wallpaper'

const STATUS_COLOR: Record<string, string> = {
  RUNNING: '#38bdf8',
  NEEDS_INPUT: '#f59e0b',
  COMPLETED_UNREAD: '#22c55e',
  FAILED_UNREAD: '#ef4444',
}

let queue: HubTask[] = []
let leaveTimer: number | null = null
let expanded = false
let dragging = false
let dragMoved = false
let pressX = 0
let pressY = 0
let mounted = false

const STATUS_ORDER: TaskStatus[] = ['RUNNING', 'NEEDS_INPUT', 'FAILED_UNREAD', 'COMPLETED_UNREAD']

/** 壁纸状态缓存，供 orb 面板壁纸按钮使用 */
let wallpaperState: WallpaperState | null = null

async function refreshWallpaper(): Promise<void> {
  try {
    wallpaperState = await window.aihub.getWallpaper()
    applyWallpaper(wallpaperState)
    updateWallpaperToggle()
  } catch {
    // 主进程尚未就绪
  }
}

function updateWallpaperToggle(): void {
  const toggle = document.querySelector('.orb-bg-toggle') as HTMLElement | null
  if (!toggle) return
  const hasWallpaper = wallpaperState?.hasImage && wallpaperState?.dataUrl
  toggle.classList.toggle('active', !!hasWallpaper)
  toggle.textContent = hasWallpaper ? '🌄 已设' : '🌄 壁纸'
  toggle.title = hasWallpaper ? '更换 / 清除壁纸' : '选择一张桌面壁纸'
}

export function mountOrb(): void {
  if (mounted) return
  mounted = true

  let root = document.getElementById('orb-root')
  if (!root) {
    root = document.createElement('div')
    root.id = 'orb-root'
    document.body.append(root)
  }

  const ball = document.createElement('button')
  ball.className = 'orb-ball'
  ball.type = 'button'
  ball.title = '拖动移动 · 点击打开面板 · 悬停查看任务'
  ball.innerHTML = `<span class="orb-ring"></span><span class="orb-core">${BRAND_MARK_SVG}</span><span class="orb-count hidden">0</span>`

  const panel = document.createElement('div')
  panel.className = 'orb-panel'
  panel.innerHTML = `
    <div class="orb-panel-head">
      <h2>任务概览</h2>
      <button type="button" class="orb-bg-toggle" title="选择一张桌面壁纸">🌄 壁纸</button>
      <span class="hint">拖动可移动</span>
    </div>
    <div class="orb-stats"></div>
    <div class="orb-list"></div>
    <div class="orb-foot">
      <button type="button" class="primary" data-act="open">打开面板</button>
      <button type="button" data-act="read">一键已读</button>
    </div>
  `

  root.className = 'orb-root'
  root.append(panel, ball)

  const statsEl = panel.querySelector('.orb-stats') as HTMLElement
  const listEl = panel.querySelector('.orb-list') as HTMLElement
  const countEl = ball.querySelector('.orb-count') as HTMLElement

  const countBy = (status: TaskStatus): number => queue.filter((t) => t.status === status).length

  const dominantTone = (): string => {
    if (countBy('RUNNING')) return 'run'
    if (countBy('NEEDS_INPUT')) return 'input'
    if (countBy('FAILED_UNREAD')) return 'fail'
    if (countBy('COMPLETED_UNREAD')) return 'done'
    return 'idle'
  }

  const setExpanded = (next: boolean): void => {
    if (expanded === next) return
    expanded = next
    root!.classList.toggle('expanded', next)
    void window.aihub.setOrbPanelExpanded(next)
  }

  const scheduleCollapse = (): void => {
    if (leaveTimer) window.clearTimeout(leaveTimer)
    leaveTimer = window.setTimeout(() => setExpanded(false), 220)
  }

  const cancelCollapse = (): void => {
    if (leaveTimer) {
      window.clearTimeout(leaveTimer)
      leaveTimer = null
    }
  }

  const render = (): void => {
    const running = countBy('RUNNING')
    const needs = countBy('NEEDS_INPUT')
    const done = countBy('COMPLETED_UNREAD')
    const failed = countBy('FAILED_UNREAD')
    const total = queue.length

    ball.dataset.tone = dominantTone()
    ball.classList.toggle('pulse', running > 0)

    if (total > 0) {
      countEl.textContent = total > 99 ? '99+' : String(total)
      countEl.classList.remove('hidden')
    } else {
      countEl.classList.add('hidden')
    }

    const chips: Array<[string, number, string]> = [
      ['执行中', running, STATUS_COLOR.RUNNING],
      ['待输入', needs, STATUS_COLOR.NEEDS_INPUT],
      ['已完成', done, STATUS_COLOR.COMPLETED_UNREAD],
      ['失败', failed, STATUS_COLOR.FAILED_UNREAD],
    ]
    statsEl.replaceChildren(
      ...chips
        .filter(([, n]) => n > 0)
        .map(([label, n, c]) => {
          const el = document.createElement('span')
          el.className = 'orb-stat'
          el.style.setProperty('--c', c)
          el.innerHTML = `<span class="dot"></span>${label} ${n}`
          return el
        }),
    )
    if (!statsEl.childElementCount) {
      const idle = document.createElement('span')
      idle.className = 'orb-stat'
      idle.textContent = '空闲 · 暂无待处理'
      statsEl.append(idle)
    }

    listEl.replaceChildren()
    if (queue.length === 0) {
      const empty = document.createElement('div')
      empty.className = 'orb-empty'
      empty.textContent = '当前没有运行中或待查看的任务'
      listEl.append(empty)
      return
    }

    const sorted = [...queue].sort((a, b) => STATUS_ORDER.indexOf(a.status) - STATUS_ORDER.indexOf(b.status))
    for (const task of sorted.slice(0, 8)) {
      const color = STATUS_COLOR[task.status] ?? '#94a3b8'
      const btn = document.createElement('button')
      btn.type = 'button'
      btn.className = 'orb-item'
      btn.style.setProperty('--c', color)
      btn.innerHTML = `
        <div class="orb-item-top">
          <span class="orb-item-src">${SOURCE_LABELS[task.source] ?? task.source}</span>
          <span class="orb-item-status">${STATUS_LABELS[task.status] ?? task.status}</span>
        </div>
        <div class="orb-item-title"></div>
      `
      ;(btn.querySelector('.orb-item-title') as HTMLElement).textContent = displayTitle(task)
      btn.onclick = async (e) => {
        e.stopPropagation()
        await window.aihub.openTask(task.id)
        await reload()
      }
      listEl.append(btn)
    }
  }

  const reload = async (): Promise<void> => {
    try {
      queue = await window.aihub.getQueue()
    } catch {
      queue = []
    }
    render()
  }

  ball.addEventListener('mouseenter', () => {
    if (dragging) return
    cancelCollapse()
    setExpanded(true)
  })
  ball.addEventListener('mouseleave', () => {
    if (dragging) return
    scheduleCollapse()
  })
  panel.addEventListener('mouseenter', () => {
    cancelCollapse()
    setExpanded(true)
  })
  panel.addEventListener('mouseleave', () => {
    if (dragging) return
    scheduleCollapse()
  })

  ball.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return
    dragging = true
    dragMoved = false
    pressX = e.screenX
    pressY = e.screenY
    ball.classList.add('dragging')
    try {
      ball.setPointerCapture(e.pointerId)
    } catch {
      /* ignore */
    }
  })

  ball.addEventListener('pointermove', (e) => {
    if (!dragging) return
    const dx = e.screenX - pressX
    const dy = e.screenY - pressY
    if (!dragMoved && dx * dx + dy * dy > 25) {
      dragMoved = true
      cancelCollapse()
      setExpanded(false)
      // 超过阈值才开始拖窗口，避免单击被当成拖动
      window.aihub.startOrbDrag(pressX, pressY)
      window.aihub.moveOrbDrag(e.screenX, e.screenY)
    } else if (dragMoved) {
      window.aihub.moveOrbDrag(e.screenX, e.screenY)
    }
  })

  const endDrag = (e: PointerEvent): void => {
    if (!dragging) return
    const wasDrag = dragMoved
    dragging = false
    ball.classList.remove('dragging')
    try {
      ball.releasePointerCapture(e.pointerId)
    } catch {
      /* ignore */
    }
    if (wasDrag) window.aihub.endOrbDrag()
  }

  ball.addEventListener('pointerup', (e) => {
    const wasDrag = dragMoved
    endDrag(e)
    // 单击 / 双击：未拖动则打开主面板
    if (!wasDrag) void window.aihub.showMainWindow()
  })
  ball.addEventListener('pointercancel', endDrag)

  ball.addEventListener('dblclick', (e) => {
    e.preventDefault()
    e.stopPropagation()
    void window.aihub.showMainWindow()
  })

  // 阻止浏览器默认 click 与 pointerup 重复触发两次打开
  ball.addEventListener('click', (e) => {
    e.preventDefault()
    e.stopPropagation()
  })

  panel.querySelector('[data-act="open"]')!.addEventListener('click', () => {
    void window.aihub.showMainWindow()
  })
  panel.querySelector('[data-act="read"]')!.addEventListener('click', async () => {
    await window.aihub.readAllTasks()
    await reload()
  })

  // 壁纸切换：单击换壁纸，如果已有壁纸则弹出选择（可换或清除）
  const bgToggle = panel.querySelector('.orb-bg-toggle') as HTMLElement
  bgToggle.addEventListener('click', async (e) => {
    e.stopPropagation()
    try {
      const hasWallpaper = wallpaperState?.hasImage && wallpaperState?.dataUrl
      let next: WallpaperState | null = null
      if (hasWallpaper) {
        const action = await window.aihub.showWallpaperDialog()
        if (action === 'clear') {
          next = await window.aihub.clearWallpaper()
        } else if (action === 'pick') {
          next = await window.aihub.pickWallpaper()
        }
        // 'cancel' → next 保持 null，什么都不做
      } else {
        next = await window.aihub.pickWallpaper()
      }
      if (next) {
        wallpaperState = next
        applyWallpaper(wallpaperState)
        updateWallpaperToggle()
      }
    } catch {
      // 对话框被中断或主进程出错：保持现有壁纸状态，不产生未处理 rejection
    }
  })

  // 初始加载壁纸状态
  void refreshWallpaper()

  // 订阅共享状态而非重复注册 onTaskChanged：main.ts 已统一监听并 reload 更新 state.queue，
  // 这里只随 state 重渲染，避免每个事件触发两次 reload（M20）
  subscribe(() => {
    queue = state.queue
    render()
  })

  void reload()
}

export function applyUiMode(mode: 'panel' | 'orb'): void {
  document.documentElement.dataset.mode = mode
  const app = document.getElementById('app')
  const orb = document.getElementById('orb-root')
  const wall = document.getElementById('wallpaper')

  if (mode === 'orb') {
    mountOrb()
    if (app) app.style.display = 'none'
    // 保留壁纸在 orb 模式可见，作为背景层
    const root = document.getElementById('orb-root')
    if (root) root.style.display = 'block'
  } else {
    expanded = false
    if (app) {
      app.style.display = ''
      app.style.visibility = 'visible'
    }
    if (wall) wall.style.display = ''
    if (orb) {
      orb.style.display = 'none'
      orb.classList.remove('expanded')
    }
  }
}

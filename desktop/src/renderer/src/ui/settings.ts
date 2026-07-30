/* 设置视图：接入集成向导（一键接入 + 体检）、应用更新、诊断信息 */

import type { IntegrationsStatus, ServerStatus } from '../../../shared/types'
import { state } from '../state'
import { h, showToast, svgIcon } from './dom'

export function renderSettingsView(container: HTMLElement): void {
  container.append(h('div', 'content-header', [h('h1', '', ['设置'])]))

  const integrationsSection = h('section', 'settings-section', [
    h('h2', '', ['接入集成']),
    h('div', 'settings-loading', ['正在检测三平台接入状态…']),
  ])
  const updateSection = makeUpdateSection()
  const diagSection = h('section', 'settings-section', [
    h('h2', '', ['诊断']),
    h('div', 'settings-loading', ['正在读取后端状态…']),
  ])
  container.append(integrationsSection, updateSection, diagSection)

  void fillIntegrations(integrationsSection)
  void fillDiagnostics(diagSection)
}

/* ---------- 接入集成 ---------- */

async function fillIntegrations(section: HTMLElement): Promise<void> {
  let info: IntegrationsStatus
  try {
    info = await window.aihub.getIntegrations()
  } catch {
    section.querySelector('.settings-loading')!.textContent = '后端离线，无法检测接入状态'
    return
  }
  section.querySelector('.settings-loading')!.remove()

  const grid = h('div', 'int-grid', [
    claudeCard(info),
    codexCard(info),
    chatgptCard(info),
  ])
  section.append(grid)
}

function statusDot(ok: boolean | 'warn'): HTMLElement {
  const dot = h('span', 'dot')
  const color = ok === 'warn' ? 'var(--st-input)' : ok ? 'var(--st-done)' : 'var(--st-fail)'
  dot.style.background = color
  dot.style.boxShadow = `0 0 6px ${color}`
  return dot
}

function claudeCard(info: IntegrationsStatus): HTMLElement {
  const installed = info.claudeCode.installed
  const installBtn = h('button', 'btn primary', ['一键接入'])
  installBtn.disabled = installed
  installBtn.onclick = async () => {
    installBtn.disabled = true
    const res = await window.aihub.installClaude()
    if (res.success) {
      showToast(res.changed ? 'Claude Code 接入完成' : 'Claude Code 已接入，无需变更', 'var(--st-done)')
      refreshSettings()
    } else {
      showToast(res.error ?? '接入失败', 'var(--st-fail)')
      installBtn.disabled = false
    }
  }

  return h('div', 'int-card', [
    h('div', 'int-card-head', [statusDot(installed), h('span', 'int-name', ['Claude Code'])]),
    h('div', 'int-desc', ['通过 PostToolUse hook 上报 Bash/Edit 等工具事件']),
    h('div', 'int-path', [info.claudeCode.settingsPath]),
    h('div', 'int-status', [installed ? '已接入' : '未接入']),
    h('div', 'int-actions', [installBtn]),
  ])
}

function codexCard(info: IntegrationsStatus): HTMLElement {
  const { installed, stale, exeRunning, processCount } = info.codex
  const installBtn = h('button', 'btn primary', ['一键接入'])
  installBtn.disabled = installed
  installBtn.onclick = async () => {
    installBtn.disabled = true
    const res = await window.aihub.installCodex()
    if (res.success) {
      const extra = res.forwardTarget ? '（原 notify 命令已保留转发）' : ''
      showToast(res.changed ? `Codex 接入完成${extra}` : 'Codex 已接入，无需变更', 'var(--st-done)')
      refreshSettings()
    } else {
      showToast(res.error ?? '接入失败', 'var(--st-fail)')
      installBtn.disabled = false
    }
  }

  const rows: Array<Node | string> = [
    h('div', 'int-card-head', [statusDot(stale ? 'warn' : installed), h('span', 'int-name', ['Codex'])]),
    h('div', 'int-desc', ['改写 config.toml 的 notify 为链式转发（保留原命令）']),
    h('div', 'int-path', [info.codex.configPath]),
    h('div', 'int-status', [installed ? '已接入' : '未接入']),
  ]
  if (stale) {
    rows.push(
      h('div', 'int-warn', [
        `检测到 ${processCount} 个 Codex 进程早于配置写入启动，新事件不会上报——请重启 Codex（桌面 App 与终端会话）`,
      ]),
    )
  } else if (installed && exeRunning) {
    rows.push(h('div', 'int-ok', ['Codex 进程在线']))
  }
  rows.push(h('div', 'int-actions', [installBtn]))
  return h('div', 'int-card', rows)
}

function chatgptCard(info: IntegrationsStatus): HTMLElement {
  const online = info.chatgpt.installed
  const openDirBtn = h('button', 'btn', [svgIcon('folder'), '打开扩展目录'])
  openDirBtn.onclick = () => void window.aihub.openPath(info.chatgpt.extensionDir)

  const rows: Array<Node | string> = [
    h('div', 'int-card-head', [statusDot(online), h('span', 'int-name', ['ChatGPT 网页'])]),
    h('div', 'int-desc', ['Chrome/Edge 扩展监听 chatgpt.com 对话完成事件（含离线补偿队列）']),
    h('div', 'int-status', [
      online ? `扩展在线 v${info.chatgpt.version ?? ''}` : '未检测到扩展心跳',
    ]),
  ]
  if (!online) {
    rows.push(
      h('div', 'int-warn', [
        '在浏览器打开 chrome://extensions → 开发者模式 → 加载已解压的扩展程序 → 选择下方目录；装好后保持浏览器运行，10 分钟内自动上线',
      ]),
    )
  }
  rows.push(h('div', 'int-actions', [openDirBtn]))
  return h('div', 'int-card', rows)
}

function refreshSettings(): void {
  // 简单粗暴：重填当前视图（设置页无跨页状态）
  const content = document.querySelector('.content')
  if (content) {
    content.textContent = ''
    renderSettingsView(content as HTMLElement)
  }
}

/* ---------- 更新 ---------- */

function makeUpdateSection(): HTMLElement {
  const checkBtn = h('button', 'btn', [svgIcon('refresh'), '检查更新'])
  checkBtn.onclick = async () => {
    checkBtn.disabled = true
    await window.aihub.checkUpdates()
    setTimeout(() => {
      checkBtn.disabled = false
    }, 4000)
  }

  const statusLine = h('div', 'int-status', [updateText()])
  const section = h('section', 'settings-section', [
    h('h2', '', ['应用更新']),
    h('div', 'update-row', [statusLine, h('div', 'int-actions', [checkBtn])]),
  ])

  const us = state.updateState
  if (us?.state === 'downloaded') {
    const installBtn = h('button', 'btn primary', [`重启安装 v${us.version}`])
    installBtn.onclick = () => window.aihub.installUpdate()
    section.querySelector('.int-actions')!.append(installBtn)
  }
  return section
}

function updateText(): string {
  const us = state.updateState
  if (!us) return '打包安装后自动检查更新（每 4 小时）'
  switch (us.state) {
    case 'checking':
      return '正在检查更新…'
    case 'available':
      return `发现新版本 v${us.version}，后台下载中…`
    case 'downloading':
      return `正在下载更新 ${us.percent ?? 0}%`
    case 'downloaded':
      return `v${us.version} 已下载完成，可重启安装`
    case 'not-available':
      return '已是最新版本'
    case 'error':
      return `检查失败：${us.message ?? '网络异常'}（稍后自动重试）`
  }
}

/* ---------- 诊断 ---------- */

async function fillDiagnostics(section: HTMLElement): Promise<void> {
  let s: ServerStatus
  try {
    s = await window.aihub.getServerStatus()
  } catch {
    section.querySelector('.settings-loading')!.textContent = '后端离线'
    return
  }
  section.querySelector('.settings-loading')!.remove()

  const openLogBtn = h('button', 'btn', [svgIcon('folder'), '打开日志目录'])
  const logDir = s.logFile.replace(/[\\/][^\\/]+$/, '')
  openLogBtn.onclick = () => void window.aihub.openPath(logDir)

  const uptime =
    s.uptimeSec >= 3600
      ? `${Math.floor(s.uptimeSec / 3600)}h ${Math.floor((s.uptimeSec % 3600) / 60)}m`
      : `${Math.floor(s.uptimeSec / 60)}m ${s.uptimeSec % 60}s`

  section.append(
    h('div', 'diag-grid', [
      h('span', 'k', ['后端版本']),
      h('span', 'v', [`v${s.version}（运行 ${uptime}）`]),
      h('span', 'k', ['数据库']),
      h('span', 'v', [
        s.db.ok ? `正常 · ${s.db.database}@${s.db.host}:${s.db.port}` : '连接失败',
      ]),
      h('span', 'k', ['任务 / 事件']),
      h('span', 'v', [`${s.tasks ?? '—'} / ${s.events ?? '—'}`]),
      h('span', 'k', ['日志文件']),
      h('span', 'v path', [s.logFile]),
    ]),
    h('div', 'int-actions', [openLogBtn]),
  )
}

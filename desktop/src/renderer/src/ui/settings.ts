/* 设置视图：外观（壁纸）/ 接入集成 / 应用更新 / 诊断 */

import type {
  DbBackendValue,
  IntegrationsStatus,
  ServerStatus,
  UserIconState,
  WallpaperPrefs,
  WallpaperState,
} from '../../../shared/types'
import { state } from '../state'
import { h, showToast, svgIcon } from './dom'
import { applyWallpaper } from './wallpaper'

const INTEGRATIONS_CACHE_MS = 5000
let integrationsCache: { value: IntegrationsStatus; expiresAt: number } | null = null
let integrationsInFlight: Promise<IntegrationsStatus | null> | null = null

function loadIntegrations(): Promise<IntegrationsStatus | null> {
  if (integrationsCache && integrationsCache.expiresAt > Date.now()) {
    return Promise.resolve(integrationsCache.value)
  }
  if (!integrationsInFlight) {
    integrationsInFlight = window.aihub.getIntegrations()
      .then((value) => {
        if (value) {
          integrationsCache = {
            value,
            expiresAt: Date.now() + INTEGRATIONS_CACHE_MS,
          }
        }
        return value
      })
      .finally(() => {
        integrationsInFlight = null
      })
  }
  return integrationsInFlight
}

function invalidateIntegrationsCache(): void {
  integrationsCache = null
}

export function renderSettingsView(container: HTMLElement): void {
  container.append(h('div', 'content-header', [h('h1', '', ['设置'])]))

  const appearanceSection = makeAppearanceSection()
  const integrationsSection = h('section', 'settings-section', [
    h('h2', '', ['接入集成']),
    h('div', 'settings-loading', ['正在检测三平台接入状态…']),
  ])
  const dbBackendSection = makeDbBackendSection()
  const updateSection = makeUpdateSection()
  const diagSection = h('section', 'settings-section', [
    h('h2', '', ['诊断']),
    h('div', 'settings-loading', ['正在读取后端状态…']),
  ])
  container.append(appearanceSection, integrationsSection, dbBackendSection, updateSection, diagSection)

  if (state.backend === 'online') {
    void fillIntegrations(integrationsSection)
    void fillDiagnostics(diagSection)
  } else {
    integrationsSection.querySelector('.settings-loading')!.textContent = '后端正在启动，连接后自动检测'
    diagSection.querySelector('.settings-loading')!.textContent = '后端正在启动，连接后自动读取'
  }
}

/* ---------- 外观 / 壁纸 ---------- */

function makeAppearanceSection(): HTMLElement {
  const status = h('div', 'wallpaper-status', ['读取中…'])
  const presetWrap = h('div', 'icon-preset-wrap')
  const pickBtn = h('button', 'btn', ['选择本地壁纸…'])
  const clearBtn = h('button', 'btn', ['清除壁纸'])

  const blurVal = h('span', 'val', ['—'])
  const dimVal = h('span', 'val', ['—'])
  const opacityVal = h('span', 'val', ['—'])

  const blurInput = rangeInput(0, 40, 1)
  const dimInput = rangeInput(0, 80, 1)
  const opacityInput = rangeInput(8, 100, 1)

  let applying = false

  const applyState = (ws: WallpaperState): void => {
    applying = true
    applyWallpaper(ws)
    const selectedPresetId = ws.selection.source === 'preset' ? ws.selection.presetId : null
    presetWrap.replaceChildren(
      ...ws.presets.map((preset) => {
        const thumb = h('span', 'preset-thumb')
        if (preset.previewDataUrl) thumb.style.backgroundImage = `url("${preset.previewDataUrl}")`
        const btn = h('button', 'btn preset-choice', [thumb, h('span', '', [preset.name])])
        btn.classList.toggle(
          'primary',
          selectedPresetId === preset.id,
        )
        btn.onclick = async () => {
          try {
            const next = await window.aihub.setWallpaperPreset(preset.id)
            applyState(next)
            showToast(`已切换背景：${preset.name}`, 'var(--st-done)')
          } catch (err) {
            showToast(err instanceof Error ? err.message : '切换背景失败', 'var(--st-fail)')
          }
        }
        return btn
      }),
    )
    if (ws.selection.source === 'custom') {
      status.textContent = '已使用本地壁纸（保存在应用数据目录）'
    } else if (selectedPresetId !== null) {
      const name = ws.presets.find((preset) => preset.id === selectedPresetId)?.name
      status.textContent = `已使用内置主题：${name ?? selectedPresetId}`
    } else {
      status.textContent = '未设置壁纸（使用纯色背景）'
    }
    clearBtn.disabled = !ws.hasImage
    blurInput.value = String(ws.prefs.blur)
    dimInput.value = String(ws.prefs.dim)
    opacityInput.value = String(ws.prefs.opacity)
    blurVal.textContent = `${ws.prefs.blur}px`
    dimVal.textContent = `${ws.prefs.dim}%`
    opacityVal.textContent = `${ws.prefs.opacity}%`
    applying = false
  }

  const pushPrefs = async (partial: Partial<WallpaperPrefs>): Promise<void> => {
    if (applying) return
    applying = true
    try {
      const ws = await window.aihub.setWallpaperPrefs(partial)
      applyState(ws)
    } catch (err) {
      showToast(err instanceof Error ? err.message : '保存外观失败', 'var(--st-fail)')
    } finally {
      applying = false
    }
  }

  let debounceTimer: ReturnType<typeof setTimeout> | undefined
  const debouncePush = (partial: Partial<WallpaperPrefs>): void => {
    if (debounceTimer !== undefined) clearTimeout(debounceTimer)
    debounceTimer = setTimeout(() => {
      // 视图已卸载/重渲染（刷新设置页会重建外观区）则丢弃，避免对脱离文档的滑杆再写偏好
      if (!blurInput.isConnected) return
      void pushPrefs(partial)
    }, 200)
  }

  blurInput.oninput = () => {
    blurVal.textContent = `${blurInput.value}px`
    debouncePush({ blur: Number(blurInput.value) })
  }
  dimInput.oninput = () => {
    dimVal.textContent = `${dimInput.value}%`
    debouncePush({ dim: Number(dimInput.value) })
  }
  opacityInput.oninput = () => {
    opacityVal.textContent = `${opacityInput.value}%`
    debouncePush({ opacity: Number(opacityInput.value) })
  }

  pickBtn.onclick = async () => {
    pickBtn.disabled = true
    try {
      const ws = await window.aihub.pickWallpaper()
      applyState(ws)
      showToast(ws.hasImage ? '壁纸已更新' : '未选择图片', 'var(--st-done)')
    } catch (err) {
      showToast(err instanceof Error ? err.message : '选择壁纸失败', 'var(--st-fail)')
    } finally {
      pickBtn.disabled = false
    }
  }

  clearBtn.onclick = async () => {
    const ws = await window.aihub.clearWallpaper()
    applyState(ws)
    showToast('已恢复默认背景', 'var(--st-done)')
  }

  void window.aihub.getWallpaper().then(applyState).catch(() => {
    status.textContent = '无法读取壁纸配置'
  })

  return h('section', 'settings-section', [
    h('h2', '', ['外观']),
    h('div', 'wallpaper-panel', [
      h('div', 'preset-label', ['内置背景主题（自动适配 Light / Dark）']),
      presetWrap,
      h('div', 'wallpaper-actions', [pickBtn, clearBtn, status]),
      h('div', 'wallpaper-sliders', [
        h('div', 'wallpaper-slider-row', [h('span', '', ['模糊']), blurInput, blurVal]),
        h('div', 'wallpaper-slider-row', [h('span', '', ['暗角']), dimInput, dimVal]),
        h('div', 'wallpaper-slider-row', [h('span', '', ['面板']), opacityInput, opacityVal]),
      ]),
      h('div', 'wallpaper-status', [
        '效果对齐编辑器壁纸：图要透得出来。面板越低越透；暗角只做轻压暗保证可读；模糊建议保持 0。',
      ]),
    ]),
    makeIconPanel(),
  ])
}

/* ---------- 外观 / 应用图标 ---------- */

function makeIconPanel(): HTMLElement {
  const status = h('div', 'wallpaper-status', ['读取中…'])
  const presetWrap = h('div', 'icon-preset-wrap')
  const pickBtn = h('button', 'btn', ['选择本地图片…'])
  const resetBtn = h('button', 'btn', ['恢复默认'])

  const render = (s: UserIconState): void => {
    presetWrap.replaceChildren(
      ...s.presets.map((p) => {
        const thumb = h('span', 'preset-thumb')
        if (p.previewDataUrl) thumb.style.backgroundImage = `url("${p.previewDataUrl}")`
        const btn = h('button', 'btn icon-preset-btn preset-choice', [thumb, h('span', '', [p.name])])
        btn.dataset.presetId = p.id
        btn.classList.toggle('primary', s.prefs.source === 'preset' && s.prefs.presetId === p.id)
        btn.onclick = async () => {
          try {
            render(await window.aihub.setUserIconPreset(p.id))
            showToast(`已切换图标：${p.name}`, 'var(--st-done)')
          } catch (err) {
            showToast(err instanceof Error ? err.message : '切换图标失败', 'var(--st-fail)')
          }
        }
        return btn
      }),
    )
    const defaultId = s.presets[0]?.id ?? 'default'
    resetBtn.disabled = s.prefs.source === 'preset' && s.prefs.presetId === defaultId
    status.textContent =
      s.prefs.source === 'custom'
        ? '已使用本地图片（标题栏 / 悬浮球 / 窗口 / 托盘即时生效）'
        : '已使用内置预设（标题栏 / 悬浮球 / 窗口 / 托盘即时生效）'
  }

  pickBtn.onclick = async () => {
    pickBtn.disabled = true
    try {
      const s = await window.aihub.pickUserIcon()
      render(s)
      showToast(s.prefs.source === 'custom' ? '图标已更新为本地图片' : '未选择图片', 'var(--st-done)')
    } catch (err) {
      showToast(err instanceof Error ? err.message : '选择图标失败', 'var(--st-fail)')
    } finally {
      pickBtn.disabled = false
    }
  }

  resetBtn.onclick = async () => {
    try {
      const s = await window.aihub.resetUserIcon()
      render(s)
      showToast('已恢复默认图标', 'var(--st-done)')
    } catch (err) {
      showToast(err instanceof Error ? err.message : '恢复失败', 'var(--st-fail)')
    }
  }

  void window.aihub.getUserIcon().then(render).catch(() => {
    status.textContent = '无法读取图标配置'
  })

  return h('div', 'wallpaper-panel icon-panel', [
    h('div', 'preset-label', ['内置应用图标']),
    h('div', 'wallpaper-actions', [presetWrap]),
    h('div', 'wallpaper-actions', [pickBtn, resetBtn, status]),
    h('div', 'wallpaper-status', [
      '图标应用到标题栏、悬浮球球面、窗口/任务栏与托盘；重新打包安装包时也会更新安装包图标。',
    ]),
  ])
}

function rangeInput(min: number, max: number, step: number): HTMLInputElement {
  const input = document.createElement('input')
  input.type = 'range'
  input.min = String(min)
  input.max = String(max)
  input.step = String(step)
  return input
}

/* ---------- 接入集成 ---------- */

async function fillIntegrations(section: HTMLElement): Promise<void> {
  let info: IntegrationsStatus | null
  try {
    info = await loadIntegrations()
  } catch {
    if (!section.isConnected) return
    const loading = section.querySelector('.settings-loading')
    if (loading) loading.textContent = '接入状态检测失败，请稍后重试'
    return
  }
  if (!section.isConnected) return
  const loading = section.querySelector('.settings-loading')
  if (!loading) return
  if (!info) {
    loading.textContent = '接入状态检测超时，请稍后重试'
    return
  }
  loading.remove()

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
      invalidateIntegrationsCache()
      showToast(res.changed ? 'Claude Code 接入完成' : 'Claude Code 已接入，无需变更', 'var(--st-done)')
      refreshSettings()
    } else {
      showToast(res.error ?? '接入失败', 'var(--st-fail)')
      installBtn.disabled = false
    }
  }

  return h('div', 'int-card', [
    h('div', 'int-card-head', [statusDot(installed), h('span', 'int-name', ['Claude Code'])]),
    h('div', 'int-desc', ['通过 UserPromptSubmit / Notification / Stop 钩子上报会话事件']),
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
      invalidateIntegrationsCache()
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

/* ---------- 存储后端 ---------- */

const DB_BACKEND_OPTIONS: ReadonlyArray<{ value: DbBackendValue; label: string; desc: string }> = [
  { value: 'sqlite', label: '直接用 SQLite（默认，推荐）', desc: '数据保存在本地文件，零依赖开箱即用' },
  { value: 'mysql', label: '本机 MySQL', desc: '需自行安装配置 MySQL，连不上即报错' },
  { value: 'auto', label: '自动', desc: '优先本机 MySQL，连不上自动改用 SQLite' },
]

function dbBackendLabel(value: DbBackendValue): string {
  return DB_BACKEND_OPTIONS.find((opt) => opt.value === value)?.label ?? value
}

/** 诊断 / 存储区块共用：数据库定位文案（sqlite 无 host/port） */
function dbLocationText(db: ServerStatus['db']): string {
  if (db.backend === 'sqlite') return db.database ?? 'SQLite 本地文件'
  return `${db.database}@${db.host}:${db.port}`
}

/** 存储区块「当前」列：auto 配置按 /api/status 的实际 backend 展示实际落点 */
function dbBackendCurrentText(configured: DbBackendValue, db: ServerStatus['db']): string {
  const actual = db.backend === 'mysql' ? '本机 MySQL' : '直接使用 SQLite'
  return configured === 'auto' ? `自动（实际：${actual}）` : actual
}

function makeDbBackendSection(): HTMLElement {
  const current = h('span', 'db-backend-current', ['读取中…'])
  const select = document.createElement('select')
  select.className = 'db-backend-select'
  for (const opt of DB_BACKEND_OPTIONS) {
    const option = document.createElement('option')
    option.value = opt.value
    option.textContent = opt.label
    select.append(option)
  }

  // 选择器初值 = config.env 显式配置（未配置默认 sqlite）；「当前」列展示 /api/status 的实际后端。
  // 两路都是主进程本地 IPC / 后端 HTTP，独立失败都不致命，分别回退。
  void Promise.allSettled([window.aihub.getDbBackend(), window.aihub.getServerStatus()]).then(
    ([cfg, st]) => {
      if (!select.isConnected || !current.isConnected) return
      const configured = cfg.status === 'fulfilled' ? cfg.value.value : 'sqlite'
      select.value = configured
      current.textContent =
        st.status === 'fulfilled'
          ? dbBackendCurrentText(configured, st.value.db)
          : '后端离线，无法读取当前存储后端'
    },
  )

  select.onchange = async () => {
    const value = select.value as DbBackendValue
    select.disabled = true
    try {
      const res = await window.aihub.setDbBackend(value)
      if (res.ok) {
        showToast(`存储后端已设为「${dbBackendLabel(value)}」，重启后端后生效`, 'var(--st-done)')
      } else {
        showToast(res.error ?? '写入存储配置失败', 'var(--st-fail)')
      }
    } catch (err) {
      showToast(err instanceof Error ? err.message : '写入存储配置失败', 'var(--st-fail)')
    } finally {
      select.disabled = false
    }
  }

  return h('section', 'settings-section', [
    h('h2', '', ['存储后端']),
    h('div', 'wallpaper-panel', [
      h('div', 'wallpaper-actions', [select, current]),
      h('div', 'wallpaper-status', [
        ...DB_BACKEND_OPTIONS.map((opt) => h('div', '', [`${opt.label}：${opt.desc}`])),
      ]),
      h('div', 'wallpaper-status', ['切换写入 config.env，重启后端后生效；写入失败不影响使用']),
    ]),
  ])
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

  const buildBtn = h('button', 'btn primary', [svgIcon('gear'), '生成 exe 安装包'])
  const buildStatus = h('div', 'wallpaper-status', [
    '开发用：点按钮会先弹出确认框，再本地打包到 desktop/dist',
  ])
  // 安装版（打包进 app.asar）内没有 packaging/、.venv 等源码文件，无法应用内打包：
  // 禁用按钮并给出从源码打包的指引（主进程 buildExeWithConfirm 另有 app.isPackaged 兜底）
  void window.aihub.isPackaged().then((packaged) => {
    if (packaged) {
      buildBtn.disabled = true
      buildBtn.textContent = '安装版不可用'
      buildBtn.title = '安装版内没有打包所需的源码文件，请在源码仓库打包'
      buildStatus.textContent = '安装版不支持应用内打包，请到源码仓库运行 `cd desktop && npm run dist:local`'
    }
  })
  let cleanupPackagingStatus: (() => void) | null = null

  buildBtn.onclick = async () => {
    buildBtn.disabled = true
    buildStatus.textContent = '等待确认…'

    // 实时跟随主进程发回的打包步骤状态
    cleanupPackagingStatus?.()
    cleanupPackagingStatus = window.aihub.onPackagingStatus((s) => {
      // 视图被刷新（refreshSettings 重建设置区）后旧监听不再有可见 UI → 自行解除
      if (!buildStatus.isConnected) {
        cleanupPackagingStatus?.()
        cleanupPackagingStatus = null
        return
      }
      if (s.state === 'running') {
        buildStatus.textContent = `⏳ ${s.message}`
      } else if (s.state === 'error') {
        buildStatus.textContent = `❌ ${s.message}`
      } else if (s.state === 'done') {
        buildStatus.textContent = `✅ ${s.message}`
      }
    })

    try {
      const result = await window.aihub.buildExe()
      if (result.cancelled) {
        buildStatus.textContent = '已取消生成'
      } else if (result.ok) {
        buildStatus.textContent = `✅ ${result.message}`
        showToast(result.message, 'var(--st-done)')
      } else {
        const hint = result.missing
          ? { nsis: '请安装 NSIS 后重试', backend: '后端打包失败，见弹窗', python: '请配置 Python 环境' }[
              result.missing
            ]
          : ''
        buildStatus.textContent = `❌ ${result.message}${hint ? `（${hint}）` : ''}`
        showToast('生成失败，详见状态说明', 'var(--st-fail)')
      }
    } catch (err) {
      const msg = err instanceof Error ? err.message : '生成失败'
      buildStatus.textContent = `❌ ${msg}`
      showToast(msg, 'var(--st-fail)')
    } finally {
      buildBtn.disabled = false
      cleanupPackagingStatus?.()
      cleanupPackagingStatus = null
    }
  }

  const statusLine = h('div', 'int-status', [updateText()])
  const section = h('section', 'settings-section', [
    h('h2', '', ['应用更新 / 打包']),
    h('div', 'update-row', [statusLine, h('div', 'int-actions', [checkBtn])]),
    h('div', 'update-row', [
      h('div', '', [
        h('div', 'int-status', ['生成本地安装包（Setup.exe）']),
        buildStatus,
      ]),
      h('div', 'int-actions', [buildBtn]),
    ]),
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
        s.db.ok ? `正常 · ${dbLocationText(s.db)}` : '连接失败',
      ]),
      h('span', 'k', ['任务 / 事件']),
      h('span', 'v', [`${s.tasks ?? '—'} / ${s.events ?? '—'}`]),
      h('span', 'k', ['日志文件']),
      h('span', 'v path', [s.logFile]),
    ]),
    h('div', 'int-actions', [openLogBtn]),
  )
}

import { spawn, execFileSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { app, BrowserWindow, Notification, dialog, shell } from 'electron'
import { getUserIconSourcePath } from './icon-picker'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

// ---------------------------------------------------------------------------
// Windows 中文系统：Python / pip 默认输出 GBK，Node.js 按 UTF-8 收会乱码
// 用 TextDecoder 自动探测编码
// ---------------------------------------------------------------------------

const utf8Decoder = new TextDecoder('utf-8', { fatal: true })
const gbkDecoder = new TextDecoder('gbk', { fatal: true })

function decodeBuffer(buf: Buffer): string {
  try {
    return utf8Decoder.decode(buf)
  } catch {
    try {
      return gbkDecoder.decode(buf)
    } catch {
      // 最后兜底：用 lossy UTF-8，剔除 Unicode 替换字符（U+FFFD）
      return buf.toString('utf-8').replace(/�/g, '')
    }
  }
}

// ---------------------------------------------------------------------------
// 截断错误日志，避免对话框被撑爆
// ---------------------------------------------------------------------------

function tail(s: string, maxLen = 600): string {
  if (s.length <= maxLen) return s
  return '…（前面已省略）\n' + s.slice(-maxLen)
}

// ---------------------------------------------------------------------------
// 路径工具
// ---------------------------------------------------------------------------

export type BuildExeResult =
  | { ok: true; distDir: string; message: string }
  | { ok: false; cancelled?: boolean; message: string; missing?: 'nsis' | 'backend' | 'python' }

function desktopRoot(): string {
  return path.resolve(__dirname, '../..')
}

function repoRoot(): string {
  return path.resolve(desktopRoot(), '..')
}

function backendExePath(): string {
  return path.join(repoRoot(), 'packaging', 'dist', 'aihub-backend.exe')
}

function distDir(): string {
  return path.join(desktopRoot(), 'dist')
}

// ---------------------------------------------------------------------------
// 运行命令（不用 shell，消除 DEP0190 warning；自动处理 GBK/UTF-8）
// ---------------------------------------------------------------------------

function runCommand(
  command: string,
  args: string[],
  cwd: string,
  env?: NodeJS.ProcessEnv,
): Promise<{ code: number; log: string }> {
  return new Promise((resolve) => {
    const child = spawn(command, args, {
      cwd,
      env: { ...process.env, ...env, PYTHONIOENCODING: 'utf-8', FORCE_COLOR: '0' },
      windowsHide: true,
    })
    let log = ''
    child.stdout?.on('data', (buf: Buffer) => {
      log += decodeBuffer(buf)
    })
    child.stderr?.on('data', (buf: Buffer) => {
      log += decodeBuffer(buf)
    })
    child.on('error', (err) => {
      resolve({ code: 1, log: log + `\n${err.message}` })
    })
    child.on('close', (code) => {
      resolve({ code: code ?? 1, log })
    })
  })
}

// ---------------------------------------------------------------------------
// NSIS 检测
// ---------------------------------------------------------------------------

const NSIS_SEARCH_DIRS = [
  path.join(process.env.LOCALAPPDATA ?? '', 'Programs', 'NSIS'),
  path.join('E:', 'develop', 'NSIS'),
  path.join('D:', 'develop', 'NSIS'),
  path.join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'NSIS'),
  path.join(process.env['ProgramFiles'] ?? 'C:\\Program Files', 'NSIS'),
  path.join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'NSIS', 'Bin'),
]

function findNsis(): string | null {
  try {
    execFileSync('makensis', ['-VERSION'], { stdio: 'pipe', windowsHide: true })
    return 'makensis'
  } catch {
    // not in PATH
  }
  for (const dir of NSIS_SEARCH_DIRS) {
    const exe = path.join(dir, 'makensis.exe')
    if (fs.existsSync(exe)) return exe
  }
  return null
}

function nsisInstallHint(): string {
  return [
    '需要 NSIS（Nullsoft Scriptable Install System）来生成 Windows 安装包。',
    '',
    '安装方式（任选一种）：',
    '  1. winget install NSIS.NSIS',
    '  2. 从 https://nsis.sourceforge.io/Download 下载安装包',
    '',
    '安装后请重新启动本应用再试。',
  ].join('\n')
}

// ---------------------------------------------------------------------------
// Python 检测
// ---------------------------------------------------------------------------

function findPython(): string {
  const venvPython = path.join(repoRoot(), '.venv', 'Scripts', 'python.exe')
  if (fs.existsSync(venvPython)) return venvPython
  try {
    execFileSync('python', ['--version'], { stdio: 'pipe', windowsHide: true })
    return 'python'
  } catch {
    // nope
  }
  return ''
}

// ---------------------------------------------------------------------------
// 确保 PyInstaller 可用
// ---------------------------------------------------------------------------

async function ensurePyinstaller(
  python: string,
  win: BrowserWindow | null,
): Promise<'ok' | 'fail'> {
  const check = await runCommand(python, ['-m', 'pip', 'show', 'pyinstaller'], repoRoot())
  if (check.code === 0) return 'ok'

  win?.webContents.send('packaging:status', {
    state: 'running',
    message: '正在安装 PyInstaller…',
  })

  let result = await runCommand(python, ['-m', 'pip', 'install', 'pyinstaller'], repoRoot())
  if (result.code === 0) return 'ok'

  result = await runCommand('uv', ['pip', 'install', 'pyinstaller'], repoRoot())
  if (result.code === 0) return 'ok'

  return 'fail'
}

// ---------------------------------------------------------------------------
// 确保后端 exe（自动打包，不弹窗）
// ---------------------------------------------------------------------------

async function ensureBackendExe(win: BrowserWindow | null): Promise<string | null> {
  if (fs.existsSync(backendExePath())) return null

  win?.webContents.send('packaging:status', {
    state: 'running',
    message: '正在检查 Python 环境…',
  })

  const python = findPython()
  if (!python) {
    return '未找到 Python。请先在仓库根目录执行 `uv venv` 并 `uv pip install -r requirements.txt`'
  }

  const pipOk = await ensurePyinstaller(python, win)
  if (pipOk === 'fail') {
    return 'PyInstaller 安装失败。请手动执行：`uv pip install pyinstaller`'
  }

  win?.webContents.send('packaging:status', {
    state: 'running',
    message: '正在 PyInstaller 打包后端 exe（约 1~2 分钟）…',
  })

  const result = await runCommand(
    python,
    [
      '-m', 'PyInstaller',
      'packaging/backend.spec',
      '--distpath', 'packaging/dist',
      '--workpath', 'packaging/build',
      '--noconfirm',
    ],
    repoRoot(),
  )

  if (result.code !== 0 || !fs.existsSync(backendExePath())) {
    return `后端打包失败（退出码 ${result.code}）\n${tail(result.log)}`
  }

  return null
}

// ---------------------------------------------------------------------------
// 主入口：一键生成 Windows 安装包
// ---------------------------------------------------------------------------

export async function buildExeWithConfirm(win: BrowserWindow | null): Promise<BuildExeResult> {
  // 安装版（打包进 app.asar 后只读，仓库内的 packaging/、.venv、desktop/package.json 均不存在）
  // 不具备在应用内打包的条件，直接拒绝并给出从源码打包的指引（打包按钮在安装版中应已隐藏，此为兜底）。
  if (app.isPackaged) {
    return {
      ok: false,
      message: '安装版不支持在应用内打包：请从源码仓库运行 `cd desktop && npm run dist:local`',
    }
  }
  // ---- 确认 ----
  const selectedIcon = getUserIconSourcePath()
  const detailLines = [
    '将依次：检查工具链 → 打包后端 → 编译前端 → 生成安装包（约几分钟）。',
    '完成后自动打开 desktop/dist 目录。',
  ]
  if (selectedIcon) {
    detailLines.push(
      '',
      '将用当前所选图标重新生成安装包图标（会覆盖 resources/icon.png、tray.png、icon.ico 与 packaging/app.ico；重跑 desktop/scripts/make-icon.ps1 可恢复默认）。',
    )
  }
  detailLines.push('', '注意：请先退出正在运行的 AI Task Hub（含托盘 / npm run dev）。')
  const detail = detailLines.join('\n')

  const confirm = win
    ? await dialog.showMessageBox(win, {
        type: 'question',
        buttons: ['取消', '确定生成'],
        defaultId: 1,
        cancelId: 0,
        title: '生成安装包',
        message: '确定生成 exe 安装包？',
        detail,
      })
    : await dialog.showMessageBox({
        type: 'question',
        buttons: ['取消', '确定生成'],
        defaultId: 1,
        cancelId: 0,
        title: '生成安装包',
        message: '确定生成 exe 安装包？',
        detail,
      })
  if (confirm.response !== 1) {
    return { ok: false, cancelled: true, message: '已取消' }
  }

  // ---- Step 1: 检查 NSIS ----
  win?.webContents.send('packaging:status', {
    state: 'running',
    message: '正在检查 NSIS…',
  })

  if (!findNsis()) {
    win?.webContents.send('packaging:status', {
      state: 'error',
      message: '未安装 NSIS',
    })
    const ask = win
      ? await dialog.showMessageBox(win, {
          type: 'warning',
          buttons: ['取消', '打开 NSIS 下载页'],
          defaultId: 1,
          cancelId: 0,
          title: '缺少 NSIS',
          message: '本机未安装 NSIS',
          detail: nsisInstallHint(),
        })
      : await dialog.showMessageBox({
          type: 'warning',
          buttons: ['取消', '打开 NSIS 下载页'],
          defaultId: 1,
          cancelId: 0,
          title: '缺少 NSIS',
          message: '本机未安装 NSIS',
          detail: nsisInstallHint(),
        })
    if (ask.response === 1) {
      void shell.openExternal('https://nsis.sourceforge.io/Download')
    }
    return { ok: false, message: '未安装 NSIS', missing: 'nsis' }
  }

  // ---- Step 2: 确保后端 exe ----
  win?.webContents.send('packaging:status', {
    state: 'running',
    message: '正在确保后端 exe…',
  })

  const backendErr = await ensureBackendExe(win)
  if (backendErr) {
    win?.webContents.send('packaging:status', { state: 'error', message: backendErr })
    const isPythonMissing = backendErr.includes('未找到 Python')
    const detail = isPythonMissing
      ? backendErr
      : `${backendErr}\n\n如自动打包持续失败，可手动执行命令打包。`
    await (win
      ? dialog.showMessageBox(win, {
          type: 'error',
          buttons: ['确定'],
          title: '后端打包失败',
          message: '后端打包失败',
          detail,
        })
      : dialog.showMessageBox({
          type: 'error',
          buttons: ['确定'],
          title: '后端打包失败',
          message: '后端打包失败',
          detail,
        }))
    return {
      ok: false,
      message: backendErr,
      missing: isPythonMissing ? 'python' : 'backend',
    }
  }

  // ---- Step 2.5: 当前所选图标 → 重生成安装包图标（非致命，失败沿用现有图标） ----
  const selectedIcon2 = getUserIconSourcePath()
  if (selectedIcon2) {
    win?.webContents.send('packaging:status', {
      state: 'running',
      message: '正在用当前图标重生成安装包图标…',
    })
    const iconResult = await runCommand(
      'pwsh',
      [
        '-NoProfile',
        '-ExecutionPolicy',
        'Bypass',
        '-File',
        path.join(desktopRoot(), 'scripts', 'make-icon.ps1'),
        '-IconSource',
        selectedIcon2,
      ],
      desktopRoot(),
    )
    if (iconResult.code !== 0) {
      win?.webContents.send('packaging:status', {
        state: 'running',
        message: `当前图标重生成失败，沿用现有图标：${tail(iconResult.log)}`,
      })
    }
  }

  // ---- Step 3: 编译前端 ----
  win?.webContents.send('packaging:status', {
    state: 'running',
    message: '正在编译前端（electron-vite build）…',
  })

  const build = await runCommand('npm', ['run', 'build'], desktopRoot())
  if (build.code !== 0) {
    const message = `前端编译失败\n${tail(build.log)}`
    win?.webContents.send('packaging:status', { state: 'error', message })
    await (win
      ? dialog.showMessageBox(win, {
          type: 'error',
          buttons: ['确定'],
          title: '编译失败',
          message: '前端编译失败',
          detail: tail(build.log),
        })
      : dialog.showMessageBox({
          type: 'error',
          buttons: ['确定'],
          title: '编译失败',
          message: '前端编译失败',
          detail: tail(build.log),
        }))
    return { ok: false, message }
  }

  // ---- Step 4: 打包安装包 ----
  win?.webContents.send('packaging:status', {
    state: 'running',
    message: '正在 electron-builder 生成安装包（约 2~5 分钟）…',
  })

  const nsisExe = findNsis()!
  // When makensis is found in PATH, nsisExe is just 'makensis'; path.dirname('makensis') returns '.'
  // which would corrupt PATH when prepended, so we leave nsisPath null (no PATH modification needed).
  // When found by searching dirs, nsisExe is an absolute path, so we extract its directory.
  const nsisPath = nsisExe !== 'makensis' ? path.dirname(nsisExe) : null
  const packEnv: NodeJS.ProcessEnv = { CSC_IDENTITY_AUTO_DISCOVERY: 'false' }
  if (nsisPath) {
    packEnv.PATH = `${nsisPath};${process.env.PATH ?? ''}`
  }

  const pack = await runCommand(
    'npx',
    ['electron-builder', '--publish', 'never'],
    desktopRoot(),
    packEnv,
  )
  if (pack.code !== 0) {
    const message = `打包失败\n${tail(pack.log)}`
    win?.webContents.send('packaging:status', { state: 'error', message })
    await (win
      ? dialog.showMessageBox(win, {
          type: 'error',
          buttons: ['确定'],
          title: '打包失败',
          message: 'electron-builder 打包失败',
          detail: tail(pack.log),
        })
      : dialog.showMessageBox({
          type: 'error',
          buttons: ['确定'],
          title: '打包失败',
          message: 'electron-builder 打包失败',
          detail: tail(pack.log),
        }))
    return { ok: false, message }
  }

  // ---- 完成 ----
  const out = distDir()
  win?.webContents.send('packaging:status', {
    state: 'done',
    message: `已生成到 ${out}`,
  })
  new Notification({
    title: '安装包已生成',
    body: `输出目录：${out}`,
  }).show()
  void shell.openPath(out)
  return { ok: true, distDir: out, message: `已生成到 ${out}` }
}

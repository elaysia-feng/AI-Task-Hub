import { spawn } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { BrowserWindow, Notification, dialog, shell } from 'electron'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

export type BuildExeResult =
  | { ok: true; distDir: string; message: string }
  | { ok: false; cancelled?: boolean; message: string }

function desktopRoot(): string {
  // out/main → desktop/
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

function runCommand(
  command: string,
  args: string[],
  cwd: string,
  env?: NodeJS.ProcessEnv,
): Promise<{ code: number; log: string }> {
  return new Promise((resolve) => {
    const child = spawn(command, args, {
      cwd,
      env: { ...process.env, ...env, FORCE_COLOR: '0' },
      shell: true,
      windowsHide: true,
    })
    let log = ''
    child.stdout?.on('data', (buf: Buffer) => {
      log += buf.toString()
    })
    child.stderr?.on('data', (buf: Buffer) => {
      log += buf.toString()
    })
    child.on('error', (err) => {
      resolve({ code: 1, log: log + `\n${err.message}` })
    })
    child.on('close', (code) => {
      resolve({ code: code ?? 1, log })
    })
  })
}

async function ensureBackendExe(win: BrowserWindow | null): Promise<string | null> {
  if (fs.existsSync(backendExePath())) return null

  const opts = {
    type: 'warning' as const,
    buttons: ['取消', '先打包后端'],
    defaultId: 1,
    cancelId: 0,
    title: '缺少后端 exe',
    message: '尚未找到 packaging/dist/aihub-backend.exe',
    detail: '安装包需要内嵌后端。是否现在用 PyInstaller 生成？需要本机已安装 pyinstaller。',
  }
  const ask = win ? await dialog.showMessageBox(win, opts) : await dialog.showMessageBox(opts)
  if (ask.response !== 1) return '已取消：缺少后端 exe'

  const py = path.join(repoRoot(), '.venv', 'Scripts', 'python.exe')
  const python = fs.existsSync(py) ? `"${py}"` : 'python'
  const result = await runCommand(
    python,
    [
      '-m',
      'PyInstaller',
      'packaging/backend.spec',
      '--distpath',
      'packaging/dist',
      '--workpath',
      'packaging/build',
      '-y',
    ],
    repoRoot(),
  )
  if (result.code !== 0 || !fs.existsSync(backendExePath())) {
    return `后端打包失败（退出码 ${result.code}）\n${result.log.slice(-1200)}`
  }
  return null
}

/** 弹出确认框后本地生成 Windows 安装包（Setup.exe） */
export async function buildExeWithConfirm(win: BrowserWindow | null): Promise<BuildExeResult> {
  const opts = {
    type: 'question' as const,
    buttons: ['取消', '确定生成'],
    defaultId: 1,
    cancelId: 0,
    title: '生成安装包',
    message: '确定生成 exe 安装包？',
    detail:
      '将执行桌面端打包（electron-builder，约需几分钟）。\n完成后打开 desktop/dist 目录。\n\n注意：请先退出正在运行的 AI Task Hub（含托盘图标 / npm run dev），否则新 exe 会因单实例直接退出，看起来像打不开。',
  }
  const confirm = win ? await dialog.showMessageBox(win, opts) : await dialog.showMessageBox(opts)
  if (confirm.response !== 1) {
    return { ok: false, cancelled: true, message: '已取消' }
  }

  win?.webContents.send('packaging:status', { state: 'running', message: '正在检查后端…' })

  const backendErr = await ensureBackendExe(win)
  if (backendErr) {
    win?.webContents.send('packaging:status', { state: 'error', message: backendErr })
    return { ok: false, message: backendErr }
  }

  win?.webContents.send('packaging:status', { state: 'running', message: '正在编译并打包…' })

  const desktop = desktopRoot()
  // 先 vite/electron-vite build，再 builder（与 npm run dist:local 一致，但 publish never）
  const build = await runCommand('npm', ['run', 'build'], desktop)
  if (build.code !== 0) {
    const message = `前端编译失败\n${build.log.slice(-1500)}`
    win?.webContents.send('packaging:status', { state: 'error', message })
    return { ok: false, message }
  }

  const pack = await runCommand(
    'npx',
    [
      'electron-builder',
      '--publish',
      'never',
      '-c.electronDist=node_modules/electron/dist',
    ],
    desktop,
    { CSC_IDENTITY_AUTO_DISCOVERY: 'false' },
  )
  if (pack.code !== 0) {
    const message = `打包失败\n${pack.log.slice(-1500)}`
    win?.webContents.send('packaging:status', { state: 'error', message })
    return { ok: false, message }
  }

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

import { existsSync, mkdirSync } from 'node:fs'
import { spawnSync } from 'node:child_process'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const scriptDir = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(scriptDir, '../..')
const backendExe = path.join(repoRoot, 'packaging', 'dist', 'aihub-backend.exe')

// Windows 中文系统：强制 Python 输出 UTF-8，避免 GBK 乱码
const PY_ENV = { PYTHONIOENCODING: 'utf-8' }

if (existsSync(backendExe)) {
  console.log('[ensure-backend] 后端 exe 已就绪，跳过打包。')
  process.exit(0)
}

console.log('[ensure-backend] 未找到 packaging/dist/aihub-backend.exe，开始自动打包后端…')

// 确保输出目录存在
mkdirSync(path.dirname(backendExe), { recursive: true })

// 找 Python：优先 .venv，其次系统 python
function findVenvPython(repoRoot) {
  const subdir = process.platform === 'win32' ? path.join('Scripts', 'python.exe') : path.join('bin', 'python')
  return path.join(repoRoot, '.venv', subdir)
}
const venvPython = findVenvPython(repoRoot)
const python = existsSync(venvPython) ? venvPython : 'python'

// ---- 确保 PyInstaller 已安装 ----
const pipCheck = spawnSync(python, ['-m', 'pip', 'show', 'pyinstaller'], {
  cwd: repoRoot,
  stdio: 'pipe',
  env: { ...process.env, ...PY_ENV },
})
if (pipCheck.status !== 0) {
  console.log('[ensure-backend] 正在安装 PyInstaller…')

  // 先试 pip install
  let result = spawnSync(python, ['-m', 'pip', 'install', 'pyinstaller'], {
    cwd: repoRoot,
    stdio: 'inherit',
    env: { ...process.env, ...PY_ENV },
  })

  // pip 失败则尝试 uv pip（如果你用 uv 管理 venv）
  if (result.status !== 0) {
    const hasUv = spawnSync('uv', ['--version'], { stdio: 'ignore' }).status === 0
    if (!hasUv) {
      console.error('[ensure-backend] uv 未安装，跳过 uv pip fallback')
    } else {
      console.log('[ensure-backend] pip 失败，尝试 uv pip install…')
      result = spawnSync('uv', ['pip', 'install', 'pyinstaller'], {
        cwd: repoRoot,
        stdio: 'inherit',
        env: { ...process.env, ...PY_ENV },
      })
    }
  }

  if (result.status !== 0) {
    console.error('[ensure-backend] PyInstaller 安装失败，请手动执行：')
    console.error(`  uv pip install pyinstaller`)
    console.error('或：')
    console.error(`  ${python} -m pip install pyinstaller`)
    process.exit(result.status ?? 1)
  }
}

// ---- PyInstaller 打包 ----
console.log('[ensure-backend] 正在执行 PyInstaller…')
const result = spawnSync(
  python,
  [
    '-m', 'PyInstaller',
    'packaging/backend.spec',
    '--distpath', 'packaging/dist',
    '--workpath', 'packaging/build',
    '--noconfirm',
  ],
  {
    cwd: repoRoot,
    stdio: 'inherit',
    env: { ...process.env, ...PY_ENV },
  },
)

if (result.status !== 0 || !existsSync(backendExe)) {
  console.error('[ensure-backend] 后端打包失败，请检查 Python/依赖是否就绪。')
  console.error('手动重试：')
  console.error(`  cd ${repoRoot}`)
  console.error(`  ${python} -m PyInstaller packaging/backend.spec --distpath packaging/dist --workpath packaging/build --noconfirm`)
  process.exit(result.status ?? 1)
}

console.log('[ensure-backend] 后端 exe 打包完成。')

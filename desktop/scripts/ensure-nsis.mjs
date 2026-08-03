import { execFileSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import path from 'node:path'

// ---------------------------------------------------------------------------
// NSIS 检查 + 友好提示，确保 electron-builder 能生成 Setup.exe
// ---------------------------------------------------------------------------

const SEARCH_DIRS = [
  path.join(process.env.LOCALAPPDATA ?? '', 'Programs', 'NSIS'),
  // 常见自定义路径
  path.join('E:', 'develop', 'NSIS'),
  path.join('D:', 'develop', 'NSIS'),
  // 系统默认路径
  path.join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'NSIS'),
  path.join(process.env['ProgramFiles'] ?? 'C:\\Program Files', 'NSIS'),
  path.join(process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)', 'NSIS', 'Bin'),
]

function findNsis() {
  try {
    execFileSync('makensis', ['-VERSION'], { stdio: 'pipe', windowsHide: true })
    return 'makensis'
  } catch { /* not in PATH */ }
  for (const dir of SEARCH_DIRS) {
    const exe = path.join(dir, 'makensis.exe')
    if (existsSync(exe)) return exe
  }
  return null
}

const found = findNsis()

if (found === 'makensis') {
  console.log('[ensure-nsis] NSIS 已就绪（PATH 中找到 makensis）。')
  process.exit(0)
}

if (found) {
  // 找到了但不在 PATH —— 进程内改 PATH 会随脚本退出失效，对 electron-builder 无效，
  // 只如实报告位置（构建实际依赖 electron-builder 自带 NSIS 或已配置的系统 PATH）
  const nsisDir = path.dirname(found)
  console.log(`[ensure-nsis] 检测到 makensis（${nsisDir}），但不在 PATH 中；脚本内修改不生效，已跳过。`)
  process.exit(0)
}

// 没找到 —— 给出详细安装指引
console.error(`
╔════════════════════════════════════════════════════════════╗
║  [ensure-nsis] 未找到 NSIS（Nullsoft Scriptable Install）  ║
╠════════════════════════════════════════════════════════════╣
║                                                             ║
║  electron-builder 需要 NSIS 才能生成 Windows Setup.exe。    ║
║                                                             ║
║  安装方式（任选一种）：                                      ║
║    1. winget install NSIS.NSIS                              ║
║    2. 访问 https://nsis.sourceforge.io/Download 下载安装包  ║
║                                                             ║
║  安装后重新打开终端，再执行 npm run dist:local。              ║
║                                                             ║
║  如果安装到自定义目录（如 E:\\develop\\NSIS），请将其加入    ║
║  系统 PATH 环境变量，或从设置页的"一键打包"按钮打包          ║
║  （该按钮会自动搜索常见安装路径）。                          ║
║                                                             ║
╚════════════════════════════════════════════════════════════╝
`)
process.exit(1)

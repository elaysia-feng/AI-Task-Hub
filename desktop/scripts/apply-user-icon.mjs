// apply-user-icon.mjs —— electron-builder 预构建钩子
//
// 若用户设置过自定义应用图标（{userData}/icon/current.<ext>），则用该源图重跑
// make-icon.ps1，让打包出的 exe / 安装包也使用自定义图标（覆盖 resources/icon.png、
// tray.png、icon.ico 与 packaging/app.ico）。未设置自定义图标时 no-op（默认看板娘）。
//
// 说明：
//   - userData 目录：打包版为 %APPDATA%\AI Task Hub，dev 版为 %APPDATA%\ai-task-hub-desktop，
//     两者都探测。
//   - 重生成会覆盖 resources/ 下图标文件 → git status 变脏；重跑
//     `pwsh desktop/scripts/make-icon.ps1`（不带参数）可恢复默认。
//   - 失败不阻断打包：沿用现有图标继续，仅告警。

import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

function findCurrentIcon() {
  const appData = process.env.APPDATA
  if (!appData) return null
  for (const dirName of ['AI Task Hub', 'ai-task-hub-desktop']) {
    const iconDir = path.join(appData, dirName, 'icon')
    let names = []
    try {
      names = fs.readdirSync(iconDir)
    } catch {
      continue
    }
    for (const name of names) {
      if (name.startsWith('current.') && /\.(png|jpe?g|webp|bmp)$/i.test(name)) {
        return path.join(iconDir, name)
      }
    }
  }
  return null
}

const icon = findCurrentIcon()
if (!icon) {
  console.log('[apply-user-icon] 未设置自定义图标，使用默认图标打包')
  process.exit(0)
}
if (!fs.existsSync(icon)) {
  console.log(`[apply-user-icon] 自定义图标文件不存在（${icon}），使用默认图标打包`)
  process.exit(0)
}

const script = path.join(__dirname, 'make-icon.ps1')
console.log(`[apply-user-icon] 用自定义图标重生成安装包图标：${icon}`)
const res = spawnSync(
  'pwsh',
  ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, '-IconSource', icon],
  { stdio: 'inherit', windowsHide: true },
)
if (res.status !== 0) {
  // 非致命：打包仍可继续，但明确告警，避免用户以为用的是自定义图标
  console.warn(`[apply-user-icon] make-icon.ps1 退出码 ${res.status ?? 'N/A'}，沿用现有图标继续打包`)
  process.exit(0)
}
console.log('[apply-user-icon] 安装包图标已更新')

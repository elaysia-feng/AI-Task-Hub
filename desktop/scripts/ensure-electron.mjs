import { existsSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const scriptDir = path.dirname(fileURLToPath(import.meta.url))
const electronDir = path.resolve(scriptDir, '../node_modules/electron')
const installScript = path.join(electronDir, 'install.js')
const electronExe = path.join(electronDir, 'dist/electron.exe')
const pathFile = path.join(electronDir, 'path.txt')
const versionFile = path.join(electronDir, 'dist/version')
const packageFile = path.join(electronDir, 'package.json')

if (!existsSync(installScript)) {
  console.error('Electron npm 包不存在，请先运行 npm install。')
  process.exit(1)
}

if (!runtimeComplete()) {
  console.log('Electron 运行时缺失，正在补充安装…')
  const result = spawnSync(process.execPath, [installScript], {
    cwd: electronDir,
    stdio: 'inherit',
  })
  if (result.status !== 0) process.exit(result.status ?? 1)
}

if (!runtimeComplete()) {
  console.error('Electron 运行时安装失败，请检查网络后重试。')
  process.exit(1)
}

function runtimeComplete() {
  if (![electronExe, pathFile, versionFile, packageFile].every(existsSync)) return false
  try {
    const packageVersion = JSON.parse(readFileSync(packageFile, 'utf8')).version
    return (
      readFileSync(pathFile, 'utf8').trim() === 'electron.exe' &&
      readFileSync(versionFile, 'utf8').trim() === packageVersion
    )
  } catch {
    return false
  }
}

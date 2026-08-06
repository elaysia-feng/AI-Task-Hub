import fs from 'node:fs'
import path from 'node:path'
import { app } from 'electron'
import type { DbBackendValue } from '../shared/types'

const CONFIG_FILE = 'config.env'
const BACKEND_KEY = 'AIHUB_DB_BACKEND'

/**
 * 后端加载的候选配置之一：%APPDATA%\AI Task Hub\config.env
 * （见 app/database/mysql.py _config_candidates，打包版与开发态均读这里）。
 * appData 在 Windows 即 %APPDATA%，与 app name 无关，故打包/开发一致。
 */
export function configEnvPath(): string {
  return path.join(app.getPath('appData'), 'AI Task Hub', CONFIG_FILE)
}

/** 读 config.env 中 AIHUB_DB_BACKEND 的显式配置；文件不存在/键缺失/非法值一律回退 sqlite（默认） */
export function getConfiguredDbBackend(): { value: DbBackendValue; path: string } {
  const file = configEnvPath()
  return { value: readBackendKey(file), path: file }
}

/**
 * 把 AIHUB_DB_BACKEND 写入 config.env：保留其它键与注释，只替换目标行；
 * 文件不存在则新建（含父目录）。失败返回 { ok:false, error }，不抛异常（设置页非致命）。
 */
export function setConfiguredDbBackend(
  value: DbBackendValue,
): { ok: boolean; path?: string; error?: string } {
  const file = configEnvPath()
  try {
    fs.mkdirSync(path.dirname(file), { recursive: true })
    const lines = fs.existsSync(file) ? fs.readFileSync(file, 'utf-8').split(/\r?\n/) : []
    const entry = `${BACKEND_KEY}=${value}`
    let replaced = false
    const out = lines.map((line) => {
      if (/^\s*AIHUB_DB_BACKEND\s*=/.test(line)) {
        replaced = true
        return entry
      }
      return line
    })
    if (!replaced) out.push(entry)
    fs.writeFileSync(file, out.join('\n') + '\n', 'utf-8')
    return { ok: true, path: file }
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) }
  }
}

function readBackendKey(file: string): DbBackendValue {
  try {
    if (!fs.existsSync(file)) return 'sqlite'
    for (const raw of fs.readFileSync(file, 'utf-8').split(/\r?\n/)) {
      const line = raw.trim()
      if (!line || line.startsWith('#') || !line.includes('=')) continue
      const eq = line.indexOf('=')
      if (line.slice(0, eq).trim() !== BACKEND_KEY) continue
      const value = line.slice(eq + 1).trim().replace(/^['"]|['"]$/g, '')
      return value === 'auto' || value === 'mysql' || value === 'sqlite' ? value : 'sqlite'
    }
    return 'sqlite'
  } catch {
    return 'sqlite'
  }
}

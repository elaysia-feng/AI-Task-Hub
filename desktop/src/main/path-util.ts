import fs from 'node:fs'
import path from 'node:path'

const isWin = process.platform === 'win32'

/**
 * 校验 input 是否落在允许前缀目录下，返回解析后的安全绝对路径；否则返回 null。
 *
 * - 先用 realpath 解析 junction / symlink，防止用链接把前缀校验绕过到允许目录之外（M19）
 * - Windows / NTFS 大小写不敏感，前缀比较统一转小写（M18）
 * - 拒绝 URL 协议形式输入（http://、file://、javascript: 等）；Windows 盘符 `C:\` 不是协议
 * - 路径不存在时 realpath 抛错，回退到 normalize 结果继续做前缀校验
 */
export function resolveAllowedPath(
  input: string | null | undefined,
  prefixes: string[],
): string | null {
  if (!input) return null
  const normalized = path.normalize(input)
  // Windows 盘符（`C:\` 或 `C:/`）是合法绝对路径；其余 `scheme:` 前缀一律拒绝
  if (/^[a-z]+:/i.test(normalized) && !/^[a-zA-Z]:[\\/]/.test(normalized)) return null
  let resolved: string
  try {
    resolved = fs.realpathSync(normalized)
  } catch {
    resolved = normalized
  }
  if (!path.isAbsolute(resolved)) return null
  const key = isWin ? resolved.toLowerCase() : resolved
  for (const prefix of prefixes) {
    let prefixReal: string
    try {
      prefixReal = fs.realpathSync(prefix)
    } catch {
      prefixReal = path.normalize(prefix)
    }
    const pKey = isWin ? prefixReal.toLowerCase() : prefixReal
    if (key.startsWith(pKey)) return resolved
  }
  return null
}

/** 仅允许 http/https 的跳转目标，拦截 file://、javascript: 等危险协议（M16） */
export function isSafeExternalUrl(raw: string): boolean {
  try {
    const u = new URL(raw)
    return u.protocol === 'http:' || u.protocol === 'https:'
  } catch {
    return false
  }
}

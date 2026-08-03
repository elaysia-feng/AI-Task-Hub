import fs from 'node:fs'
import path from 'node:path'
import { app } from 'electron'

/** ui-preferences.json：orb 位置与壁纸外观共用这一份偏好文件 */
export function prefsPath(): string {
  return path.join(app.getPath('userData'), 'ui-preferences.json')
}

export function readPrefs(): Record<string, unknown> {
  try {
    return JSON.parse(fs.readFileSync(prefsPath(), 'utf-8')) as Record<string, unknown>
  } catch {
    return {}
  }
}

/**
 * 原子写（tmp + rename）ui-preferences.json。
 *
 * 同步实现：read-modify-write 在事件循环内不会交错（sync 代码不可重入），
 * 且 tmp+rename 保证写入中途崩溃不损坏文件。orb.ts 的 saveOrbPos 与 wallpaper.ts
 * 的 writeStoredPrefs 共用本函数，避免各自一套读写把对方的键覆盖掉（M17）。
 */
export function writePrefs(
  mutate: (existing: Record<string, unknown>) => Record<string, unknown>,
): void {
  const next = mutate(readPrefs())
  const tmp = prefsPath() + '.tmp'
  fs.writeFileSync(tmp, JSON.stringify(next, null, 2), 'utf-8')
  fs.renameSync(tmp, prefsPath())
}

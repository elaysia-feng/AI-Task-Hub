/* 存储后端选择器的 config.env 读写逻辑：文件不存在新建 / 只替换目标键 / 含空格路径 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

// db-backend.ts 依赖 electron.app 取 %APPDATA%；用可变的 mock 路径隔离纯文件逻辑
const h = vi.hoisted(() => ({ appData: '' }))
vi.mock('electron', () => ({
  app: { getPath: () => h.appData },
}))

import { configEnvPath, getConfiguredDbBackend, setConfiguredDbBackend } from './db-backend'

describe('db-backend config.env 读写', () => {
  let root: string

  beforeEach(() => {
    root = fs.mkdtempSync(path.join(os.tmpdir(), 'aihub-db-backend-'))
    h.appData = root
  })

  afterEach(() => {
    fs.rmSync(root, { recursive: true, force: true })
  })

  it('文件不存在时新建并写入 AIHUB_DB_BACKEND', () => {
    const file = configEnvPath()
    const res = setConfiguredDbBackend('sqlite')
    expect(res.ok).toBe(true)
    expect(fs.existsSync(file)).toBe(true)
    expect(fs.readFileSync(file, 'utf-8')).toContain('AIHUB_DB_BACKEND=sqlite')
    expect(getConfiguredDbBackend().value).toBe('sqlite')
  })

  it('文件已存在时只替换 AIHUB_DB_BACKEND 行，保留注释与其它键', () => {
    const file = configEnvPath()
    fs.mkdirSync(path.dirname(file), { recursive: true })
    fs.writeFileSync(
      file,
      '# 用户配置\nAIHUB_DB_BACKEND=mysql\nAIHUB_MYSQL_HOST=localhost\n',
      'utf-8',
    )
    const res = setConfiguredDbBackend('auto')
    expect(res.ok).toBe(true)
    const content = fs.readFileSync(file, 'utf-8')
    expect(content).toContain('# 用户配置')
    expect(content).toContain('AIHUB_MYSQL_HOST=localhost')
    expect(content).toContain('AIHUB_DB_BACKEND=auto')
    expect(content.match(/AIHUB_DB_BACKEND=/g)).toHaveLength(1)
    expect(getConfiguredDbBackend().value).toBe('auto')
  })

  it('Windows 含空格路径可正常写入与回读', () => {
    // mock 的 %APPDATA% 本身带空格，模拟 C:\Users\...\App Data\...
    h.appData = path.join(root, 'App Data Folder')
    const file = configEnvPath()
    expect(file).toContain(' ')
    const res = setConfiguredDbBackend('mysql')
    expect(res.ok).toBe(true)
    expect(fs.existsSync(file)).toBe(true)
    expect(getConfiguredDbBackend().value).toBe('mysql')
  })
})

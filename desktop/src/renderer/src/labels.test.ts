import { describe, expect, it } from 'vitest'
import { displayTitle } from '../../shared/labels'

describe('displayTitle', () => {
  it('prefers human title', () => {
    expect(displayTitle({ title: '修登录', contentPreview: '其他' })).toBe('修登录')
  })

  it('falls back to content preview', () => {
    expect(displayTitle({ title: null, contentPreview: '请帮我写交接文档' })).toBe('请帮我写交接文档')
  })

  it('truncates long preview', () => {
    const preview = '字'.repeat(80)
    const result = displayTitle({ title: '', contentPreview: preview })
    expect(result.endsWith('…')).toBe(true)
    expect(result.length).toBe(51)
  })

  it('skips vite log noise and uses project name', () => {
    expect(
      displayTitle({
        title: '12:08:19 [vite] Internal server error: Unable to parse HTML',
        contentPreview: 'Claude is waiting for your input',
        projectPath: 'C:\\Users\\wlzx\\Desktop\\python-report-dev',
      }),
    ).toBe('python-report-dev 会话')
  })

  it('skips task-notification markup', () => {
    expect(
      displayTitle({
        title: '<task-notification> <task-id>a7e42912</task-id> <to>x</to>',
        projectPath: 'D:/projects/demo',
      }),
    ).toBe('demo 会话')
  })

  it('extracts summary from markup when present', () => {
    expect(
      displayTitle({
        title: '<task-notification><summary>修复登录鉴权</summary></task-notification>',
      }),
    ).toBe('修复登录鉴权')
  })

  it('uses placeholder when empty', () => {
    expect(displayTitle({ title: null, contentPreview: null })).toBe('(无标题任务)')
  })
})

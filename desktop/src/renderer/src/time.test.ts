import { describe, expect, it } from 'vitest'
import { formatRelativeTime } from './time'

describe('formatRelativeTime', () => {
  it('空值与非法输入返回空串', () => {
    expect(formatRelativeTime(null)).toBe('')
    expect(formatRelativeTime(undefined)).toBe('')
    expect(formatRelativeTime('not-a-date')).toBe('')
  })

  it('一分钟内显示刚刚', () => {
    expect(formatRelativeTime(new Date().toISOString())).toBe('刚刚')
  })

  it('一小时内显示 N 分钟前', () => {
    const tenMinAgo = new Date(Date.now() - 10 * 60 * 1000).toISOString()
    expect(formatRelativeTime(tenMinAgo)).toBe('10 分钟前')
  })

  it('同一天内显示 N 小时前', () => {
    const threeHoursAgo = new Date(Date.now() - 3 * 3600 * 1000)
    const now = new Date()
    if (threeHoursAgo.getDate() === now.getDate()) {
      expect(formatRelativeTime(threeHoursAgo.toISOString())).toBe('3 小时前')
    }
  })

  it('昨天显示 昨天 HH:mm（UTC）', () => {
    // Uses UTC to avoid DST boundary skew; formatRelativeTime also uses UTC
    const yesterday = new Date()
    yesterday.setUTCDate(yesterday.getUTCDate() - 1)
    yesterday.setUTCHours(15, 30, 0, 0)
    expect(formatRelativeTime(yesterday.toISOString())).toBe('昨天 15:30')
  })

  it('跨年显示完整日期', () => {
    const old = new Date('2020-06-15T12:00:00')
    expect(formatRelativeTime(old.toISOString())).toBe('2020-06-15')
  })
})

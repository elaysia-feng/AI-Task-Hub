/** ISO 时间串 → 中文相对时间 */
export function formatRelativeTime(iso: string | null | undefined): string {
  if (!iso) return ''
  const date = new Date(iso)
  if (Number.isNaN(date.getTime())) return ''

  // Use UTC to avoid DST boundary skew
  const now = new Date()
  const diffSec = Math.floor((now.getTime() - date.getTime()) / 1000)
  if (diffSec < 60) return '刚刚'
  if (diffSec < 3600) return `${Math.floor(diffSec / 60)} 分钟前`
  if (diffSec < 86400 && isSameDay(now, date)) {
    return `${Math.floor(diffSec / 3600)} 小时前`
  }

  const yesterday = new Date(now)
  yesterday.setDate(now.getDate() - 1)
  const hm = formatHM(date)
  if (isSameDay(yesterday, date)) return `昨天 ${hm}`
  if (date.getUTCFullYear() === now.getUTCFullYear()) {
    return `${pad(date.getUTCMonth() + 1)}-${pad(date.getUTCDate())} ${hm}`
  }
  return `${date.getUTCFullYear()}-${pad(date.getUTCMonth() + 1)}-${pad(date.getUTCDate())}`
}

function isSameDay(a: Date, b: Date): boolean {
  return (
    a.getUTCFullYear() === b.getUTCFullYear() &&
    a.getUTCMonth() === b.getUTCMonth() &&
    a.getUTCDate() === b.getUTCDate()
  )
}

function formatHM(d: Date): string {
  return `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}`
}

function pad(n: number): string {
  return String(n).padStart(2, '0')
}

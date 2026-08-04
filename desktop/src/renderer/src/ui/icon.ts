import type { UserIconState } from '../../../shared/types'

/**
 * 应用图标渲染端工具：把主进程推送的图标状态应用到标题栏与悬浮球球面。
 *
 * 时序安全：图标状态在 bootstrap 拉取（或 onIconChanged 推送），球可能稍后才挂载
 * （mountOrb 只在 orb 模式激活时执行）。因此这里缓存最近一次状态，applyIconToBall
 * 在球创建后调用即可立即生效。
 */

export interface OrbFaceStyle {
  /** 给 --orb-face 的值：url("…") 或 none */
  face: string
  /** 给 --orb-face-size 的值：预设放大裁角，自定义 cover 铺满 */
  size: string
}

/** 纯函数：根据图标状态算出球面 CSS 变量 */
export function faceStyleFor(state: UserIconState): OrbFaceStyle {
  if (!state.dataUrl) return { face: 'none', size: '125% auto' }
  const custom = state.prefs.source === 'custom'
  return { face: `url("${state.dataUrl}")`, size: custom ? 'cover' : '125% auto' }
}

let cachedState: UserIconState | null = null

/** 记录最新状态并应用到已挂载的球（未挂载则等 mountOrb 时经 applyIconToBall 应用） */
export function applyIcon(state: UserIconState): void {
  cachedState = state
  const logo = document.querySelector('.titlebar .logo') as HTMLElement | null
  if (logo) {
    const { face, size } = faceStyleFor(state)
    logo.style.setProperty('--titlebar-icon', face)
    logo.style.setProperty('--titlebar-icon-size', size)
    logo.classList.toggle('has-image', face !== 'none')
  }
  const ball = document.querySelector('.orb-ball') as HTMLElement | null
  if (ball) applyIconToBall(ball)
}

/** 球创建后调用：用缓存的图标状态设置球面 */
export function applyIconToBall(ball: HTMLElement): void {
  if (!cachedState) return
  const { face, size } = faceStyleFor(cachedState)
  ball.style.setProperty('--orb-face', face)
  ball.style.setProperty('--orb-face-size', size)
}

import { describe, expect, it } from 'vitest'
import type { UserIconState } from '../../../shared/types'
import { faceStyleFor } from './icon'

function stateOf(partial: Partial<UserIconState>): UserIconState {
  return {
    prefs: { source: 'preset', presetId: 'default' },
    dataUrl: null,
    presets: [],
    ...partial,
  }
}

describe('faceStyleFor', () => {
  it('自定义图片 → cover 铺满', () => {
    const s = stateOf({
      prefs: { source: 'custom' },
      dataUrl: 'data:image/png;base64,xxx',
    })
    expect(faceStyleFor(s)).toEqual({
      face: 'url("data:image/png;base64,xxx")',
      size: 'cover',
    })
  })

  it('内置预设 → 125% auto 放大裁角', () => {
    const s = stateOf({
      prefs: { source: 'preset', presetId: 'default' },
      dataUrl: 'data:image/png;base64,xxx',
    })
    expect(faceStyleFor(s)).toEqual({
      face: 'url("data:image/png;base64,xxx")',
      size: '125% auto',
    })
  })

  it('无图标 → none', () => {
    const s = stateOf({ prefs: { source: 'preset', presetId: 'default' }, dataUrl: null })
    expect(faceStyleFor(s)).toEqual({ face: 'none', size: '125% auto' })
  })
})

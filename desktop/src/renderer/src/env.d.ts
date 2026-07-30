/// <reference types="vite/client" />

import type { AihubApi } from '../../shared/types'

declare global {
  interface Window {
    aihub: AihubApi
  }
}

export {}

/* DOM 辅助：内联 SVG 图标、h() 构造器、Toast */

const ICONS = {
  logo: '<svg viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="2.4" stroke-linecap="round"><circle cx="12" cy="5" r="2.2" fill="white" stroke="none"/><circle cx="5" cy="18" r="2.2" fill="white" stroke="none"/><circle cx="19" cy="18" r="2.2" fill="white" stroke="none"/><path d="M12 7.5 6 16M12 7.5 18 16M7 18h10"/></svg>',
  minus: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><path d="M2 6h8"/></svg>',
  close: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><path d="M2.5 2.5 9.5 9.5M9.5 2.5 2.5 9.5"/></svg>',
  check: '<svg viewBox="0 0 24 24" fill="none" stroke="#a78bfa" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 12.5 9.5 18 20 6.5"/></svg>',
  inbox: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 13h4l2 3h6l2-3h4"/><path d="M5 6h14l2 7v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5z"/></svg>',
  inboxArt: '<svg viewBox="0 0 24 24" fill="none" stroke="#a78bfa" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 13h4l2 3h6l2-3h4"/><path d="M5 6h14l2 7v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5z"/></svg>',
  history: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 3v5h5"/><path d="M3.05 13a9 9 0 1 0 .5-4.5L3 8"/><path d="M12 7v5l3.5 2"/></svg>',
  external: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M4.5 2.5H2.5v7h7V7.5M7 2h3v3M10 2 5.5 6.5"/></svg>',
  trash: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M1.5 3h9M4.5 3V1.8h3V3M2.8 3l.6 7.2h5.2L9.2 3"/></svg>',
  ignore: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"><circle cx="6" cy="6" r="4.6"/><path d="M3 3l6 6"/></svg>',
  sun: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>',
  moon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>',
  gear: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.87l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.7 1.7 0 0 0-1.87-.34 1.7 1.7 0 0 0-1 1.55V21a2 2 0 1 1-4 0v-.09a1.7 1.7 0 0 0-1-1.55 1.7 1.7 0 0 0-1.87.34l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-1.55-1H3a2 2 0 1 1 0-4h.09a1.7 1.7 0 0 0 1.55-1 1.7 1.7 0 0 0-.34-1.87l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.7 1.7 0 0 0 9 4.6a1.7 1.7 0 0 0 1-1.55V3a2 2 0 1 1 4 0v.09a1.7 1.7 0 0 0 1 1.55 1.7 1.7 0 0 0 1.87-.34l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.7 1.7 0 0 0-.34 1.87 1.7 1.7 0 0 0 1.55 1H21a2 2 0 1 1 0 4h-.09a1.7 1.7 0 0 0-1.55 1z"/></svg>',
  refresh: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M10.5 6a4.5 4.5 0 1 1-1.3-3.2M10.5 1.5v2.6H7.9"/></svg>',
  folder: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M1.5 3.5v5a1 1 0 0 0 1 1h7a1 1 0 0 0 1-1v-4a1 1 0 0 0-1-1H6L5 2.5H2.5a1 1 0 0 0-1 1z"/></svg>',
  link: '<svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"><path d="M7 5 5 7M5.2 3.4l1-1a2.1 2.1 0 0 1 3 3l-1 1M6.8 8.6l-1 1a2.1 2.1 0 0 1-3-3l1-1"/></svg>',
} as const

export type IconName = keyof typeof ICONS

export function h<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className?: string,
  children?: Array<Node | string>,
): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag)
  if (className) el.className = className
  if (children) el.append(...children)
  return el
}

export function svgIcon(name: IconName): HTMLElement {
  const span = document.createElement('span')
  span.innerHTML = ICONS[name]
  return span.firstElementChild as HTMLElement
}

/* ---------- Toast ---------- */

let toastRoot: HTMLElement | null = null

export function showToast(text: string, accent?: string): void {
  if (!toastRoot) {
    toastRoot = h('div', 'toast-root')
    document.body.append(toastRoot)
  }
  const toast = h('div', 'toast', [h('span', 'dot'), h('span', '', [text])])
  if (accent) toast.style.setProperty('--toast-accent', accent)
  toastRoot.append(toast)
  setTimeout(() => {
    toast.classList.add('leaving')
    toast.addEventListener('animationend', () => toast.remove(), { once: true })
  }, 3600)
}

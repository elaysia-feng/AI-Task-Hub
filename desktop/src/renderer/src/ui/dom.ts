/* DOM 辅助：内联 SVG 图标、h() 构造器、Toast */

/* 统一 24×24 / stroke 1.75 / currentColor */
const S =
  'viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"'

const ICONS = {
  logo:
    '<svg viewBox="0 0 24 24" fill="none">' +
    '<circle cx="12" cy="12" r="2.6" fill="#fff"/>' +
    '<circle cx="5" cy="7" r="1.85" fill="#fff"/>' +
    '<circle cx="19" cy="7" r="1.85" fill="#fff"/>' +
    '<circle cx="5" cy="17" r="1.85" fill="#fff"/>' +
    '<circle cx="19" cy="17" r="1.85" fill="#fff"/>' +
    '<path d="M6.6 8 10.1 10.4M17.4 8 13.9 10.4M6.6 16 10.1 13.6M17.4 16 13.9 13.6" stroke="#fff" stroke-width="1.65" stroke-linecap="round"/>' +
    '</svg>',
  minus: `<svg ${S}><path d="M5 12h14"/></svg>`,
  close: `<svg ${S}><path d="m6 6 12 12M18 6 6 18"/></svg>`,
  check: `<svg ${S}><path d="m5 12.5 4.5 4.5L19 7.5"/></svg>`,
  inbox: `<svg ${S}><polyline points="22 12 16 12 14 15 10 15 8 12 2 12"/><path d="M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"/></svg>`,
  inboxArt: `<svg ${S}><polyline points="22 12 16 12 14 15 10 15 8 12 2 12"/><path d="M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"/></svg>`,
  history: `<svg ${S}><path d="M3 12a9 9 0 1 0 3-6.7"/><path d="M3 4v5h5"/><path d="M12 7v5l3 2"/></svg>`,
  external: `<svg ${S}><path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/></svg>`,
  trash: `<svg ${S}><path d="M3 6h18"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M10 11v6M14 11v6"/></svg>`,
  ignore: `<svg ${S}><circle cx="12" cy="12" r="9"/><path d="m5.2 5.2 13.6 13.6"/></svg>`,
  sun: `<svg ${S}><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41"/></svg>`,
  moon: `<svg ${S}><path d="M21 14.5A8.5 8.5 0 1 1 9.5 3 7 7 0 0 0 21 14.5z"/></svg>`,
  gear: `<svg ${S}><path d="M20 7h-9"/><path d="M14 17H5"/><circle cx="17" cy="17" r="3"/><circle cx="7" cy="7" r="3"/></svg>`,
  refresh: `<svg ${S}><path d="M21 12a9 9 0 1 1-2.64-6.36"/><path d="M21 3v6h-6"/></svg>`,
  folder: `<svg ${S}><path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9l-.81-1.2A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2z"/></svg>`,
  link: `<svg ${S}><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>`,
  orb: `<svg ${S}><circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="3.2"/></svg>`,
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

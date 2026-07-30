/**
 * AI Task Hub · ChatGPT Connector — 内容脚本
 * 完成检测（三重判断）：最后一条消息是 assistant + 停止按钮消失 + 文本稳定一段时间
 * 已读检测：页面可见且聚焦 + 回答进入可视区域 → TASK_VIEWED
 *
 * 注意：依赖 ChatGPT 页面 DOM 结构（data-message-author-role / data-testid），
 * 页面改版后若检测失效，需同步更新选择器。
 */

const STABLE_DELAY_MS = 1500
const OBSERVE_DEBOUNCE_MS = 400
const PREVIEW_LEN = 200

let lastReportedFingerprint = null // 已上报的回答指纹（防重复）
let completionTimer = null
let pendingCompletion = null // 等待"用户查看"的完成记录

/* ---------- 事件发送 ---------- */

function sendEvent(event) {
  try {
    chrome.runtime.sendMessage({ type: 'AIHUB_EVENT', event })
  } catch {
    // 扩展重载后上下文失效，忽略
  }
}

/* ---------- DOM 提取 ---------- */

function getAssistantMessages() {
  return [...document.querySelectorAll('[data-message-author-role="assistant"]')]
}

function getLastAssistantMessage() {
  const list = getAssistantMessages()
  return list.length ? list[list.length - 1] : null
}

function isGenerating() {
  // 生成中会出现"停止生成"按钮（中英文界面兼容）
  return Boolean(
    document.querySelector(
      'button[aria-label*="Stop"], button[aria-label*="停止"], button[data-testid="stop-button"]',
    ),
  )
}

function getConversationId() {
  const match = location.pathname.match(/\/c\/([\w-]+)/)
  return match ? match[1] : null
}

function getConversationTitle() {
  // ChatGPT 会把对话标题写入 document.title（格式："标题 - ChatGPT"）
  const raw = document.title.replace(/\s*[-–|]\s*ChatGPT\s*$/i, '').trim()
  return raw || 'ChatGPT 对话'
}

function getPreviewText(messageEl) {
  const text = (messageEl?.innerText ?? '').trim().replace(/\s+/g, ' ')
  return text.slice(0, PREVIEW_LEN) + (text.length > PREVIEW_LEN ? '…' : '')
}

function fingerprintOf(messageEl) {
  // 回答指纹：消息总数 + 最后一条文本长度，回答继续增长时指纹变化
  return `${getAssistantMessages().length}:${(messageEl?.innerText ?? '').length}`
}

/* ---------- 完成检测 ---------- */

function checkCompleted() {
  if (isGenerating()) return
  const last = getLastAssistantMessage()
  if (!last) return

  const fingerprint = fingerprintOf(last)
  if (fingerprint === lastReportedFingerprint) return

  // 等文本稳定后再确认一次（流式输出尾部的抖动）
  clearTimeout(completionTimer)
  completionTimer = setTimeout(() => {
    if (isGenerating()) return
    const current = getLastAssistantMessage()
    if (!current || fingerprintOf(current) !== fingerprint) return
    reportCompleted(current, fingerprint)
  }, STABLE_DELAY_MS)
}

function reportCompleted(messageEl, fingerprint) {
  lastReportedFingerprint = fingerprint
  const conversationId = getConversationId()

  sendEvent({
    source: 'CHATGPT',
    eventType: 'TASK_COMPLETED',
    externalTaskId: conversationId,
    title: getConversationTitle(),
    contentPreview: getPreviewText(messageEl),
    openTarget: 'browser',
    openUrl: location.href,
  })

  pendingCompletion = { conversationId, messageEl }
  watchViewed()
}

/* ---------- 已读检测 ---------- */

function isInViewport(el) {
  const rect = el.getBoundingClientRect()
  return rect.bottom > 0 && rect.top < window.innerHeight
}

function maybeViewed() {
  if (!pendingCompletion) return
  if (document.visibilityState !== 'visible' || !document.hasFocus()) return
  if (!isInViewport(pendingCompletion.messageEl)) return

  sendEvent({
    source: 'CHATGPT',
    eventType: 'TASK_VIEWED',
    externalTaskId: pendingCompletion.conversationId,
    openTarget: 'browser',
    openUrl: location.href,
  })
  pendingCompletion = null
}

function watchViewed() {
  // 完成瞬间用户通常正看着页面：延迟一拍检查 + 监听后续可见性/滚动变化
  setTimeout(maybeViewed, 800)
}

document.addEventListener('visibilitychange', maybeViewed)
window.addEventListener('focus', maybeViewed)
window.addEventListener('scroll', () => setTimeout(maybeViewed, 300), { passive: true })

/* ---------- 观察页面变化 ---------- */

function startObserver() {
  const target = document.querySelector('main') ?? document.body
  let debounceTimer = null
  new MutationObserver(() => {
    clearTimeout(debounceTimer)
    debounceTimer = setTimeout(checkCompleted, OBSERVE_DEBOUNCE_MS)
  }).observe(target, { childList: true, subtree: true, characterData: true })
}

startObserver()
checkCompleted() // 页面加载时可能已是完成态

/**
 * AI Task Hub · ChatGPT Connector — 后台 Service Worker
 * 接收 content script 的统一事件并 POST 到本地事件服务。
 * 事件服务不可达时写入本地队列，定时补偿重发（离线事件补偿）。
 */

const API_URL = 'http://127.0.0.1:17891/api/events'
const HEARTBEAT_URL = 'http://127.0.0.1:17891/api/integrations/chatgpt/heartbeat'
const QUEUE_KEY = 'aihub_pending_events'
const RETRY_ALARM = 'aihub-retry'
const RETRY_PERIOD_MIN = 1
const HEARTBEAT_ALARM = 'aihub-heartbeat'
const HEARTBEAT_PERIOD_MIN = 5

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type !== 'AIHUB_EVENT') return false
  postOrQueue(message.event).then(sendResponse)
  return true // 异步响应
})

// 心跳：让 Hub 体检页知道扩展在线（后端不可达时静默失败，不影响事件队列）
function heartbeat() {
  fetch(HEARTBEAT_URL, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ version: chrome.runtime.getManifest().version }),
  }).catch(() => {})
}

function ensureHeartbeatAlarm() {
  chrome.alarms.create(HEARTBEAT_ALARM, { periodInMinutes: HEARTBEAT_PERIOD_MIN })
  heartbeat()
}

chrome.runtime.onInstalled.addListener(ensureHeartbeatAlarm)
chrome.runtime.onStartup.addListener(ensureHeartbeatAlarm)

async function postOrQueue(event) {
  const ok = await postEvent(event)
  if (!ok) {
    await enqueue(event)
    await ensureAlarm()
  }
  return { delivered: ok }
}

async function postEvent(event) {
  try {
    const res = await fetch(API_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(event),
    })
    return res.ok
  } catch {
    return false
  }
}

async function enqueue(event) {
  const queue = await getQueue()
  queue.push(event)
  await chrome.storage.local.set({ [QUEUE_KEY]: queue })
}

async function getQueue() {
  const data = await chrome.storage.local.get(QUEUE_KEY)
  return data[QUEUE_KEY] ?? []
}

async function ensureAlarm() {
  const alarm = await chrome.alarms.get(RETRY_ALARM)
  if (!alarm) {
    chrome.alarms.create(RETRY_ALARM, { periodInMinutes: RETRY_PERIOD_MIN })
  }
}

chrome.alarms.onAlarm.addListener(async (alarm) => {
  if (alarm.name === HEARTBEAT_ALARM) {
    heartbeat()
    return
  }
  if (alarm.name !== RETRY_ALARM) return
  const queue = await getQueue()
  if (queue.length === 0) {
    chrome.alarms.clear(RETRY_ALARM)
    return
  }
  const remaining = []
  for (const event of queue) {
    if (!(await postEvent(event))) remaining.push(event)
  }
  await chrome.storage.local.set({ [QUEUE_KEY]: remaining })
  if (remaining.length === 0) chrome.alarms.clear(RETRY_ALARM)
})

/**
 * AI Task Hub · ChatGPT Connector — 后台 Service Worker
 * 接收 content script 的统一事件并 POST 到本地事件服务。
 * 事件服务不可达时写入本地队列，定时补偿重发（离线事件补偿）。
 */

const API_URL = 'http://127.0.0.1:17891/api/events'
const QUEUE_KEY = 'aihub_pending_events'
const RETRY_ALARM = 'aihub-retry'
const RETRY_PERIOD_MIN = 1

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type !== 'AIHUB_EVENT') return false
  postOrQueue(message.event).then(sendResponse)
  return true // 异步响应
})

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

import { WS_URL } from './config'
import type { ServerMessage } from '../shared/types'

const INITIAL_RECONNECT_DELAY_MS = 1000
const MAX_RECONNECT_DELAY_MS = 30000
const MAX_RECONNECT_ATTEMPTS = 20

type MessageListener = (msg: ServerMessage) => void

/** 与服务端 /ws/tasks 保持长连，断线自动重连，消息转发给窗口与通知模块 */
export class TaskSocket {
  private ws: WebSocket | null = null
  private listeners = new Set<MessageListener>()
  private closedByUser = false
  private reconnectAttempts = 0
  private reconnectTimer: NodeJS.Timeout | null = null

  onMessage(listener: MessageListener): void {
    this.listeners.add(listener)
  }

  connect(): void {
    this.closedByUser = false
    if (this.ws && (this.ws.readyState === WebSocket.CONNECTING || this.ws.readyState === WebSocket.OPEN)) {
      return
    }
    // 清掉任何尚未触发的重连定时器，保证同一时刻至多一个连接在途（LOW）
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    try {
      this.ws = new WebSocket(WS_URL)
    } catch {
      this.scheduleReconnect()
      return
    }

    this.ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(String(event.data)) as ServerMessage
        for (const listener of this.listeners) listener(msg)
      } catch {
        // 忽略无法解析的消息
      }
    }
    this.ws.onopen = () => {
      this.reconnectAttempts = 0
    }
    this.ws.onclose = () => {
      this.ws = null
      if (!this.closedByUser) this.scheduleReconnect()
    }
    this.ws.onerror = () => {
      // 后端切换期间的连接失败由 onclose 统一重连，不重复输出无诊断价值的 ErrorEvent
      this.ws?.close()
    }
  }

  private scheduleReconnect(): void {
    if (this.reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
      console.warn('[ws-client] Max reconnect attempts reached, giving up')
      return
    }
    const delay = Math.min(
      INITIAL_RECONNECT_DELAY_MS * 2 ** this.reconnectAttempts + Math.random() * 1000,
      MAX_RECONNECT_DELAY_MS,
    )
    this.reconnectAttempts++
    // 先清旧句柄再排新定时器：onclose 与 onerror 可能先后各触发一次，避免双连接（LOW）
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer)
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null
      if (!this.closedByUser) this.connect()
    }, delay)
  }

  close(): void {
    this.closedByUser = true
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer)
      this.reconnectTimer = null
    }
    this.ws?.close()
    this.ws = null
  }
}

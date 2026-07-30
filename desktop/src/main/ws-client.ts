import { WS_URL } from './config'
import type { ServerMessage } from '../shared/types'

const RECONNECT_DELAY_MS = 2000

type MessageListener = (msg: ServerMessage) => void

/** 与服务端 /ws/tasks 保持长连，断线自动重连，消息转发给窗口与通知模块 */
export class TaskSocket {
  private ws: WebSocket | null = null
  private listeners = new Set<MessageListener>()
  private closedByUser = false

  onMessage(listener: MessageListener): void {
    this.listeners.add(listener)
  }

  connect(): void {
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
    this.ws.onclose = () => {
      if (!this.closedByUser) this.scheduleReconnect()
    }
    this.ws.onerror = () => {
      this.ws?.close()
    }
  }

  private scheduleReconnect(): void {
    setTimeout(() => {
      if (!this.closedByUser) this.connect()
    }, RECONNECT_DELAY_MS)
  }

  close(): void {
    this.closedByUser = true
    this.ws?.close()
  }
}

# AI Task Hub · ChatGPT Connector

Chrome 扩展（MV3，零构建），监听 ChatGPT 网页回答状态并上报本地 AI Task Hub。

## 安装

1. 打开 `chrome://extensions`
2. 开启右上角 **开发者模式**
3. 点击 **加载已解压的扩展程序**，选择本目录（`adapters/chatgpt-extension`）
4. 打开 https://chatgpt.com 进行任意对话，回答完成即会出现在 AI Task Hub 桌面端

## 工作原理

```text
MutationObserver 监听会话区
    ↓
完成检测（三重判断，防误报）
  最后一条消息是 assistant
  + 停止生成按钮消失
  + 文本 1.5s 内无变化
    ↓
POST TASK_COMPLETED → 127.0.0.1:17891
    ↓
页面可见且聚焦 + 回答进入可视区域
    ↓
POST TASK_VIEWED（任务自动移出未读队列）
```

- **防重复上报**：以"消息数 + 文本长度"作为回答指纹，回答继续生成时指纹变化自动续报
- **离线补偿**：事件服务不可达时事件写入 `chrome.storage.local`，每 1 分钟自动重发
- 事件协议见 `shared/event_schema.json`

## 已知限制

- 依赖 ChatGPT 页面 `data-message-author-role` 等 DOM 属性，官方改版后需同步更新选择器
- 页面刷新后"完成待读"状态不跨会话保留，极端情况下会少发一次 VIEWED（任务可在桌面端手动点掉）

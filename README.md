# AI Task Hub

运行在本地的桌面任务中心，统一管理 **Claude Code / Codex / ChatGPT** 等多个 AI Agent 的任务状态：

```
AI 开始执行
    ↓
AI 完成 / 失败 / 等待输入
    ↓
任务进入桌面端 Queue
    ↓
系统通知 + 桌面卡片实时出现
    ↓
点击卡片打开对应会话（终端 / 浏览器）
    ↓
任务标记已查看并退出 Queue
```

## 核心设计

> **桌面端永远只处理统一任务模型，每个平台的差异全部放在 Adapter 中。**

```
Claude Code Hook ──┐
Codex notify ──────┼── Adapter（统一事件协议）──► FastAPI 事件服务 ──► MySQL
Codex launcher ────┘        POST /api/events         │
ChatGPT Chrome 扩展 ────────┘                        │ WebSocket
                                                    ▼
                                          Electron 桌面端（托盘常驻）
```

- **统一事件协议**：`shared/event_schema.json`，所有平台事件先由 Adapter 转成该格式
- **幂等去重**：`(source, external_task_id)` 唯一约束，同一会话的多次事件合并为同一任务
- **状态机**：`RUNNING → NEEDS_INPUT / COMPLETED_UNREAD / FAILED_UNREAD → VIEWED / IGNORED`
- **生命周期流水**：`task_event` 表记录每次事件原始报文，支持时间线回放与离线补偿

## 技术栈

| 模块 | 技术 |
|------|------|
| 桌面端 | Electron + TypeScript（electron-vite，原生 TS 渲染层） |
| 事件服务 | FastAPI + WebSocket |
| 数据库 | MySQL 8（`ai_task_hub`，测试库 `test_mysql`） |
| Adapter | Python 纯标准库（Hook 环境零依赖） |

## 目录结构

```
ai-task-hub/
├── app/                        # 后端（FastAPI + MySQL）
│   ├── main.py                 # 事件服务入口
│   ├── api/                    # REST + WebSocket 路由
│   ├── service/                # 任务状态机
│   ├── repository/             # 数据访问
│   ├── model/                  # AgentEvent / Task 模型
│   └── database/               # 连接管理与 schema.sql
├── desktop/                    # 桌面端（Electron + TypeScript）
│   └── src/
│       ├── main/               # 主进程：窗口/托盘/通知/WS/后端拉起
│       ├── preload/            # contextBridge 安全桥
│       ├── renderer/           # 渲染层：原生 TS + 手写设计系统
│       └── shared/             # 进程间共享类型
├── adapters/                   # 平台适配器（纯标准库）
│   ├── claude-code/            # Hook → 统一事件
│   └── codex/                  # notify 钩子 + 启动包装器
├── shared/                     # 事件协议 JSON Schema 与常量
├── tests/                      # pytest（使用 test_mysql 测试库）
└── requirements.txt
```

## 快速开始

### 1. 配置 MySQL

```bash
cp .env.example .env   # 填写本机 MySQL 账号密码
```

数据库 `ai_task_hub` 与表结构会在首次启动时**自动创建**。

### 2. 启动事件服务

```bash
uv venv
uv pip install -r requirements.txt
.venv/Scripts/python.exe -m app.main          # http://127.0.0.1:17891
```

### 3. 启动桌面端

```bash
cd desktop
npm install
npm run dev        # 开发模式（HMR）
npm run build && npm run start   # 生产构建运行
```

桌面端启动时会自动探测事件服务，未运行则尝试用仓库 `.venv` 拉起。

### 4. 验证链路

```bash
curl -X POST http://127.0.0.1:17891/api/events \
  -H "Content-Type: application/json" \
  -d '{"source":"CODEX","eventType":"TASK_COMPLETED","externalTaskId":"demo-001","title":"测试任务"}'
```

桌面端应立即出现任务卡片。

## 接入 AI 平台

### Claude Code

把 `adapters/claude-code/settings.example.json` 中的 `hooks` 合并到 `~/.claude/settings.json`：

| Hook | 映射事件 |
|------|---------|
| `UserPromptSubmit` | `TASK_STARTED` |
| `Notification` | `TASK_NEEDS_INPUT` |
| `Stop` | `TASK_COMPLETED` |

### Codex

方式一（推荐）：config.toml 配置 notify 钩子。若 notify 已被占用（如 Codex 桌面端），使用链式适配器——上报后继续转发给原命令（原命令存于 `adapters/codex/forward_target.json`）：

```toml
notify = [ 'D:\develop\AI-Task-Hub\.venv\Scripts\python.exe', 'D:\develop\AI-Task-Hub\adapters\codex\notify_chain.py' ]
```

方式二：用启动包装器代替直接运行 codex

```bash
python adapters/codex/launcher.py "帮我修复登录接口"
```

### ChatGPT

加载 `adapters/chatgpt-extension`（Chrome → 扩展程序 → 开发者模式 → 加载已解压的扩展程序）。回答完成自动上报，打开对话自动已读；事件服务不可达时本地缓存自动补发。

> 所有 Adapter 均**纯标准库零依赖**，且已内置系统代理绕过（localhost 直连）。

### 点击卡片后的唤起

- **ChatGPT** → 系统浏览器打开对话 URL
- **Claude Code** → Windows Terminal 进入项目目录并 `claude --resume <session>`
- **Codex** → Windows Terminal 进入项目目录并 `codex resume <session>`

第一版采用"点击即视为已读"。

## API 一览

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/events` | 接收统一事件（Adapter 入口） |
| GET | `/api/tasks?view=queue\|history` | 任务列表 |
| GET | `/api/tasks/{id}/events` | 任务生命周期时间线 |
| POST | `/api/tasks/{id}/view` | 标记已读 |
| POST | `/api/tasks/{id}/ignore` | 忽略 |
| DELETE | `/api/tasks/{id}` | 删除 |
| WS | `/ws/tasks` | 任务变更实时推送 |

## 测试

```bash
.venv/Scripts/python.exe -m pytest tests -v   # 18 项测试，使用 test_mysql 库
cd desktop && npm run typecheck               # 桌面端类型检查
```

## 数据库说明

- 主键 `BIGINT AUTO_INCREMENT`
- 时间字段 `DATETIME(3)`（毫秒精度，本地时间）
- 枚举码字段 `VARCHAR(32)`（`status` / `event_type` / `source`，保持可读性便于排障）
- `raw_payload JSON`：MySQL 原生 JSON 类型，存统一事件原始报文

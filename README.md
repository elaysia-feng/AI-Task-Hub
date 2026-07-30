# AI Task Hub

> 多 AI 平台的本地任务中心：ChatGPT 网页 / Claude Code / Codex CLI 的任务事件统一汇入桌面队列，实时通知、一键打开对话、一键已读清理。

[![CI](https://github.com/elaysia-feng/AI-Task-Hub/actions/workflows/ci.yml/badge.svg)](https://github.com/elaysia-feng/AI-Task-Hub/actions/workflows/ci.yml)

## 它能做什么

- **统一队列**：三个平台的「任务完成 / 等待输入 / 失败」事件实时进入同一个桌面队列（WebSocket 秒级推送）
- **原生通知**：Windows 原生通知 + 系统托盘未读计数，点击直达对话现场（浏览器标签 / 终端 / 会话链接）
- **接入向导**：设置页一键接入 Claude Code（hooks）与 Codex（notify 链式转发，保留你原有 notify 命令），ChatGPT 扩展在线体检
- **事件时间线**：每个任务可展开完整生命周期事件与原始载荷，排障有据
- **自动更新**：打包版每 4 小时检查 GitHub Releases，下载后一键重启安装

## 架构

```
ChatGPT 网页 ── Chrome 扩展 ─┐
Claude Code ── hooks 适配器 ─┼─ POST /api/events ─→ FastAPI 事件服务 ─→ MySQL（任务状态机）
Codex CLI ──── notify 适配器 ┘      （统一事件协议）        │
                                     │ WebSocket /ws/tasks
                                     ▼
                          Electron 桌面端（队列/历史/详情/设置）
```

- **事件协议**：[`shared/event_schema.json`](shared/event_schema.json)（source / eventType / externalTaskId / title / projectPath / conversationUrl / contentPreview…）
- **幂等**：`(source, externalTaskId)` 唯一约束，重复事件合并到同一任务
- **状态机**：RUNNING → NEEDS_INPUT / COMPLETED_UNREAD / FAILED_UNREAD → VIEWED / IGNORED
- **离线补偿**：Chrome 扩展在后端不可达时本地排队，恢复后自动补发
- **适配器零依赖**：仅用 Python 标准库，且绕过系统代理访问 127.0.0.1

## 快速开始（用户）

1. 从 [Releases](https://github.com/elaysia-feng/AI-Task-Hub/releases) 下载 `AI Task Hub Setup x.y.z.exe` 安装
2. 准备 MySQL 8：创建数据库 `ai_task_hub`，在 `%APPDATA%\AI Task Hub\config.env` 写入：
   ```env
   AIHUB_MYSQL_HOST=127.0.0.1
   AIHUB_MYSQL_PORT=3306
   AIHUB_MYSQL_USER=root
   AIHUB_MYSQL_PASSWORD=你的密码
   AIHUB_MYSQL_DB=ai_task_hub
   ```
3. 启动应用，首次运行自动进入**设置页 → 接入集成**：
   - **Claude Code**：点「一键接入」（写入 `~/.claude/settings.json` 的 PostToolUse hook）
   - **Codex**：点「一键接入」（改写 `~/.codex/config.toml` 的 notify；原命令保留转发）。**已运行的 Codex 进程需重启**才会加载新配置，设置页会检测并提示
   - **ChatGPT**：点「打开扩展目录」，在 `chrome://extensions` 开启开发者模式 →「加载已解压的扩展程序」→ 选择该目录；扩展每 5 分钟心跳，设置页显示在线状态

## 快速开始（开发）

```bash
# 后端（Python 3.12+，uv 或 pip）
uv venv && uv pip install -r requirements.txt
copy .env.example .env   # 填入 MySQL 凭据
python -m app.main       # http://127.0.0.1:17891

# 桌面端（Node 22+）
cd desktop
npm install
npm run dev              # electron-vite 热更新，自动拉起后端
```

### 测试与冒烟

```bash
pytest                          # 后端 38 项（test_mysql 库，缺 MySQL 自动 skip）
cd desktop && npm test          # 渲染层 vitest 12 项
python scripts/e2e_smoke.py     # 端到端 8 步冒烟（隔离端口 17899 + test_mysql）
```

### 构建与发布

```bash
# 后端单文件 exe（PyInstaller）
pyinstaller packaging/backend.spec --distpath packaging/dist --workpath packaging/build

# 桌面安装包（NSIS，内嵌后端 exe）
cd desktop && npm run dist

# 发布：打 tag 触发 GitHub Actions 构建并发布 Release（latest.yml 供自动更新）
git tag v0.1.1 && git push origin v0.1.1
```

## 目录结构

```
app/            # FastAPI 事件服务（api/service/repository/database 分层）
adapters/       # 平台适配器：claude-code hooks / codex notify / chatgpt 扩展
desktop/        # Electron + TypeScript 桌面端（main/preload/renderer）
packaging/      # PyInstaller spec
scripts/        # e2e 冒烟、DB 诊断、Codex 模拟器
shared/         # 事件协议 schema 与共享常量
tests/          # pytest（状态机 / API / 集成接入 / 编码防回归）
```

## 常见问题

- **Codex 事件不上报**：Codex 只在启动时读配置。接入后需重启所有 Codex 进程（设置页会检测旧进程并警告）
- **ChatGPT 无通知**：确认扩展已安装且浏览器在运行；设置页看「扩展在线 vX」
- **系统代理导致事件丢失**：所有适配器已内置 127.0.0.1 代理绕过；若仍异常看 `%APPDATA%\AI Task Hub\logs\backend.log`
- **更新检查失败**：更新走 GitHub Releases，需要能访问 github.com（设置页可手动重试）

## License

MIT

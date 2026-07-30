# AI Task Hub

> 多 AI 平台的本地任务中心：ChatGPT 网页 / Claude Code / Codex CLI 的任务事件统一汇入桌面队列，实时通知、一键打开对话、一键已读清理。

[![CI](https://github.com/elaysia-feng/AI-Task-Hub/actions/workflows/ci.yml/badge.svg)](https://github.com/elaysia-feng/AI-Task-Hub/actions/workflows/ci.yml)

## 它能做什么

- **统一队列**：三个平台的「执行中 / 等待输入 / 完成 / 失败」事件实时进入同一个桌面队列（WebSocket 秒级推送）
- **原生通知**：Windows 原生通知 + 系统托盘未读计数，点击直达对话现场（浏览器标签 / 终端 / 会话链接）
- **悬浮球**：关闭主窗口后收起为右下角小球，悬停看任务状态，拖动可移动；单击打开面板
- **本地壁纸**：设置页可选本机图片作背景，调节模糊 / 暗角 / 面板透明度（不上传云端）
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

- **事件协议**：[`shared/event_schema.json`](shared/event_schema.json)（source / eventType / externalTaskId / title / projectPath / openUrl / contentPreview…）
- **幂等**：`(source, externalTaskId)` 唯一约束，重复事件合并到同一任务
- **状态机**：RUNNING → NEEDS_INPUT / COMPLETED_UNREAD / FAILED_UNREAD → VIEWED / IGNORED
- **离线补偿**：Chrome 扩展在后端不可达时本地排队，恢复后自动补发
- **适配器零依赖**：仅用 Python 标准库，且绕过系统代理访问 127.0.0.1

---

## 怎么启动（必读）

### 方式 A：开发模式（改代码时用这个）

环境要求：Windows、Node 22+、Python 3.12+、MySQL 8。

**1. 准备数据库与后端依赖（首次）**

```powershell
cd d:\develop\AI-Task-Hub
uv venv
uv pip install -r requirements.txt
copy .env.example .env
# 编辑 .env，填入 MySQL 账号密码，库名建议 ai_task_hub
```

**2. 启动桌面端（推荐：一条命令）**

```powershell
cd d:\develop\AI-Task-Hub\desktop
npm install          # 首次需要
npm run dev          # 打开 Electron 窗口；会自动探测并拉起本地后端
```

正常时会弹出 **AI Task Hub** 窗口。关闭窗口会收成右下角**悬浮球**（不是退出）；彻底退出请：托盘图标右键 → **退出**。

**3. 如果只想单独跑后端（可选）**

```powershell
cd d:\develop\AI-Task-Hub
.\.venv\Scripts\python.exe -m app.main
# 健康检查：浏览器打开 http://127.0.0.1:17891/api/health
```

开发模式下桌面端一般会自己拉起 `.venv` 里的后端，多数情况不用手动开。

---

### 方式 B：安装包 / 本地打包的 exe

**从 GitHub 安装**

1. 打开 [Releases](https://github.com/elaysia-feng/AI-Task-Hub/releases)
2. 下载 `AI Task Hub Setup x.y.z.exe`，双击安装
3. 准备 MySQL，并在 `%APPDATA%\AI Task Hub\config.env` 写入连接信息（见下方「数据库配置」）
4. 开始菜单或桌面快捷方式启动 **AI Task Hub**

**本机自己打包后再启动**

```powershell
# ① 后端 exe（Hub 图标，不要用默认 Python 图标）
cd d:\develop\AI-Task-Hub
pwsh desktop\scripts\make-icon.ps1
.\.venv\Scripts\python.exe -m PyInstaller packaging\backend.spec --distpath packaging\dist --workpath packaging\build --noconfirm

# ② 桌面安装包
cd desktop
npm run dist:local
```

产物在 `desktop\dist\`：

| 文件 | 怎么用 |
|------|--------|
| `AI Task Hub Setup x.y.z.exe` | 双击安装 |
| `AI-Task-Hub-Portable-x.y.z.exe` | 免安装，双击即跑 |
| `win-unpacked\AI Task Hub.exe` | 解压目录里直接运行 |

也可在开发版窗口里打开 **设置 → 应用更新 / 打包 → 生成 exe 安装包**（会先弹确认框）。

**重要：双击 exe「没反应」时**

应用是**单实例**的。如果已经在跑（托盘里有图标，或另一个 `npm run dev` 窗口还在）：

1. 托盘右键 → **退出**，或结束所有 `AI Task Hub` / `electron` 进程  
2. 再双击 exe  

否则新进程会立刻退出，并把焦点交给旧实例（旧实例若是角落悬浮球，容易误以为没打开）。

若 Windows 提示「已保护你的电脑」：点 **更多信息 → 仍要运行**。

---

### 数据库配置

用户安装版写到：`%APPDATA%\AI Task Hub\config.env`

```env
AIHUB_MYSQL_HOST=127.0.0.1
AIHUB_MYSQL_PORT=3306
AIHUB_MYSQL_USER=root
AIHUB_MYSQL_PASSWORD=你的密码
AIHUB_MYSQL_DB=ai_task_hub
```

开发版写到仓库根目录 `.env`（从 `.env.example` 复制）。

---

### 首次进入后的设置

启动后第一次会进**设置页**：

- **外观**：本机壁纸、模糊 / 暗角 / 面板透明度  
- **Claude Code**：一键接入（写入 `~/.claude/settings.json` 钩子）  
- **Codex**：一键接入（改 `~/.codex/config.toml` 的 notify）。**已运行的 Codex 需重启**才生效  
- **ChatGPT**：打开扩展目录 → Chrome 开发者模式加载该目录  

---

## 测试与冒烟

```powershell
cd d:\develop\AI-Task-Hub
pytest
cd desktop
npm test
cd ..
.\.venv\Scripts\python.exe scripts\e2e_smoke.py
```

## 发布

```powershell
# 打 tag 触发 GitHub Actions 构建并发布 Release（latest.yml 供自动更新）
git tag v0.1.8
git push origin v0.1.8
```

本地发版前请先按「方式 B」打出后端 exe + 桌面安装包验证。

## 目录结构

```
app/            # FastAPI 事件服务（api/service/repository/database 分层）
adapters/       # 平台适配器：claude-code hooks / codex notify / chatgpt 扩展
desktop/        # Electron + TypeScript 桌面端（main/preload/renderer）
packaging/      # PyInstaller spec + app.ico
scripts/        # e2e 冒烟、DB 诊断、Codex 模拟器
shared/         # 事件协议 schema 与共享常量
tests/          # pytest（状态机 / API / 集成接入 / 编码防回归）
```

## 常见问题

- **开发怎么启动？** → `cd desktop` 后 `npm run dev`（见上文「方式 A」）
- **打包 exe 双击没反应？** → 先退出托盘里已有实例 / 关掉 `npm run dev`，再双击
- **Codex 事件不上报**：Codex 只在启动时读配置；接入后需重启所有 Codex 进程
- **ChatGPT 无通知**：确认扩展已安装且浏览器在运行；设置页看「扩展在线」
- **系统代理导致事件丢失**：适配器已绕过 127.0.0.1 代理；仍异常看 `%APPDATA%\AI Task Hub\logs\backend.log`
- **更新检查失败**：需能访问 github.com；设置页可手动「检查更新」
- **图标仍是 Python**：重新执行 `pwsh desktop\scripts\make-icon.ps1` 并按「方式 B」重打后端 exe 与安装包

## License

MIT

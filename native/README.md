# AI Task Hub Native

这是 AI Task Hub 的 Windows 原生版本：一个 C++/Qt 6.8 进程同时承载 QML 界面、HTTP 事件服务、SQLite WAL 和系统托盘。运行时不再启动 Electron、Chromium 或 Python 后端，继续兼容 [`shared/event_schema.json`](../shared/event_schema.json) 定义的 `POST /api/events` 契约。

## 已迁移能力

- 待处理、历史、搜索、来源/状态筛选和详情时间线
- 任务幂等更新、状态机、批量已读、按范围清理和 SQLite 外键级联
- ChatGPT / Claude Code / Codex / OTHER 事件来源与 `replyText` 保留
- 打开浏览器对话、Windows Terminal 项目目录和数据目录
- 本地 CORS、分页快照、任务详情、事件流水、AI 答复和集成状态 API
- Windows 系统托盘、原生通知和悬浮球模式
- 深浅色界面切换、低常驻模型列表和 WAL 数据库

## 构建

Qt 6.8.3 的 MinGW 版本与编译器需要匹配（本机默认路径为 `E:/QT/6.8.3/mingw_64`）：

```powershell
cmake -S native -B native/build -G Ninja -DCMAKE_PREFIX_PATH=E:/QT/6.8.3/mingw_64
cmake --build native/build --parallel 2
```

启动开发版：

```powershell
$env:PATH = "E:/QT/6.8.3/mingw_64/bin;E:/QT/Tools/mingw1310_64/bin;$env:PATH"
./native/build/ai-task-hub-native.exe
```

生成可分发目录（自动调用 `windeployqt`）：

```powershell
./native/package-native.ps1
```

默认服务地址是 `http://127.0.0.1:17891`，数据目录与旧 Python SQLite 服务保持一致：`%APPDATA%\AI Task Hub\data.sqlite`。如端口被旧版服务占用，先从旧托盘退出，再启动原生版。

## 事件示例

```json
{
  "source": "CODEX",
  "eventType": "TASK_COMPLETED",
  "externalTaskId": "session-123",
  "title": "构建完成",
  "contentPreview": "native build passed",
  "projectPath": "F:\\myInterestingProgram\\AI-Task-Hub",
  "openTarget": "terminal"
}
```

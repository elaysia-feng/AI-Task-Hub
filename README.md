# AI Task Hub

把 ChatGPT 网页、Claude Code 和 Codex 的任务事件集中到本地收件箱，查看任务状态、事件时间线和未读消息。

## 选择版本

仓库保留三套桌面实现，构建方式和运行依赖不同：

| 目录 | 实现 | 使用入口 |
| --- | --- | --- |
| `native-win32/` | C++、Win32、Direct2D、SQLite | [原生版构建与使用](native-win32/README.md) |
| `desktop/` + `app/` | Electron 桌面端、Python FastAPI 后端 | 桌面脚本和后端依赖分别维护 |
| `native/` | Qt 原生实现 | [Qt 版说明](native/README.md) |

希望运行单进程 Windows 原生版时，从 `native-win32/` 开始；不要把 Electron 的 Node.js 和后端环境要求套用到该版本。

## Win32 版快速开始

在 Windows 上准备原生版构建脚本要求的编译工具，然后从仓库根目录运行：

```powershell
.\native-win32\build-win32.ps1
& '.\native-win32\dist\AI Task Hub Win32.exe'
```

主程序内置本地 HTTP 服务与 SQLite。Claude Code、Codex 的外部适配器仍需要系统 Python；ChatGPT 网页通过浏览器扩展上报事件。

## 使用流程

1. 在设置中选择平台接入，按提示配置适配器或加载浏览器扩展。
2. 在对应工具中执行任务，返回收件箱查看状态和事件时间线。
3. 按来源、状态或关键词筛选任务，查看详情、标记已读或清理消息。
4. 需要后台提醒时使用悬浮球、托盘、系统通知与应用内提醒卡片。

“已配置”只说明接入配置已写入，真实事件到达后才能确认链路可用。浏览器扩展需要手动加载。

## 数据与接入

Win32 版默认把 `data.sqlite` 放在可执行文件目录，可在设置中迁移数据目录。迁移通过 SQLite 备份复制数据，重启生效并保留旧数据库。

本地事件服务使用 `127.0.0.1:17891`。自动接入修改配置前会备份文件；复杂或损坏的配置会提示手工处理。详细行为见[原生版说明](native-win32/README.md)。

## 开发与验证

- `adapters/`：平台事件适配器。
- `shared/`：共享事件协议。
- `tests/`：Python 等现有回归测试。
- `native-win32/`：原生 UI、本地服务、测试与打包脚本。

原生版可先构建候选文件，再执行隔离测试：

```powershell
.\native-win32\build-win32.ps1 -StageOnly
.\native-win32\test-win32.ps1
```

GUI 验证需要交互式 Windows 桌面，按原生版文档使用独立测试目录。

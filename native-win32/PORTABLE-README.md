# AI Task Hub · Win32 便携版 0.3.5

适用于 Windows 10 / 11 x64。使用 C++、Win32、Direct2D 和 SQLite，无需安装 Qt、Electron、Node.js 或 MinGW。

## 使用

1. 将整个 ZIP 解压到可写目录，不要在压缩包里直接运行，也不要只复制 exe。
2. 退出旧版任务中心，避免占用本地端口 `127.0.0.1:17891`。
3. 双击 `AI Task Hub Win32.exe`，默认在屏幕右上角显示悬浮球。点击打开主窗口，悬停预览消息，拖动调整位置。
4. 最小化或点击标题栏 × 收回悬浮球；托盘右键菜单中的“结束进程”才会退出程序。
5. 任务完成、失败或等待输入时会显示应用内提醒卡片；点击卡片可打开任务面板。

## 数据与更新

数据保存在 exe 同目录的 `data.sqlite`，偏好保存在 `AI Task Hub.ini`。首次运行可能从旧 `%APPDATA%\AI Task Hub` 迁移数据。发布包不附带任何用户数据库或个人配置。

更新前退出程序并备份数据文件；将新版程序、`resources` 和 `adapters` 覆盖到原目录，不删除原数据库和个人配置。卸载时关闭程序后删除解压目录即可；接入钩子、浏览器扩展和旧 AppData 数据需按需单独处理。

## 平台接入

- Claude Code：设置 → 平台接入 → 一键接入，默认配置为 `%USERPROFILE%\.claude\settings.json`。
- Codex：默认配置为 `%USERPROFILE%\.codex\config.toml`，保留既有通知转发。
- 两种 CLI 的适配器需要可用的 Python 3；程序主体不依赖 Python。如未能自动检测，可设置 `AIHUB_PYTHON` 为解释器完整路径。
- 修改配置前自动保留 `.aihub-*.bak` 备份。自定义配置根目录、复杂 TOML 可能需要手动配置。
- ChatGPT 网页：点击“打开目录”，在 Chrome / Edge 的扩展管理中开启开发者模式，选择“加载已解压的扩展程序”，加载打开的目录，再刷新 ChatGPT 页面。
- “已配置”不等于已经接收真实事件；浏览器扩展收到心跳后才显示在线。CLI 修改后建议重新启动对应工具。

## 注意

这是未签名的便携程序，Windows 可能提示未知发布者。请核对 GitHub 仓库来源与 SHA256，不要关闭系统安全防护。壁纸和窗口大小会影响内存占用，不承诺固定几 MB。

源码与问题反馈：https://github.com/elaysia-feng/AI-Task-Hub

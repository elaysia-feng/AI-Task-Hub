## AI Task Hub · Win32 便携版 0.3.0

本次发布的是 **Windows x64 原生便携版**，不是 Electron 安装包。

### 下载与启动

下载 `AI-Task-Hub-Win32-x64-0.3.0.zip`，完整解压后双击 `AI Task Hub Win32.exe`。默认在右上角显示悬浮球，点击打开主窗口。请先退出占用 `127.0.0.1:17891` 的旧版任务中心。

### 功能

- C++ / Win32 / Direct2D / SQLite 单进程桌面程序，静态链接 MinGW 运行库，无需 Qt、Electron 或 Node.js。
- 保留内置主题壁纸、头像、深浅色和透明卡片；设置分为外观、平台接入、数据与通知。
- GPT 网页、Claude Code、Codex 等来源分类，支持状态筛选、搜索、可滚动详情、已读、忽略和确认删除。
- 悬浮球支持拖动、悬停消息预览及点击打开；系统消息通知可开关。
- Claude Code / Codex 一键接入，修改前备份配置，保留已有通知转发；ChatGPT 扩展目录可直接从设置打开。

### 数据与注意事项

- 数据库及偏好保存在 exe 同目录。首次运行可能迁移旧 AppData 数据；升级前退出应用并备份数据文件。
- ZIP 不包含用户数据库、个人配置、会话标题缓存、日志或密钥。
- 程序主体不依赖 Python；Claude Code / Codex 的外部钩子适配器需要 Python 3。ChatGPT 扩展需要在 Chrome / Edge 手动加载。
- 未签名程序可能触发 Windows 未知发布者提示，请核对仓库来源与附带 SHA256。
- 内存随壁纸、字体缓存与窗口大小变化，不承诺固定几 MB 占用。

源码位于本标签的 `native-win32/`；构建和回归验证方法见该目录 README。

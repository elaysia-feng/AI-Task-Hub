# AI Task Hub · Win32/Direct2D

这是完整脱离 Qt/QML 的 Windows 原生版本：Win32 窗口与输入、Direct2D 绘制、DirectWrite 中文文本、WIC 图片解码、Winsock 本地 HTTP 和内置 SQLite amalgamation 均在同一个轻量进程中。

## 构建

在项目根目录执行：

```powershell
.\native-win32\build-win32.ps1
```

发布目录为 `native-win32/dist`，脚本会复制旧版的所有主题壁纸、头像预设、图标、事件协议和三类适配器脚本。启动：

```powershell
& '.\native-win32\dist\AI Task Hub Win32.exe'
```

程序主体不依赖 Qt DLL、QML、Chromium 或 Python；只有点击 Claude Code / Codex 的“一键接入”后，外部钩子才会调用系统 Python 运行适配器脚本。

## 视觉与透明效果

- **真·逐像素透明窗口**：使用 `WS_EX_LAYERED + UpdateLayeredWindow(ULW_ALPHA)` 渲染管线，标题栏 / 侧栏 / 卡片都是 RGBA 渐变半透明叠层，桌面壁纸直接透出，对齐桌面/QML 版的 `color: "transparent"`。
- **设计令牌**：完全采用桌面 CSS 变量与 QML `readonly property` 的色值与圆角（`--radius=18`、`--radius-sm=14`、`--radius-xs=10`、`--radius-pill=999`），含 `#111416/#f4f2ed` 画布、`#df7654` 主题色、`#4f8ff7/#8b5cf6` 来源色等。
- **深浅色切换**：标题栏右侧 ☼/☾ 按钮即时切换两套 Palette，卡片描边、文字、状态点全部同步。
- **窗口尺寸**：标题栏 46px、侧栏 180px、任务卡 146px、详情面板 360px，与桌面/QML 版一致。

## 保留的行为

- 默认使用 exe 所在目录的 `data.sqlite`，下载到哪里数据就跟随到哪里；首次启动会从旧 `%APPDATA%\AI Task Hub` 迁移已有任务、事件流水、主题和头像配置。
- `127.0.0.1:17891` 提供 `/api/events`、任务快照、详情、事件流水、已读/忽略/清理等接口，适配器无需改协议。
- 主窗口保留壁纸、头像、GPT 网页 / Claude Code / Codex / 其他来源分类、状态颜色、任务详情与生命周期事件时间线。
- 设置页恢复三类真实接入：Claude Code 写入 `%USERPROFILE%\\.claude\\settings.json`，Codex 写入 `%USERPROFILE%\\.codex\\config.toml`，适配器脚本复制到 `%APPDATA%\\AI Task Hub\\adapters`；重复点击会更新到当前发布版路径，不会重复追加钩子。
- ChatGPT 网页接入会准备 `%APPDATA%\\AI Task Hub\\chatgpt-extension` 并打开目录；浏览器安全策略不允许桌面程序静默安装扩展，需要在 `chrome://extensions` 开启开发者模式后“加载已解压的扩展程序”。
- 最小化按钮和设置页“收起为悬浮球”都会进入圆形悬浮球；拖动超过 5px 才算拖拽，点击打开主窗口时自动放到悬浮球反侧。
- 悬停悬浮球显示任务概览；离开面板后延迟收起，不会因为拖动误触打开主窗口。
- 悬浮球保留头像、静态高光与未读计数；空闲时不运行动画或定时刷新任务列表。

## 本轮界面与交互完善

- 设置分为「外观主题」「平台接入」「数据与通知」，切换后回到页首；主题选择在窄窗口自动调整列数，浅色面板保留壁纸透出。
- 来源与状态标签准确选择对应条件；搜索支持 `Ctrl+K` / `Ctrl+F`、粘贴和 `Esc` 清除，无匹配时提供清除筛选入口。
- 卡片按钮绑定被点击的任务；历史和详情删除前二次确认；打开目标失败不标记已读。
- 详情中的摘要、完整答复及事件列表独立滚动，底部操作固定；窄窗口改用单栏详情。
- 悬停小球后可点击消息查看详情、打开面板或一键已读；拖动与点击分开判定，丢失鼠标捕获会取消操作。
- 系统通知可开关，深浅色和通知偏好保存到便携目录。

## 接入安全与限制

Claude Code 的标准配置是 `.claude/settings.json`，不是 `.claudecode`。Codex 使用 `.codex/config.toml`。当前一键接入针对这两个用户目录默认位置，不自动改动自定义配置根目录。

配置变更前在原目录留下 `.aihub-*.bak` 备份；保留其他 hooks 和原有 Codex 通知转发。重复安装不会重复添加钩子。复杂或损坏的 TOML（例如根表多行字符串）会停止自动修改并提示手工处理，不猜测配置边界。安装操作只由原生设置按钮触发，不开放 HTTP 安装接口。

「已配置」表示本地钩子已配置，不代表已经收到该工具的真实事件。ChatGPT 只有收到扩展心跳才显示在线；准备目录不能代替浏览器加载步骤。发布包不包含个人 `session_titles.json`，旧包内的缓存移到 `obj` 备份，运行期缓存不覆盖。

## 回归验证

构建后运行独立的原生测试，使用新建隔离目录，不修改真实配置或数据库：

```powershell
.\native-win32\build-win32.ps1 -StageOnly
.\native-win32\test-win32.ps1
```

`-StageOnly` 生成 `dist/AI Task Hub Win32.next.exe`，便于先验证后替换正在使用的版本。测试涵盖 JSON 中文/emoji/数字精度、接入备份与幂等、TOML 根表与注释、扩展心跳、任务筛选/已读/按来源删除、事件级联及偏好持久化。测试副本保存在 `obj/test-fixtures-*`。

`tests/verify-ui.ps1` 用于隔离版 GUI 截图与交互验证，需要先关闭现有窗口，并提供带独立数据库的测试 exe；不能直接用于日常数据库。该脚本会检查服务返回的数据库位置后才生成测试消息。

## 便携版打包

先生成发布候选，再从源码白名单打包（不会直接复制含个人数据的 `dist`）：

```powershell
.\native-win32\build-win32.ps1 -StageOnly
.\native-win32\test-win32.ps1
.\native-win32\package-portable.ps1 -Version 0.3.0
```

ZIP 与 SHA256 校验文件位于 `native-win32/release/`。发布使用独立标签 `win32-v0.3.0`，不会触发现有 Electron 的 `v*` 构建流程。MinGW 运行库已静态链接，无需分发 `libwinpthread-1.dll`。

## 资源与内存

图片按当前窗口覆盖尺寸解码；收起为小球时释放主面板壁纸、预览图和渲染目标。任务变化通过事件消息刷新，不再每 2.5 秒查询整份数据库。Windows 的字体、渲染与堆缓存可能继续保留内存，因此资源释放不等于工作集立即下降；请分别观察私有内存和工作集，不保证固定的几 MB 占用。
